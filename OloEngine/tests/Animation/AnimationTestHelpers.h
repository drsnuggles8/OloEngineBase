#pragma once

// =============================================================================
// AnimationTestHelpers.h — small shared helpers for the animation-graph tests
// (unit and functional). Keep this additive and dependency-light: only helpers
// used by more than one animation test file belong here.
// =============================================================================

#include "OloEngine/Animation/AnimationClip.h" // BoneAnimation
#include "OloEngine/Animation/BlendNode.h"     // BoneTransform, PoseEvalContext

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace OloEngine::AnimTest
{
    // A clip channel that holds a constant translation across the whole clip, so
    // sampling at any time yields exactly that translation — makes the bone a
    // channel lands on unambiguous.
    inline BoneAnimation MakeConstantChannel(const std::string& boneName, const glm::vec3& translation)
    {
        BoneAnimation anim;
        anim.BoneName = boneName;
        anim.PositionKeys.push_back({ 0.0, translation });
        anim.PositionKeys.push_back({ 1.0, translation });
        anim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        anim.RotationKeys.push_back({ 1.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        anim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
        anim.ScaleKeys.push_back({ 1.0, glm::vec3(1.0f) });
        return anim;
    }

    // Single-"Bone0" sampling context shared by the single-bone fixtures in
    // BlendTreeTest / AnimationStateMachineTest: bone 0 is named "Bone0", so
    // by-name sampling reduces to the historical index-0 behaviour there.
    // inline so the PoseEvalContext's span refers to one program-wide names
    // instance across every including TU.
    inline const std::vector<std::string> s_Bone0Names = { "Bone0" };
    inline const PoseEvalContext s_Bone0Ctx{ s_Bone0Names, {} };
} // namespace OloEngine::AnimTest
