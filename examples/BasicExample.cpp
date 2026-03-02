/**
 * BasicExample.cpp
 * ============================================================
 * Demonstrates OpenALSpatialAudioClient as a drop-in replacement
 * for ISpatialAudioClient, with 4 sine-tone objects orbiting
 * the listener processed through OpenAL Soft's per-object HRTF.
 *
 * Build:
 *   cl BasicExample.cpp /std:c++17 /I..\include
 *       openal_spatial.lib OpenAL32.lib ole32.lib /link
 * ============================================================
 */
#define _USE_MATH_DEFINES
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>
#include <mmreg.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

#include "../include/OpenALSpatialAudioClient.h"

using Microsoft::WRL::ComPtr;

static constexpr float kPi = 3.14159265358979323846f;

// ─────────────────────────────────────────────────────────────
// HRESULT diagnostic helpers
// ─────────────────────────────────────────────────────────────
static const char* HRName(HRESULT hr)
{
    switch ((unsigned)hr) {
    case (unsigned)S_OK:                          return "S_OK";
    case (unsigned)E_POINTER:                     return "E_POINTER";
    case (unsigned)E_INVALIDARG:                  return "E_INVALIDARG";
    case (unsigned)E_OUTOFMEMORY:                 return "E_OUTOFMEMORY";
    case (unsigned)E_NOINTERFACE:                 return "E_NOINTERFACE";
    case (unsigned)E_FAIL:                        return "E_FAIL";
    case (unsigned)AUDCLNT_E_UNSUPPORTED_FORMAT:  return "AUDCLNT_E_UNSUPPORTED_FORMAT";
    case (unsigned)AUDCLNT_E_SERVICE_NOT_RUNNING: return "AUDCLNT_E_SERVICE_NOT_RUNNING";
    case (unsigned)AUDCLNT_E_OUT_OF_ORDER:        return "AUDCLNT_E_OUT_OF_ORDER";
    case (unsigned)AUDCLNT_E_DEVICE_INVALIDATED:  return "AUDCLNT_E_DEVICE_INVALIDATED";
    case 0x88890020u:                             return "SPTLAUDCLNT_E_OBJECT_NOTVALID";
    case 0x88890017u:                             return "SPTLAUDCLNT_E_NO_MORE_OBJECTS";
    default:                                      return "(unknown)";
    }
}

static bool CHK(HRESULT hr, const char* callSite)
{
    if (SUCCEEDED(hr)) { printf("  [OK]   %s\n", callSite); return true; }
    fprintf(stderr, "  [FAIL] %s  hr=0x%08X (%s)\n",
        callSite, (unsigned)hr, HRName(hr));
    return false;
}
#define CHECK(expr) CHK((expr), #expr)

// ─────────────────────────────────────────────────────────────
// Audio synthesis
// ─────────────────────────────────────────────────────────────
static void GenerateSine(float* buf, UINT32 frames,
                          float freq, float sr, float& phase)
{
    const float step = 2.f * kPi * freq / sr;
    for (UINT32 i = 0; i < frames; ++i) {
        buf[i]  = 0.20f * std::sinf(phase);
        phase  += step;
        if (phase > 2.f * kPi) phase -= 2.f * kPi;
    }
}

// ─────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────
int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    printf("OpenAL Spatial Audio -- Basic Example\n");
    printf("======================================\n\n");

    // ── 1. Create client ──────────────────────────────────────
    printf("[Step 1] Creating OpenAL spatial client...\n");
    OpenALSpatial::HRTFConfig hrtfCfg;
    hrtfCfg.mode         = OpenALSpatial::HRTFMode::Default;
    hrtfCfg.enableReverb = true;
    hrtfCfg.reverbMix    = 0.10f;

    ComPtr<ISpatialAudioClient> client = OpenALSpatial::CreateClient(hrtfCfg);
    if (!client) {
        fprintf(stderr, "  [FAIL] CreateClient returned null.\n"
            "  Ensure OpenAL Soft is installed and openal32.dll/soft_oal.dll\n"
            "  is on PATH or beside this executable.\n");
        CoUninitialize();
        return 1;
    }
    printf("  [OK]   Client created.\n\n");

    // ── 2. HRTF info ──────────────────────────────────────────
    printf("[Step 2] Querying HRTF info...\n");
    ComPtr<OpenALSpatial::IOpenALSpatialAudioClient> extClient;
    client.As(&extClient);
    if (extClient) {
        wchar_t hrtfName[256] = {};
        extClient->GetActiveHRTFName(hrtfName, 256);
        wprintf(L"  Active HRTF  : %s\n",
            hrtfName[0] ? hrtfName : L"(none -- HRTF may not be active)");

        UINT32 count = 0; LPWSTR names = nullptr;
        extClient->EnumerateHRTFDatasets(&count, &names);
        wprintf(L"  Datasets     : %u\n", count);
        if (count > 0 && names) {
            wchar_t* p = names;
            for (UINT32 i = 0; i < count; ++i) {
                wprintf(L"    [%u] %s\n", i, p);
                p += wcslen(p) + 1;
            }
            CoTaskMemFree(names);
        }
    }
    printf("\n");

    // ── 3. Format ─────────────────────────────────────────────
    printf("[Step 3] Configuring audio format...\n");
    WAVEFORMATEX wfx    = {};
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = 48000 * 4;

    if (!CHECK(client->IsAudioObjectFormatSupported(&wfx))) {
        CoUninitialize(); return 1;
    }

    UINT32 framesPerBuffer = 0;
    if (!CHECK(client->GetMaxFrameCount(&wfx, &framesPerBuffer))) {
        CoUninitialize(); return 1;
    }
    printf("  Frames/buffer: %u  (%.1f ms)\n\n",
        framesPerBuffer, framesPerBuffer * 1000.0 / 48000.0);

    // ── 4. Activate stream ────────────────────────────────────
    printf("[Step 4] Activating render stream...\n");
    SpatialAudioObjectRenderStreamActivationParams sp = {};
    sp.ObjectFormat          = &wfx;
    sp.StaticObjectTypeMask  = AudioObjectType_None;
    sp.MaxDynamicObjectCount = 4;
    sp.Category              = AudioCategory_GameEffects;
    sp.EventHandle           = nullptr;
    sp.NotifyObject          = nullptr;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt             = VT_BLOB;
    pv.blob.cbSize    = sizeof(sp);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&sp);

    ComPtr<ISpatialAudioObjectRenderStream> stream;
    if (!CHECK(client->ActivateSpatialAudioStream(
            &pv, __uuidof(ISpatialAudioObjectRenderStream),
            reinterpret_cast<void**>(stream.GetAddressOf())))) {
        CoUninitialize(); return 1;
    }
    printf("\n");

    // ── 5. Start ──────────────────────────────────────────────
    printf("[Step 5] Starting stream...\n");
    if (!CHECK(stream->Start())) { CoUninitialize(); return 1; }
    printf("\n");

    // ── 6. Activate objects ───────────────────────────────────
    printf("[Step 6] Activating 4 dynamic audio objects...\n");
    ComPtr<ISpatialAudioObject> objects[4];
    for (int i = 0; i < 4; ++i) {
        char tag[48]; sprintf_s(tag, "ActivateSpatialAudioObject[%d]", i);
        HRESULT hr = stream->ActivateSpatialAudioObject(
            AudioObjectType_Dynamic, objects[i].GetAddressOf());
        if (!CHK(hr, tag)) { stream->Stop(); CoUninitialize(); return 1; }
    }
    printf("\n");

    // ── 7. Per-object extended params ─────────────────────────
    if (extClient) {
        printf("[Step 7] Applying per-object spatial params...\n");
        for (int i = 0; i < 4; ++i) {
            OpenALSpatial::ObjectSpatialParams osp;
            osp.referenceDistance = 1.0f;
            osp.maxDistance       = 20.0f;
            osp.rolloffFactor     = 1.0f;
            osp.distModel = OpenALSpatial::DistanceModel::InverseDistanceClamped;
            extClient->SetObjectSpatialParams(objects[i].Get(), osp);
        }
        printf("  [OK]   Extended params applied.\n\n");
    }

    // ── 8. Render loop ────────────────────────────────────────
    printf("Stream running. 4 objects orbiting the listener.\n");
    printf("Press Enter to stop...\n\n");

    const float sr          = static_cast<float>(wfx.nSamplesPerSec);
    const float dt          = static_cast<float>(framesPerBuffer) / sr;
    const float orbitSpeed  = 0.6f;   // rad/s

    float phases[4] = {};
    float freqs[4]  = { 220.f, 330.f, 440.f, 550.f };
    float orbAngle  = 0.f;
    UINT32 iter     = 0;

    // atomic<bool> avoids the data race that plain DWORD had
    std::atomic<bool> stopFlag{ false };

    // detach() so cleanup never hangs if the loop exits early
    std::thread([&stopFlag] {
        (void)getchar();
        stopFlag.store(true, std::memory_order_release);
    }).detach();

    HRESULT loopHR = S_OK;

    while (!stopFlag.load(std::memory_order_acquire)) {

        UINT32 available = 0, frameCount = 0;
        loopHR = stream->BeginUpdatingAudioObjects(&available, &frameCount);
        if (FAILED(loopHR)) {
            fprintf(stderr,
                "\n[Loop %u] BeginUpdatingAudioObjects FAILED: "
                "hr=0x%08X (%s)\n",
                iter, (unsigned)loopHR, HRName(loopHR));
            break;
        }

        // Periodic heartbeat log (~every second)
        if (iter % 100 == 0) {
            printf("[Loop %4u] available=%u frameCount=%u angle=%.2f\n",
                iter, available, frameCount, orbAngle);
        }

        for (int i = 0; i < 4; ++i) {
            if (!objects[i]) continue;

            BOOL active = FALSE;
            objects[i]->IsActive(&active);
            if (!active) {
                if (iter % 100 == 0)
                    printf("  [Loop %u] object[%d] not active\n", iter, i);
                continue;
            }

            // Orbit: 4 objects equally spaced, alternating radii & heights
            float angle  = orbAngle + i * (2.f * kPi / 4.f);
            float radius = 2.0f + (float)(i % 2) * 1.5f;
            float x = radius * std::cosf(angle);
            float z = radius * std::sinf(angle);
            float y = 0.8f   * std::sinf(angle * 0.5f);

            HRESULT hr = objects[i]->SetPosition(x, y, z);
            if (FAILED(hr) && iter % 100 == 0)
                fprintf(stderr, "  SetPosition[%d]: 0x%08X (%s)\n",
                    i, (unsigned)hr, HRName(hr));

            BYTE* buf = nullptr; UINT32 bufLen = 0;
            hr = objects[i]->GetBuffer(&buf, &bufLen);
            if (SUCCEEDED(hr) && buf && frameCount > 0)
                GenerateSine(reinterpret_cast<float*>(buf),
                    frameCount, freqs[i], sr, phases[i]);
            else if (FAILED(hr) && iter % 100 == 0)
                fprintf(stderr, "  GetBuffer[%d]: 0x%08X (%s)\n",
                    i, (unsigned)hr, HRName(hr));
        }

        loopHR = stream->EndUpdatingAudioObjects();
        if (FAILED(loopHR)) {
            fprintf(stderr,
                "\n[Loop %u] EndUpdatingAudioObjects FAILED: "
                "hr=0x%08X (%s)\n",
                iter, (unsigned)loopHR, HRName(loopHR));
            break;
        }

        orbAngle += orbitSpeed * dt;
        if (orbAngle > 2.f * kPi) orbAngle -= 2.f * kPi;
        ++iter;

        Sleep(static_cast<DWORD>(dt * 1000.f));
    }

    printf("\n[Exit] Ran %u iterations. Final HR: 0x%08X (%s)\n\n",
        iter, (unsigned)loopHR, HRName(loopHR));

    // ── 9. Clean up ───────────────────────────────────────────
    printf("[Cleanup] Sending end-of-stream to all objects...\n");
    {
        UINT32 d1 = 0, d2 = 0;
        if (SUCCEEDED(stream->BeginUpdatingAudioObjects(&d1, &d2))) {
            for (auto& obj : objects)
                if (obj) obj->SetEndOfStream(0);
            stream->EndUpdatingAudioObjects();
        }
    }
    Sleep(50);

    printf("[Cleanup] Stopping stream...\n");
    CHECK(stream->Stop());

    // Release in reverse-dependency order
    for (auto& obj : objects) obj.Reset();
    stream.Reset();
    client.Reset();

    printf("[Cleanup] Done.\n");
    CoUninitialize();
    return 0;
}
