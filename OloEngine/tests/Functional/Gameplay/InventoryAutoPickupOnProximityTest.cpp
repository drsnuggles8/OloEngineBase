#include "OloEnginePCH.h"

// =============================================================================
// InventoryAutoPickupOnProximityTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × InventorySystem::OnUpdate × ItemPickupComponent ×
//   InventoryComponent × Scene::DestroyEntity. The "walk near a dropped
//   item, it gets vacuumed into your inventory" gameplay flow. The
//   inventory system runs every tick, scans pickup entities, and on
//   proximity:
//     1. AddItem to the picker's Inventory.
//     2. DestroyEntity the pickup.
//   That's three subsystems coordinating per-frame: gameplay logic,
//   spatial proximity, and entity lifecycle. A regression in any of
//   them produces a "ghost item" (visible but uncollectable) or
//   double-pickup (item duplicates into inventory).
//
// Scenario: a pickup entity sits within an inventory entity's auto-
// pickup radius. Tick. Assert the inventory gained the item AND the
// pickup entity is gone from the scene.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class InventoryAutoPickupOnProximityTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Register a test item in the global item database. Inventory's
        // AddItem looks up item definitions by ID.
        ItemDatabase::Clear();
        ItemDefinition def;
        def.ItemID = "test_potion";
        def.DisplayName = "Test Potion";
        def.Category = ItemCategory::Consumable;
        def.Rarity = ItemRarity::Common;
        def.MaxStackSize = 5;
        def.Weight = 0.5f;
        ItemDatabase::Register(def);

        // Picker — entity with an InventoryComponent at origin.
        m_Picker = GetScene().CreateEntity("Player");
        m_Picker.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
        m_Picker.AddComponent<InventoryComponent>();

        // Pickup — entity with an ItemPickupComponent within radius.
        m_Pickup = GetScene().CreateEntity("HealthPotion");
        m_Pickup.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f }; // 1m away
        ItemPickupComponent pickup;
        pickup.Item.ItemDefinitionID = "test_potion";
        pickup.Item.StackCount = 1;
        pickup.PickupRadius = 2.0f; // > 1m distance, so within range
        pickup.AutoPickup = true;
        m_Pickup.AddComponent<ItemPickupComponent>(pickup);
        m_PickupUUID = m_Pickup.GetUUID();
    }

    void TearDown() override
    {
        ItemDatabase::Clear();
        FunctionalTest::TearDown();
    }

    Entity m_Picker;
    Entity m_Pickup;
    UUID m_PickupUUID;
};

TEST_F(InventoryAutoPickupOnProximityTest, NearbyAutoPickupIsConsumedIntoInventory)
{
    // Sanity: picker starts with empty inventory.
    {
        const auto& inv = m_Picker.GetComponent<InventoryComponent>().PlayerInventory;
        ASSERT_EQ(inv.CountItem("test_potion"), 0)
            << "inventory not empty at start";
    }

    // One tick should be enough — InventorySystem runs the proximity check
    // every frame and the pickup is well within radius.
    RunFrames(/*count=*/2);

    // Picker now has the potion.
    {
        const auto& inv = m_Picker.GetComponent<InventoryComponent>().PlayerInventory;
        EXPECT_EQ(inv.CountItem("test_potion"), 1)
            << "InventorySystem did not auto-pick up the nearby ItemPickup; "
               "inventory count for test_potion is "
            << inv.CountItem("test_potion");
    }

    // Pickup entity is destroyed. (Scene::DestroyEntity is called by the
    // inventory system after AddItem succeeds.)
    EXPECT_FALSE(GetScene().TryGetEntityWithUUID(m_PickupUUID))
        << "pickup entity still exists after being collected — "
           "InventorySystem's deferred-destruction step did not fire, "
           "or DestroyEntity is broken for entities without rigidbodies.";
}
