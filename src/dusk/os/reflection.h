/**
 * @file reflection.h
 * @brief Basic runtime reflection.
 *
 * Some porting code makes use of side tables to dynamically switch out GC-specific behavior for
 * multi-platform alternatives. To better support this, we define a set of reflective types and
 * provide a runtime check to determine if a pointer is of a reflective type.
 *
 * This works pretty much the same way as how Vulkan does it. A statically sized enum value must be
 * the very first member of the target struct or class. Then, `isType` simply reads the first few
 * bytes of the pointer to determine if it matches the specified reflective type.
 *
 * @author Brenden Davidson <brenden@bdavidson.dev>
 * @date 2026-07-15
 */

#pragma once


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
 * @param ptr Pointer to check.
 * @param refType Reflective type to check against.
 * @return True if the pointer is of the specified reflective type, false otherwise.
 */
bool isType(void* ptr, ReflectiveType refType);