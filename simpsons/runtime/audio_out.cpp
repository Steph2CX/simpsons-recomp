// Sortie son : les images audio du jeu (256 echantillons, 6 canaux 5.1 en flottants big-endian,
// un bloc de 256 par canal) sont mixees en stereo et jouees avec AudioQueue (macOS) ou SDL (Windows, Linux).
#include "runtime.h"

#ifndef __APPLE__
#include <SDL3/SDL.h>
#else
#include <AudioToolbox/AudioToolbox.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

constexpr uint32_t FRAME_SAMPLES = 256;
constexpr uint32_t FRAME_CHANNELS = 6;
constexpr uint32_t QUEUE_BUFFERS = 4;
constexpr size_t MAX_BUFFERED_SAMPLES = 48000 / 4; // drop audio rather than drift more than 250 ms

static std::mutex g_outputMutex;
static std::deque<float> g_stereo; // interleaved L, R

#ifndef __APPLE__
static SDL_AudioStream* g_stream = nullptr;
// Jitter buffer: the game mixes 5.3 ms frames on a timer thread that falls behind when the PC is busy, while SDL
// pulls at its own pace. Playing whatever is there gave crackles; after running dry, wait for 40 ms of sound again.
constexpr size_t START_SAMPLES = 48000 / 25;
static bool g_refilling = true;

// SDL asks for more samples from its audio thread
static void FillStream(void*, SDL_AudioStream* stream, int additional, int)
{
    float out[FRAME_SAMPLES * 2 * 4];
    while (additional > 0)
    {
        uint32_t count = uint32_t(std::min<size_t>(sizeof(out), size_t(additional)) / sizeof(float));
        {
            std::lock_guard lock(g_outputMutex);
            if (g_refilling && g_stereo.size() >= START_SAMPLES * 2)
                g_refilling = false;
            uint32_t available = g_refilling ? 0 : uint32_t(std::min<size_t>(count, g_stereo.size()));
            if (!g_refilling && available < count)
                g_refilling = true;
            std::copy_n(g_stereo.begin(), available, out);
            g_stereo.erase(g_stereo.begin(), g_stereo.begin() + available);
            std::fill(out + available, out + count, 0.0f);
        }
        SDL_PutAudioStreamData(stream, out, int(count * sizeof(float)));
        additional -= int(count * sizeof(float));
    }
}

static bool StartOutput()
{
    if (getenv("SIMPSONS_MUET") != nullptr)
        return false;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, 48000 };
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, FillStream, nullptr);
    if (g_stream == nullptr)
    {
        fprintf(stderr, "[audio] impossible d'ouvrir la sortie son : %s\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    fprintf(stderr, "[audio] sortie son demarree (48 kHz stereo)\n");
    return true;
}
#else
static AudioQueueRef g_queue = nullptr;

static void FillBuffer(void*, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    auto* out = static_cast<float*>(buffer->mAudioData);
    uint32_t count = buffer->mAudioDataBytesCapacity / sizeof(float);
    {
        std::lock_guard lock(g_outputMutex);
        uint32_t available = uint32_t(std::min<size_t>(count, g_stereo.size()));
        std::copy_n(g_stereo.begin(), available, out);
        g_stereo.erase(g_stereo.begin(), g_stereo.begin() + available);
        std::fill(out + available, out + count, 0.0f);
    }
    buffer->mAudioDataByteSize = count * sizeof(float);
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

static bool StartOutput()
{
    if (getenv("SIMPSONS_MUET") != nullptr)
        return false;

    AudioStreamBasicDescription format{};
    format.mSampleRate = 48000;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 8;
    format.mBytesPerPacket = 8;

    if (AudioQueueNewOutput(&format, FillBuffer, nullptr, nullptr, nullptr, 0, &g_queue) != noErr)
    {
        fprintf(stderr, "[audio] impossible d'ouvrir la sortie son\n");
        return false;
    }
    for (uint32_t i = 0; i < QUEUE_BUFFERS; i++)
    {
        AudioQueueBufferRef buffer;
        AudioQueueAllocateBuffer(g_queue, FRAME_SAMPLES * 4 * 8, &buffer);
        FillBuffer(nullptr, g_queue, buffer);
    }
    AudioQueueStart(g_queue, nullptr);
    fprintf(stderr, "[audio] sortie son demarree (48 kHz stereo)\n");
    return true;
}
#endif

// Called for every frame submitted by the game
void SubmitAudioFrame(uint32_t samples)
{
    static bool started = StartOutput();

    float channel[FRAME_CHANNELS][FRAME_SAMPLES];
    for (uint32_t ch = 0; ch < FRAME_CHANNELS; ch++)
    {
        for (uint32_t i = 0; i < FRAME_SAMPLES; i++)
        {
            uint32_t bits = LoadBE32(samples + (ch * FRAME_SAMPLES + i) * 4);
            memcpy(&channel[ch][i], &bits, 4);
        }
    }

    // SIMPSONS_TRACE_AUDIO: once per second, the peak level of each channel
    static const bool trace = getenv("SIMPSONS_TRACE_AUDIO") != nullptr;
    static uint32_t frames = 0;
    static float peak[FRAME_CHANNELS] = {};
    if (trace)
    {
        for (uint32_t ch = 0; ch < FRAME_CHANNELS; ch++)
            for (uint32_t i = 0; i < FRAME_SAMPLES; i++)
                peak[ch] = std::max(peak[ch], std::abs(channel[ch][i]));
        if (++frames % 188 == 0)
        {
            fprintf(stderr, "[audio] crete par canal :");
            for (uint32_t ch = 0; ch < FRAME_CHANNELS; ch++)
            {
                fprintf(stderr, " %.3f", peak[ch]);
                peak[ch] = 0;
            }
            fprintf(stderr, "\n");
        }
    }

    // SIMPSONS_ENREGISTRE=<file>: stereo mix written as raw 48 kHz float32 (ffmpeg -f f32le -ar 48000 -ac 2)
    static FILE* record = [] {
        const char* path = getenv("SIMPSONS_ENREGISTRE");
        return path ? fopen(path, "wb") : nullptr;
    }();
    if (record)
    {
        float mix[FRAME_SAMPLES * 2];
        for (uint32_t i = 0; i < FRAME_SAMPLES; i++)
        {
            mix[i * 2] = channel[0][i] + 0.707f * (channel[2][i] + channel[4][i]);
            mix[i * 2 + 1] = channel[1][i] + 0.707f * (channel[2][i] + channel[5][i]);
        }
        fwrite(mix, sizeof(mix), 1, record);
    }

    if (!started)
        return;

    // 5.1 order: front left, front right, center, LFE, back left, back right
    std::lock_guard lock(g_outputMutex);
    for (uint32_t i = 0; i < FRAME_SAMPLES; i++)
    {
        float left = channel[0][i] + 0.707f * (channel[2][i] + channel[4][i]);
        float right = channel[1][i] + 0.707f * (channel[2][i] + channel[5][i]);
        g_stereo.push_back(std::clamp(left, -1.0f, 1.0f));
        g_stereo.push_back(std::clamp(right, -1.0f, 1.0f));
    }
    while (g_stereo.size() > MAX_BUFFERED_SAMPLES * 2)
        g_stereo.pop_front();
}
