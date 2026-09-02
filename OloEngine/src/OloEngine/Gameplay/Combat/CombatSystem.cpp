#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Combat/CombatSystem.h"

#include "OloEngine/Gameplay/Abilities/Damage/DamageEvent.h"
#include "OloEngine/Gameplay/Abilities/Damage/CombatEvents.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/AI/Perception/PerceptionSystem.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Audio/AudioEvents/AudioPlayback.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/Combat/WeaponDamage.h"
#include "OloEngine/Gameplay/Combat/CombatComponents.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigComponents.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigSystem.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

namespace OloEngine
{
    f32 CombatSystem::ApplyWeaponImpact(Scene* scene, Entity source, const WeaponDefinition& definition, const SceneQueryHit& hit)
    {
        if (scene == nullptr || !source || !hit.HasHit())
        {
            return 0.0f;
        }

        auto target = scene->TryGetEntityWithUUID(hit.m_HitEntity);
        if (!target.has_value() || *target == source)
        {
            return 0.0f;
        }

        DamageEvent event;
        event.Source = source;
        event.Target = *target;
        event.RawDamage = ComputeWeaponDamage(definition, hit.m_Distance);
        event.DamageType = GameplayTag(definition.DamageType);
        event.HitLocation = hit.m_Position;
        event.HitNormal = hit.m_Normal;
        const f32 appliedDamage = GameplayAbilitySystem::ApplyDamage(scene, event);

        if (target->HasComponent<AnimationGraphComponent>() && !definition.HitReactionTrigger.empty())
        {
            auto& parameters = target->GetComponent<AnimationGraphComponent>().Parameters;
            if (parameters.HasParameter(definition.HitReactionTrigger))
            {
                parameters.SetTrigger(definition.HitReactionTrigger);
            }
        }
        if (source.HasComponent<TransformComponent>())
        {
            PerceptionSystem::ReportAlert(*target, source.GetComponent<TransformComponent>().Translation);
        }
        if (!definition.ImpactAudioTrigger.empty())
        {
            (void)Audio::AudioPlayback::PostTriggerByName(definition.ImpactAudioTrigger, static_cast<u64>(target->GetUUID()));
        }

        Entity impactDecal = scene->CreateEntity("Weapon Impact");
        auto& decalTransform = impactDecal.GetComponent<TransformComponent>();
        glm::vec3 normal = hit.m_Normal;
        const f32 normalLengthSquared = glm::dot(normal, normal);
        if (!std::isfinite(normalLengthSquared) || normalLengthSquared <= 1.0e-8f)
        {
            normal = glm::vec3(0.0f, 0.0f, 1.0f);
        }
        else
        {
            normal /= std::sqrt(normalLengthSquared);
        }
        decalTransform.Translation = hit.m_Position + normal * 0.002f;
        decalTransform.SetRotation(glm::rotation(glm::vec3(0.0f, 0.0f, -1.0f), -normal));
        DecalComponent decal;
        decal.m_Color = { 0.18f, 0.02f, 0.01f, 0.85f };
        decal.m_Size = { 0.18f, 0.18f, 0.04f };
        decal.m_FadeDistance = 15.0f;
        decal.m_Transparent = true;
        impactDecal.AddComponent<DecalComponent>(std::move(decal));
        impactDecal.AddComponent<ImpactDecalComponent>();

        scene->GetGameplayEvents().Publish(WeaponImpactEvent{
            source.GetUUID(), target->GetUUID(), hit.m_Position, hit.m_Normal, appliedDamage });
        return appliedDamage;
    }

    bool CombatSystem::FireHitscan(Scene* scene, SceneQueries& queries, Entity source, const WeaponDefinition& definition,
                                   const glm::vec3& origin, const glm::vec3& direction)
    {
        const f32 lengthSquared = glm::dot(direction, direction);
        if (scene == nullptr || !source || !std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8f ||
            !std::isfinite(definition.Range) || definition.Range <= 0.0f)
        {
            return false;
        }

        RayCastInfo ray;
        ray.m_Origin = origin;
        ray.m_Direction = direction / std::sqrt(lengthSquared);
        ray.m_MaxDistance = definition.Range;
        ray.m_ExcludedEntities.push_back(source.GetUUID());

        SceneQueryHit hit;
        if (!queries.CastRay(ray, hit) || !hit.HasHit())
        {
            return false;
        }

        (void)ApplyWeaponImpact(scene, source, definition, hit);
        return true;
    }

    bool CombatSystem::AdvanceProjectile(Scene* scene, SceneQueries& queries, ProjectileState& projectile, f32 deltaSeconds)
    {
        if (scene == nullptr || !projectile.Active || !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
        {
            return false;
        }

        const auto source = scene->TryGetEntityWithUUID(projectile.Owner);
        if (!source.has_value())
        {
            projectile.Active = false;
            return false;
        }

        const auto& definition = projectile.Definition;
        const f32 speed = std::isfinite(definition.ProjectileSpeed) ? std::max(0.0f, definition.ProjectileSpeed) : 0.0f;
        const f32 range = std::isfinite(definition.Range) ? std::max(0.0f, definition.Range) : 0.0f;
        const f32 remainingRange = std::max(0.0f, range - projectile.DistanceTraveled);
        const f32 stepDistance = std::min(speed * deltaSeconds, remainingRange);
        if (stepDistance <= 0.0f)
        {
            projectile.Active = false;
            return false;
        }

        SphereCastInfo sweep(projectile.Position, projectile.Direction,
                             std::isfinite(definition.ProjectileRadius) ? std::max(0.0f, definition.ProjectileRadius) : 0.0f,
                             stepDistance);
        sweep.m_ExcludedEntities.push_back(projectile.Owner);

        SceneQueryHit hit;
        if (queries.CastSphere(sweep, hit) && hit.HasHit())
        {
            hit.m_Distance += projectile.DistanceTraveled;
            projectile.Active = false;
            (void)ApplyWeaponImpact(scene, *source, definition, hit);
            return true;
        }

        projectile.Position += projectile.Direction * stepDistance;
        projectile.DistanceTraveled += stepDistance;
        projectile.LifetimeRemaining -= deltaSeconds;
        if (!std::isfinite(projectile.LifetimeRemaining) || projectile.LifetimeRemaining <= 0.0f ||
            projectile.DistanceTraveled >= range)
        {
            projectile.Active = false;
        }
        return false;
    }

    void CombatSystem::OnUpdate(Scene* scene, SceneQueries* queries, f32 deltaSeconds)
    {
        if (scene == nullptr || !std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
        {
            return;
        }

        const GameplayTag deadTag("State.Dead");
        const GameplayTag aliveTag("State.Alive");
        for (auto view = scene->GetAllEntitiesWith<PlayerRespawnComponent, AbilityComponent, TransformComponent>(); auto entityID : view)
        {
            Entity player{ entityID, scene };
            auto& respawn = player.GetComponent<PlayerRespawnComponent>();
            auto& ability = player.GetComponent<AbilityComponent>();
            if (!ability.OwnedTags.HasTagExact(deadTag))
            {
                respawn.m_DeathObserved = false;
                continue;
            }

            if (!respawn.m_DeathObserved)
            {
                respawn.m_TimeRemaining = std::isfinite(respawn.m_RespawnDelay)
                                              ? std::max(0.0f, respawn.m_RespawnDelay)
                                              : 0.0f;
                respawn.m_DeathObserved = true;
            }
            respawn.m_TimeRemaining = std::max(0.0f, respawn.m_TimeRemaining - deltaSeconds);

            if (player.HasComponent<PlayerRigComponent>())
            {
                auto& rig = player.GetComponent<PlayerRigComponent>();
                rig.m_MoveInput = glm::vec2(0.0f);
                rig.m_LookInput = glm::vec2(0.0f);
                rig.m_SprintInput = false;
                rig.m_JumpInput = false;
            }
            if (player.HasComponent<WeaponComponent>())
            {
                player.GetComponent<WeaponComponent>().m_FireInput = false;
            }
            if (respawn.m_TimeRemaining > 0.0f)
            {
                continue;
            }

            const f32 maxHealth = ability.Attributes.HasAttribute("MaxHealth")
                                      ? ability.Attributes.GetCurrentValue("MaxHealth")
                                      : 0.0f;
            ability.Attributes.SetBaseValue("Health", std::isfinite(maxHealth) ? std::max(0.0f, maxHealth) : 0.0f);
            ability.OwnedTags.RemoveTag(deadTag);
            ability.OwnedTags.AddTag(aliveTag);

            auto& transform = player.GetComponent<TransformComponent>();
            const glm::vec3 spawnPoint = Math::IsFinite(respawn.m_SpawnPoint) ? respawn.m_SpawnPoint : glm::vec3(0.0f);
            transform.Translation = spawnPoint;
            const f32 yaw = std::isfinite(respawn.m_SpawnYawDeg) ? respawn.m_SpawnYawDeg : 0.0f;
            const glm::quat rotation = glm::angleAxis(glm::radians(yaw), glm::vec3(0.0f, 1.0f, 0.0f));
            transform.SetRotation(rotation);
            if (player.HasComponent<PlayerRigComponent>())
            {
                player.GetComponent<PlayerRigComponent>().m_YawDeg = yaw;
            }
            if (JoltScene* physics = scene->GetPhysicsScene(); physics != nullptr)
            {
                if (Ref<JoltCharacterController> controller = physics->GetCharacterController(player))
                {
                    controller->SetTranslation(spawnPoint);
                    controller->SetRotation(rotation);
                    controller->SetLinearVelocity(glm::vec3(0.0f));
                    controller->SetAngularVelocity(glm::vec3(0.0f));
                }
                if (Ref<JoltBody> body = physics->GetBody(player))
                {
                    body->SetTransform(spawnPoint, rotation);
                    body->SetLinearVelocity(glm::vec3(0.0f));
                    body->SetAngularVelocity(glm::vec3(0.0f));
                }
            }
            if (player.HasComponent<WeaponComponent>())
            {
                auto& weapon = player.GetComponent<WeaponComponent>();
                if (const ItemDefinition* item = ItemDatabase::Get(weapon.m_WeaponItemID); item != nullptr && item->Weapon.has_value())
                {
                    weapon.m_State = WeaponState::Loaded(*item->Weapon);
                    weapon.m_LoadedItemID = weapon.m_WeaponItemID;
                    weapon.m_Initialized = true;
                }
            }
            respawn.m_DeathObserved = false;
        }

        struct LaunchRequest
        {
            ProjectileState State;
        };
        std::vector<LaunchRequest> launches;
        struct HitscanRequest
        {
            UUID Owner;
            WeaponDefinition Definition;
            glm::vec3 Origin;
            glm::vec3 Direction;
        };
        std::vector<HitscanRequest> hitscanRequests;

        for (auto view = scene->GetAllEntitiesWith<WeaponComponent, TransformComponent>(); auto entityID : view)
        {
            Entity owner{ entityID, scene };
            auto& weapon = owner.GetComponent<WeaponComponent>();
            if (owner.HasComponent<AbilityComponent>() &&
                owner.GetComponent<AbilityComponent>().OwnedTags.HasTagExact(deadTag))
            {
                weapon.m_FireInput = false;
                continue;
            }
            const ItemDefinition* item = ItemDatabase::Get(weapon.m_WeaponItemID);
            if (item == nullptr || !item->Weapon.has_value())
            {
                continue;
            }
            const WeaponDefinition& definition = *item->Weapon;

            if (!weapon.m_Initialized || weapon.m_LoadedItemID != weapon.m_WeaponItemID)
            {
                weapon.m_State = WeaponState::Loaded(definition);
                weapon.m_LoadedItemID = weapon.m_WeaponItemID;
                weapon.m_Initialized = true;
            }
            weapon.m_State.Advance(definition, deltaSeconds);

            if (weapon.m_UseDeviceInput)
            {
                weapon.m_FireInput = Input::IsMouseButtonPressed(Mouse::ButtonLeft);
                const bool reloadDown = Input::IsKeyPressed(Key::R);
                weapon.m_ReloadInput = weapon.m_ReloadInput || (reloadDown && !weapon.m_ReloadKeyWasDown);
                weapon.m_ReloadKeyWasDown = reloadDown;
            }

            if (weapon.m_ReloadInput)
            {
                (void)weapon.m_State.BeginReload(definition);
                weapon.m_ReloadInput = false;
            }
            if (!weapon.m_FireInput || queries == nullptr || weapon.m_State.TryFire(definition) != WeaponFireResult::Fired)
            {
                continue;
            }

            auto& transform = owner.GetComponent<TransformComponent>();
            const glm::quat ownerRotation = transform.GetRotation();
            const glm::vec3 origin = transform.Translation + ownerRotation * weapon.m_MuzzleOffset;
            glm::vec3 direction = ownerRotation * glm::vec3(0.0f, 0.0f, -1.0f);
            if (owner.HasComponent<PlayerRigComponent>())
            {
                auto& rig = owner.GetComponent<PlayerRigComponent>();
                direction = PlayerRigSystem::LookRotation(rig.m_YawDeg, rig.m_PitchDeg) * glm::vec3(0.0f, 0.0f, -1.0f);
                const f32 minPitch = std::min(rig.m_MinPitchDeg, rig.m_MaxPitchDeg);
                const f32 maxPitch = std::max(rig.m_MinPitchDeg, rig.m_MaxPitchDeg);
                rig.m_PitchDeg = std::clamp(rig.m_PitchDeg + definition.RecoilPitch, minPitch, maxPitch);
                const f32 yawKick = (weapon.m_State.GetShotsFired() & 1u) != 0u ? definition.RecoilYaw : -definition.RecoilYaw;
                rig.m_YawDeg = PlayerRigSystem::WrapDegrees(rig.m_YawDeg + yawKick);
            }

            if (definition.Delivery == WeaponDelivery::Hitscan)
            {
                hitscanRequests.push_back({ owner.GetUUID(), definition, origin, direction });
            }
            else
            {
                launches.push_back({ ProjectileState::Launch(owner.GetUUID(), definition, origin, direction) });
            }
            if (!definition.MuzzleAudioTrigger.empty())
            {
                (void)Audio::AudioPlayback::PostTriggerByName(definition.MuzzleAudioTrigger, static_cast<u64>(owner.GetUUID()));
            }
        }

        for (const auto& request : hitscanRequests)
        {
            if (auto owner = scene->TryGetEntityWithUUID(request.Owner); owner.has_value())
            {
                (void)FireHitscan(scene, *queries, *owner, request.Definition, request.Origin, request.Direction);
            }
        }

        for (auto& launch : launches)
        {
            Entity projectile = scene->CreateEntity("Projectile");
            projectile.GetComponent<TransformComponent>().Translation = launch.State.Position;
            ProjectileComponent component;
            component.m_State = std::move(launch.State);
            projectile.AddComponent<ProjectileComponent>(std::move(component));
        }

        std::vector<Entity> projectiles;
        for (auto view = scene->GetAllEntitiesWith<ProjectileComponent, TransformComponent>(); auto entityID : view)
        {
            projectiles.emplace_back(entityID, scene);
        }
        std::vector<Entity> expired;
        for (Entity projectile : projectiles)
        {
            auto& component = projectile.GetComponent<ProjectileComponent>();
            if (queries != nullptr)
            {
                (void)AdvanceProjectile(scene, *queries, component.m_State, deltaSeconds);
            }
            projectile.GetComponent<TransformComponent>().Translation = component.m_State.Position;
            if (!component.m_State.Active)
            {
                expired.push_back(projectile);
            }
        }
        for (Entity projectile : expired)
        {
            scene->DestroyEntity(projectile);
        }

        std::vector<Entity> expiredDecals;
        for (auto view = scene->GetAllEntitiesWith<ImpactDecalComponent>(); auto entityID : view)
        {
            Entity decalEntity{ entityID, scene };
            auto& lifetime = decalEntity.GetComponent<ImpactDecalComponent>();
            lifetime.m_RemainingSeconds -= deltaSeconds;
            if (!std::isfinite(lifetime.m_RemainingSeconds) || lifetime.m_RemainingSeconds <= 0.0f)
            {
                expiredDecals.push_back(decalEntity);
            }
        }
        for (Entity decalEntity : expiredDecals)
        {
            scene->DestroyEntity(decalEntity);
        }
    }
} // namespace OloEngine
