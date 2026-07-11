# OSSideTable alignment fix — handle-based slot (design sketch)

> **AI-authored note.** This file was written by Claude (an AI assistant) as a design
> sketch for Brenden to review and implement. It is illustrative pseudo-C++, not
> finished code, and has not been compiled. Brenden writes the actual implementation.

## Problem recap

The committed `OSSideTable` caches an 8-byte `PCData*` inside the dead
`queueSend.head` / `queue.head` field and accesses it via `std::atomic_ref<void*>`,
whose `required_alignment` is 8.

`std::atomic_ref<void*>` asserts (debug) / is UB (release) when the referenced
address is not 8-aligned. It is not, because the game **deliberately** allocates
many of the enclosing objects on 4-byte boundaries:

- `libs/JSystem/src/JKernel/JKRArchivePub.cpp` — both `JKRArchive::mount` overloads
  call `JKR_NEW_ARGS(heap, alignment)` with `alignment = 4` (`-4` for tail-mount).
  The `JKRDvdFile` / thread objects owned by a mount (which embed the `OSMessageQueue`
  / `OSMutex`) inherit that 4-byte placement.

The failing address is the object's address (the slot field is at offset 0), so:

- **This is not fixable at compile time.** The field's *type* already has
  `alignof == 8`; a `static_assert` passes. What is misaligned is the *runtime
  placement* of a specific object, which `OSSideTable` does not control.
- **It is systematic, not one-off** — every archive-mounted queue/mutex is affected,
  and those are exactly the archive-load contention hot path the rework targets.
  So "align-check then fall back to the map" alone would route the hot path back
  through the lock and defeat the purpose.
- Worst-case observed alignment is `4-mod-8` (mount only ever passes `±4`; static
  instances stay 8-aligned). Never odd/2-aligned. So **4-byte alignment is a safe
  floor.**

## The fix: store a 4-byte handle, not an 8-byte pointer

`alignof(uint32_t) == 4`, satisfied by a `4-mod-8` address. Both desktop (x86-64)
and the Vita target are little-endian, so the low 4 bytes of the 8-byte dead field
are the first 4 bytes at offset 0 — no endianness trap.

- **Slot** becomes `uint32_t&` (reinterpret the dead field). `0` stays the
  "unassigned" sentinel that matches the field `OSInit*` nulls, so handles are
  **1-based**. Upper 4 bytes of the field are left as-is (zero).
- A handle indexes a registry that maps `handle -> PCData*`.
- One template change (`void*` slot -> `uint32_t` slot) fixes all three tables
  (`OSMessageQueue`, `OSMutex`, `OSCond`) at once.

### Why not a plain `std::deque`/`std::vector` registry

The fast path resolves `handle -> PCData*` **without the map lock**, concurrently
with another thread appending a new entry under the lock. `deque::operator[]` /
`vector::operator[]` read container metadata (chunk map / data pointer + would-be
realloc) that a concurrent `push_back` mutates → data race / UB.

The registry must therefore resolve a handle by touching **only write-once state**.
A single-level **paged atomic table** does that: the directory of page pointers and
each page slot are each published once with a release store and read with an acquire
load; nothing an in-flight reader touches is ever mutated after publication.

## Sketch

### 1. Lock-free-read paged registry (write-once slots)

```cpp
// Resolves a 1-based handle -> PCData* with a lock-free acquire load.
// Appends happen under the OSSideTable map mutex (rare: first touch of an object).
// PCData is intentionally never freed (matches the plan's shutdown UAF-avoidance),
// so raw ownership + process-exit leak is fine.
template <typename PCData>
class PCDataRegistry {
    static constexpr uint32_t kPageBits = 9;              // 512 entries / page
    static constexpr uint32_t kPageSize = 1u << kPageBits;
    static constexpr uint32_t kPageMask = kPageSize - 1;
    static constexpr uint32_t kMaxPages = 4096;           // ~2.1M handles ceiling

    struct Page { std::atomic<PCData*> entries[kPageSize]; };

    std::atomic<Page*> mPages[kMaxPages] = {};            // directory, entries write-once
    // mCount is only ever mutated under the caller's map mutex, so a plain integer
    // is fine; expose it so a handle exceeding it can be treated as "not yet visible".

public:
    // Caller holds the map mutex. Returns a 1-based handle.
    uint32_t add(uint32_t& outCount, PCData* data) {
        const uint32_t idx = outCount;                    // 0-based slot
        const uint32_t page = idx >> kPageBits;
        JUT_ASSERT(page < kMaxPages);                     // no silent cap — assert loudly
        Page* p = mPages[page].load(std::memory_order_relaxed);
        if (!p) {
            p = new Page();                               // leaked at exit, like PCData
            mPages[page].store(p, std::memory_order_release);
        }
        p->entries[idx & kPageMask].store(data, std::memory_order_release);
        outCount = idx + 1;
        return idx + 1;                                   // 1-based handle
    }

    // Lock-free. `handle` must be a value previously returned by add().
    PCData* resolve(uint32_t handle) const {
        const uint32_t idx = handle - 1;
        Page* p = mPages[idx >> kPageBits].load(std::memory_order_acquire);
        return p->entries[idx & kPageMask].load(std::memory_order_acquire);
    }
};
```

> Decision point: `mCount` is passed in/out here to keep the registry stateless about
> counting. It's cleaner to just make `mCount` a private member of `PCDataRegistry`
> guarded by the same map mutex. Shown split only to make the "mutated under lock"
> boundary explicit — collapse it when you write the real thing.

### 2. OSSideTable using the registry

```cpp
template <typename GCType, typename PCData>
class OSSideTable {
public:
    static uint32_t& slot(GCType* obj);                  // specialized per type, below

    static PCData* get(GCType* obj) {
        const std::atomic_ref<uint32_t> cached(slot(obj));   // 4-byte aligned: OK
        if (uint32_t h = cached.load(std::memory_order_acquire); h != 0) {
            return registry().resolve(h);
        }
        return slowPath(obj);
    }

    template <typename Fn>
    static void forEach(Fn&& fn) {
        std::lock_guard lock(mapMutex());
        for (auto& [gc, handle] : index()) {
            fn(*registry().resolve(handle));
        }
    }

private:
    static PCData* slowPath(GCType* obj) {
        std::lock_guard lock(mapMutex());
        const std::atomic_ref<uint32_t> cached(slot(obj));

        // Another thread may have populated the slot before we took the lock.
        if (uint32_t h = cached.load(std::memory_order_acquire); h != 0) {
            return registry().resolve(h);
        }
        // Re-init of the same object nulls its slot but the map still knows it,
        // so reuse the existing PCData instead of leaking a fresh one (plan intent).
        if (auto it = index().find(obj); it != index().end()) {
            cached.store(it->second, std::memory_order_release);
            return registry().resolve(it->second);
        }
        PCData* data = new PCData();                      // leaked at exit (plan)
        uint32_t h = registry().add(count(), data);
        index().emplace(obj, h);
        cached.store(h, std::memory_order_release);       // publish last
        return data;
    }

    // Function-local statics: lazy-init, DLL static-init-order safe.
    static PCDataRegistry<PCData>& registry() { static PCDataRegistry<PCData> r; return r; }
    static std::unordered_map<GCType*, uint32_t>& index() {
        static std::unordered_map<GCType*, uint32_t> m; return m;
    }
    static uint32_t& count() { static uint32_t c = 0; return c; }
    static std::mutex& mapMutex() { static std::mutex m; return m; }
};
```

### 3. Slot specializations (only the type changes: `void*&` -> `uint32_t&`)

```cpp
struct PCMessageQueueData;
template <>
inline uint32_t& OSSideTable<OSMessageQueue, PCMessageQueueData>::slot(OSMessageQueue* obj) {
    return reinterpret_cast<uint32_t&>(obj->queueSend.head);   // low 4 bytes @ offset 0
}

struct PCMutexData;
template <>
inline uint32_t& OSSideTable<OSMutex, PCMutexData>::slot(OSMutex* obj) {
    return reinterpret_cast<uint32_t&>(obj->queue.head);
}

struct PCCondData;
template <>
inline uint32_t& OSSideTable<OSCond, PCCondData>::slot(OSCond* obj) {
    return reinterpret_cast<uint32_t&>(obj->queue.head);
}
```

## Things to double-check when implementing

- **1-based handles / sentinel.** `0` must remain "unassigned." Confirm every code path
  that writes a handle writes `idx + 1`, and that `OSInit*` still zeroes the field
  *before* `get()` runs (already handled — struct fields nulled first, slot published
  last).
- **`resolve()` upper bound.** `resolve` trusts the handle. If a caller could ever pass
  a handle from a *different* type's table (it shouldn't — tables are per-type statics),
  it would index garbage. Keep the tables strictly per-`(GCType,PCData)`.
- **`count()` mutation is lock-only.** It is read/written solely inside `slowPath`
  under `mapMutex`. The lock-free `resolve` never reads `count()` — it trusts the
  handle it was given, which was published *after* its registry slot.
- **`forEach` still under the lock** for shutdown wake-all; it must **not** destroy
  `PCData` (plan's UAF-avoidance — threads may still be inside `wait()`).
- **Assert, don't truncate.** The `kMaxPages` ceiling must assert loudly if hit, not
  silently drop entries. Size it well above the realistic per-session object count
  (archive mounts over a long session), or make the directory itself growable if that
  ceiling is ever a real concern.
- **Vita build.** Re-run the 32-bit config after this change: `void*` is 4 bytes there,
  but so is `uint32_t`, and the handle scheme is unaffected. The point is to confirm the
  misaligned-atomic UB is actually gone on the LE/ARM target, not just silenced on x86.

## Open question to settle first

Is the `unordered_map<GCType*, uint32_t>` still worth keeping, or can the registry be
the single source of truth? It earns its place only for **re-init dedup** (same object
`OSInit`'d twice reuses its `PCData`). If re-init-with-reuse isn't a real scenario for
these queues/mutexes, the map can be dropped and `forEach` can iterate the registry
directly up to `count()`. Decide before writing — it changes both `slowPath` and
`forEach`.
