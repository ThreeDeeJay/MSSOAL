/**
 * SpatialAudioObject.cpp
 * ============================================================
 * Each ISpatialAudioObject wraps one OpenAL Soft AL source.
 * True per-object HRTF is enforced via AL_SOURCE_SPATIALIZE_SOFT.
 * ============================================================
 */
// Must come before any system headers to unlock M_PI on MSVC
#define _USE_MATH_DEFINES
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <spatialaudioclient.h>
#include <al.h>
#include <alc.h>
#include <alext.h>

#include <cassert>
#include <cstring>
#include <stdexcept>

#include "SpatialAudioObject.h"

// SPTLAUDCLNT_E_OBJECT_NOTVALID was added in a later Windows SDK revision.
// Define a fallback so the code compiles against older SDKs too.
#ifndef SPTLAUDCLNT_E_OBJECT_NOTVALID
#define SPTLAUDCLNT_E_OBJECT_NOTVALID  ((HRESULT)0x88890020L)
#endif

#ifndef NDEBUG
#include <sstream>
#define OAL_LOG(msg) \
    do { \
        std::wostringstream _ss; \
        _ss << L"[OALObject] " << msg << L"\n"; \
        OutputDebugStringW(_ss.str().c_str()); \
    } while(0)
#else
#define OAL_LOG(msg) (void)0
#endif

namespace OpenALSpatial {

// ─────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────
std::shared_ptr<SpatialAudioObjectImpl> SpatialAudioObjectImpl::Create(
    AudioObjectType type,
    UINT32 sampleRate,
    UINT32 framesPerBuffer,
    UINT32 numChannels)
{
    // Use make_shared with the public PrivateToken overload so MSVC
    // doesn't choke on constructing a shared_ptr from a raw private-ctor pointer.
    auto obj = std::make_shared<SpatialAudioObjectImpl>(PrivateToken{});
    obj->type_           = type;
    obj->sampleRate_     = sampleRate;
    obj->framesPerBuffer_= framesPerBuffer;
    obj->numChannels_    = numChannels;

    // Choose AL format
    if (numChannels == 1) {
        // Check if float32 extension is available
        obj->alFormat_ = alIsExtensionPresent("AL_EXT_FLOAT32")
            ? AL_FORMAT_MONO_FLOAT32
            : AL_FORMAT_MONO16;
    } else {
        obj->alFormat_ = alIsExtensionPresent("AL_EXT_FLOAT32")
            ? AL_FORMAT_STEREO_FLOAT32
            : AL_FORMAT_STEREO16;
    }

    // Size staging buffer (float32 samples)
    obj->stagingF32_.resize((size_t)framesPerBuffer * numChannels, 0.f);
    obj->stagingPCM_.resize(obj->stagingF32_.size() * sizeof(float), 0);

    obj->InitALSource();
    obj->active_.store(true, std::memory_order_release);
    return obj;
}

// ─────────────────────────────────────────────────────────────
// AL resource init
// ─────────────────────────────────────────────────────────────
void SpatialAudioObjectImpl::InitALSource()
{
    alGetError(); // clear

    // Generate streaming buffers
    alGenBuffers((ALsizei)kNumStreamingBuffers, alBuffers_.data());
    if (alGetError() != AL_NO_ERROR) {
        OAL_LOG(L"alGenBuffers failed");
        return;
    }

    // Generate source
    alGenSources(1, &alSource_);
    if (alGetError() != AL_NO_ERROR) {
        OAL_LOG(L"alGenSources failed");
        alDeleteBuffers((ALsizei)kNumStreamingBuffers, alBuffers_.data());
        return;
    }

    // ── Force per-object HRTF spatialization ─────────────────
    // AL_SOURCE_SPATIALIZE_SOFT ensures HRTF is applied to this
    // source even if the source is stereo (downmixed to mono first
    // and then HRTF-processed).
    if (alIsExtensionPresent("AL_SOFT_source_spatialize")) {
        alSourcei(alSource_, AL_SOURCE_SPATIALIZE_SOFT, AL_TRUE);
        OAL_LOG(L"Per-source HRTF enabled on source " << alSource_);
    }

    // Distance model for this source
    alSourcei(alSource_, AL_SOURCE_RELATIVE, AL_FALSE);
    alSourcef(alSource_, AL_ROLLOFF_FACTOR, 1.0f);
    alSourcef(alSource_, AL_REFERENCE_DISTANCE, 1.0f);
    alSourcef(alSource_, AL_MAX_DISTANCE, 1000.0f);

    // For LFE / static channels we may not want distance attenuation
    if (type_ == AudioObjectType_LowFrequency) {
        alSourcei(alSource_, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(alSource_, AL_POSITION, 0.f, 0.f, 0.f);
        alSourcef(alSource_, AL_ROLLOFF_FACTOR, 0.f);
    }

    // Direction (omnidirectional by default)
    alSource3f(alSource_, AL_DIRECTION, 0.f, 0.f, 0.f);

    // Stereo sources: use AL_SOFT_stereo_angles for width control
    if (numChannels_ == 2 &&
        alIsExtensionPresent("AL_SOFT_stereo_angles")) {
        // Default ±30° spread
        ALfloat angles[2] = {
             static_cast<ALfloat>( 30.0 * M_PI / 180.0),
             static_cast<ALfloat>(-30.0 * M_PI / 180.0)
        };
        alSourcefv(alSource_, AL_STEREO_ANGLES, angles);
    }

    alReady_ = (alGetError() == AL_NO_ERROR);
    OAL_LOG(L"AL source " << alSource_ << L" ready="
        << alReady_ << L" type=" << (int)type_);
}

// ─────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────
SpatialAudioObjectImpl::~SpatialAudioObjectImpl()
{
    DestroyALResources();
}

void SpatialAudioObjectImpl::DestroyALResources()
{
    if (alSource_) {
        alSourceStop(alSource_);
        alSourcei(alSource_, AL_BUFFER, 0);
        alDeleteSources(1, &alSource_);
        alSource_ = 0;
    }
    bool anyNonZero = false;
    for (auto b : alBuffers_) if (b) { anyNonZero = true; break; }
    if (anyNonZero) {
        alDeleteBuffers((ALsizei)kNumStreamingBuffers, alBuffers_.data());
        alBuffers_.fill(0);
    }
    alReady_ = false;
}

// ─────────────────────────────────────────────────────────────
// ISpatialAudioObjectBase – GetBuffer
// Called by app to get a write pointer for this update cycle
// ─────────────────────────────────────────────────────────────
STDMETHODIMP SpatialAudioObjectImpl::GetBuffer(
    BYTE** buffer, UINT32* bufferLength)
{
    if (!buffer || !bufferLength) return E_POINTER;
    if (!active_.load(std::memory_order_acquire)) return SPTLAUDCLNT_E_OBJECT_NOTVALID;

    std::lock_guard<std::mutex> lk(bufMutex_);
    endOfStream_ = false;

    // Zero out so apps that don't write all frames get silence
    std::memset(stagingF32_.data(), 0,
        stagingF32_.size() * sizeof(float));

    *buffer       = stagingPCM_.data();
    *bufferLength = static_cast<UINT32>(stagingF32_.size() * sizeof(float));
    return S_OK;
}

// ─────────────────────────────────────────────────────────────
// ISpatialAudioObjectBase – SetEndOfStream
// ─────────────────────────────────────────────────────────────
STDMETHODIMP SpatialAudioObjectImpl::SetEndOfStream(UINT32 frameCount)
{
    std::lock_guard<std::mutex> lk(bufMutex_);
    stagingFrameCount_ = frameCount;
    endOfStream_       = true;
    active_.store(false, std::memory_order_release);
    OAL_LOG(L"SetEndOfStream – src=" << alSource_);
    return S_OK;
}

STDMETHODIMP SpatialAudioObjectImpl::IsActive(BOOL* isActive)
{
    if (!isActive) return E_POINTER;
    *isActive = active_.load(std::memory_order_acquire) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP SpatialAudioObjectImpl::GetAudioObjectType(AudioObjectType* type)
{
    if (!type) return E_POINTER;
    *type = type_;
    return S_OK;
}

// ─────────────────────────────────────────────────────────────
// ISpatialAudioObject – SetPosition
// Converts from MS API coordinate system (metres, right-handed,
// Y-up) directly to OpenAL (same convention).
// ─────────────────────────────────────────────────────────────
STDMETHODIMP SpatialAudioObjectImpl::SetPosition(float x, float y, float z)
{
    pos_[0] = x; pos_[1] = y; pos_[2] = z;
    if (alSource_) {
        alSource3f(alSource_, AL_POSITION, x, y, z);
    }
    return S_OK;
}

STDMETHODIMP SpatialAudioObjectImpl::SetVolume(float volume)
{
    volume_ = std::max(0.f, volume);
    if (alSource_) {
        alSourcef(alSource_, AL_GAIN, volume_);
    }
    return S_OK;
}

// ─────────────────────────────────────────────────────────────
// Extended spatial parameters
// ─────────────────────────────────────────────────────────────
void SpatialAudioObjectImpl::ApplySpatialParams(const ObjectSpatialParams& p)
{
    if (!alSource_) return;

    alSource3f(alSource_, AL_POSITION,   p.x, p.y, p.z);
    alSource3f(alSource_, AL_VELOCITY,
        p.velocityX, p.velocityY, p.velocityZ);
    alSourcef(alSource_, AL_GAIN,              p.gain);
    alSourcef(alSource_, AL_ROLLOFF_FACTOR,    p.rolloffFactor);
    alSourcef(alSource_, AL_REFERENCE_DISTANCE,p.referenceDistance);
    alSourcef(alSource_, AL_MAX_DISTANCE,      p.maxDistance);
    alSourcef(alSource_, AL_CONE_INNER_ANGLE,  p.coneInnerAngle);
    alSourcef(alSource_, AL_CONE_OUTER_ANGLE,  p.coneOuterAngle);
    alSourcef(alSource_, AL_CONE_OUTER_GAIN,   p.coneOuterGain);

    if (alIsExtensionPresent("AL_SOFT_source_spatialize")) {
        alSourcei(alSource_, AL_SOURCE_SPATIALIZE_SOFT,
            p.spatializeOverride ? AL_TRUE : AL_AUTO_SOFT);
    }

    // Per-source distance model (requires AL_EXT_source_distance_model)
    if (alIsExtensionPresent("AL_EXT_source_distance_model")) {
        alSourcei(alSource_, AL_DISTANCE_MODEL, [&]() -> ALint {
            switch (p.distModel) {
            case DistanceModel::InverseDistance:
                return AL_INVERSE_DISTANCE;
            case DistanceModel::InverseDistanceClamped:
                return AL_INVERSE_DISTANCE_CLAMPED;
            case DistanceModel::LinearDistance:
                return AL_LINEAR_DISTANCE;
            case DistanceModel::LinearDistanceClamped:
                return AL_LINEAR_DISTANCE_CLAMPED;
            case DistanceModel::ExponentDistance:
                return AL_EXPONENT_DISTANCE;
            case DistanceModel::ExponentDistanceClamped:
                return AL_EXPONENT_DISTANCE_CLAMPED;
            default:
                return AL_NONE;
            }
        }());
    }

    pos_[0] = p.x; pos_[1] = p.y; pos_[2] = p.z;
    volume_ = p.gain;
}

// ─────────────────────────────────────────────────────────────
// UploadPendingBuffers – called from AL thread
// Drains processed buffers from the source queue, then refills
// with the latest PCM frame from the staging area.
// ─────────────────────────────────────────────────────────────
void SpatialAudioObjectImpl::UploadPendingBuffers()
{
    if (!alReady_ || !alSource_) return;

    // ── Drain processed buffers ──────────────────────────────
    ALint processed = 0;
    alGetSourcei(alSource_, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0) {
        ALuint buf = 0;
        alSourceUnqueueBuffers(alSource_, 1, &buf);
    }

    // ── Check if we have headroom ─────────────────────────────
    ALint queued = 0;
    alGetSourcei(alSource_, AL_BUFFERS_QUEUED, &queued);
    if (queued >= (ALint)kNumStreamingBuffers) return;

    // ── Copy staging data (lock) ──────────────────────────────
    std::vector<float> localSamples;
    UINT32 localFrames = 0;
    bool   eos         = false;
    {
        std::lock_guard<std::mutex> lk(bufMutex_);
        localSamples = stagingF32_;
        localFrames  = framesPerBuffer_;
        eos          = endOfStream_;
    }

    if (localSamples.empty()) return;

    // ── Upload to an available AL buffer ─────────────────────
    // Find first idle buffer (not currently queued)
    // Simple strategy: use processed-count as ring index
    static thread_local size_t bufIdx = 0;
    ALuint buf = alBuffers_[bufIdx % kNumStreamingBuffers];
    bufIdx++;

    ALsizei dataSize = (ALsizei)(localSamples.size() * sizeof(float));
    alBufferData(buf, alFormat_, localSamples.data(),
        dataSize, (ALsizei)sampleRate_);

    if (alGetError() == AL_NO_ERROR) {
        alSourceQueueBuffers(alSource_, 1, &buf);
    }

    // ── Restart source if it underran ────────────────────────
    ALint state = 0;
    alGetSourcei(alSource_, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING && active_.load(std::memory_order_acquire)) {
        alSourcePlay(alSource_);
    }

    if (eos) {
        // Drain naturally – do not stop immediately so last frame plays
        alSourcei(alSource_, AL_LOOPING, AL_FALSE);
    }
}

// ─────────────────────────────────────────────────────────────
// State management
// ─────────────────────────────────────────────────────────────
void SpatialAudioObjectImpl::Deactivate()
{
    active_.store(false, std::memory_order_release);
    if (alSource_) {
        alSourceStop(alSource_);
        alSourcei(alSource_, AL_BUFFER, 0); // detach all buffers
    }
}

void SpatialAudioObjectImpl::Reset()
{
    Deactivate();
    std::lock_guard<std::mutex> lk(bufMutex_);
    std::fill(stagingF32_.begin(), stagingF32_.end(), 0.f);
    endOfStream_ = false;
}

// ─────────────────────────────────────────────────────────────
// QueryInterface
// ─────────────────────────────────────────────────────────────
STDMETHODIMP SpatialAudioObjectImpl::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(ISpatialAudioObjectBase) ||
        riid == __uuidof(ISpatialAudioObject))
    {
        *ppv = static_cast<ISpatialAudioObject*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) SpatialAudioObjectImpl::Release()
{
    ULONG r = --refCount_;
    if (r == 0) {
        DestroyALResources();
        delete this;
    }
    return r;
}

} // namespace OpenALSpatial
