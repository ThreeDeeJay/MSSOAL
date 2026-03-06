# OpenAL Spatial Audio Renderer

A drop-in COM-compatible replacement for Microsoft's **ISpatialAudioClient**
(Windows Sonic for Headphones) that routes all audio objects through
**OpenAL Soft's true per-object HRTF engine** — giving genuine 3D object-based
audio rather than channel-based virtualizer upmixing.

---

## Why this exists

| Feature | Windows Sonic | This renderer |
|---|---|---|
| Rendering approach | Channel-bed virtualizer | True object-based HRTF |
| HRTF processing | Per-channel (7.1 → headphones) | Per-source FIR convolution |
| Dynamic objects | Yes (ISpatialAudioClient) | Yes (same COM API) |
| Max simultaneous sources | ~32 | **256** |
| HRTF dataset | Microsoft proprietary | MIT KEMAR + **SOFA files** |
| Doppler effect | No | Yes |
| EFX reverb | No | Yes (EAX Reverb) |
| Per-source distance model | Limited | Full (InverseDistanceClamped etc.) |
| Source cone / directivity | No | Yes |
| Listener velocity (Doppler) | No | Yes |
| API compatibility | Full ISpatialAudioClient | **Full ISpatialAudioClient** |
| Hot-swap HRTF dataset | No | Yes |

---

## Architecture

```
 Application
     │
     │  Uses ISpatialAudioClient COM interface (standard MS API)
     ▼
 OpenALSpatialAudioClientImpl          ← openal_spatial.dll
     │
     │  COM-compatible COM object implementing:
     │    • ISpatialAudioClient
     │    • IOpenALSpatialAudioClient  (extensions)
     │
     ▼
 SpatialAudioStreamImpl
     │  ┌─────────────────────────────────────────────────────┐
     │  │ AL Upload Thread (Pro Audio priority)               │
     │  │  • Drains finished AL buffers                       │
     │  │  • Uploads PCM frames from per-object staging area  │
     │  │  • Updates listener orientation                     │
     │  │  • Applies EFX reverb sends                         │
     │  └─────────────────────────────────────────────────────┘
     │
     ▼
 SpatialAudioObjectImpl (one per ISpatialAudioObject)
     │  • One AL source
     │  • AL_SOURCE_SPATIALIZE_SOFT = AL_TRUE  ← per-source HRTF
     │  • Per-source distance model
     │  • Ring of streaming AL buffers
     ▼
 OpenAL Soft (soft_oal.dll / openal32.dll)
     │  • HRTF FIR convolution per source
     │  • MIT KEMAR built-in or custom SOFA dataset
     │  • EAX Reverb via EFX
     ▼
 WASAPI → Audio hardware → Headphones
```

---

## Requirements

- **Windows 10** version 1703 or later (Creators Update — ISpatialAudioClient)
- **OpenAL Soft 1.21+** ([releases](https://github.com/kcat/openal-soft/releases))
  - The 64-bit `soft_oal.dll` or `openal32.dll` must be on `PATH` or beside your app
- **Visual Studio 2019+** with MSVC or Clang-CL, C++17
- **CMake 3.20+**

---

## Build

### With vcpkg (recommended)

```powershell
vcpkg install openal-soft:x64-windows
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Manual OpenAL path

```powershell
cmake -B build -DOPENAL_ROOT="C:\OpenAL"
cmake --build build --config Release
```

---

## Usage

### Option A — Direct drop-in (no registration)

Replace your `ISpatialAudioClient` creation with:

```cpp
#include "OpenALSpatialAudioClient.h"

// Configure HRTF
OpenALSpatial::HRTFConfig hrtfCfg;
hrtfCfg.mode         = OpenALSpatial::HRTFMode::Default;   // MIT KEMAR
hrtfCfg.enableReverb = true;
hrtfCfg.reverbMix    = 0.10f;

// Create — returns ComPtr<ISpatialAudioClient>
auto client = OpenALSpatial::CreateClient(hrtfCfg);

// Use exactly like ISpatialAudioClient from here on
```

Everything else in your application stays the same — `ActivateSpatialAudioStream`,
`BeginUpdatingAudioObjects`, `SetPosition`, `GetBuffer`, etc., all work identically.

### Option B — Register as a Windows Spatial Sound provider

Run the registration tool as Administrator to make your renderer appear in the
Windows Sound control panel alongside Windows Sonic:

```powershell
# Register
.\RegisterProvider.exe register "C:\MyApp\openal_spatial.dll"

# Verify
.\RegisterProvider.exe list

# Unregister (restores Windows Sonic)
.\RegisterProvider.exe unregister
```

After registration, users can select "OpenAL Soft 3D Audio (HRTF)" in:
- **Settings → System → Sound → App volume and device preferences**
- **System tray speaker icon → Spatial sound**

### SOFA HRTF datasets

```cpp
OpenALSpatial::HRTFConfig cfg;
cfg.mode     = OpenALSpatial::HRTFMode::SOFA;
cfg.sofaPath = L"C:\\HRTFs\\IRC_1002_R.sofa";
auto client  = OpenALSpatial::CreateClient(cfg);
```

Free SOFA datasets:
- **ARI** — [sofahoo.com](http://sofacoustics.org/data/database/ari/)
- **SADIE II** — [York University](https://www.york.ac.uk/sadie-project/database.html)
- **3D3A** — [Princeton](https://www.princeton.edu/3D3A/HRTFMeasurements.html)

---

## Extended API (`IOpenALSpatialAudioClient`)

Query the extended interface for features beyond ISpatialAudioClient:

```cpp
ComPtr<OpenALSpatial::IOpenALSpatialAudioClient> ext;
client.As(&ext);

// Hot-swap HRTF dataset
UINT32 count = 0; LPWSTR names;
ext->EnumerateHRTFDatasets(&count, &names);
ext->SetHRTFDataset(2);   // Switch to dataset index 2

// Per-object distance & cone parameters
OpenALSpatial::ObjectSpatialParams sp;
sp.referenceDistance = 1.5f;
sp.rolloffFactor     = 2.0f;
sp.coneInnerAngle    = 60.0f;   // 60° directional cone
sp.coneOuterAngle    = 120.0f;
sp.coneOuterGain     = 0.1f;
ext->SetObjectSpatialParams(obj.Get(), sp);

// Direct AL context access for custom effects
ALCcontext* ctx = ext->GetALCContext();
```

---

## OpenAL Soft configuration

Copy `alsoft.ini` to `%APPDATA%\alsoft.ini` to tune quality settings:

```ini
[general]
frequency    = 48000
stereo-mode  = hrtf    # Force HRTF output
resampler    = bsinc24 # High-quality resampler
sources      = 256

[hrtf]
hrtf         = true
hrtf-mode    = bsinc24
ambi-order   = 3       # Higher = more accurate, more CPU
```

---

## How HRTF differs from Windows Sonic

Windows Sonic processes audio by:
1. Summing all dynamic objects into a 7.1 channel bed
2. Applying one pair of HRTF filters per channel (8 sources total)

This renderer processes audio by:
1. Keeping each object on its own dedicated AL source
2. Applying a **separate, unique HRTF filter pair per source**
3. The FIR convolution encodes the exact elevation and azimuth of each object

The difference is audible: objects placed above or below the listener are
correctly perceived as such, and two objects at the same azimuth but different
elevations remain distinguishable — something channel-based virtualizers cannot do.

---

## Thread safety

| Operation | Thread-safe? |
|---|---|
| `CreateClient()` | ✓ |
| `ActivateSpatialAudioStream()` | ✓ |
| `BeginUpdatingAudioObjects()` / `EndUpdatingAudioObjects()` | ✓ (single caller) |
| `SetPosition()` / `SetVolume()` | ✓ (from update thread) |
| `SetListenerOrientation()` | ✓ (any thread) |
| `SetObjectSpatialParams()` | ✓ (any thread) |
| `SetHRTFDataset()` | ✓ (stalls briefly) |

---

## License

GPLv3 License — see LICENSE file.
OpenAL Soft is distributed under the LGPL 2.1.
