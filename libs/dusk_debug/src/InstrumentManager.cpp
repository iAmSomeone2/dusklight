/**
 * \file InstrumentManager.cpp
 * \brief  
 *
 * \author Brenden Davidson <brenden@bdavidson.dev>
 * \date 2026-08-16
 */

#include "dusk_debug/InstrumentManager.hpp"
#include <chrono>

using namespace dusk;
using namespace dusk::detail;

std::optional<size_t> InstrumentTable::p_allocate_proxy(Instrumented&& instrumented) noexcept {
    std::unique_lock lock(this->m_mutex);

    // Grab index of first available slot
    // Array only has 256 values, so even a naïve search will be fast enough.
    for (size_t i = 0; i < TABLE_CAP; ++i) {
        if (!this->m_instrumented_vals[i].has_value()) {
            this->m_instrumented_vals[i] = std::move(instrumented);
            return { i };
        }
    }
    return { std::nullopt };
}

void InstrumentTable::deallocate_proxy(const size_t proxy_idx) noexcept {
    std::unique_lock lock(this->m_mutex);
    this->m_instrumented_vals[proxy_idx].reset();
}

std::optional<Instrumented>& InstrumentTable::operator[](const size_t index) noexcept {
    assert(index < TABLE_CAP);
    return this->m_instrumented_vals[index];
}

bool InstrumentTable::is_allocated(const size_t index) const noexcept {
    return this->m_instrumented_vals[index].has_value();
}

/**
 * Minimum possible thread sleep time accounting for OS call time.
 *
 * @remark This value was determined on a 12-core M4 Pro MacBook running macOS 27 Beta 5. Other
 * configurations will likely have different values.
 */
static constexpr std::chrono::nanoseconds MIN_THREAD_SLEEP_NS{9'000};

/**
 * Spin loop used to read reports coming through the channel and submit them to Tracy
 *
 * @remark This function is only intended to be supplied to background threads via `std::jthread`
 *
 * @param stop_token stop token provided by the `jthread` call
 * @param receiver Receiver side of the reporting Channel
 */
static void change_report_worker(std::stop_token const& stop_token, Channel<detail::CHANNEL_CAP, Report>::Receiver receiver) noexcept {
    while (!stop_token.stop_requested()) {
        if (const auto maybe_report = receiver.recv(); maybe_report.has_value()) {
            [[maybe_unused]] const auto [value_name, value] = maybe_report.value();
            TracyPlot(value_name, value);
        } else {
            std::this_thread::sleep_for(MIN_THREAD_SLEEP_NS);
        }
    }
}

InstrumentManager::InstrumentManager() noexcept {
    auto [sender, receiver] = Channel<detail::CHANNEL_CAP, Report>::make_channel("ReportChannel", false);
    this->m_report_sender = std::move(sender);
    this->m_plot_thread = std::jthread(&change_report_worker, std::move(receiver));
}

InstrumentManager::~InstrumentManager() noexcept {
    this->m_plot_thread.request_stop();
    this->m_plot_thread.join();
}

void InstrumentManager::submit_report(Report const& report) noexcept {
    this->m_report_sender.value().send(report);
}

InstrumentManager& InstrumentManager::instance() noexcept {
    static InstrumentManager instance;
    return instance;
}

// =========================
// Supported Specializations
// =========================
//
// The following section declares and instantiates all currently-supported specializations of `InstrumentProxy`.
// Each of these is matched with a compile-time size check to provide immediate, traceable feedback in the event that
// the sizing is no longer aligned with the type being proxied.

template class dusk::InstrumentProxy<int8_t>;
static_assert(sizeof(InstrumentProxy<int8_t>) == sizeof(int8_t), "InstrumentProxy<int8_t> is not the same size as int8_t");

template class dusk::InstrumentProxy<uint8_t>;
static_assert(sizeof(InstrumentProxy<uint8_t>) == sizeof(uint8_t), "InstrumentProxy<uint8_t> is not the same size as uint8_t");

template class dusk::InstrumentProxy<int16_t>;
static_assert(sizeof(InstrumentProxy<int16_t>) == sizeof(int16_t), "InstrumentProxy<int16_t> is not the same size as int16_t");

template class dusk::InstrumentProxy<uint16_t>;
static_assert(sizeof(InstrumentProxy<uint16_t>) == sizeof(uint16_t), "InstrumentProxy<uint16_t> is not the same size as uint16_t");

template class dusk::InstrumentProxy<int32_t>;
static_assert(sizeof(InstrumentProxy<int32_t>) == sizeof(int32_t), "InstrumentProxy<int32_t> is not the same size as int32_t");

template class dusk::InstrumentProxy<uint32_t>;
static_assert(sizeof(InstrumentProxy<uint32_t>) == sizeof(uint32_t), "InstrumentProxy<uint32_t> is not the same size as uint32_t");
