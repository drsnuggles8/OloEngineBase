#include "OloEnginePCH.h"

// =============================================================================
// InventoryComponentSceneYAMLRoundTripTest — Functional Test.
//
// Cross-subsystem seam under test:
//   InventoryComponent × ItemDatabase × SceneSerializer YAML round-trip.
//   The InventoryComponent writer emits the items in slot order with their
//   definition IDs, stack counts, durability, and per-instance affixes /
//   custom data. The reader must rebuild the inventory by adding each
//   serialized item back via AddItem (which in turn consults
//   ItemDatabase). If the writer skips a field or the reader doesn't
//   re-register an item type, a player's bag silently empties between
//   save and load.
//
// Scenario: a player with two stackable items in their inventory (5x
// `iron_ingot` and 2x `iron_ingot` in a separate slot — these stack-
// merge automatically; we just verify the COUNT survives), plus a non-
// trivial Currency value. Serialize → deserialize → confirm:
//   - Component still attached on the restored player
//   - Currency preserved
//   - HasItem("iron_ingot", 7) holds (the two stacks merged to 7 on add)
//   - CountItem == 7
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/Inventory.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Inventory/ItemInstance.h"
#include "OloEngine/Gameplay/Inventory/Item.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class InventoryComponentSceneYAMLRoundTripTest : public FunctionalTest
{
  protected:
    static constexpr const char* kItemID = "iron_ingot";
    static constexpr i32 kCurrency = 137;

    void BuildScene() override
    {
        ItemDatabase::Clear();
        ItemDefinition def;
        def.ItemID = kItemID;
        def.DisplayName = "Iron Ingot";
        def.MaxStackSize = 10;
        def.Weight = 0.5f;
        def.Category = ItemCategory::Material;
        ItemDatabase::Register(def);

        m_Player = GetScene().CreateEntity("Hero");
        auto& ic = m_Player.AddComponent<InventoryComponent>();
        ic.Currency = kCurrency;

        // Two AddItem calls. The second stacks onto the first (same
        // definition, no per-instance state) so we end with one slot
        // holding StackCount=7, total CountItem=7.
        ItemInstance batch;
        batch.InstanceID = UUID{ 90001 };
        batch.ItemDefinitionID = kItemID;
        batch.StackCount = 5;
        ASSERT_TRUE(ic.PlayerInventory.AddItem(batch));

        batch.InstanceID = UUID{ 90002 };
        batch.StackCount = 2;
        ASSERT_TRUE(ic.PlayerInventory.AddItem(batch));
        ASSERT_EQ(ic.PlayerInventory.CountItem(kItemID), 7);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        ItemDatabase::Clear();
    }

    Entity m_Player;
};

TEST_F(InventoryComponentSceneYAMLRoundTripTest, ItemsAndCurrencySurviveSceneYAMLRoundTrip)
{
    SceneSerializer serializer(GetSceneRef());
    const std::string yaml = serializer.SerializeToYAML();
    ASSERT_FALSE(yaml.empty());

    // Re-register the item type in the database — the YAML stores the
    // definition ID by string, so the deserializer needs the database to
    // resolve it on AddItem. (In production, ItemDatabase::LoadFromDirectory
    // runs at scene-open; we simulate that by keeping our test registration.)
    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    SceneSerializer restoreSerializer(restored);
    ASSERT_TRUE(restoreSerializer.DeserializeFromYAML(yaml));

    Entity restoredPlayer = restored->FindEntityByName("Hero");
    ASSERT_TRUE(restoredPlayer);
    ASSERT_TRUE(restoredPlayer.HasComponent<InventoryComponent>())
        << "InventoryComponent dropped on restore — Scene serializer reader doesn't claim the YAML node.";

    const auto& ic = restoredPlayer.GetComponent<InventoryComponent>();

    EXPECT_EQ(ic.Currency, kCurrency)
        << "Currency value didn't round-trip — writer/reader disagree on the field name.";

    EXPECT_TRUE(ic.PlayerInventory.HasItem(kItemID, /*count=*/7))
        << "stacked iron_ingot didn't restore at full count — items may have "
           "been dropped because ItemDatabase::Get returned null on AddItem.";
    EXPECT_EQ(ic.PlayerInventory.CountItem(kItemID), 7)
        << "CountItem disagrees with HasItem — partial restoration or stack-count "
           "miscoded during emit/read.";
}
