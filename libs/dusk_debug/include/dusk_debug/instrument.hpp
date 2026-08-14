//
// Created by Brenden Davidson on 8/2/26.
//

#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tracy/Tracy.hpp>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <thread>

#include "dusk_debug/channel.hpp"

namespace dusk {

struct Report {
    const char* value_name;
    int64_t value;
};

/**
 * Value instrumentation
 */
class Instrumented {
protected:
    /// Name used in instrumentation tools
    std::string const& m_name;
    size_t m_value_size;
    std::unique_ptr<std::byte[]> m_value_ptr;
    std::unique_ptr<std::byte[]> m_prev_value_ptr;
    bool m_signed;

    [[nodiscard]] int64_t get_plot_value() const noexcept;

public:
    template <std::integral T>
    Instrumented(const T initial_val, std::string const& name) noexcept
        : m_name(name), m_signed(std::is_signed_v<T>) {
        constexpr size_t value_size = sizeof(T);
        this->m_value_ptr = std::make_unique<std::byte[]>(value_size);
        this->m_prev_value_ptr = std::make_unique<std::byte[]>(value_size);
        this->m_value_size = value_size;

        memcpy(this->m_value_ptr.get(), &initial_val, value_size);
        memcpy(this->m_prev_value_ptr.get(), &initial_val, value_size);
    }

    ~Instrumented() noexcept;

    /// Report this value's current state to Tracy
    void send_report() noexcept;

    template <std::integral T>
    struct Handle {
        std::shared_ptr<Instrumented> m_instrumented;

        explicit Handle(std::shared_ptr<Instrumented> instrumented) noexcept : m_instrumented(std::move(instrumented)) {};

        ~Handle() noexcept { this->m_instrumented->send_report(); }

        T get_value() const noexcept {
            T value;
            memcpy(&value, this->m_instrumented->m_value_ptr.get(), sizeof(T));
            return value;
        }

        void set_value(const T new_val) noexcept {
            memcpy(this->m_instrumented->m_value_ptr.get(), &new_val, sizeof(T));
        }
    };
};

/**
 * Proxy used in-place of an instrumented value.
 *
 * @details
 *
 * @remark Specialized proxies must be the same runtime size as the value they are proxying;
 * therefore, a fully generic version of this class is not possible.
 *
 * @tparam T type being proxied
 */
template <std::integral T>
class InstrumentProxy {
    T m_index;

    Instrumented::Handle<T> get_handle() noexcept;

public:
    /**
     * Create a new InstrumentProxy instance
     *
     * @param initial_val initial underlying value
     * @param debug_name name used in Tracy for profiling
     */
    explicit InstrumentProxy(std::string_view const& debug_name, T initial_val = 0) noexcept;

    InstrumentProxy(InstrumentProxy&) = delete;

    ~InstrumentProxy() noexcept;

    [[nodiscard]] uint32_t get_index() const noexcept {
        return static_cast<uint32_t>(this->m_index);
    }

    explicit operator T() noexcept { return this->get_handle().get_value(); }

    T operator++(int) noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val += 1);
        return old_val;
    }

    T operator++() noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val += 1);
        return handle.get_value();
    }

    T operator--(int) noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val -= 1);
        return old_val;
    }

    T operator--() noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val -= 1);
        return handle.get_value();
    }

    InstrumentProxy& operator+=(const T other) noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val += other);
        return *this;
    }

    InstrumentProxy& operator-=(const T other) noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val -= other);
        return *this;
    }

    InstrumentProxy& operator=(const T new_val) noexcept {
        this->get_handle().set_value(new_val);
        return *this;
    }

    template <std::integral OtherNum>
    bool operator==(const OtherNum other) noexcept {
        return this->get_handle().get_value() == other;
    }

    template <std::integral OtherNum>
    bool operator<(const OtherNum other) noexcept {
        return this->get_handle().get_value() < other;
    }

    template <std::integral OtherNum>
    bool operator>(const OtherNum other) noexcept {
        return this->get_handle().get_value() > other;
    }

    template <std::integral OtherNum>
    bool operator<=(const OtherNum other) noexcept {
        return this->get_handle().get_value() <= other;
    }

    template <std::integral OtherNum>
    bool operator>=(const OtherNum other) noexcept {
        return this->get_handle().get_value() >= other;
    }
};

/**
 * Singleton class in charge of managing instrumented values and objects
 *
 * @details
 * This class uses a side table to look up a value's instrumentation details at runtime.
 * Instrumenting a value creates a proxy of the same size containing details on how to perform this
 * lookup.
 */
class InstrumentManager {
    static constexpr uint8_t U8_RESERVED = std::numeric_limits<uint8_t>::max();
    static constexpr uint16_t U16_RESERVED = std::numeric_limits<uint16_t>::max() - U8_RESERVED;
    static constexpr uint32_t U32_RESERVED = std::numeric_limits<uint32_t>::max() - U16_RESERVED;

    /**
     * Mutex used for standard bookkeeping of instrumented values.
     *
     * @remarks Despite faster alternatives being available, InstrumentManager uses this mutex to
     * prevent data races. In practice, this single contention point shouldn't cause bottlenecks. It
     * should only be locked on creation or destruction of instrumented values.
     */
    std::mutex m_mutex{};

    /**
     * Slots previously allocated which have become free.
     *
     * @details The proxy registration process checks for openings here before attempting to
     * allocate a new index.
     *
     * @remarks The underlying array has 3 slots aligning with 1-byte, 2-byte, and 4-byte indices
     * respectively. When an InstrumentedProxy's destructor is called, it asks InstrumentManager to
     * mark its index as free in this field.
     */
    std::array<std::vector<uint32_t>, 3> m_free_slots{};

    /// Gets a reference to the singleton registered name set
    std::unordered_set<std::string> m_name_set{};

    /**
     * Index-to-instrumentation map.
     *
     * @details An `InstrumentedProxy` holds its own copy of the index associated with the
     * instrumented value. That index is used with this map to quickly find the associated
     * `Instrumented` instance.
     */
    std::unordered_map<uint32_t, std::shared_ptr<Instrumented>> m_instrumented_vals{};

    /**
     * Capacity remaining for u32-sized proxies
     */
    uint32_t u32_capacity = U32_RESERVED;

    /**
     * Try to allocate an index for a new u32-sized proxy
     * @return
     */
    std::optional<uint32_t> allocate_u32_proxy() noexcept;

    /**
     * Capacity remaining for u16-sized proxies
     *
     * @remark The max capacity of `u8_capacity` is reserved
     */
    uint16_t u16_capacity = U16_RESERVED;

    /**
     * Try to allocate an index for a new u16-sized proxy
     * @return
     */
    std::optional<uint16_t> allocate_u16_proxy() noexcept;

    /**
     * Capacity remaining for u8-sized proxies
     */
    uint8_t u8_capacity = U8_RESERVED;

    /**
     * Try to allocate an index for a new u8-sized proxy
     * @return
     */
    std::optional<uint8_t> allocate_u8_proxy() noexcept;

    /// Interns the given name so that Tracy gets always gets the same address during a run.
    std::string const& intern_name(std::string_view const& name_view) noexcept;

    /// Bounded MPMC queue used to send value updates to the plot thread
    ///
    /// Defaults to 10 message capacity
    std::shared_ptr<Channel<10, int64_t>> m_message_queue = std::make_shared<Channel<10, int64_t>>("ValueQueue");

    /// Background thread used to send updates to Tracy
    std::jthread m_plot_thread;
public:
    InstrumentManager() noexcept;

    InstrumentManager(InstrumentManager&) = delete;
    InstrumentManager(InstrumentManager&&) = delete;
    InstrumentManager& operator=(InstrumentManager&) = delete;
    InstrumentManager& operator=(InstrumentManager&&) = delete;

    /// Gets the singleton instance of InstrumentManager
    static InstrumentManager& instance() noexcept;

    void submit_report(Report& report) const noexcept;

    /**
     * Gets a RAII handle for the proxied value.
     *
     * @remark The handle makes the instrumentation system check the current state of the underlying
     * value as soon as it goes out of scope. Changes are reported to Tracy if detected.
     *
     * @tparam T type of the underlying data
     * @param proxy reference to the proxy instance in charge of the data
     * @return handle to the underlying data
     */
    template <std::integral T>
    Instrumented::Handle<T> get_handle_for_proxy(const InstrumentProxy<T>& proxy) noexcept {
        const auto index = proxy.get_index();
        std::unique_lock lock(this->m_mutex);
        auto& instrument = this->m_instrumented_vals.at(index);
        return Instrumented::Handle<T>(instrument);
    }

    /**
     * Registers a new proxy, returning its lookup index.
     *
     * @tparam T type of value being proxied
     * @param debug_name static name used in debug tooling
     * @param initial_value initial stored value
     * @return newly registered lookup index
     */
    template <std::integral T>
    T register_proxy(std::string_view const& debug_name, T initial_value = 0) noexcept {
        constexpr size_t size_of_t = sizeof(T);
        static_assert(size_of_t <= sizeof(uint32_t), "Invalid type size");

        const auto& name = intern_name(debug_name);
        // Get the appropriate index for the value's type.
        // Assertions are used to immediately panic if we run out of indices.
        std::optional<T> out_idx;
        if constexpr (size_of_t == sizeof(uint8_t)) {
            out_idx = this->allocate_u8_proxy();
        } else if constexpr (size_of_t == sizeof(uint16_t)) {
            out_idx = this->allocate_u16_proxy();
        } else if constexpr (size_of_t == sizeof(uint32_t)) {
            out_idx = this->allocate_u32_proxy();
        } else {
            out_idx = {std::nullopt};
        }
        assert(out_idx.has_value());

        // Create instrumented value
        {
            auto instrumented = std::make_shared<Instrumented>(initial_value, name);
            std::unique_lock lock(this->m_mutex);
            this->m_instrumented_vals.emplace(static_cast<uint32_t>(out_idx.value()), instrumented);
        }

        return out_idx.value();
    }

    template <std::integral T>
    void unregister_proxy(const InstrumentProxy<T>& proxy) noexcept {
        constexpr size_t size_of_t = sizeof(T);
        static_assert(size_of_t <= sizeof(uint32_t), "Invalid type size");

        const auto proxy_idx = proxy.get_index();
        {
            std::unique_lock lock(this->m_mutex);
            this->m_instrumented_vals.erase(proxy_idx);
        }

        size_t free_slot_vec_idx = 0;
        if constexpr (size_of_t == sizeof(uint8_t)) {
            free_slot_vec_idx = 0;
        } else if constexpr (size_of_t == sizeof(uint16_t)) {
            free_slot_vec_idx = 1;
        } else if constexpr (size_of_t == sizeof(uint32_t)) {
            free_slot_vec_idx = 2;
        }

        {
            std::unique_lock lock(this->m_mutex);
            this->m_free_slots[free_slot_vec_idx].emplace_back(proxy_idx);
        }
    }
};

template <std::integral T>
InstrumentProxy<T>::InstrumentProxy(std::string_view const& debug_name, T initial_val) noexcept {
    this->m_index = InstrumentManager::instance().register_proxy<T>(debug_name, initial_val);
}

template <std::integral T>
InstrumentProxy<T>::~InstrumentProxy() noexcept {
    InstrumentManager::instance().unregister_proxy(*this);
}

template <std::integral T>
Instrumented::Handle<T> InstrumentProxy<T>::get_handle() noexcept {
    return InstrumentManager::instance().get_handle_for_proxy(*this);
}

extern template class InstrumentProxy<int8_t>;
extern template class InstrumentProxy<uint8_t>;
extern template class InstrumentProxy<int16_t>;
extern template class InstrumentProxy<uint16_t>;
extern template class InstrumentProxy<int32_t>;
extern template class InstrumentProxy<uint32_t>;
}  // namespace dusk
