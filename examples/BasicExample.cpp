/**
 * BasicExample.cpp
 * ============================================================
 * 4 sine-tone objects orbiting the listener, demonstrating two
 * distinct rendering back-ends selectable at the command line:
 *
 *   BasicExample.exe [--mode openal]
 *     Direct path: our ISpatialAudioClient shim talks straight
 *     to OpenAL Soft. Windows Audio is not involved at all.
 *     Works without any spatial sound provider registered.
 *     This is the default mode.
 *
 *   BasicExample.exe --mode mssapi
 *     Windows path: uses the real ISpatialAudioClient from the
 *     Windows Audio stack. Requires a spatial sound provider
 *     (Windows Sonic, Dolby Atmos, or our registered provider)
 *     to be active on the default output device in Sound settings.
 *     Useful for validating the provider registration end-to-end.
 *
 * Timing notes:
 *   Both modes call timeBeginPeriod(1) so Sleep() has 1ms
 *   granularity instead of the default ~15ms, preventing the
 *   app loop from oversleeping and starving the AL upload thread.
 *   The MSSAPI loop is event-driven (WaitForSingleObject on the
 *   event handle returned by Windows Audio) as required by the
 *   real ISpatialAudioClient -- passing a null EventHandle to the
 *   real stack returns E_INVALIDARG.
 *
 * Build:
 *   cl BasicExample.cpp /std:c++17 /I..\include
 *       openal_spatial.lib OpenAL32.lib ole32.lib mmdevapi.lib
 *       winmm.lib /link
 * ============================================================
 */
#define _USE_MATH_DEFINES
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>           // timeBeginPeriod / timeEndPeriod
#include <spatialaudioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <mmreg.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../include/OpenALSpatialAudioClient.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

static constexpr float kPi = 3.14159265358979323846f;

enum class Mode { OpenAL, MSSAPI };

// ----------------------------------------------------------------
// HRESULT helpers
// ----------------------------------------------------------------
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

// ----------------------------------------------------------------
// Sine synthesis
// ----------------------------------------------------------------
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

// ----------------------------------------------------------------
// MSSAPI: obtain ISpatialAudioClient from the default render device
//
// The real Windows stack requires IMMDeviceEnumerator to get an
// IMMDevice, then IMMDevice::Activate(ISpatialAudioClient).
// This will fail if no spatial sound provider is active in Settings.
// ----------------------------------------------------------------
static HRESULT CreateMSSAPIClient(ComPtr<ISpatialAudioClient>& out)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        fprintf(stderr,
            "  CoCreateInstance(MMDeviceEnumerator): 0x%08X (%s)\n"
            "  Windows Audio service may not be running.\n",
            (unsigned)hr, HRName(hr));
        return hr;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        fprintf(stderr, "  GetDefaultAudioEndpoint: 0x%08X (%s)\n",
            (unsigned)hr, HRName(hr));
        return hr;
    }

    hr = device->Activate(
        __uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER,
        nullptr, reinterpret_cast<void**>(out.GetAddressOf()));

    if (FAILED(hr)) {
        fprintf(stderr,
            "  IMMDevice::Activate(ISpatialAudioClient): 0x%08X (%s)\n\n"
            "  CAUSE: No spatial sound provider is active on the default\n"
            "  output device.\n\n"
            "  FIX:\n"
            "    1. Right-click the speaker tray icon -> Spatial sound\n"
            "       and select Windows Sonic, Dolby Atmos, or our provider.\n"
            "    2. Retry: BasicExample.exe --mode mssapi\n\n"
            "  The default openal mode works without any registration:\n"
            "    BasicExample.exe --mode openal\n",
            (unsigned)hr, HRName(hr));
    }
    return hr;
}

// ----------------------------------------------------------------
// Shared per-frame audio writing (identical API in both modes)
// ----------------------------------------------------------------
static void WriteFrame(
    ISpatialAudioObject* objects[4], int count,
    float freqs[4], float sr, float phases[4],
    UINT32 frameCount, float& orbAngle, float dt)
{
    for (int i = 0; i < count; ++i) {
        if (!objects[i]) continue;
        BOOL active = FALSE;
        objects[i]->IsActive(&active);
        if (!active) continue;

        float angle  = orbAngle + i * (2.f * kPi / 4.f);
        float radius = 2.0f + (float)(i % 2) * 1.5f;
        objects[i]->SetPosition(
            radius * std::cosf(angle),
            0.8f * std::sinf(angle * 0.5f),
            radius * std::sinf(angle));

        BYTE*  buf    = nullptr;
        UINT32 bufLen = 0;
        if (SUCCEEDED(objects[i]->GetBuffer(&buf, &bufLen)) && buf && frameCount > 0)
            GenerateSine(reinterpret_cast<float*>(buf),
                         frameCount, freqs[i], sr, phases[i]);
    }
    orbAngle += (0.6f * dt);
    if (orbAngle > 2.f * kPi) orbAngle -= 2.f * kPi;
}

// ----------------------------------------------------------------
// OpenAL render loop
//
// Timing: timeBeginPeriod(1) gives Sleep 1ms resolution so the
// 10ms sleep does not balloon to 15ms and starve the AL upload
// thread. The AL thread also has a built-in repeat-last-frame
// policy so brief oversleeps don't cause audible gaps.
// ----------------------------------------------------------------
static int RunOpenALLoop(
    ISpatialAudioObjectRenderStream* stream,
    ISpatialAudioObject* objects[4],
    const WAVEFORMATEX& wfx,
    ISpatialAudioClient* client)
{
    const float sr         = static_cast<float>(wfx.nSamplesPerSec);
    UINT32 framesPerBuffer = 0;
    client->GetMaxFrameCount(&wfx, &framesPerBuffer);
    const float dt = static_cast<float>(framesPerBuffer) / sr;

    float phases[4] = {};
    float freqs[4]  = { 220.f, 330.f, 440.f, 550.f };
    float orbAngle  = 0.f;
    UINT32 iter     = 0;

    std::atomic<bool> stopFlag{ false };
    std::thread([&stopFlag] { (void)getchar();
        stopFlag.store(true, std::memory_order_release); }).detach();

    // 1ms timer resolution -- prevents Sleep(10) from sleeping 15ms
    timeBeginPeriod(1);

    HRESULT loopHR = S_OK;
    while (!stopFlag.load(std::memory_order_acquire)) {
        UINT32 available = 0, frameCount = 0;
        loopHR = stream->BeginUpdatingAudioObjects(&available, &frameCount);
        if (FAILED(loopHR)) {
            fprintf(stderr, "\n[Loop %u] BeginUpdatingAudioObjects: "
                "hr=0x%08X (%s)\n", iter, (unsigned)loopHR, HRName(loopHR));
            break;
        }

        if (iter % 100 == 0)
            printf("[Loop %4u] available=%u frameCount=%u angle=%.2f\n",
                iter, available, frameCount, orbAngle);

        WriteFrame(objects, 4, freqs, sr, phases, frameCount, orbAngle, dt);

        loopHR = stream->EndUpdatingAudioObjects();
        if (FAILED(loopHR)) {
            fprintf(stderr, "\n[Loop %u] EndUpdatingAudioObjects: "
                "hr=0x%08X (%s)\n", iter, (unsigned)loopHR, HRName(loopHR));
            break;
        }
        ++iter;
        Sleep(static_cast<DWORD>(dt * 1000.f));
    }

    timeEndPeriod(1);

    printf("\n[Exit] Ran %u iterations. Final HR: 0x%08X (%s)\n\n",
        iter, (unsigned)loopHR, HRName(loopHR));
    return 0;
}

// ----------------------------------------------------------------
// MSSAPI render loop
//
// The real ISpatialAudioClient is event-driven: Windows signals
// hEvent when it is ready for the next frame. We must wait on that
// event rather than sleeping, otherwise ActivateSpatialAudioStream
// returns E_INVALIDARG (null EventHandle is rejected by the real
// stack, though our shim accepts it).
// ----------------------------------------------------------------
static int RunMSSAPILoop(
    ISpatialAudioObjectRenderStream* stream,
    ISpatialAudioObject* objects[4],
    const WAVEFORMATEX& wfx,
    HANDLE hEvent)
{
    const float sr = static_cast<float>(wfx.nSamplesPerSec);
    UINT32 framesPerBuffer = 480; // Will be overridden by BeginUpdating

    float phases[4] = {};
    float freqs[4]  = { 220.f, 330.f, 440.f, 550.f };
    float orbAngle  = 0.f;
    UINT32 iter     = 0;

    std::atomic<bool> stopFlag{ false };
    std::thread([&stopFlag] { (void)getchar();
        stopFlag.store(true, std::memory_order_release); }).detach();

    HRESULT loopHR = S_OK;
    while (!stopFlag.load(std::memory_order_acquire)) {
        // Windows signals hEvent when it wants the next frame.
        // 200ms timeout as a safety net so we can check stopFlag.
        DWORD waitResult = WaitForSingleObject(hEvent, 200);
        if (waitResult == WAIT_TIMEOUT) continue;
        if (waitResult != WAIT_OBJECT_0) break;

        UINT32 available = 0, frameCount = 0;
        loopHR = stream->BeginUpdatingAudioObjects(&available, &frameCount);
        if (FAILED(loopHR)) {
            fprintf(stderr, "\n[Loop %u] BeginUpdatingAudioObjects: "
                "hr=0x%08X (%s)\n", iter, (unsigned)loopHR, HRName(loopHR));
            break;
        }

        if (frameCount > 0) framesPerBuffer = frameCount;
        float dt = (float)framesPerBuffer / sr;

        if (iter % 100 == 0)
            printf("[Loop %4u] available=%u frameCount=%u angle=%.2f\n",
                iter, available, frameCount, orbAngle);

        WriteFrame(objects, 4, freqs, sr, phases, frameCount, orbAngle, dt);

        loopHR = stream->EndUpdatingAudioObjects();
        if (FAILED(loopHR)) {
            fprintf(stderr, "\n[Loop %u] EndUpdatingAudioObjects: "
                "hr=0x%08X (%s)\n", iter, (unsigned)loopHR, HRName(loopHR));
            break;
        }
        ++iter;
    }

    printf("\n[Exit] Ran %u iterations. Final HR: 0x%08X (%s)\n\n",
        iter, (unsigned)loopHR, HRName(loopHR));
    return 0;
}

// ----------------------------------------------------------------
// Shared stream setup (format check, activation, object creation)
// ----------------------------------------------------------------
static int SetupAndRun(ISpatialAudioClient* client,
                        const WAVEFORMATEX& wfx,
                        Mode mode)
{
    UINT32 framesPerBuffer = 0;
    if (!CHECK(client->GetMaxFrameCount(&wfx, &framesPerBuffer))) return 1;
    printf("  Frames/buffer: %u  (%.1f ms)\n\n",
        framesPerBuffer, framesPerBuffer * 1000.0 / wfx.nSamplesPerSec);

    printf("[Step 4] Activating render stream...\n");

    // The real Windows ISpatialAudioClient rejects a null EventHandle.
    // Create an auto-reset event for MSSAPI mode; our shim accepts either.
    HANDLE hEvent = nullptr;
    if (mode == Mode::MSSAPI) {
        hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent) {
            fprintf(stderr, "  [FAIL] CreateEvent: %lu\n", GetLastError());
            return 1;
        }
    }

    SpatialAudioObjectRenderStreamActivationParams sp = {};
    sp.ObjectFormat          = const_cast<WAVEFORMATEX*>(&wfx);
    sp.StaticObjectTypeMask  = AudioObjectType_None;
    sp.MaxDynamicObjectCount = 4;
    sp.Category              = AudioCategory_GameEffects;
    sp.EventHandle           = hEvent;    // non-null for MSSAPI, null for OpenAL
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
        if (hEvent) CloseHandle(hEvent);
        return 1;
    }
    printf("\n");

    printf("[Step 5] Starting stream...\n");
    if (!CHECK(stream->Start())) {
        if (hEvent) CloseHandle(hEvent);
        return 1;
    }
    printf("\n");

    printf("[Step 6] Activating 4 dynamic audio objects...\n");
    ComPtr<ISpatialAudioObject> objPtrs[4];
    ISpatialAudioObject*        rawPtrs[4] = {};
    for (int i = 0; i < 4; ++i) {
        char tag[48]; sprintf_s(tag, "ActivateSpatialAudioObject[%d]", i);
        HRESULT hr = stream->ActivateSpatialAudioObject(
            AudioObjectType_Dynamic, objPtrs[i].GetAddressOf());
        if (!CHK(hr, tag)) { stream->Stop(); if (hEvent) CloseHandle(hEvent); return 1; }
        rawPtrs[i] = objPtrs[i].Get();
    }
    printf("\n");

    // Extended per-object params (only meaningful in openal mode)
    if (mode == Mode::OpenAL) {
        ComPtr<OpenALSpatial::IOpenALSpatialAudioClient> ext;
        ComPtr<IUnknown>(client).As(&ext);
        if (ext) {
            printf("[Step 7] Applying per-object spatial params...\n");
            for (int i = 0; i < 4; ++i) {
                OpenALSpatial::ObjectSpatialParams osp;
                osp.referenceDistance = 1.0f;
                osp.maxDistance       = 20.0f;
                osp.rolloffFactor     = 1.0f;
                osp.distModel =
                    OpenALSpatial::DistanceModel::InverseDistanceClamped;
                ext->SetObjectSpatialParams(rawPtrs[i], osp);
            }
            printf("  [OK]   Extended params applied.\n\n");
        }
    }

    printf("Stream running. 4 objects orbiting the listener.\n");
    printf("Press Enter to stop...\n\n");

    int ret;
    if (mode == Mode::OpenAL)
        ret = RunOpenALLoop(stream.Get(), rawPtrs, wfx, client);
    else
        ret = RunMSSAPILoop(stream.Get(), rawPtrs, wfx, hEvent);

    // Clean up objects before stopping
    {
        UINT32 d1 = 0, d2 = 0;
        if (SUCCEEDED(stream->BeginUpdatingAudioObjects(&d1, &d2))) {
            for (auto& obj : objPtrs) if (obj) obj->SetEndOfStream(0);
            stream->EndUpdatingAudioObjects();
        }
    }
    Sleep(50);
    CHECK(stream->Stop());
    for (auto& obj : objPtrs) obj.Reset();
    if (hEvent) CloseHandle(hEvent);
    return ret;
}

// ----------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------
int main(int argc, char* argv[])
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    Mode mode = Mode::OpenAL;
    for (int i = 1; i < argc; ++i) {
        if (_stricmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            ++i;
            if      (_stricmp(argv[i], "mssapi") == 0) mode = Mode::MSSAPI;
            else if (_stricmp(argv[i], "openal") == 0) mode = Mode::OpenAL;
            else {
                fprintf(stderr,
                    "Unknown mode '%s'.\n"
                    "Usage: BasicExample.exe [--mode openal|mssapi]\n"
                    "  openal  Direct -> OpenAL Soft (default, no registration needed)\n"
                    "  mssapi  Real Windows Spatial Audio stack (needs active provider)\n",
                    argv[i]);
                CoUninitialize(); return 1;
            }
        }
    }

    printf("OpenAL Spatial Audio -- Basic Example\n");
    printf("======================================\n");
    if (mode == Mode::OpenAL) {
        printf("Mode: openal  (direct -> OpenAL Soft, Windows Audio bypassed)\n");
        printf("Tip:  For the Windows Audio route, use: BasicExample.exe --mode mssapi\n");
    } else {
        printf("Mode: mssapi  (real Windows Spatial Audio stack)\n");
        printf("Tip:  For the direct OpenAL route, use: BasicExample.exe --mode openal\n");
    }
    printf("\n");

    WAVEFORMATEX wfx    = {};
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = 48000 * 4;

    ComPtr<ISpatialAudioClient> client;

    if (mode == Mode::OpenAL) {
        printf("[Step 1] Creating OpenAL Soft spatial client (direct)...\n");
        OpenALSpatial::HRTFConfig cfg;
        cfg.mode         = OpenALSpatial::HRTFMode::Default;
        cfg.enableReverb = true;
        cfg.reverbMix    = 0.10f;
        client = OpenALSpatial::CreateClient(cfg);
        if (!client) {
            fprintf(stderr,
                "  [FAIL] CreateClient returned null.\n"
                "  Ensure openal32.dll or soft_oal.dll is on PATH or\n"
                "  beside this executable.\n");
            CoUninitialize(); return 1;
        }
        printf("  [OK]   OpenAL Soft client created.\n\n");

        printf("[Step 2] Querying HRTF info...\n");
        ComPtr<OpenALSpatial::IOpenALSpatialAudioClient> ext;
        client.As(&ext);
        if (ext) {
            wchar_t name[256] = {};
            ext->GetActiveHRTFName(name, 256);
            wprintf(L"  Active HRTF: %s\n", name[0] ? name : L"(none)");
            UINT32 n = 0; LPWSTR names = nullptr;
            ext->EnumerateHRTFDatasets(&n, &names);
            wprintf(L"  Datasets   : %u\n", n);
            if (n && names) {
                wchar_t* p = names;
                for (UINT32 k = 0; k < n; ++k, p += wcslen(p) + 1)
                    wprintf(L"    [%u] %s\n", k, p);
                CoTaskMemFree(names);
            }
        }
        printf("\n");

    } else {
        printf("[Step 1] Creating ISpatialAudioClient via Windows Audio...\n");
        if (FAILED(CreateMSSAPIClient(client))) { CoUninitialize(); return 1; }
        printf("  [OK]   Windows Spatial Audio client created.\n\n");
        printf("[Step 2] (HRTF enumeration not available in mssapi mode)\n\n");
    }

    printf("[Step 3] Validating audio format (48 kHz, mono, float32)...\n");
    if (!CHECK(client->IsAudioObjectFormatSupported(&wfx)))
        { CoUninitialize(); return 1; }

    int ret = SetupAndRun(client.Get(), wfx, mode);
    client.Reset();
    CoUninitialize();
    return ret;
}
