#include "OloEnginePCH.h"
#include "OloEngine/Animation/BlendUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace OloEngine::Animation::BlendUtils
{
    // --- Coordinate-space conversions ---

    glm::vec3 WorldToModelSpace(const glm::vec3& worldPos, const glm::mat4& entityWorldTransform)
    {
        return glm::vec3(glm::affineInverse(entityWorldTransform) * glm::vec4(worldPos, 1.0f));
    }

    glm::vec3 ModelToWorldSpace(const glm::vec3& modelPos, const glm::mat4& entityWorldTransform)
    {
        return glm::vec3(entityWorldTransform * glm::vec4(modelPos, 1.0f));
    }

    // --- Transform math ---

    glm::vec3 TransformPoint(const BoneTransform& t, const glm::vec3& p)
    {
        return t.Translation + t.Rotation * (t.Scale * p);
    }

    glm::vec3 TransformVector(const BoneTransform& t, const glm::vec3& v)
    {
        return t.Rotation * (t.Scale * v);
    }

    BoneTransform InverseTransform(const BoneTransform& t)
    {
        auto invRot = glm::conjugate(t.Rotation);
        auto invScale = glm::vec3(1.0f) / t.Scale;
        auto invTrans = invScale * (invRot * -t.Translation);
        return { invTrans, invRot, invScale };
    }

    BoneTransform MultiplyTransforms(const BoneTransform& a, const BoneTransform& b)
    {
        return {
            a.Translation + a.Rotation * (a.Scale * b.Translation),
            a.Rotation * b.Rotation,
            a.Scale * b.Scale
        };
    }

    BoneTransform DecomposeMatrix(const glm::mat4& m)
    {
        glm::vec3 scale;
        glm::vec3 translation;
        glm::quat rotation;
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(m, scale, rotation, translation, skew, perspective);
        return { translation, rotation, scale };
    }

    void ComputeModelSpacePose(
        std::span<const BoneTransform> localPose,
        std::span<const int> parentIndices,
        std::vector<BoneTransform>& outModelSpacePose,
        std::span<const glm::mat4> preTransforms)
    {
        auto count = std::min(localPose.size(), parentIndices.size());
        outModelSpacePose.resize(count);

        for (sizet i = 0; i < count; ++i)
        {
            // Build effective local: preTransform[i] * local[i]
            auto effectiveLocal = localPose[i];
            if (i < preTransforms.size())
            {
                static constexpr glm::mat4 kIdentity{1.0f};
                if (std::memcmp(&preTransforms[i], &kIdentity, sizeof(glm::mat4)) != 0)
                {
                    auto pre = DecomposeMatrix(preTransforms[i]);
                    effectiveLocal = MultiplyTransforms(pre, localPose[i]);
                }
            }

            auto parent = parentIndices[i];
            if (parent >= 0 && static_cast<sizet>(parent) < i)
            {
                outModelSpacePose[i] = MultiplyTransforms(outModelSpacePose[static_cast<sizet>(parent)], effectiveLocal);
            }
            else
            {
                outModelSpacePose[i] = effectiveLocal;
            }
        }
    }

    BoneTransform ComputeModelSpaceTransform(
        u32 boneIndex,
        std::span<const BoneTransform> localPose,
        std::span<const int> parentIndices)
    {
        if (boneIndex >= localPose.size() || boneIndex >= parentIndices.size())
        {
            return {};
        }

        // Walk up the parent chain and accumulate
        // Build the chain from root to boneIndex
        std::vector<u32> chain;
        for (auto idx = static_cast<int>(boneIndex); idx >= 0 && static_cast<sizet>(idx) < localPose.size();)
        {
            chain.push_back(static_cast<u32>(idx));
            idx = parentIndices[static_cast<sizet>(idx)];
        }

        BoneTransform result;
        // Process from root (back of chain) to leaf (front of chain)
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            result = MultiplyTransforms(result, localPose[*it]);
        }
        return result;
    }

    // --- Standard blending ---

    void LerpPose(
        std::span<const BoneTransform> a,
        std::span<const BoneTransform> b,
        f32 weight,
        std::span<BoneTransform> out)
    {
        auto count = std::min({a.size(), b.size(), out.size()});

        for (sizet i = 0; i < count; ++i)
        {
            out[i].Translation = glm::mix(a[i].Translation, b[i].Translation, weight);
            out[i].Rotation = glm::slerp(a[i].Rotation, b[i].Rotation, weight);
            out[i].Scale = glm::mix(a[i].Scale, b[i].Scale, weight);
        }
    }

    // --- Additive blending ---

    void AdditivePose(
        std::span<const BoneTransform> base,
        std::span<const BoneTransform> additive,
        std::span<const BoneTransform> restPose,
        f32 weight,
        u32 blendRootBone,
        std::span<const int> parentIndices,
        std::span<BoneTransform> out)
    {
        auto count = std::min({base.size(), additive.size(), restPose.size(), out.size()});

        // Build affected-bones mask when masking is requested
        std::vector<bool> affected;
        bool useMask = (blendRootBone > 0) && (blendRootBone < count);
        if (useMask)
        {
            affected.resize(count, false);
            affected[blendRootBone] = true;
            for (sizet i = blendRootBone + 1; i < count; ++i)
            {
                if (i < parentIndices.size())
                {
                    auto parent = parentIndices[i];
                    if (parent >= 0 && static_cast<sizet>(parent) < count && affected[static_cast<sizet>(parent)])
                    {
                        affected[i] = true;
                    }
                }
            }
        }

        for (sizet i = 0; i < count; ++i)
        {
            if (useMask && !affected[i])
            {
                out[i] = base[i];
                continue;
            }

            // Translation: base + weight * (additive - rest)
            out[i].Translation = base[i].Translation + weight * (additive[i].Translation - restPose[i].Translation);

            // Rotation: base * conjugate(rest) * slerp(rest, additive, weight)
            auto restRot = restPose[i].Rotation;
            auto weightedRot = glm::slerp(restRot, additive[i].Rotation, weight);
            out[i].Rotation = base[i].Rotation * glm::conjugate(restRot) * weightedRot;

            // Scale: base + weight * (additive - rest)
            out[i].Scale = base[i].Scale + weight * (additive[i].Scale - restPose[i].Scale);
        }
    }

    // --- Masked blending ---

    void MaskedLerpPose(
        std::span<const BoneTransform> a,
        std::span<const BoneTransform> b,
        f32 alpha,
        u32 blendRootBone,
        std::span<const int> parentIndices,
        std::span<BoneTransform> out)
    {
        auto count = std::min({a.size(), b.size(), out.size()});

        // If blendRootBone is 0, apply to all bones
        if (blendRootBone == 0)
        {
            LerpPose(a, b, alpha, out);
            return;
        }

        // Build affected-bones mask
        std::vector<bool> affected(count, false);
        if (blendRootBone < count)
        {
            affected[blendRootBone] = true;
            for (sizet i = blendRootBone + 1; i < count; ++i)
            {
                if (i < parentIndices.size())
                {
                    auto parent = parentIndices[i];
                    if (parent >= 0 && static_cast<sizet>(parent) < count && affected[static_cast<sizet>(parent)])
                    {
                        affected[i] = true;
                    }
                }
            }
        }

        for (sizet i = 0; i < count; ++i)
        {
            if (affected[i])
            {
                out[i].Translation = glm::mix(a[i].Translation, b[i].Translation, alpha);
                out[i].Rotation = glm::slerp(a[i].Rotation, b[i].Rotation, alpha);
                out[i].Scale = glm::mix(a[i].Scale, b[i].Scale, alpha);
            }
            else
            {
                out[i] = a[i];
            }
        }
    }

} // namespace OloEngine::Animation::BlendUtils
