#pragma once

#include "OloEngine/Core/Base.h"

#include <string_view>

namespace OloEngine
{
    class Scene;
}

namespace OloEngine::Animation
{
    /**
     * @brief Why a skeleton's deformation history was dropped.
     *
     * Every reset is attributed. A motion vector computed across a
     * discontinuity is not a crash and not a test failure — it is a plausible
     * wrong image that shows up two subsystems away as a smear — so the only
     * defence is that resets are counted and their cause is nameable.
     *
     * Mirrors the vocabulary of TemporalHistoryInvalidationCause deliberately;
     * these are the same events seen from the skinning side, but bone history
     * is per-entity CPU state rather than a screen-space plane, so it is not
     * held in that registry.
     */
    enum class DeformationHistoryResetCause : u8
    {
        None = 0,
        /// The skeleton has no previous pose yet — first tick after creation.
        FirstUse,
        /// The bone count changed, so the previous palette is not comparable.
        BoneCountChanged,
        /// The skeleton asset behind the entity was swapped or re-bound.
        SkeletonReplaced,
        /// Scene load, play-mode entry/exit, or another wholesale state change.
        SceneTransition,
        /// The entity was moved discontinuously, or the view cut.
        Teleport,
        /// Asked for explicitly by editor tooling or a test.
        Manual,
    };

    [[nodiscard]] std::string_view ToString(DeformationHistoryResetCause cause);

    /**
     * @brief Per-frame, engine-wide counters for the shared deformation output.
     *
     * Surfaced through Renderer3D's statistics so #1227's morph targets and
     * #1228's GPU Scene surfaces can see the same numbers rather than each
     * measuring their own. Written only from the tick thread — see
     * SkeletalDeformationSystem::AdvanceHistory.
     */
    struct SkeletalDeformationStats
    {
        /// Skinned entities whose history was advanced this tick.
        u32 SkeletonsAdvanced = 0;
        /// Of those, how many carried a genuine previous pose afterwards.
        u32 SkeletonsWithHistory = 0;
        /// Bone matrices advanced this tick, across all skeletons.
        u32 BoneMatricesAdvanced = 0;
        /// Histories dropped this tick, by cause.
        u32 HistoryResets = 0;
        u32 HistoryResetsFirstUse = 0;
        u32 HistoryResetsBoneCountChanged = 0;
        u32 HistoryResetsExplicit = 0;
        /// Cause of the most recent explicit reset, for the statistics panel.
        DeformationHistoryResetCause LastResetCause = DeformationHistoryResetCause::None;

        void Reset()
        {
            *this = SkeletalDeformationStats{};
        }
    };

    /**
     * @brief The single owner of skeletal deformation history (#1226).
     *
     * The deformation output has two halves — the current pose in
     * SkeletonData::m_FinalBoneMatrices and the previous pose in
     * m_PrevFinalBoneMatrices — and every consumer (colour, depth, the raster
     * shadow techniques, velocity) reads both through the same palettes. This
     * system owns when the second half advances.
     *
     * Two things about where it runs are load-bearing.
     *
     * It advances EVERY skinned entity, not only the ones that animated.
     * Advancing inside an animation update ties history to whether the entity
     * animated, and a paused entity then keeps the last tick's delta forever:
     * prev and current stay one frame apart, so every subsequent frame emits
     * the same non-zero per-pixel motion and TAA and motion blur smear a
     * character that is standing perfectly still. Advancing unconditionally
     * makes a pause produce prev == current, and therefore zero motion, with no
     * special case anywhere.
     *
     * And it runs at the FRAME boundary, not inside the gameplay tick. Pausing
     * skips the whole gameplay schedule, so a history pass registered there
     * would stop running and freeze exactly the stale delta it exists to
     * prevent. At the frame boundary the previous pose is also the right one by
     * construction under a fixed-tick clock: whether the frame ran no ticks or
     * several, prev is the pose the last rendered frame drew and current is the
     * pose this one draws, which is what a motion vector is supposed to mean.
     */
    class SkeletalDeformationSystem
    {
      public:
        /**
         * @brief Advance every skinned entity's history by one frame.
         *
         * Call once per frame, at the top of the frame, before anything writes
         * this frame's pose — and call it whether or not the frame will run a
         * gameplay tick. Runs on the main thread ahead of both the tick and all
         * render submission, so the palettes it writes are never the ones a
         * parallel recording worker is reading.
         *
         * @return the number of skeletons advanced.
         */
        static u32 AdvanceHistory(Scene* scene);

        /**
         * @brief Drop every skinned entity's history, attributing the cause.
         *
         * For wholesale discontinuities: scene load, play-mode transitions.
         * @return the number of skeletons reset.
         */
        static u32 ResetHistory(Scene* scene, DeformationHistoryResetCause cause);

        [[nodiscard]] static const SkeletalDeformationStats& GetStats();
        static void ResetStats();

        /// Record an explicit per-entity reset performed by a caller that
        /// already holds the skeleton, so the counters stay complete.
        static void NoteExplicitReset(DeformationHistoryResetCause cause);
    };
} // namespace OloEngine::Animation
