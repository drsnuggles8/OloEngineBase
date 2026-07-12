#include "OloEnginePCH.h"
#include "OloEngine/Animation/IK/FootIKPostPass.h"

#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Animation/FootIKComponent.h"
#include "OloEngine/Animation/IK/LimbIKSolver.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/norm.hpp>
#include <span>
#include <vector>

namespace OloEngine::Animation
{
    namespace
    {
        [[nodiscard("finite check must gate use")]] bool IsFiniteVec3(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // Decompose the skeleton's mat4 local transforms into TRS for the IK
        // solvers (mirrors ApplyIKPostPass's DecomposeLocalPose).
        std::vector<BoneTransform> DecomposeLocalPose(const Skeleton& skeleton, sizet boneCount)
        {
            std::vector<BoneTransform> localPose(boneCount);
            for (sizet i = 0; i < boneCount; ++i)
            {
                glm::vec3 scale;
                glm::vec3 translation;
                glm::quat rotation;
                glm::vec3 skew;
                if (glm::vec4 perspective; !glm::decompose(skeleton.m_LocalTransforms[i], scale, rotation, translation, skew, perspective))
                {
                    localPose[i] = { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
                    continue;
                }
                localPose[i] = { translation, rotation, scale };
            }
            return localPose;
        }

        void MarkChain(u32 startBone, u32 chainLength, sizet boneCount,
                       std::span<const int> parentIndices, std::vector<bool>& modified)
        {
            auto bone = startBone;
            for (u32 j = 0; j < chainLength; ++j)
            {
                if (bone >= static_cast<u32>(boneCount) || bone >= parentIndices.size())
                {
                    break;
                }
                modified[bone] = true;
                const int parent = parentIndices[bone];
                bone = (parent < 0) ? static_cast<u32>(boneCount) : static_cast<u32>(parent);
            }
        }

        void WriteBackModifiedBones(Skeleton& skeleton, const std::vector<BoneTransform>& localPose,
                                    const std::vector<bool>& modified, sizet boneCount)
        {
            for (sizet i = 0; i < boneCount; ++i)
            {
                if (modified[i])
                {
                    skeleton.m_LocalTransforms[i] =
                        glm::translate(glm::mat4(1.0f), localPose[i].Translation) *
                        glm::mat4_cast(localPose[i].Rotation) *
                        glm::scale(glm::mat4(1.0f), localPose[i].Scale);
                }
            }
        }

        struct FootPlan
        {
            bool Valid = false; // bone index in range
            glm::vec3 AnimatedWorld{ 0.0f };
            glm::vec3 TargetWorld{ 0.0f };
            bool HasTarget = false;  // ground (or lock) produced a target
            f32 RequiredDrop = 0.0f; // how far below the animated foot the target sits (>= 0)
        };

        // Decide where a foot wants to be this tick: ground-conformed target,
        // plant lock bookkeeping, and the pelvis-drop requirement.
        FootPlan PlanFoot(const FootIKComponent& footIK, FootIKFootState& state,
                          u32 footBone, sizet boneCount,
                          std::span<const BoneTransform> localPose,
                          std::span<const int> parentIndices,
                          std::span<const glm::mat4> preTransforms,
                          const glm::mat4& entityWorldTransform,
                          f32 deltaTime)
        {
            FootPlan plan;
            if (footBone >= static_cast<u32>(boneCount))
            {
                return plan;
            }
            plan.Valid = true;

            const BoneTransform footModel = BlendUtils::ComputeModelSpaceTransform(footBone, localPose, parentIndices, preTransforms);
            plan.AnimatedWorld = BlendUtils::ModelToWorldSpace(footModel.Translation, entityWorldTransform);

            // World-space speed of the (post-IK) foot from last tick's history —
            // the plant heuristic: a grounded foot moving slowly is in stance.
            f32 footSpeed = 0.0f;
            if (state.HasPrev && deltaTime > 0.0f)
            {
                footSpeed = glm::length(plan.AnimatedWorld - state.PrevWorldPos) / deltaTime;
            }

            // Ground conformance: keep the clip's lift above the LOCAL ground.
            // footModel.Translation.y is the foot's height above the entity
            // origin plane; the clip authors a planted foot at ~FootHeight.
            if (state.HasGround && IsFiniteVec3(state.GroundPoint) && IsFiniteVec3(state.GroundNormal))
            {
                const f32 lift = std::max(0.0f, footModel.Translation.y - footIK.FootHeight);
                glm::vec3 target = plan.AnimatedWorld;
                target.y = state.GroundPoint.y + footIK.FootHeight + lift;
                plan.TargetWorld = target;
                plan.HasTarget = true;
                plan.RequiredDrop = std::max(0.0f, plan.AnimatedWorld.y - target.y);

                // Plant lock: engage when grounded, slow, and near the ground;
                // release when the animation lifts or swings the foot.
                if (footIK.FootLock)
                {
                    const bool inStance = footSpeed <= footIK.PlantVelocityThreshold && lift <= footIK.PlantLiftThreshold;
                    if (!state.Locked && inStance)
                    {
                        state.Locked = true;
                        state.LockedWorldPos = target;
                        state.UnlockBlend = 1.0f;
                    }
                    else if (state.Locked && !inStance)
                    {
                        state.Locked = false; // start easing back to animation
                    }
                }
            }
            else if (state.Locked)
            {
                state.Locked = false; // lost the ground — release
            }

            // Locked feet stay put; released feet ease back over UnlockBlendTime.
            if (state.Locked)
            {
                plan.TargetWorld = state.LockedWorldPos;
                plan.HasTarget = true;
                plan.RequiredDrop = std::max(0.0f, plan.AnimatedWorld.y - plan.TargetWorld.y);
                state.UnlockBlend = 1.0f;
            }
            else if (state.UnlockBlend > 0.0f)
            {
                state.UnlockBlend = std::max(0.0f, state.UnlockBlend - deltaTime / std::max(footIK.UnlockBlendTime, 0.01f));
                if (plan.HasTarget)
                {
                    plan.TargetWorld = glm::mix(plan.TargetWorld, state.LockedWorldPos, state.UnlockBlend);
                }
                else if (state.UnlockBlend > 0.0f)
                {
                    plan.TargetWorld = glm::mix(plan.AnimatedWorld, state.LockedWorldPos, state.UnlockBlend);
                    plan.HasTarget = true;
                }
            }

            return plan;
        }

        // Rotate the foot bone so the character's up axis follows the ground
        // normal (clamped), applied in model space onto the foot's local rotation.
        void AlignFootToGround(std::vector<BoneTransform>& localPose,
                               std::span<const int> parentIndices,
                               std::span<const glm::mat4> preTransforms,
                               u32 footBone, u32 toeBone, bool toeRoll,
                               const glm::vec3& groundNormalWorld,
                               const glm::mat4& entityWorldTransform,
                               f32 maxSlopeAngleDeg, f32 weight,
                               sizet boneCount, std::vector<bool>& modified)
        {
            if (footBone >= static_cast<u32>(boneCount) || glm::length2(groundNormalWorld) < 1e-8f)
            {
                return;
            }

            // Ground normal into model space (rotation part only — normals don't translate).
            const glm::mat3 worldToModel = glm::inverse(glm::mat3(entityWorldTransform));
            glm::vec3 normalModel = worldToModel * groundNormalWorld;
            if (glm::length2(normalModel) < 1e-8f)
            {
                return;
            }
            normalModel = glm::normalize(normalModel);

            // Clamp the slope: steeper ground aligns as if at MaxSlopeAngle.
            const glm::vec3 upModel(0.0f, 1.0f, 0.0f);
            const f32 cosAngle = glm::clamp(glm::dot(normalModel, upModel), -1.0f, 1.0f);
            f32 angle = std::acos(cosAngle);
            const f32 maxAngle = glm::radians(glm::clamp(maxSlopeAngleDeg, 0.0f, 90.0f));
            if (angle < 1e-4f)
            {
                return; // flat ground — nothing to align
            }
            angle = std::min(angle, maxAngle);

            const glm::vec3 axis = glm::normalize(glm::cross(upModel, normalModel));
            if (!IsFiniteVec3(axis))
            {
                return;
            }
            const glm::quat slopeDelta = glm::angleAxis(angle * glm::clamp(weight, 0.0f, 1.0f), axis);

            // Apply in model space: rotate the foot's model orientation by the
            // slope delta, then convert back into the bone's local frame via the
            // parent's model rotation (local' = parentModel⁻¹ · delta · parentModel · local).
            const int parent = (footBone < parentIndices.size()) ? parentIndices[footBone] : -1;
            glm::quat parentModelRot{ 1.0f, 0.0f, 0.0f, 0.0f };
            if (parent >= 0)
            {
                parentModelRot = BlendUtils::ComputeModelSpaceTransform(static_cast<u32>(parent), localPose, parentIndices, preTransforms).Rotation;
            }
            const glm::quat localDelta = glm::inverse(parentModelRot) * slopeDelta * parentModelRot;
            localPose[footBone].Rotation = glm::normalize(localDelta * localPose[footBone].Rotation);
            modified[footBone] = true;

            // Toe counter-roll: keep the toes flat by removing the pitch the
            // slope alignment added, in the toe's own local frame.
            if (toeRoll && toeBone < static_cast<u32>(boneCount) && toeBone < parentIndices.size())
            {
                const glm::quat toeParentModelRot =
                    BlendUtils::ComputeModelSpaceTransform(static_cast<u32>(parentIndices[toeBone] >= 0 ? parentIndices[toeBone] : toeBone),
                                                           localPose, parentIndices, preTransforms)
                        .Rotation;
                const glm::quat toeLocalDelta = glm::inverse(toeParentModelRot) * glm::inverse(slopeDelta) * toeParentModelRot;
                localPose[toeBone].Rotation = glm::normalize(toeLocalDelta * localPose[toeBone].Rotation);
                modified[toeBone] = true;
            }
        }
    } // namespace

    void ApplyFootIKPostPass(
        Skeleton& skeleton,
        const FootIKComponent& footIK,
        FootIKStateComponent& state,
        const glm::mat4& entityWorldTransform,
        f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        const sizet boneCount = skeleton.m_LocalTransforms.size();
        if (boneCount == 0 || !footIK.Enabled)
        {
            return;
        }
        const f32 weight = glm::clamp(footIK.Weight, 0.0f, 1.0f);
        if (weight <= 0.0f)
        {
            return;
        }

        std::span<const int> parents = skeleton.m_ParentIndices;
        std::span<const glm::mat4> preTransforms = skeleton.m_BonePreTransforms;

        std::vector<BoneTransform> localPose = DecomposeLocalPose(skeleton, boneCount);
        std::vector<bool> modified(boneCount, false);

        // ── Plan both feet (ground conformance + plant locks) ────────────────
        FootPlan left = PlanFoot(footIK, state.Left, footIK.LeftFootBone, boneCount,
                                 localPose, parents, preTransforms, entityWorldTransform, deltaTime);
        FootPlan right = PlanFoot(footIK, state.Right, footIK.RightFootBone, boneCount,
                                  localPose, parents, preTransforms, entityWorldTransform, deltaTime);

        // ── Pelvis adaptation: lower the hips so the lowest target is reachable ─
        if (footIK.AdjustPelvis && footIK.PelvisBone < static_cast<u32>(boneCount))
        {
            f32 requiredDrop = 0.0f;
            if (left.HasTarget)
            {
                requiredDrop = std::max(requiredDrop, left.RequiredDrop);
            }
            if (right.HasTarget)
            {
                requiredDrop = std::max(requiredDrop, right.RequiredDrop);
            }
            const f32 targetOffset = -std::min(requiredDrop, footIK.MaxPelvisDrop) * weight;

            // Exponential smoothing toward the target offset.
            const f32 alpha = (deltaTime > 0.0f) ? (1.0f - std::exp(-footIK.PelvisLerpSpeed * deltaTime)) : 1.0f;
            state.PelvisOffset += (targetOffset - state.PelvisOffset) * alpha;

            if (std::abs(state.PelvisOffset) > 1e-5f)
            {
                // Offset along model-space up, expressed in the pelvis's parent frame.
                const int parent = (footIK.PelvisBone < parents.size()) ? parents[footIK.PelvisBone] : -1;
                glm::quat parentModelRot{ 1.0f, 0.0f, 0.0f, 0.0f };
                if (parent >= 0)
                {
                    parentModelRot = BlendUtils::ComputeModelSpaceTransform(static_cast<u32>(parent), localPose, parents, preTransforms).Rotation;
                }
                const glm::vec3 offsetLocal = glm::inverse(parentModelRot) * glm::vec3(0.0f, state.PelvisOffset, 0.0f);
                localPose[footIK.PelvisBone].Translation += offsetLocal;
                modified[footIK.PelvisBone] = true;
            }
        }

        // ── Solve each foot to its target with the two-bone limb solver ──────
        auto solveFoot = [&](const FootPlan& plan, u32 footBone)
        {
            if (!plan.Valid || !plan.HasTarget || !IsFiniteVec3(plan.TargetWorld))
            {
                return;
            }
            MarkChain(footBone, footIK.ChainLength, boneCount, parents, modified);
            LimbIKParams params;
            params.TargetBoneIndex = footBone;
            params.TargetPosition = BlendUtils::WorldToModelSpace(plan.TargetWorld, entityWorldTransform);
            params.ChainLength = std::clamp(footIK.ChainLength, 2u, std::max(2u, static_cast<u32>(boneCount)));
            params.Weight = weight;
            LimbIKSolver::Solve(localPose, parents, params, preTransforms);
        };
        solveFoot(left, footIK.LeftFootBone);
        solveFoot(right, footIK.RightFootBone);

        // ── Slope alignment (+ optional toe counter-roll) after the limb solve ─
        if (footIK.AlignFootToSlope)
        {
            if (left.Valid && state.Left.HasGround)
            {
                AlignFootToGround(localPose, parents, preTransforms, footIK.LeftFootBone, footIK.LeftToeBone,
                                  footIK.EnableToeRoll, state.Left.GroundNormal, entityWorldTransform,
                                  footIK.MaxSlopeAngle, weight, boneCount, modified);
            }
            if (right.Valid && state.Right.HasGround)
            {
                AlignFootToGround(localPose, parents, preTransforms, footIK.RightFootBone, footIK.RightToeBone,
                                  footIK.EnableToeRoll, state.Right.GroundNormal, entityWorldTransform,
                                  footIK.MaxSlopeAngle, weight, boneCount, modified);
            }
        }

        // ── Hand IK onto resolved prop/ledge targets ──────────────────────────
        const f32 handWeight = glm::clamp(footIK.HandWeight, 0.0f, 1.0f) * weight;
        auto solveHand = [&](bool active, u32 handBone, const glm::vec3& targetWorld)
        {
            if (!active || handWeight <= 0.0f || handBone >= static_cast<u32>(boneCount) || !IsFiniteVec3(targetWorld))
            {
                return;
            }
            MarkChain(handBone, footIK.HandChainLength, boneCount, parents, modified);
            LimbIKParams params;
            params.TargetBoneIndex = handBone;
            params.TargetPosition = BlendUtils::WorldToModelSpace(targetWorld, entityWorldTransform);
            params.ChainLength = std::clamp(footIK.HandChainLength, 2u, std::max(2u, static_cast<u32>(boneCount)));
            params.Weight = handWeight;
            LimbIKSolver::Solve(localPose, parents, params, preTransforms);
        };
        solveHand(state.LeftHandActive, footIK.LeftHandBone, state.LeftHandResolvedTarget);
        solveHand(state.RightHandActive, footIK.RightHandBone, state.RightHandResolvedTarget);

        WriteBackModifiedBones(skeleton, localPose, modified, boneCount);

        // ── History for next tick's plant detection (post-solve world pose) ───
        auto recordFoot = [&](FootIKFootState& footState, u32 footBone)
        {
            if (footBone >= static_cast<u32>(boneCount))
            {
                footState.HasPrev = false;
                return;
            }
            const BoneTransform footModel = BlendUtils::ComputeModelSpaceTransform(footBone, localPose, parents, preTransforms);
            footState.PrevWorldPos = BlendUtils::ModelToWorldSpace(footModel.Translation, entityWorldTransform);
            footState.HasPrev = true;
        };
        recordFoot(state.Left, footIK.LeftFootBone);
        recordFoot(state.Right, footIK.RightFootBone);
    }
} // namespace OloEngine::Animation
