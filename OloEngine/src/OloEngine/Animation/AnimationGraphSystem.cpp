#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationGraphSystem.h"
#include "OloEngine/Animation/FootIKComponent.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/IK/FootIKPostPass.h"
#include "OloEngine/Animation/IK/IKPostPass.h"
#include "OloEngine/Animation/SpringBoneComponent.h"
#include "OloEngine/Animation/Procedural/SpringBonePostPass.h"
#include "OloEngine/Animation/NoiseAnimationComponent.h"
#include "OloEngine/Animation/Procedural/NoisePostPass.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSystem.h"
#include "OloEngine/Animation/BlendNode.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Core/Log.h"

#include <vector>

namespace OloEngine::Animation
{
    void AnimationGraphSystem::Update(
        AnimationGraphComponent& graphComp,
        Skeleton& skeleton,
        f32 deltaTime,
        const IKTargetComponent* ikTarget,
        const glm::mat4& entityWorldTransform,
        const SpringBoneComponent* springBone,
        SpringBoneState* springBoneState,
        const NoiseAnimationComponent* noise,
        NoiseAnimationState* noiseState,
        MorphTargetComponent* morphTarget,
        const FootIKComponent* footIK,
        FootIKStateComponent* footIKState)
    {
        OLO_PROFILE_FUNCTION();

        if (!graphComp.RuntimeGraph)
        {
            return;
        }

        sizet boneCount = skeleton.m_BoneNames.size();
        if (boneCount == 0)
        {
            return;
        }

        // Sync per-entity parameters into the runtime graph
        graphComp.RuntimeGraph->Parameters = graphComp.Parameters;

        // Decompose the skeleton's bind-pose local transforms into TRS so the
        // graph can fall back to bind pose (not identity) for bones a clip does
        // not animate — mirrors AnimationSystem's bind-pose reset (issue #543).
        // The bind pose is stable per skeleton, so cache the decomposed TRS on the
        // component and rebuild only when the bone count changes (or on first tick)
        // rather than re-decomposing every mat4 each frame. Stays empty when the
        // skeleton has no captured bind pose; the graph then degrades to an
        // identity fallback.
        if (graphComp.BindPoseLocalTRS.size() != boneCount)
        {
            graphComp.BindPoseLocalTRS.clear();
            if (skeleton.m_BindPoseLocalTransforms.size() == boneCount)
            {
                graphComp.BindPoseLocalTRS.resize(boneCount);
                for (sizet i = 0; i < boneCount; ++i)
                {
                    graphComp.BindPoseLocalTRS[i] = BlendUtils::DecomposeMatrix(skeleton.m_BindPoseLocalTransforms[i]);
                }
            }
        }

        // Evaluate the animation graph directly into the skeleton's local
        // transform buffer. Bind globals + pre-transforms feed root-motion
        // model-space conversion (issue #631).
        graphComp.RuntimeGraph->Update(deltaTime, boneCount, skeleton.m_LocalTransforms, skeleton.m_BoneNames, skeleton.m_ParentIndices, graphComp.BindPoseLocalTRS,
                                       skeleton.m_BindPoseMatrices, skeleton.m_BonePreTransforms);

        // Copy parameters back (triggers may have been consumed)
        graphComp.Parameters = graphComp.RuntimeGraph->Parameters;

        // Publish this tick's root-motion delta for Scene::UpdateRootMotion to
        // consume (overwritten every tick; nothing consumes it in editor preview,
        // where the pinned pose simply plays in place).
        {
            const RootMotionDelta& rootMotion = graphComp.RuntimeGraph->GetLastRootMotion();
            graphComp.RootMotionTranslation = rootMotion.Translation;
            graphComp.RootMotionRotation = rootMotion.Rotation;
            graphComp.HasRootMotion = rootMotion.HasMotion;
        }

        // Sample morph-target (blend-shape) weights from the graph's active
        // clip(s) into the entity's MorphTargetComponent. The graph just
        // advanced its state machines above, so the active state/time are
        // current. CPU evaluation + mesh deformation run later in the global
        // morph pass (Scene::OnUpdateRuntime), keeping this system free of any
        // mesh/renderer dependency.
        if (morphTarget)
        {
            std::vector<AnimationGraph::ActiveMorphClip> morphClips;
            graphComp.RuntimeGraph->CollectActiveMorphClips(morphClips);
            for (const auto& mc : morphClips)
            {
                MorphTargetSystem::SampleMorphKeyframes(mc.Clip, mc.Time, *morphTarget);
            }
        }

        // Ensure output vectors are sized before forward kinematics
        OLO_CORE_ASSERT(skeleton.m_ParentIndices.size() >= boneCount, "ParentIndices too small for boneCount");
        skeleton.m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));
        skeleton.m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));

        // Rotate current final bones into the previous-frame slot so the
        // G-Buffer skinned pass can compute per-bone motion vectors.
        skeleton.RotateBoneHistory();

        // Apply procedural noise (breathing / idle sway) before IK so the noise
        // produces the organic "intent" pose that IK then corrects.
        if (noise && noiseState && noise->Enabled)
        {
            ApplyNoisePostPass(skeleton, *noise, *noiseState, deltaTime);
        }

        // Apply IK pass between pose evaluation and forward kinematics
        if (ikTarget && (ikTarget->AimIKEnabled || ikTarget->LimbIKEnabled || ikTarget->ChainIKEnabled))
        {
            ApplyIKPostPass(skeleton, *ikTarget, entityWorldTransform);
        }

        // Ground-adaptation foot/hand IK after aim/limb/chain IK so it corrects
        // the final intent pose (issue #631 part 3)
        if (footIK && footIKState && footIK->Enabled)
        {
            ApplyFootIKPostPass(skeleton, *footIK, *footIKState, entityWorldTransform, deltaTime);
        }

        // Apply spring-bone secondary motion after IK so springs react to the
        // IK-corrected pose
        if (springBone && springBoneState && springBone->Enabled)
        {
            ApplySpringBonePostPass(skeleton, *springBone, *springBoneState, entityWorldTransform, deltaTime);
        }

        // Compute global transforms (forward kinematics)
        // Pre-transforms account for non-bone intermediate nodes in the hierarchy
        auto localTransformCount = skeleton.m_LocalTransforms.size();
        for (sizet i = 0; i < localTransformCount; ++i)
        {
            const glm::mat4& preTransform = (i < skeleton.m_BonePreTransforms.size())
                                                ? skeleton.m_BonePreTransforms[i]
                                                : glm::mat4(1.0f);
            i32 parent = skeleton.m_ParentIndices[i];
            if (parent >= 0)
            {
                skeleton.m_GlobalTransforms[i] = skeleton.m_GlobalTransforms[static_cast<sizet>(parent)] * preTransform * skeleton.m_LocalTransforms[i];
            }
            else
            {
                skeleton.m_GlobalTransforms[i] = preTransform * skeleton.m_LocalTransforms[i];
            }
        }

        // Compute final bone matrices for GPU skinning (GlobalTransform * InverseBindPose)
        auto globalTransformCount = skeleton.m_GlobalTransforms.size();
        for (sizet i = 0; i < globalTransformCount; ++i)
        {
            if (i < skeleton.m_InverseBindPoses.size())
            {
                skeleton.m_FinalBoneMatrices[i] = skeleton.m_GlobalTransforms[i] * skeleton.m_InverseBindPoses[i];
            }
            else
            {
                skeleton.m_FinalBoneMatrices[i] = skeleton.m_GlobalTransforms[i];
            }
        }
    }
} // namespace OloEngine::Animation
