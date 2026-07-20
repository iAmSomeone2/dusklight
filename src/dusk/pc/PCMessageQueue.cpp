//
// Created by Brenden Davidson on 7/15/26.
//

#include "dusk/pc/PCMessageQueue.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "dusk/main.h"

EventCount::~EventCount() {
    this->cv.notify_all();
}

void EventCount::signal() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (this->waitCount.load(std::memory_order_acquire) == 0) {
        return;
    }

    {
        std::unique_lock lock(this->mutex);
        this->sequence.fetch_add(1, std::memory_order_release);
    }
    this->cv.notify_all();
}

size_t EventCount::prepareWait() {
    const auto ecSeq = this->sequence.load(std::memory_order_acquire);
    this->waitCount.fetch_add(1, std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return ecSeq;
}

void EventCount::commitWait(const size_t ecSeq) {
    std::unique_lock lock(this->mutex);
    this->cv.wait(lock, [&]{ return this->sequence.load(std::memory_order_relaxed) != ecSeq || dusk::IsShuttingDown.load(std::memory_order_acquire); });
    this->waitCount.fetch_sub(1, std::memory_order_release);
}

void EventCount::cancelWait() {
    this->waitCount.fetch_sub(1, std::memory_order_release);
}

void EventCount::wakeAllWaiters() {
    this->cv.notify_all();
}

struct MessageCell {
    std::atomic_size_t sequence;
    OSMessage value;
};


PCMessageQueue::PCMessageQueue(const size_t capacity): capacity(capacity) {
    if (this->capacity == 0) {
        this->cells = nullptr;
        return;
    }
    if (this->capacity == 1) {
        // Bump capacity to 2 to ensure the queue works as expected.
        this->capacity = 2;
    }

    this->cells = new MessageCell*[capacity];
    for (size_t i = 0; i < this->capacity; i++) {
        const auto cell = new MessageCell { i, nullptr };
        this->cells[i] = cell;
    }
}

PCMessageQueue::~PCMessageQueue() {
    this->wakeAllWaiters();
    if (this->cells) {
        for (size_t i = 0; i < this->capacity; i++) {
            delete this->cells[i];
        }
        delete[] this->cells;
    }
}

void PCMessageQueue::wakeAllWaiters() {
    this->sendEC.wakeAllWaiters();
    this->recvEC.wakeAllWaiters();
}

size_t PCMessageQueue::msgCapacity() const {
    return this->capacity;
}

MessageCell* PCMessageQueue::msgAt(const size_t idx) const {
    ASSERT(this->capacity > 0);
    return this->cells[idx % this->capacity];
}

bool PCMessageQueue::tryPush(const OSMessage value) {
    size_t estPos = 0;
    auto locateCandidateCell = [this, &estPos]() -> MessageCell* {
        for (;;) {
            estPos = this->tail.load(std::memory_order_relaxed);
            const auto msg = this->msgAt(estPos);

            const auto seq = msg->sequence.load(std::memory_order_acquire);
            if (const auto diff = static_cast<int32_t>(seq - estPos); diff == 0) {
                return msg;
            } else if (diff > 0) {
                // estPos was stale. Go around again
                continue;
            }

            return nullptr;
        }
    };

    auto cell = locateCandidateCell();

    if (cell == nullptr) {
        return false;
    }

    // Spin until we can write to the cell
    while (!this->tail.compare_exchange_weak(estPos, estPos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
        cell = locateCandidateCell();
        if (cell == nullptr) {
            return false;
        }
    }
    // Cell is now free to write to
    cell->value = value;
    cell->sequence.store(estPos + 1, std::memory_order_release);
    this->recvEC.signal();
    return true;
}

bool PCMessageQueue::push(const OSMessage value, const bool shouldBlock) {
    if (this->msgCapacity() == 0) {
        // No capacity, cannot push
        return false;
    }

    for (;;) {
        if (this->tryPush(value)) {
            return true;
        }
        if (!shouldBlock) {
            return false;
        }

        // Wait for a slot to become available
        const auto ecSeq = this->sendEC.prepareWait();
        // Do a quick re-check before settling in
        if (this->tryPush(value)) {
            this->sendEC.cancelWait();
            return true;
        }

        // Immediate wait cancel and return if shutting down
        if (dusk::IsShuttingDown.load(std::memory_order_relaxed)) {
            this->sendEC.cancelWait();
            return false;
        }

        // Park thread while waiting for a slot to become available
        this->sendEC.commitWait(ecSeq);
    }
}

bool PCMessageQueue::tryPop(OSMessage* ret) {
    size_t estPos = 0;
    auto locateCandidateCell = [this, &estPos]() -> MessageCell* {
        for (;;) {
            estPos = this->head.load(std::memory_order_relaxed);
            const auto msg = this->msgAt(estPos);

            const auto expectedSeq = estPos + 1;
            const auto seq = msg->sequence.load(std::memory_order_acquire);
            if (const auto diff = static_cast<int32_t>(seq - expectedSeq); diff == 0) {
                return msg;
            } else if (diff > 0) {
                // estPos was stale. Go around again
                continue;
            }

            return nullptr;
        }
    };

    auto cell = locateCandidateCell();

    if (cell == nullptr) {
        return false;
    }

    // Spin until we can read from the cell
    while (!this->head.compare_exchange_weak(estPos, estPos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
        cell = locateCandidateCell();
        if (cell == nullptr) {
            return false;
        }
    }
    // Cell is now free to read from to
    if (ret) {
        // Sometimes GC code pops using a 'nullptr' to simply discard the message
        *ret = cell->value;
    }
    cell->value = nullptr;
    cell->sequence.store(estPos + this->msgCapacity(), std::memory_order_release);
    this->sendEC.signal();
    return true;
}

bool PCMessageQueue::pop(OSMessage* ret, const bool shouldBlock) {
    if (this->capacity == 0) {
        // Don't return a message if the queue is empty
        if (ret) {
            *ret = nullptr;
        }
        return true;
    }

    for (;;) {
        // Grab the jam slot if something is in it
        if (const auto msg = this->jamSlot.exchange(nullptr, std::memory_order_acquire)) {
            if (ret) {
                *ret = msg;
            }
            this->sendEC.signal();
            return true;
        }

        /// Estimate current insertion position using a relaxed load

        if (this->tryPop(ret)) {
            return true;
        }
        if (!shouldBlock) {
            return false;
        }

        // Wait for a message to be pushed
        const auto ecSeq = this->recvEC.prepareWait();
        // Do a quick re-check before settling in
        if (this->tryPop(ret)) {
            this->recvEC.cancelWait();
            return true;
        }

        // Immediate wait cancel and return if shutting down
        if (dusk::IsShuttingDown.load(std::memory_order_relaxed)) {
            this->recvEC.cancelWait();
            return false;
        }

        // Park thread while waiting for a slot to become available
        this->recvEC.commitWait(ecSeq);
    }
}

bool PCMessageQueue::jam(const OSMessage value, const bool shouldBlock) {
    if (this->msgCapacity() == 0) {
        // There is *technically* no capacity, so we can't insert
        return false;
    }

    auto tryJam = [this, value]() -> bool {
        auto slot = this->jamSlot.load(std::memory_order_acquire);

        if (slot) {
            // Data is already present, so we can't insert
            return false;
        }

        // slot is empty, so we can insert
        this->jamSlot.compare_exchange_strong(slot, value, std::memory_order_release, std::memory_order_acquire);
        this->recvEC.signal();
        return true;
    };

    for (;;) {
        if (tryJam()) {
            return true;
        }
        if (!shouldBlock) {
            return false;
        }

        // Wait for a slot to become available
        const auto ecSeq = this->sendEC.prepareWait();
        // Do a quick re-check before settling in
        if (tryJam()) {
            this->sendEC.cancelWait();
            return true;
        }

        // Immediate wait cancel and return if shutting down
        if (dusk::IsShuttingDown.load(std::memory_order_relaxed)) {
            this->sendEC.cancelWait();
            return false;
        }

        // Park thread while waiting for a slot to become available
        this->sendEC.commitWait(ecSeq);
    }
}

size_t PCMessageQueue::msgCount() const {
    const auto tailVal = this->tail.load(std::memory_order_relaxed);
    const auto headVal = this->head.load(std::memory_order_relaxed);
    return tailVal - headVal;
}
