#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/BlendNode.h"
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <span>
#include <vector>

namespace OloEngine::Animation::BlendUtils
{
    // --- Coordinate-space conversions ---
    // IK targets live in world space (from scene entities), but IK solvers
    // operate in model space (relative to the skeleton's entity origin).

    // Convert a world-space position to model space given the entity's world transform.
    [[nodiscard]] glm::vec3 WorldToModelSpace(const glm::vec3& worldPos, const glm::mat4& entityWorldTransform);

    // Convert a model-space position to world space given the entity's world transform.
    [[nodiscard]] glm::vec3 ModelToWorldSpace(const glm::vec3& modelPos, const glm::mat4& entityWorldTransform);

    // --- Transform math ---

    // Apply transform to a point: translation + rotation * (scale * point)
    [[nodiscard]] glm::vec3 TransformPoint(const BoneTransform& t, const glm::vec3& p);

    // Apply transform to a direction vector: rotation * (scale * vector)
    [[nodiscard]] glm::vec3 TransformVector(const BoneTransform& t, const glm::vec3& v);

    // Compute the inverse of a BoneTransform
    [[nodiscard]] BoneTransform InverseTransform(const BoneTransform& t);

    // Compose two transforms: a then b (result = a * b)
    [[nodiscard]] BoneTransform MultiplyTransforms(const BoneTransform& a, const BoneTransform& b);

    // Decompose a glm::mat4 into a BoneTransform (TRS)
    [[nodiscard]] BoneTransform DecomposeMatrix(const glm::mat4& m);

    // Convert local-space bone transforms to model-space via parent chain.
    // When preTransforms is non-empty, each bone's effective local transform
    // is preTransforms[i] * localPose[i], matching the forward-kinematics
    // formula: Global[i] = Global[parent] * PreTransform[i] * Local[i].
    void ComputeModelSpacePose(
        std::span<const BoneTransform> localPose,
        std::span<const int> parentIndices,
        std::vector<BoneTransform>& outModelSpacePose,
        std::span<const glm::mat4> preTransforms = {});

    // Compute a single bone's model-space transform by walking up the parent chain
    [[nodiscard]] BoneTransform ComputeModelSpaceTransform(
        u32 boneIndex,
        std::span<const BoneTransform> localPose,
        std::span<const int> parentIndices);

    // --- Standard blending ---

    // Linear interpolation between two poses: out[i] = lerp(a[i], b[i], weight)
    void LerpPose(
        std::span<const BoneTransform> a,
        std::span<const BoneTransform> b,
        f32 weight,
        std::span<BoneTransform> out);

    // --- Additive blending ---
    // result[i] = base[i] + weight * (additive[i] - restPose[i])
    // blendRootBone: index of bone from which additive starts; 0 = all bones
    void AdditivePose(
        std::span<const BoneTransform> base,
        std::span<const BoneTransform> additive,
        std::span<const BoneTransform> restPose,
        f32 weight,
        u32 blendRootBone,
        std::span<const int> parentIndices,
        std::span<BoneTransform> out);

    // --- Masked blending ---
    // Blends a -> b starting from blendRootBone and its descendants.
    // Bones outside the subtree keep pose A untouched.
    // blendRootBone: 0 = all bones affected (no masking)
    void MaskedLerpPose(
        std::span<const BoneTransform> a,
        std::span<const BoneTransform> b,
        f32 alpha,
        u32 blendRootBone,
        std::span<const int> parentIndices,
        std::span<BoneTransform> out);

    // Build a boolean mask marking blendRootBone and all its descendants.
    // Returns an empty vector when blendRootBone is out of range.
    [[nodiscard]] std::vector<bool> BuildAffectedBonesMask(
        u32 blendRootBone,
        sizet boneCount,
        std::span<const int> parentIndices);

} // namespace OloEngine::Animation::BlendUtils
