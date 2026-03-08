#pragma once
/**
 * SpatialAudioStream.h
 * ============================================================
 * COM implementation of ISpatialAudioObjectRenderStream.
 *
 * Threading model (synchronous -- matches Windows Spatial Audio):
 *
 *  App Thread
 *  ----------------------------------------------------------
 *  BeginUpdatingAudioObjects()
 *    -- reap dead objects, return frameCount
 *  [app writes PCM into each object's staging buffer]
 *  [app calls SetPosition on each object]
 *  EndUpdatingAudioObjects()
 *    -- update AL listener
 *    -- UploadAllObjects() - alBufferData / alSourceQueueBuffers
 *    -- restart any underrun sources
 *
 *  No second thread. No independent clock. The upload is driven
 *  entirely by the app's write cadence, just like the real
 *  ISpatialAudioClient event loop.
 *
 *  The calling thread must remain the same across Start(),
 *  BeginUpdatingAudioObjects(), EndUpdatingAudioObjects(), and
 *  Stop() because alcMakeContextCurrent() is per-thread.
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

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "OpenALSpatialAudioClient.h"
#include "SpatialAudioObject.h"

namespace OpenALSpatial {

// -------------------------------------------------------------
// EFX / reverb support
// -------------------------------------------------------------
struct EFXFunctions {
    LPALGENEFFECTS     alGenEffects     = nullptr;
    LPALDELETEEFFECTS  alDeleteEffects  = nullptr;
    LPALISEFFECT       alIsEffect       = nullptr;
    LPALEFFECTI        alEffecti        = nullptr;
    LPALEFFECTF        alEffectf        = nullptr;
    LPALEFFECTFV       alEffectfv       = nullptr;
    LPALGENAUXILIARYEFFECTSLOTS    alGenAuxiliaryEffectSlots    = nullptr;
    LPALDELETEAUXILIARYEFFECTSLOTS alDeleteAuxiliaryEffectSlots = nullptr;
    LPALAUXILIARYEFFECTSLOTI       alAuxiliaryEffectSloti       = nullptr;
    LPALAUXILIARYEFFECTSLOTF       alAuxiliaryEffectSlotf       = nullptr;

    bool loaded = false;
    void Load(ALCcontext* ctx);
};

// -------------------------------------------------------------
// COM implementation of ISpatialAudioObjectRenderStream
// -------------------------------------------------------------
class SpatialAudioStreamImpl final
    : public ISpatialAudioObjectRenderStream
{
public:
    // -- COM boilerplate --------------------------------------
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef()  override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override;

    // -- ISpatialAudioObjectRenderStreamBase -------------------
    STDMETHODIMP GetAvailableDynamicObjectCount(UINT32* count) override;
    STDMETHODIMP GetService(REFIID riid, void** service)       override;
    STDMETHODIMP Start()                                       override;
    STDMETHODIMP Stop()                                        override;
    STDMETHODIMP Reset()                                       override;
    STDMETHODIMP BeginUpdatingAudioObjects(
        UINT32* availDynObjects,
        UINT32* frameCount) override;
    STDMETHODIMP EndUpdatingAudioObjects() override;

    // -- ISpatialAudioObjectRenderStream -----------------------
    STDMETHODIMP ActivateSpatialAudioObject(
        AudioObjectType type, ISpatialAudioObject** obj) override;

    // -- Factory -----------------------------------------------
    static std::shared_ptr<SpatialAudioStreamImpl> Create(
        ALCdevice*  device,
        ALCcontext* ctx,
        const SpatialAudioObjectRenderStreamActivationParams& params,
        const HRTFConfig& hrtfCfg,
        ISpatialAudioObjectRenderStreamNotify* notify);

    void SetListenerOrientation(const ListenerOrientation& lo);
    HRESULT SetObjectSpatialParams(ISpatialAudioObject* obj,
                                    const ObjectSpatialParams& p);

    struct PrivateToken {};
    explicit SpatialAudioStreamImpl(PrivateToken) {}
    ~SpatialAudioStreamImpl();

private:
    void UploadAllObjects();
    void InitReverb();
    void DestroyReverb();
    void AttachReverbToSource(ALuint src);

    std::atomic<ULONG> refCount_{1};

    ALCdevice*  device_ = nullptr;
    ALCcontext* ctx_    = nullptr;

    UINT32 sampleRate_      = kDefaultSampleRate;
    UINT32 framesPerBuffer_ = kDefaultFramesPerBuffer;
    UINT32 numChannels_     = 1;
    UINT32 maxDynObjects_   = kMaxDynamicObjects;
    WAVEFORMATEX wfx_{};

    EFXFunctions efx_;
    ALuint efxEffect_    = 0;
    ALuint efxSlot_      = 0;
    bool   reverbActive_ = false;
    float  reverbMix_    = 0.15f;

    ISpatialAudioObjectRenderStreamNotify* notify_ = nullptr;

    mutable std::mutex objMutex_;
    std::unordered_map<ISpatialAudioObject*,
        std::shared_ptr<SpatialAudioObjectImpl>> objects_;
    std::atomic<UINT32> activeDynCount_{0};

    std::atomic<bool> inUpdate_{false};
    std::atomic<bool> running_{false};

    // Listener state is applied directly in EndUpdatingAudioObjects
    // on the app thread, so no separate mutex is needed.
    ListenerOrientation listener_{};

    std::unordered_map<ISpatialAudioObject*, ObjectSpatialParams> extParams_;

    // MMCSS handle -- boosted on the calling thread in Start()
    HANDLE mmcssHandle_ = nullptr;
    DWORD  mmcssTaskIdx_= 0;

    SpatialAudioStreamImpl(const SpatialAudioStreamImpl&) = delete;
    SpatialAudioStreamImpl& operator=(const SpatialAudioStreamImpl&) = delete;
};

} // namespace OpenALSpatial
