/**
 * \file Instrumented.hpp
 * \brief
 *
 * \author Brenden Davidson <brenden@bdavidson.dev>
 * \date 2026-08-16
 */

#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace dusk {
/**
 * Value instrumentation
 */
class Instrumented {
protected:
    std::uintptr_t m_proxy_address;
    /// Name used in instrumentation tools
    std::shared_ptr<std::string> m_name;
    int64_t m_value;
    int64_t m_prev_value;
    bool m_signed;

public:
    template <std::integral T>
    Instrumented(const T initial_val, std::shared_ptr<std::string> name,
        const std::uintptr_t proxy_addr) noexcept
        : m_proxy_address(proxy_addr), m_name(std::move(name)),
          m_value(static_cast<int64_t>(initial_val)),
          m_prev_value(static_cast<int64_t>(initial_val)), m_signed(std::is_signed_v<T>) {}

    ~Instrumented() noexcept;

    /// Report this value's current state to Tracy
    void send_report() noexcept;

    [[nodiscard]] std::uintptr_t get_proxy_address() const noexcept;

    template <std::integral T>
    struct Handle {
        Instrumented& m_instrumented;

        explicit Handle(Instrumented& instrumented) noexcept : m_instrumented(instrumented) {};

        ~Handle() noexcept { this->m_instrumented.send_report(); }

        T get_value() const noexcept { return this->m_instrumented.m_value; }

        void set_value(const T new_val) noexcept {
            this->m_instrumented.m_value = static_cast<int64_t>(new_val);
        }
    };
};
}  // namespace dusk
