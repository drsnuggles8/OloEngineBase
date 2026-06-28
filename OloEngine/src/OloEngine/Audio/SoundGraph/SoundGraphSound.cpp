#include "OloEnginePCH.h"
#include "SoundGraphSound.h"
#include "SoundGraphSource.h"
#include "SoundGraph.h"
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Asset/AssetManager.h"

#include <choc/containers/choc_Value.h>
#include <algorithm>
#include <cmath>
#include <string_view>
// AudioEngine.h forward-declares ma_engine; we need the full miniaudio API surface here
// for ma_engine_get_sample_rate (and the implicit pointer arithmetic on ma_engine*).
#include <miniaudio.h>

namespace OloEngine::Audio::SoundGraph
{
    using EPlayState = SoundPlayState;

    namespace
    {
        // Conventional graph-input parameter names the high-level Sound controls route to.
        // A SoundGraph opts a control in by exposing a graph input stream of the matching
        // name + type — float for Volume / Pitch / LowPass / HighPass, bool for Loop — and
        // routing it to the node it should drive (e.g. a Gain node's amplitude, or a
        // WavePlayer's "Loop" input). A graph that omits an endpoint simply ignores that
        // control: the routed write no-ops (SoundGraphSource::SetParameter returns false).
        // "Loop" matches WavePlayer's loop endpoint so a pass-through graph input lines up.
        constexpr std::string_view kVolumeParam = "Volume";
        constexpr std::string_view kPitchParam = "Pitch";
        constexpr std::string_view kLoopParam = "Loop";
        constexpr std::string_view kLowPassParam = "LowPass";
        constexpr std::string_view kHighPassParam = "HighPass";
    } // namespace

    //==============================================================================
    SoundGraphSound::SoundGraphSound()
    {
        OLO_PROFILE_FUNCTION();
        // Initialize member variables to safe defaults
        m_PlayState = SoundPlayState::Stopped;
        m_NextPlayState = SoundPlayState::Stopped;
        m_IsReadyToPlay = false;
        m_IsFinished = false;
        m_IsLooping = false;
        m_Volume = 1.0f;
        m_Pitch = 1.0f;
        m_IsFading = false;
        m_Priority = 128;
    }

    SoundGraphSound::~SoundGraphSound()
    {
        OLO_PROFILE_FUNCTION();
        ReleaseResources();
    }

    bool SoundGraphSound::InitializeAudioCallback()
    {
        OLO_PROFILE_FUNCTION();
        // Replacing the source invalidates readiness (a graph must be re-installed before
        // Play() may succeed); keep the invariant uniform with InitializeDetachedSource.
        m_IsReadyToPlay = false;
        m_Source = CreateScope<Audio::SoundGraph::SoundGraphSource>();

        // Hook the source's custom ma_node into miniaudio's node graph using the live
        // AudioEngine instance. Without this, the source allocates the custom-node struct
        // but ma_node_init never runs and the audio thread never pulls samples — i.e. the
        // graph plays silently. We fetch the engine through the AudioEngine facade rather
        // than threading it through every caller.
        auto* engine = static_cast<ma_engine*>(AudioEngine::GetEngine());
        if (!engine)
        {
            // Return failure so callers don't mark the sound "ready" while it's
            // actually unattached to ma_engine. Previously this returned true, which
            // let Play() proceed silently and made debugging "no sound" turn into a
            // hunt through every other layer. The source object is half-initialized
            // here (Initialize() never ran), so we also reset it to release any
            // partial state.
            OLO_CORE_WARN("SoundGraphSound::InitializeAudioCallback - AudioEngine not initialized; source not attached");
            m_Source.reset();
            return false;
        }

        // ma_engine reports its native sample rate; using it keeps the source in lock-step
        // with the engine's processing rate. Block size 1024 is the typical miniaudio
        // default for engine nodes.
        const u32 sampleRate = ::ma_engine_get_sample_rate(engine);
        constexpr u32 kBlockSize = 1024;
        if (!m_Source->Initialize(engine, sampleRate, kBlockSize, /*channelCount=*/2))
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeAudioCallback - SoundGraphSource::Initialize failed");
            m_Source.reset();
            return false;
        }
        return true;
    }

    bool SoundGraphSound::InitializeDetachedSource()
    {
        OLO_PROFILE_FUNCTION();
        // Swapping in a fresh, graphless source invalidates any prior readiness: it can't
        // play until InitializeFromGraph installs a graph. Clear the flag so a stale
        // m_IsReadyToPlay from a previous source can't let Play() fire on this one.
        m_IsReadyToPlay = false;
        // Mirror InitializeAudioCallback's source allocation but skip the ma_engine
        // attachment: the source can still drive its graph and apply parameter writes,
        // it just never feeds miniaudio's output. See the header for the rationale.
        m_Source = CreateScope<Audio::SoundGraph::SoundGraphSource>();
        return true;
    }

    // Additional overload to match header declaration
    bool SoundGraphSound::InitializeFromGraph(const Ref<Audio::SoundGraph::SoundGraph>& soundGraph)
    {
        // Validate input parameters
        if (!soundGraph)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeFromGraph - Invalid soundGraph provided");
            return false;
        }

        // Ensure we have a valid source to work with
        if (!m_Source)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeFromGraph - No source available, call InitializeAudioCallback first");
            return false;
        }

        // Wire the sound graph into the source
        try
        {
            // ReplaceGraph can decline the swap (audio thread didn't quiesce in time) without
            // throwing. Only mark ready / push controls when the swap actually took — otherwise
            // m_Graph still holds the old (or no) graph and we'd report a sound that isn't there.
            if (!m_Source->ReplaceGraph(soundGraph))
            {
                OLO_CORE_ERROR("SoundGraphSound::InitializeFromGraph - graph swap did not take effect; not ready");
                m_IsReadyToPlay = false;
                return false;
            }
            m_IsReadyToPlay = true;
            // Apply any controls set before the graph existed (volume/pitch/looping/filters).
            SyncControlParametersToGraph();
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeFromGraph - Failed to replace graph: {}", e.what());
            m_IsReadyToPlay = false;
            return false;
        }
        catch (...)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeFromGraph - Unknown error occurred while replacing graph");
            m_IsReadyToPlay = false;
            return false;
        }
    }

    bool SoundGraphSound::InitializeDataSource(const std::vector<AssetHandle>& dataSources, const Ref<Audio::SoundGraph::SoundGraph>& soundGraph)
    {
        // Validate input parameters
        if (dataSources.empty())
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - No data sources provided");
            return false;
        }

        if (!soundGraph)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Invalid soundGraph provided");
            return false;
        }

        // Ensure we have a valid source to work with
        if (!m_Source)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - No source available, call InitializeAudioCallback first");
            return false;
        }

        // Validate all asset handles before proceeding
        for (const auto& handle : dataSources)
        {
            if (handle == 0) // UUID 0 is invalid/null handle
            {
                OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Invalid asset handle in data sources");
                return false;
            }
        }

        // Initialize data sources first
        if (!m_Source->InitializeDataSources(dataSources))
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Failed to initialize data sources");
            m_IsReadyToPlay = false;
            return false;
        }

        // Wire the sound graph into the source
        try
        {
            // See InitializeFromGraph: a declined swap (no throw) must not be reported ready.
            if (!m_Source->ReplaceGraph(soundGraph))
            {
                OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - graph swap did not take effect; not ready");
                m_Source->UninitializeDataSources();
                m_IsReadyToPlay = false;
                return false;
            }
            // Only set ready state if both operations succeeded
            m_IsReadyToPlay = true;
            // Apply any controls set before the graph existed (volume/pitch/looping/filters).
            SyncControlParametersToGraph();
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Failed to replace graph: {}", e.what());
            // Clean up data sources since graph setup failed
            m_Source->UninitializeDataSources();
            m_IsReadyToPlay = false;
            return false;
        }
        catch (...)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Unknown error occurred while replacing graph");
            // Clean up data sources since graph setup failed
            m_Source->UninitializeDataSources();
            m_IsReadyToPlay = false;
            return false;
        }
    }

    void SoundGraphSound::ReleaseResources()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Source)
        {
            // Clean up - no method calls on m_Source since they don't exist
            m_Source.reset();
        }
        m_IsReadyToPlay = false;
        m_IsFinished = true;
    }

    //==============================================================================
    /// Main Sound Interface

    bool SoundGraphSound::Play()
    {
        if (!m_IsReadyToPlay)
            return false;

        m_PlayState = SoundPlayState::Playing;
        m_IsFinished = false;

        // Forward the Play trigger into the runtime graph. Without this the play state
        // flips to Playing on the sound wrapper but the graph's "Play" event input is
        // never raised, so any node listening for that event (WavePlayer, envelopes,
        // trigger nodes) never fires — the audio callback runs but every node stays at
        // its idle/silent default.
        if (m_Source)
            m_Source->SendPlayEvent();

        return true;
    }

    bool SoundGraphSound::Stop()
    {
        // Cancel any active fades
        m_IsFading = false;
        m_FadeCurrentTime = 0.0f;
        m_FadeDuration = 0.0f;
        m_FadeStartVolume = m_Volume;
        m_FadeTargetVolume = m_Volume;

        // Set play state and finished flag
        m_PlayState = SoundPlayState::Stopped;
        m_IsFinished = true;

        return true;
    }

    bool SoundGraphSound::Pause()
    {
        if (m_PlayState == SoundPlayState::Playing)
        {
            m_PlayState = SoundPlayState::Pausing;
            return true;
        }
        return false;
    }

    bool SoundGraphSound::IsPlaying() const
    {
        return m_PlayState == SoundPlayState::Playing && !m_IsFinished;
    }

    //==============================================================================
    /// Volume and Pitch Control

    void SoundGraphSound::SetVolume(f32 newVolume)
    {
        OLO_PROFILE_FUNCTION();

        // Volume can arrive from script / YAML / network (Scene forwards the component's
        // VolumeMultiplier here); reject a non-finite value rather than letting std::clamp
        // pass NaN straight through to the graph.
        if (!std::isfinite(newVolume))
        {
            OLO_CORE_WARN("SoundGraphSound::SetVolume - ignoring non-finite volume; keeping {}", m_Volume);
            return;
        }

        m_Volume = std::clamp(newVolume, 0.0f, 1.0f);
        RouteFloatParameter(kVolumeParam, m_Volume);
    }

    void SoundGraphSound::SetPitch(f32 newPitch)
    {
        OLO_PROFILE_FUNCTION();

        if (!std::isfinite(newPitch))
        {
            OLO_CORE_WARN("SoundGraphSound::SetPitch - ignoring non-finite pitch; keeping {}", m_Pitch);
            return;
        }

        m_Pitch = std::clamp(newPitch, 0.1f, 4.0f);
        RouteFloatParameter(kPitchParam, m_Pitch);
    }

    void SoundGraphSound::SetLooping(bool looping)
    {
        OLO_PROFILE_FUNCTION();

        m_IsLooping = looping;
        RouteBoolParameter(kLoopParam, m_IsLooping);
    }

    f32 SoundGraphSound::GetVolume() const
    {
        OLO_PROFILE_FUNCTION();

        return m_Volume;
    }

    f32 SoundGraphSound::GetPitch() const
    {
        OLO_PROFILE_FUNCTION();

        return m_Pitch;
    }

    void SoundGraphSound::SetLowPassFilter(f32 value)
    {
        OLO_PROFILE_FUNCTION();

        if (!std::isfinite(value))
        {
            OLO_CORE_WARN("SoundGraphSound::SetLowPassFilter - ignoring non-finite value; keeping {}", m_LowPassValue);
            return;
        }

        m_LowPassValue = std::clamp(value, 0.0f, 1.0f);
        RouteFloatParameter(kLowPassParam, m_LowPassValue);
    }

    void SoundGraphSound::SetHighPassFilter(f32 value)
    {
        OLO_PROFILE_FUNCTION();

        if (!std::isfinite(value))
        {
            OLO_CORE_WARN("SoundGraphSound::SetHighPassFilter - ignoring non-finite value; keeping {}", m_HighPassValue);
            return;
        }

        m_HighPassValue = std::clamp(value, 0.0f, 1.0f);
        RouteFloatParameter(kHighPassParam, m_HighPassValue);
    }

    void SoundGraphSound::RouteFloatParameter(std::string_view parameterName, f32 value)
    {
        // Best-effort routing into the live graph. SoundGraphSource::SetParameter(name)
        // hashes the name, checks the graph exposes a matching input endpoint, and applies
        // the value synchronously; it returns false (no-op) when there's no graph yet or the
        // endpoint isn't exposed, so this is safe to call before init and on every graph.
        if (m_Source)
            m_Source->SetParameter(parameterName, choc::value::createFloat32(value));
    }

    void SoundGraphSound::RouteBoolParameter(std::string_view parameterName, bool value)
    {
        if (m_Source)
            m_Source->SetParameter(parameterName, choc::value::createBool(value));
    }

    void SoundGraphSound::SyncControlParametersToGraph()
    {
        // Re-push every stored control so a value set before the graph was installed (or
        // before a graph swap) still lands. Each routes to a conventional endpoint name and
        // is ignored by a graph that doesn't expose it.
        RouteFloatParameter(kVolumeParam, m_Volume);
        RouteFloatParameter(kPitchParam, m_Pitch);
        RouteBoolParameter(kLoopParam, m_IsLooping);
        RouteFloatParameter(kLowPassParam, m_LowPassValue);
        RouteFloatParameter(kHighPassParam, m_HighPassValue);
    }

    //==============================================================================
    /// Parameter Interface - ONLY call SetParameter if source exists and is valid

    void SoundGraphSound::SetParameter(u32 parameterID, f32 value)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Source)
        {
            // Only call if we can verify the method exists via the source's SetParameter method
            try
            {
                m_Source->SetParameter(parameterID, choc::value::createFloat32(value));
            }
            catch (...)
            {
                OLO_CORE_WARN("SoundGraphSound: SetParameter(float) threw unexpectedly for parameterID {}", parameterID);
            }
        }
    }

    void SoundGraphSound::SetParameter(u32 parameterID, i32 value)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Source)
        {
            try
            {
                m_Source->SetParameter(parameterID, choc::value::createInt32(value));
            }
            catch (...)
            {
                OLO_CORE_WARN("SoundGraphSound: SetParameter(int) threw unexpectedly for parameterID {}", parameterID);
            }
        }
    }

    void SoundGraphSound::SetParameter(u32 parameterID, bool value)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Source)
        {
            try
            {
                m_Source->SetParameter(parameterID, choc::value::createBool(value));
            }
            catch (...)
            {
                OLO_CORE_WARN("SoundGraphSound: SetParameter(bool) threw unexpectedly for parameterID {}", parameterID);
            }
        }
    }

    //==============================================================================
    /// Fade Control

    bool SoundGraphSound::FadeIn(f32 duration, f32 targetVolume)
    {
        OLO_PROFILE_FUNCTION();

        if (duration <= 0.0f)
            return false;

        m_IsFading = true;
        m_FadeStartVolume = m_Volume;
        m_FadeTargetVolume = std::clamp(targetVolume, 0.0f, 1.0f);
        m_FadeDuration = duration;
        m_FadeCurrentTime = 0.0f;

        return true;
    }

    bool SoundGraphSound::FadeOut(f32 duration, f32 targetVolume)
    {
        OLO_PROFILE_FUNCTION();
        if (duration <= 0.0f)
            return false;

        m_IsFading = true;
        m_FadeStartVolume = m_Volume;
        m_FadeTargetVolume = std::clamp(targetVolume, 0.0f, 1.0f);
        m_FadeDuration = duration;
        m_FadeCurrentTime = 0.0f;

        return true;
    }

    //==============================================================================
    /// 3D Audio
    ///
    /// These store the spatial state and back GetLocation()/GetVelocity(), but do not
    /// yet feed a spatializer: SoundGraphSource attaches its bare ma_node straight to the
    /// engine endpoint, whereas Audio::DSP::Spatializer (AudioEngine::GetSpatializer)
    /// inserts per-source panning nodes after an ma_engine_node and is wired only into the
    /// ma_sound-based AudioSource path. Routing a SoundGraph voice through the spatializer
    /// is real new infrastructure (per-voice node insertion + listener/source registration),
    /// not a parameter route, so it is deferred — tracked in issue #424.

    void SoundGraphSound::SetLocation(const glm::vec3& location)
    {
        OLO_PROFILE_FUNCTION();
        m_Position = location;
    }

    void SoundGraphSound::SetVelocity(const glm::vec3& velocity)
    {
        OLO_PROFILE_FUNCTION();
        m_Velocity = velocity;
    }

    void SoundGraphSound::SetOrientation(const glm::vec3& forward, const glm::vec3& up)
    {
        OLO_PROFILE_FUNCTION();
        (void)up;
        m_Orientation = forward;
    }

    //==============================================================================
    /// Status and Update

    void SoundGraphSound::Update(f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();
        // Handle fading
        if (m_IsFading)
        {
            m_FadeCurrentTime += deltaTime;

            if (m_FadeCurrentTime >= m_FadeDuration)
            {
                // Fade completed
                m_IsFading = false;
                SetVolume(m_FadeTargetVolume);

                // If faded to zero, stop playback
                if (m_FadeTargetVolume <= 0.0f)
                {
                    Stop();
                }
            }
            else
            {
                // Update fade volume
                f32 t = m_FadeCurrentTime / m_FadeDuration;
                f32 currentVolume = m_FadeStartVolume + (m_FadeTargetVolume - m_FadeStartVolume) * t;
                SetVolume(currentVolume);
            }
        }

        // Pump the source (drains its audio→main-thread event/message queues) and pick up
        // a natural-finish transition. Guarded defensively: a throw from the source must not
        // propagate out of the per-frame Update.
        if (m_Source)
        {
            try
            {
                m_Source->Update(static_cast<f64>(deltaTime));

                if (m_Source->IsFinished() && !m_IsFinished)
                {
                    m_IsFinished = true;
                    if (m_OnPlaybackComplete)
                        m_OnPlaybackComplete();
                }
            }
            catch (...)
            {
                OLO_CORE_WARN("SoundGraphSound::Update - source update threw unexpectedly");
            }
        }
    }

    bool SoundGraphSound::IsFinished() const
    {
        OLO_PROFILE_FUNCTION();

        return m_IsFinished;
    }

    f32 SoundGraphSound::GetCurrentPriority() const
    {
        OLO_PROFILE_FUNCTION();

        f32 basePriority = static_cast<f32>(m_Priority) / 255.0f;
        f32 volumeMultiplier = m_Volume;

        if (m_IsFading && m_FadeDuration > 0.0f)
        {
            f32 t = std::clamp(m_FadeCurrentTime / m_FadeDuration, 0.0f, 1.0f);
            volumeMultiplier = m_FadeStartVolume + (m_FadeTargetVolume - m_FadeStartVolume) * t;
        }

        return basePriority * volumeMultiplier;
    }

    f32 SoundGraphSound::GetPlaybackPercentage() const
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Source)
            return 0.0f;

        u64 maxFrames = m_Source->GetMaxTotalFrames();
        if (maxFrames == 0)
            return 0.0f;

        u64 currentFrame = m_Source->GetCurrentFrame();
        return static_cast<f32>(currentFrame) / static_cast<f32>(maxFrames);
    }

    //==============================================================================
    /// Private Methods

    bool SoundGraphSound::StopFade(u64 numSamples)
    {
        OLO_PROFILE_FUNCTION();
        // Get actual sample rate from the source, fallback to 48000 if not available
        u32 sampleRate = m_Source ? m_Source->GetSampleRate() : 48000;
        if (sampleRate == 0)
            sampleRate = 48000; // Safety fallback if source hasn't been initialized

        // Compute milliseconds using f64 to avoid overflow
        f64 milliseconds = static_cast<f64>(numSamples) * 1000.0 / static_cast<f64>(sampleRate);

        // Clamp to valid i32 range before calling the overload
        constexpr f64 maxInt32 = static_cast<f64>(INT32_MAX);
        if (milliseconds > maxInt32)
        {
            milliseconds = maxInt32;
        }
        else if (milliseconds < 0.0)
        {
            milliseconds = 0.0;
        }
        else
        {
            // No additional handling required.
        }

        return StopFade(static_cast<i32>(milliseconds));
    }

    bool SoundGraphSound::StopFade(i32 milliseconds)
    {
        OLO_PROFILE_FUNCTION();
        if (milliseconds <= 0)
        {
            return StopNow() >= 0;
        }
        return FadeOut(milliseconds / 1000.0f, 0.0f);
    }

    i32 SoundGraphSound::StopNow(StopOptions options)
    {
        OLO_PROFILE_FUNCTION();
        m_PlayState = SoundPlayState::Stopped;
        m_IsFading = false;
        m_IsFinished = true;

        if ((options & StopOptions::ResetPlaybackPosition) != StopOptions::None)
        {
            // Reset logic would go here
        }

        if ((options & StopOptions::NotifyPlaybackComplete) != StopOptions::None)
        {
            if (m_OnPlaybackComplete)
                m_OnPlaybackComplete();
        }

        // A SoundGraphSound owns a single voice — there is no voice pool to return an ID
        // from. 0 is the non-negative success sentinel StopFade()'s `>= 0` check expects.
        return 0;
    }

    f32 SoundGraphSound::NormalizedToFrequency(f32 normalizedValue)
    {
        OLO_PROFILE_FUNCTION();
        f32 minFreq = 20.0f;
        f32 maxFreq = 20000.0f;
        return minFreq + (maxFreq - minFreq) * normalizedValue;
    }

    f32 SoundGraphSound::FrequencyToNormalized(f32 frequency)
    {
        OLO_PROFILE_FUNCTION();
        f32 minFreq = 20.0f;
        f32 maxFreq = 20000.0f;
        return std::clamp((frequency - minFreq) / (maxFreq - minFreq), 0.0f, 1.0f);
    }

} // namespace OloEngine::Audio::SoundGraph
