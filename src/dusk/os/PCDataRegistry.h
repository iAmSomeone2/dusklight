//
// Created by Brenden Davidson on 7/13/26.
//

#pragma once

#include <atomic>
#include <cstdint>

/**
 * The backing registry for contention-minimized management of PCData entries
 *
 * @tparam PCData porting code-specific data
 */
template <typename PCData>
class PCDataRegistry {
    /// Number of bits allocated to the page index
    static constexpr uint32_t kPageBits = 9;
    /// Number of entries in a page
    static constexpr uint32_t kPageSize = 1u << kPageBits;
    /// Mask to extract an entry's index from a page index
    static constexpr uint32_t kPageMask = kPageSize - 1;

    /// Maximum number of pages in the registry
    ///
    /// @note
    /// The current value of 4096 pages allows for nearly 2.1 million entries while using ~32KiB on
    /// a 64-bit system (~16KiB on 32-bit) for bookkeeping. This should be plenty for TP, but it can
    /// always be increased if needed.
    static constexpr uint32_t kMaxPages = 4096;

    struct Page {
        std::atomic<PCData*> entries[kPageSize];
    };

    std::atomic<Page*> mPages[kMaxPages] = {};
    uint32_t mEntryCount = 0;

public:
    /// Add a new PCData entry to the registry, returning its 1-based handle on success.
    ///
    /// @param data The PCData entry to add
    /// @return The 1-based handle of the added entry
    uint32_t add(PCData* data) {
        const auto idx = this->mEntryCount;
        const auto pageIdx = idx >> kPageBits;
        if (pageIdx >= kMaxPages) {
            // Panic if we've exceeded the maximum number of pages
            abort();
        }

        Page* page = this->mPages[pageIdx].load(std::memory_order_relaxed);
        if (page == nullptr) {
            page = new Page();
            this->mPages[pageIdx].store(page, std::memory_order_release);
        }

        page->entries[idx & kPageMask].store(data, std::memory_order_release);
        this->mEntryCount = idx + 1;

        return idx + 1;
    }

    /// Resolve a handle to a PCData entry without using a lock, only atomic loads.
    ///
    /// @param handle The 1-based handle of the entry to resolve
    /// @return The PCData entry, or nullptr if the handle is invalid
    PCData* resolve(const uint32_t handle) const {
        const uint32_t index = handle - 1;

        Page* page = this->mPages[index >> kPageBits].load(std::memory_order_acquire);
        if (page == nullptr) {
            return nullptr;
        }

        return page->entries[index & kPageMask].load(std::memory_order_acquire);
    }
};
