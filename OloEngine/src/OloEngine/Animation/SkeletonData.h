#pragma once

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

        // False until the deformation history has been advanced at least once
        // since the last discontinuity. While false, prev is held equal to
        // current, so consumers see zero bone motion rather than a jump from
        // whatever pose the previous skeleton happened to be in.
        bool m_BoneHistoryValid = false;

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
        void AdvanceBoneHistory()
        {
            const bool resized = CopyPoseToHistory();
            m_BoneHistoryValid = !resized;
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
    };
} // namespace OloEngine
