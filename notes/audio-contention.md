# The THP audio crackle: root cause, hotfix anatomy, and the mental models behind the rework

*Reference material for re-implementing the audio host by hand. File/line references are
against the `movie-audio-fix` branch head (hotfix applied), unless marked "pre-hotfix".
The target design itself lives in the implementation plan; this document is the "why".*

---

## 1. Root cause walkthrough

### The render path (pre-hotfix)

Before the hotfix, audio was produced *on demand, on SDL's audio device thread*:

- `SDL_OpenAudioDeviceStream` was given a `GetNewAudio` callback. SDL calls it from the
  device thread at the moment the OS audio device needs more samples.
- The callback called `RenderNewAudioFrame()`, which immediately takes
  `JASCriticalSection` — a RAII wrapper around **one global
  `std::recursive_mutex gAudioThreadMutex`** (`src/dusk/audio/JASCriticalSection.cpp:7`) —
  and holds it while rendering a full JAS audio frame: `JASDriver::getSubFrames()` (7 at
  32 kHz) subframes of `DSP_SUBFRAME_SIZE` (0x50 = 80) samples each, ≈ 17.5 ms of audio.
- Each subframe runs `JASDriver::updateDSP()` (command processing + 64 channel updates),
  `DspRender()`, interleaving, the movie-audio mix callback, then
  `SDL_PutAudioStreamData`.

### The other side of the lock

The **main game thread** takes the *same* mutex constantly. `mDoAud_Execute()` runs every
game frame and calls `Z2AudioMgr::gframeProcess()` (`src/m_Do/m_Do_audio.cpp:189`) — BGM
state machines, sound-effect spatialization, reverb updates — and every individual game
call like `seStart()`/`bgmStart()` also acquires `JASCriticalSection`. During bank loads
(area transitions), audio-heap operations hold it for much longer stretches.

### Why that combination crackles

An OS audio device is a **hard real-time consumer**. Roughly every N milliseconds it
needs the next buffer; if the samples aren't there, the OS inserts silence. A silence gap
in the middle of continuous audio is heard as a pop/crackle.

Pre-hotfix, the moment SDL asked for samples, delivering them required winning
`gAudioThreadMutex`. If the game thread happened to hold it right then, the device thread
*blocked*. The wait isn't bounded by how fast your CPU is — it's bounded by how long the
current holder keeps the lock plus OS scheduling latency to hand it over, which is
measured in **scheduler quanta (commonly ~1 ms or more)**, not in instructions. Meanwhile
the actual render work is only ~1–2 ms of CPU per 17.5 ms of audio.

That's why the crackle was consistent on an M4 Pro and a Ryzen 5800X: **it was never a
throughput problem.** A faster CPU shrinks render time, which was already tiny; it does
nothing to the lock-wait tail. Any main-thread hold that overlapped a device refill and
exceeded the device's slack produced an audible gap, on any hardware.

The general principle this violates is the first rule of real-time audio:
**the audio device thread must never block on a lock that a non-real-time thread can
hold for unbounded time.**

---

## 2. Why the THP movie pipeline amplified it

The THP player (`src/d/actor/d_a_movie_player.cpp`) made a marginal problem constant:

1. **Tiny, blocking buffer chain.** Decoded movie audio flows through a queue only
   **3 buffers deep** (`THP_AUDIO_BUFFER_COUNT`, `include/d/actor/d_a_movie_player.h:93`).
   The audio decode thread *blocks* on `daMP_PopFreeAudioBuffer()` until the mixer
   returns a fully-consumed buffer. So when the render side stalled on the mutex, it
   stopped returning free buffers → the decode thread parked → the 3-deep queue drained
   almost immediately.

2. **The silence-memset underrun path.** The movie mixer `daMP_MixAudio` pulls decoded
   audio with a *non-blocking* pop; when the queue is empty it memsets the output chunk
   to silence and returns (the pre-hotfix crackle you actually heard). At the current
   per-subframe call granularity, each miss is an ~2.5 ms silence gap.

3. **Video is slaved to the audio clock.** THP presents video frames against audio
   consumption progress (`curAudioNumber` advances only when the mixer pops a decoded
   buffer), and the *audio* decode thread is also the one that recycles shared read
   buffers back to the DVD reader thread (`d_a_movie_player.cpp` ~2996–3015). A stalled
   mixer therefore starves the **video** decoder two ways at once — which is why fixing
   the audio also fixed the video stutter, without touching any video code.

---

## 3. Anatomy of the hotfix (commit `8e43a20`), hunk by hunk

The hotfix's single idea: **move rendering off the device thread, and keep finished
audio queued ahead of playback so a lock stall delays production instead of playback.**

- **`src/dusk/audio/DuskAudioSystem.cpp` — the core.**
  - The stream callback was removed (`SDL_OpenAudioDeviceStream(..., nullptr, nullptr)`),
    switching the stream to *push mode*.
  - A dedicated thread ("dusk audio render") loops: if
    `SDL_GetAudioStreamQueued < TARGET_QUEUED_FRAMES (=2)` full JAS frames (~35 ms),
    render one frame (still under the whole-frame lock, unchanged); otherwise sleep 1 ms.
  - *Why 2 frames:* one frame (17.5 ms) is the render unit, so the queue oscillates
    between ~1 and ~2 frames; the ~17.5 ms floor comfortably exceeds typical lock-stall
    lengths. It was a deliberately conservative "make it stop crackling" number, not a
    tuned one.
  - *Known weaknesses, by design:* +17.5–35 ms output latency, and 1 ms sleep-polling
    (up to 1000 wakeups/s, and up to 1 ms of refill slop). Both are what the rework
    (get-callback wakeup + shrink-and-adapt target) addresses.

- **`include/dusk/audio/DuskAudioSystem.h` + `src/m_Do/m_Do_main.cpp` — `Shutdown()`.**
  A joinable `std::thread` must be joined before its object is destroyed, and the thread
  reads JAS/heap state — so it must stop **before** that state is torn down.
  `dusk::audio::Shutdown()` (flag → join → pause) is called in `game_main` teardown right
  after `MoviePlayerShutdown()` and before `mDoMch_Destroy()` / `OSResetSystem`. Teardown
  ordering *is* the design here; get it wrong and you trade a crackle for a
  shutdown crash.

- **`src/d/actor/d_a_movie_player.cpp` — temp instrumentation only.** Three blocks marked
  `>>> TEMP: THP movie-audio underrun instrumentation`: a Tracy include, a
  `ZoneScopedN("daMP_MixAudio")`, and an underrun counter + red timeline message exactly
  on the silence-memset path. Diagnostic, never meant to ship; the rework replaces it
  with a permanent device-underrun counter at the correct layer (the SDL edge, where
  audibility is actually decided).

- **`.gitignore` / `.vscode/launch.json` / `CLAUDE.md` — incidental conveniences**, not
  part of the fix.

---

## 4. The lock-contention mental model

Think of the system as three threads with very different deadline regimes:

| Thread | Deadline | May it block on `gAudioThreadMutex`? |
|---|---|---|
| SDL device thread | hard, ~ms | **never** |
| audio render thread | soft (queue depth) | yes — that's its job |
| main game thread | soft (frame rate) | yes (status quo) |

`gAudioThreadMutex` is a **coarse lock**: one mutex guards all 64 DSP channels, the
command queue, heaps, and render state. Coarse locking is *correct* and simple — it
faithfully expresses the decomp's frame-atomic model (console audio ran single-threaded
between DSP interrupts) — but it makes hold times unpredictable, which is fatal for
anyone with a hard deadline. The hotfix/rework don't make the lock finer; they **remove
every hard-deadline thread from the set of waiters**. The device thread's only remaining
interactions are lock-free (semaphore release, atomic bump, thread-safe stream ops).

The spectrum, for orientation:

```
callback renders under coarse lock   ←  the bug
render thread + queued depth         ←  hotfix / rework (device thread never waits)
render thread owns all state,
game talks via lock-free SPSC queue  ←  "full decoupling" (future work; hundreds of
                                         decomp call sites currently mutate state
                                         under the lock)
```

A second, subtler instance of the same disease: *every* `OSSendMessage`/
`OSReceiveMessage` in the whole game serializes on one global map mutex just to find its
queue's condition variables (`GetMsgQueueData`, `src/dusk/stubs.cpp:78`). It wasn't the
crackle's cause, but it's the same pattern one layer down — worth remembering if audio
contention ever resurfaces.

---

## 5. Queued depth ≡ latency (why "the cushion" can't be zero)

`SDL_AudioStream` does exactly what it promises: you push samples, the device consumes
them in order at the device rate. But the device **cannot wait**. At every pull, whatever
is in the stream's queue is all it can play; if the queue is empty, SDL emits silence.

So the "cushion" is not a mechanism added on top of SDL — it is simply the **fill level
of that same stream queue**, and it obeys one identity:

> queued depth (ms) = producer jitter you can survive = added output latency

- Depth ≈ 0 means "produce exactly when the device asks" — which is the callback-render
  architecture, i.e. the original bug. There is no free configuration where SDL "handles
  timing" and no queue depth exists; *something* must absorb the gap between the device's
  hard schedule and the producer's soft one.
- Therefore the design goal is not "eliminate the cushion" but "sit at the minimum depth
  this machine actually needs" — hence: start at hotfix parity (2 frames), add
  observability, then default to 1 frame and grow only on a *measured* device underrun.

---

## 6. Sample format: where s16 lives, and why the mix bus is f32

JAS is genuinely a 32 kHz / s16 engine **at its edges**:

- **Sources** decode to s16: PCM16 and ADPCM decode paths produce s16 buffers
  (`DecodeSample` and friends, `src/dusk/audio/DuskDsp.cpp` ~419–504).
- **Console output** was s16: the DAC DMA buffers are `s16*`
  (`JASDriver::sDmaDacBuffer`, `libs/JSystem/src/JAudio2/JASAiCtrl.cpp:23`), and the
  console's fixed-point mix saturated (clamped) at s16 range.

But Dusklight's DSP reimplementation promotes to float **exactly once**, at channel
load — the resampler emits `(prev + pos*(next-prev)) / 32768.0f`
(`src/dusk/audio/DuskDsp.cpp:693`) — and every downstream stage already runs in f32:
per-channel IIR filters, 64-channel summing, freeverb reverb, and the ramped
master-volume multiply.

Why float is the right bus and output format:

- **Headroom.** Summing 64 channels + movie mix + reverb in s16 would need intermediate
  clamping at every stage; float sums losslessly and clamps once at the end.
- **The OS wants float anyway.** CoreAudio, WASAPI, and PipeWire mix in f32 internally.
  Outputting s16 would quantize our float mix only for the OS to convert it straight
  back — a pure loss.
- **The s16-ness that matters is already honored** at the source-decode boundary, which
  is where the original data's precision lives.

One faithfulness nuance: because the console clamped at s16 saturation, a heavily
overdriven console mix *saturated*; the current float path can sum past ±1.0 and gets
clamped later at SDL's conversion instead. An explicit final clamp on the interleave
buffer reproduces the console's saturation point exactly, cheaply, and at a defined
place — that's the optional clamp in the rework design.
