#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scene/Components.h" // JointType3D

namespace OloEngine
{
    // POD notification payloads published on the Scene's GameplayEventBus by the
    // Physics3D subsystem. Mirrors the Gameplay event headers
    // (Quest/QuestEvents.h, Inventory/InventoryEvents.h): plain structs, not
    // Event-derived classes, dispatched synchronously by GameplayEventBus.

    // Fired when a breakable PhysicsJoint3DComponent's constraint exceeds its
    // authored break threshold and is removed at runtime. By the time a
    // subscriber sees this, the Jolt constraint is already gone and the owning
    // component's m_RuntimeConstraintToken has been cleared to 0 — the two
    // bodies are free. Gameplay can react (spawn debris, play SFX, despawn the
    // component) from the handler.
    struct JointBrokeEvent
    {
        UUID EntityID;                         // entity carrying the PhysicsJoint3DComponent
        UUID ConnectedEntityID;                // the other body the joint connected to (0 = world)
        JointType3D Type = JointType3D::Fixed; // joint type that broke
        f32 Force = 0.0f;                      // linear constraint force at the breaking step (N)
        f32 Torque = 0.0f;                     // angular constraint torque at the breaking step (N·m)
        bool BrokeByForce = false;             // the force threshold (m_BreakForce) was exceeded
        bool BrokeByTorque = false;            // the torque threshold (m_BreakTorque) was exceeded
    };

} // namespace OloEngine
