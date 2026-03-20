#include <gtest/gtest.h>

#include "OloEngine/Gameplay/Inventory/Item.h"
#include "OloEngine/Gameplay/Inventory/ItemInstance.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Inventory/Inventory.h"
#include "OloEngine/Gameplay/Inventory/EquipmentSlots.h"
#include "OloEngine/Gameplay/Inventory/LootTable.h"

using namespace OloEngine;

class InventoryTestFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ItemDatabase::Clear();

        // Register test items
        ItemDefinition sword;
        sword.ItemID = "sword_iron";
        sword.DisplayName = "Iron Sword";
        sword.Description = "A basic iron sword";
        sword.Category = ItemCategory::Weapon;
        sword.Rarity = ItemRarity::Common;
        sword.MaxStackSize = 1;
        sword.Weight = 3.0f;
        sword.BuyPrice = 100;
        sword.SellPrice = 50;
        sword.AttributeModifiers = { { "AttackPower", 15.0f } };
        ItemDatabase::Register(sword);

        ItemDefinition potion;
        potion.ItemID = "health_potion";
        potion.DisplayName = "Health Potion";
        potion.Description = "Restores health";
        potion.Category = ItemCategory::Consumable;
        potion.Rarity = ItemRarity::Common;
        potion.MaxStackSize = 20;
        potion.Weight = 0.5f;
        potion.IsConsumable = true;
        ItemDatabase::Register(potion);

        ItemDefinition helmet;
        helmet.ItemID = "helmet_steel";
        helmet.DisplayName = "Steel Helmet";
        helmet.Description = "A sturdy steel helmet";
        helmet.Category = ItemCategory::Armor;
        helmet.Rarity = ItemRarity::Uncommon;
        helmet.MaxStackSize = 1;
        helmet.Weight = 2.0f;
        helmet.AttributeModifiers = { { "Defense", 10.0f }, { "MaxHealth", 50.0f } };
        ItemDatabase::Register(helmet);

        ItemDefinition questItem;
        questItem.ItemID = "quest_amulet";
        questItem.DisplayName = "Ancient Amulet";
        questItem.Description = "A mysterious ancient amulet";
        questItem.Category = ItemCategory::QuestItem;
        questItem.Rarity = ItemRarity::Legendary;
        questItem.MaxStackSize = 1;
        questItem.Weight = 0.1f;
        questItem.IsQuestItem = true;
        questItem.Tags = { "Quest", "Ancient" };
        ItemDatabase::Register(questItem);

        ItemDefinition gold;
        gold.ItemID = "gold_coin";
        gold.DisplayName = "Gold Coin";
        gold.Category = ItemCategory::Currency;
        gold.MaxStackSize = 999;
        gold.Weight = 0.01f;
        ItemDatabase::Register(gold);
    }

    void TearDown() override
    {
        ItemDatabase::Clear();
    }
};

// ===== ItemDatabase Tests =====

TEST_F(InventoryTestFixture, ItemDatabase_GetRegisteredItem)
{
    const auto* sword = ItemDatabase::Get("sword_iron");
    ASSERT_NE(sword, nullptr);
    EXPECT_EQ(sword->DisplayName, "Iron Sword");
    EXPECT_EQ(sword->Category, ItemCategory::Weapon);
    EXPECT_EQ(sword->Rarity, ItemRarity::Common);
}

TEST_F(InventoryTestFixture, ItemDatabase_GetUnregisteredItem)
{
    const auto* missing = ItemDatabase::Get("nonexistent_item");
    EXPECT_EQ(missing, nullptr);
}

TEST_F(InventoryTestFixture, ItemDatabase_GetByCategory)
{
    auto weapons = ItemDatabase::GetByCategory(ItemCategory::Weapon);
    EXPECT_EQ(weapons.size(), 1u);
    EXPECT_EQ(weapons[0]->ItemID, "sword_iron");
}

TEST_F(InventoryTestFixture, ItemDatabase_GetByTag)
{
    auto questItems = ItemDatabase::GetByTag("Quest");
    EXPECT_EQ(questItems.size(), 1u);
    EXPECT_EQ(questItems[0]->ItemID, "quest_amulet");
}

TEST_F(InventoryTestFixture, ItemDatabase_GetAll)
{
    auto all = ItemDatabase::GetAll();
    EXPECT_EQ(all.size(), 5u);
}

// ===== String Conversion Tests =====

TEST(ItemStringConversion, CategoryRoundTrip)
{
    for (u8 i = 0; i <= static_cast<u8>(ItemCategory::Misc); ++i)
    {
        auto cat = static_cast<ItemCategory>(i);
        auto str = ItemCategoryToString(cat);
        EXPECT_EQ(ItemCategoryFromString(str), cat);
    }
}

TEST(ItemStringConversion, RarityRoundTrip)
{
    for (u8 i = 0; i <= static_cast<u8>(ItemRarity::Legendary); ++i)
    {
        auto rarity = static_cast<ItemRarity>(i);
        auto str = ItemRarityToString(rarity);
        EXPECT_EQ(ItemRarityFromString(str), rarity);
    }
}

// ===== Inventory Add/Remove Tests =====

TEST_F(InventoryTestFixture, Inventory_AddItem)
{
    Inventory inv(10);
    ItemInstance sword;
    sword.InstanceID = UUID();
    sword.ItemDefinitionID = "sword_iron";
    sword.StackCount = 1;

    EXPECT_TRUE(inv.AddItem(sword));
    EXPECT_EQ(inv.GetUsedSlots(), 1);
}

TEST_F(InventoryTestFixture, Inventory_AddStackableItems)
{
    Inventory inv(10);

    ItemInstance potion1;
    potion1.InstanceID = UUID();
    potion1.ItemDefinitionID = "health_potion";
    potion1.StackCount = 5;

    ItemInstance potion2;
    potion2.InstanceID = UUID();
    potion2.ItemDefinitionID = "health_potion";
    potion2.StackCount = 3;

    EXPECT_TRUE(inv.AddItem(potion1));
    EXPECT_TRUE(inv.AddItem(potion2));

    // Should stack into 1 slot
    EXPECT_EQ(inv.GetUsedSlots(), 1);
    EXPECT_EQ(inv.CountItem("health_potion"), 8);
}

TEST_F(InventoryTestFixture, Inventory_CapacityLimit)
{
    Inventory inv(2);

    ItemInstance sword1;
    sword1.InstanceID = UUID();
    sword1.ItemDefinitionID = "sword_iron";
    sword1.StackCount = 1;

    ItemInstance sword2;
    sword2.InstanceID = UUID();
    sword2.ItemDefinitionID = "sword_iron";
    sword2.StackCount = 1;

    ItemInstance sword3;
    sword3.InstanceID = UUID();
    sword3.ItemDefinitionID = "sword_iron";
    sword3.StackCount = 1;

    EXPECT_TRUE(inv.AddItem(sword1));
    EXPECT_TRUE(inv.AddItem(sword2));
    EXPECT_FALSE(inv.AddItem(sword3)); // Inventory full
}

TEST_F(InventoryTestFixture, Inventory_WeightLimit)
{
    Inventory inv(10);
    inv.MaxWeight = 5.0f;

    ItemInstance sword;
    sword.InstanceID = UUID();
    sword.ItemDefinitionID = "sword_iron"; // Weight = 3.0
    sword.StackCount = 1;

    EXPECT_TRUE(inv.AddItem(sword));
    EXPECT_FLOAT_EQ(inv.GetTotalWeight(), 3.0f);

    ItemInstance sword2;
    sword2.InstanceID = UUID();
    sword2.ItemDefinitionID = "sword_iron";
    sword2.StackCount = 1;

    EXPECT_FALSE(inv.AddItem(sword2)); // Would exceed weight limit
}

TEST_F(InventoryTestFixture, Inventory_RemoveByDefinition)
{
    Inventory inv(10);

    ItemInstance potion;
    potion.InstanceID = UUID();
    potion.ItemDefinitionID = "health_potion";
    potion.StackCount = 10;

    inv.AddItem(potion);
    EXPECT_TRUE(inv.RemoveItemByDefinition("health_potion", 3));
    EXPECT_EQ(inv.CountItem("health_potion"), 7);
}

TEST_F(InventoryTestFixture, Inventory_RemoveAll)
{
    Inventory inv(10);

    ItemInstance potion;
    potion.InstanceID = UUID();
    potion.ItemDefinitionID = "health_potion";
    potion.StackCount = 5;

    inv.AddItem(potion);
    EXPECT_TRUE(inv.RemoveItemByDefinition("health_potion", 5));
    EXPECT_EQ(inv.CountItem("health_potion"), 0);
    EXPECT_EQ(inv.GetUsedSlots(), 0);
}

TEST_F(InventoryTestFixture, Inventory_RemoveTooMany)
{
    Inventory inv(10);

    ItemInstance potion;
    potion.InstanceID = UUID();
    potion.ItemDefinitionID = "health_potion";
    potion.StackCount = 3;

    inv.AddItem(potion);
    EXPECT_FALSE(inv.RemoveItemByDefinition("health_potion", 5));
    // Non-destructive: items should still be intact after failed removal
    EXPECT_EQ(inv.CountItem("health_potion"), 3);
}

TEST_F(InventoryTestFixture, Inventory_HasItem)
{
    Inventory inv(10);

    ItemInstance potion;
    potion.InstanceID = UUID();
    potion.ItemDefinitionID = "health_potion";
    potion.StackCount = 5;

    inv.AddItem(potion);
    EXPECT_TRUE(inv.HasItem("health_potion", 3));
    EXPECT_TRUE(inv.HasItem("health_potion", 5));
    EXPECT_FALSE(inv.HasItem("health_potion", 6));
    EXPECT_FALSE(inv.HasItem("sword_iron"));
}

TEST_F(InventoryTestFixture, Inventory_FindItem)
{
    Inventory inv(10);

    ItemInstance sword;
    sword.InstanceID = UUID();
    sword.ItemDefinitionID = "sword_iron";
    sword.StackCount = 1;

    inv.AddItemToSlot(3, sword);
    EXPECT_EQ(inv.FindItem("sword_iron"), 3);
    EXPECT_EQ(inv.FindItem("health_potion"), -1);
}

// ===== Inventory Move/Swap Tests =====

TEST_F(InventoryTestFixture, Inventory_SwapItems)
{
    Inventory inv(10);

    ItemInstance sword;
    sword.InstanceID = UUID();
    sword.ItemDefinitionID = "sword_iron";
    sword.StackCount = 1;

    ItemInstance potion;
    potion.InstanceID = UUID();
    potion.ItemDefinitionID = "health_potion";
    potion.StackCount = 5;

    inv.AddItemToSlot(0, sword);
    inv.AddItemToSlot(1, potion);

    EXPECT_TRUE(inv.SwapItems(0, 1));

    EXPECT_EQ(inv.GetItemAtSlot(0)->ItemDefinitionID, "health_potion");
    EXPECT_EQ(inv.GetItemAtSlot(1)->ItemDefinitionID, "sword_iron");
}

TEST_F(InventoryTestFixture, Inventory_MoveToEmptySlot)
{
    Inventory inv(10);

    ItemInstance sword;
    sword.InstanceID = UUID();
    sword.ItemDefinitionID = "sword_iron";
    sword.StackCount = 1;

    inv.AddItemToSlot(0, sword);
    EXPECT_TRUE(inv.MoveItem(0, 5));

    EXPECT_EQ(inv.GetItemAtSlot(0), nullptr);
    EXPECT_NE(inv.GetItemAtSlot(5), nullptr);
    EXPECT_EQ(inv.GetItemAtSlot(5)->ItemDefinitionID, "sword_iron");
}

TEST_F(InventoryTestFixture, Inventory_TransferBetweenInventories)
{
    Inventory inv1(10);
    Inventory inv2(10);

    ItemInstance sword;
    sword.InstanceID = UUID();
    sword.ItemDefinitionID = "sword_iron";
    sword.StackCount = 1;

    inv1.AddItemToSlot(0, sword);
    EXPECT_TRUE(inv1.TransferItem(0, inv2));

    EXPECT_EQ(inv1.GetUsedSlots(), 0);
    EXPECT_EQ(inv2.GetUsedSlots(), 1);
    EXPECT_EQ(inv2.CountItem("sword_iron"), 1);
}

// ===== Inventory Sort Tests =====

TEST_F(InventoryTestFixture, Inventory_SortByCategory)
{
    Inventory inv(10);

    ItemInstance quest;
    quest.InstanceID = UUID();
    quest.ItemDefinitionID = "quest_amulet";
    quest.StackCount = 1;

    ItemInstance sword;
    sword.InstanceID = UUID();
    sword.ItemDefinitionID = "sword_iron";
    sword.StackCount = 1;

    ItemInstance potion;
    potion.InstanceID = UUID();
    potion.ItemDefinitionID = "health_potion";
    potion.StackCount = 5;

    inv.AddItem(quest);
    inv.AddItem(sword);
    inv.AddItem(potion);

    inv.SortByCategory();

    // Weapon < Consumable < QuestItem
    const auto* first = inv.GetItemAtSlot(0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->ItemDefinitionID, "sword_iron");
}

TEST_F(InventoryTestFixture, Inventory_SortByRarity)
{
    Inventory inv(10);

    ItemInstance common;
    common.InstanceID = UUID();
    common.ItemDefinitionID = "sword_iron";
    common.StackCount = 1;

    ItemInstance legendary;
    legendary.InstanceID = UUID();
    legendary.ItemDefinitionID = "quest_amulet";
    legendary.StackCount = 1;

    ItemInstance uncommon;
    uncommon.InstanceID = UUID();
    uncommon.ItemDefinitionID = "helmet_steel";
    uncommon.StackCount = 1;

    inv.AddItem(common);
    inv.AddItem(legendary);
    inv.AddItem(uncommon);

    inv.SortByRarity();

    // Sorted descending by rarity: Legendary > Uncommon > Common
    const auto* first = inv.GetItemAtSlot(0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->ItemDefinitionID, "quest_amulet");
}

// ===== Equipment Tests =====

TEST_F(InventoryTestFixture, Equipment_EquipUnequip)
{
    Inventory inv(10);
    EquipmentSlots equip;

    ItemInstance helmet;
    helmet.InstanceID = UUID();
    helmet.ItemDefinitionID = "helmet_steel";
    helmet.StackCount = 1;

    EXPECT_TRUE(equip.Equip(EquipmentSlots::Slot::Head, helmet, inv));
    EXPECT_FALSE(equip.IsSlotEmpty(EquipmentSlots::Slot::Head));

    auto const* equipped = equip.GetEquipped(EquipmentSlots::Slot::Head);
    ASSERT_NE(equipped, nullptr);
    EXPECT_EQ(equipped->ItemDefinitionID, "helmet_steel");

    EXPECT_TRUE(equip.Unequip(EquipmentSlots::Slot::Head, inv));
    EXPECT_TRUE(equip.IsSlotEmpty(EquipmentSlots::Slot::Head));
    EXPECT_EQ(inv.CountItem("helmet_steel"), 1);
}

TEST_F(InventoryTestFixture, Equipment_AttributeModifiers)
{
    Inventory inv(10);
    EquipmentSlots equip;

    ItemInstance helmet;
    helmet.InstanceID = UUID();
    helmet.ItemDefinitionID = "helmet_steel";
    helmet.StackCount = 1;

    equip.Equip(EquipmentSlots::Slot::Head, helmet, inv);

    auto modifiers = equip.GetAllAttributeModifiers();
    EXPECT_EQ(modifiers.size(), 2u);

    // Check Defense and MaxHealth modifiers
    bool hasDefense = false;
    bool hasMaxHealth = false;
    for (auto const& [attr, val] : modifiers)
    {
        if (attr == "Defense" && val == 10.0f)
            hasDefense = true;
        if (attr == "MaxHealth" && val == 50.0f)
            hasMaxHealth = true;
    }
    EXPECT_TRUE(hasDefense);
    EXPECT_TRUE(hasMaxHealth);
}

TEST_F(InventoryTestFixture, Equipment_AffixModifiers)
{
    Inventory inv(10);
    EquipmentSlots equip;

    ItemInstance sword;
    sword.InstanceID = UUID();
    sword.ItemDefinitionID = "sword_iron";
    sword.StackCount = 1;
    sword.Affixes.push_back({ "of Fire", "FireDamage", 25.0f });

    equip.Equip(EquipmentSlots::Slot::MainHand, sword, inv);

    auto modifiers = equip.GetAllAttributeModifiers();
    // Should have AttackPower from definition + FireDamage from affix
    EXPECT_EQ(modifiers.size(), 2u);
}

TEST_F(InventoryTestFixture, Equipment_SlotStringConversion)
{
    for (i32 i = 0; i < EquipmentSlots::SlotCount; ++i)
    {
        auto slot = static_cast<EquipmentSlots::Slot>(i);
        auto str = EquipmentSlots::SlotToString(slot);
        EXPECT_EQ(EquipmentSlots::SlotFromString(str), slot);
    }
}

// ===== LootTable Tests =====

TEST_F(InventoryTestFixture, LootTable_BasicRoll)
{
    LootTable table;
    table.TableID = "test_loot";
    table.MinDrops = 1;
    table.MaxDrops = 1;

    LootTableEntry entry;
    entry.ItemDefinitionID = "health_potion";
    entry.Weight = 1.0f;
    entry.MinCount = 1;
    entry.MaxCount = 5;
    table.Entries.push_back(entry);

    auto results = table.Roll();
    EXPECT_EQ(results.size(), 1u);
    if (!results.empty())
    {
        EXPECT_EQ(results[0].ItemDefinitionID, "health_potion");
        EXPECT_GE(results[0].StackCount, 1);
        EXPECT_LE(results[0].StackCount, 5);
    }
}

TEST_F(InventoryTestFixture, LootTable_NothingWeight)
{
    LootTable table;
    table.TableID = "nothing_loot";
    table.MinDrops = 100;
    table.MaxDrops = 100;
    table.NothingWeight = 1000000.0f; // Extremely high chance of nothing

    LootTableEntry entry;
    entry.ItemDefinitionID = "sword_iron";
    entry.Weight = 0.00001f; // Near-zero chance
    table.Entries.push_back(entry);

    auto results = table.Roll();
    // With such a high nothing weight, virtually all drops should be nothing
    EXPECT_LE(static_cast<i32>(results.size()), 5);
}

TEST_F(InventoryTestFixture, LootTable_EmptyTable)
{
    LootTable table;
    table.TableID = "empty";
    table.MinDrops = 5;
    table.MaxDrops = 5;

    auto results = table.Roll();
    EXPECT_TRUE(results.empty());
}

TEST_F(InventoryTestFixture, LootTable_StatisticalDistribution)
{
    LootTable table;
    table.TableID = "distribution_test";
    table.MinDrops = 1;
    table.MaxDrops = 1;

    LootTableEntry swordEntry;
    swordEntry.ItemDefinitionID = "sword_iron";
    swordEntry.Weight = 50.0f;
    table.Entries.push_back(swordEntry);

    LootTableEntry potionEntry;
    potionEntry.ItemDefinitionID = "health_potion";
    potionEntry.Weight = 50.0f;
    table.Entries.push_back(potionEntry);

    i32 swordCount = 0;
    i32 potionCount = 0;
    constexpr i32 NUM_ROLLS = 10000;

    for (i32 i = 0; i < NUM_ROLLS; ++i)
    {
        auto results = table.Roll();
        if (!results.empty())
        {
            if (results[0].ItemDefinitionID == "sword_iron")
                swordCount++;
            else if (results[0].ItemDefinitionID == "health_potion")
                potionCount++;
        }
    }

    // With equal weights, each should be roughly 50% (allow 10% tolerance)
    f32 swordRatio = static_cast<f32>(swordCount) / static_cast<f32>(NUM_ROLLS);
    f32 potionRatio = static_cast<f32>(potionCount) / static_cast<f32>(NUM_ROLLS);

    EXPECT_GE(swordRatio, 0.40f);
    EXPECT_LE(swordRatio, 0.60f);
    EXPECT_GE(potionRatio, 0.40f);
    EXPECT_LE(potionRatio, 0.60f);
}

TEST_F(InventoryTestFixture, LootTable_ItemLevelFilter)
{
    LootTable table;
    table.TableID = "level_filter";
    table.MinDrops = 1;
    table.MaxDrops = 1;

    LootTableEntry lowLevelEntry;
    lowLevelEntry.ItemDefinitionID = "health_potion";
    lowLevelEntry.Weight = 1.0f;
    lowLevelEntry.MinItemLevel = 0.0f;
    lowLevelEntry.MaxItemLevel = 10.0f;
    table.Entries.push_back(lowLevelEntry);

    LootTableEntry highLevelEntry;
    highLevelEntry.ItemDefinitionID = "sword_iron";
    highLevelEntry.Weight = 1.0f;
    highLevelEntry.MinItemLevel = 50.0f;
    highLevelEntry.MaxItemLevel = 100.0f;
    table.Entries.push_back(highLevelEntry);

    // At level 5, only potions should drop
    for (i32 i = 0; i < 100; ++i)
    {
        auto results = table.Roll(5.0f);
        if (!results.empty())
        {
            EXPECT_EQ(results[0].ItemDefinitionID, "health_potion");
        }
    }

    // At level 75, only swords should drop
    for (i32 i = 0; i < 100; ++i)
    {
        auto results = table.Roll(75.0f);
        if (!results.empty())
        {
            EXPECT_EQ(results[0].ItemDefinitionID, "sword_iron");
        }
    }
}
