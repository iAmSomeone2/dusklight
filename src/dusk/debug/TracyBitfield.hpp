//
// Created by Brenden Davidson on 7/31/26.
//

#pragma once
#include <cstdint>
#include <unordered_map>
#include <set>

class TracyBitfield {
    uint8_t value;
    const char* name;

    std::unordered_map<uint8_t, const char*> flag_name_map;
    std::set<const char*> active_flags;

    void log_change() const noexcept;

    void update_all_flags() noexcept;

    void update_value(uint8_t new_val) noexcept;
public:
    explicit TracyBitfield(const char* field_name, uint8_t initial_val = 0) noexcept;

    /**
     * Add a named flag for debug output
     * @param val flag value
     * @param flag_name name of this flag
     * @return reference to the TracyBitfield instance, useful for chaining
     */
    TracyBitfield& with_flag_name(uint8_t val, const char* flag_name) noexcept;

    TracyBitfield& operator=(uint8_t val) noexcept;

    uint8_t operator|(uint8_t val) const noexcept;

    TracyBitfield& operator|=(uint8_t val) noexcept;

    uint8_t operator&(uint8_t val) const noexcept;

    TracyBitfield& operator&=(uint8_t val) noexcept;

    bool operator==(uint8_t val) const noexcept;

    bool operator!=(uint8_t val) const noexcept;
};
