//! Contention-free re-implementation of the message queue side-table system used in
//! 'stubs.cpp'
#pragma once

#include <atomic>
#include <unordered_map>
#include <memory>
#include <mutex>

#include "dolphin/os/OSMessage.h"
#include "dolphin/os/OSMutex.h"

// TODO: Update this to handle 4-byte aligned data.
// See 'notes/osside-table-alignment-fix.md' for details

template <typename GCType, typename PCData>
class OSSideTable {
public:
    /// Reference to the dead pointer field
    static void*& slot(GCType *obj);

    ///
    static PCData* get(GCType *obj) {
        const std::atomic_ref<void*> cachedSlot(slot(obj));
        if (void* cached = cachedSlot.load(std::memory_order_acquire); cached) {
            return static_cast<PCData*>(cached);
        }
        return slowPath(obj);
    }

    /// Iterate all live entries under the map lock - used at shutdown to wake every waiter.
    template <typename Fn>
    static void forEach(Fn&& fn) {
        std::lock_guard lock(mapMutex());
        for (auto& [_key, value] : map()) {
            fn(*value);
        }
    }

private:
    static PCData* slowPath(GCType* obj) {
        std::lock_guard lock(mapMutex());

        // Ensure that another thread hasn't already updated the cache
        const std::atomic_ref<void*> cachedSlot(slot(obj));
        if (void* cached = cachedSlot.load(std::memory_order_acquire)) {
            return static_cast<PCData*>(cached);
        }

        auto [it, inserted] = map().try_emplace(obj, std::make_unique<PCData>());
        PCData* result = it->second.get();

        cachedSlot.store(result, std::memory_order_release);
        return result;
    }

    /// Function-local statics: lazy-init, safe w.r.t DLL static-init order.
    static std::unordered_map<GCType*, std::unique_ptr<PCData>>& map() {
        static std::unordered_map<GCType*, std::unique_ptr<PCData>> instance;
        return instance;
    }

    /// The static map mutation mutex.
    static std::mutex& mapMutex() {
        static std::mutex mapMutex;
        return mapMutex;
    }
};

/// Forward declaration of PCMessageQueueData from 'stubs.cpp'
struct PCMessageQueueData;

template <>
inline void*& OSSideTable<OSMessageQueue, PCMessageQueueData>::slot(OSMessageQueue* obj) {
    return reinterpret_cast<void*&>(obj->queueSend.head);
}

/// Forward declaration of PCMutexData from 'OSMutex.cpp'
struct PCMutexData;

template <>
inline void*& OSSideTable<OSMutex, PCMutexData>::slot(OSMutex* obj) {
    return reinterpret_cast<void*&>(obj->queue.head);
}

/// Forward declaration of PCCondData from 'OSMutex.cpp'
struct PCCondData;

template <>
inline void*& OSSideTable<OSCond, PCCondData>::slot(OSCond* obj) {
    return reinterpret_cast<void*&>(obj->queue.head);
}

