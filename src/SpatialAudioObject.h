#pragma once
/**
 * SpatialAudioObject.h
 * ============================================================
 * COM implementation of ISpatialAudioObject backed by a single
 * OpenAL Soft AL source with true per-object HRTF processing.
 *
 * Each object owns:
 *  - One AL source
 *  - A ring of kNumStreamingBuffers AL buffers (PCM streaming)
 *  - A dedicated lock-free audio feed queue
 * ============================================================
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <spatialaudioclient.h>
#include <al.h>
#include <alext.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "OpenALSpatialAudioClient.h"

namespace OpenALSpatial {

// ─────────────────────────────────────────────────────────────
// Internal PCM frame buffer passed between render-thread and
// AL upload thread via a wait-free single-producer ring.
// ─────────────────────────────────────────────────────────────
struct AudioFrame {
    std::vector<float> samples;   // Interleaved, normalised float32
    UINT32 frameCount = 0;
    double timestampQPC = 0.0;
    bool   isStatic = false;      // true → don't expect more frames
};

// ─────────────────────────────────────────────────────────────
// COM implementation
// ─────────────────────────────────────────────────────────────
class SpatialAudioObjectImpl final : public ISpatialAudioObject
{
public:
    // ── COM boilerplate ──────────────────────────────────────
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef()  override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override;

    // ── ISpatialAudioObjectBase ───────────────────────────────
    STDMETHODIMP GetBuffer(BYTE** buffer, UINT32* bufferLength) override;
    STDMETHODIMP SetEndOfStream(UINT32 frameCount) override;
    STDMETHODIMP IsActive(BOOL* isActive) override;
    STDMETHODIMP GetAudioObjectType(AudioObjectType* type) override;

    // ── ISpatialAudioObject ───────────────────────────────────
    STDMETHODIMP SetPosition(float x, float y, float z) override;
    STDMETHODIMP SetVolume(float volume) override;

    // ── Internal (called by stream) ───────────────────────────
    static std::shared_ptr<SpatialAudioObjectImpl> Create(
        AudioObjectType type,
        UINT32 sampleRate,
        UINT32 framesPerBuffer,
        UINT32 numChannels);

    void ApplySpatialParams(const ObjectSpatialParams& p);
    void UploadPendingBuffers();   // Call from AL thread
    void Deactivate();
    void Reset();

    ALuint GetALSource() const { return alSource_; }
    bool   IsActive()    const { return active_.load(std::memory_order_acquire); }

    // Position accessors (read from AL thread)
    float X() const { return pos_[0]; }
    float Y() const { return pos_[1]; }
    float Z() const { return pos_[2]; }

    // ── Passkey for make_shared ───────────────────────────────
    // Allows std::make_shared<SpatialAudioObjectImpl> to work while
    // still preventing accidental construction from outside this class.
    struct PrivateToken {};
    explicit SpatialAudioObjectImpl(PrivateToken) {}

private:
    ~SpatialAudioObjectImpl();

    void InitALSource();
    void DestroyALResources();
    ALuint DequeueFinishedBuffer();

    // COM
    std::atomic<ULONG> refCount_{1};

    // Identity
    AudioObjectType type_   = AudioObjectType_Dynamic;
    UINT32 sampleRate_      = kDefaultSampleRate;
    UINT32 framesPerBuffer_ = kDefaultFramesPerBuffer;
    UINT32 numChannels_     = 1;

    // OpenAL resources
    ALuint alSource_  = 0;
    std::array<ALuint, kNumStreamingBuffers> alBuffers_{};
    bool   alReady_   = false;

    // Staging buffer (written by app, read by upload thread)
    std::vector<float>  stagingF32_;      // float staging area
    std::vector<uint8_t> stagingPCM_;     // byte view for GetBuffer
    UINT32 stagingFrameCount_ = 0;
    bool   endOfStream_ = false;

    // Thread safety
    mutable std::mutex bufMutex_;

    // State
    std::atomic<bool> active_{false};
    float pos_[3] = {0.f, 0.f, 0.f};
    float volume_ = 1.f;

    // AL format
    ALenum alFormat_ = AL_FORMAT_MONO_FLOAT32;

    // Prevent copy/move
    SpatialAudioObjectImpl(const SpatialAudioObjectImpl&) = delete;
    SpatialAudioObjectImpl& operator=(const SpatialAudioObjectImpl&) = delete;
};

} // namespace OpenALSpatial
