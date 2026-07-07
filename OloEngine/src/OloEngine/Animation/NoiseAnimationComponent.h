#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/ComponentReflection.h"
#include "OloEngine/Animation/Procedural/NoiseSolver.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // Procedural noise animator: applies smooth fractal-noise-driven *additive*
    // local transform offsets to a bone chain, layered on top of keyframe
    // animation for organic idle motion — breathing, idle sway, gentle wind.
    // The chain is addressed like the IK / spring-bone components: EndBoneIndex
    // is the tip and the chain walks up ChainLength bones via the skeleton's
    // parent indices. Each bone gets a de-correlated noise sample so the chain
    // doesn't move as a rigid unit. The pass runs *before* IK so end-effector
    // constraints stay satisfied while the body sways (see NoisePostPass.h).
    struct NoiseAnimationComponent
    {
        OLO_PROPERTY()
        bool Enabled = true;
        OLO_PROPERTY()
        u32 EndBoneIndex = 0; // chain tip bone (e.g. head or chest)
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 1u)
        u32 ChainLength = 1; // bones in the chain including the tip (>= 1)
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1e4f)
        f32 Frequency = 1.0f; // noise evolution speed — higher = faster wobble
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = -6.2832f, Max = 6.2832f)
        glm::vec3 RotationAmplitude{ 0.08f, 0.08f, 0.08f }; // peak rotation offset per local axis (radians)
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = -1e4f, Max = 1e4f)
        glm::vec3 TranslationAmplitude{ 0.0f, 0.0f, 0.0f }; // peak translation offset per local axis (model units)
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 1u, Max = 8u)
        u32 Octaves = 2; // fractal octaves (clamped 1..8)
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 1.0f, Max = 8.0f)
        f32 Lacunarity = 2.0f; // frequency multiplier per octave (clamped 1..8)
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 Gain = 0.5f; // amplitude multiplier per octave (clamped 0..1)
        OLO_PROPERTY()
        u32 Seed = 0; // de-correlates entities/instances sharing a clip
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 Weight = 1.0f; // 0 = animated pose, 1 = full noise offset

        // Trivially-copyable POD component: a single whole-struct bitwise compare
        // gives the same bitwise-exact undo detection as the editor's tier-1 memcmp
        // path (see docs/agent-rules/cpp-coding-quality.md §7).
        auto operator==(const NoiseAnimationComponent& o) const -> bool
        {
            return Math::BitwiseEqual(*this, o);
        }
    };

    // Runtime-only phase accumulator for NoiseAnimationComponent. Created on
    // demand by the Scene's animation update; deliberately NOT serialized and
    // NOT in the AllComponents tuple — a scene copy (play mode) starts the noise
    // phase fresh at zero so the motion is deterministic from the start of play.
    struct NoiseAnimationStateComponent
    {
        Animation::NoiseAnimationState State;
    };
} // namespace OloEngine
