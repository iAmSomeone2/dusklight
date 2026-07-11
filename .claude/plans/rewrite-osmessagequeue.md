# OSMessageQueue / OSMutex / OSCond side-table rework — design doc

**Authorship rule:** per the standing project constraint, **Brenden writes all code**. This document is the design deliverable; Claude reviews the implementation afterward.

## Context

The PC layer reimplements the GameCube `OSMessageQueue` API in [src/dusk/stubs.cpp](src/dusk/stubs.cpp:58) and `OSMutex`/`OSCond` in [src/dusk/OSMutex.cpp](src/dusk/OSMutex.cpp) using a side-table: a global `std::unordered_map<GCStruct*, unique_ptr<PCData>>` guarded by a **single global mutex**, consulted via `GetMsgQueueData()` / `GetMutexData()` / `GetCondData()` on **every operation**.

That means every send/receive on any queue — and every lock/unlock of any mutex — serializes all threads on one global lock plus a hash lookup, even for unrelated objects. The hot parties: THP movie player's read/video-decode/audio-decode threads (7 queues, per-frame while a movie plays), the JAS audio streaming thread (per-audio-callback), JKR decompression/ARAM/DVD threads, and the main thread. This is the follow-up flagged during the audio-contention work (memory: `movie-audio-fix-branch`).

Goals in priority order: **(1) eliminate the cross-object lock contention, (2) improve readability, (3) optimize for modern CPUs.**

## Key facts established by exploration

- `OSMessageQueue` (libs/dolphin/include/dolphin/os/OSMessage.h:15) begins with two `OSThreadQueue` fields (`queueSend`, `queueReceive`), each `{OSThread* head; OSThread* tail;}` — **four pointer-sized slots that are dead on PC** (the dusk impl never sleeps/wakes threads on them; `OSInitMessageQueue` just nulls them).
- `OSMutex` has a dead `queue` field; `OSCond` is *only* a dead `queue` field. Same opportunity.
- Game code never touches these fields: the only direct field access outside the dusk layer is a read-only ImGui debug read of `.usedCount` (d_a_movie_player.cpp:4416).
- All queue/mutex/cond objects are static or embedded in long-lived objects — **never freed while in use**.
- `dusk::IsShuttingDown` is a **plain `bool`** (include/dusk/main.h:9, defined m_Do_main.cpp:125) read inside cv wait predicates from multiple threads → a real data race (UB).
- Shutdown teardown (`ClearMsgQueueMap`/`ClearCondMap`, called from `OSResetSystem`) `notify_all()`s then **destroys** the PCData while other threads may still be blocked inside `wait()` on those very objects → use-after-free window.
- C++20 is available (`CMAKE_CXX_STANDARD 20`) → `std::atomic_ref`, `std::hardware_destructive_interference_size`.
- `OSJamMessage` has no callers but stays implemented (API completeness).

## Design: cache the PCData pointer inside the GC struct

Replace the per-operation map lookup with a pointer stashed in the dead `OSThreadQueue` slot of each struct. The hot path becomes: one atomic pointer load → operate on that object's own mutex/cv. **Zero shared state between distinct objects; the global map disappears from the hot path entirely.**

### Slot assignment

| GC struct        | Slot for `PCData*`        |
|------------------|---------------------------|
| `OSMessageQueue` | `mq->queueSend.head`      |
| `OSMutex`        | `mutex->queue.head`       |
| `OSCond`         | `cond->queue.head`        |

Add a `static_assert(sizeof(OSThread*) == sizeof(void*))` for documentation. `queueSend.tail` etc. stay null.

### Shared helper (readability goal)

The three side-tables are the same pattern; implement it once in a new header, e.g. `src/dusk/os_sidetable.h`:

```
template <typename GCType, typename PCData>
class OSSideTable {
    // slot(GCType*) -> void*&  : reference to the dead pointer field
    // get(GCType* obj):
    //     void* cached = std::atomic_ref(slot(obj)).load(std::memory_order_acquire);
    //     if (cached) return *static_cast<PCData*>(cached);
    //     return slowPath(obj);   // global map, publishes into the slot
    // slowPath: lock map mutex; find-or-emplace; store raw ptr into slot
    //           with atomic_ref release store; return it.
    // forEach(fn): lock map mutex, iterate (for shutdown notify)
};
```

Details:
- The **map stays**, but only as the *owner* of the `unique_ptr<PCData>` allocations and as the slow-path fallback for objects used without `OSInit*` (the current lazy-init tolerance is preserved). It is touched only on first use of each object and at shutdown.
- Publish/read of the slot uses `std::atomic_ref<void*>` (release store in slow path under the map mutex, acquire load on the fast path) so a lazily-created entry is safe even if two threads race first use. The double-checked pattern is race-free because creation happens exactly once under the map mutex.
- Keep the existing lazy-static idiom (`GetMap()`, `GetMapMutex()` as function-local statics) for the DLL static-init reason already documented.
- `OSInit*` functions eagerly create the entry (as today) so steady-state never hits the slow path. **Caveat to preserve:** `OSInitMessageQueue` nulls `queueSend.head` *before* installing the cached pointer — order the writes so the slot ends up populated (write struct fields first, install pointer last). Re-init of an already-initialized object should reuse the existing entry (map find hits), not leak a new one.

### PCData layout (modern-CPU goal)

```
struct alignas(std::hardware_destructive_interference_size) PCMessageQueueData {
    std::mutex mtx;
    std::condition_variable cvSend;
    std::condition_variable cvReceive;
};
```

- `alignas(64)` (via interference-size constant) prevents false sharing between separately allocated PCData objects and neighbors in the heap. Same treatment for `PCMutexData` / `PCCondData`.
- Optional micro-opts, apply only if they don't hurt readability: replace `% msgCount` with an `if (idx >= msgCount) idx -= msgCount;` wrap (avoids integer division; `msgCount` isn't a power of two in general), and move `notify_one()` after unlocking (drop the lock via scope, then notify) to avoid waking a thread that immediately blocks on the still-held mutex.

### Fix the `IsShuttingDown` data race

Change `dusk::IsShuttingDown` to `std::atomic<bool>` (include/dusk/main.h + m_Do_main.cpp definition). Reads in wait predicates / OSThread.cpp:45 keep working syntactically (`if (dusk::IsShuttingDown)` still compiles); grep for any use that takes its address or binds a `bool&`.

### Fix the shutdown use-after-free

In `ClearMsgQueueMap` / `ClearCondMap` (and `OSResetSystem` at stubs.cpp:202):
1. `IsShuttingDown.store(true)` **first** (already the order today).
2. Notify all cvs (as today) — do this by iterating the map under the map mutex.
3. **Do not `map.clear()`.** Destroying `PCData` while another thread is inside `wait()`/`lock()` on it is UB. The objects are static and the process is exiting; intentionally leaking the side-table entries at shutdown is the correct, boring answer. Rename the functions to reflect intent (e.g. `WakeAllMsgQueueWaiters()` / `WakeAllCondWaiters()`).

### Function-by-function changes

**stubs.cpp** — `OSInitMessageQueue`, `OSSendMessage`, `OSReceiveMessage`, `OSJamMessage`: swap `GetMsgQueueData(mq)` for the side-table `get()`; logic inside the lock is unchanged (predicates already handle spurious wakes and shutdown). Keep `OSJamMessage` (no callers, but API-complete).

**OSMutex.cpp** — `OSInitMutex`, `OSLockMutex`, `OSUnlockMutex`, `OSTryLockMutex`, `OSInitCond`, `OSWaitCond`, `OSSignalCond`: same swap. `OSWaitCond` touches *two* side-tables (cond + mutex); both become single-atomic-load fast paths. No change to its recursion-level save/restore logic.

**Files touched:**
- `src/dusk/os_sidetable.h` (new)
- [src/dusk/stubs.cpp](src/dusk/stubs.cpp) (message queue section + `OSResetSystem`)
- [src/dusk/OSMutex.cpp](src/dusk/OSMutex.cpp)
- [include/dusk/main.h](include/dusk/main.h) + `src/m_Do/m_Do_main.cpp` (atomic `IsShuttingDown`)

### Explicitly out of scope / rejected

- **Lock-free MPMC ring buffer** for the queue itself: rejected. Contention is *cross-queue* (the global map lock), not intra-queue; per-queue mutexes are uncontended in practice (distinct producer/consumer pairs). A lock-free ring plus blocking semantics would hurt readability (goal 2) for no measured win.
- Sharded map: strictly worse than the inline pointer (still hashes, still shares locks).
- Removing the GC struct field mirroring (`usedCount`, `mutex->thread/count`): kept — game code and the ImGui overlay read them.

## Verification

1. Build (both a normal config and, if convenient, MSVC/Windows to confirm no `atomic_ref` portability surprises).
2. **Functional:** boot the game; play a THP movie (heaviest multi-queue user — read/video/audio threads) and confirm video+audio play cleanly; exercise archive loads (DVD thread single-slot queue sync); quit via the normal path and confirm no hang and no crash in `OSResetSystem` teardown (worker threads blocked in `OSReceiveMessage` must wake and exit).
3. **Contention (before/after):** Tracy capture during movie playback using the protocol-78 frontend at `~/tools/tracy-6789e7.../build/tracy-profiler`. Optionally wrap the map mutex acquisition in a `TracyLockable`/zone temporarily to visualize the before-state; after the rework the global lock should appear only at init.
4. **TSan pass** if a sanitizer config exists (validates the `IsShuttingDown` fix and the atomic_ref publish).
5. ImGui movie-player debug overlay still shows sane `usedCount` values.
