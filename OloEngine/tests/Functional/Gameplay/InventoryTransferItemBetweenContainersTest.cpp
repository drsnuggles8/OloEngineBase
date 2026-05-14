#include "OloEnginePCH.h"

// =============================================================================
// InventoryTransferItemBetweenContainersTest — Functional Test.
//
// Cross-subsystem seam under test:
//   ItemDatabase × Inventory::TransferItem × destination Inventory::AddItem
//   × source slot clearing. TransferItem reads slot N from `this`, calls
//   target.AddItem with a copy of the item. If AddItem succeeds, the
//   source slot is reset(). If AddItem fails (e.g. target full), the
//   transfer is rolled back: the source keeps the item. Trade UIs,
//   chest interactions, and pickup-to-bag flows all rely on both
//   halves of this contract.
//
// Scenario: two inventory components (a player and a chest), each with
// the registered "test_iron" stackable item registered. Player has one
// instance in slot 0. Transfer from player slot 0 to chest:
//   - chest gains the item (UsedSlots goes 0 → 1)
//   - player's slot 0 is now empty (UsedSlots goes 1 → 0)
// Then attempt a second transfer from the (now-empty) player slot 0 —
// returns false, neither side mutates.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/Inventory.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Inventory/ItemInstance.h"
#include "OloEngine/Gameplay/Inventory/Item.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class InventoryTransferItemBetweenContainersTest : public FunctionalTest
{
  protected:
    static constexpr const char* kItemID = "test_iron";

    void BuildScene() override
    {
        ItemDatabase::Clear();
        ItemDefinition def;
        def.ItemID = kItemID;
        def.DisplayName = "Iron Ingot";
        def.MaxStackSize = 5;
        def.Weight = 1.0f;
        def.Category = ItemCategory::Material;
        ItemDatabase::Register(def);

        m_Player = GetScene().CreateEntity("Player");
        auto& playerInv = m_Player.AddComponent<InventoryComponent>().PlayerInventory;

        m_Chest = GetScene().CreateEntity("Chest");
        // ItemContainerComponent owns its own Inventory; we use InventoryComponent
        // on the chest too so both sides share the same PlayerInventory member.
        m_Chest.AddComponent<InventoryComponent>();

        // Seed the player with one item in slot 0.
        ItemInstance ingot;
        ingot.InstanceID = UUID{ 7000 };
        ingot.ItemDefinitionID = kItemID;
        ingot.StackCount = 1;
        ASSERT_TRUE(playerInv.AddItem(ingot));
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        ItemDatabase::Clear();
    }

    Entity m_Player;
    Entity m_Chest;
};

TEST_F(InventoryTransferItemBetweenContainersTest, TransferFromPlayerToChestMovesItemAndClearsSource)
{
    auto& playerInv = m_Player.GetComponent<InventoryComponent>().PlayerInventory;
    auto& chestInv  = m_Chest .GetComponent<InventoryComponent>().PlayerInventory;
    ASSERT_EQ(playerInv.GetUsedSlots(), 1);
    ASSERT_EQ(chestInv.GetUsedSlots(),  0);

    EXPECT_TRUE(playerInv.TransferItem(/*fromSlot=*/0, chestInv))
        << "TransferItem returned false on a valid transfer — the destination "
           "rejected the AddItem or the source slot validation failed.";

    EXPECT_EQ(playerInv.GetUsedSlots(), 0)
        << "source slot 0 wasn't cleared after a successful transfer — "
           "TransferItem didn't slot.reset() after the destination accepted.";
    EXPECT_EQ(chestInv.GetUsedSlots(),  1);
    EXPECT_TRUE(chestInv.HasItem(kItemID))
        << "destination doesn't report holding the transferred item — "
           "either AddItem mis-tracked or HasItem queries the wrong axis.";

    // Second transfer from the (now-empty) source slot must return false
    // and mutate neither side.
    EXPECT_FALSE(playerInv.TransferItem(/*fromSlot=*/0, chestInv))
        << "TransferItem from an empty source slot returned true — the "
           "has_value() guard was skipped.";
    EXPECT_EQ(playerInv.GetUsedSlots(), 0);
    EXPECT_EQ(chestInv.GetUsedSlots(),  1) << "failed transfer mutated the destination.";

    // Same-inventory self-transfer is also rejected (different invariant,
    // but it lives in the same code path).
    EXPECT_FALSE(chestInv.TransferItem(0, chestInv))
        << "TransferItem onto self returned true — the &this == &target "
           "guard at the top of TransferItem regressed.";
}
