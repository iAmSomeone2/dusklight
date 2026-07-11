#include "dusk/audio/DuskAudioSystem.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_mutex.h>
#include <array>
#include <cassert>
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
#include "dusk/logging.h"
#include "tracy/Tracy.hpp"

using namespace dusk::audio;

static OutputSubframe OutBuffer;
static std::array<f32, DSP_SUBFRAME_SIZE * OutputSubframe::NUM_CHANNELS> OutInterleaveBuffer;

static SDL_AudioStream* PlaybackStream;

/// Number of audio frames that should be queued in SDL3's buffer. The more frames queued, the less likely audio underruns will occur, but the more latency there will be between the game and the audio output.
constexpr size_t TARGET_QUEUED_FRAMES = 2;

/// Background thread that renders audio frames and outputs them to SDL3. This is used to avoid blocking the main thread when rendering audio, which can cause stuttering if the main thread is busy with other work.
static std::thread AudioRenderThread;
static std::atomic_bool AudioRenderThreadRunning = true;
static SDL_Semaphore* AudioRenderThreadSemaphore = nullptr;


/**
 * Render an entire new frame of audio and output it to SDL3.
 * Note: "audio frames" are unrelated to video frames.
 */
static void RenderNewAudioFrame();

/**
 * Render an audio subframe and output it to SDL3.
 */
static void RenderAudioSubframe();

/**
 * SDL audiostream worker thread for rendering new audio data.
 */
void RenderAudioWorker() {
    // This code currently assumes that the value of sSubFrames will not change during the lifetime of the program. If that ever changes, this code will need to be updated to handle it.
    const auto target_subframes = JASDriver::sSubFrames * TARGET_QUEUED_FRAMES;
    const auto target_queued_bytes = target_subframes * DSP_SUBFRAME_SIZE * OutputSubframe::NUM_CHANNELS * sizeof(f32);

    /// Gets the number of bytes currently queued in SDL3's audio buffer. Returns SIZE_MAX on error.
    auto get_queued_bytes = [](size_t &failure_count) -> size_t {
        const auto queued_bytes = SDL_GetAudioStreamQueued(PlaybackStream);
        if (queued_bytes < 0) {
            failure_count += 1;
            DuskLog.warn("SDL_GetAudioStreamQueued failed: {}", SDL_GetError());
            return SIZE_MAX;
        }
        failure_count = 0;
        return static_cast<size_t>(queued_bytes);
    };

    size_t failure_count = 0;
    while (AudioRenderThreadRunning.load(std::memory_order_acquire)) {
        SDL_WaitSemaphoreTimeout(AudioRenderThreadSemaphore, 1);
        auto queued_bytes = get_queued_bytes(failure_count);
        if (failure_count > 10) {
            DuskLog.error("SDL_GetAudioStreamQueued failed too many times! Audio thread exiting...");
            AudioRenderThreadRunning.store(false, std::memory_order_release);
            break;
        }

        // We don't need an explicit check for SIZE_MAX here, since target_queued_bytes will never be greater than SIZE_MAX.
        while(queued_bytes < target_queued_bytes) {
            RenderNewAudioFrame();
            queued_bytes = get_queued_bytes(failure_count);
        }
    }
}

static void InitSDL3Output() {
    SDL_Init(SDL_INIT_AUDIO);

    constexpr SDL_AudioSpec spec = {
        SDL_AUDIO_F32,
        2,
        SampleRate,
    };

    AudioRenderThreadSemaphore = SDL_CreateSemaphore(1);

    PlaybackStream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec,
        [](void*, SDL_AudioStream*, int, int) { SDL_SignalSemaphore(AudioRenderThreadSemaphore); },
        nullptr);
}

void dusk::audio::Initialize() {
    InitSDL3Output();
    DspInit();

    JASDsp::initBuffer();
    JASDSPChannel::initAll();

    JASPoolAllocObject_MultiThreaded<JASChannel>::newMemPool(0x48);

    AudioRenderThread = std::thread(RenderAudioWorker);

    SDL_ResumeAudioStreamDevice(PlaybackStream);
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

void RenderNewAudioFrame() {
    ZoneScoped;
    JASCriticalSection section;
    const u32 countSubframes = JASDriver::getSubFrames();

    JASAudioThread::setDSPSyncCount(countSubframes);

    for (u32 i = 0; i < countSubframes; i++) {
        RenderAudioSubframe();

        JASAudioThread::snIntCount -= 1;
    }
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

void dusk::audio::Shutdown() {
    AudioRenderThreadRunning.store(false, std::memory_order_release);
    SDL_SignalSemaphore(AudioRenderThreadSemaphore);
    SDL_PauseAudioStreamDevice(PlaybackStream);

    if (AudioRenderThread.joinable()) {
        AudioRenderThread.join();
    }
    
    SDL_DestroyAudioStream(PlaybackStream);
    PlaybackStream = nullptr;

    SDL_DestroySemaphore(AudioRenderThreadSemaphore);
    AudioRenderThreadSemaphore = nullptr;
}
