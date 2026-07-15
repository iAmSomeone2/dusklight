/**
 * Contention-free re-implementation of the message queue side-table system used in
 * 'stubs.cpp'
 */

#pragma once

#include <atomic>
#include <unordered_map>
#include <mutex>

#include <dolphin/os/OSMutex.h>
#include "PCDataRegistry.hpp"

/**
 * @brief Contention-free re-implementation of the message queue side-table system used in
 * 'stubs.cpp'
 *
 * @details
 * This implementation of the side-table idea is designed to provide a contention-free mechanism
 * for managing additional porting-related data associated with objects in the GameCube engine code.
 * For this to work, the GCType's first field must be unused in porting code and be at least 4-bytes
 * wide. This value will be used as a handle for each PCData entry, allowing access to the
 * associated porting-related data.
 *
 * @details
 * On the backend, 'PCDataRegistry' is used to manage the pointers to the PCData objects and to
 * provide a contention-free mechanism for accessing them. Handles are used to look up the actual
 * PCData pointers in the registry without requiring additional synchronization. You can think of
 * them like resource IDs in OpenGL; how the driver allocates them is opaque to the calling code.
 * The caller simply uses them to access/manipulate the associated data while the backend handles
 * the rest.
 *
 * @tparam GCType The data the original GameCube engine code used. The GCType's first field must be
 *                unused in porting code and be at least 4-bytes wide. Additionally, the `slot`
 *                function must be specialized for each GCType.
 * @tparam PCData The associated data type used by porting code for managing additional information
 *                 related to the given GCType object.
 */
template <typename GCType, typename PCData>
class OSSideTable {
public:
    /** Get a reference to the lower 4-bytes of the dead field (4-bytes aligned) to use as
     * a handle for each live entry.
     *
     * @note This function must be specialized for each GCType.
     *
     * @param obj The object for which to extract the handle from.
     * @return the extracted handle value.
     */
    static uint32_t& slot(GCType *obj);

    /** Retrieve the PCData associated with the given GCType object.
     *
     * @param obj The object for which to retrieve the associated PCData.
     * @return the associated PCData object or `nullptr` if not found.
     */
    static PCData* get(GCType *obj) {
        const std::atomic_ref<uint32_t> cachedSlot(slot(obj));
        if (const auto handle = cachedSlot.load(std::memory_order_acquire); handle) {
            return registry().resolve(handle);
        }
        return slowPath(obj);
    }

    /** Iterate all live entries under the map lock; executing the provided function for each.
     *
     * @details
     * This method's main use is to wake all waiters at shutdown and to allow the caller to run any
     * required cleanup or finalization tasks for each live entry.
     *
     * @param fn The function to execute for each live entry.
     */
    template <typename Fn>
    static void forEach(Fn&& fn) {
        std::lock_guard lock(mapMutex());
        for (auto& [_key, handle] : map()) {
            auto data = registry().resolve(handle);
            fn(*data);
        }
    }

private:
    /// Creates a new PCData object for the provided GCType and associates it with the object.
    /// This caches the handle in the map and updates the slot so that future lookups can reuse it.
    ///
    /// @param obj The GCType object to associate with the new PCData.
    /// @return The newly created PCData object.
    static PCData* slowPath(GCType* obj) {
        std::lock_guard lock(mapMutex());

        // Ensure that another thread hasn't already updated the cache
        const std::atomic_ref<uint32_t> cachedSlot(slot(obj));
        if (const auto handle = cachedSlot.load(std::memory_order_acquire); handle) {
            return registry().resolve(handle);
        }

        // Re-initializing the same associated object (ex. OSMessageQueue) nulls its slot but doesn't
        // clear a handle in the map. When that happens, we should try to locate the handle in the
        // map for reuse.
        if (const auto it = map().find(obj); it != map().end()) {
            cachedSlot.store(it->second, std::memory_order_release);
            return registry().resolve(it->second);
        }

        // Create new PCData object for the provided GCType
        auto data = new PCData();
        uint32_t handle = registry().add(data);
        map().emplace(obj, handle);

        cachedSlot.store(handle, std::memory_order_release);
        return data;
    }

    // Function-local statics are used here to avoid race conditions during initialization.

    /// Backing registry for the PCData objects.
    static PCDataRegistry<PCData>& registry() {
        static PCDataRegistry<PCData> instance;
        return instance;
    }

    /**
     * Map of GCType pointers to PCData handles.
     *
     * @note This is intentionally leaked at shutdown so that data isn't dropped before all threads
     * have exited.
     */
    static std::unordered_map<GCType*, uint32_t>& map() {
        static std::unordered_map<GCType*, uint32_t> instance;
        return instance;
    }

    /// The static map mutation mutex.
    ///
    /// @note Should only be used when modifying the map. Otherwise, rely on the registry for fast,
    /// lock-free access.
    static std::mutex& mapMutex() {
        static std::mutex mapMutex;
        return mapMutex;
    }
};

/// Forward declaration of PCMutexData from 'OSMutex.cpp'
struct PCMutexData;

template <>
inline uint32_t& OSSideTable<OSMutex, PCMutexData>::slot(OSMutex* obj) {
    return reinterpret_cast<uint32_t&>(obj->queue.head);
}

/// Forward declaration of PCCondData from 'OSMutex.cpp'
struct PCCondData;

template <>
inline uint32_t& OSSideTable<OSCond, PCCondData>::slot(OSCond* obj) {
    return reinterpret_cast<uint32_t&>(obj->queue.head);
}

