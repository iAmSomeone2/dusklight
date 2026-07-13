// OSMutex.cpp - PC implementation of GameCube OSMutex/OSCond API
// Uses std::recursive_mutex and std::condition_variable_any behind the
// unchanged GameCube C API. The OSMutex struct layout is preserved so
// game code can read its fields.

#include <dolphin/dolphin.h>
#include <dolphin/os.h>

#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <cstdlib>

#include "JSystem/JKernel/JKRHeap.h"
#include "OSSideTable.hpp"

// ============================================================================
// Side-table: native mutex per OSMutex
// ============================================================================

struct PCMutexData {
    std::recursive_mutex nativeMutex;
};

using MutexSideTable = OSSideTable<OSMutex, PCMutexData>;

// ============================================================================
// Side-table: native condition variable per OSCond
// ============================================================================

struct PCCondData {
    std::condition_variable_any cv;
};

using CondSideTable = OSSideTable<OSCond, PCCondData>;

void WakeAllCondWaiters() {
    CondSideTable::forEach([](PCCondData& data) {
        data.cv.notify_all();
    });
}

// ============================================================================
// C API functions
// ============================================================================

extern "C" {

void OSInitMutex(OSMutex* mutex) {
    if (!mutex) return;
    OSInitThreadQueue(&mutex->queue);
    mutex->thread = nullptr;
    mutex->count  = 0;

    // Create/reset side-table entry
    MutexSideTable::get(mutex);
}

void OSLockMutex(OSMutex* mutex) {
    if (!mutex) return;

    PCMutexData* data = MutexSideTable::get(mutex);
    data->nativeMutex.lock();

    // Update GC-visible fields
    OSThread* currentThread = OSGetCurrentThread();
    mutex->thread = currentThread;
    mutex->count++;
}

void OSUnlockMutex(OSMutex* mutex) {
    if (!mutex) return;

    OSThread* currentThread = OSGetCurrentThread();
    if (mutex->thread != currentThread) return;

    mutex->count--;
    if (mutex->count == 0) {
        mutex->thread = nullptr;
    }

    PCMutexData* data = MutexSideTable::get(mutex);
    data->nativeMutex.unlock();
}

BOOL OSTryLockMutex(OSMutex* mutex) {
    if (!mutex) return FALSE;

    PCMutexData* data = MutexSideTable::get(mutex);
    if (data->nativeMutex.try_lock()) {
        OSThread* currentThread = OSGetCurrentThread();
        mutex->thread = currentThread;
        mutex->count++;
        return TRUE;
    }
    return FALSE;
}

// ============================================================================
// Internal: unlock all mutexes held by a thread (called on thread exit)
// ============================================================================

void __OSUnlockAllMutex(OSThread* thread) {
    // On GC this walks the thread's mutex queue.
    // On PC the native mutexes are cleaned up when threads exit.
    // Clear the GC-visible queue.
    if (!thread) return;
    thread->queueMutex.head = nullptr;
    thread->queueMutex.tail = nullptr;
}

int __OSCheckDeadLock(OSThread* thread) {
    // Simplified: native OS handles deadlock detection.
    return 0;
}

int __OSCheckMutexes(OSThread* thread) {
    return 1;
}

// ============================================================================
// Condition Variable API
// ============================================================================

void OSInitCond(OSCond* cond) {
    if (!cond) return;
    OSInitThreadQueue(&cond->queue);
    CondSideTable::get(cond);
}

void OSWaitCond(OSCond* cond, OSMutex* mutex) {
    if (!cond || !mutex) return;

    PCCondData* condData = CondSideTable::get(cond);
    PCMutexData* mutexData = MutexSideTable::get(mutex);

    // Save and clear the GC mutex state
    OSThread* currentThread = OSGetCurrentThread();
    s32 savedCount = mutex->count;
    mutex->count = 0;
    mutex->thread = nullptr;

    // Keep one recursion level held so cv.wait() is what releases the mutex;
    // fully unlocking before the wait opens a window where a signal is lost.
    if (savedCount >= 1) {
        for (s32 i = 1; i < savedCount; i++) {
            mutexData->nativeMutex.unlock();
        }
        std::unique_lock lock(mutexData->nativeMutex, std::adopt_lock);
        condData->cv.wait(lock);
        lock.release();
        for (s32 i = 1; i < savedCount; i++) {
            mutexData->nativeMutex.lock();
        }
    } else {
        // Mutex wasn't held on entry (contract violation); wait anyway.
        std::unique_lock lock(mutexData->nativeMutex);
        condData->cv.wait(lock);
    }

    // Restore GC mutex state
    mutex->thread = currentThread;
    mutex->count  = savedCount;
}

void OSSignalCond(OSCond* cond) {
    if (!cond) return;
    PCCondData* condData = CondSideTable::get(cond);
    condData->cv.notify_all();
}

#ifdef __cplusplus
}
#endif
