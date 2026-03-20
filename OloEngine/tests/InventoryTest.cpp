#include <gtest/gtest.h>

#include <optional>

#include "OloEngine/Gameplay/Inventory/Item.h"
#include "OloEngine/Gameplay/Inventory/ItemInstance.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Inventory/Inventory.h"
#include "OloEngine/Gameplay/Inventory/EquipmentSlots.h"
#include "OloEngine/Gameplay/Inventory/LootTable.h"
#include "OloEngine/Gameplay/Inventory/AffixDatabase.h"

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
        AffixDatabase::Clear();
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

    EXPECT_TRUE(inv.AddItem(helmet));
    EXPECT_TRUE(equip.Equip(EquipmentSlots::Slot::Head, helmet, inv));
    EXPECT_EQ(inv.GetUsedSlots(), 0);
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

    inv.AddItem(helmet);
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
    sword.Affixes.push_back({ .Name = "of Fire", .Attribute = "FireDamage", .Value = 25.0f });

    inv.AddItem(sword);
    equip.Equip(EquipmentSlots::Slot::MainHand, sword, inv);

    auto modifiers = equip.GetAllAttributeModifiers();
    // Should have AttackPower from definition + FireDamage from affix
    EXPECT_EQ(modifiers.size(), 2u);

    auto findMod = [&](const std::string& attr) -> std::optional<f32>
    {
        for (auto const& [name, val] : modifiers)
        {
            if (name == attr)
                return val;
        }
        return std::nullopt;
    };
    auto attackPower = findMod("AttackPower");
    ASSERT_TRUE(attackPower.has_value());
    EXPECT_FLOAT_EQ(*attackPower, 15.0f);
    auto fireDamage = findMod("FireDamage");
    ASSERT_TRUE(fireDamage.has_value());
    EXPECT_FLOAT_EQ(*fireDamage, 25.0f);
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
    LootTableEntry entry;
    entry.ItemDefinitionID = "sword_iron";
    entry.Weight = 1.0f;

    // Control table: NothingWeight = 0 → every roll yields an item
    LootTable control;
    control.TableID = "control";
    control.MinDrops = 100;
    control.MaxDrops = 100;
    control.NothingWeight = 0.0f;
    control.Entries.push_back(entry);

    auto controlResults = control.Roll();
    EXPECT_EQ(static_cast<i32>(controlResults.size()), 100);

    // Test table: extremely high NothingWeight → most rolls produce nothing
    LootTable table;
    table.TableID = "nothing_loot";
    table.MinDrops = 100;
    table.MaxDrops = 100;
    table.NothingWeight = 1000000.0f;
    entry.Weight = 0.00001f;
    table.Entries.push_back(entry);

    auto results = table.Roll();
    EXPECT_LT(static_cast<i32>(results.size()), static_cast<i32>(controlResults.size()));
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
        ASSERT_EQ(results.size(), 1u);
        auto const& id = results[0].ItemDefinitionID;
        EXPECT_TRUE(id == "sword_iron" || id == "health_potion");
        if (id == "sword_iron")
            swordCount++;
        else if (id == "health_potion")
            potionCount++;
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
        ASSERT_EQ(results.size(), 1u);
        EXPECT_EQ(results[0].ItemDefinitionID, "health_potion");
    }

    // At level 75, only swords should drop
    for (i32 i = 0; i < 100; ++i)
    {
        auto results = table.Roll(75.0f);
        ASSERT_EQ(results.size(), 1u);
        EXPECT_EQ(results[0].ItemDefinitionID, "sword_iron");
    }
}

// ===== AffixDatabase Tests =====

TEST_F(InventoryTestFixture, AffixDatabase_RegisterAndGet)
{
    AffixDefinition def;
    def.AffixID = "fire_damage";
    def.Type = AffixType::Prefix;
    def.Attribute = "FireDamage";
    def.Description = "Adds fire damage";
    def.Tiers = { { 1, "Heated", 5.0f, 10.0f, 0.0f },
                  { 2, "Flaming", 11.0f, 20.0f, 10.0f },
                  { 3, "Scorching", 21.0f, 40.0f, 25.0f } };
    AffixDatabase::Register(def);

    const auto* fetched = AffixDatabase::Get("fire_damage");
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(fetched->AffixID, "fire_damage");
    EXPECT_EQ(fetched->Type, AffixType::Prefix);
    EXPECT_EQ(fetched->Attribute, "FireDamage");
    EXPECT_EQ(fetched->Tiers.size(), 3u);
    EXPECT_EQ(fetched->Tiers[0].DisplayName, "Heated");

    EXPECT_EQ(AffixDatabase::Get("nonexistent"), nullptr);
}

TEST_F(InventoryTestFixture, AffixDatabase_RegisterPoolAndGet)
{
    AffixPool pool;
    pool.PoolID = "weapon_prefixes";
    pool.AffixIDs = { "fire_damage", "cold_damage" };
    AffixDatabase::RegisterPool(pool);

    const auto* fetched = AffixDatabase::GetPool("weapon_prefixes");
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(fetched->PoolID, "weapon_prefixes");
    EXPECT_EQ(fetched->AffixIDs.size(), 2u);

    EXPECT_EQ(AffixDatabase::GetPool("nonexistent"), nullptr);
}

TEST_F(InventoryTestFixture, AffixDatabase_GetAll)
{
    AffixDefinition def1;
    def1.AffixID = "fire_damage";
    def1.Type = AffixType::Prefix;
    def1.Attribute = "FireDamage";
    def1.Tiers = { { 1, "Heated", 5.0f, 10.0f, 0.0f } };
    AffixDatabase::Register(def1);

    AffixDefinition def2;
    def2.AffixID = "cold_damage";
    def2.Type = AffixType::Suffix;
    def2.Attribute = "ColdDamage";
    def2.Tiers = { { 1, "of Frost", 3.0f, 8.0f, 0.0f } };
    AffixDatabase::Register(def2);

    auto all = AffixDatabase::GetAll();
    EXPECT_EQ(all.size(), 2u);
}

TEST_F(InventoryTestFixture, AffixDatabase_ClearRemovesAll)
{
    AffixDefinition def;
    def.AffixID = "test_affix";
    def.Type = AffixType::Prefix;
    def.Attribute = "TestAttr";
    def.Tiers = { { 1, "Test T1", 1.0f, 5.0f, 0.0f } };
    AffixDatabase::Register(def);

    AffixPool pool;
    pool.PoolID = "test_pool";
    pool.AffixIDs = { "test_affix" };
    AffixDatabase::RegisterPool(pool);

    ASSERT_NE(AffixDatabase::Get("test_affix"), nullptr);
    ASSERT_NE(AffixDatabase::GetPool("test_pool"), nullptr);

    AffixDatabase::Clear();

    EXPECT_EQ(AffixDatabase::Get("test_affix"), nullptr);
    EXPECT_EQ(AffixDatabase::GetPool("test_pool"), nullptr);
    EXPECT_TRUE(AffixDatabase::GetAll().empty());
}

// ===== Affix Roll Tests =====

TEST_F(InventoryTestFixture, LootTable_RollWithAffixPool)
{
    // Register affix definitions + pool
    AffixDefinition fireDmg;
    fireDmg.AffixID = "fire_damage";
    fireDmg.Type = AffixType::Prefix;
    fireDmg.Attribute = "FireDamage";
    fireDmg.Tiers = { { 1, "Heated", 5.0f, 10.0f, 0.0f } };
    AffixDatabase::Register(fireDmg);

    AffixDefinition lifeSteal;
    lifeSteal.AffixID = "life_steal";
    lifeSteal.Type = AffixType::Suffix;
    lifeSteal.Attribute = "LifeSteal";
    lifeSteal.Tiers = { { 1, "of Leeching", 1.0f, 3.0f, 0.0f } };
    AffixDatabase::Register(lifeSteal);

    AffixPool pool;
    pool.PoolID = "weapon_pool";
    pool.AffixIDs = { "fire_damage", "life_steal" };
    AffixDatabase::RegisterPool(pool);

    LootTable table;
    table.TableID = "affix_test";
    table.MinDrops = 1;
    table.MaxDrops = 1;

    LootTableEntry entry;
    entry.ItemDefinitionID = "sword_iron";
    entry.Weight = 1.0f;
    entry.MinAffixes = 2;
    entry.MaxAffixes = 2;
    entry.PossibleAffixPoolIDs = { "weapon_pool" };
    table.Entries.push_back(entry);

    auto results = table.Roll(10.0f);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].ItemDefinitionID, "sword_iron");
    EXPECT_EQ(results[0].Affixes.size(), 2u);

    for (auto const& affix : results[0].Affixes)
    {
        EXPECT_FALSE(affix.DefinitionID.empty());
        EXPECT_FALSE(affix.Name.empty());
        EXPECT_FALSE(affix.Attribute.empty());
        EXPECT_GT(affix.Value, 0.0f);
        EXPECT_GE(affix.Tier, 1);
    }
}

TEST_F(InventoryTestFixture, LootTable_AffixTierSelection)
{
    AffixDefinition dmg;
    dmg.AffixID = "phys_damage";
    dmg.Type = AffixType::Prefix;
    dmg.Attribute = "PhysDamage";
    dmg.Tiers = { { 1, "Heavy", 5.0f, 10.0f, 0.0f },
                  { 2, "Massive", 15.0f, 25.0f, 20.0f },
                  { 3, "Devastating", 30.0f, 50.0f, 50.0f } };
    AffixDatabase::Register(dmg);

    AffixPool pool;
    pool.PoolID = "tier_test_pool";
    pool.AffixIDs = { "phys_damage" };
    AffixDatabase::RegisterPool(pool);

    LootTable table;
    table.TableID = "tier_test";
    table.MinDrops = 1;
    table.MaxDrops = 1;

    LootTableEntry entry;
    entry.ItemDefinitionID = "sword_iron";
    entry.Weight = 1.0f;
    entry.MinAffixes = 1;
    entry.MaxAffixes = 1;
    entry.PossibleAffixPoolIDs = { "tier_test_pool" };
    table.Entries.push_back(entry);

    // At level 5, only tier 1 is eligible (MinItemLevel 0)
    for (i32 i = 0; i < 50; ++i)
    {
        auto results = table.Roll(5.0f);
        ASSERT_EQ(results.size(), 1u);
        ASSERT_EQ(results[0].Affixes.size(), 1u);
        EXPECT_EQ(results[0].Affixes[0].Tier, 1);
        EXPECT_EQ(results[0].Affixes[0].Name, "Heavy");
        EXPECT_GE(results[0].Affixes[0].Value, 5.0f);
        EXPECT_LE(results[0].Affixes[0].Value, 10.0f);
    }

    // At level 30, tier 2 is best eligible (MinItemLevel 20)
    for (i32 i = 0; i < 50; ++i)
    {
        auto results = table.Roll(30.0f);
        ASSERT_EQ(results.size(), 1u);
        ASSERT_EQ(results[0].Affixes.size(), 1u);
        EXPECT_EQ(results[0].Affixes[0].Tier, 2);
        EXPECT_EQ(results[0].Affixes[0].Name, "Massive");
        EXPECT_GE(results[0].Affixes[0].Value, 15.0f);
        EXPECT_LE(results[0].Affixes[0].Value, 25.0f);
    }

    // At level 60, tier 3 is best eligible (MinItemLevel 50)
    for (i32 i = 0; i < 50; ++i)
    {
        auto results = table.Roll(60.0f);
        ASSERT_EQ(results.size(), 1u);
        ASSERT_EQ(results[0].Affixes.size(), 1u);
        EXPECT_EQ(results[0].Affixes[0].Tier, 3);
        EXPECT_EQ(results[0].Affixes[0].Name, "Devastating");
        EXPECT_GE(results[0].Affixes[0].Value, 30.0f);
        EXPECT_LE(results[0].Affixes[0].Value, 50.0f);
    }
}

TEST_F(InventoryTestFixture, LootTable_AffixPrefixSuffixLimits)
{
    // Register 4 prefix and 4 suffix affixes — should cap at 3/3
    for (i32 i = 0; i < 4; ++i)
    {
        AffixDefinition prefix;
        prefix.AffixID = "prefix_" + std::to_string(i);
        prefix.Type = AffixType::Prefix;
        prefix.Attribute = "PrefixAttr" + std::to_string(i);
        prefix.Tiers = { { 1, "P" + std::to_string(i), 1.0f, 5.0f, 0.0f } };
        AffixDatabase::Register(prefix);

        AffixDefinition suffix;
        suffix.AffixID = "suffix_" + std::to_string(i);
        suffix.Type = AffixType::Suffix;
        suffix.Attribute = "SuffixAttr" + std::to_string(i);
        suffix.Tiers = { { 1, "S" + std::to_string(i), 1.0f, 5.0f, 0.0f } };
        AffixDatabase::Register(suffix);
    }

    AffixPool pool;
    pool.PoolID = "limit_pool";
    for (i32 i = 0; i < 4; ++i)
    {
        pool.AffixIDs.push_back("prefix_" + std::to_string(i));
        pool.AffixIDs.push_back("suffix_" + std::to_string(i));
    }
    AffixDatabase::RegisterPool(pool);

    LootTable table;
    table.TableID = "limit_test";
    table.MinDrops = 1;
    table.MaxDrops = 1;

    LootTableEntry entry;
    entry.ItemDefinitionID = "sword_iron";
    entry.Weight = 1.0f;
    entry.MinAffixes = 8; // Request 8, but max 3+3=6
    entry.MaxAffixes = 8;
    entry.PossibleAffixPoolIDs = { "limit_pool" };
    table.Entries.push_back(entry);

    // Roll many times and verify the limits hold
    for (i32 trial = 0; trial < 50; ++trial)
    {
        auto results = table.Roll(10.0f);
        ASSERT_EQ(results.size(), 1u);

        i32 prefixCount = 0;
        i32 suffixCount = 0;
        for (auto const& affix : results[0].Affixes)
        {
            if (affix.Type == AffixType::Prefix)
                ++prefixCount;
            else
                ++suffixCount;
        }
        EXPECT_LE(prefixCount, 3);
        EXPECT_LE(suffixCount, 3);
    }
}

TEST_F(InventoryTestFixture, LootTable_LegacyAffixFallback)
{
    // Don't register any pool — should fall back to legacy behavior
    LootTable table;
    table.TableID = "legacy_test";
    table.MinDrops = 1;
    table.MaxDrops = 1;

    LootTableEntry entry;
    entry.ItemDefinitionID = "sword_iron";
    entry.Weight = 1.0f;
    entry.MinAffixes = 1;
    entry.MaxAffixes = 1;
    entry.PossibleAffixPoolIDs = { "unregistered_pool" };
    table.Entries.push_back(entry);

    auto results = table.Roll();
    ASSERT_EQ(results.size(), 1u);
    ASSERT_EQ(results[0].Affixes.size(), 1u);

    // Legacy fallback uses pool ID as Name/Attribute
    EXPECT_EQ(results[0].Affixes[0].Name, "unregistered_pool");
    EXPECT_EQ(results[0].Affixes[0].Attribute, "unregistered_pool");
    EXPECT_GE(results[0].Affixes[0].Value, 1.0f);
    EXPECT_LE(results[0].Affixes[0].Value, 10.0f);
    // Legacy affixes have empty DefinitionID and default Type/Tier
    EXPECT_TRUE(results[0].Affixes[0].DefinitionID.empty());
    EXPECT_EQ(results[0].Affixes[0].Tier, 0);
}

TEST_F(InventoryTestFixture, AffixTypeStringConversion)
{
    EXPECT_STREQ(AffixTypeToString(AffixType::Prefix), "Prefix");
    EXPECT_STREQ(AffixTypeToString(AffixType::Suffix), "Suffix");
    EXPECT_EQ(AffixTypeFromString("Prefix"), AffixType::Prefix);
    EXPECT_EQ(AffixTypeFromString("Suffix"), AffixType::Suffix);
    EXPECT_EQ(AffixTypeFromString("invalid"), AffixType::Prefix); // Default
}
