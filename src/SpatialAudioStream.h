#pragma once
/**
 * SpatialAudioStream.h
 * ============================================================
 * COM implementation of ISpatialAudioObjectRenderStream that
 * drives the per-object update loop and routes PCM frames from
 * application buffers into OpenAL Soft sources.
 *
 * Threading model:
 *  ┌─────────────────────────────────────────────────────────┐
 *  │  App Thread                                             │
 *  │  BeginUpdatingAudioObjects() → write PCM to each obj   │
 *  │  EndUpdatingAudioObjects()   → signals render thread   │
 *  └───────────────────────────────┬─────────────────────────┘
 *                                  │ lock-free handoff
 *  ┌───────────────────────────────▼─────────────────────────┐
 *  │  AL Upload Thread (high-priority)                       │
 *  │  Drains finished AL buffers, uploads new PCM frames,   │
 *  │  updates source positions, calls alSourcePlay as needed │
 *  └─────────────────────────────────────────────────────────┘
 * ============================================================
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <spatialaudioclient.h>
#include <al.h>
#include <alc.h>
#include <alext.h>
#include <efx.h>           // EFX reverb

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "OpenALSpatialAudioClient.h"
#include "SpatialAudioObject.h"

namespace OpenALSpatial {

// ─────────────────────────────────────────────────────────────
// EFX / reverb support (loaded dynamically from the AL context)
// ─────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────
// COM implementation of ISpatialAudioObjectRenderStream
// ─────────────────────────────────────────────────────────────
class SpatialAudioStreamImpl final
    : public ISpatialAudioObjectRenderStream
{
public:
    // ── COM boilerplate ──────────────────────────────────────
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef()  override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override;

    // ── ISpatialAudioObjectRenderStreamBase ───────────────────
    STDMETHODIMP GetAvailableDynamicObjectCount(UINT32* count) override;
    STDMETHODIMP GetService(REFIID riid, void** service)       override;
    STDMETHODIMP Start()                                       override;
    STDMETHODIMP Stop()                                        override;
    STDMETHODIMP Reset()                                       override;
    STDMETHODIMP BeginUpdatingAudioObjects(
        UINT32* availDynObjects,
        UINT32* frameCount) override;
    STDMETHODIMP EndUpdatingAudioObjects() override;

    // ── ISpatialAudioObjectRenderStream ───────────────────────
    STDMETHODIMP ActivateSpatialAudioObject(
        AudioObjectType   type,
        ISpatialAudioObject** obj) override;

    // ── Internal factory ─────────────────────────────────────
    static std::shared_ptr<SpatialAudioStreamImpl> Create(
        ALCdevice*  device,
        ALCcontext* ctx,
        const SpatialAudioObjectRenderStreamActivationParams& params,
        const HRTFConfig& hrtfCfg,
        ISpatialAudioObjectRenderStreamNotify* notify);

    // Update listener orientation (thread-safe)
    void SetListenerOrientation(const ListenerOrientation& lo);

    // Per-object extended params
    HRESULT SetObjectSpatialParams(
        ISpatialAudioObject* obj,
        const ObjectSpatialParams& p);

private:
    SpatialAudioStreamImpl() = default;
    ~SpatialAudioStreamImpl();

    // Rendering pump (runs on alThread_)
    void RenderLoop();
    void UploadAllObjects();
    void InitReverb();
    void DestroyReverb();
    void AttachReverbToSource(ALuint src);

    // COM
    std::atomic<ULONG> refCount_{1};

    // OpenAL context (owned by the client, not the stream)
    ALCdevice*  device_  = nullptr;
    ALCcontext* ctx_     = nullptr;

    // Stream parameters
    UINT32 sampleRate_      = kDefaultSampleRate;
    UINT32 framesPerBuffer_ = kDefaultFramesPerBuffer;
    UINT32 numChannels_     = 1;
    UINT32 maxDynObjects_   = kMaxDynamicObjects;
    WAVEFORMATEX wfx_{};

    // EFX
    EFXFunctions efx_;
    ALuint efxEffect_   = 0;
    ALuint efxSlot_     = 0;
    bool   reverbActive_= false;
    float  reverbMix_   = 0.15f;

    // Notification callback
    ISpatialAudioObjectRenderStreamNotify* notify_ = nullptr;

    // Active objects
    mutable std::mutex objMutex_;
    std::unordered_map<ISpatialAudioObject*,
        std::shared_ptr<SpatialAudioObjectImpl>> objects_;
    std::atomic<UINT32> activeDynCount_{0};

    // Update sync
    std::mutex              updateMutex_;
    std::condition_variable updateCV_;
    std::atomic<bool>       updatePending_{false};
    std::atomic<bool>       inUpdate_{false};

    // AL render thread
    std::thread             alThread_;
    std::atomic<bool>       running_{false};

    // Listener state
    mutable std::mutex   listenerMutex_;
    ListenerOrientation  listener_;

    // Per-object extended params
    std::unordered_map<ISpatialAudioObject*, ObjectSpatialParams> extParams_;

    SpatialAudioStreamImpl(const SpatialAudioStreamImpl&) = delete;
    SpatialAudioStreamImpl& operator=(const SpatialAudioStreamImpl&) = delete;
};

} // namespace OpenALSpatial
