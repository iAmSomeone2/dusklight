//
// Created by Brenden Davidson on 8/2/26.
//

#include "dusk_debug/instrument.hpp"
#include <condition_variable>
#include <cstring>
#include <exception>

using namespace dusk;

// ============
// Instrumented
// ============

Instrumented::~Instrumented() noexcept = default;

int64_t Instrumented::get_plot_value() const noexcept {
    if (this->m_value_ptr == nullptr) return 0;

    if (this->m_value_size == sizeof(uint8_t)) {
        if (this->m_signed) return *reinterpret_cast<int8_t*>(this->m_value_ptr.get());
        return *reinterpret_cast<uint8_t*>(this->m_value_ptr.get());
    }
    if (this->m_value_size == sizeof(uint16_t)) {
        if (this->m_signed) return *reinterpret_cast<int16_t*>(this->m_value_ptr.get());
        return *reinterpret_cast<uint16_t*>(this->m_value_ptr.get());
    }
    if (this->m_value_size == sizeof(uint32_t)) {
        if (this->m_signed) return *reinterpret_cast<int32_t*>(this->m_value_ptr.get());
        return *reinterpret_cast<uint32_t*>(this->m_value_ptr.get());
    }

    // `m_value_size` not being 1, 2, or 4 bytes wide is critical error
    std::terminate();
}

void Instrumented::send_report() noexcept {
    assert(this->m_value_ptr != nullptr);

    // Skip reporting if value didn't change
    if (memcmp(this->m_value_ptr.get(), this->m_prev_value_ptr.get(), this->m_value_size) == 0) return;
    memcpy(this->m_prev_value_ptr.get(), this->m_value_ptr.get(), this->m_value_size);
    InstrumentManager::instance().submit_report({ .value_name = this->m_name.c_str(), .value = this->get_plot_value() });
}

// =================
// InstrumentManager
// =================

static void change_report_worker(std::stop_token stop_token, std::shared_ptr<PCMessageQueue> message_queue) noexcept {
    Report report;
    while (!stop_token.stop_requested()) {
        message_queue->pop(&msg, true);

    }
}

InstrumentManager::InstrumentManager() noexcept {
    this->m_plot_thread = std::jthread(&change_report_worker, this->m_message_queue);
}

void InstrumentManager::submit_report(Report& report) const noexcept {
    this->m_message_queue->push(&report, false);
}

std::optional<uint8_t> InstrumentManager::allocate_u8_proxy() noexcept {
    std::unique_lock lock(this->m_mutex);

    // Check the freed list first
    if (auto& free_u8_indices = this->m_free_slots[0]; !free_u8_indices.empty()) {
        const uint32_t index = free_u8_indices.back();
        free_u8_indices.pop_back();
        return { index };
    }

    auto& capacity = this->u8_capacity;
    // Ensure at least one slot is open
    if (capacity == 0)
        return {std::nullopt};
    // Grab the current index, decrement capacity, then return index
    return { capacity-- };
}

std::optional<uint16_t> InstrumentManager::allocate_u16_proxy() noexcept {
    std::unique_lock lock(this->m_mutex);

    // Check the freed list first
    if (auto& free_u16_indices = this->m_free_slots[1]; !free_u16_indices.empty()) {
        const uint32_t index = free_u16_indices.back();
        free_u16_indices.pop_back();
        return { index };
    }

    auto& capacity = this->u16_capacity;
    // Ensure at least one slot is open and that we don't try to use u8's space
    if (capacity <= std::numeric_limits<uint8_t>::max())
        return {std::nullopt};
    // Grab the current index, decrement capacity, then return index
    return { capacity-- };
}

std::optional<uint32_t> InstrumentManager::allocate_u32_proxy() noexcept {
    std::unique_lock lock(this->m_mutex);

    // Check the freed list first
    if (auto& free_u32_indices = this->m_free_slots[2]; !free_u32_indices.empty()) {
        const uint32_t index = free_u32_indices.back();
        free_u32_indices.pop_back();
        return { index };
    }

    auto& capacity = this->u32_capacity;
    // Ensure at least one slot is open and that we don't try to use u8 or u16's space
    if (capacity <= std::numeric_limits<uint16_t>::max())
        return { std::nullopt };
    // Grab the current index, decrement capacity, then return index
    return { capacity-- };
}

std::string const& InstrumentManager::intern_name(
    std::string_view const& name_view) noexcept {

    auto& names = this->m_name_set;

    std::unique_lock lock(this->m_mutex);
    const auto& [interned_name, did_insert] =
        names.emplace(name_view.data(), name_view.size());

    return *interned_name;
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
