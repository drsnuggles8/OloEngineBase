#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/UUID.h"

namespace OloEngine
{
    class Scene;
    class Entity;

    // Runtime destructible / debris system (issue #459).
    //
    // Drives DestructibleComponent: on sufficient damage a breakable object
    // shatters into physical debris (mesh + box collider + dynamic rigidbody on
    // the Jolt DEBRIS layer), the debris settles under physics, and each piece is
    // cleaned up after its lifetime — with a hard global cap on live debris so a
    // burst of breaks can never exceed the budget. It does NOT fracture meshes at
    // runtime; debris is pre-authored/asset-swapped (see docs/adr/0013 and
    // docs/agent-rules/destructible-debris.md).
    //
    // A stateless static system in the InventorySystem / QuestSystem mould: all
    // per-object state lives on the components. OnUpdate runs as the unmarked,
    // game-thread "Destructible" scheduler node (it makes structural ECS changes,
    // so it must never be marked Parallelizable).
    class DestructibleSystem
    {
      public:
        // Global cap on simultaneously-live debris pieces across the whole scene.
        // A break that would exceed it evicts the oldest debris first, so the live
        // count is a hard invariant, not a target.
        static constexpr u32 kMaxLiveDebris = 256;

        // Fallback lifetime (seconds) when a DestructibleComponent leaves
        // m_DebrisLifetime at 0.
        static constexpr f32 kDefaultDebrisLifetime = 6.0f;

        // Subscribe to the scene's GameplayEventBus so a combat kill
        // (EntityKilledEvent) or a breakable-joint failure (JointBrokeEvent) marks
        // a destructible for shattering. Call once per runtime session — from
        // Scene::OnRuntimeStart (production) or directly from a headless test,
        // which never calls OnRuntimeStart. The handlers only flip a component
        // flag, so they are safe to run mid-iteration inside the publisher.
        static void WireEvents(Scene* scene);

        // Public damage entry point: reduce a destructible's structural integrity.
        // Reaching 0 marks it to shatter on the next OnUpdate. Game-thread only
        // (writes a component field). Returns true if the hit registered (the
        // entity is a not-yet-broken destructible and the amount cleared its
        // m_DamageThreshold).
        static bool ApplyDamage(Scene* scene, Entity entity, f32 amount);

        // Per-tick: shatter every pending/health-depleted destructible, then age
        // out and budget-evict existing debris. dtSeconds is the frame timestep.
        static void OnUpdate(Scene* scene, f32 dtSeconds);
    };
} // namespace OloEngine
