#include "OloEnginePCH.h"

// =============================================================================
// InventoryEventsEmittedTest — Functional Test.
//
// Cross-subsystem seam under test:
//   InventorySystem (entity-aware service layer) × Inventory / EquipmentSlots
//   (pure value types) × GameplayEventBus × Scene tick.
//
// Pins the "finish-wire the inventory event payloads" work: InventorySystem is
// the layer that knows the owning entity, so it is where add / remove / equip /
// unequip become the entity-stamped InventoryEvents.h payloads published on
// Scene::GetGameplayEvents().
//
// The auto-pickup case proves the *real per-frame path*: walking a pickup into
// range during Scene::OnUpdateRuntime publishes ItemAdded with no explicit
// service call.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/InventorySystem.h"
#include "OloEngine/Gameplay/Inventory/InventoryEvents.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Inventory/ItemInstance.h"

#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

class InventoryEventsEmittedTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        ItemDatabase::Clear();

        ItemDefinition potion;
        potion.ItemID = "potion";
        potion.DisplayName = "Potion";
        potion.Category = ItemCategory::Consumable;
        potion.Rarity = ItemRarity::Common;
        potion.MaxStackSize = 5;
        potion.Weight = 0.1f;
        ItemDatabase::Register(potion);

        ItemDefinition sword;
        sword.ItemID = "sword";
        sword.DisplayName = "Sword";
        sword.Category = ItemCategory::Weapon;
        sword.Rarity = ItemRarity::Rare;
        sword.MaxStackSize = 1;
        sword.Weight = 3.0f;
        ItemDatabase::Register(sword);

        m_Player = GetScene().CreateEntity("Player");
        m_Player.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
        m_Player.AddComponent<InventoryComponent>();
        m_PlayerUUID = m_Player.GetUUID();
    }

    void TearDown() override
    {
        ItemDatabase::Clear();
        FunctionalTest::TearDown();
    }

    // A fresh sword instance with a stable, distinguishable InstanceID.
    static ItemInstance MakeSword()
    {
        ItemInstance s;
        s.InstanceID = UUID();
        s.ItemDefinitionID = "sword";
        s.StackCount = 1;
        return s;
    }

    Entity m_Player;
    UUID m_PlayerUUID;
};

TEST_F(InventoryEventsEmittedTest, AddItemPublishesItemAdded)
{
    std::vector<ItemAddedEvent> added;
    GetScene().GetGameplayEvents().Subscribe<ItemAddedEvent>([&](const ItemAddedEvent& e)
                                                             { added.push_back(e); });

    ItemInstance sword = MakeSword();
    ASSERT_TRUE(InventorySystem::AddItem(&GetScene(), m_Player, sword));

    ASSERT_EQ(added.size(), 1u);
    EXPECT_EQ(added[0].ItemDefinitionID, "sword");
    EXPECT_EQ(static_cast<u64>(added[0].ItemInstanceID), static_cast<u64>(sword.InstanceID));
    EXPECT_EQ(static_cast<u64>(added[0].EntityID), static_cast<u64>(m_PlayerUUID));
    EXPECT_GE(added[0].SlotIndex, 0) << "non-stacking add should report the slot it landed in.";
}

TEST_F(InventoryEventsEmittedTest, RemoveItemByDefinitionPublishesItemRemoved)
{
    ASSERT_TRUE(InventorySystem::AddItem(&GetScene(), m_Player, MakeSword()));

    std::vector<ItemRemovedEvent> removed;
    GetScene().GetGameplayEvents().Subscribe<ItemRemovedEvent>([&](const ItemRemovedEvent& e)
                                                               { removed.push_back(e); });

    ASSERT_TRUE(InventorySystem::RemoveItemByDefinition(&GetScene(), m_Player, "sword", 1));
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0].ItemDefinitionID, "sword");
    EXPECT_EQ(static_cast<u64>(removed[0].EntityID), static_cast<u64>(m_PlayerUUID));

    // Removing something that isn't there publishes nothing.
    EXPECT_FALSE(InventorySystem::RemoveItemByDefinition(&GetScene(), m_Player, "sword", 1));
    EXPECT_EQ(removed.size(), 1u);
}

TEST_F(InventoryEventsEmittedTest, EquipAndUnequipPublishEvents)
{
    ItemInstance sword = MakeSword();
    ASSERT_TRUE(InventorySystem::AddItem(&GetScene(), m_Player, sword));

    std::vector<ItemEquippedEvent> equipped;
    std::vector<ItemUnequippedEvent> unequipped;
    auto& bus = GetScene().GetGameplayEvents();
    bus.Subscribe<ItemEquippedEvent>([&](const ItemEquippedEvent& e)
                                     { equipped.push_back(e); });
    bus.Subscribe<ItemUnequippedEvent>([&](const ItemUnequippedEvent& e)
                                       { unequipped.push_back(e); });

    // Equip the sword from the player's own inventory into MainHand.
    ASSERT_TRUE(InventorySystem::EquipItem(&GetScene(), m_Player, EquipmentSlots::Slot::MainHand, sword));
    ASSERT_EQ(equipped.size(), 1u);
    EXPECT_EQ(equipped[0].SlotName, "MainHand");
    EXPECT_EQ(static_cast<u64>(equipped[0].ItemInstanceID), static_cast<u64>(sword.InstanceID));
    EXPECT_EQ(static_cast<u64>(equipped[0].EntityID), static_cast<u64>(m_PlayerUUID));

    // Unequip back into the inventory.
    ASSERT_TRUE(InventorySystem::UnequipItem(&GetScene(), m_Player, EquipmentSlots::Slot::MainHand));
    ASSERT_EQ(unequipped.size(), 1u);
    EXPECT_EQ(unequipped[0].SlotName, "MainHand");
    EXPECT_EQ(static_cast<u64>(unequipped[0].ItemInstanceID), static_cast<u64>(sword.InstanceID));

    // Unequipping an empty slot publishes nothing.
    EXPECT_FALSE(InventorySystem::UnequipItem(&GetScene(), m_Player, EquipmentSlots::Slot::MainHand));
    EXPECT_EQ(unequipped.size(), 1u);
}

TEST_F(InventoryEventsEmittedTest, AutoPickupPublishesItemAddedViaTick)
{
    // A nearby auto-pickup within the player's radius.
    Entity pickup = GetScene().CreateEntity("PotionPickup");
    pickup.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
    ItemPickupComponent pc;
    pc.Item.ItemDefinitionID = "potion";
    pc.Item.StackCount = 1;
    pc.PickupRadius = 2.0f;
    pc.AutoPickup = true;
    pickup.AddComponent<ItemPickupComponent>(pc);
    const UUID pickupUUID = pickup.GetUUID();

    std::vector<ItemAddedEvent> added;
    GetScene().GetGameplayEvents().Subscribe<ItemAddedEvent>([&](const ItemAddedEvent& e)
                                                             { added.push_back(e); });

    // InventorySystem::OnUpdate runs the proximity scan each tick.
    RunFrames(2);

    ASSERT_EQ(added.size(), 1u) << "auto-pickup did not publish ItemAdded via the per-frame tick path.";
    EXPECT_EQ(added[0].ItemDefinitionID, "potion");
    EXPECT_EQ(static_cast<u64>(added[0].EntityID), static_cast<u64>(m_PlayerUUID));
    EXPECT_FALSE(GetScene().TryGetEntityWithUUID(pickupUUID)) << "collected pickup should be destroyed.";
}
