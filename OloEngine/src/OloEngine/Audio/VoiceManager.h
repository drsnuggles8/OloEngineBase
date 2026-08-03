#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/vec3.hpp>

#include <mutex>
#include <vector>

namespace OloEngine::Audio
{
    /// Opaque per-playing-instance identity handed out by VoiceManager::Acquire.
    /// Monotonic and never reused within a process run, so a stale handle held by a
    /// released voice can never alias a later one.
    using VoiceHandle = u64;
    inline constexpr VoiceHandle kInvalidVoiceHandle = 0;

    /// Everything the budget policy needs in order to rank a voice. Deliberately plain
    /// data with no miniaudio (or any backend) dependency — this is what makes the whole
    /// scoring / stealing / virtualization decision unit headless-testable (issue #730,
    /// acceptance criterion 3). A real backend voice can't be constructed without an
    /// audio device, so the policy must not need one.
    struct VoiceParams
    {
        /// Authored importance in [0, 1]; 1 = never steal this if anything else will do.
        f32 Priority = 0.5f;
        /// Current gain multiplier. A voice already faded to silence scores near zero and
        /// is the natural first steal victim.
        f32 Volume = 1.0f;
        /// When false the voice is 2D (music, UI) and distance does not attenuate its score.
        bool Spatialized = false;
        glm::vec3 Position{ 0.0f };
        /// Distance attenuation window; mirrors AudioSourceConfig's Min/MaxDistance so the
        /// score falls off in step with what the listener actually hears.
        f32 MinDistance = 1.0f;
        f32 MaxDistance = 1000.0f;
        bool Looping = false;
        /// Playback rate; the logical position of a virtualized voice advances by
        /// deltaSeconds * Pitch so it resumes in the right *phase*, not merely playing.
        f32 Pitch = 1.0f;
        /// Total length in seconds. 0 means "unknown / endless" (a streaming or
        /// procedurally generated voice) — such a voice never auto-completes while virtual.
        f64 DurationSeconds = 0.0;
    };

    /// The backend side of a voice. VoiceManager owns no audio objects; it drives this
    /// interface when a voice is admitted, stolen, or brought back.
    ///
    /// Every method is const because the two real implementations (AudioSource,
    /// SoundGraphSound) start/stop through const-qualified backend calls; the mutated
    /// state on those classes is `mutable`. That keeps VoiceManager free of const_cast.
    ///
    /// CONTRACT: OnVoiceQueryPosition is called while the manager's lock is held and must
    /// NOT call back into VoiceManager. OnVoiceStart / OnVoiceStop are always invoked with
    /// the lock released, so they may.
    class IVoiceHost
    {
      public:
        virtual ~IVoiceHost() = default;

        /// Begin real playback at positionSeconds (0 for a fresh start, the retained
        /// logical position when a virtualized voice is brought back). Return false if the
        /// backend refused; the manager then leaves the voice virtual.
        virtual bool OnVoiceStart(f64 positionSeconds) const = 0;

        /// Stop real playback because the voice lost its slot. Return the backend playback
        /// position at the moment of stopping, or a negative value if the backend cannot
        /// report one — the manager then keeps advancing its own logical position instead.
        virtual f64 OnVoiceStop() const = 0;

        /// Live backend playback position in seconds, or negative when the backend does not
        /// track one. Only consulted while the voice is actually audible.
        virtual f64 OnVoiceQueryPosition() const
        {
            return -1.0;
        }
    };

    enum class VoiceState : u8
    {
        /// Audible: occupies one of the MaxVoices slots.
        Playing,
        /// Inaudible but still logically running — its playback position keeps advancing so
        /// it can be brought back at the correct phase.
        Virtual
    };

    struct VoiceStats
    {
        u32 Playing = 0;
        u32 Virtual = 0;
        u32 MaxVoices = 0;
        /// Cumulative counters, for the editor's audio panel and for tests that need to
        /// distinguish "never started" from "started and then stolen".
        u64 Steals = 0;
        u64 Virtualizations = 0;
        u64 Devirtualizations = 0;
        u64 Completions = 0;
    };

    /// Fixed-size concurrent-voice budget with priority/distance-based stealing and
    /// virtualization (issue #730).
    ///
    /// The invariant this type exists to hold: the number of voices in VoiceState::Playing
    /// never exceeds GetMaxVoices(). Everything else — scoring, the steal victim choice,
    /// promoting a virtual voice back once a slot frees — is policy layered on top.
    ///
    /// Thread-safety: every public method takes an internal mutex. Scene::UpdateAudio runs
    /// on a worker thread (it is `.Parallelizable()` in the gameplay scheduler) while
    /// scripts start sounds from the game thread, so this is not optional.
    class VoiceManager
    {
      public:
        /// Default concurrent-voice cap. Sized well under the point where a software mixer
        /// becomes the frame's bottleneck while staying above any plausible authored scene.
        static constexpr u32 kDefaultMaxVoices = 32;

        /// A virtual voice must beat the worst audible voice by more than this before it
        /// takes the slot. Without the margin, two voices with near-identical scores swap
        /// every single tick — an audible stutter, not a mix improvement.
        static constexpr f32 kDefaultPromotionMargin = 0.02f;

        VoiceManager() = default;
        VoiceManager(const VoiceManager&) = delete;
        auto operator=(const VoiceManager&) -> VoiceManager& = delete;

        /// Process-wide instance used by the engine's playback paths. Constructed on first
        /// use and independent of AudioEngine::Init, so a headless test (and a scene loaded
        /// with no audio device) sees a working budget.
        [[nodiscard]] static VoiceManager& Get();

        void SetMaxVoices(u32 maxVoices);
        [[nodiscard]] u32 GetMaxVoices() const;

        void SetPromotionMargin(f32 margin);
        [[nodiscard]] f32 GetPromotionMargin() const;

        /// The listener every spatialized voice is scored against. Pushed once per tick by
        /// Scene::UpdateAudio from the active AudioListenerComponent.
        void SetListenerPosition(const glm::vec3& position);
        [[nodiscard]] glm::vec3 GetListenerPosition() const;

        /// Register a newly started voice. Returns its handle; the voice is either audible
        /// or virtual on return (query with IsAudible). Passing a null host returns
        /// kInvalidVoiceHandle. `host` must outlive the voice — call Release before
        /// destroying it.
        VoiceHandle Acquire(const IVoiceHost* host, const VoiceParams& params);

        /// Retire a voice. Safe with kInvalidVoiceHandle or an already-released handle.
        /// Does NOT call OnVoiceStop — the caller is stopping its own backend voice; this
        /// only hands the slot back. Frees a slot, so a virtual voice may be promoted.
        void Release(VoiceHandle handle);

        /// Refresh the scoring inputs of a live voice (position/gain move every frame).
        /// Cheap: no rebalance happens here, only at Update.
        void UpdateParams(VoiceHandle handle, const VoiceParams& params);

        /// Overwrite the tracked logical playback position, e.g. after the owner seeks.
        void SetPlaybackPosition(VoiceHandle handle, f64 seconds);
        [[nodiscard]] f64 GetPlaybackPosition(VoiceHandle handle) const;

        [[nodiscard]] bool IsAudible(VoiceHandle handle) const;
        [[nodiscard]] bool IsVirtual(VoiceHandle handle) const;
        /// Audible OR virtual — i.e. "this voice is still logically running". This is what
        /// an IsPlaying() query on a source should answer, or a virtualized voice would be
        /// mistaken for a finished one and reaped.
        [[nodiscard]] bool IsActive(VoiceHandle handle) const;
        [[nodiscard]] VoiceState GetState(VoiceHandle handle) const;

        /// Advance every voice's logical playback position by deltaSeconds * Pitch, retire
        /// one-shots that finished while inaudible, rescore, and rebalance the budget.
        void Update(f32 deltaSeconds);

        /// Drop every voice without touching the hosts. For scene teardown and tests.
        void Reset();

        [[nodiscard]] VoiceStats GetStats() const;

        /// The ranking function, exposed so tests (and the editor panel) can reason about
        /// the policy directly. score = Priority * clamp(Volume) * distanceAttenuation.
        /// Result is in [0, 1]; a non-finite input scores 0 rather than poisoning the sort.
        [[nodiscard]] static f32 ComputeScore(const VoiceParams& params, const glm::vec3& listenerPosition);

      private:
        struct VoiceRecord
        {
            VoiceHandle Handle = kInvalidVoiceHandle;
            const IVoiceHost* Host = nullptr;
            VoiceParams Params;
            VoiceState State = VoiceState::Virtual;
            f64 PlaybackPosition = 0.0;
            f32 Score = 0.0f;
        };

        /// A start/stop the caller must apply once the lock is released.
        struct PendingTransition
        {
            const IVoiceHost* Host = nullptr;
            VoiceHandle Handle = kInvalidVoiceHandle;
            bool Start = false;
            f64 Position = 0.0;
        };

        [[nodiscard]] VoiceRecord* Find(VoiceHandle handle);
        [[nodiscard]] const VoiceRecord* Find(VoiceHandle handle) const;

        /// Re-derive the audible set from the current scores. Appends the resulting
        /// backend calls to `out`; the caller applies them after unlocking. Must be called
        /// with m_Mutex held.
        void Rebalance(std::vector<PendingTransition>& out);

        /// Run the queued backend calls with the lock released, then reconcile: a host that
        /// refuses to start is put back to Virtual.
        void ApplyTransitions(std::vector<PendingTransition>& transitions);

        mutable std::mutex m_Mutex;
        std::vector<VoiceRecord> m_Voices;
        u32 m_MaxVoices = kDefaultMaxVoices;
        f32 m_PromotionMargin = kDefaultPromotionMargin;
        glm::vec3 m_ListenerPosition{ 0.0f };
        VoiceHandle m_NextHandle = 1;
        u64 m_Steals = 0;
        u64 m_Virtualizations = 0;
        u64 m_Devirtualizations = 0;
        u64 m_Completions = 0;
    };
} // namespace OloEngine::Audio
