#include "dusk/audio/DuskAudioSystem.h"

#include <SDL3/SDL_init.h>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <span>
#include <thread>

#include "JSystem/JAudio2/JASAiCtrl.h"
#include "JSystem/JAudio2/JASChannel.h"
#include "JSystem/JAudio2/JASCriticalSection.h"
#include "JSystem/JAudio2/JASDSPChannel.h"
#include "JSystem/JAudio2/JASHeapCtrl.h"

#include "DuskDsp.hpp"
#include "JSystem/JAudio2/JASAudioThread.h"
#include "JSystem/JAudio2/JASDriverIF.h"
#include "dusk/os.h"
#include "tracy/Tracy.hpp"

using namespace dusk::audio;

static OutputSubframe OutBuffer;
static std::array<f32, DSP_SUBFRAME_SIZE * OutputSubframe::NUM_CHANNELS> OutInterleaveBuffer;

static SDL_AudioStream* PlaybackStream;

// Audio is rendered on a dedicated thread and pushed into PlaybackStream ahead of
// playback, instead of being rendered on demand from SDL's real-time device thread.
//
// Rendering takes gAudioThreadMutex (JASCriticalSection) for a whole frame, and the
// main game thread takes the same lock for most sound operations (Z2AudioCS, JASCmdStack,
// heap work, ...). When rendering happened inside the SDL callback, any main-thread hold
// at refill time blocked the device thread past its deadline and caused audible dropouts
// (most noticeably THP movie-audio crackle). With a queued cushion, a lock stall only
// delays the render thread, and the device keeps draining already-queued samples.
static std::thread RenderThread;
static std::atomic<bool> RenderThreadRunning{false};

/**
 * Number of full JAS audio frames to keep queued in PlaybackStream.
 * One frame is JASDriver::getSubFrames() subframes of DSP_SUBFRAME_SIZE samples
 * (7 * 80 = 560 samples = 17.5ms at 32kHz on GCN settings), so this cushion can
 * absorb lock stalls of up to ~one frame at the cost of one frame of latency.
 */
static constexpr u32 TARGET_QUEUED_FRAMES = 2;

/**
 * Render thread entry: keeps PlaybackStream topped up to TARGET_QUEUED_FRAMES.
 */
static void RenderThreadMain();

/**
 * Render an entire new frame of audio and output it to SDL3.
 * Note: "audio frames" are unrelated to video frames.
 * @return Amount of audio samples rendered.
 */
static int RenderNewAudioFrame();

/**
 * Render an audio subframe and output it to SDL3.
 */
static void RenderAudioSubframe();

static void InitSDL3Output() {
    SDL_Init(SDL_INIT_AUDIO);

    constexpr SDL_AudioSpec spec = {
        SDL_AUDIO_F32,
        2,
        SampleRate,
    };
    PlaybackStream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec,
        nullptr,
        nullptr);
}

void dusk::audio::Initialize() {
    InitSDL3Output();
    DspInit();

    JASDsp::initBuffer();
    JASDSPChannel::initAll();

    JASPoolAllocObject_MultiThreaded<JASChannel>::newMemPool(0x48);

    SDL_ResumeAudioStreamDevice(PlaybackStream);

    RenderThreadRunning.store(true, std::memory_order_relaxed);
    RenderThread = std::thread(RenderThreadMain);
}

void dusk::audio::Shutdown() {
    if (!RenderThreadRunning.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    if (RenderThread.joinable()) {
        RenderThread.join();
    }
    SDL_PauseAudioStreamDevice(PlaybackStream);
}

void dusk::audio::SetMasterVolume(const f32 value) {
    JASCriticalSection section;

    MasterVolume = value;
}

void dusk::audio::SetPaused(const bool paused) {
    if (paused) {
        SDL_PauseAudioStreamDevice(PlaybackStream);
    } else {
        SDL_ResumeAudioStreamDevice(PlaybackStream);
    }
}

void dusk::audio::SetEnableReverb(const bool value) {
    JASCriticalSection section;

    EnableReverb = value;
}

#ifdef TRACY_ENABLE
static auto FrameName = "RenderAudioFrame";
#endif

static void RenderThreadMain() {
    OSSetCurrentThreadName("dusk audio render");

    while (RenderThreadRunning.load(std::memory_order_relaxed)) {
        // getSubFrames() can change with the output mode, so compute the target each pass.
        const u32 targetBytes = TARGET_QUEUED_FRAMES * JASDriver::getSubFrames() *
                                DSP_SUBFRAME_SIZE * OutputSubframe::NUM_CHANNELS * sizeof(f32);

        if (static_cast<u32>(SDL_GetAudioStreamQueued(PlaybackStream)) < targetBytes) {
            FrameMarkStart(FrameName);
            RenderNewAudioFrame();
            FrameMarkEnd(FrameName);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

int RenderNewAudioFrame() {
    ZoneScoped;
    JASCriticalSection section;
    const u32 countSubframes = JASDriver::getSubFrames();

    JASAudioThread::setDSPSyncCount(countSubframes);

    for (u32 i = 0; i < countSubframes; i++) {
        RenderAudioSubframe();

        JASAudioThread::snIntCount -= 1;
    }

    return static_cast<u16>(countSubframes) * DSP_SUBFRAME_SIZE;
}

static void InterleaveOutputData(const OutputSubframe& data, std::span<f32> target) {
    assert(target.size() >= data.channels[0].size() * OutputSubframe::NUM_CHANNELS);

    size_t outPos = 0;
    for (size_t inPos = 0; inPos < data.channels[0].size(); inPos++) {
        for (size_t channelIdx = 0; channelIdx < OutputSubframe::NUM_CHANNELS; channelIdx++) {
            target[outPos++] = data.channels[channelIdx][inPos];
        }
    }
}

void RenderAudioSubframe() {
    ZoneScoped;
    OutBuffer = {};

    JASDriver::updateDSP();
    DspRender(OutBuffer);

    InterleaveOutputData(OutBuffer, OutInterleaveBuffer);

    if (JASDriver::extMixCallback != nullptr && JASDriver::sMixMode == MIX_MODE_INTERLEAVE) {
        static_assert(OutputSubframe::NUM_CHANNELS == 2); // This code only works with Stereo so far.
        // NOTE: In the real game, this gets called on the entire audio frame, rather than the subframe.
        // That's probably more efficient, but I didn't wanna change the code to calculate the
        // entire audio buffers at once.
        // This is only used for the movie player, and it seems to work fine with the smaller calls.
        const auto mixData = JASDriver::extMixCallback(DSP_SUBFRAME_SIZE);
        if (mixData) {
            for (int i = 0; i < OutInterleaveBuffer.size(); i++) {
                OutInterleaveBuffer[i] += static_cast<f32>(mixData[i]) / static_cast<f32>(0x7FFF);
            }
        }
    }

    SDL_PutAudioStreamData(PlaybackStream, &OutInterleaveBuffer, sizeof(OutInterleaveBuffer));
}

u32 dusk::audio::GetResetCount(int channelIdx) {
    return ChannelAux[channelIdx].resetCount;
}

f32 dusk::audio::VolumeFromU16(u16 value) {
    return static_cast<f32>(value) / static_cast<f32>(JASDriver::getChannelLevel_dsp());
}
