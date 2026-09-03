// OLO_TEST_LAYER: Functional

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Functional/FunctionalTest.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Damage/CombatEvents.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/Combat/CombatSystem.h"
#include "OloEngine/Gameplay/Combat/CombatComponents.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigComponents.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    class SingleHitSceneQueries final : public SceneQueries
    {
      public:
        SceneQueryHit Hit;
        RayCastInfo LastRay;
        SphereCastInfo LastSphere;
        bool WasCast = false;

        bool CastRay(const RayCastInfo& rayInfo, SceneQueryHit& outHit) override
        {
            LastRay = rayInfo;
            WasCast = true;
            outHit = Hit;
            return Hit.HasHit();
        }

        bool CastShape(const ShapeCastInfo&, SceneQueryHit&) override
        {
            return false;
        }
        bool CastBox(const BoxCastInfo&, SceneQueryHit&) override
        {
            return false;
        }
        bool CastSphere(const SphereCastInfo& sphereInfo, SceneQueryHit& outHit) override
        {
            LastSphere = sphereInfo;
            WasCast = true;
            outHit = Hit;
            return Hit.HasHit() && Hit.m_Distance <= sphereInfo.m_MaxDistance;
        }
        bool CastCapsule(const CapsuleCastInfo&, SceneQueryHit&) override
        {
            return false;
        }
        i32 OverlapShape(const ShapeOverlapInfo&, SceneQueryHit*, i32) override
        {
            return 0;
        }
        i32 OverlapBox(const BoxOverlapInfo&, SceneQueryHit*, i32) override
        {
            return 0;
        }
        i32 OverlapSphere(const SphereOverlapInfo&, SceneQueryHit*, i32) override
        {
            return 0;
        }
        i32 OverlapCapsule(const CapsuleOverlapInfo&, SceneQueryHit*, i32) override
        {
            return 0;
        }
        i32 CastRayMultiple(const RayCastInfo&, SceneQueryHit*, i32) override
        {
            return 0;
        }
        i32 CastShapeMultiple(const ShapeCastInfo&, SceneQueryHit*, i32) override
        {
            return 0;
        }
        i32 CastBoxMultiple(const BoxCastInfo&, SceneQueryHit*, i32) override
        {
            return 0;
        }
        i32 CastSphereMultiple(const SphereCastInfo&, SceneQueryHit*, i32) override
        {
            return 0;
        }
        i32 CastCapsuleMultiple(const CapsuleCastInfo&, SceneQueryHit*, i32) override
        {
            return 0;
        }
    };
} // namespace

class WeaponImpactUsesGameplayDamageTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Shooter = GetScene().CreateEntity("Shooter");
        auto& shooterAbility = m_Shooter.AddComponent<AbilityComponent>();
        shooterAbility.InitializeDefaultRPGAttributes(100.0f, 0.0f, 0.0f, 0.0f);

        m_Target = GetScene().CreateEntity("AI Target");
        auto& targetAbility = m_Target.AddComponent<AbilityComponent>();
        targetAbility.InitializeDefaultRPGAttributes(50.0f, 0.0f, 0.0f, 0.0f);
        targetAbility.OwnedTags.AddTag(GameplayTag("State.Alive"));
    }

    Entity m_Shooter;
    Entity m_Target;
};

TEST_F(WeaponImpactUsesGameplayDamageTest, HitEntityTakesFalloffDamageThroughGas)
{
    WeaponDefinition definition;
    definition.Damage = 20.0f;
    definition.FalloffStart = 10.0f;
    definition.FalloffEnd = 30.0f;
    definition.MinimumDamageMultiplier = 0.5f;

    SceneQueryHit hit;
    hit.m_HitEntity = m_Target.GetUUID();
    hit.m_Distance = 20.0f;
    hit.m_Position = { 0.0f, 0.0f, -20.0f };
    hit.m_Normal = { 0.0f, 0.0f, 1.0f };

    const f32 applied = CombatSystem::ApplyWeaponImpact(&GetScene(), m_Shooter, definition, hit);

    EXPECT_FLOAT_EQ(applied, 15.0f);
    EXPECT_FLOAT_EQ(m_Target.GetComponent<AbilityComponent>().Attributes.GetCurrentValue("Health"), 35.0f);
}

TEST_F(WeaponImpactUsesGameplayDamageTest, HitscanExcludesShooterAndDamagesClosestHit)
{
    WeaponDefinition definition;
    definition.Damage = 10.0f;
    definition.Range = 80.0f;
    definition.FalloffStart = 80.0f;
    definition.FalloffEnd = 80.0f;

    SingleHitSceneQueries queries;
    queries.Hit.m_HitEntity = m_Target.GetUUID();
    queries.Hit.m_Distance = 20.0f;
    queries.Hit.m_Position = { 0.0f, 0.0f, -20.0f };

    const bool hit = CombatSystem::FireHitscan(
        &GetScene(), queries, m_Shooter, definition, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -4.0f });

    ASSERT_TRUE(hit);
    ASSERT_TRUE(queries.WasCast);
    EXPECT_EQ(queries.LastRay.m_ExcludedEntities, ExcludedEntityMap{ m_Shooter.GetUUID() });
    EXPECT_FLOAT_EQ(queries.LastRay.m_MaxDistance, 80.0f);
    EXPECT_FLOAT_EQ(glm::length(queries.LastRay.m_Direction), 1.0f);
    EXPECT_FLOAT_EQ(m_Target.GetComponent<AbilityComponent>().Attributes.GetCurrentValue("Health"), 40.0f);
}

TEST_F(WeaponImpactUsesGameplayDamageTest, ProjectileSweepAppliesImpactExactlyOnce)
{
    WeaponDefinition definition;
    definition.Delivery = WeaponDelivery::Projectile;
    definition.Damage = 12.0f;
    definition.Range = 100.0f;
    definition.FalloffStart = 100.0f;
    definition.FalloffEnd = 100.0f;
    definition.ProjectileSpeed = 20.0f;
    definition.ProjectileRadius = 0.1f;

    SingleHitSceneQueries queries;
    queries.Hit.m_HitEntity = m_Target.GetUUID();
    queries.Hit.m_Distance = 1.0f;
    queries.Hit.m_Position = { 0.0f, 0.0f, -1.0f };

    ProjectileState projectile = ProjectileState::Launch(
        m_Shooter.GetUUID(), definition, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f });

    EXPECT_TRUE(CombatSystem::AdvanceProjectile(&GetScene(), queries, projectile, 0.1f));
    EXPECT_FALSE(projectile.Active);
    EXPECT_FLOAT_EQ(m_Target.GetComponent<AbilityComponent>().Attributes.GetCurrentValue("Health"), 38.0f);

    EXPECT_FALSE(CombatSystem::AdvanceProjectile(&GetScene(), queries, projectile, 0.1f));
    EXPECT_FLOAT_EQ(m_Target.GetComponent<AbilityComponent>().Attributes.GetCurrentValue("Health"), 38.0f);
}

TEST_F(WeaponImpactUsesGameplayDamageTest, ProjectileCannotHitBeyondItsRemainingLifetime)
{
    WeaponDefinition definition;
    definition.Delivery = WeaponDelivery::Projectile;
    definition.Damage = 12.0f;
    definition.Range = 100.0f;
    definition.FalloffStart = 100.0f;
    definition.FalloffEnd = 100.0f;
    definition.ProjectileSpeed = 20.0f;
    definition.ProjectileRadius = 0.1f;
    definition.ProjectileLifetime = 0.05f;

    SingleHitSceneQueries queries;
    queries.Hit.m_HitEntity = m_Target.GetUUID();
    queries.Hit.m_Distance = 1.5f;
    queries.Hit.m_Position = { 0.0f, 0.0f, -1.5f };

    ProjectileState projectile = ProjectileState::Launch(
        m_Shooter.GetUUID(), definition, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f });

    EXPECT_FALSE(CombatSystem::AdvanceProjectile(&GetScene(), queries, projectile, 0.1f));
    EXPECT_FALSE(projectile.Active);
    EXPECT_FLOAT_EQ(queries.LastSphere.m_MaxDistance, 1.0f);
    EXPECT_FLOAT_EQ(projectile.Position.x, 0.0f);
    EXPECT_FLOAT_EQ(projectile.Position.y, 0.0f);
    EXPECT_FLOAT_EQ(projectile.Position.z, -1.0f);
    EXPECT_FLOAT_EQ(m_Target.GetComponent<AbilityComponent>().Attributes.GetCurrentValue("Health"), 50.0f);
}

TEST_F(WeaponImpactUsesGameplayDamageTest, WeaponComponentConsumesExternalFireIntent)
{
    ItemDatabase::Clear();
    ItemDefinition item;
    item.ItemID = "test_rifle";
    item.Category = ItemCategory::Weapon;
    item.Weapon = WeaponDefinition{};
    item.Weapon->Damage = 10.0f;
    item.Weapon->Range = 100.0f;
    item.Weapon->FalloffStart = 100.0f;
    item.Weapon->FalloffEnd = 100.0f;
    ItemDatabase::Register(item);

    WeaponComponent weapon;
    weapon.m_WeaponItemID = item.ItemID;
    weapon.m_UseDeviceInput = false;
    weapon.m_FireInput = true;
    m_Shooter.AddComponent<WeaponComponent>(weapon);

    SingleHitSceneQueries queries;
    queries.Hit.m_HitEntity = m_Target.GetUUID();
    queries.Hit.m_Distance = 10.0f;
    queries.Hit.m_Position = { 0.0f, 0.0f, -10.0f };

    CombatSystem::OnUpdate(&GetScene(), &queries, 1.0f / 60.0f);

    const auto& updated = m_Shooter.GetComponent<WeaponComponent>();
    EXPECT_EQ(updated.m_State.GetMagazineAmmo(), 29u);
    EXPECT_FLOAT_EQ(m_Target.GetComponent<AbilityComponent>().Attributes.GetCurrentValue("Health"), 40.0f);
    ItemDatabase::Clear();
}

TEST_F(WeaponImpactUsesGameplayDamageTest, HeldFirePreservesCadenceAcrossVariableTimesteps)
{
    ItemDatabase::Clear();
    ItemDefinition item;
    item.ItemID = "cadence_rifle";
    item.Category = ItemCategory::Weapon;
    item.Weapon = WeaponDefinition{};
    item.Weapon->RoundsPerMinute = 600.0f;
    item.Weapon->MagazineSize = 30;
    ItemDatabase::Register(item);

    WeaponComponent weapon;
    weapon.m_WeaponItemID = item.ItemID;
    weapon.m_UseDeviceInput = false;
    weapon.m_FireInput = true;
    m_Shooter.AddComponent<WeaponComponent>(weapon);

    SingleHitSceneQueries queries;
    CombatSystem::OnUpdate(&GetScene(), &queries, 0.31f);
    const u32 coarseShots = m_Shooter.GetComponent<WeaponComponent>().m_State.GetShotsFired();

    auto& reset = m_Shooter.GetComponent<WeaponComponent>();
    reset.m_State = WeaponState::Loaded(*item.Weapon);
    reset.m_LoadedItemID = item.ItemID;
    reset.m_Initialized = true;
    CombatSystem::OnUpdate(&GetScene(), &queries, 0.1f);
    CombatSystem::OnUpdate(&GetScene(), &queries, 0.1f);
    CombatSystem::OnUpdate(&GetScene(), &queries, 0.11f);
    const u32 fineShots = reset.m_State.GetShotsFired();

    EXPECT_EQ(coarseShots, 4u);
    EXPECT_EQ(fineShots, coarseShots);
    ItemDatabase::Clear();
}

TEST_F(WeaponImpactUsesGameplayDamageTest, HeldFireCatchUpIsBoundedAndRetainsOverdueCadence)
{
    ItemDatabase::Clear();
    ItemDefinition item;
    item.ItemID = "bounded_cadence_rifle";
    item.Category = ItemCategory::Weapon;
    item.Weapon = WeaponDefinition{};
    item.Weapon->RoundsPerMinute = 600.0f;
    item.Weapon->MagazineSize = 100;
    ItemDatabase::Register(item);

    WeaponComponent weapon;
    weapon.m_WeaponItemID = item.ItemID;
    weapon.m_UseDeviceInput = false;
    weapon.m_FireInput = true;
    m_Shooter.AddComponent<WeaponComponent>(weapon);

    SingleHitSceneQueries queries;
    CombatSystem::OnUpdate(&GetScene(), &queries, 10.0f);
    const u32 firstUpdateShots = m_Shooter.GetComponent<WeaponComponent>().m_State.GetShotsFired();
    CombatSystem::OnUpdate(&GetScene(), &queries, 0.0f);
    const u32 secondUpdateShots = m_Shooter.GetComponent<WeaponComponent>().m_State.GetShotsFired();

    EXPECT_GT(firstUpdateShots, 1u);
    EXPECT_LT(firstUpdateShots, item.Weapon->MagazineSize);
    EXPECT_GT(secondUpdateShots, firstUpdateShots);
    ItemDatabase::Clear();
}

TEST_F(WeaponImpactUsesGameplayDamageTest, ImpactPublishesReactionAndAlertsTheTarget)
{
    m_Shooter.GetComponent<TransformComponent>().Translation = { 3.0f, 1.0f, 4.0f };
    auto& animation = m_Target.AddComponent<AnimationGraphComponent>();
    animation.Parameters.DefineTrigger("Hit");
    auto& perception = m_Target.AddComponent<PerceptionComponent>();

    WeaponImpactEvent observed{};
    bool received = false;
    GetScene().GetGameplayEvents().Subscribe<WeaponImpactEvent>([&](const WeaponImpactEvent& event)
                                                                {
                                                                   observed = event;
                                                                   received = true; });

    WeaponDefinition definition;
    definition.Damage = 8.0f;
    definition.HitReactionTrigger = "Hit";
    SceneQueryHit hit;
    hit.m_HitEntity = m_Target.GetUUID();
    hit.m_Distance = 2.0f;
    hit.m_Position = { 0.0f, 1.0f, -2.0f };
    hit.m_Normal = { 0.0f, 0.0f, 1.0f };

    EXPECT_FLOAT_EQ(CombatSystem::ApplyWeaponImpact(&GetScene(), m_Shooter, definition, hit), 8.0f);
    EXPECT_TRUE(animation.Parameters.IsTriggerSet("Hit"));
    EXPECT_TRUE(perception.HasLastKnownPosition);
    EXPECT_EQ(perception.LastKnownPosition, glm::vec3(3.0f, 1.0f, 4.0f));
    ASSERT_TRUE(received);
    EXPECT_EQ(observed.SourceID, m_Shooter.GetUUID());
    EXPECT_EQ(observed.TargetID, m_Target.GetUUID());
    EXPECT_FLOAT_EQ(observed.AppliedDamage, 8.0f);
    EXPECT_EQ(observed.Position, hit.m_Position);
    const auto decals = GetScene().GetAllEntitiesWith<ImpactDecalComponent, DecalComponent>();
    EXPECT_EQ(std::ranges::distance(decals), 1);

    CombatSystem::OnUpdate(&GetScene(), nullptr, 8.1f);
    EXPECT_TRUE(GetScene().GetAllEntitiesWith<ImpactDecalComponent>().empty());
}

TEST_F(WeaponImpactUsesGameplayDamageTest, LethalDamageRespawnsTheSamePlayerEntity)
{
    const UUID playerID = m_Target.GetUUID();
    auto& transform = m_Target.GetComponent<TransformComponent>();
    transform.Translation = { 20.0f, 0.0f, 20.0f };
    m_Target.AddComponent<PlayerRigComponent>();
    PlayerRespawnComponent respawn;
    respawn.m_SpawnPoint = { 2.0f, 3.0f, 4.0f };
    respawn.m_SpawnYawDeg = 90.0f;
    respawn.m_RespawnDelay = 0.5f;
    m_Target.AddComponent<PlayerRespawnComponent>(respawn);
    Rigidbody3DComponent rigidbody;
    rigidbody.m_Type = BodyType3D::Dynamic;
    m_Target.AddComponent<Rigidbody3DComponent>(rigidbody);
    m_Target.AddComponent<BoxCollider3DComponent>();
    EnablePhysics3D();
    Ref<JoltBody> physicsBody = GetScene().GetPhysicsScene()->GetBody(m_Target);
    ASSERT_TRUE(physicsBody);
    physicsBody->SetLinearVelocity({ 3.0f, 4.0f, 5.0f });
    physicsBody->SetAngularVelocity({ 1.0f, 2.0f, 3.0f });

    WeaponDefinition definition;
    definition.Damage = 100.0f;
    SceneQueryHit hit;
    hit.m_HitEntity = playerID;
    hit.m_Distance = 1.0f;
    ASSERT_FLOAT_EQ(CombatSystem::ApplyWeaponImpact(&GetScene(), m_Shooter, definition, hit), 100.0f);

    CombatSystem::OnUpdate(&GetScene(), nullptr, 0.25f);
    EXPECT_TRUE(m_Target.GetComponent<AbilityComponent>().OwnedTags.HasTagExact(GameplayTag("State.Dead")));
    EXPECT_EQ(m_Target.GetComponent<TransformComponent>().Translation, glm::vec3(20.0f, 0.0f, 20.0f));

    CombatSystem::OnUpdate(&GetScene(), nullptr, 0.25f);
    ASSERT_TRUE(GetScene().TryGetEntityWithUUID(playerID).has_value());
    const auto& ability = m_Target.GetComponent<AbilityComponent>();
    EXPECT_FLOAT_EQ(ability.Attributes.GetCurrentValue("Health"), 50.0f);
    EXPECT_TRUE(ability.OwnedTags.HasTagExact(GameplayTag("State.Alive")));
    EXPECT_FALSE(ability.OwnedTags.HasTagExact(GameplayTag("State.Dead")));
    EXPECT_EQ(m_Target.GetComponent<TransformComponent>().Translation, respawn.m_SpawnPoint);
    EXPECT_FLOAT_EQ(m_Target.GetComponent<PlayerRigComponent>().m_YawDeg, 90.0f);
    EXPECT_EQ(physicsBody->GetPosition(), respawn.m_SpawnPoint);
    EXPECT_EQ(physicsBody->GetLinearVelocity(), glm::vec3(0.0f));
    EXPECT_EQ(physicsBody->GetAngularVelocity(), glm::vec3(0.0f));
}

TEST_F(WeaponImpactUsesGameplayDamageTest, SceneTickFiresHitscanThroughTheLivePhysicsWorld)
{
    m_Target.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, -5.0f };
    Rigidbody3DComponent body;
    body.m_Type = BodyType3D::Static;
    m_Target.AddComponent<Rigidbody3DComponent>(body);
    BoxCollider3DComponent collider;
    collider.m_HalfExtents = { 0.5f, 0.5f, 0.5f };
    m_Target.AddComponent<BoxCollider3DComponent>(collider);

    ItemDatabase::Clear();
    ItemDefinition item;
    item.ItemID = "live_hitscan";
    item.Category = ItemCategory::Weapon;
    item.Weapon = WeaponDefinition{};
    item.Weapon->Damage = 10.0f;
    item.Weapon->FalloffStart = 100.0f;
    item.Weapon->FalloffEnd = 100.0f;
    ItemDatabase::Register(item);
    WeaponComponent weapon;
    weapon.m_WeaponItemID = item.ItemID;
    weapon.m_UseDeviceInput = false;
    weapon.m_FireInput = true;
    m_Shooter.AddComponent<WeaponComponent>(weapon);

    EnablePhysics3D();
    RunFrames(1);

    EXPECT_FLOAT_EQ(m_Target.GetComponent<AbilityComponent>().Attributes.GetCurrentValue("Health"), 40.0f);
    ItemDatabase::Clear();
}

TEST_F(WeaponImpactUsesGameplayDamageTest, SceneTickSweepsProjectileThroughTheLivePhysicsWorld)
{
    m_Target.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, -5.0f };
    Rigidbody3DComponent body;
    body.m_Type = BodyType3D::Static;
    m_Target.AddComponent<Rigidbody3DComponent>(body);
    BoxCollider3DComponent collider;
    collider.m_HalfExtents = { 0.5f, 0.5f, 0.5f };
    m_Target.AddComponent<BoxCollider3DComponent>(collider);

    ItemDatabase::Clear();
    ItemDefinition item;
    item.ItemID = "live_projectile";
    item.Category = ItemCategory::Weapon;
    item.Weapon = WeaponDefinition{};
    item.Weapon->Delivery = WeaponDelivery::Projectile;
    item.Weapon->Damage = 12.0f;
    item.Weapon->MagazineSize = 1;
    item.Weapon->ReserveAmmo = 0;
    item.Weapon->ProjectileSpeed = 20.0f;
    item.Weapon->ProjectileRadius = 0.1f;
    item.Weapon->FalloffStart = 100.0f;
    item.Weapon->FalloffEnd = 100.0f;
    ItemDatabase::Register(item);
    WeaponComponent weapon;
    weapon.m_WeaponItemID = item.ItemID;
    weapon.m_UseDeviceInput = false;
    weapon.m_FireInput = true;
    m_Shooter.AddComponent<WeaponComponent>(weapon);

    EnablePhysics3D();
    RunFrames(30);

    EXPECT_FLOAT_EQ(m_Target.GetComponent<AbilityComponent>().Attributes.GetCurrentValue("Health"), 38.0f);
    EXPECT_TRUE(GetScene().GetAllEntitiesWith<ProjectileComponent>().empty());
    ItemDatabase::Clear();
}
