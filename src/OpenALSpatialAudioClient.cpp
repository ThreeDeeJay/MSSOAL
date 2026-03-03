/**
 * OpenALSpatialAudioClient.cpp
 * ============================================================
 * COM implementation of ISpatialAudioClient routed through
 * OpenAL Soft for true per-object HRTF 3D audio.
 * ============================================================
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>
#include <initguid.h>

#include <al.h>
#include <alc.h>
#include <alext.h>

#include <algorithm>
#include <cassert>
#include <codecvt>
#include <locale>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../include/OpenALSpatialAudioClient.h"
#include "SpatialAudioStream.h"

// - Logging helper -
#ifndef NDEBUG
#include <sstream>
#define OAL_LOG(msg) \
    do { \
        std::wostringstream _ss; \
        _ss << L"[OpenALSpatial] " << msg << L"\n"; \
        OutputDebugStringW(_ss.str().c_str()); \
    } while(0)
#else
#define OAL_LOG(msg) (void)0
#endif

// - Error helpers -
#define OAL_CHECK(expr) \
    do { \
        ALenum _e = alGetError(); \
        (void)(expr); \
        _e = alGetError(); \
        if (_e != AL_NO_ERROR) { \
            OAL_LOG(L"AL error " << _e << L" after " #expr); \
            return E_FAIL; \
        } \
    } while(0)

#define HR(hr) do { HRESULT _hr = (hr); if (FAILED(_hr)) return _hr; } while(0)

namespace OpenALSpatial {

// -
// OpenALSpatialAudioClientImpl
// Full ISpatialAudioClient + IOpenALSpatialAudioClient COM object
// -
class OpenALSpatialAudioClientImpl final : public IOpenALSpatialAudioClient
{
public:
    OpenALSpatialAudioClientImpl() = default;

    // - COM boilerplate -
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(ISpatialAudioClient) ||
            riid == __uuidof(IOpenALSpatialAudioClient))
        {
            *ppv = static_cast<IOpenALSpatialAudioClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    // - ISpatialAudioClient -
    STDMETHODIMP GetStaticObjectPosition(
        AudioObjectType type, float* x, float* y, float* z) override;

    STDMETHODIMP GetNativeStaticObjectTypeMask(
        AudioObjectType* mask) override;

    STDMETHODIMP GetMaxDynamicObjectCount(UINT32* value) override;

    STDMETHODIMP GetSupportedAudioObjectFormatEnumerator(
        IAudioFormatEnumerator** enumerator) override;

    STDMETHODIMP GetMaxFrameCount(
        const WAVEFORMATEX* objectFormat,
        UINT32* frameCountPerBuffer) override;

    STDMETHODIMP IsAudioObjectFormatSupported(
        const WAVEFORMATEX* objectFormat) override;

    STDMETHODIMP IsSpatialAudioStreamAvailable(
        REFIID streamUuid,
        const PROPVARIANT* params) override;

    STDMETHODIMP ActivateSpatialAudioStream(
        const PROPVARIANT* activationParams,
        REFIID riid,
        void** stream) override;

    // - IOpenALSpatialAudioClient (extensions) -
    STDMETHODIMP SetObjectSpatialParams(
        ISpatialAudioObject* obj,
        const ObjectSpatialParams& params) override;

    STDMETHODIMP GetActiveHRTFName(
        LPWSTR buffer, UINT32 bufferLen) override;

    STDMETHODIMP EnumerateHRTFDatasets(
        UINT32* count, LPWSTR* namesOut) override;

    STDMETHODIMP SetHRTFDataset(UINT32 index) override;

    ALCdevice*  STDMETHODCALLTYPE GetALCDevice()  override { return device_; }
    ALCcontext* STDMETHODCALLTYPE GetALCContext() override { return ctx_; }

    // - Internal init -
    HRESULT Init(const HRTFConfig& cfg, const std::wstring& deviceId);

private:
    ~OpenALSpatialAudioClientImpl()
    {
        if (ctx_) {
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(ctx_);
        }
        if (device_) alcCloseDevice(device_);
        OAL_LOG(L"Client destroyed");
    }

    HRESULT OpenALDevice(const std::wstring& deviceId);
    HRESULT ConfigureHRTF(const HRTFConfig& cfg);
    bool    CheckALExtension(const char* ext);
    bool    cfg_wants_hrtf() const;

    std::atomic<ULONG> refCount_{1};

    ALCdevice*  device_ = nullptr;
    ALCcontext* ctx_    = nullptr;

    HRTFConfig hrtfCfg_;
    UINT32     sampleRate_   = kDefaultSampleRate;
    UINT32     framesPerBuf_ = kDefaultFramesPerBuffer;

    // Strong reference - keeps the stream alive for the lifetime of the client.
    // A weak_ptr was wrong here: the local shared_ptr in ActivateSpatialAudioStream
    // would drop to refcount 0 before the caller could use the returned pointer.
    std::shared_ptr<SpatialAudioStreamImpl> activeStream_;

    // Enumerated HRTF names (populated in ConfigureHRTF)
    std::vector<std::string> hrtfNames_;

    OpenALSpatialAudioClientImpl(const OpenALSpatialAudioClientImpl&) = delete;
    OpenALSpatialAudioClientImpl& operator=(const OpenALSpatialAudioClientImpl&) = delete;
};

// -
// Init
// -
HRESULT OpenALSpatialAudioClientImpl::Init(
    const HRTFConfig& cfg, const std::wstring& deviceId)
{
    hrtfCfg_ = cfg;
    HR(OpenALDevice(deviceId));
    HR(ConfigureHRTF(cfg));
    OAL_LOG(L"Client initialised - device=" << deviceId);
    return S_OK;
}

// -
// OpenAL device + context creation
// -
HRESULT OpenALSpatialAudioClientImpl::OpenALDevice(const std::wstring& deviceId)
{
    // Convert device ID to narrow string for ALC
    std::string narrowId;
    if (!deviceId.empty()) {
        int len = WideCharToMultiByte(CP_UTF8, 0,
            deviceId.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            narrowId.resize(len);
            WideCharToMultiByte(CP_UTF8, 0,
                deviceId.c_str(), -1, &narrowId[0], len, nullptr, nullptr);
        }
    }

    // Use enumerate-all extension to find matching device
    const ALCchar* deviceName = narrowId.empty() ? nullptr : narrowId.c_str();

    // Prefer WASAPI exclusive-mode via "OpenAL Soft on <device>"
    // OpenAL Soft 1.21+ will select the Windows audio endpoint
    device_ = alcOpenDevice(deviceName);
    if (!device_) {
        OAL_LOG(L"alcOpenDevice failed - falling back to default");
        device_ = alcOpenDevice(nullptr);
        if (!device_) {
            OAL_LOG(L"No OpenAL device available");
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
    }

    // Request 48 kHz context with HRTF and large enough output
    ALCint attrs[] = {
        ALC_FREQUENCY,      (ALCint)kDefaultSampleRate,
        ALC_HRTF_SOFT,      cfg_wants_hrtf() ? ALC_TRUE : ALC_FALSE,
        ALC_OUTPUT_LIMITER_SOFT, ALC_TRUE,
        0
    };

    ctx_ = alcCreateContext(device_, attrs);
    if (!ctx_) {
        ALCenum err = alcGetError(device_);
        OAL_LOG(L"alcCreateContext failed: " << err);
        (void)err;   // suppress C4189 in Release where OAL_LOG is a no-op
        alcCloseDevice(device_);
        device_ = nullptr;
        return E_FAIL;
    }

    alcMakeContextCurrent(ctx_);

    // Query actual sample rate
    ALCint actualRate = 0;
    alcGetIntegerv(device_, ALC_FREQUENCY, 1, &actualRate);
    sampleRate_ = static_cast<UINT32>(actualRate > 0 ? actualRate : kDefaultSampleRate);
    framesPerBuf_ = sampleRate_ / 100;  // 10 ms

    OAL_LOG(L"OpenAL device opened at " << sampleRate_ << L" Hz");
    return S_OK;
}

bool OpenALSpatialAudioClientImpl::cfg_wants_hrtf() const
{
    return hrtfCfg_.mode != HRTFMode::Disabled;
}

// -
// HRTF configuration
// -
HRESULT OpenALSpatialAudioClientImpl::ConfigureHRTF(const HRTFConfig& cfg)
{
    if (cfg.mode == HRTFMode::Disabled) {
        OAL_LOG(L"HRTF disabled - using panning only");
        return S_OK;
    }

    // Enumerate available HRTF datasets
    if (alcIsExtensionPresent(device_, "ALC_SOFT_HRTF")) {
        // alcGetStringiSOFT is an extension function -- must be loaded at runtime
        typedef const ALCchar* (ALC_APIENTRY* PFN_alcGetStringiSOFT)(
            ALCdevice*, ALCenum, ALCsizei);
        PFN_alcGetStringiSOFT alcGetStringiSOFT =
            reinterpret_cast<PFN_alcGetStringiSOFT>(
                alcGetProcAddress(device_, "alcGetStringiSOFT"));

        ALCint numHrtf = 0;
        alcGetIntegerv(device_, ALC_NUM_HRTF_SPECIFIERS_SOFT, 1, &numHrtf);
        hrtfNames_.clear();
        if (alcGetStringiSOFT) {
            for (int i = 0; i < numHrtf; ++i) {
                const ALCchar* name = alcGetStringiSOFT(device_,
                    ALC_HRTF_SPECIFIER_SOFT, i);
                if (name) hrtfNames_.push_back(name);
            }
        }
        OAL_LOG(L"Found " << numHrtf << L" HRTF dataset(s)");
    }

    // For SOFA mode, build the alcResetDeviceSOFT attribute set
    if (cfg.mode == HRTFMode::SOFA && !cfg.sofaPath.empty()) {
        // OpenAL Soft loads SOFA files placed in its data directory.
        // We copy the file to the OAL data path if needed.
        std::wstring sofa = cfg.sofaPath;
        wchar_t alDataPath[MAX_PATH] = {};
        // Attempt to find OAL Soft data directory from registry/env
        if (GetEnvironmentVariableW(L"ALSOFT_CONF_DIR",
                alDataPath, MAX_PATH) > 0) {
            std::wstring dest = std::wstring(alDataPath) + L"\\hrtf\\" +
                sofa.substr(sofa.find_last_of(L"/\\") + 1);
            CopyFileW(sofa.c_str(), dest.c_str(), FALSE);
        }
    }

    // Activate HRTF on the context (may be called after context creation
    // via alcResetDeviceSOFT if available)
    if (alcIsExtensionPresent(device_, "ALC_SOFT_HRTF")) {
        LPALCRESETDEVICESOFT alcResetDeviceSOFT =
            reinterpret_cast<LPALCRESETDEVICESOFT>(
                alcGetProcAddress(device_, "alcResetDeviceSOFT"));

        if (alcResetDeviceSOFT) {
            ALCint resetAttrs[] = {
                ALC_HRTF_SOFT, ALC_TRUE,
                ALC_HRTF_ID_SOFT, 0,   // Index 0 = best available / SOFA
                0
            };
            if (!alcResetDeviceSOFT(device_, resetAttrs)) {
                OAL_LOG(L"alcResetDeviceSOFT failed - HRTF may not be active");
            }
        }
    }

    // Verify HRTF is actually enabled
    ALCint hrtfStatus = 0;
    alcGetIntegerv(device_, ALC_HRTF_STATUS_SOFT, 1, &hrtfStatus);
    if (hrtfStatus == ALC_HRTF_ENABLED_SOFT ||
        hrtfStatus == ALC_HRTF_REQUIRED_SOFT) {
        OAL_LOG(L"HRTF active: " <<
            alcGetString(device_, ALC_HRTF_SPECIFIER_SOFT));
    } else {
        OAL_LOG(L"HRTF not active (status=" << hrtfStatus <<
            L") - check OpenAL Soft config");
    }

    // Configure Doppler
    alSpeedOfSound(343.3f);             // m/s
    alDopplerFactor(1.0f);
    alDopplerVelocity(1.0f);

    return S_OK;
}

// -
// ISpatialAudioClient - static object positions
// Mirrors Windows Sonic's standard speaker bed positions
// -
STDMETHODIMP OpenALSpatialAudioClientImpl::GetStaticObjectPosition(
    AudioObjectType type, float* x, float* y, float* z)
{
    if (!x || !y || !z) return E_POINTER;
    // Positions in metres (right-handed, Y-up, -Z forward)
    switch (type) {
    case AudioObjectType_FrontLeft:         *x=-1.0f; *y=0.f; *z=-1.0f; break;
    case AudioObjectType_FrontRight:        *x= 1.0f; *y=0.f; *z=-1.0f; break;
    case AudioObjectType_FrontCenter:       *x= 0.0f; *y=0.f; *z=-1.0f; break;
    case AudioObjectType_LowFrequency:      *x= 0.0f; *y=-0.3f;*z=-1.0f;break;
    case AudioObjectType_SideLeft:          *x=-1.0f; *y=0.f; *z= 0.0f; break;
    case AudioObjectType_SideRight:         *x= 1.0f; *y=0.f; *z= 0.0f; break;
    case AudioObjectType_BackLeft:          *x=-1.0f; *y=0.f; *z= 1.0f; break;
    case AudioObjectType_BackRight:         *x= 1.0f; *y=0.f; *z= 1.0f; break;
    case AudioObjectType_TopFrontLeft:      *x=-1.0f; *y=1.0f; *z=-1.0f;break;
    case AudioObjectType_TopFrontRight:     *x= 1.0f; *y=1.0f; *z=-1.0f;break;
    case AudioObjectType_TopBackLeft:       *x=-1.0f; *y=1.0f; *z= 1.0f;break;
    case AudioObjectType_TopBackRight:      *x= 1.0f; *y=1.0f; *z= 1.0f;break;
    case AudioObjectType_BackCenter:        *x= 0.0f; *y=0.f; *z= 1.0f; break;
#if defined(AudioObjectType_TopFrontCenter)
    case AudioObjectType_TopFrontCenter:    *x= 0.0f; *y=1.0f; *z=-1.0f;break;
    case AudioObjectType_TopBackCenter:     *x= 0.0f; *y=1.0f; *z= 1.0f;break;
#endif
    default: return E_INVALIDARG;
    }
    return S_OK;
}

STDMETHODIMP OpenALSpatialAudioClientImpl::GetNativeStaticObjectTypeMask(
    AudioObjectType* mask)
{
    if (!mask) return E_POINTER;
    // We support the full Atmos bed + all dynamic objects
    *mask = AudioObjectType_FrontLeft   | AudioObjectType_FrontRight  |
            AudioObjectType_FrontCenter | AudioObjectType_LowFrequency |
            AudioObjectType_SideLeft    | AudioObjectType_SideRight    |
            AudioObjectType_BackLeft    | AudioObjectType_BackRight    |
            AudioObjectType_TopFrontLeft| AudioObjectType_TopFrontRight|
            AudioObjectType_TopBackLeft | AudioObjectType_TopBackRight |
            AudioObjectType_BackCenter
#if defined(AudioObjectType_TopFrontCenter)
            | AudioObjectType_TopFrontCenter | AudioObjectType_TopBackCenter
#endif
            ;
    return S_OK;
}

STDMETHODIMP OpenALSpatialAudioClientImpl::GetMaxDynamicObjectCount(UINT32* value)
{
    if (!value) return E_POINTER;
    *value = kMaxDynamicObjects;
    return S_OK;
}

STDMETHODIMP OpenALSpatialAudioClientImpl::GetSupportedAudioObjectFormatEnumerator(
    IAudioFormatEnumerator** enumerator)
{
    // Implement a minimal IAudioFormatEnumerator that returns
    // 32-bit float mono and stereo at the device sample rate.
    if (!enumerator) return E_POINTER;

    // - Inner COM class -
    class FormatEnum final : public IAudioFormatEnumerator {
    public:
        explicit FormatEnum(UINT32 sr)
        {
            // Mono float32
            auto& f0 = fmts_[0];
            f0.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
            f0.nChannels       = 1;
            f0.nSamplesPerSec  = sr;
            f0.wBitsPerSample  = 32;
            f0.nBlockAlign     = 4;
            f0.nAvgBytesPerSec = sr * 4;
            f0.cbSize          = 0;
            // Stereo float32
            auto& f1 = fmts_[1];
            f1 = f0;
            f1.nChannels = 2;
            f1.nBlockAlign = 8;
            f1.nAvgBytesPerSec = sr * 8;
        }
        STDMETHODIMP QueryInterface(REFIID r, void** p) override {
            if (r == __uuidof(IUnknown) ||
                r == __uuidof(IAudioFormatEnumerator)) {
                *p = this; AddRef(); return S_OK;
            }
            *p = nullptr; return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef()  override { return ++rc_; }
        STDMETHODIMP_(ULONG) Release() override {
            auto r = --rc_; if (!r) delete this; return r;
        }
        STDMETHODIMP GetCount(UINT32* c) override {
            if (!c) return E_POINTER; *c = 2; return S_OK;
        }
        STDMETHODIMP GetFormat(UINT32 i, WAVEFORMATEX** f) override {
            if (!f) return E_POINTER;
            if (i >= 2) return E_INVALIDARG;
            *f = &fmts_[i]; return S_OK;
        }
    private:
        std::atomic<ULONG> rc_{1};
        WAVEFORMATEX fmts_[2]{};
    };

    *enumerator = new (std::nothrow) FormatEnum(sampleRate_);
    return *enumerator ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP OpenALSpatialAudioClientImpl::GetMaxFrameCount(
    const WAVEFORMATEX* /*fmt*/, UINT32* frameCount)
{
    if (!frameCount) return E_POINTER;
    *frameCount = framesPerBuf_;
    return S_OK;
}

STDMETHODIMP OpenALSpatialAudioClientImpl::IsAudioObjectFormatSupported(
    const WAVEFORMATEX* fmt)
{
    if (!fmt) return E_POINTER;
    // Accept mono or stereo float32 at any sample rate <= 48 kHz
    bool ok = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) &&
              (fmt->nChannels == 1 || fmt->nChannels == 2) &&
              (fmt->wBitsPerSample == 32) &&
              (fmt->nSamplesPerSec <= 48000);
    // Also accept WAVE_FORMAT_EXTENSIBLE wrapping float32
    if (!ok && fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        fmt->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        ok = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
             (fmt->nChannels <= 2) &&
             (fmt->wBitsPerSample == 32);
    }
    return ok ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
}

STDMETHODIMP OpenALSpatialAudioClientImpl::IsSpatialAudioStreamAvailable(
    REFIID streamUuid, const PROPVARIANT* /*params*/)
{
    // We support ISpatialAudioObjectRenderStream only
    return (streamUuid == __uuidof(ISpatialAudioObjectRenderStream))
        ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
}

// -
// Stream activation - the main entry point applications call
// -
STDMETHODIMP OpenALSpatialAudioClientImpl::ActivateSpatialAudioStream(
    const PROPVARIANT* activationParams,
    REFIID riid,
    void** stream)
{
    if (!stream || !activationParams) return E_POINTER;
    if (riid != __uuidof(ISpatialAudioObjectRenderStream)) return E_NOINTERFACE;

    if (activationParams->vt != VT_BLOB) return E_INVALIDARG;
    if (activationParams->blob.cbSize <
            sizeof(SpatialAudioObjectRenderStreamActivationParams))
        return E_INVALIDARG;

    const auto* p = reinterpret_cast<const
        SpatialAudioObjectRenderStreamActivationParams*>(
            activationParams->blob.pBlobData);

    // Validate format
    HR(IsAudioObjectFormatSupported(p->ObjectFormat));

    auto impl = SpatialAudioStreamImpl::Create(
        device_, ctx_, *p, hrtfCfg_, p->NotifyObject);
    if (!impl) return E_OUTOFMEMORY;

    activeStream_ = impl;

    // Return as ISpatialAudioObjectRenderStream
    auto rawPtr = impl.get();
    rawPtr->AddRef();
    *stream = static_cast<ISpatialAudioObjectRenderStream*>(rawPtr);
    return S_OK;
}

// -
// IOpenALSpatialAudioClient extensions
// -
STDMETHODIMP OpenALSpatialAudioClientImpl::SetObjectSpatialParams(
    ISpatialAudioObject* obj, const ObjectSpatialParams& params)
{
    if (!activeStream_) return AUDCLNT_E_SERVICE_NOT_RUNNING;
    return activeStream_->SetObjectSpatialParams(obj, params);
}

STDMETHODIMP OpenALSpatialAudioClientImpl::GetActiveHRTFName(
    LPWSTR buffer, UINT32 bufferLen)
{
    if (!buffer) return E_POINTER;
    const ALCchar* name = alcGetString(device_, ALC_HRTF_SPECIFIER_SOFT);
    if (!name) { buffer[0] = 0; return S_OK; }
    int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, buffer, (int)bufferLen);
    return (n > 0) ? S_OK : E_FAIL;
}

STDMETHODIMP OpenALSpatialAudioClientImpl::EnumerateHRTFDatasets(
    UINT32* count, LPWSTR* namesOut)
{
    if (!count || !namesOut) return E_POINTER;
    *count = static_cast<UINT32>(hrtfNames_.size());
    if (hrtfNames_.empty()) { *namesOut = nullptr; return S_OK; }

    // Caller frees with CoTaskMemFree on the entire block
    size_t totalChars = 0;
    for (auto& n : hrtfNames_) totalChars += n.size() + 1;
    wchar_t* buf = static_cast<wchar_t*>(
        CoTaskMemAlloc(totalChars * sizeof(wchar_t)));
    if (!buf) return E_OUTOFMEMORY;

    wchar_t* pos = buf;
    for (auto& n : hrtfNames_) {
        int written = MultiByteToWideChar(
            CP_UTF8, 0, n.c_str(), -1, pos, (int)(totalChars));
        pos += written;
    }
    *namesOut = buf;
    return S_OK;
}

STDMETHODIMP OpenALSpatialAudioClientImpl::SetHRTFDataset(UINT32 index)
{
    if (index >= hrtfNames_.size()) return E_INVALIDARG;

    LPALCRESETDEVICESOFT alcResetDeviceSOFT =
        reinterpret_cast<LPALCRESETDEVICESOFT>(
            alcGetProcAddress(device_, "alcResetDeviceSOFT"));
    if (!alcResetDeviceSOFT) return E_NOTIMPL;

    ALCint attrs[] = {
        ALC_HRTF_SOFT,    ALC_TRUE,
        ALC_HRTF_ID_SOFT, (ALCint)index,
        0
    };
    return alcResetDeviceSOFT(device_, attrs) ? S_OK : E_FAIL;
}

// -
// Public factory
// -
Microsoft::WRL::ComPtr<ISpatialAudioClient> CreateClient(
    const HRTFConfig& cfg,
    const std::wstring& deviceId)
{
    auto* impl = new (std::nothrow) OpenALSpatialAudioClientImpl();
    if (!impl) return nullptr;

    HRESULT hr = impl->Init(cfg, deviceId);
    if (FAILED(hr)) {
        impl->Release();
        OAL_LOG(L"CreateClient failed: 0x" << std::hex << hr);
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ISpatialAudioClient> out;
    out.Attach(static_cast<ISpatialAudioClient*>(impl));
    return out;
}

} // namespace OpenALSpatial

// =============================================================================
// COM DLL Server exports
// =============================================================================
// These four exports are what Windows uses to treat openal_spatial.dll as a
// proper COM in-process server.  Without them CoCreateInstance returns
// REGDB_E_CLASSNOTREG and Windows Audio silently skips the provider.
//
// DllGetClassObject  -- called by COM when activating our CLSID
// DllCanUnloadNow    -- called by COM to free the DLL from memory
// DllRegisterServer  -- optional: called by regsvr32 as an alternative to
//                       RegisterProvider.exe
// DllUnregisterServer-- called by regsvr32 /u
// =============================================================================

// - Reference-count for DLL lifetime -
static std::atomic<LONG> g_dllRefCount{ 0 };

// - Class factory -
// IClassFactory that creates OpenALSpatialAudioClientImpl instances.
// Windows Audio calls CreateInstance with IID_ISpatialAudioClient.
class OpenALSpatialClassFactory final : public IClassFactory
{
public:
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    // IClassFactory
    STDMETHODIMP CreateInstance(IUnknown* pOuter,
                                REFIID    riid,
                                void**    ppv) override
    {
        if (!ppv) return E_POINTER;
        if (pOuter) return CLASS_E_NOAGGREGATION;

        // Default HRTF config -- the caller can QI for IOpenALSpatialAudioClient
        // afterwards to customise it, or use OpenALSpatial::CreateClient() directly.
        OpenALSpatial::HRTFConfig cfg;
        cfg.mode         = OpenALSpatial::HRTFMode::Default;
        cfg.enableReverb = false;

        auto* impl = new (std::nothrow) OpenALSpatial::OpenALSpatialAudioClientImpl();
        if (!impl) return E_OUTOFMEMORY;

        HRESULT hr = impl->Init(cfg, L"");
        if (FAILED(hr)) { impl->Release(); return hr; }

        hr = impl->QueryInterface(riid, ppv);
        impl->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) ++g_dllRefCount;
        else      --g_dllRefCount;
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount_{ 1 };
};

// - CLSID supported by this DLL -
// Must match CLSID_OpenALSpatialProvider in RegisterProvider.cpp.
// {9A3B4C5D-6E7F-8901-ABCD-EF1234567890}
DEFINE_GUID(CLSID_OpenALSpatialProviderDll,
    0x9a3b4c5d, 0x6e7f, 0x8901,
    0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90);

// - DLL entry points -

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

// Called by COM runtime when an app calls CoCreateInstance with our CLSID.
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    if (rclsid != CLSID_OpenALSpatialProviderDll) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) OpenALSpatialClassFactory();
    if (!factory) return E_OUTOFMEMORY;

    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

// Called by COM to determine whether the DLL can be freed.
STDAPI DllCanUnloadNow()
{
    return (g_dllRefCount.load() == 0) ? S_OK : S_FALSE;
}

// Called by regsvr32 (optional alternative to RegisterProvider.exe).
// Writes only the COM InProcServer32 entry; RegisterProvider.exe still
// needed for the MMDevices key which requires the LocalSystem service.
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(
            GetModuleHandleW(nullptr), path, MAX_PATH))
        return HRESULT_FROM_WIN32(GetLastError());

    // Attempt to retrieve this DLL's own path
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&DllRegisterServer), &hSelf);
    if (hSelf) GetModuleFileNameW(hSelf, path, MAX_PATH);

    wchar_t clsidStr[64] = {};
    StringFromGUID2(CLSID_OpenALSpatialProviderDll, clsidStr, 64);

    std::wstring keyBase = std::wstring(L"SOFTWARE\\Classes\\CLSID\\")
                         + clsidStr;
    std::wstring keyIP   = keyBase + L"\\InProcServer32";

    HKEY hk = nullptr;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyBase.c_str(),
                 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32((DWORD)r);
    const wchar_t* name = L"OpenAL Soft Spatial Audio Renderer";
    RegSetValueExW(hk, nullptr, 0, REG_SZ,
        (const BYTE*)name, (DWORD)((wcslen(name)+1)*sizeof(wchar_t)));
    RegCloseKey(hk);

    r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyIP.c_str(),
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32((DWORD)r);
    RegSetValueExW(hk, nullptr, 0, REG_SZ,
        (const BYTE*)path, (DWORD)((wcslen(path)+1)*sizeof(wchar_t)));
    const wchar_t* model = L"Both";
    RegSetValueExW(hk, L"ThreadingModel", 0, REG_SZ,
        (const BYTE*)model, (DWORD)((wcslen(model)+1)*sizeof(wchar_t)));
    RegCloseKey(hk);
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    wchar_t clsidStr[64] = {};
    StringFromGUID2(CLSID_OpenALSpatialProviderDll, clsidStr, 64);
    std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsidStr;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    return S_OK;
}
