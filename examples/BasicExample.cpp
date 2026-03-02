/**
 * BasicExample.cpp
 * ============================================================
 * Demonstrates using OpenALSpatialAudioClient as a drop-in
 * replacement for Microsoft ISpatialAudioClient.
 *
 * This example creates 4 audio objects orbiting the listener
 * and plays a 440 Hz sine tone through each one, demonstrating
 * true per-object HRTF via OpenAL Soft.
 *
 * Build:
 *   cl BasicExample.cpp /std:c++17 /I..\include
 *       openal_spatial.lib OpenAL32.lib ole32.lib /link
 * ============================================================
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>
#include <mmreg.h>

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

// std::numbers (C++20) not available in C++17 — define pi locally
static constexpr float kPi = 3.14159265358979323846f;

// Our OpenAL-backed client (same include as MS ISpatialAudioClient apps)
#include "../include/OpenALSpatialAudioClient.h"

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────
// Generate one period of a sine wave at frequency Hz
// ─────────────────────────────────────────────────────────────
static void GenerateSine(float* buf, UINT32 frames, float freq,
                          float sampleRate, float& phase)
{
    float phaseStep = 2.f * kPi * freq / sampleRate;
    for (UINT32 i = 0; i < frames; ++i) {
        buf[i] = 0.25f * std::sin(phase);
        phase += phaseStep;
        if (phase > 2.f * kPi)
            phase -= 2.f * kPi;
    }
}

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    printf("OpenAL Spatial Audio – Basic Example\n");
    printf("=====================================\n\n");

    // ── 1. Create our OpenAL-backed client ───────────────────
    OpenALSpatial::HRTFConfig hrtfCfg;
    hrtfCfg.mode         = OpenALSpatial::HRTFMode::Default;
    hrtfCfg.enableReverb = true;
    hrtfCfg.reverbMix    = 0.10f;

    ComPtr<ISpatialAudioClient> client =
        OpenALSpatial::CreateClient(hrtfCfg);

    if (!client) {
        fprintf(stderr, "Failed to create OpenAL spatial client.\n"
            "Ensure OpenAL Soft is installed (soft_oal.dll / openal32.dll).\n");
        return 1;
    }

    // ── 2. Query HRTF info via extended interface ─────────────
    ComPtr<OpenALSpatial::IOpenALSpatialAudioClient> extClient;
    client.As(&extClient);
    if (extClient) {
        wchar_t hrtfName[256] = {};
        extClient->GetActiveHRTFName(hrtfName, 256);
        wprintf(L"Active HRTF dataset: %s\n\n", hrtfName);

        UINT32 count = 0;
        LPWSTR names = nullptr;
        extClient->EnumerateHRTFDatasets(&count, &names);
        if (count > 0) {
            wprintf(L"Available HRTF datasets (%u):\n", count);
            wchar_t* p = names;
            for (UINT32 i = 0; i < count; ++i) {
                wprintf(L"  [%u] %s\n", i, p);
                p += wcslen(p) + 1;
            }
            CoTaskMemFree(names);
            wprintf(L"\n");
        }
    }

    // ── 3. Set up the audio format (mono float32 @ 48 kHz) ───
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag     = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels      = 1;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 32;
    wfx.nBlockAlign    = 4;
    wfx.nAvgBytesPerSec= 48000 * 4;
    wfx.cbSize         = 0;

    UINT32 framesPerBuffer = 0;
    client->GetMaxFrameCount(&wfx, &framesPerBuffer);
    printf("Frames per buffer: %u (%.1f ms)\n\n",
        framesPerBuffer, framesPerBuffer * 1000.0 / 48000.0);

    // ── 4. Activate an ISpatialAudioObjectRenderStream ────────
    SpatialAudioObjectRenderStreamActivationParams streamParams = {};
    streamParams.ObjectFormat        = &wfx;
    streamParams.StaticObjectTypeMask= AudioObjectType_None;
    streamParams.MaxDynamicObjectCount = 4;
    streamParams.Category            = AudioCategory_GameEffects;
    streamParams.EventHandle         = nullptr;
    streamParams.NotifyObject        = nullptr;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt         = VT_BLOB;
    pv.blob.cbSize    = sizeof(streamParams);
    pv.blob.pBlobData = (BYTE*)&streamParams;

    ComPtr<ISpatialAudioObjectRenderStream> stream;
    HRESULT hr = client->ActivateSpatialAudioStream(
        &pv, __uuidof(ISpatialAudioObjectRenderStream),
        (void**)stream.GetAddressOf());

    if (FAILED(hr)) {
        fprintf(stderr, "ActivateSpatialAudioStream failed: 0x%08X\n", hr);
        return 1;
    }

    // ── 5. Start the stream ───────────────────────────────────
    stream->Start();
    printf("Stream started. 4 objects orbiting the listener.\n");
    printf("Press Enter to stop...\n\n");

    // ── 6. Activate 4 dynamic objects ────────────────────────
    ComPtr<ISpatialAudioObject> objects[4];
    for (int i = 0; i < 4; ++i) {
        hr = stream->ActivateSpatialAudioObject(
            AudioObjectType_Dynamic, objects[i].GetAddressOf());
        if (FAILED(hr)) {
            fprintf(stderr, "ActivateSpatialAudioObject[%d] failed\n", i);
            return 1;
        }
    }

    // ── 7. Per-object HRTF params (optional extension) ───────
    if (extClient) {
        for (int i = 0; i < 4; ++i) {
            OpenALSpatial::ObjectSpatialParams sp;
            sp.referenceDistance = 2.0f;
            sp.maxDistance       = 50.0f;
            sp.rolloffFactor     = 1.0f;
            sp.distModel = OpenALSpatial::DistanceModel::InverseDistanceClamped;
            extClient->SetObjectSpatialParams(objects[i].Get(), sp);
        }
    }

    // ── 8. Render loop ────────────────────────────────────────
    float phases[4] = {0.f, 0.f, 0.f, 0.f};
    float freqs[4]  = {220.f, 330.f, 440.f, 550.f};
    float orbAngle  = 0.f;
    const float orbitRadius = 3.0f;   // metres
    const float orbitSpeed  = 0.5f;   // radians/sec
    const float dt          = (float)framesPerBuffer / 48000.f;

    // Update listener orientation (head-locked, facing -Z)
    OpenALSpatial::ListenerOrientation listener;
    listener.fwdX = 0.f; listener.fwdY = 0.f; listener.fwdZ = -1.f;
    listener.upX  = 0.f; listener.upY  = 1.f; listener.upZ  =  0.f;
    listener.masterGain = 1.0f;

    // (SetListenerOrientation is available via IOpenALSpatialAudioClient
    //  if you query it from the client, not the stream directly)

    DWORD stopSignal = 0;
    std::thread inputThread([&stopSignal] {
        getchar();
        stopSignal = 1;
    });

    while (!stopSignal) {
        UINT32 available = 0, frameCount = 0;
        hr = stream->BeginUpdatingAudioObjects(&available, &frameCount);
        if (FAILED(hr)) break;

        // Update each object
        for (int i = 0; i < 4; ++i) {
            if (!objects[i]) continue;

            BOOL active = FALSE;
            objects[i]->IsActive(&active);
            if (!active) continue;

            // Orbit position (equally spaced, different radii per object)
            float angle = orbAngle + i * (2.f * kPi / 4.f);
            float radius = orbitRadius * (0.5f + 0.5f * (i % 2));
            float x = radius * std::cos(angle);
            float z = radius * std::sin(angle);
            float y = std::sin(angle * 0.3f);  // gentle up-down
            objects[i]->SetPosition(x, y, z);

            // Write audio
            BYTE* buf = nullptr;
            UINT32 bufLen = 0;
            if (SUCCEEDED(objects[i]->GetBuffer(&buf, &bufLen))) {
                float* f = reinterpret_cast<float*>(buf);
                GenerateSine(f, frameCount, freqs[i], 48000.f, phases[i]);
            }
        }

        stream->EndUpdatingAudioObjects();

        orbAngle += orbitSpeed * dt;
        if (orbAngle > 2.f * kPi)
            orbAngle -= 2.f * kPi;

        // Sleep for roughly one buffer period
        Sleep((DWORD)(dt * 1000.f));
    }

    // ── 9. Clean up ───────────────────────────────────────────
    printf("\nStopping...\n");

    UINT32 dummy1, dummy2;
    stream->BeginUpdatingAudioObjects(&dummy1, &dummy2);
    for (auto& obj : objects) {
        if (obj) obj->SetEndOfStream(0);
    }
    stream->EndUpdatingAudioObjects();
    Sleep(100);

    stream->Stop();
    if (inputThread.joinable()) inputThread.join();

    printf("Done.\n");
    CoUninitialize();
    return 0;
}
