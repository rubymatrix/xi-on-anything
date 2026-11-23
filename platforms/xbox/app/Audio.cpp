#include "pch.h"

#include "Audio.h"

#include <xaudio2.h>

#include "uwp_bridge.h"

namespace
{
    // Blocks in flight: each is one 10 ms callback. XAudio2 reads a buffer where it lies until it has
    // played it, so they rotate through a fixed ring; at MAX_QUEUED (60 ms) a block is dropped rather
    // than let the delay grow, since the producer runs on its own clock.
    constexpr int RING = 16, MAX_QUEUED = 6, BLOCK_FRAMES = 480;

    winrt::com_ptr<IXAudio2> g_xaudio;
    IXAudio2MasteringVoice* g_master;
    IXAudio2SourceVoice* g_voice;
    float g_ring[RING][BLOCK_FRAMES * 2];
    int g_next;
    unsigned g_dropped;

    void play(const float* frames, int n)
    {
        if (!g_voice || n <= 0)
            return;
        XAUDIO2_VOICE_STATE st;
        g_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued >= MAX_QUEUED)
        {
            ++g_dropped;
            return;
        }
        n = n < BLOCK_FRAMES ? n : BLOCK_FRAMES;
        float* block = g_ring[g_next];
        g_next = (g_next + 1) % RING;
        memcpy(block, frames, sizeof(float) * 2 * (size_t)n);
        XAUDIO2_BUFFER b{};
        b.AudioBytes = (UINT32)(sizeof(float) * 2 * (size_t)n);
        b.pAudioData = reinterpret_cast<const BYTE*>(block);
        g_voice->SubmitSourceBuffer(&b);
    }
}

bool StartAudio()
{
    if (g_voice)
        return true;
    HRESULT hr = XAudio2Create(g_xaudio.put(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr))
        hr = g_xaudio->CreateMasteringVoice(&g_master);
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = 2;
    f.nSamplesPerSec = 48000;
    f.wBitsPerSample = 32;
    f.nBlockAlign = (WORD)(f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    if (SUCCEEDED(hr))
        hr = g_xaudio->CreateSourceVoice(&g_voice, &f);
    if (SUCCEEDED(hr))
        hr = g_voice->Start();
    if (FAILED(hr))
    {
        fprintf(stderr, "[app] audio: XAudio2 unavailable (%08lx); the game runs silent\n", (unsigned long)hr);
        g_voice = nullptr;
        return false;
    }
    uwp_set_audio_sink(play);
    fprintf(stderr, "[app] audio: XAudio2, 48 kHz stereo\n");
    return true;
}
