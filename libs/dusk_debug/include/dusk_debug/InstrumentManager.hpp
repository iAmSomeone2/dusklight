/**
 * \file InstrumentManager.hpp
 * \brief
 *
 * \author Brenden Davidson <brenden@bdavidson.dev>
 * \date 2026-08-16
 */

#pragma once

#include <array>
#include <cassert>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tracy/Tracy.hpp>
#include "PlotNameManager.hpp"
#include "dusk_debug/channel.hpp"
#include "dusk_debug/instrument.hpp"

namespace dusk {
struct Report {
    const char* value_name;
    int64_t value;
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
     * @remark Should allocating the proxy fail, the InstrumentationManager will throw a
     * `ProxyAllocError` causing this constructor to call `std::terminate`. Details of the failure
     * will be logged.
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

    /**
     * Try to restore a proxy using its index
     *
     * @param proxied_val
     * @return
     */
    static void restore(T* proxied_val);

    operator T() noexcept { return this->get_handle().get_value(); }

    T operator++(int) noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val + 1);
        return old_val;
    }

    T operator++() noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val + 1);
        return handle.get_value();
    }

    T operator--(int) noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val - 1);
        return old_val;
    }

    T operator--() noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val - 1);
        return handle.get_value();
    }

    InstrumentProxy& operator+=(const T other) noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val + other);
        return *this;
    }

    InstrumentProxy& operator-=(const T other) noexcept {
        Instrumented::Handle<T> handle = get_handle();
        T old_val = handle.get_value();
        handle.set_value(old_val - other);
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

namespace detail {
static constexpr size_t CHANNEL_CAP = 1024;

static constexpr size_t TABLE_CAP = std::numeric_limits<uint8_t>::max() + 1;

class InstrumentTable {
    std::array<std::optional<Instrumented>, TABLE_CAP> m_instrumented_vals{};

    std::mutex m_mutex{};

    std::optional<size_t> p_allocate_proxy(Instrumented&& instrumented) noexcept;

public:
    template <std::integral T>
    std::optional<T> allocate_proxy(Instrumented&& instrumented) noexcept {
        if (const auto maybe_index = this->p_allocate_proxy(std::move(instrumented));
            maybe_index.has_value())
        {
            return {static_cast<T>(maybe_index.value())};
        }
        return {std::nullopt};
    }

    void deallocate_proxy(size_t proxy_idx) noexcept;

    std::optional<Instrumented>& operator[](size_t index) noexcept;

    [[nodiscard]] bool is_allocated(size_t index) const noexcept;
};
}  // namespace detail

class ProxyAllocError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    // explicit ProxyAllocError(std::string const& what_arg);
    // explicit ProxyAllocError(const char* what_arg);
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

    /// Table containing instrumentation for 1-byte values
    detail::InstrumentTable m_ui8_table;
    /// Table containing instrumentation for 2-bytes values
    detail::InstrumentTable m_ui16_table;
    /// Table containing instrumentation for 4-byte values
    detail::InstrumentTable m_ui32_table;

    /// Send side of a bounded MPSC queue used to send value updates to the plot thread
    std::optional<Channel<detail::CHANNEL_CAP, Report>::Sender> m_report_sender;

    /// Background thread used to send updates to Tracy
    std::jthread m_plot_thread;

    template <std::integral T>
    detail::InstrumentTable& select_table() noexcept {
        // Assert that T is one of the expected bit-widths
        static_assert(sizeof(T) == sizeof(uint8_t) || sizeof(T) == sizeof(uint16_t) ||
                          sizeof(T) == sizeof(uint32_t),
            "Invalid type size");

        if constexpr (sizeof(T) == sizeof(uint8_t)) {
            return m_ui8_table;
        } else if constexpr (sizeof(T) == sizeof(uint16_t)) {
            return m_ui16_table;
        } else {
            return m_ui32_table;
        }
    }

protected:
    InstrumentManager() noexcept;

public:
    InstrumentManager(InstrumentManager&) = delete;
    InstrumentManager(InstrumentManager&&) = delete;
    InstrumentManager& operator=(InstrumentManager&) = delete;
    InstrumentManager& operator=(InstrumentManager&&) = delete;

    ~InstrumentManager() noexcept;

    /// Gets the singleton instance of InstrumentManager
    static InstrumentManager& instance() noexcept;

    void submit_report(Report const& report) noexcept;

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
        auto& table = select_table<T>();
        auto& instrument = table[index];
        assert(instrument.has_value());
        return Instrumented::Handle<T>(instrument.value());
    }

    /**
     * Registers a new proxy, returning its lookup index.
     *
     * @remark This method will intentionally panic should the user try to register an excessive
     * number (>256) of proxies for a given bit-width (1, 2, or 4-byte).
     *
     * @tparam T type of value being proxied
     * @param proxy_addr address where the proxy lives
     * @param debug_name static name used in debug tooling
     * @param initial_value initial stored value
     * @return newly registered lookup index
     * @throws ProxyAllocError if the given type's table is full and cannot allocate another proxy
     */
    template <std::integral T>
    T register_proxy(const std::uintptr_t proxy_addr, std::string_view const& debug_name, T initial_value = 0) {
        constexpr size_t size_of_t = sizeof(T);
        static_assert(size_of_t <= sizeof(uint32_t), "Invalid type size");

        const auto& name = PlotNameManager::instance().intern_name(std::format("{} [0x{:X}]", debug_name, proxy_addr));
        // Get the appropriate index for the value's type.
        // Assertions are used to immediately panic if we run out of indices.
        auto& table = select_table<T>();
        std::optional<T> out_idx =
            table.template allocate_proxy<T>(Instrumented{initial_value, name, proxy_addr});

        if (!out_idx.has_value()) {
            throw ProxyAllocError(
                std::format("failed to allocate {:d}-byte proxy \"{}\"", size_of_t, *name));
        }

        return out_idx.value();
    }

    /**
     * Unregisters the given proxy
     *
     * @tparam T type of value being proxied
     * @param proxy proxy object instance
     */
    template <std::integral T>
    void unregister_proxy(const InstrumentProxy<T>& proxy) noexcept {
        auto& table = select_table<T>();
        const auto proxy_idx = proxy.get_index();
        table.deallocate_proxy(proxy_idx);
    }

    template <std::integral T>
    [[nodiscard]] bool index_is_allocated(const size_t index) noexcept {
        auto& table = select_table<T>();
        return table.is_allocated(index);
    }

    template <std::integral T>
    [[nodiscard]] std::optional<size_t> locate_proxy_index(const std::uintptr_t proxy_addr) noexcept {
        auto& table = select_table<T>();
        for (auto i = 0; i < detail::TABLE_CAP; ++i) {
            const auto instrument_slot = table[i];
            if (instrument_slot.has_value() && instrument_slot.value().get_proxy_address() == proxy_addr) {
                return { i };
            }
        }

        return { std::nullopt };
    }
};

template <std::integral T>
InstrumentProxy<T>::InstrumentProxy(std::string_view const& debug_name, T initial_val) noexcept {
    const auto proxy_addr = reinterpret_cast<std::uintptr_t>(this);
    this->m_index = InstrumentManager::instance().register_proxy<T>(proxy_addr, debug_name, initial_val);
}

template <std::integral T>
InstrumentProxy<T>::~InstrumentProxy() noexcept {
    InstrumentManager::instance().unregister_proxy(*this);
}

template <std::integral T>
void InstrumentProxy<T>::restore(T* proxied_val) {
    const auto proxy_addr = reinterpret_cast<std::uintptr_t>(proxied_val);
    const auto proxy_idx = InstrumentManager::instance().locate_proxy_index<T>(proxy_addr);
    if (proxy_idx.has_value()) {
        *proxied_val = proxy_idx.value();
    } else {
        throw std::runtime_error(
            std::format("no registered {:d}-byte proxy for value at [0x{:X}]", sizeof(T), proxy_addr));
    }
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
