#pragma once

#include <glm/vec3.hpp>
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Audio/VoiceManager.h"

struct ma_sound;

namespace OloEngine
{
    namespace Audio::DSP
    {
        class LowPassFilter;
        class HighPassFilter;
        class Reverb;
        class Spatializer;
    } // namespace Audio::DSP

    enum class AttenuationModelType
    {
        None = 0,
        Inverse,
        Linear,
        Exponential
    };

    struct AudioSourceConfig
    {
        f32 VolumeMultiplier = 1.0f;
        f32 PitchMultiplier = 1.0f;
        /// Voice-budget importance in [0, 1] (issue #730). Combined with the current gain
        /// and the distance to the listener into the runtime score that decides which
        /// voices keep a slot when more sounds are triggered than the budget allows.
        /// 0.5 is the neutral default: authored content only needs to move a sound off it
        /// when that sound must survive (dialogue, a boss stinger) or must yield first
        /// (an ambient loop, a footstep).
        f32 Priority = 0.5f;
        bool PlayOnAwake = true;
        bool Looping = false;

        bool Spatialization = false;
        AttenuationModelType AttenuationModel = AttenuationModelType::Inverse;
        f32 RollOff = 1.0f;
        f32 MinGain = 0.0f;
        f32 MaxGain = 1.0f;
        f32 MinDistance = 0.3f;
        f32 MaxDistance = 1000.0f;

        f32 ConeInnerAngle = glm::radians(360.0f);
        f32 ConeOuterAngle = glm::radians(360.0f);
        f32 ConeOuterGain = 0.0f;

        f32 DopplerFactor = 1.0f;

        // VBAP spatialization parameters
        f32 Spread = 1.0f; // VBAP virtual source spread [0,1]
        f32 Focus = 1.0f;  // VBAP channel focus [0,1]

        // DSP filter parameters
        f32 LowPassCutoff = 1.0f;  // Normalized [0,1], 1.0 = 20 kHz (bypassed)
        f32 HighPassCutoff = 0.0f; // Normalized [0,1], 0.0 = 20 Hz (bypassed)
        f32 ReverbSend = 0.0f;     // Reverb send level [0,1], 0.0 = no reverb
    };

    /// A clip-backed voice.
    ///
    /// Play()/Stop() are the engine's single admission point for the clip path (issue
    /// #730): every caller — Scene::InitAudioRuntime, SceneStreamer, the C# and Lua script
    /// glue, AudioEventsManager — reaches the concurrent-voice budget through here rather
    /// than each call site being wired up separately. Play() therefore does NOT guarantee
    /// audible output: if the mix is full of higher-scoring voices this source starts
    /// *virtual* — logically running, silent, and resumed at the right position (and, for
    /// a loop, the right phase) as soon as a slot frees.
    class AudioSource : public RefCounted, public Audio::IVoiceHost
    {
      public:
        AudioSource(const char* filepath);
        ~AudioSource() override;

        AudioSource(const AudioSource& other) = default;
        AudioSource(AudioSource&& other) = default;

        [[nodiscard("Store this!")]] const char* GetPath() const
        {
            return m_Path.c_str();
        }

        /// Request playback. Registers a voice with the budget; the source becomes audible
        /// immediately only if it earns a slot (see the class comment).
        void Play() const;
        void Pause() const;
        void UnPause() const;
        void Stop() const;
        /// True while the source is logically running — audible OR virtualized. A
        /// virtualized voice must answer true here or the owners that reap finished
        /// sounds (AudioEventsManager) would delete a sound that is merely inaudible.
        [[nodiscard("Store this!")]] bool IsPlaying() const;
        /// True only while the source actually holds one of the budget's voice slots.
        [[nodiscard("Store this!")]] bool IsAudible() const;
        /// True while the source is registered with the budget but not audible.
        [[nodiscard("Store this!")]] bool IsVirtualized() const;

        /// Clip length in seconds; 0 when the backend cannot report one.
        [[nodiscard("Store this!")]] f64 GetLengthSeconds() const;
        /// Current playback position in seconds. Tracked by the voice budget while
        /// virtualized, so it keeps advancing (and wraps for a loop) while inaudible.
        [[nodiscard("Store this!")]] f64 GetPlaybackPositionSeconds() const;

        void SetConfig(const AudioSourceConfig& config);
        /// Voice-budget importance in [0, 1]; see AudioSourceConfig::Priority.
        void SetPriority(f32 priority) const;

        void SetVolume(const f32 volume) const;
        void SetPitch(const f32 pitch) const;
        void SetLooping(const bool state) const;
        void SetSpatialization(const bool state);
        void SetAttenuationModel(const AttenuationModelType type) const;
        void SetRollOff(const f32 rollOff) const;
        void SetMinGain(const f32 minGain) const;
        void SetMaxGain(const f32 maxGain) const;
        void SetMinDistance(const f32 minDistance) const;
        void SetMaxDistance(const f32 maxDistance) const;
        void SetCone(const f32 innerAngle, const f32 outerAngle, const f32 outerGain) const;
        void SetDopplerFactor(const f32 factor) const;

        void SetPosition(const glm::vec3& position) const;
        void SetDirection(const glm::vec3& forward) const;
        void SetVelocity(const glm::vec3& velocity) const;

        // DSP controls
        void SetLowPassCutoff(f32 normalizedCutoff);
        void SetHighPassCutoff(f32 normalizedCutoff);
        void SetReverbSend(f32 sendLevel);

        void InitializeDSP();
        void UninitializeDSP();

        //--- Audio::IVoiceHost ---------------------------------------------------------
        // Driven by the voice budget, never called directly. See VoiceManager.h for the
        // locking contract.
        bool OnVoiceStart(f64 positionSeconds) const override;
        f64 OnVoiceStop() const override;
        f64 OnVoiceQueryPosition() const override;

      private:
        /// Snapshot the current scoring inputs for the budget.
        [[nodiscard]] Audio::VoiceParams BuildVoiceParams() const;
        /// Hand the slot back and forget the handle. Idempotent.
        void ReleaseVoice() const;
        /// Push refreshed scoring inputs (position/gain/priority moved) at the budget.
        void SyncVoiceParams() const;

      private:
        std::string m_Path;
        Scope<ma_sound> m_Sound;
        bool m_Spatialization = false;

        // Voice-budget state (issue #730). Mutable because the playback and 3D setters are
        // const-qualified — the class already treats the backing ma_sound as mutable state
        // behind const methods, and the const-ness is load-bearing at the call sites
        // (AudioEventsManager iterates its source table by const reference).
        mutable Audio::VoiceHandle m_VoiceHandle = Audio::kInvalidVoiceHandle;
        /// Last position pushed through SetPosition — the spatial input the score needs.
        /// miniaudio has no getter that survives a stopped/virtual voice, so mirror it.
        mutable glm::vec3 m_Position{ 0.0f };
        /// Last config applied via SetConfig, kept so a voice can be re-scored without the
        /// owning component (the budget outlives any single frame's component access).
        mutable AudioSourceConfig m_Config;
        /// Clip length, resolved once. Scene::UpdateAudio re-scores every source every
        /// frame, and asking the backend for a length on each of those is a decoder query
        /// per source per frame for a value that cannot change. -1 = not yet resolved.
        mutable f64 m_LengthSecondsCache = -1.0;

        // DSP chain (lazily initialized when parameters change from defaults)
        Scope<Audio::DSP::LowPassFilter> m_LowPassFilter;
        Scope<Audio::DSP::HighPassFilter> m_HighPassFilter;
        void* m_SplitterNode = nullptr; // ma_splitter_node* — type-erased because ma_splitter_node is an anonymous struct typedef in miniaudio, cannot forward-declare
        bool m_DSPInitialized = false;
    };
} // namespace OloEngine
