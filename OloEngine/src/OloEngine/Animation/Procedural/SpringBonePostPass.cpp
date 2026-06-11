#include "OloEnginePCH.h"
#include "OloEngine/Animation/Procedural/SpringBonePostPass.h"
#include "OloEngine/Animation/Procedural/SpringBoneSolver.h"
#include "OloEngine/Animation/SpringBoneComponent.h"
#include "OloEngine/Animation/Skeleton.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <glm/gtx/matrix_decompose.hpp>

namespace OloEngine::Animation
{
    void ApplySpringBonePostPass(
        Skeleton& skeleton,
        const SpringBoneComponent& springBone,
        SpringBoneState& state,
        const glm::mat4& entityWorldTransform,
        f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (!springBone.Enabled || springBone.ChainLength < 2)
        {
            return;
        }

        // Derive bone count from the skeleton itself to avoid stale caller values
        auto boneCount = skeleton.m_LocalTransforms.size();
        if (boneCount == 0 || springBone.EndBoneIndex >= static_cast<u32>(boneCount))
        {
            return;
        }
        OLO_CORE_ASSERT(skeleton.m_ParentIndices.size() == boneCount, "ParentIndices/LocalTransforms size mismatch");
        OLO_CORE_ASSERT(skeleton.m_BonePreTransforms.empty() || skeleton.m_BonePreTransforms.size() == boneCount,
                        "BonePreTransforms size mismatch");

        // Validate all floating-point component inputs (the solver validates
        // again, but rejecting here skips the decompose work too)
        auto isFiniteVec3 = [](const glm::vec3& v)
        { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        if (!std::isfinite(springBone.Stiffness) || !std::isfinite(springBone.Damping) || !std::isfinite(springBone.Weight) || !isFiniteVec3(springBone.Gravity) || !std::isfinite(deltaTime))
        {
            return;
        }

        // Skip if clamped weight is zero — no visible effect
        if (glm::clamp(springBone.Weight, 0.0f, 1.0f) <= 0.0f)
        {
            return;
        }

        // Track which bones the chain will modify so we only write those
        // back — avoids the lossy glm::decompose round-trip on untouched bones.
        std::vector<bool> chainModified(boneCount, false);
        {
            auto bone = springBone.EndBoneIndex;
            for (u32 j = 0; j < springBone.ChainLength && bone < static_cast<u32>(boneCount); ++j)
            {
                chainModified[bone] = true;
                if (auto parent = skeleton.m_ParentIndices[bone]; parent < 0)
                {
                    break;
                }
                else
                {
                    bone = static_cast<u32>(parent);
                }
            }
        }

        // Decompose mat4 local transforms to BoneTransform for the solver
        std::vector<BoneTransform> localPose(boneCount);
        for (sizet i = 0; i < boneCount; ++i)
        {
            glm::vec3 scale;
            glm::vec3 translation;
            glm::quat rotation;
            glm::vec3 skew;
            glm::vec4 perspective;
            if (!glm::decompose(skeleton.m_LocalTransforms[i], scale, rotation, translation, skew, perspective))
            {
                localPose[i] = { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
                continue;
            }
            localPose[i] = { translation, rotation, scale };
        }

        SpringBoneParams params;
        params.EndBoneIndex = springBone.EndBoneIndex;
        params.ChainLength = springBone.ChainLength;
        params.Stiffness = springBone.Stiffness;
        params.Damping = springBone.Damping;
        params.Weight = springBone.Weight;

        // Rotate the world-space gravity into model space. Falls back to the
        // raw vector when the entity transform is degenerate (zero scale).
        auto entityRotScale = glm::mat3(entityWorldTransform);
        constexpr f32 kDeterminantEpsilon = 1e-12f;
        if (std::abs(glm::determinant(entityRotScale)) > kDeterminantEpsilon)
        {
            params.Gravity = glm::inverse(entityRotScale) * springBone.Gravity;
            if (!isFiniteVec3(params.Gravity))
            {
                params.Gravity = springBone.Gravity;
            }
        }
        else
        {
            params.Gravity = springBone.Gravity;
        }

        SpringBoneSolver::Solve(localPose, skeleton.m_ParentIndices, params, state, deltaTime, skeleton.m_BonePreTransforms);

        // Only write back bones in the simulated chain
        for (sizet i = 0; i < boneCount; ++i)
        {
            if (chainModified[i])
            {
                skeleton.m_LocalTransforms[i] =
                    glm::translate(glm::mat4(1.0f), localPose[i].Translation) * glm::mat4_cast(localPose[i].Rotation) * glm::scale(glm::mat4(1.0f), localPose[i].Scale);
            }
        }
    }
} // namespace OloEngine::Animation
