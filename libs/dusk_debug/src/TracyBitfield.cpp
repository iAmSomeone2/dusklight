//
// Created by Brenden Davidson on 7/31/26.
//

#include "dusk_debug/TracyBitfield.hpp"
#include <format>
#include <sstream>
#include <tracy/Tracy.hpp>

void TracyBitfield::update_all_flags() noexcept {
    this->active_flags.clear();
    for (const auto [flag, flag_name] : this->flag_name_map) {
        if ((this->value & flag) == flag) {
            this->active_flags.emplace(flag_name);
        }
    }
}

void TracyBitfield::update_value(const uint8_t new_val) noexcept {
    const auto old_val = this->value;
    this->value = new_val;
    if (this->value != old_val) {
        this->update_all_flags();
        this->log_change();
    }
}

TracyBitfield::TracyBitfield(const char* field_name, const uint8_t initial_val) noexcept : value(initial_val), name(field_name) {}


TracyBitfield& TracyBitfield::with_flag_name(const uint8_t val, const char* flag_name) noexcept {
    this->flag_name_map.emplace(val, flag_name);
    return *this;
}

TracyBitfield& TracyBitfield::operator=(const uint8_t val) noexcept {
    this->update_value(val);
    return *this;
}

uint8_t TracyBitfield::operator|(const uint8_t val) const noexcept {
    return this->value | val;
}

TracyBitfield& TracyBitfield::operator|=(const uint8_t val) noexcept {
    this->update_value(*this | val);
    return *this;
}

uint8_t TracyBitfield::operator&(const uint8_t val) const noexcept {
    return this->value & val;
}

TracyBitfield& TracyBitfield::operator&=(const uint8_t val) noexcept {
    this->update_value(*this & val);
    return *this;
}

bool TracyBitfield::operator==(const uint8_t val) const noexcept {
    return this->value == val;
}

bool TracyBitfield::operator!=(const uint8_t val) const noexcept {
    return this->value != val;
}

void TracyBitfield::log_change() const noexcept {
    std::ostringstream flag_list;
    flag_list << "\tActive flags: [" << std::endl;
    for (const auto flag_name : this->active_flags) {
        flag_list << "\t\t" << flag_name << ',' << std::endl;
    }
    flag_list << "\t]";

    const std::string log_msg = std::format("{} {{\n\tvalue = 0b{:08b}\n{}\n}}", this->name, this->value, flag_list.view());
    TracyMessageS(log_msg.data(), log_msg.size(), 5);
}
