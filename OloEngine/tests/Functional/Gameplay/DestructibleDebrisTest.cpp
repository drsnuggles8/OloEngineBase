#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// DestructibleDebrisTest — Functional Test (issue #459).
//
// Cross-subsystem seam under test:
//   Gameplay (damage / combat kill / joint break) × Physics3D (runtime body
//   spawn + settle) × Scene/ECS (structural spawn+destroy through the gameplay
//   scheduler) × SaveGame. A breakable object must shatter into physical debris
//   on sufficient damage, the debris must settle under physics and be cleaned up
//   within a hard budget, and the break must fire EXACTLY once.
//
// These run through the real Scene::OnUpdateRuntime tick (RunFrames), so the
// DestructibleSystem executes as its scheduled "Destructible" node with physics
// live — the same path the shipping game takes. No OnRuntimeStart (headless), so
// tests that exercise the bus seams wire the subscriptions themselves.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Gameplay/Abilities/Damage/CombatEvents.h"
#include "OloEngine/Gameplay/Destruction/DestructibleSystem.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Physics3D/PhysicsEvents.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <limits>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // A breakable object: TransformComponent (from CreateEntity) + a configured
    // DestructibleComponent. No mesh/body of its own is needed — the debris is
    // what carries physics.
    Entity MakeDestructible(Scene& scene, const char* name, const glm::vec3& pos, u32 chunkCount,
                            bool destroyOnBreak = true, f32 debrisLifetime = 6.0f)
    {
        Entity e = scene.CreateEntity(name);
        e.GetComponent<TransformComponent>().Translation = pos;
        auto& dc = e.AddComponent<DestructibleComponent>();
        dc.m_Health = 100.0f;
        dc.m_MaxHealth = 100.0f;
        dc.m_ChunkCount = chunkCount;
        dc.m_ChunkScale = 0.2f;
        dc.m_ExplosionImpulse = 3.0f;
        dc.m_DebrisLifetime = debrisLifetime;
        dc.m_DestroyOnBreak = destroyOnBreak;
        return e;
    }
} // namespace

class DestructibleDebrisTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // A static floor so debris has something to settle on.
        Entity floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent fb;
        fb.m_Type = BodyType3D::Static;
        BoxCollider3DComponent fc;
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(fc);
        floor.AddComponent<Rigidbody3DComponent>(fb);

        EnablePhysics3D();
    }

    [[nodiscard]] u32 CountDebris()
    {
        u32 n = 0;
        for (auto e : GetScene().GetAllEntitiesWith<DebrisComponent>())
        {
            (void)e;
            ++n;
        }
        return n;
    }
};

// A single-hit under threshold does nothing; the killing hit shatters the object
// exactly once, spawning the configured chunk count — and it never re-shatters.
TEST_F(DestructibleDebrisTest, DamageShattersIntoDebrisExactlyOnce)
{
    // destroyOnBreak = false so the source survives and we can re-poke it.
    Entity crate = MakeDestructible(GetScene(), "Crate", { 0.0f, 1.0f, 0.0f },
                                    /*chunkCount=*/6, /*destroyOnBreak=*/false);

    // A non-lethal hit: health drops but no break.
    EXPECT_TRUE(DestructibleSystem::ApplyDamage(&GetScene(), crate, 40.0f));
    RunFrames(1);
    EXPECT_EQ(CountDebris(), 0u) << "a sub-lethal hit must not shatter the object";
    EXPECT_FALSE(crate.GetComponent<DestructibleComponent>().m_Broken);

    // The killing hit.
    EXPECT_TRUE(DestructibleSystem::ApplyDamage(&GetScene(), crate, 80.0f));
    RunFrames(1);
    EXPECT_EQ(CountDebris(), 6u) << "the killing hit must spawn exactly m_ChunkCount debris";
    EXPECT_TRUE(crate.GetComponent<DestructibleComponent>().m_Broken);

    // Further damage on an already-broken object is rejected and spawns nothing.
    EXPECT_FALSE(DestructibleSystem::ApplyDamage(&GetScene(), crate, 999.0f))
        << "an already-broken destructible must reject further damage";
    RunFrames(5);
    EXPECT_EQ(CountDebris(), 6u) << "the break must fire exactly once — no extra debris on later ticks";
}

// destroyOnBreak destroys the source entity when it shatters.
TEST_F(DestructibleDebrisTest, DestroyOnBreakRemovesTheSourceEntity)
{
    Entity crate = MakeDestructible(GetScene(), "Crate", { 0.0f, 1.0f, 0.0f },
                                    /*chunkCount=*/4, /*destroyOnBreak=*/true);
    const UUID crateID = crate.GetUUID();

    EXPECT_TRUE(DestructibleSystem::ApplyDamage(&GetScene(), crate, 150.0f));
    RunFrames(1);

    EXPECT_EQ(CountDebris(), 4u);
    EXPECT_FALSE(GetScene().TryGetEntityWithUUID(crateID).has_value())
        << "with m_DestroyOnBreak the source entity must be gone after the shatter";
}

// Debris settles under physics (falls to the floor under gravity) and is cleaned
// up once its lifetime elapses.
TEST_F(DestructibleDebrisTest, DebrisSettlesAndIsCleanedUpWithinLifetime)
{
    constexpr f32 kSpawnY = 3.0f;
    Entity crate = MakeDestructible(GetScene(), "Crate", { 0.0f, kSpawnY, 0.0f },
                                    /*chunkCount=*/5, /*destroyOnBreak=*/true,
                                    /*debrisLifetime=*/2.0f);

    EXPECT_TRUE(DestructibleSystem::ApplyDamage(&GetScene(), crate, 150.0f));
    RunFrames(1);
    ASSERT_EQ(CountDebris(), 5u) << "debris must exist right after the break";

    // Lowest debris across the field. Debris is launched outward/upward, so
    // gravity has to win the arc back before this drops below the spawn height.
    const auto lowestDebrisY = [this]() -> f32
    {
        f32 minY = std::numeric_limits<f32>::max();
        for (auto e : GetScene().GetAllEntitiesWith<DebrisComponent, TransformComponent>())
            minY = std::min(minY, Entity{ e, &GetScene() }.GetComponent<TransformComponent>().Translation.y);
        return minY;
    };

    // After ~1s the launch impulse has been overcome by gravity and debris has
    // fallen well below the spawn height (settling toward the floor at y≈0).
    RunFrames(60); // 1.0s — still within the 2.0s lifetime
    ASSERT_GT(CountDebris(), 0u) << "debris must still be alive mid-lifetime";
    EXPECT_LT(lowestDebrisY(), kSpawnY)
        << "debris must fall under gravity — physics is actually driving it (and it collides with the floor)";

    // Past the 2.0s lifetime the debris is cleaned up (settles-then-cleanup).
    const bool cleaned = TickUntil([this]
                                   { return CountDebris() == 0u; }, /*timeoutSeconds=*/3.0f);
    EXPECT_TRUE(cleaned) << "all debris must be cleaned up within its lifetime";
}

// A burst of simultaneous breaks can never push the live debris count past the
// global budget.
TEST_F(DestructibleDebrisTest, DebrisCountNeverExceedsBudget)
{
    // 50 objects × 8 chunks = 400 requested, well over kMaxLiveDebris (256).
    constexpr u32 kObjects = 50;
    constexpr u32 kChunksEach = 8;
    for (u32 i = 0; i < kObjects; ++i)
    {
        Entity e = MakeDestructible(GetScene(), "Crate", { static_cast<f32>(i) * 2.0f, 1.0f, 0.0f },
                                    kChunksEach, /*destroyOnBreak=*/true);
        e.GetComponent<DestructibleComponent>().m_PendingBreak = true;
    }

    RunFrames(1); // all breaks resolve this tick

    const u32 debris = CountDebris();
    EXPECT_LE(debris, DestructibleSystem::kMaxLiveDebris)
        << "live debris (" << debris << ") exceeded the budget of " << DestructibleSystem::kMaxLiveDebris;
    EXPECT_GT(debris, 0u) << "the burst should have produced debris up to the cap";
    // With no pre-existing debris to evict, the cap is filled exactly.
    EXPECT_EQ(debris, DestructibleSystem::kMaxLiveDebris);
}

// A combat kill (EntityKilledEvent) shatters a destructible victim — the bus
// integration seam. WireEvents is normally called by OnRuntimeStart, which the
// headless harness skips, so we wire it here.
TEST_F(DestructibleDebrisTest, CombatKillShattersDestructible)
{
    DestructibleSystem::WireEvents(&GetScene());
    Entity victim = MakeDestructible(GetScene(), "Victim", { 0.0f, 1.0f, 0.0f },
                                     /*chunkCount=*/5, /*destroyOnBreak=*/true);
    const UUID victimID = victim.GetUUID();

    GetScene().GetGameplayEvents().Publish(EntityKilledEvent{ victimID, /*KillerID=*/UUID(0), /*XP=*/0 });
    RunFrames(1);

    EXPECT_EQ(CountDebris(), 5u) << "a kill event on a destructible must shatter it";
    EXPECT_FALSE(GetScene().TryGetEntityWithUUID(victimID).has_value());
}

// A breakable joint giving way (JointBrokeEvent) shatters the entity it was on.
TEST_F(DestructibleDebrisTest, JointBreakShattersDestructible)
{
    DestructibleSystem::WireEvents(&GetScene());
    Entity anchor = MakeDestructible(GetScene(), "Anchor", { 0.0f, 1.0f, 0.0f },
                                     /*chunkCount=*/3, /*destroyOnBreak=*/false);
    anchor.GetComponent<DestructibleComponent>().m_BreakOnJointBreak = true;

    JointBrokeEvent ev;
    ev.EntityID = anchor.GetUUID();
    ev.Type = JointType3D::Fixed;
    ev.BrokeByForce = true;
    GetScene().GetGameplayEvents().Publish(ev);
    RunFrames(1);

    EXPECT_EQ(CountDebris(), 3u) << "a joint break on a destructible with m_BreakOnJointBreak must shatter it";
    EXPECT_TRUE(anchor.GetComponent<DestructibleComponent>().m_Broken);
}

// m_BreakOnJointBreak = false ignores joint-break events.
TEST_F(DestructibleDebrisTest, JointBreakIgnoredWhenOptedOut)
{
    DestructibleSystem::WireEvents(&GetScene());
    Entity anchor = MakeDestructible(GetScene(), "Anchor", { 0.0f, 1.0f, 0.0f },
                                     /*chunkCount=*/3, /*destroyOnBreak=*/false);
    anchor.GetComponent<DestructibleComponent>().m_BreakOnJointBreak = false;

    JointBrokeEvent ev;
    ev.EntityID = anchor.GetUUID();
    GetScene().GetGameplayEvents().Publish(ev);
    RunFrames(1);

    EXPECT_EQ(CountDebris(), 0u) << "a joint break must be ignored when m_BreakOnJointBreak is false";
    EXPECT_FALSE(anchor.GetComponent<DestructibleComponent>().m_Broken);
}
