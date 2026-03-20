#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/InventorySystem.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <unordered_set>

namespace OloEngine
{
    void InventorySystem::OnUpdate(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        std::unordered_set<entt::entity> entitiesToDestroy;

        // Process pickup despawn timers
        auto pickupView = scene->GetAllEntitiesWith<ItemPickupComponent, TransformComponent>();
        for (auto e : pickupView)
        {
            Entity entity = { e, scene };
            auto& pickup = entity.GetComponent<ItemPickupComponent>();

            if (pickup.DespawnTimer > 0.0f)
            {
                pickup.DespawnTimer -= dt;
                if (pickup.DespawnTimer <= 0.0f)
                {
                    entitiesToDestroy.insert(e);
                    continue;
                }
            }
        }

        // Process auto-pickup: check proximity between pickups and entities with inventories
        auto inventoryView = scene->GetAllEntitiesWith<InventoryComponent, TransformComponent>();
        auto pickupView2 = scene->GetAllEntitiesWith<ItemPickupComponent, TransformComponent>();
        for (auto invEntity : inventoryView)
        {
            Entity invEnt = { invEntity, scene };
            auto& invTransform = invEnt.GetComponent<TransformComponent>();
            auto& invComp = invEnt.GetComponent<InventoryComponent>();

            for (auto pickupEntity : pickupView2)
            {
                // Skip pickups already claimed this frame
                if (entitiesToDestroy.contains(pickupEntity))
                {
                    continue;
                }

                Entity pickupEnt = { pickupEntity, scene };
                if (!pickupEnt)
                {
                    continue;
                }

                auto& pickupComp = pickupEnt.GetComponent<ItemPickupComponent>();
                if (!pickupComp.AutoPickup)
                {
                    continue;
                }

                auto& pickupTransform = pickupEnt.GetComponent<TransformComponent>();
                glm::vec3 diff = invTransform.Translation - pickupTransform.Translation;
                f32 distSq = glm::dot(diff, diff);
                f32 radiusSq = pickupComp.PickupRadius * pickupComp.PickupRadius;

                if (distSq <= radiusSq)
                {
                    if (invComp.PlayerInventory.AddItem(pickupComp.Item))
                    {
                        entitiesToDestroy.insert(pickupEntity);
                    }
                }
            }
        }

        // Deferred destruction
        for (auto e : entitiesToDestroy)
        {
            Entity entity = { e, scene };
            if (entity)
            {
                scene->DestroyEntity(entity);
            }
        }
    }

} // namespace OloEngine
