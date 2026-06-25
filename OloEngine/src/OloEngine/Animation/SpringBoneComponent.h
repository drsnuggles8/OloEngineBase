#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/ComponentReflection.h"
#include "OloEngine/Animation/Procedural/SpringBoneSolver.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // Spring-bone (jiggle-bone) procedural secondary animation: a damped
    // spring simulation over a bone chain (hair, tails, capes, antennae)
    // applied on top of keyframe animation and IK. The chain is addressed
    // like the IK components: EndBoneIndex is the chain tip and the chain
    // walks up ChainLength bones via the skeleton's parent indices.
    struct SpringBoneComponent
    {
        OLO_PROPERTY()
        bool Enabled = true;
        OLO_PROPERTY()
        u32 EndBoneIndex = 0; // chain tip bone (e.g. last tail bone)
        OLO_PROPERTY()
        u32 ChainLength = 3; // bones in the chain including the tip (min 2 to simulate)
        OLO_PROPERTY()
        f32 Stiffness = 80.0f; // pull toward the animated pose (1/s^2)
        OLO_PROPERTY()
        f32 Damping = 12.0f; // velocity damping rate (1/s)
        OLO_PROPERTY()
        glm::vec3 Gravity{ 0.0f, -9.81f, 0.0f }; // world-space acceleration
        OLO_PROPERTY()
        f32 Weight = 1.0f; // 0 = animated pose, 1 = full spring result

        // Trivially-copyable POD component: a single whole-struct bitwise compare
        // gives the same bitwise-exact undo detection as the editor's tier-1 memcmp
        // path (see docs/agent-rules/cpp-coding-quality.md §7).
        auto operator==(const SpringBoneComponent& o) const -> bool
        {
            return Math::BitwiseEqual(*this, o);
        }
    };

    // Runtime-only simulation state for SpringBoneComponent. Created on
    // demand by the Scene's animation update; deliberately NOT serialized
    // and NOT in the AllComponents tuple — a scene copy (play mode) starts
    // the simulation fresh from the animated pose.
    struct SpringBoneStateComponent
    {
        Animation::SpringBoneState State;
    };
} // namespace OloEngine
