//
// Created by Brenden Davidson on 7/15/26.
//

#pragma once

#include <thread>
#include <condition_variable>
#include <mutex>
#include <atomic>

#include <dolphin/os/OSMessage.h>

#include "../os/reflection.h"

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
    std::mutex mutex;
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
