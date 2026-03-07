/**
 * BasicExample.cpp
 * ============================================================
 * Four audio objects on distinct orbits, demonstrating two
 * rendering back-ends selectable at the command line:
 *
 *   BasicExample.exe [--mode openal]   (default)
 *     Direct -> OpenAL Soft. Windows Spatial Audio not involved.
 *     Works without any spatial sound provider registered.
 *
 *   BasicExample.exe --mode mssapi
 *     Real Windows Spatial Audio stack. Requires a spatial sound
 *     provider (Windows Sonic, Dolby Atmos, or our registered
 *     provider) to be active in Sound settings on the default
 *     output device.
 *
 * Audio objects:
 *   [0] Continuous pink noise  -- wide orbit, constant
 *   [1] Intermittent pink noise -- tight orbit, bursts every ~1.2 s
 *   [2] Continuous low note (110 Hz)  -- figure-eight path
 *   [3] Continuous high note (880 Hz) -- fast tight circle overhead
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
#include <timeapi.h>
#include <spatialaudioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <mmreg.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <thread>

#include "../include/OpenALSpatialAudioClient.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

static constexpr float kPi  = 3.14159265358979323846f;
static constexpr float k2Pi = 2.f * kPi;

enum class Mode { OpenAL, MSSAPI };

// ----------------------------------------------------------------
// HRESULT name table
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
    case (unsigned)AUDCLNT_E_DEVICE_INVALIDATED:  return "AUDCLNT_E_DEVICE_INVALIDATED (spatial audio turned off)";
    case 0x88890020u:                             return "SPTLAUDCLNT_E_OBJECT_NOTVALID";
    case 0x88890017u:                             return "SPTLAUDCLNT_E_NO_MORE_OBJECTS";
    case 0x88890100u:                             return "SPTLAUDCLNT_E_DESTROYED (spatial audio turned off)";
    default:                                      return "(unknown)";
    }
}

// Returns true if the HRESULT signals the user disabled spatial audio --
// this is expected and should terminate the loop gracefully, not as a crash.
static bool IsDeviceGone(HRESULT hr)
{
    return hr == (HRESULT)0x88890100  // SPTLAUDCLNT_E_DESTROYED
        || hr == AUDCLNT_E_DEVICE_INVALIDATED;
}

static bool CHK(HRESULT hr, const char* callSite)
{
    if (SUCCEEDED(hr)) { printf("  [OK]   %s\n", callSite); return true; }
    if (IsDeviceGone(hr)) {
        printf("  [--]   %s  (spatial audio disabled -- hr=0x%08X)\n",
            callSite, (unsigned)hr);
        return false;
    }
    fprintf(stderr, "  [FAIL] %s  hr=0x%08X (%s)\n",
        callSite, (unsigned)hr, HRName(hr));
    return false;
}
#define CHECK(expr) CHK((expr), #expr)

// ----------------------------------------------------------------
// Pink noise generator (Paul Kellet's "economy" method)
// Produces perceptually flat spectral density; much more natural
// than white noise or tones for spatial audio demos.
// ----------------------------------------------------------------
struct PinkNoise {
    float b0=0,b1=0,b2=0,b3=0,b4=0,b5=0,b6=0;
    uint32_t rng = 0x12345678u;

    float next()
    {
        // LCG white noise
        rng = rng * 1664525u + 1013904223u;
        float w = (float)(int32_t)rng / (float)0x80000000u;

        b0 = 0.99886f*b0 + w*0.0555179f;
        b1 = 0.99332f*b1 + w*0.0750759f;
        b2 = 0.96900f*b2 + w*0.1538520f;
        b3 = 0.86650f*b3 + w*0.3104856f;
        b4 = 0.55000f*b4 + w*0.5329522f;
        b5 = -0.7616f*b5 - w*0.0168980f;
        float pink = b0+b1+b2+b3+b4+b5+b6 + w*0.5362f;
        b6 = w * 0.115926f;
        return pink * 0.11f;   // normalise to approx -1..1
    }
};

// ----------------------------------------------------------------
// Sine oscillator
// ----------------------------------------------------------------
struct Sine {
    float phase = 0.f;
    float next(float freq, float sr)
    {
        float v = std::sinf(phase);
        phase += k2Pi * freq / sr;
        if (phase > k2Pi) phase -= k2Pi;
        return v * 0.20f;
    }
};

// ----------------------------------------------------------------
// Per-object state (audio generator + orbital path)
// ----------------------------------------------------------------
enum class ObjKind { PinkContinuous, PinkIntermittent, LowTone, HighTone };

struct ObjState {
    ObjKind kind;
    PinkNoise pink;
    Sine      sine;

    // Intermittent burst state
    float burstTimer  = 0.f;   // seconds elapsed in current phase
    bool  burstActive = true;  // currently in the "on" phase
    float burstOn     = 0.9f;  // seconds of sound
    float burstOff    = 1.3f;  // seconds of silence

    // Orbital params
    float speed      = 0.f;   // rad/s
    float radius     = 1.f;
    float height     = 0.f;
    float phaseOff   = 0.f;   // initial angle offset
    float tiltFreq   = 0.f;   // figure-eight tilt rate
    float tiltAmp    = 0.f;   // figure-eight tilt amplitude

    float angle      = 0.f;   // accumulated angle (updated each frame)

    void init(ObjKind k, float spd, float r, float h,
              float poff, float tf = 0.f, float ta = 0.f)
    {
        kind = k; speed = spd; radius = r; height = h;
        phaseOff = poff; tiltFreq = tf; tiltAmp = ta;
        angle = poff;
    }

    // Advance by dt seconds; writes frameCount float32 samples into buf.
    // Returns current 3D position in x/y/z.
    void tick(float* buf, UINT32 frames, float sr, float dt,
              float& ox, float& oy, float& oz)
    {
        angle += speed * dt;
        if (angle > k2Pi) angle -= k2Pi;

        ox = radius * std::cosf(angle);
        oz = radius * std::sinf(angle);
        oy = height + tiltAmp * std::sinf(tiltFreq * angle);

        float vol = 1.f;
        if (kind == ObjKind::PinkIntermittent) {
            burstTimer += dt;
            if (burstActive && burstTimer >= burstOn) {
                burstActive = false; burstTimer = 0.f;
            } else if (!burstActive && burstTimer >= burstOff) {
                burstActive = true;  burstTimer = 0.f;
            }
            vol = burstActive ? 1.f : 0.f;
        }

        for (UINT32 i = 0; i < frames; ++i) {
            switch (kind) {
            case ObjKind::PinkContinuous:
            case ObjKind::PinkIntermittent:
                buf[i] = pink.next() * vol;
                break;
            case ObjKind::LowTone:
                buf[i] = sine.next(110.f, sr);
                break;
            case ObjKind::HighTone:
                buf[i] = sine.next(880.f, sr);
                break;
            }
        }
    }
};

static ObjState gObjs[4];

static void InitObjects()
{
    // [0] Continuous pink noise: slow wide orbit, slightly elevated
    gObjs[0].init(ObjKind::PinkContinuous,
        /*speed*/0.40f, /*r*/1.0f, /*h*/0.3f, /*phase*/0.f);

    // [1] Intermittent pink noise: medium orbit, opposite direction
    gObjs[1].init(ObjKind::PinkIntermittent,
        /*speed*/-0.70f, /*r*/1.0f, /*h*/-0.2f, /*phase*/kPi);
    gObjs[1].burstOn  = 0.9f;
    gObjs[1].burstOff = 1.3f;

    // [2] Low tone (110 Hz): figure-eight path (Lissajous)
    gObjs[2].init(ObjKind::LowTone,
        /*speed*/0.55f, /*r*/1.0f, /*h*/0.f,
        /*phase*/kPi*0.5f, /*tiltFreq*/2.0f, /*tiltAmp*/0.5f);

    // [3] High tone (880 Hz): fast tight circle overhead
    gObjs[3].init(ObjKind::HighTone,
        /*speed*/1.40f, /*r*/1.0f, /*h*/0.6f, /*phase*/kPi*1.5f);
}

// ----------------------------------------------------------------
// MSSAPI: obtain ISpatialAudioClient from the default render device
// ----------------------------------------------------------------
static HRESULT CreateMSSAPIClient(ComPtr<ISpatialAudioClient>& out)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        fprintf(stderr,
            "  CoCreateInstance(MMDeviceEnumerator): 0x%08X (%s)\n",
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

    hr = device->Activate(__uuidof(ISpatialAudioClient),
        CLSCTX_INPROC_SERVER, nullptr,
        reinterpret_cast<void**>(out.GetAddressOf()));

    if (FAILED(hr)) {
        fprintf(stderr,
            "  IMMDevice::Activate(ISpatialAudioClient): 0x%08X (%s)\n\n"
            "  CAUSE: No spatial sound provider is active.\n"
            "  FIX:   Right-click the speaker tray icon -> Spatial sound\n"
            "         and select Windows Sonic, Dolby Atmos, or our provider.\n"
            "         Then retry: BasicExample.exe --mode mssapi\n\n"
            "  The direct mode works without registration:\n"
            "    BasicExample.exe --mode openal\n",
            (unsigned)hr, HRName(hr));
    }
    return hr;
}

// ----------------------------------------------------------------
// Disable console QuickEdit so clicking the window doesn't freeze
// the process (Windows pauses output when the user starts a selection)
// ----------------------------------------------------------------
static void DisableConsoleQuickEdit()
{
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(hStdin, &mode)) return;
    // ENABLE_QUICK_EDIT_MODE = 0x0040, ENABLE_EXTENDED_FLAGS = 0x0080
    mode &= ~0x0040u;
    mode |=  0x0080u;
    SetConsoleMode(hStdin, mode);
}

// ----------------------------------------------------------------
// One BeginUpdating / EndUpdating cycle, shared by both modes
// ----------------------------------------------------------------
static bool DoFrame(ISpatialAudioObjectRenderStream* stream,
                    ISpatialAudioObject* rawPtrs[4],
                    float sr, float dt,
                    UINT32 iter, HRESULT& loopHR)
{
    UINT32 available = 0, frameCount = 0;
    loopHR = stream->BeginUpdatingAudioObjects(&available, &frameCount);
    if (FAILED(loopHR)) return false;

    if (iter % 100 == 0)
        printf("[Loop %4u] available=%u frameCount=%u\n",
            iter, available, frameCount);

    for (int i = 0; i < 4; ++i) {
        if (!rawPtrs[i]) continue;
        BOOL active = FALSE;
        rawPtrs[i]->IsActive(&active);
        if (!active) continue;

        BYTE* buf = nullptr; UINT32 bufLen = 0;
        if (FAILED(rawPtrs[i]->GetBuffer(&buf, &bufLen)) || !buf) continue;

        float ox=0, oy=0, oz=0;
        gObjs[i].tick(reinterpret_cast<float*>(buf),
                      frameCount, sr, dt, ox, oy, oz);
        rawPtrs[i]->SetPosition(ox, oy, oz);
    }

    loopHR = stream->EndUpdatingAudioObjects();
    return SUCCEEDED(loopHR);
}

// ----------------------------------------------------------------
// OpenAL render loop -- Sleep-based with 1ms timer resolution
// ----------------------------------------------------------------
static int RunOpenALLoop(
    ISpatialAudioObjectRenderStream* stream,
    ISpatialAudioObject* rawPtrs[4],
    const WAVEFORMATEX& wfx,
    ISpatialAudioClient* client)
{
    UINT32 framesPerBuffer = 0;
    client->GetMaxFrameCount(&wfx, &framesPerBuffer);
    const float sr = (float)wfx.nSamplesPerSec;
    const float dt = (float)framesPerBuffer / sr;

    std::atomic<bool> stopFlag{ false };
    std::thread([&stopFlag] {
        (void)getchar();
        stopFlag.store(true, std::memory_order_release);
    }).detach();

    timeBeginPeriod(1);   // 1ms Sleep granularity

    UINT32 iter = 0;
    HRESULT loopHR = S_OK;
    while (!stopFlag.load(std::memory_order_acquire)) {
        if (!DoFrame(stream, rawPtrs, sr, dt, iter, loopHR)) {
            if (IsDeviceGone(loopHR))
                printf("\n[Info] Spatial audio was turned off (hr=0x%08X). Stopping.\n",
                    (unsigned)loopHR);
            break;
        }
        ++iter;
        Sleep((DWORD)(dt * 1000.f));
    }

    timeEndPeriod(1);

    printf("\n[Exit] Ran %u iterations. Final HR: 0x%08X (%s)\n\n",
        iter, (unsigned)loopHR, HRName(loopHR));
    return 0;
}

// ----------------------------------------------------------------
// MSSAPI render loop -- event-driven (WaitForSingleObject)
// The real ISpatialAudioClient requires a non-null EventHandle and
// signals it when it is ready for each new frame.
// ----------------------------------------------------------------
static int RunMSSAPILoop(
    ISpatialAudioObjectRenderStream* stream,
    ISpatialAudioObject* rawPtrs[4],
    const WAVEFORMATEX& wfx,
    HANDLE hEvent)
{
    const float sr = (float)wfx.nSamplesPerSec;
    float dt = 480.f / sr;    // initial estimate; updated each frame

    std::atomic<bool> stopFlag{ false };
    std::thread([&stopFlag] {
        (void)getchar();
        stopFlag.store(true, std::memory_order_release);
    }).detach();

    UINT32 iter = 0;
    HRESULT loopHR = S_OK;
    while (!stopFlag.load(std::memory_order_acquire)) {
        DWORD w = WaitForSingleObject(hEvent, 200);
        if (w == WAIT_TIMEOUT) continue;
        if (w != WAIT_OBJECT_0) break;

        // Update dt from actual frameCount on first few frames
        UINT32 available = 0, frameCount = 0;
        loopHR = stream->BeginUpdatingAudioObjects(&available, &frameCount);
        if (FAILED(loopHR)) {
            if (IsDeviceGone(loopHR))
                printf("\n[Info] Spatial audio was turned off (hr=0x%08X). Stopping.\n",
                    (unsigned)loopHR);
            break;
        }
        if (frameCount > 0) dt = (float)frameCount / sr;

        if (iter % 100 == 0)
            printf("[Loop %4u] available=%u frameCount=%u\n",
                iter, available, frameCount);

        for (int i = 0; i < 4; ++i) {
            if (!rawPtrs[i]) continue;
            BOOL active = FALSE;
            rawPtrs[i]->IsActive(&active);
            if (!active) continue;
            BYTE* buf = nullptr; UINT32 bufLen = 0;
            if (FAILED(rawPtrs[i]->GetBuffer(&buf, &bufLen)) || !buf) continue;
            float ox=0,oy=0,oz=0;
            gObjs[i].tick(reinterpret_cast<float*>(buf),
                          frameCount, sr, dt, ox, oy, oz);
            rawPtrs[i]->SetPosition(ox, oy, oz);
        }

        loopHR = stream->EndUpdatingAudioObjects();
        if (FAILED(loopHR)) {
            if (IsDeviceGone(loopHR))
                printf("\n[Info] Spatial audio was turned off (hr=0x%08X). Stopping.\n",
                    (unsigned)loopHR);
            break;
        }
        ++iter;
    }

    printf("\n[Exit] Ran %u iterations. Final HR: 0x%08X (%s)\n\n",
        iter, (unsigned)loopHR, HRName(loopHR));
    return 0;
}

// ----------------------------------------------------------------
// Shared stream setup
// ----------------------------------------------------------------
static int SetupAndRun(ISpatialAudioClient* client,
                        const WAVEFORMATEX& wfx, Mode mode)
{
    UINT32 framesPerBuffer = 0;
    if (!CHECK(client->GetMaxFrameCount(&wfx, &framesPerBuffer))) return 1;
    printf("  Frames/buffer: %u  (%.1f ms)\n\n",
        framesPerBuffer, framesPerBuffer * 1000.0 / wfx.nSamplesPerSec);

    printf("[Step 4] Activating render stream...\n");

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
    sp.EventHandle           = hEvent;
    sp.NotifyObject          = nullptr;

    PROPVARIANT pv; PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize    = sizeof(sp);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&sp);

    ComPtr<ISpatialAudioObjectRenderStream> stream;
    HRESULT activateHR = client->ActivateSpatialAudioStream(
        &pv, __uuidof(ISpatialAudioObjectRenderStream),
        reinterpret_cast<void**>(stream.GetAddressOf()));

    if (FAILED(activateHR)) {
        // E_INVALIDARG (0x80070057) from the real Windows Spatial Audio stack
        // almost always means spatial audio is turned off on the device --
        // the ISpatialAudioClient was obtained but the stream cannot be opened
        // because no spatializer is active.
        if (activateHR == E_INVALIDARG && mode == Mode::MSSAPI) {
            fprintf(stderr,
                "  [--]  ActivateSpatialAudioStream: Spatial audio is off.\n\n"
                "  The Windows Spatial Audio stack returned E_INVALIDARG because\n"
                "  no spatial sound provider is currently active on the device.\n\n"
                "  To fix:\n"
                "    Right-click the speaker tray icon -> Spatial sound\n"
                "    and select Windows Sonic for Headphones, Dolby Atmos,\n"
                "    or our registered OpenAL provider.\n\n"
                "  Then retry: BasicExample.exe --mode mssapi\n\n"
                "  The direct OpenAL mode works without any spatial sound\n"
                "  provider active:\n"
                "    BasicExample.exe --mode openal\n");
        } else {
            fprintf(stderr,
                "  [FAIL] ActivateSpatialAudioStream  hr=0x%08X (%s)\n",
                (unsigned)activateHR, HRName(activateHR));
        }
        if (hEvent) CloseHandle(hEvent);
        return 1;
    }
    printf("  [OK]   ActivateSpatialAudioStream\n");
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
        if (!CHK(hr, tag)) {
            stream->Stop(); if (hEvent) CloseHandle(hEvent); return 1;
        }
        rawPtrs[i] = objPtrs[i].Get();
    }
    printf("\n");

    if (mode == Mode::OpenAL) {
        ComPtr<OpenALSpatial::IOpenALSpatialAudioClient> ext;
        ComPtr<IUnknown>(client).As(&ext);
        if (ext) {
            printf("[Step 7] Applying per-object spatial params...\n");
            for (int i = 0; i < 4; ++i) {
                OpenALSpatial::ObjectSpatialParams osp;
                osp.referenceDistance = 1.0f;
                osp.maxDistance       = 5.0f;
                osp.rolloffFactor     = 1.0f;
                osp.distModel =
                    OpenALSpatial::DistanceModel::InverseDistanceClamped;
                ext->SetObjectSpatialParams(rawPtrs[i], osp);
            }
            printf("  [OK]   Extended params applied.\n\n");
        }
    }

    printf("Stream running.\n");
    printf("  [0] Continuous pink noise  -- wide slow orbit\n");
    printf("  [1] Intermittent pink noise -- tight reverse orbit, bursts\n");
    printf("  [2] Low tone (110 Hz)       -- figure-eight path\n");
    printf("  [3] High tone (880 Hz)      -- fast circle overhead\n");
    printf("Press Enter to stop...\n\n");

    int ret;
    if (mode == Mode::OpenAL)
        ret = RunOpenALLoop(stream.Get(), rawPtrs, wfx, client);
    else
        ret = RunMSSAPILoop(stream.Get(), rawPtrs, wfx, hEvent);

    // Drain gracefully
    {
        UINT32 d1=0,d2=0;
        if (SUCCEEDED(stream->BeginUpdatingAudioObjects(&d1, &d2))) {
            for (auto& o : objPtrs) if (o) o->SetEndOfStream(0);
            stream->EndUpdatingAudioObjects();
        }
    }
    Sleep(50);

    HRESULT stopHR = stream->Stop();
    if (FAILED(stopHR) && !IsDeviceGone(stopHR))
        fprintf(stderr, "  [FAIL] stream->Stop()  hr=0x%08X (%s)\n",
            (unsigned)stopHR, HRName(stopHR));
    else if (SUCCEEDED(stopHR))
        printf("  [OK]   stream->Stop()\n");
    else
        printf("  [--]   stream->Stop() -- spatial audio already gone, that's fine.\n");

    for (auto& o : objPtrs) o.Reset();
    if (hEvent) CloseHandle(hEvent);
    return ret;
}

// ----------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------
int main(int argc, char* argv[])
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Disable QuickEdit so clicking the console window doesn't freeze audio
    DisableConsoleQuickEdit();

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
                    "  openal  Direct -> OpenAL Soft (default; no registration needed)\n"
                    "  mssapi  Real Windows Spatial Audio stack (needs active provider)\n",
                    argv[i]);
                CoUninitialize(); return 1;
            }
        }
    }

    InitObjects();

    printf("OpenAL Spatial Audio -- Basic Example\n");
    printf("======================================\n");
    if (mode == Mode::OpenAL) {
        printf("Mode: openal  (direct -> OpenAL Soft, Windows Spatial Audio bypassed)\n");
        printf("Tip:  For the Windows Spatial Audio route, run with --mode mssapi\n");
        printf("      (requires a spatial sound provider active in Sound settings)\n");
    } else {
        printf("Mode: mssapi  (real Windows Spatial Audio stack)\n");
        printf("Tip:  For the direct OpenAL Soft route, run with --mode openal\n");
        printf("      (works without any spatial sound provider registered)\n");
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
                "  Ensure openal32.dll or soft_oal.dll is on PATH or beside this exe.\n");
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
                for (UINT32 k = 0; k < n; ++k, p += wcslen(p)+1)
                    wprintf(L"    [%u] %s\n", k, p);
                CoTaskMemFree(names);
            }
        }
        printf("\n");

    } else {
        printf("[Step 1] Creating ISpatialAudioClient via Windows Spatial Audio...\n");
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
