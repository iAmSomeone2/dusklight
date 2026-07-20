//
// Created by Brenden Davidson on 7/20/26.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <memory_resource>

#if TRACY_ENABLE
#include <fmt/format.h>
#include <string>
#include <tracy/Tracy.hpp>
#endif

namespace dusk::pc {
#if TRACY_ENABLE
enum AllocEventType : uint8_t { ALLOC, FREE, RESET };

/**
 * Debug allocation statistics
 */
struct AllocStats {
    std::atomic<size_t> bytes_in_use{0};
    std::atomic<size_t> high_water_mark{0};
    std::atomic<size_t> alloc_count{0};

    void on_alloc(const std::size_t size) noexcept {
        const auto now = this->bytes_in_use.fetch_add(size, std::memory_order_relaxed) + size;
        auto prev_hwm = this->high_water_mark.load(std::memory_order_acquire);

        while (now > prev_hwm && !this->high_water_mark.compare_exchange_weak(
                                     prev_hwm, now, std::memory_order_relaxed, std::memory_order_relaxed))
        {
        }

        this->alloc_count.fetch_add(1, std::memory_order_relaxed);
        this->write_to_tracy(ALLOC, size);
    }

    void on_free(const std::size_t size) noexcept {
        this->bytes_in_use.fetch_sub(size, std::memory_order_relaxed);
        this->write_to_tracy(FREE, size);
    }

    void reset_bytes_in_use() noexcept {
        this->bytes_in_use.store(0, std::memory_order_relaxed);
        this->write_to_tracy(RESET, 0);
    }

private:
    void write_to_tracy(const AllocEventType event_type, const std::size_t size) const noexcept {
        std::string log_msg;
        switch (event_type) {
        case ALLOC:
            log_msg = fmt::format("Allocated: {} bytes", size);
            break;
        case FREE:
            log_msg = fmt::format("Freed: {} bytes", size);
            break;
        case RESET:
            log_msg = fmt::format("Heap reset");
            break;
        }

        const auto current_bytes_in_use =
            static_cast<int64_t>(this->bytes_in_use.load(std::memory_order_acquire));
        TracyMessage(log_msg.c_str(), log_msg.length());
        TracyPlot("[PCHeap] allocated bytes", current_bytes_in_use);

        const auto hwm =
            static_cast<int64_t>(this->high_water_mark.load(std::memory_order_acquire));
        TracyPlot("[PCHeap] high water mark (bytes)", hwm);

        const auto allocs = static_cast<int64_t>(this->alloc_count.load(std::memory_order_acquire));
        TracyPlot("[PCHeap] alloc count", allocs);
    }
};
#endif

/**
 * Port-specific heap implementation.
 *
 * @details
 * Owns a fixed-size memory pool, bump-allocates from it, and resets wholesale at well-defined
 * points.
 *
 * @tparam CAPACITY The capacity of the heap in bytes.
 */
template <const size_t CAPACITY>
class PCHeap : public std::pmr::memory_resource {
#if TRACY_ENABLE
    AllocStats alloc_stats{};
#endif

    /// The backing memory pool aligned to the target architecture's cache line size.
    alignas(std::hardware_destructive_interference_size) std::byte pool[CAPACITY];

    /// The current cursor into the memory pool.
    std::atomic_uintptr_t cursor{reinterpret_cast<uintptr_t>(this->pool)};

public:
    PCHeap() = default;

    ~PCHeap() override {

    }

    PCHeap(const PCHeap&) = delete;

    PCHeap& operator=(const PCHeap&) = delete;

    /**
     * Reset the heap, freeing all allocated memory.
     */
    void reset() noexcept {
        this->cursor.store(reinterpret_cast<uintptr_t>(this->pool), std::memory_order_release);
#if TRACY_ENABLE
        this->alloc_stats.reset_bytes_in_use();
#endif
    }

    /**
     * Get the number of bytes allocated for this heap.
     *
     * @return Number of bytes allocated for this heap.
     */
    static size_t capacity() noexcept { return CAPACITY; }

    /**
     * Get the number of bytes currently allocated from this heap.
     *
     * @return Number of bytes currently allocated from this heap.
     */
    size_t in_use() const noexcept {
        const auto cursor_val = this->cursor.load(std::memory_order_acquire);
        return static_cast<size_t>(cursor_val - reinterpret_cast<uintptr_t>(this->pool));
    }

    /**
     * Get the number of bytes currently available for allocation from this heap.
     *
     * @return Number of bytes currently available for allocation from this heap.
     */
    size_t available() const noexcept { return CAPACITY - this->in_use(); }

    /**
     * Exception-free allocate
     *
     * @param bytes number of bytes to allocate
     * @param alignment alignment of the allocated memory
     * @return pointer to allocated memory or nullptr if allocation failed
     */
    void* try_allocate(const size_t bytes, const size_t alignment = alignof(std::max_align_t)) noexcept {
        try {
            return this->allocate(bytes, alignment);
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
    }
private:
    void* do_allocate(const std::size_t bytes, const std::size_t alignment) override {
        // Validate that alignment is a power of two
        assert((alignment & (alignment-1)) == 0);

        for (;;) {
            auto addr = this->cursor.load(std::memory_order_relaxed);
            const auto aligned = align_up(addr, alignment);
            auto next = aligned + bytes;
            if (next > this->pool + CAPACITY) {
                // Not enough space.
                throw std::bad_alloc{};
            }
            if (!this->cursor.compare_exchange_strong(addr, reinterpret_cast<uintptr_t>(next), std::memory_order_release, std::memory_order_relaxed)) {
                continue;
            }
#if TRACY_ENABLE
            this->alloc_stats.on_alloc(bytes);
#endif
            return aligned;
        }
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {
        // Intentional no-op given that this heap is not expected to deallocate outside of resets
    }

    bool do_is_equal(memory_resource const& other) const noexcept override {
        return this == &other;
    }

    /**
     * Aligns a pointer up to the given alignment.
     *
     * @param alignment Alignment to align to.
     * @return Aligned pointer.
     */
    static std::byte* align_up(const uintptr_t ptr, const std::size_t alignment) {
        const auto aligned = (ptr + (alignment - 1)) & ~(alignment - 1);
        return reinterpret_cast<std::byte*>(aligned);
    }
};
}  // namespace dusk::pc
