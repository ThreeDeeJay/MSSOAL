/**
 * SpatialAudioStream.cpp
 * ============================================================
 * ISpatialAudioObjectRenderStream -- synchronous model.
 *
 * The upload is driven directly by EndUpdatingAudioObjects() on
 * the app thread. There is no independent render thread and no
 * second clock. This matches the Windows Spatial Audio event
 * model exactly and eliminates the inter-clock drift that caused
 * stuttering in the previous two-thread design.
 * ============================================================
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <spatialaudioclient.h>
#include <al.h>
#include <alc.h>
#include <alext.h>
#include <efx.h>
#include <efx-presets.h>

#pragma comment(lib, "avrt.lib")

#include <cstring>
#include <memory>
#include <stdexcept>

#include "SpatialAudioStream.h"

#ifndef NDEBUG
#include <sstream>
#define OAL_LOG(msg) \
    do { \
        std::wostringstream _ss; \
        _ss << L"[OALStream] " << msg << L"\n"; \
        OutputDebugStringW(_ss.str().c_str()); \
    } while(0)
#else
#define OAL_LOG(msg) (void)0
#endif

namespace OpenALSpatial {

// -------------------------------------------------------------
// EFX loader
// -------------------------------------------------------------
void EFXFunctions::Load(ALCcontext* ctx)
{
    (void)ctx;
#define LOAD(name) name = reinterpret_cast<decltype(name)>(alGetProcAddress(#name))
    LOAD(alGenEffects);
    LOAD(alDeleteEffects);
    LOAD(alIsEffect);
    LOAD(alEffecti);
    LOAD(alEffectf);
    LOAD(alEffectfv);
    LOAD(alGenAuxiliaryEffectSlots);
    LOAD(alDeleteAuxiliaryEffectSlots);
    LOAD(alAuxiliaryEffectSloti);
    LOAD(alAuxiliaryEffectSlotf);
#undef LOAD
    loaded = alGenEffects && alDeleteEffects && alGenAuxiliaryEffectSlots;
    OAL_LOG(L"EFX loaded=" << loaded);
}

// -------------------------------------------------------------
// Factory
// -------------------------------------------------------------
std::shared_ptr<SpatialAudioStreamImpl> SpatialAudioStreamImpl::Create(
    ALCdevice*  device,
    ALCcontext* ctx,
    const SpatialAudioObjectRenderStreamActivationParams& params,
    const HRTFConfig& hrtfCfg,
    ISpatialAudioObjectRenderStreamNotify* notify)
{
    auto s = std::make_shared<SpatialAudioStreamImpl>(PrivateToken{});

    s->device_    = device;
    s->ctx_       = ctx;
    s->notify_    = notify;
    s->reverbMix_ = hrtfCfg.reverbMix;

    if (params.ObjectFormat) {
        s->sampleRate_  = params.ObjectFormat->nSamplesPerSec;
        s->numChannels_ = params.ObjectFormat->nChannels;
        std::memcpy(&s->wfx_, params.ObjectFormat, sizeof(WAVEFORMATEX));
    } else {
        s->sampleRate_  = kDefaultSampleRate;
        s->numChannels_ = 1;
    }
    s->framesPerBuffer_ = s->sampleRate_ / 100;   // 10 ms
    s->maxDynObjects_   = std::min(params.MaxDynamicObjectCount,
                                    kMaxDynamicObjects);

    // Enable per-source distance model override
    if (alIsExtensionPresent("AL_EXT_source_distance_model"))
        alEnable(AL_SOURCE_DISTANCE_MODEL);
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);

    if (hrtfCfg.enableReverb &&
        alcIsExtensionPresent(device, "ALC_EXT_EFX"))
    {
        s->efx_.Load(ctx);
        if (s->efx_.loaded) s->InitReverb();
    }

    OAL_LOG(L"Stream created - sr=" << s->sampleRate_
        << L" maxDyn=" << s->maxDynObjects_);
    return s;
}

SpatialAudioStreamImpl::~SpatialAudioStreamImpl()
{
    Stop();
    DestroyReverb();
    OAL_LOG(L"Stream destroyed");
}

// -------------------------------------------------------------
// EFX reverb
// -------------------------------------------------------------
void SpatialAudioStreamImpl::InitReverb()
{
    if (!efx_.alGenEffects) return;

    efx_.alGenEffects(1, &efxEffect_);
    efx_.alEffecti(efxEffect_, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);

    EFXEAXREVERBPROPERTIES preset = EFX_REVERB_PRESET_GENERIC;
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_DENSITY,           preset.flDensity);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_DIFFUSION,         preset.flDiffusion);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_GAIN,              preset.flGain);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_GAINHF,            preset.flGainHF);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_GAINLF,            preset.flGainLF);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_DECAY_TIME,        preset.flDecayTime);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_DECAY_HFRATIO,     preset.flDecayHFRatio);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_DECAY_LFRATIO,     preset.flDecayLFRatio);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_REFLECTIONS_GAIN,  preset.flReflectionsGain);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_REFLECTIONS_DELAY, preset.flReflectionsDelay);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_LATE_REVERB_GAIN,  preset.flLateReverbGain);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_LATE_REVERB_DELAY, preset.flLateReverbDelay);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_ECHO_TIME,         preset.flEchoTime);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_ECHO_DEPTH,        preset.flEchoDepth);
    efx_.alEffectf(efxEffect_, AL_EAXREVERB_AIR_ABSORPTION_GAINHF,
        preset.flAirAbsorptionGainHF);

    efx_.alGenAuxiliaryEffectSlots(1, &efxSlot_);
    efx_.alAuxiliaryEffectSloti(efxSlot_, AL_EFFECTSLOT_EFFECT, (ALint)efxEffect_);
    efx_.alAuxiliaryEffectSlotf(efxSlot_, AL_EFFECTSLOT_GAIN,   reverbMix_);

    reverbActive_ = (alGetError() == AL_NO_ERROR);
    OAL_LOG(L"EFX reverb " << (reverbActive_ ? L"active" : L"failed"));
}

void SpatialAudioStreamImpl::DestroyReverb()
{
    if (!efx_.loaded) return;
    if (efxSlot_)   { efx_.alDeleteAuxiliaryEffectSlots(1, &efxSlot_);  efxSlot_    = 0; }
    if (efxEffect_) { efx_.alDeleteEffects(1, &efxEffect_);             efxEffect_  = 0; }
    reverbActive_ = false;
}

void SpatialAudioStreamImpl::AttachReverbToSource(ALuint src)
{
    if (!reverbActive_ || !efxSlot_) return;
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)efxSlot_, 0, AL_FILTER_NULL);
}

// -------------------------------------------------------------
// Stream lifecycle
//
// Start() makes the OpenAL context current on the calling thread.
// All subsequent BeginUpdating / EndUpdating calls must come from
// the same thread (or the caller must ensure alcMakeContextCurrent
// before each call). This matches OpenAL's per-thread context model.
// -------------------------------------------------------------
STDMETHODIMP SpatialAudioStreamImpl::Start()
{
    if (running_.exchange(true)) return S_OK;

    // Make the AL context current on this thread so that all AL
    // calls in EndUpdatingAudioObjects() work without a separate thread.
    alcMakeContextCurrent(ctx_);

    // Boost this thread to Pro Audio priority.
    // AvRevertMmThreadCharacteristics is called in Stop().
    if (!mmcssHandle_) {
        mmcssHandle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIdx_);
    }

    OAL_LOG(L"Stream started (synchronous mode)");
    return S_OK;
}

STDMETHODIMP SpatialAudioStreamImpl::Stop()
{
    if (!running_.exchange(false)) return S_OK;

    if (mmcssHandle_) {
        AvRevertMmThreadCharacteristics(mmcssHandle_);
        mmcssHandle_ = nullptr;
    }

    // Make context current here too in case Stop() is called from
    // a different thread than Start() (e.g. cleanup on main thread).
    alcMakeContextCurrent(ctx_);

    std::lock_guard<std::mutex> lk(objMutex_);
    for (auto& [ptr, obj] : objects_) {
        if (obj) alSourceStop(obj->GetALSource());
    }

    OAL_LOG(L"Stream stopped");
    return S_OK;
}

STDMETHODIMP SpatialAudioStreamImpl::Reset()
{
    Stop();
    {
        std::lock_guard<std::mutex> lk(objMutex_);
        for (auto& [ptr, obj] : objects_)
            if (obj) obj->Reset();
        objects_.clear();
        activeDynCount_.store(0);
    }
    return Start();
}

// -------------------------------------------------------------
// Per-frame update protocol
// -------------------------------------------------------------
STDMETHODIMP SpatialAudioStreamImpl::BeginUpdatingAudioObjects(
    UINT32* availDynObjects, UINT32* frameCount)
{
    if (!availDynObjects || !frameCount) return E_POINTER;
    if (!running_.load())                return AUDCLNT_E_SERVICE_NOT_RUNNING;
    if (inUpdate_.exchange(true))        return AUDCLNT_E_OUT_OF_ORDER;

    // Reap objects whose app-side COM ref has dropped to zero
    {
        std::lock_guard<std::mutex> lk(objMutex_);
        for (auto it = objects_.begin(); it != objects_.end(); ) {
            if (it->second && !it->second->IsActive()) {
                it->second->Deactivate();
                it = objects_.erase(it);
                activeDynCount_.fetch_sub(1, std::memory_order_relaxed);
            } else {
                ++it;
            }
        }
    }

    *availDynObjects = maxDynObjects_ -
        activeDynCount_.load(std::memory_order_relaxed);
    *frameCount = framesPerBuffer_;
    return S_OK;
}

STDMETHODIMP SpatialAudioStreamImpl::EndUpdatingAudioObjects()
{
    if (!inUpdate_.exchange(false)) return AUDCLNT_E_OUT_OF_ORDER;

    // -- Update AL listener ------------------------------------
    // Called directly on the app thread (context is current here).
    {
        const auto& lo = listener_;
        ALfloat ori[6] = {
            lo.fwdX, lo.fwdY, lo.fwdZ,
            lo.upX,  lo.upY,  lo.upZ
        };
        alListenerfv(AL_ORIENTATION, ori);
        alListener3f(AL_POSITION, lo.posX, lo.posY, lo.posZ);
        alListener3f(AL_VELOCITY, lo.velX, lo.velY, lo.velZ);
        alListenerf(AL_GAIN, lo.masterGain);
    }

    // -- Upload PCM to AL buffer queue (synchronous) -----------
    // This is the key change: no signal to a second thread.
    // The upload happens immediately in the same call, on the same
    // thread, with zero clock drift relative to the write.
    UploadAllObjects();

    // -- Optional notify callback ------------------------------
    if (notify_) {
        UINT32 avail = maxDynObjects_ -
            activeDynCount_.load(std::memory_order_relaxed);
        notify_->OnAvailableDynamicObjectCountChange(this, 0, avail);
    }

    return S_OK;
}

// -------------------------------------------------------------
// Object activation
// -------------------------------------------------------------
STDMETHODIMP SpatialAudioStreamImpl::ActivateSpatialAudioObject(
    AudioObjectType type, ISpatialAudioObject** obj)
{
    if (!obj) return E_POINTER;

    if (type == AudioObjectType_Dynamic) {
        UINT32 cur = activeDynCount_.load(std::memory_order_relaxed);
        if (cur >= maxDynObjects_) return SPTLAUDCLNT_E_NO_MORE_OBJECTS;
    }

    auto impl = SpatialAudioObjectImpl::Create(
        type, sampleRate_, framesPerBuffer_, numChannels_);
    if (!impl) return E_OUTOFMEMORY;

    AttachReverbToSource(impl->GetALSource());

    {
        std::lock_guard<std::mutex> lk(objMutex_);
        auto* raw = static_cast<ISpatialAudioObject*>(impl.get());
        objects_[raw] = impl;
        if (type == AudioObjectType_Dynamic)
            activeDynCount_.fetch_add(1, std::memory_order_relaxed);
    }

    impl->AddRef();
    *obj = static_cast<ISpatialAudioObject*>(impl.get());
    OAL_LOG(L"Activated object type=" << (int)type
        << L" src=" << impl->GetALSource());
    return S_OK;
}

// -------------------------------------------------------------
// Misc
// -------------------------------------------------------------
STDMETHODIMP SpatialAudioStreamImpl::GetAvailableDynamicObjectCount(UINT32* count)
{
    if (!count) return E_POINTER;
    *count = maxDynObjects_ - activeDynCount_.load(std::memory_order_relaxed);
    return S_OK;
}

STDMETHODIMP SpatialAudioStreamImpl::GetService(REFIID riid, void** service)
{
    (void)riid;
    if (!service) return E_POINTER;
    *service = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP SpatialAudioStreamImpl::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(ISpatialAudioObjectRenderStreamBase) ||
        riid == __uuidof(ISpatialAudioObjectRenderStream))
    {
        *ppv = static_cast<ISpatialAudioObjectRenderStream*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) SpatialAudioStreamImpl::Release()
{
    return --refCount_;
}

// -------------------------------------------------------------
// Listener orientation (no mutex needed: called only from app
// thread between Begin/EndUpdating, same thread as End which reads it)
// -------------------------------------------------------------
void SpatialAudioStreamImpl::SetListenerOrientation(const ListenerOrientation& lo)
{
    listener_ = lo;
}

// -------------------------------------------------------------
// Extended spatial params
// -------------------------------------------------------------
HRESULT SpatialAudioStreamImpl::SetObjectSpatialParams(
    ISpatialAudioObject* obj, const ObjectSpatialParams& p)
{
    std::lock_guard<std::mutex> lk(objMutex_);
    auto it = objects_.find(obj);
    if (it == objects_.end()) return E_INVALIDARG;
    if (it->second) it->second->ApplySpatialParams(p);
    extParams_[obj] = p;
    return S_OK;
}

// -------------------------------------------------------------
// Upload -- called directly from EndUpdatingAudioObjects()
// -------------------------------------------------------------
void SpatialAudioStreamImpl::UploadAllObjects()
{
    std::lock_guard<std::mutex> lk(objMutex_);
    for (auto& [ptr, obj] : objects_) {
        if (obj && obj->IsActive())
            obj->UploadPendingBuffers();
    }
}

} // namespace OpenALSpatial
