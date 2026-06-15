#include "OloEnginePCH.h"
#include "OloEngine/Animation/Procedural/NoiseSolver.h"
#include "OloEngine/Particle/SimplexNoise.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine::Animation
{
    namespace
    {
        bool IsFiniteVec3(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // Fractal Brownian motion over 3D simplex noise, normalized to ~[-1,1]
        // then hard-clamped so callers get a guaranteed amplitude bound (simplex
        // can marginally exceed ±1 on its own). The (x, y, z) coordinate is a
        // smooth path through the noise field, so the result is a smooth,
        // continuous function of x — no jitter between frames.
        f32 Fbm(f32 x, f32 y, f32 z, u32 octaves, f32 lacunarity, f32 gain)
        {
            octaves = std::clamp(octaves, 1u, 8u);
            f32 sum = 0.0f;
            f32 amplitude = 1.0f;
            f32 frequency = 1.0f;
            f32 totalAmplitude = 0.0f;
            for (u32 o = 0; o < octaves; ++o)
            {
                sum += amplitude * SimplexNoise3D(x * frequency, y * frequency, z * frequency);
                totalAmplitude += amplitude;
                amplitude *= gain;
                frequency *= lacunarity;
            }
            f32 normalized = (totalAmplitude > 0.0f) ? (sum / totalAmplitude) : 0.0f;
            return std::clamp(normalized, -1.0f, 1.0f);
        }
    } // namespace

    NoiseBoneOffset NoiseSolver::SampleBoneOffset(const NoiseParams& params, u32 chainIndex, u32 boneIndex, f32 time)
    {
        NoiseBoneOffset offset;

        // Reject a non-finite configuration outright — a NaN/Inf would propagate
        // straight into the skeleton's local transforms.
        if (!std::isfinite(time) || !std::isfinite(params.Frequency) ||
            !std::isfinite(params.Lacunarity) || !std::isfinite(params.Gain) ||
            !std::isfinite(params.Weight) || !IsFiniteVec3(params.RotationAmplitude) ||
            !IsFiniteVec3(params.TranslationAmplitude))
        {
            return offset;
        }

        f32 weight = std::clamp(params.Weight, 0.0f, 1.0f);
        if (weight <= 0.0f)
        {
            return offset;
        }

        // Clamp the fractal controls to sane ranges so a bad authored value can't
        // blow up the octave loop or break the amplitude bound.
        f32 lacunarity = std::clamp(params.Lacunarity, 1.0f, 8.0f);
        f32 gain = std::clamp(params.Gain, 0.0f, 1.0f);

        // The noise phase advances with accumulated playback time → frame-rate
        // independent. Per-bone and per-axis sampling coordinates are spread far
        // apart in the noise field so the six channels de-correlate (avoids the
        // rigid "every bone moves identically" look).
        f32 phase = time * params.Frequency;
        f32 seedOffset = static_cast<f32>(params.Seed) * 19.19f;
        f32 boneDecorr = static_cast<f32>(chainIndex) * 5.31f + static_cast<f32>(boneIndex) * 0.733f + seedOffset;

        auto sample = [&](f32 lane) -> f32
        {
            return Fbm(phase, boneDecorr + lane, lane * 1.7f, params.Octaves, lacunarity, gain);
        };

        // Distinct lanes per axis (rotation 11/29/47, translation 71/89/107) so
        // all six channels are independent smooth signals.
        glm::vec3 rotNoise{ sample(11.0f), sample(29.0f), sample(47.0f) };
        glm::vec3 transNoise{ sample(71.0f), sample(89.0f), sample(107.0f) };

        offset.EulerRadians = rotNoise * params.RotationAmplitude * weight;
        offset.Translation = transNoise * params.TranslationAmplitude * weight;
        return offset;
    }

    void NoiseSolver::Apply(std::span<BoneTransform> pose, std::span<const int> parentIndices, const NoiseParams& params, f32 time)
    {
        OLO_PROFILE_FUNCTION();

        if (params.ChainLength == 0)
        {
            return;
        }

        const auto boneCount = pose.size();
        if (boneCount == 0 || parentIndices.size() != boneCount)
        {
            return;
        }
        if (params.EndBoneIndex >= static_cast<u32>(boneCount))
        {
            return;
        }
        if (std::clamp(params.Weight, 0.0f, 1.0f) <= 0.0f)
        {
            return;
        }
        // Reject non-finite time up front so a NaN/Inf phase can never reach the
        // per-bone application (SampleBoneOffset would return zero anyway, but
        // this keeps Apply a strict no-op).
        if (!std::isfinite(time))
        {
            return;
        }

        u32 bone = params.EndBoneIndex;
        for (u32 j = 0; j < params.ChainLength; ++j)
        {
            if (bone >= static_cast<u32>(boneCount))
            {
                break;
            }

            const NoiseBoneOffset bo = SampleBoneOffset(params, j, bone, time);

            // Additive in the bone's local frame: post-multiplying the noise
            // rotation makes RotationAmplitude.x a wobble about the bone's own
            // local X axis, which is the intuitive authoring behaviour.
            const glm::quat noiseRot = glm::quat(bo.EulerRadians);
            pose[bone].Rotation = glm::normalize(pose[bone].Rotation * noiseRot);
            pose[bone].Translation += bo.Translation;

            const int parent = parentIndices[bone];
            if (parent < 0)
            {
                break;
            }
            bone = static_cast<u32>(parent);
        }
    }
} // namespace OloEngine::Animation
