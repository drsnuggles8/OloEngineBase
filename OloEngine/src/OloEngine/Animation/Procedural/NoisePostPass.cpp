#include "OloEnginePCH.h"
#include "OloEngine/Animation/Procedural/NoisePostPass.h"
#include "OloEngine/Animation/Procedural/NoiseSolver.h"
#include "OloEngine/Animation/NoiseAnimationComponent.h"
#include "OloEngine/Animation/Skeleton.h"

#include <cmath>
#include <vector>
#include <glm/gtx/matrix_decompose.hpp>

namespace OloEngine::Animation
{
    void ApplyNoisePostPass(
        Skeleton& skeleton,
        const NoiseAnimationComponent& noise,
        NoiseAnimationState& state,
        f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (!noise.Enabled || noise.ChainLength == 0)
        {
            return;
        }

        // Derive bone count from the skeleton itself to avoid stale caller values
        const auto boneCount = skeleton.m_LocalTransforms.size();
        if (boneCount == 0 || noise.EndBoneIndex >= static_cast<u32>(boneCount))
        {
            return;
        }
        OLO_CORE_ASSERT(skeleton.m_ParentIndices.size() == boneCount, "ParentIndices/LocalTransforms size mismatch");
        // Asserts compile out in release — guard the indexed accesses below
        // (parent walk, solver spans) against a malformed skeleton at runtime.
        if (skeleton.m_ParentIndices.size() != boneCount)
        {
            return;
        }

        // Reject non-finite deltaTime before it poisons the accumulated phase.
        if (!std::isfinite(deltaTime))
        {
            return;
        }

        // Advance the noise phase by accumulated playback time → frame-rate
        // independent. Guard against an unbounded accumulator growing into the
        // float-precision danger zone by wrapping; simplex noise is aperiodic so
        // a large wrap point is visually seamless.
        state.Time += deltaTime;
        constexpr f32 kPhaseWrap = 100000.0f;
        if (!std::isfinite(state.Time) || std::abs(state.Time) > kPhaseWrap)
        {
            state.Time = std::fmod(state.Time, kPhaseWrap);
            if (!std::isfinite(state.Time))
            {
                state.Time = 0.0f;
            }
        }

        // Skip if clamped weight is zero — no visible effect, skip the decompose.
        if (glm::clamp(noise.Weight, 0.0f, 1.0f) <= 0.0f)
        {
            return;
        }

        // Mark the bones the chain will modify so we only decompose / recompose
        // those (avoids the lossy glm::decompose round-trip on untouched bones).
        std::vector<bool> chainModified(boneCount, false);
        {
            u32 bone = noise.EndBoneIndex;
            for (u32 j = 0; j < noise.ChainLength && bone < static_cast<u32>(boneCount); ++j)
            {
                chainModified[bone] = true;
                if (const auto parent = skeleton.m_ParentIndices[bone]; parent < 0)
                {
                    break;
                }
                else
                {
                    bone = static_cast<u32>(parent);
                }
            }
        }

        // Full-size pose span (the solver walks parent indices), but only the
        // chain bones are decomposed/populated — they are the only entries the
        // solver reads or writes.
        std::vector<BoneTransform> localPose(boneCount);
        for (sizet i = 0; i < boneCount; ++i)
        {
            if (!chainModified[i])
            {
                continue;
            }
            glm::vec3 scale;
            glm::vec3 translation;
            glm::quat rotation;
            glm::vec3 skew;
            glm::vec4 perspective;
            if (!glm::decompose(skeleton.m_LocalTransforms[i], scale, rotation, translation, skew, perspective))
            {
                // Degenerate/non-affine local transform — drop this bone from the
                // write-back so we preserve its original matrix instead of
                // snapping it to identity (origin).
                chainModified[i] = false;
                continue;
            }
            localPose[i] = { translation, rotation, scale };
        }

        NoiseParams params;
        params.EndBoneIndex = noise.EndBoneIndex;
        params.ChainLength = noise.ChainLength;
        params.Frequency = noise.Frequency;
        params.RotationAmplitude = noise.RotationAmplitude;
        params.TranslationAmplitude = noise.TranslationAmplitude;
        params.Octaves = noise.Octaves;
        params.Lacunarity = noise.Lacunarity;
        params.Gain = noise.Gain;
        params.Seed = noise.Seed;
        params.Weight = noise.Weight;

        NoiseSolver::Apply(localPose, skeleton.m_ParentIndices, params, state.Time);

        // Recompose only the chain bones back into the skeleton.
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
