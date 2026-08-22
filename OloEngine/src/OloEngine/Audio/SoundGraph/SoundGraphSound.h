#pragma once

#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSource.h"
#include "OloEngine/Audio/VoiceManager.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Asset/Asset.h" // For AssetHandle
#include <glm/glm.hpp>
#include <atomic>
#include <string>
#include <string_view>
#include <functional>
#include <utility>
#include <vector>

// Forward declarations
namespace OloEngine
{
    class AudioEngine;

    namespace Audio::SoundGraph
    {
        class SoundGraph;
    } // namespace Audio::SoundGraph

    enum class SoundPlayState
    {
        Stopped = 0,
        Playing,
        Pausing,
        Stopping
    };

    struct SoundConfig
    {
        AssetHandle m_DataSourceAsset = 0;
        f32 m_VolumeMultiplier = 1.0f;
        f32 m_PitchMultiplier = 1.0f;
        bool m_Looping = false;
        bool m_SpatializationEnabled = false;

        // Filter values (0.0 - 1.0, normalized)
        f32 m_LPFilterValue = 1.0f; // 1.0 = no filtering
        f32 m_HPFilterValue = 0.0f; // 0.0 = no filtering

        bool m_PlayOnAwake = true;
        u8 m_Priority = 128; // 0 = highest, 255 = lowest
    };

    /**
     * Abstract base interface for audio playback objects
     */
    class IPlayableAudio : public RefCounted
    {
      public:
        virtual ~IPlayableAudio() = default;

        // Core playback interface
        virtual bool Play() = 0;
        virtual bool Stop() = 0;
        virtual bool Pause() = 0;
        virtual bool IsPlaying() const = 0;

        // Volume and pitch control
        virtual void SetVolume(f32 newVolume) = 0;
        virtual void SetPitch(f32 newPitch) = 0;
        virtual f32 GetVolume() const = 0;
        virtual f32 GetPitch() const = 0;
    };

    /* =====================================
        SoundGraph Sound, represents playing voice
        Based on Hazel::SoundGraphSound
        ------------------------------------
    */

    namespace Audio
    {
        namespace SoundGraph
        {
            /// A SoundGraph-driven voice.
            ///
            /// Play()/Stop() are the admission point for the graph path into the shared
            /// concurrent-voice budget (issue #730) — the sibling of AudioSource for the
            /// clip path. Both paths MUST route through VoiceManager or the cap is
            /// enforced on only half the engine's sounds.
            ///
            /// Virtualizing a graph voice mutes it AND freezes the graph runtime
            /// (SoundGraphSource::SetVoiceSuspended, issue #745): while it is virtual the
            /// graph is not stepped at all, so the DSP cost is genuinely reclaimed the way
            /// ma_sound_stop reclaims it on the clip path. Thawing continues the same
            /// sample stream — node state is not saved and restored, it is simply not
            /// advanced — so it is a resume rather than #730's restart-instead-of-resume
            /// bug.
            ///
            /// The one thing it cannot do, and this is inherent rather than missing API:
            /// come back at the position the voice WOULD have reached had it kept running.
            /// For a procedural graph, advancing to frame N is exactly the DSP work of
            /// frames 0..N, so reclaiming the CPU and preserving that phase are the same
            /// computation and you get one or the other. OnVoiceStart's positionSeconds is
            /// therefore honoured only where it is reachable (a fresh start at 0); see the
            /// comment there and docs/agent-rules/audio-voice-budget.md section 8.
            class SoundGraphSound : public IPlayableAudio, public OloEngine::Audio::IVoiceHost
            {
              public:
                explicit SoundGraphSound();
                ~SoundGraphSound() override;

                //--- Sound Source Interface
                bool Play() override;
                bool Stop() override;
                bool Pause() override;
                bool IsPlaying() const override;
                // ~ End of Sound Source Interface

                void SetVolume(f32 newVolume) override;
                void SetPitch(f32 newPitch) override;
                void SetLooping(bool looping);

                f32 GetVolume() const override;
                f32 GetPitch() const override;

                void SetLowPassFilter(f32 value);  // 0.0 - 1.0 normalized
                void SetHighPassFilter(f32 value); // 0.0 - 1.0 normalized

                //==============================================================================
                /// Sound Parameter Interface
                void SetParameter(u32 parameterID, f32 value);
                void SetParameter(u32 parameterID, i32 value);
                void SetParameter(u32 parameterID, bool value);

                virtual bool FadeIn(f32 duration, f32 targetVolume);
                virtual bool FadeOut(f32 duration, f32 targetVolume);

                //==============================================================================
                /// Initialization
                /** Allocate the internal SoundGraphSource. Must be called before any of the
                    InitializeFromGraph / InitializeDataSource overloads. Exposed publicly so
                    Scene::InitAudioRuntime (and other owners) can drive the lifecycle. */
                bool InitializeAudioCallback();

                /** Allocate the internal SoundGraphSource WITHOUT attaching it to the live
                    ma_engine (contrast InitializeAudioCallback). A detached source still
                    processes its graph and applies parameter writes — so SetVolume / SetPitch /
                    SetParameter take effect on the graph's input cells — but produces no audible
                    output because it is never wired into miniaudio's node graph. Used for
                    headless graph evaluation and for unit-testing the parameter-routing path
                    without an audio device. Returns true (no failure mode); replaces any
                    existing source. Follow with InitializeFromGraph as usual. */
                bool InitializeDetachedSource();

                /** Initialize from SoundGraph instance */
                bool InitializeFromGraph(const Ref<Audio::SoundGraph::SoundGraph>& soundGraph);

                /** Initialize from SoundGraph asset and data sources */
                bool InitializeDataSource(const std::vector<AssetHandle>& dataSources, const Ref<Audio::SoundGraph::SoundGraph>& soundGraph);

                void ReleaseResources();

                //==============================================================================
                /// 3D Audio
                ///
                /// These now drive the per-voice 3D spatializer (issue #424): each setter
                /// stores the spatial state AND, once a spatializer node is hosted by the
                /// source, pushes the combined transform/velocity into
                /// Audio::DSP::Spatializer::UpdateSourcePosition. They no-op gracefully before
                /// the source/spatializer exists (the value is still stored, so the getters and
                /// a later registration stay correct).
                void SetLocation(const glm::vec3& location);
                void SetVelocity(const glm::vec3& velocity);
                void SetOrientation(const glm::vec3& forward, const glm::vec3& up);

                const glm::vec3& GetLocation() const
                {
                    return m_Position;
                } // Alias for m_Position
                const glm::vec3& GetVelocity() const
                {
                    return m_Velocity;
                }

                /// Whether this voice is routed through the 3D spatializer. Owned here at
                /// runtime (the AudioSoundGraphComponent has no serialized spatialization flag
                /// yet — see issue #424); defaults to enabled so a SoundGraph voice on an entity
                /// is positioned. Must be set BEFORE InitializeAudioCallback to take effect, as
                /// that is where the spatializer node is registered.
                void SetSpatializationEnabled(bool enabled)
                {
                    m_SpatializationEnabled = enabled;
                }
                bool IsSpatializationEnabled() const
                {
                    return m_SpatializationEnabled;
                }

                /// True once the source actually hosts a spatializer node (false if disabled,
                /// the source is detached, or the engine spatializer was unavailable).
                bool IsSpatialized() const;

                /// The spatializer sourceID for this voice (0 when not spatialized). Lets
                /// callers/tests query Audio::DSP::Spatializer's per-source getters.
                u32 GetSpatializerSourceID() const;

                //==============================================================================
                /// Status
                bool IsReadyToPlay() const
                {
                    return m_IsReadyToPlay;
                }
                bool IsFinished() const;
                bool IsStopping() const
                {
                    return m_IsStopping;
                }

                //==============================================================================
                /// Update (called from main thread)
                void Update(f32 deltaTime);

                //==============================================================================
                /// Advanced Control
                f32 GetCurrentFadeVolume() const
                {
                    return m_CurrentFadeVolume;
                }
                f32 GetCurrentPriority() const;
                f32 GetPlaybackPercentage() const;

                //==============================================================================
                /// Voice budget (issue #730)
                /// Authored importance, miniaudio-style: 0 = highest, 255 = lowest. Mapped
                /// to VoiceParams::Priority (0..1, higher = more important) on the way in.
                void SetPriority(u8 priority);
                u8 GetPriority() const
                {
                    return m_Priority;
                }
                /// True while registered with the budget but muted because a higher-scoring
                /// voice holds the slot.
                bool IsVirtualized() const;

                Audio::SoundGraph::SoundGraphSource* GetSource() const
                {
                    return m_Source.get();
                }

                //--- OloEngine::Audio::IVoiceHost ------------------------------------------
                // Driven by the voice budget, never called directly.
                bool OnVoiceStart(f64 positionSeconds) const override;
                f64 OnVoiceStop() const override;
                f64 OnVoiceQueryPosition() const override;

              private:
                /// Snapshot the current scoring inputs for the budget.
                OloEngine::Audio::VoiceParams BuildVoiceParams() const;
                /// Hand the slot back and forget the handle. Idempotent.
                ///
                /// `resumePlayback` decides which state the live source is left in, because
                /// the budget is no longer tracking this voice and nothing else would ever
                /// lift its virtualization. Pass TRUE only when the source must be left
                /// running with nobody to un-mute it (teardown). Pass FALSE whenever it must
                /// be left muted and frozen — both when retiring a voice that is meant to
                /// fall silent (Stop, natural completion), and in Play, where the source has
                /// to stay silent until Acquire has ruled on the new admission. Before #745
                /// the graph had no transport, so FALSE could only leave it muted-but-running.
                void ReleaseVoice(bool resumePlayback) const;

                /// Enter or leave the virtualized state on the live source: the budget's
                /// gain mute plus the graph-runtime freeze that actually reclaims the DSP
                /// cost. Order matters in both directions — see the definition.
                void SetVirtualized(bool virtualized) const;
                /// Push refreshed scoring inputs at the budget.
                void SyncVoiceParams() const;
                /// Apply m_Volume scaled by the budget's mute state to the live graph.
                void ApplyEffectiveGain() const;

                /* Stop playback with short fade-out to prevent click.
                @param numSamples - length of the fade-out in PCM frames

                @returns true - if successfully initialized fade
                */
                bool StopFade(u64 numSamples);

                /* Stop playback with short fade-out to prevent click.
                @param milliseconds - length of the fade-out in milliseconds

                @returns true - if successfully initialized fade
                */
                bool StopFade(i32 milliseconds);

                enum class StopOptions : u16
                {
                    None = 0,
                    NotifyPlaybackComplete = (1 << 0),
                    ResetPlaybackPosition = (1 << 1)
                };

                // Bitwise operators for StopOptions
                inline friend StopOptions operator|(StopOptions lhs, StopOptions rhs)
                {
                    return static_cast<StopOptions>(std::to_underlying(lhs) | std::to_underlying(rhs));
                }

                inline friend StopOptions operator&(StopOptions lhs, StopOptions rhs)
                {
                    return static_cast<StopOptions>(std::to_underlying(lhs) & std::to_underlying(rhs));
                }

                inline friend StopOptions operator~(StopOptions opt)
                {
                    return static_cast<StopOptions>(~std::to_underlying(opt));
                }

                inline friend StopOptions& operator|=(StopOptions& lhs, StopOptions rhs)
                {
                    lhs = lhs | rhs;
                    return lhs;
                }

                inline friend StopOptions& operator&=(StopOptions& lhs, StopOptions rhs)
                {
                    lhs = lhs & rhs;
                    return lhs;
                }

                /* "Hard-stop" playback without fade. This is called to immediately stop playback,
                as well as to reset the play state when "stop-fade" has ended.
                @param options - combination of StopOptions flags

                @returns voice ID of the sound source in pool
                */
                i32 StopNow(StopOptions options = StopOptions::None);

                void InitializeEffects(const Ref<SoundConfig>& config);

                /* Push a high-level control value into the live graph as a conventionally-named
                   graph input parameter (see the kXxxParam names in the .cpp). Best-effort: a
                   graph that doesn't expose the endpoint simply ignores the write, and the call
                   is a no-op until a source + graph are installed.

                   const because these mutate the LIVE GRAPH, not this wrapper's own logical
                   state — which is what lets the const-qualified IVoiceHost callbacks reach
                   them without a const_cast. */
                void RouteFloatParameter(std::string_view parameterName, f32 value) const;
                void RouteBoolParameter(std::string_view parameterName, bool value) const;

                /* Re-push every stored high-level control (volume / pitch / looping / filters)
                   into the current graph. Called after a graph is installed so a value set
                   before the graph existed (or before a graph swap) still takes effect. */
                void SyncControlParametersToGraph();

                // Audio frequency conversion utilities
                static f32 NormalizedToFrequency(f32 normalizedValue);
                static f32 FrequencyToNormalized(f32 frequency);

                /// The single conversion from the stored miniaudio-flavoured priority
                /// (0 = highest) to the 0..1, higher-is-more-important convention every
                /// consumer wants. Shared so the two call sites cannot drift apart.
                [[nodiscard]] static f32 NormalizePriority(u8 priority);

                /* Push the current spatial state (position/orientation/up + velocity) into the
                   source's spatializer node. No-op until the source hosts one. Called from each
                   3D setter. */
                void SyncSpatialPositionToSource();

              private:
                friend class AudioEngine;
                friend class SourceManager;

                std::function<void()> m_OnPlaybackComplete;
                std::string m_DebugName;

                SoundPlayState m_PlayState{ SoundPlayState::Stopped };
                SoundPlayState m_NextPlayState{ SoundPlayState::Stopped };

                /* Data source. SoundGraphSource handles the audio processing and miniaudio integration */
                Scope<Audio::SoundGraph::SoundGraphSource> m_Source = nullptr;

                // Note: MiniAudio nodes moved to SoundGraphSource to avoid header conflicts
                // Effects chain handled internally by SoundGraphSource

                // Playback status
                u8 m_Priority = 128; // 0 = highest priority, 255 = lowest

                // Voice-budget state (issue #730). Mutable because IVoiceHost's callbacks
                // are const-qualified (AudioSource, the other implementation, needs them
                // that way — see VoiceManager.h).
                /// Atomic: written by Play/Stop on the owner's thread, read by the budget
                /// callbacks that Scene::UpdateAudio drives from a worker.
                mutable std::atomic<OloEngine::Audio::VoiceHandle> m_VoiceHandle{ OloEngine::Audio::kInvalidVoiceHandle };
                /// 0 while virtualized, 1 while audible. Multiplied into m_Volume on the
                /// way to the graph so the authored volume survives a steal. Atomic because
                /// OnVoiceStart/OnVoiceStop write it from whichever thread ticks the budget
                /// while SetVolume reads it from the owner's.
                mutable std::atomic<f32> m_VoiceGainScale{ 1.0f };

                /* Stored Fader "resting" value. Used to restore Fader before restarting playback if a fade has occurred. */
                f32 m_StoredFaderValue = 1.0f;
                f32 m_LastFadeOutDuration = 0.0f;

                f32 m_Volume = 1.0f;
                f32 m_Pitch = 1.0f;

                /* Stop-fade counter. Used to stop the sound after "stopping-fade" has finished. */
                f32 m_StopFadeTime = 0.0f;

                // Filter states
                f32 m_LowPassValue = 1.0f;  // Normalized filter value
                f32 m_HighPassValue = 0.0f; // Normalized filter value

                // Spatial audio properties
                glm::vec3 m_Position{ 0.0f };
                glm::vec3 m_Orientation{ 0.0f, 0.0f, -1.0f }; // forward; matches Audio::Transform default
                glm::vec3 m_Up{ 0.0f, 1.0f, 0.0f };
                glm::vec3 m_Velocity{ 0.0f };
                bool m_SpatializationEnabled = true; // 3D spatializer routing on by default (issue #424)

                // Status flags
                bool m_IsReadyToPlay = false;
                bool m_IsStopping = false;
                bool m_IsLooping = false;
                bool m_IsFinished = false;
                f32 m_CurrentFadeVolume = 1.0f;

                // Fade control
                bool m_IsFading = false;
                f32 m_FadeStartVolume = 1.0f;
                f32 m_FadeTargetVolume = 1.0f;
                f32 m_FadeDuration = 0.0f;
                f32 m_FadeCurrentTime = 0.0f;
            };

        } // namespace SoundGraph
    } // namespace Audio

} // namespace OloEngine
