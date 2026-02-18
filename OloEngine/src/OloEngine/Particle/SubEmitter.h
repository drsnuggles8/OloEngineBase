#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    // Forward declare — sub-emitters reference separate ParticleSystem instances
    class ParticleSystem;

    enum class SubEmitterEvent : u8
    {
        OnDeath = 0,
        OnBirth,
        OnCollision,
    };

    struct SubEmitterEntry
    {
        SubEmitterEvent Trigger = SubEmitterEvent::OnDeath;
        u32 EmitCount = 5;     // Particles to emit from child system per trigger
        bool InheritVelocity = false;
        f32 InheritVelocityScale = 0.5f;

        // Index into ParticleSystemComponent::ChildSystems for the child particle system.
        // -1 means no child system assigned (falls back to parent pool behavior).
        i32 ChildSystemIndex = -1;
    };

    struct ModuleSubEmitter
    {
        bool Enabled = false;
        std::vector<SubEmitterEntry> Entries;
    };

    // Structure to pass from parent to child when a sub-emitter triggers
    struct SubEmitterTriggerInfo
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Velocity{ 0.0f };
        SubEmitterEvent Event = SubEmitterEvent::OnDeath;
        i32 ChildSystemIndex = -1; // Which child system to emit from
        u32 EmitCount = 5;         // How many particles to emit
    };
}
