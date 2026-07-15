//
// Created by Brenden Davidson on 7/15/26.
//

#include "PCMessageQueue.hpp"
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

struct MessageCell {
    std::atomic_size_t sequence;
    OSMessage value;
};


PCMessageQueue::PCMessageQueue(const size_t capacity): capacity(capacity) {
    this->cells = new MessageCell*[capacity];
    for (size_t i = 0; i < this->capacity; i++) {
        const auto cell = new MessageCell(i, nullptr);
        this->cells[i] = cell;
    }
}

PCMessageQueue::~PCMessageQueue() {
    for (auto i = 0; i < this->capacity; i++) {
        delete this->cells[i];
    }
    delete this->cells;
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
        // TODO: implement special case for 0-sized queues
        return true;
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

    // Spin until we can write to the cell
    while (!this->head.compare_exchange_weak(estPos, estPos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
        cell = locateCandidateCell();
        if (cell == nullptr) {
            return false;
        }
    }
    // Cell is now free to write to
    *ret = cell->value;
    cell->value = nullptr;
    cell->sequence.store(estPos + this->msgCapacity(), std::memory_order_release);
    this->sendEC.signal();
    return true;
}

bool PCMessageQueue::pop(OSMessage* ret, const bool shouldBlock) {
    if (this->capacity == 0) {
        *ret = nullptr;
        return true;
    }

    /// Estimate current insertion position using a relaxed load
    for (;;) {
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
