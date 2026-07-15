//
// Created by Brenden Davidson on 7/15/26.
//

#pragma once

#include <thread>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <atomic>

#include <dolphin/os/OSMessage.h>

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
    alignas(std::hardware_destructive_interference_size) std::atomic_size_t waitCount = 0;
    std::mutex mutex;
    std::condition_variable cv;

public:
    ~EventCount();

    void signal();

    size_t prepareWait();

    void commitWait(size_t ecSeq);

    void cancelWait();
};

/**
 * Lock-free, wait-free, multi-producer, single-consumer message queue.
 *
 * @details
 * This implementation uses a lock-free, wait-free, multi-producer, single-consumer message queue.
 * It wraps 'OSMessageQueue' on ported platforms.
 */
class PCMessageQueue {
    alignas(std::hardware_destructive_interference_size) std::atomic_size_t tail = 0;
    alignas(std::hardware_destructive_interference_size) std::atomic_size_t head = 0;

    // Sequencing details for producers/consumers which elected to block
    EventCount sendEC;
    EventCount recvEC;

    size_t capacity;
    MessageCell** cells;


    MessageCell* msgAt(size_t idx) const;

    bool tryPush(OSMessage value);

    bool tryPop(OSMessage* ret);
public:
    explicit PCMessageQueue(size_t capacity);

    ~PCMessageQueue();

    size_t msgCapacity() const;

    bool push(OSMessage value, bool shouldBlock = false);

    bool pop(OSMessage* ret, bool shouldBlock = false);
};
