#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <vector>
#include <string>
#include <glm/glm.hpp>

namespace OloEngine
{
    /**
     * @brief Shared skeleton data structure for bone hierarchy and transforms
     *
     * This structure contains the common data used by both Skeleton and SkeletonComponent,
     * eliminating duplication and centralizing skeleton layout management.
     */
    /// Why one AdvanceBoneHistory call ended the way it did. The caller needs
    /// this to attribute history drops: a frame that is deliberately covering an
    /// already-declared discontinuity is NOT a new drop, and counting it as one
    /// both double-counts and overwrites the real cause with a generic one.
    enum class BoneHistoryAdvance : u8
    {
        /// The previous pose is genuine; motion this frame is real motion.
        Advanced,
        /// The skeleton has no bones yet. Never had history, nothing was lost.
        NoBonesYet,
        /// The palette grew into existence for the first time.
        FirstUse,
        /// The palette changed size away from a real previous pose.
        BoneCountChanged,
        /// Covering a discontinuity that ResetBoneHistory already declared.
        PendingReset,
    };

    // The three DeformationHistoryResetCause values this header can reach
    // without including the Animation history system. Mirrored rather than
    // included, and pinned against the enum by
    // SkeletalDeformationContract.SkeletonDataMirrorsTheResetCauseOrdinals, so
    // a reordering of the enum fails a test instead of silently renaming every
    // cause a record reports.
    inline constexpr u8 kCauseNone = 0;
    inline constexpr u8 kCauseFirstUse = 1;
    inline constexpr u8 kCauseBoneCountChanged = 2;

    struct SkeletonData
    {
        // Bone hierarchy (indices, parent indices, names)
        std::vector<int> m_ParentIndices;
        std::vector<std::string> m_BoneNames;

        // Local and global transforms for each bone
        std::vector<glm::mat4> m_LocalTransforms;
        std::vector<glm::mat4> m_GlobalTransforms;

        // Final matrices for skinning (to be sent to GPU)
        std::vector<glm::mat4> m_FinalBoneMatrices;

        // Previous-frame final bone matrices — the previous-pose half of the
        // shared deformation output (#1226). Every velocity-emitting consumer
        // reads these through the PrevBoneMatrices palette at binding 31.
        //
        // Advanced exactly once per frame by SkeletalDeformationSystem, for
        // every skinned entity, whether or not it animated that frame. Do not
        // advance it from inside an animation update: a skeleton that stops
        // updating then freezes with a stale delta between prev and current and
        // emits a non-zero motion vector forever, which is what a paused
        // animation used to do here.
        std::vector<glm::mat4> m_PrevFinalBoneMatrices;

        // Whether a genuine previous pose has EVER been established. Only used to
        // attribute the first advance: a skeleton covering its very first frame is
        // reporting FirstUse, not a repeat discontinuity, and collapsing the two
        // would leave the FirstUse counter permanently at zero for every normally
        // constructed skeleton.
        bool m_BoneHistoryEverValid = false;

        // False until the deformation history has been advanced at least once
        // since the last discontinuity. Consumers MUST read this: while it is
        // false the previous palette is not a pose this skeleton was ever in,
        // and a velocity computed from it is a jump rather than motion.
        // Renderer3D honours it by uploading the current palette as the previous
        // one, which is what makes the emitted bone motion exactly zero.
        bool m_BoneHistoryValid = false;

        // The deformation REVISION pair (#1228): the canonical, consumer-facing
        // name for "which deformation of this surface is on screen". Advanced
        // by exactly one on every history advance, and HELD EQUAL to the
        // previous value on every discontinuity — so continuity is
        // `m_DeformationRevision == m_PrevDeformationRevision + 1` and a
        // discontinuity is `m_DeformationRevision == m_PrevDeformationRevision`,
        // with no third state and no extra flag to keep in sync.
        //
        // This is NOT a second source of truth for m_BoneHistoryValid: both are
        // written by AdvanceBoneHistory from the same branch, and
        // SkeletalDeformationHistoryTest pins that HasBoneHistory() and
        // HasContinuousDeformation() never disagree. The pair exists because a
        // BOOLEAN cannot answer the question a canonical record has to answer —
        // "has this surface deformed since the thing I built from it?" — which
        // is what GPU Scene's instance record carries for the raster consumer
        // and what #1229's acceleration-structure refit will ask.
        //
        // u32 and compared with modular arithmetic on purpose: at 60 fps the
        // counter wraps after ~2.2 years of continuous runtime, and because
        // `prev + 1` wraps to 0 in u32 arithmetic exactly when `current` does,
        // continuity survives the wrap rather than reporting one spurious
        // discontinuity.
        u32 m_DeformationRevision = 0;
        u32 m_PrevDeformationRevision = 0;

        // Why the CURRENT discontinuity happened, as the u8 underlying
        // Animation::DeformationHistoryResetCause (#1228). A plain u8 rather
        // than the enum so this header, which every skinned consumer includes,
        // does not pull the Animation history system in behind it; the enum is
        // declared `: u8` precisely so the two spellings cannot drift in width.
        //
        // Only meaningful while HasContinuousDeformation() is false -- a
        // continuous surface has no live cause, and reporting the last one
        // would make a debug view name a discontinuity that is over. The
        // advance below therefore clears it on the continuous branch rather
        // than leaving it latched.
        u8 m_DeformationResetCause = 0;

        /**
         * @brief Attribute the discontinuity the caller is about to declare.
         *
         * Called by the entry points that KNOW the cause -- the scene-wide
         * reset and the per-entity rejection -- immediately around their
         * ResetBoneHistory() call. It is separate from ResetBoneHistory()
         * rather than a parameter on it because the advance path reaches the
         * same state without any caller holding a cause, and a defaulted
         * parameter would have quietly attributed those to whatever the default
         * was.
         */
        void NoteDeformationResetCause(u8 cause)
        {
            m_DeformationResetCause = cause;
        }

        // A discontinuity has been declared and the frame that must emit zero
        // motion because of it has not been rendered yet.
        //
        // TRUE on construction, because construction is itself that event: the
        // sized constructor fills BOTH palettes with identity, so without this the
        // first advance would find the sizes already matching, report Advanced,
        // and hand the first animated frame a motion vector measured from the
        // construction-time identity pose. That is the same one-frame jump this
        // whole flag exists to prevent, arriving through the one path that never
        // calls ResetBoneHistory.
        //
        // This exists because a reset and the next frame's advance are the SAME
        // copy, so a reset alone cannot survive: ResetBoneHistory sets prev to
        // current, then the next frame's AdvanceBoneHistory copies the still
        // unchanged pose again and would mark the history valid -- and only
        // THEN does the tick write the new pose. The result was a first play
        // frame whose velocity spanned the whole edit-pose to play-pose jump,
        // with the reset that was supposed to prevent it having done nothing.
        // Carrying the reset across exactly one advance is what closes that gap.
        bool m_BoneHistoryResetPending = true;

        // Bind pose data for proper skinning
        std::vector<glm::mat4> m_BindPoseMatrices;        // Original bind pose global transforms
        std::vector<glm::mat4> m_InverseBindPoses;        // Inverse bind pose matrices for skinning
        std::vector<glm::mat4> m_BindPoseLocalTransforms; // Original bind pose local transforms

        // Accumulated non-bone ancestor transforms per bone.
        // Between a bone and its parent bone (or scene root for root bones),
        // there may be non-bone nodes whose transforms are constant and not
        // affected by animation. This vector accumulates those transforms
        // so they can be applied when computing GlobalTransforms.
        std::vector<glm::mat4> m_BonePreTransforms;

        SkeletonData() = default;

        explicit SkeletonData(sizet boneCount)
        {
            m_ParentIndices.resize(boneCount, -1); // Initialize with -1 to indicate root bones
            m_BoneNames.resize(boneCount);
            m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));
            m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
            m_PrevFinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
            m_BindPoseMatrices.resize(boneCount, glm::mat4(1.0f));
            m_InverseBindPoses.resize(boneCount, glm::mat4(1.0f));
            m_BonePreTransforms.resize(boneCount, glm::mat4(1.0f));
        }

        /**
         * @brief Initialize bind pose from current global transforms
         */
        void SetBindPose()
        {
            const sizet boneCount = m_GlobalTransforms.size();
            if (m_LocalTransforms.size() != boneCount ||
                m_BindPoseMatrices.size() != boneCount ||
                m_InverseBindPoses.size() != boneCount ||
                m_BindPoseLocalTransforms.size() != boneCount)
            {
                m_BindPoseMatrices.resize(boneCount, glm::mat4(1.0f));
                m_InverseBindPoses.resize(boneCount, glm::mat4(1.0f));
                m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
                m_BindPoseLocalTransforms.resize(boneCount, glm::mat4(1.0f));
            }

            m_BindPoseLocalTransforms = m_LocalTransforms;
            for (sizet i = 0; i < boneCount; ++i)
            {
                m_BindPoseMatrices[i] = m_GlobalTransforms[i];
                m_InverseBindPoses[i] = glm::inverse(m_GlobalTransforms[i]);
            }
        }

        /**
         * @brief Copy the current pose into the previous-pose slot.
         *
         * The one primitive underneath both history operations: advancing a
         * frame and resetting on a discontinuity are the same copy, and they
         * differ only in what the caller means by it. Sizes the destination
         * when the bone count has changed, which is itself a discontinuity —
         * a resized palette has no comparable previous pose.
         *
         * @return true when the copy also had to resize, i.e. the previous
         *         contents were not a comparable pose.
         */
        bool CopyPoseToHistory()
        {
            const sizet boneCount = m_FinalBoneMatrices.size();
            if (m_PrevFinalBoneMatrices.size() != boneCount)
            {
                m_PrevFinalBoneMatrices.assign(m_FinalBoneMatrices.begin(), m_FinalBoneMatrices.end());
                return true;
            }
            // std::ranges::copy lets the compiler pick the best vectorised path for POD mat4 data.
            std::ranges::copy(m_FinalBoneMatrices, m_PrevFinalBoneMatrices.begin());
            return false;
        }

        /**
         * @brief Advance the deformation history by one tick.
         *
         * Call once per frame per skinned entity, before the pose for this
         * frame is written, and call it for every skinned entity rather than
         * only the ones that are playing. A skeleton that is paused then
         * advances into prev == current and emits zero motion, which is what a
         * paused character should do; a skeleton skipped while paused keeps
         * emitting last frame's delta and smears under TAA and motion blur for
         * as long as the pause lasts.
         *
         * SkeletalDeformationSystem::AdvanceHistory is the single caller in the
         * engine. It runs on the main thread at the frame boundary, before any
         * render submission, so the palette this reads is never the one a
         * recording worker is reading.
         */
        BoneHistoryAdvance AdvanceBoneHistory()
        {
            // EVER valid, not valid right now: a bone-count change on a frame whose
            // history is already suppressed (the frame after a reset, say) is still
            // a real skeleton swap and still deserves the loud warning. Reading the
            // per-frame flag here reported it as FirstUse and stayed silent.
            const bool hadHistory = m_BoneHistoryEverValid;
            const bool resized = CopyPoseToHistory();

            // Hold the revision first, bump it only on the Advanced branch
            // below (#1228). Written here rather than in each of the five
            // returns because every OTHER outcome of this function is a
            // discontinuity, and "hold" is what a discontinuity means: a
            // branch added later is discontinuous unless it says otherwise,
            // which is the safe default for a velocity.
            m_PrevDeformationRevision = m_DeformationRevision;

            // An empty palette is not history. Treating 0 == 0 as "nothing was
            // resized" would mark a skeleton whose bones have not loaded yet as
            // carrying a genuine previous pose, and the frame its palette
            // finally arrives would then be reported as a bone-count CHANGE --
            // a warning meant for a real skeleton swap, fired on a routine
            // deferred load.
            if (m_FinalBoneMatrices.empty())
            {
                m_BoneHistoryValid = false;
                m_BoneHistoryResetPending = false;
                // FirstUse, not a lost history: there was never a pose here.
                m_DeformationResetCause = static_cast<u8>(kCauseFirstUse);
                return BoneHistoryAdvance::NoBonesYet;
            }

            if (resized)
            {
                // No pending flag here: the resize ITSELF just set prev to current,
                // and clearing the valid flag already suppresses this frame. Arming
                // pending as well would suppress the following frame too and report
                // the same FirstUse twice, which is what a deferred-loaded skeleton
                // (palette sized after its first tick) used to do.
                m_BoneHistoryValid = false;
                m_DeformationResetCause = static_cast<u8>(hadHistory ? kCauseBoneCountChanged : kCauseFirstUse);
                return hadHistory ? BoneHistoryAdvance::BoneCountChanged : BoneHistoryAdvance::FirstUse;
            }

            if (m_BoneHistoryResetPending)
            {
                // The frame the pending discontinuity has to cover. A reset that
                // was declared explicitly has already been counted with its real
                // cause, so say PendingReset and let the caller skip it; the very
                // first frame of a skeleton's life has not been counted by anyone
                // and is FirstUse.
                m_BoneHistoryResetPending = false;
                m_BoneHistoryValid = false;
                // A PendingReset keeps whatever cause the declaring caller
                // attributed through NoteDeformationResetCause; only the
                // never-had-history case is this function's own to name.
                if (!m_BoneHistoryEverValid)
                {
                    m_DeformationResetCause = static_cast<u8>(kCauseFirstUse);
                }
                return m_BoneHistoryEverValid ? BoneHistoryAdvance::PendingReset
                                              : BoneHistoryAdvance::FirstUse;
            }

            m_BoneHistoryValid = true;
            m_BoneHistoryEverValid = true;
            // The one continuous outcome, and therefore the one bump. The cause
            // is cleared here rather than left latched: a surface that has
            // recovered its history has no live discontinuity to name.
            ++m_DeformationRevision;
            m_DeformationResetCause = static_cast<u8>(kCauseNone);
            return BoneHistoryAdvance::Advanced;
        }

        /**
         * @brief Drop the deformation history explicitly, on a discontinuity.
         *
         * A discontinuity is any event after which the previous pose is not a
         * pose this skeleton was ever actually in: the skeleton being replaced
         * or re-bound, the entity being spawned or teleported, a scene or
         * play-mode transition, a camera cut. Holding prev equal to current
         * makes the next frame's bone motion exactly zero, which is the honest
         * answer — the alternative is a velocity derived from two unrelated
         * poses, which TAA and motion blur will faithfully smear.
         */
        void ResetBoneHistory()
        {
            CopyPoseToHistory();
            m_BoneHistoryValid = false;
            m_BoneHistoryResetPending = true;
            // Hold, do not bump (#1228). A reset that arrives AFTER this
            // frame's advance — a morph surface that moved, an LOD switch, a
            // teleport discovered during the tick — must be able to cancel the
            // continuity that advance just declared, and holding is what
            // cancels it: current == previous is the discontinuity reading.
            m_PrevDeformationRevision = m_DeformationRevision;
        }

        /**
         * @brief Whether m_PrevFinalBoneMatrices holds a genuine previous pose.
         *
         * False on the tick a skeleton is created, resized or reset, so a
         * consumer can tell "no motion because nothing moved" from "no motion
         * because there is no history yet".
         */
        [[nodiscard]] bool HasBoneHistory() const
        {
            return m_BoneHistoryValid;
        }

        /**
         * @brief Whether the revision pair describes one continuous deformation.
         *
         * The revision-shaped spelling of HasBoneHistory(), and the one a
         * canonical GPU Scene record carries (#1228). Modular arithmetic, so a
         * u32 wrap after ~2.2 years of uptime reads as continuous rather than
         * as one spurious discontinuity.
         *
         * The two must never disagree; SkeletalDeformationHistoryTest pins that
         * over every transition, because a record whose verdict drifted from
         * the palettes' would emit a velocity the shaders did not compute.
         */
        [[nodiscard]] bool HasContinuousDeformation() const
        {
            return m_DeformationRevision == static_cast<u32>(m_PrevDeformationRevision + 1u);
        }
    };
} // namespace OloEngine
