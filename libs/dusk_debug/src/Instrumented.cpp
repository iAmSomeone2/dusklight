//
// Created by Brenden Davidson on 8/2/26.
//

#include "dusk_debug/instrument.hpp"

using namespace dusk;

Instrumented::~Instrumented() noexcept = default;


void Instrumented::send_report() noexcept {
    // Skip reporting if value didn't change
    if (this->m_value == this->m_prev_value) return;
    this->m_prev_value = this->m_value;

    InstrumentManager::instance().submit_report({ .value_name = this->m_name->c_str(), .value = this->m_value });
}

std::uintptr_t Instrumented::get_proxy_address() const noexcept {
    return this->m_proxy_address;
}
