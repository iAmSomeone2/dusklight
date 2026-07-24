/**
 * @file PCHeap.hpp
 * @brief Basic, port-specific heap implementation using a bump allocator.
 *
 * Twilight Princess' original GameCube code usually uses JKRHeap for any heap allocations at run
 * time. Normally, this is fine because the GC has a known, static amount of RAM available to
 * running games and TP never goes over this budget. This budget is also magnitudes lower than what
 * any modern machine would have to worry about. Unfortunately, that small, static heap budget can
 * be a problem for sections of the code base we have to modify to make work on modern hardware.
 *
 * Using THP movie playback as an example; the GC code only buffers a couple dozen milliseconds of
 * audio at a time at regular intervals to match how the original audio hardware works. In the port
 * code, we use SDL as a stand-in for that and many other low-level systems. SDL's audio system
 * requires a larger buffer to work correctly, but we can't simply ask JKRHeap for a bigger
 * allocation. The game code is very particular about how its memory is laid out and uses nearly
 * every byte it can. Asking JKRHeap for a bigger audio buffer goes over-budget and crashes the
 * game. We also can't simply use a plain array or vector due to pointer alignment expectations in
 * the original code.
 *
 * That's where PCHeap comes in! It's a simple subclass of `std::pmr::memory_resource` backed by a
 * statically sized memory region using basic bump allocation. While not quite a drop-in replacement
 * for JKRHeap, it solves our issue of needing larger memory pools that can allocate using arbitrary
 * alignments. It also reports some allocation statistics to Tracy when that's enabled.
 *
 * @author Brenden Davidson <brenden@bdavidson.dev>
 * @date 2026-07-22
 */

#pragma once

#include <atomic>
#include <array>
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
    /// Name to use for the associated heap plot data
    const char* heap_name;
    /// Max capacity of the associated heap
    size_t heap_capacity;

    std::string byte_plot_name;
    std::string hwm_plot_name;
    std::string alloc_plot_name;
    std::string pct_used_plot_name;

    /// Current number of bytes allocated from the heap
    std::atomic<size_t> bytes_in_use{0};
    /// Most bytes allocated from the heap during its lifetime
    std::atomic<size_t> high_water_mark{0};
    /// Number of allocations from the heap
    std::atomic<size_t> alloc_count{0};

    AllocStats(const char* name, const size_t capacity)  noexcept : heap_capacity(capacity) {
        this->heap_name = name != nullptr ? name : "PCHeap";

        this->byte_plot_name = fmt::format("[{}] Bytes in Use", name);
        this->pct_used_plot_name = fmt::format("[{}] % Used", name);
        this->hwm_plot_name = fmt::format("[{}] High Water Mark", name);
        this->alloc_plot_name = fmt::format("[{}] Allocations", name);


        TracyPlotConfig(this->byte_plot_name.c_str(), tracy::PlotFormatType::Memory, true, true, tracy::Color::DarkCyan);
        TracyPlotConfig(this->pct_used_plot_name.c_str(), tracy::PlotFormatType::Percentage, false, true, tracy::Color::Green2);
        TracyPlotConfig(this->hwm_plot_name.c_str(), tracy::PlotFormatType::Memory, true, true, tracy::Color::DarkBlue);
        TracyPlotConfig(this->alloc_plot_name.c_str(), tracy::PlotFormatType::Number, true, true, tracy::Color::Yellow);

        this->zero_out_plots();
    }

    ~AllocStats() {
        this->zero_out_plots();
    }

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
    void zero_out_plots() const noexcept {
        // Tracy only lets you plot with variables, so I had to add this line.
        constexpr int64_t zero = 0;
        TracyPlot(this->byte_plot_name.c_str(), zero);
        TracyPlot(this->pct_used_plot_name.c_str(), zero);
        TracyPlot(this->alloc_plot_name.c_str(), zero);
        TracyPlot(this->hwm_plot_name.c_str(), zero);
    }

    /**
     * Calculates the percentage of heap capacity in-use (out of 100)
     *
     * @return percentage of heap capacity in-use (out of 100)
     */
    uint32_t pct_in_use() const noexcept {
        const auto in_use = static_cast<float>(this->bytes_in_use.load(std::memory_order_acquire));
        return static_cast<uint32_t>((in_use / static_cast<float>(this->heap_capacity)) * 100.0f);
    }

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
        TracyPlot(this->byte_plot_name.c_str(), current_bytes_in_use);

        const auto pct_used = static_cast<int64_t>(this->pct_in_use());
        TracyPlot(this->pct_used_plot_name.c_str(), pct_used);

        const auto hwm =
            static_cast<int64_t>(this->high_water_mark.load(std::memory_order_acquire));
        TracyPlot(this->hwm_plot_name.c_str(), hwm);

        const auto allocs = static_cast<int64_t>(this->alloc_count.load(std::memory_order_acquire));
        TracyPlot(this->alloc_plot_name.c_str(), allocs);
    }
};
#endif

/**
 * @brief Port-specific heap implementation.
 *
 * @extends std::pmr::memory_resource
 *
 * @details
 * Owns a fixed-size memory pool, bump-allocates from it, and can be reset wholesale at well-defined
 * points.
 *
 * @details
 * Building with 'TRACY_ENABLE' will enable Tracy output for the number of allocations, bytes allocated,
 * and allocated bytes high watermark.
 *
 * @tparam CAPACITY The compile-time capacity of the heap in bytes.
 */
template<const size_t CAPACITY>
class PCHeap : public std::pmr::memory_resource {
    /**
     * The backing memory pool.
     *
     * @note The array is aligned to the build target's cache line size to help reduce cache misses.
     */
    alignas(std::hardware_destructive_interference_size) std::array<std::byte, CAPACITY> pool;

    /// The current cursor into the memory pool.
    std::atomic_uintptr_t cursor{reinterpret_cast<uintptr_t>(this->pool.data())};

    const char* heap_name;

    std::vector<void*> allocations;

#if TRACY_ENABLE
    std::unique_ptr<AllocStats> alloc_stats;
#endif
public:
    explicit PCHeap(const char* name = nullptr) : memory_resource(*std::pmr::null_memory_resource()), heap_name(name) {
#if TRACY_ENABLE
        this->alloc_stats = std::make_unique<AllocStats>(name, CAPACITY);
        TracyMessageL("PCHeap created");
#endif
    };

#if TRACY_ENABLE
    ~PCHeap() override {
        TracyMessageL("PCHeap destroyed");
    }
#else
    ~PCHeap() = default;
#endif

    PCHeap(const PCHeap&) = delete;

    PCHeap& operator=(const PCHeap&) = delete;

    /**
     * Reset the heap, freeing all allocated memory.
     */
    void reset() noexcept {
        this->cursor.store(reinterpret_cast<uintptr_t>(this->pool.data()), std::memory_order_release);

#if TRACY_ENABLE
        this->alloc_stats->reset_bytes_in_use();
        for (auto alloc : this->allocations) {
            TracyFreeN(alloc, this->heap_name);
        }
#endif
        this->allocations.clear();
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
        return static_cast<size_t>(cursor_val - reinterpret_cast<uintptr_t>(this->pool.data()));
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
        } catch (const std::bad_alloc& err) {
#if TRACY_ENABLE
            TracyLogString(tracy::MessageSeverity::Error, tracy::Color::Red, 0, err.what());
#endif
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
            if (next > this->pool.data() + CAPACITY) {
                // Not enough space.
                throw std::bad_alloc{};
            }
            if (!this->cursor.compare_exchange_strong(addr, reinterpret_cast<uintptr_t>(next), std::memory_order_release, std::memory_order_relaxed)) {
                continue;
            }
#if TRACY_ENABLE
            this->alloc_stats->on_alloc(bytes);
            TracyAllocN(aligned, bytes, this->heap_name);
#endif
            this->allocations.push_back(aligned);
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
     * @param ptr Pointer to align.
     * @param alignment Alignment to align to.
     * @return Aligned pointer.
     */
    static std::byte* align_up(const uintptr_t ptr, const std::size_t alignment) {
        const auto aligned = (ptr + (alignment - 1)) & ~(alignment - 1);
        return reinterpret_cast<std::byte*>(aligned);
    }
};
}  // namespace dusk::pc
