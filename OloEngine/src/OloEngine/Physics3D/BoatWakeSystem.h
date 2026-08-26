#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/Map.h"

#include <glm/glm.hpp>

#include <array>

namespace OloEngine
{
    class Scene;

    /// One recorded hull pose. Written after the physics fence, so the pose is
    /// the one this tick actually ended at rather than the one it started from.
    struct BoatWakeSample
    {
        glm::vec2 m_WorldXZ{ 0.0f };   ///< hull centre, absolute world XZ
        glm::vec2 m_ForwardXZ{ 0.0f }; ///< unit horizontal heading at that moment
        f32 m_ForwardSpeed = 0.0f;     ///< signed m/s along m_ForwardXZ
        f32 m_TimeSeconds = 0.0f;      ///< scene simulation time of the sample
        bool m_Valid = false;
    };

    // =========================================================================
    // BoatWakeTrail — the bounded, time-stamped hull-pose history for one boat
    // (issue #967).
    //
    // A fixed-capacity ring with FIFO eviction: pushing into a full ring drops
    // the OLDEST sample and nothing else. Fixed capacity rather than a growable
    // buffer because this is per-entity state that lives for the whole session
    // — a growable one is a slow leak on any boat that never stops sailing, and
    // the leak only shows up in a long play session, which no test runs.
    //
    // Capacity is sized from what actually reads the history: the V-arm lookup
    // reaches back kArmAgeMaxSeconds (1.5 s), so the ring must span that at the
    // FASTEST plausible tick rate — a higher rate means more samples for the
    // same wall-clock window, so that is the demanding direction. 256 samples
    // covers 1.5 s up to ~170 Hz, and 4.2 s at 60 Hz.
    //
    // Running short is graceful rather than wrong: `AtAge` reports invalid past
    // the end of the history and the affected arm segment is simply not laid
    // that frame, which is also what happens for a boat that has only just
    // started moving.
    //
    // Header-only and free of Scene/Jolt so WaterDisturbanceTrailTest can drive
    // it headlessly — the eviction behaviour is an acceptance criterion, and a
    // ring that can only be exercised through a live physics scene is one that
    // does not get tested.
    // =========================================================================
    class BoatWakeTrail
    {
      public:
        static constexpr u32 kCapacity = 256;

        void Push(const BoatWakeSample& sample) noexcept
        {
            m_Samples[m_Head] = sample;
            m_Head = (m_Head + 1u) % kCapacity;
            if (m_Count < kCapacity)
                ++m_Count;
            else
                ++m_Evicted; // the oldest sample just fell out of the ring
        }

        [[nodiscard]] u32 Count() const noexcept
        {
            return m_Count;
        }
        [[nodiscard]] u64 EvictedCount() const noexcept
        {
            return m_Evicted;
        }

        /// `age` 0 is the newest sample, 1 the one before it, and so on.
        /// Returns an invalid sample past the end rather than wrapping — a ring
        /// that silently wraps a too-large index hands the caller the NEWEST
        /// sample dressed as an old one, which reads as the wake's arms
        /// collapsing onto the hull.
        [[nodiscard]] BoatWakeSample At(u32 age) const noexcept
        {
            if (age >= m_Count)
                return {};
            const u32 index = (m_Head + kCapacity - 1u - age) % kCapacity;
            return m_Samples[index];
        }

        /// Newest sample at least `seconds` older than `now`, or an invalid
        /// sample when the history does not yet reach that far back.
        [[nodiscard]] BoatWakeSample AtAge(f32 now, f32 seconds) const noexcept
        {
            for (u32 age = 0; age < m_Count; ++age)
            {
                const BoatWakeSample s = At(age);
                if (s.m_Valid && (now - s.m_TimeSeconds) >= seconds)
                    return s;
            }
            return {};
        }

        void Clear() noexcept
        {
            m_Head = 0;
            m_Count = 0;
            m_Evicted = 0;
        }

      private:
        std::array<BoatWakeSample, kCapacity> m_Samples{};
        u32 m_Head = 0;
        u32 m_Count = 0;
        u64 m_Evicted = 0;
    };

    // =========================================================================
    // BoatWakeSystem — turns boat motion into generic water disturbances
    // (issue #967).
    //
    // Runs AFTER the physics fence, records a bounded history of hull poses and
    // forward speeds, and submits splats through
    // WaterDisturbanceSystem::SubmitSplat.
    //
    // The direction of the dependency is the point: this system knows about
    // both boats and the disturbance service, and the disturbance service knows
    // about neither boats nor physics. A propeller, an impact, or a scripted
    // splash submits through the same API with no BoatComponent in sight —
    // which is an acceptance criterion of #967, not just tidiness.
    //
    // What it emits per boat per frame:
    //   * the HULL churn — one capsule swept from the previous recorded pose to
    //     this one, so a fast boat cannot leave a dotted trail;
    //   * the two V-ARMS — a chain of capsules either side of the trail,
    //     sampled at kArmAgeSamples ages spanning [kArmAgeMin, kArmAgeMax] and
    //     offset laterally by an amount computed from EACH sample's own age.
    //
    //     The age RANGE is what makes the V diverge, and getting this wrong is
    //     easy: an earlier version stamped one arm pair per frame at a single
    //     age, reasoning that each patch of water would be stamped once, when
    //     it reached that age. But `AtAge` returns the newest sample at least
    //     that old, so the age — and therefore the offset — was the SAME every
    //     frame, and the "diverging V" was two parallel lines at a fixed offset.
    //     It rendered perfectly and looked like a wake, which is why the code
    //     and its own comment disagreed for a while without anything failing.
    //
    //     Sampling a range instead means one patch of water is re-stamped at a
    //     growing offset as it ages, which is what actually traces a widening V.
    //     `max()` combining makes the repeats idempotent. It follows an S-turn
    //     because the historical headings do.
    //   * the PROPELLER wash — a point splat at the stern, gated on throttle
    //     rather than speed, so a boat holding station against a current still
    //     churns.
    // =========================================================================
    class BoatWakeSystem
    {
      public:
        /// Forward speed at which the wake reaches full strength. Below
        /// kMinSpeed there is no wake at all — a drifting hull does not foam.
        static constexpr f32 kMinSpeedMetresPerSecond = 0.6f;
        static constexpr f32 kFullSpeedMetresPerSecond = 6.0f;

        /// The age RANGE the V-arms are laid over, and how fast they diverge.
        ///
        /// A range, not a single age, and that is the whole reason the arms
        /// spread at all — see the class comment below. At 1.6 m/s of spread
        /// these produce a half-angle of roughly 15 degrees, against the ~19.5
        /// of a real Kelvin wake.
        static constexpr f32 kArmAgeMinSeconds = 0.25f;
        static constexpr f32 kArmAgeMaxSeconds = 1.5f;
        /// Samples taken across that range each frame. Consecutive samples are
        /// joined by a capsule, so this is (kArmAgeSamples - 1) segments per
        /// side — 6 splats per boat per frame at 4.
        static constexpr u32 kArmAgeSamples = 4;
        static constexpr f32 kArmSpreadMetresPerSecond = 1.6f;

        /// Record this tick's hull poses and submit the resulting splats.
        ///
        /// Must run AFTER the physics fence: the wake belongs to where the hull
        /// ENDED this tick. Reading a pre-physics pose lays the trail one tick
        /// behind the boat, which at 15 m/s is a quarter of a metre of visible
        /// offset between the hull and its own foam.
        ///
        /// `history` is caller-owned (Scene holds separate runtime and editor
        /// maps, mirroring the snow deformers) so it dies with the scene rather
        /// than persisting into the next one.
        static void OnUpdate(Scene* scene, TMap<u64, BoatWakeTrail>& history, f32 simulationTime,
                             f32 deltaTime);
    };
} // namespace OloEngine
