#pragma once

#include "OloEngine/Core/Base.h"

#include <string_view>

namespace OloEngine
{
    class Scene;
    class Skeleton;
    struct MorphTargetComponent;
} // namespace OloEngine

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
        /// The morphed rest surface this frame is not the one the last frame drew
        /// (#1227). The shaders reproject the previous pose through the CURRENT
        /// rest position, so a rest surface that moved cannot be expressed as a
        /// velocity at all — see MorphDeformationSystem.
        MorphSurfaceChanged,
        /// The bound MorphTargetSet was replaced, so the previous weight vector
        /// indexes a different set of targets and is not comparable.
        MorphSetChanged,
        /// The mesh being drawn changed — a conventional LOD switch. Different
        /// vertex count and different topology, so the previous surface is a
        /// different surface (#1227).
        MeshTopologyChanged,
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
        // --- Per frame. Cleared at the top of every AdvanceHistory. ---

        /// Skinned entities whose history was advanced this frame.
        u32 SkeletonsAdvanced = 0;
        /// Of those, how many carried a genuine previous pose afterwards.
        u32 SkeletonsWithHistory = 0;
        /// Bone matrices advanced this frame, across all skeletons.
        u32 BoneMatricesAdvanced = 0;
        /// Morphing entities whose morph-weight history was advanced this frame.
        u32 MorphSurfacesAdvanced = 0;
        /// Of those, how many carried a genuine previous weight vector afterwards.
        u32 MorphSurfacesWithHistory = 0;
        /// Morphing entities whose surface moved this frame, so their deformation
        /// history was rejected rather than reprojected.
        u32 MorphSurfacesRejected = 0;

        // --- Cumulative for the session. NOT cleared per frame. ---
        //
        // Resets are rare, deliberate events, and clearing them every frame made
        // them unobservable: an explicit reset at a scene or play-mode
        // transition was wiped by the very next frame's advance, so the editor
        // panel and olo_skeletal_deformation_stats could never show a
        // SceneTransition or Teleport however hard anyone looked. A counter
        // nobody can ever read is worse than no counter, because it reads as
        // "this never happens".

        u32 HistoryResets = 0;
        u32 HistoryResetsFirstUse = 0;
        u32 HistoryResetsBoneCountChanged = 0;
        u32 HistoryResetsExplicit = 0;
        u32 HistoryResetsMorphSurfaceChanged = 0;
        u32 HistoryResetsMorphSetChanged = 0;
        u32 HistoryResetsMeshTopologyChanged = 0;
        /// Cause of the most recent reset, for the statistics panel.
        DeformationHistoryResetCause LastResetCause = DeformationHistoryResetCause::None;

        // --- Malformed morph input, cumulative for the session (#1227). ---
        //
        // "A path that cannot do its job says so loudly and countably": each of
        // these is an input the morph producer refused, and a refusal nobody can
        // count reads as "this never happens".

        /// Authored weights naming a target the bound set does not have.
        u32 MorphUnknownTargets = 0;
        /// Morph sets refused because they do not span the mesh they are bound to.
        u32 MorphIncompatibleSets = 0;
        /// Base-surface caches dropped because the mesh behind them changed.
        u32 MorphBaseCacheInvalidations = 0;

        /// Clear the per-frame counters only. Called once per frame.
        void BeginFrame()
        {
            SkeletonsAdvanced = 0;
            SkeletonsWithHistory = 0;
            BoneMatricesAdvanced = 0;
            MorphSurfacesAdvanced = 0;
            MorphSurfacesWithHistory = 0;
            MorphSurfacesRejected = 0;
        }

        /// Clear everything, including the session totals.
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

        // Malformed morph input, counted where it is refused (#1227). These are
        // session totals on the same block as the bone counters, because the two
        // halves are one surface and a reader comparing them should not have to
        // find two places.
        static void NoteMorphUnknownTargets(u32 count);
        static void NoteMorphIncompatibleSet();
        static void NoteMorphBaseCacheInvalidated();
    };

    /**
     * @brief The morph half of the one shared animated surface (#1227).
     *
     * Deliberately the same vocabulary, the same counters and the same frame
     * boundary as SkeletalDeformationSystem above rather than a parallel morph
     * path, because the whole point of the surface is that raster, depth, shadows
     * and velocity refer to ONE deformed vertex. Morph deltas are applied to the
     * rest surface on the CPU and the skin matrix is then applied to that morphed
     * rest surface on the GPU, so the combination order is morph-then-skin and it
     * is the same order for every consumer by construction: every pass reads the
     * one vertex buffer the morph pass wrote.
     *
     * What this system owns is the part the GPU producer cannot see.
     * OloDeformSkinnedVertex builds its previous-pose position as
     * `prevSkinMatrix * restPosition`, where `restPosition` is whatever is in the
     * vertex buffer THIS frame — i.e. the surface as morphed this frame. When the
     * morph weights move, that previous position is a hybrid: last frame's pose on
     * this frame's surface. The honest answer is not a velocity but an explicit
     * history rejection, which is the second branch the issue's acceptance
     * criterion allows, and it is what this system decides.
     *
     * (The first branch — a real morph velocity — needs the PREVIOUS morphed rest
     * position in the vertex stage, which is a second per-draw vertex stream. Both
     * engine-wide vertex-pull bindings are taken, so that is not a change this
     * issue can make; the rejection is counted and attributed instead of silently
     * emitting a bone-only velocity across a surface that moved.)
     */
    class MorphDeformationSystem
    {
      public:
        /**
         * @brief Advance every morphing entity's weight history by one frame.
         *
         * Call once per frame, at the frame boundary, from every entry point that
         * calls SkeletalDeformationSystem::AdvanceHistory — and for the same
         * reason: a morphing entity that is paused must advance into
         * prev == current and emit zero motion rather than keep re-emitting the
         * last delta. SkeletalDeformationContract pins that pairing over the call
         * sites, because a history pass wired into only some of the frame entry
         * points is invisible in every test.
         *
         * @return the number of morphing entities advanced.
         */
        static u32 AdvanceHistory(Scene* scene);

        /**
         * @brief Drop every morphing entity's history, attributing the cause.
         * @return the number of entities reset.
         */
        static u32 ResetHistory(Scene* scene, DeformationHistoryResetCause cause);
    };

    /**
     * @brief Throw away one entity's deformation history, both halves, with a cause.
     *
     * For a discontinuity discovered per entity rather than scene-wide: an LOD
     * switch, a morph set swap, a morphed surface that moved. Either argument may
     * be null — an entity can be skinned, morphing, or both, and the history is
     * only meaningful where it exists.
     *
     * Holding prev equal to current is what makes the next frame emit exactly zero
     * motion; the alternative is a velocity measured between two different
     * surfaces, which TAA and motion blur faithfully smear.
     */
    void RejectDeformationHistory(Skeleton* skeleton, MorphTargetComponent* morph,
                                  DeformationHistoryResetCause cause);
} // namespace OloEngine::Animation
