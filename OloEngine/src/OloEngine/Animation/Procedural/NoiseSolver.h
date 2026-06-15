#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/BlendNode.h"
#include <glm/vec3.hpp>
#include <span>

namespace OloEngine::Animation
{
    // Parameters for the procedural noise animator. The chain is addressed like
    // the IK / spring-bone components: EndBoneIndex is the tip and the chain
    // walks up ChainLength bones via the skeleton's parent indices. Each bone in
    // the chain receives a smooth, fractal-noise-driven additive offset.
    struct NoiseParams
    {
        u32 EndBoneIndex = 0;                   // chain tip bone; chain walks up via parentIndices
        u32 ChainLength = 1;                     // number of bones in the chain including the tip (>= 1)
        f32 Frequency = 1.0f;                    // noise evolution speed — higher = faster wobble
        glm::vec3 RotationAmplitude{ 0.08f, 0.08f, 0.08f }; // peak local-axis rotation offset (radians)
        glm::vec3 TranslationAmplitude{ 0.0f };  // peak local translation offset (model units)
        u32 Octaves = 2;                         // fractal octaves (clamped to 1..8)
        f32 Lacunarity = 2.0f;                   // frequency multiplier per octave (clamped 1..8)
        f32 Gain = 0.5f;                         // amplitude multiplier per octave (clamped 0..1)
        u32 Seed = 0;                            // de-correlates entities/instances sharing a clip
        f32 Weight = 1.0f;                       // 0..1 master blend (0 = animated pose unchanged)
    };

    // Per-bone additive offset produced by the noise solver.
    struct NoiseBoneOffset
    {
        glm::vec3 EulerRadians{ 0.0f }; // additive local rotation, per bone-local axis
        glm::vec3 Translation{ 0.0f };  // additive local translation
    };

    // Runtime accumulator: the noise phase is driven by accumulated playback
    // time so the motion is frame-rate independent (the same wall-clock time
    // yields the same pose regardless of step size). Runtime-only — never
    // serialized; a scene copy (play mode) starts the phase fresh at zero.
    struct NoiseAnimationState
    {
        f32 Time = 0.0f;
    };

    class NoiseSolver
    {
      public:
        // Pure, deterministic per-bone noise sample. chainIndex (0 = tip) and
        // boneIndex de-correlate bones so the chain doesn't move rigidly. Each
        // axis is bounded by its amplitude:
        //   |EulerRadians[a]| <= |RotationAmplitude[a]| * clamp(Weight, 0, 1)
        //   |Translation[a]|  <= |TranslationAmplitude[a]| * clamp(Weight, 0, 1)
        // Returns a zero offset for a non-finite or zero-weight configuration.
        [[nodiscard]] static NoiseBoneOffset SampleBoneOffset(
            const NoiseParams& params, u32 chainIndex, u32 boneIndex, f32 time);

        // Adds noise offsets to the local-space pose of the bone chain in place.
        // The chain is walked from EndBoneIndex up ChainLength parents. No-op for
        // a zero-length / out-of-range / zero-weight / non-finite configuration.
        // Deterministic given (pose, params, time).
        static void Apply(
            std::span<BoneTransform> pose,
            std::span<const int> parentIndices,
            const NoiseParams& params,
            f32 time);
    };
} // namespace OloEngine::Animation
