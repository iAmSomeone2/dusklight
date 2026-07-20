//
// Created by Brenden Davidson on 7/15/26.
//

#pragma once

#include <cstdint>

/**
 * Registry of reflective types.
 *
 * Add Reflective types here and add the matching ReflectiveType enum value as the first member of
 * your target struct or class.
 */
enum ReflectiveType {
    REFLECTIVE_TYPE_NONE = 0,
    REFLECTIVE_TYPE_MESSAGE_QUEUE = 1,
};

/**
 * Runtime check to determine if a pointer is of a reflective type.
 *
 * @note
 * This is pretty much the same idea as how Vulkan does it.
 *
 * @param ptr Pointer to check.
 * @param refType Reflective type to check against.
 * @return True if the pointer is of the specified reflective type, false otherwise.
 */
bool isType(void* ptr, ReflectiveType refType);