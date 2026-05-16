#include "OloEnginePCH.h"

// =============================================================================
// InventoryStackConsolidationTest — Functional Test.
//
// Cross-subsystem seam under test:
//   ItemDatabase (item definition with MaxStackSize > 1) × Inventory::AddItem
//   × IsStackCompatible × InventoryComponent. The Inventory.AddItem path
//   first scans for an existing stack-compatible slot with headroom; if
//   found, it bumps StackCount in-place rather than minting a new slot.
//   A regression that skips the stack-merge branch silently fills the
//   inventory with one-stack entries — usable but visually nonsense to
//   the player.
//
// Scenario: register a stackable item (MaxStackSize=5) in ItemDatabase.
// AddItem with StackCount=1 twice. The inventory should contain ONE
// occupied slot with StackCount=2, not two slots with StackCount=1 each.
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

class InventoryStackConsolidationTest : public FunctionalTest
{
  protected:
    static constexpr const char* kItemID = "test_potion";

    void BuildScene() override
    {
        // Register a stackable definition in the global ItemDatabase. The
        // database is process-static, so we Clear at the start of each test
        // to avoid leakage from other Functional tests that may also touch it.
        ItemDatabase::Clear();

        ItemDefinition def;
        def.ItemID = kItemID;
        def.DisplayName = "Test Potion";
        def.MaxStackSize = 5;
        def.Weight = 0.1f;
        def.Category = ItemCategory::Consumable;
        ItemDatabase::Register(def);

        m_Player = GetScene().CreateEntity("Player");
        m_Player.AddComponent<InventoryComponent>();
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        ItemDatabase::Clear(); // don't leak the test definition into other tests
    }

    Entity m_Player;
};

TEST_F(InventoryStackConsolidationTest, TwoAddsOfStackableItemMergeIntoOneSlotWithStackCountTwo)
{
    auto& inv = m_Player.GetComponent<InventoryComponent>().PlayerInventory;
    EXPECT_EQ(inv.GetUsedSlots(), 0);

    ItemInstance one;
    one.InstanceID = UUID{};
    one.ItemDefinitionID = kItemID;
    one.StackCount = 1;

    ASSERT_TRUE(inv.AddItem(one));
    EXPECT_EQ(inv.GetUsedSlots(), 1);

    ItemInstance second = one;
    second.InstanceID = UUID{}; // fresh instance id; stack-compatibility is by definition + state, not by InstanceID
    ASSERT_TRUE(inv.AddItem(second));

    EXPECT_EQ(inv.GetUsedSlots(), 1)
        << "second AddItem allocated a new slot instead of merging — "
           "Inventory.AddItem skipped the IsStackCompatible scan, or "
           "IsStackCompatible returned false for definitionally-identical "
           "instances (per-instance state diverged unexpectedly).";

    EXPECT_EQ(inv.CountItem(kItemID), 2)
        << "CountItem doesn't agree with what AddItem said it stored — "
           "either CountItem aggregates wrong or the merge wrote to a "
           "different slot than the scan found.";
}
