/**
 * @file PCMessageQueue.hpp
 * @brief Bounded lock-free MPMC queue for ported platforms
 *
 * Based on Dmitry Vyukov's "Bounded MPMC queue" algorithm, this queue is capable of working fully
 * lock-free on any platform supporting atomic operations.
 *
 * To ensure compatibility with existing GC code, both consumers and producers may optionally wait
 * for space or messages to become available. Additionally, front-of-line insertion is supported
 * through the use of a dedicated 'jamSlot' which preempts the queue's normal sequencing without
 * violating the invariance requirements of Vyukov's algorithm.
 *
 * @author Brenden Davidson <brenden@bdavidson.dev>
 * @date 2025-07-15
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <atomic>

#include <dolphin/os/OSMessage.h>

#include "dusk/os/reflection.h"

#if TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

/**
 * Wrapper struct used for sequencing stored messages with a PCMessageQueue.
 */
struct MessageCell;

/**
 * Event count sequencer for PCMessageQueue.
 */
struct EventCount {
private:
    alignas(std::hardware_destructive_interference_size) std::atomic_size_t sequence = 0;
    std::atomic_size_t waitCount = 0;
#if TRACY_ENABLE
    TracyLockable(std::mutex, mutex);
#else
    std::mutex mutex;
#endif
    std::condition_variable cv;

public:
    ~EventCount();

    void signal();

    size_t prepareWait();

    void commitWait(size_t ecSeq);

    void cancelWait();

    void wakeAllWaiters();
};

/**
 * Lock-free, wait-optional, multi-producer, multi-consumer message queue for ported platforms.
 */
class PCMessageQueue {
    /// Enum value used for runtime type reflection
    ReflectiveType ty = REFLECTIVE_TYPE_MESSAGE_QUEUE;

    alignas(std::hardware_destructive_interference_size) std::atomic_size_t tail = 0;
    std::atomic_size_t head = 0;

    // Sequencing details for producers/consumers which elected to block
    EventCount sendEC;
    EventCount recvEC;

    /// Specialized slot for a single top-priority message sent via jam()
    std::atomic<OSMessage> jamSlot = nullptr;

    size_t capacity;
    MessageCell** cells;


    MessageCell* msgAt(size_t idx) const;

    bool tryPush(OSMessage value);

    bool tryPop(OSMessage* ret);
public:
    explicit PCMessageQueue(size_t requested_capacity);

    ~PCMessageQueue();

    void wakeAllWaiters();

    size_t msgCapacity() const;

    bool push(OSMessage value, bool shouldBlock = false);

    bool pop(OSMessage* ret, bool shouldBlock = false);

    bool jam(OSMessage value, bool shouldBlock = false);

    size_t msgCount() const;
};
