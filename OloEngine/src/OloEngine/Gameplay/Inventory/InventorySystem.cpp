#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/InventorySystem.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    void InventorySystem::OnUpdate(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

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
                    scene->DestroyEntity(entity);
                    continue;
                }
            }
        }

        // Process auto-pickup: check proximity between pickups and entities with inventories
        auto inventoryView = scene->GetAllEntitiesWith<InventoryComponent, TransformComponent>();
        for (auto invEntity : inventoryView)
        {
            Entity invEnt = { invEntity, scene };
            auto& invTransform = invEnt.GetComponent<TransformComponent>();
            auto& invComp = invEnt.GetComponent<InventoryComponent>();

            auto pickupView2 = scene->GetAllEntitiesWith<ItemPickupComponent, TransformComponent>();
            for (auto pickupEntity : pickupView2)
            {
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
                f32 distance = glm::length(invTransform.Translation - pickupTransform.Translation);

                if (distance <= pickupComp.PickupRadius)
                {
                    if (invComp.PlayerInventory.AddItem(pickupComp.Item))
                    {
                        scene->DestroyEntity(pickupEnt);
                    }
                }
            }
        }
    }

} // namespace OloEngine
