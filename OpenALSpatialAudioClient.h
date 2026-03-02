#pragma once
/**
 * OpenALSpatialAudioClient.h
 * ============================================================
 * Drop-in replacement for Microsoft ISpatialAudioClient that
 * routes every audio object through OpenAL Soft's true HRTF
 * engine instead of Windows Sonic's channel-based virtualizer.
 *
 * Supports:
 *  - Per-object 3D positioning via AL_EXT_SOURCE_DISTANCE_MODEL
 *  - SOFA-based HRTF datasets (OpenAL Soft >= 1.21)
 *  - Up to 256 simultaneous dynamic audio objects
 *  - Full ISpatialAudioClient / ISpatialAudioObjectRenderStream
 *    / ISpatialAudioObject COM interface compliance
 *  - Optional registration as a Windows Spatial Sound provider
 *    (see register/RegisterProvider.cpp)
 *
 * Build requirements:
 *  - Windows 10 1703+ SDK  (for ISpatialAudioClient headers)
 *  - OpenAL Soft 1.21+     (soft_oal.dll or openal32.dll)
 *  - MSVC 2019+ or Clang-CL with /std:c++17
 *
 * Usage (direct / without COM registration):
 *   auto client = OpenALSpatial::CreateClient();
 *   // use exactly like ISpatialAudioClient
 * ============================================================
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>
#include <spatialaudioclient.h>   // Windows SDK – ISpatialAudioClient
#include <audiosessiontypes.h>
#include <mmreg.h>
#include <wrl/client.h>

// OpenAL Soft headers (must come after Windows headers)
#include <al.h>
#include <alc.h>
#include <alext.h>      // ALC_HRTF_SOFT, AL_SOURCE_SPATIALIZE_SOFT, etc.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OpenALSpatial {

// ─────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────
constexpr UINT32 kMaxDynamicObjects = 256;
constexpr UINT32 kDefaultFramesPerBuffer = 480;   // 10 ms @ 48 kHz
constexpr UINT32 kDefaultSampleRate = 48000;
constexpr UINT32 kNumStreamingBuffers = 4;        // ring-buffer depth

// ─────────────────────────────────────────────────────────────
// HRTF configuration
// ─────────────────────────────────────────────────────────────
enum class HRTFMode {
    Default,       // Use OpenAL Soft's built-in MIT KEMAR dataset
    SOFA,          // Load a SOFA file (set sofaPath)
    Disabled       // Panning-only, no HRTF convolution
};

struct HRTFConfig {
    HRTFMode mode     = HRTFMode::Default;
    std::wstring sofaPath;          // Only used when mode == SOFA
    float headRadius  = 0.09f;      // metres – used for ITD model
    bool  enableReverb = true;      // EFX room-scale reverb
    float reverbMix    = 0.15f;
};

// ─────────────────────────────────────────────────────────────
// Distance attenuation model (per object overrideable)
// ─────────────────────────────────────────────────────────────
enum class DistanceModel {
    None,
    InverseDistance,
    InverseDistanceClamped,   // Default – mirrors real-world
    LinearDistance,
    LinearDistanceClamped,
    ExponentDistance,
    ExponentDistanceClamped
};

// ─────────────────────────────────────────────────────────────
// Per-object spatial parameters (extended beyond MS API)
// ─────────────────────────────────────────────────────────────
struct ObjectSpatialParams {
    float x = 0.f, y = 0.f, z = 0.f;   // Metres, right-handed Y-up
    float velocityX = 0.f, velocityY = 0.f, velocityZ = 0.f;
    float gain          = 1.f;
    float rolloffFactor = 1.f;
    float referenceDistance = 1.f;
    float maxDistance       = 1000.f;
    float coneInnerAngle    = 360.f;    // Degrees
    float coneOuterAngle    = 360.f;
    float coneOuterGain     = 0.f;
    DistanceModel distModel = DistanceModel::InverseDistanceClamped;
    bool  spatializeOverride = true;    // Force per-object HRTF even if
                                        // global HRTF is disabled
};

// ─────────────────────────────────────────────────────────────
// Factory — creates a fully initialised client
// ─────────────────────────────────────────────────────────────
Microsoft::WRL::ComPtr<ISpatialAudioClient> CreateClient(
    const HRTFConfig& cfg   = {},
    const std::wstring& deviceId = L""  // empty = default output device
);

// Extended interface for features beyond ISpatialAudioClient
MIDL_INTERFACE("8E4B5A4E-2F1A-4D3C-B6A8-7C9F0E1D2345")
IOpenALSpatialAudioClient : public ISpatialAudioClient
{
    // Override spatial params for a specific object after creation
    virtual HRESULT STDMETHODCALLTYPE SetObjectSpatialParams(
        ISpatialAudioObject* obj,
        const ObjectSpatialParams& params) = 0;

    // Query the active HRTF dataset name
    virtual HRESULT STDMETHODCALLTYPE GetActiveHRTFName(
        LPWSTR buffer, UINT32 bufferLen) = 0;

    // Enumerate available HRTF datasets on this device
    virtual HRESULT STDMETHODCALLTYPE EnumerateHRTFDatasets(
        UINT32* count, LPWSTR* namesOut) = 0;

    // Hot-swap the HRTF dataset without stopping streams
    virtual HRESULT STDMETHODCALLTYPE SetHRTFDataset(
        UINT32 index) = 0;

    // Direct OpenAL device/context access for power users
    virtual ALCdevice*  STDMETHODCALLTYPE GetALCDevice()  = 0;
    virtual ALCcontext* STDMETHODCALLTYPE GetALCContext() = 0;
};

// ─────────────────────────────────────────────────────────────
// Listener orientation helper (call once per frame)
// ─────────────────────────────────────────────────────────────
struct ListenerOrientation {
    // Forward vector (normalized)
    float fwdX = 0.f, fwdY = 0.f, fwdZ = -1.f;
    // Up vector (normalized)
    float upX  = 0.f, upY  = 1.f, upZ  =  0.f;
    // Position in metres
    float posX = 0.f, posY = 0.f, posZ =  0.f;
    // Velocity (for Doppler)
    float velX = 0.f, velY = 0.f, velZ =  0.f;
    float masterGain = 1.f;
};

} // namespace OpenALSpatial
