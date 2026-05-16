#pragma once

// =============================================================================
// AnimationFixtures.h — programmatic Skeleton + AnimationClip builders for
// Functional tests that need a ticking animation entity but don't care which
// specific clip plays.
//
// Keep this file additive: every helper here is shared across ≥3 Functional test
// files. If you need a one-off skeleton/clip, build it inline in your
// test rather than growing this header.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine::Functional::Fixtures
{

    /// Bind-posed single-bone skeleton — minimal viable input for the
    /// AnimationStateComponent + SkeletonComponent view in
    /// `Scene::OnUpdateRuntime`.
    inline Ref<Skeleton> MakeSingleBoneSkeleton()
    {
        auto skeleton = Ref<Skeleton>::Create(1);
        skeleton->m_BoneNames = { "Root" };
        skeleton->m_ParentIndices = { -1 };
        skeleton->m_LocalTransforms = { glm::mat4(1.0f) };
        skeleton->m_BonePreTransforms = { glm::mat4(1.0f) };
        skeleton->m_GlobalTransforms[0] = skeleton->m_LocalTransforms[0];
        skeleton->SetBindPose();
        return skeleton;
    }

    /// Two-bone skeleton (Root + Child) — needed when a clip animates a
    /// non-root bone, mirroring the real fox.gltf pattern.
    inline Ref<Skeleton> MakeTwoBoneSkeleton()
    {
        auto skeleton = Ref<Skeleton>::Create(2);
        skeleton->m_BoneNames = { "Root", "Child" };
        skeleton->m_ParentIndices = { -1, 0 };
        skeleton->m_LocalTransforms = { glm::mat4(1.0f), glm::mat4(1.0f) };
        skeleton->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f) };
        skeleton->m_GlobalTransforms[0] = skeleton->m_LocalTransforms[0];
        skeleton->m_GlobalTransforms[1] = skeleton->m_GlobalTransforms[0] * skeleton->m_LocalTransforms[1];
        skeleton->SetBindPose();
        return skeleton;
    }

    /// Clip with one position keyframe pair on the named bone — a minimal
    /// "this entity is animating" probe. Default `boneName` matches the
    /// bone produced by `MakeSingleBoneSkeleton`.
    inline Ref<AnimationClip> MakeTranslationClip(
        f32 duration,
        std::string boneName = "Root",
        glm::vec3 fromPos = glm::vec3(0.0f),
        glm::vec3 toPos = glm::vec3(0.0f, 0.5f, 0.0f))
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "Functional_TranslationClip";
        clip->Duration = duration;

        BoneAnimation boneAnim;
        boneAnim.BoneName = std::move(boneName);
        boneAnim.PositionKeys.push_back({ 0.0, fromPos });
        boneAnim.PositionKeys.push_back({ static_cast<f64>(duration), toPos });
        boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
        boneAnim.ScaleKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f) });
        clip->BoneAnimations.push_back(std::move(boneAnim));
        clip->InitializeBoneCache();
        return clip;
    }

} // namespace OloEngine::Functional::Fixtures
