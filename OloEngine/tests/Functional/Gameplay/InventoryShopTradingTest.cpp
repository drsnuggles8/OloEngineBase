#include "OloEnginePCH.h"

// =============================================================================
// InventoryShopTradingTest — Functional Test.
//
// Cross-subsystem seam under test:
//   InventorySystem (entity-aware service layer) × ItemDatabase (pricing) ×
//   Inventory (player goods + shop stock) × InventoryComponent::Currency ×
//   GameplayEventBus.
//
// Pins the vendor / shop trading slice: a shop is any entity with an
// ItemContainerComponent whose IsShop flag is set (its Contents is the stock);
// a buyer/seller is an entity with an InventoryComponent (its Currency funds the
// trade). BuyItem/SellItem validate the shop flag, stock/ownership,
// affordability, and destination room *before* moving any currency or items, so
// a rejected trade is a no-op on both sides. Pricing comes from
// ItemDefinition::BuyPrice / SellPrice, with a half-BuyPrice buyback fallback
// (integer-truncated) when SellPrice is unset.
//
// Covers: buy debits + transfers + ItemBoughtEvent; broke / out-of-stock /
// not-a-shop / full-buyer rejections (each a no-op); sell credits + transfers +
// ItemSoldEvent; the buyback fallback and its truncation-to-zero edge; and a
// multi-stack quantity that spans more than one source slot.
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
#include "OloEngine/Gameplay/Inventory/Item.h"

#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

class InventoryShopTradingTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        ItemDatabase::Clear();

        // Stackable consumable with an explicit buy and sell price.
        ItemDefinition potion;
        potion.ItemID = "potion";
        potion.DisplayName = "Potion";
        potion.Category = ItemCategory::Consumable;
        potion.MaxStackSize = 10;
        potion.BuyPrice = 10;
        potion.SellPrice = 4;
        ItemDatabase::Register(potion);

        // Non-stackable weapon with NO explicit SellPrice — exercises the
        // half-BuyPrice buyback fallback (100 -> 50).
        ItemDefinition sword;
        sword.ItemID = "sword";
        sword.DisplayName = "Sword";
        sword.Category = ItemCategory::Weapon;
        sword.MaxStackSize = 1;
        sword.BuyPrice = 100;
        sword.SellPrice = 0;
        ItemDatabase::Register(sword);

        // BuyPrice 1, no SellPrice -> buyback truncates to 0 (rounding edge).
        ItemDefinition gem;
        gem.ItemID = "gem";
        gem.DisplayName = "Gem";
        gem.Category = ItemCategory::Material;
        gem.MaxStackSize = 99;
        gem.BuyPrice = 1;
        gem.SellPrice = 0;
        ItemDatabase::Register(gem);

        // Buyer/seller: an InventoryComponent with starting currency.
        m_Buyer = GetScene().CreateEntity("Player");
        auto& buyerInv = m_Buyer.AddComponent<InventoryComponent>();
        buyerInv.Currency = 100;
        m_BuyerUUID = m_Buyer.GetUUID();

        // Shop: an ItemContainerComponent with IsShop set, stocked.
        m_Shop = GetScene().CreateEntity("Vendor");
        auto& container = m_Shop.AddComponent<ItemContainerComponent>();
        container.IsShop = true;
        container.Contents.AddItem(MakeStack("potion", 5));
        container.Contents.AddItem(MakeStack("sword", 1));
        m_ShopUUID = m_Shop.GetUUID();
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        ItemDatabase::Clear();
    }

    static ItemInstance MakeStack(const std::string& defId, i32 count)
    {
        ItemInstance inst;
        inst.InstanceID = UUID();
        inst.ItemDefinitionID = defId;
        inst.StackCount = count;
        return inst;
    }

    Inventory& BuyerInv()
    {
        return m_Buyer.GetComponent<InventoryComponent>().PlayerInventory;
    }
    i32& BuyerCurrency()
    {
        return m_Buyer.GetComponent<InventoryComponent>().Currency;
    }
    Inventory& ShopStock()
    {
        return m_Shop.GetComponent<ItemContainerComponent>().Contents;
    }

    Entity m_Buyer;
    Entity m_Shop;
    UUID m_BuyerUUID;
    UUID m_ShopUUID;
};

TEST_F(InventoryShopTradingTest, BuyDebitsCurrencyTransfersItemAndPublishesEvent)
{
    std::vector<ItemBoughtEvent> bought;
    GetScene().GetGameplayEvents().Subscribe<ItemBoughtEvent>([&](const ItemBoughtEvent& e)
                                                              { bought.push_back(e); });

    ASSERT_TRUE(InventorySystem::BuyItem(&GetScene(), m_Buyer, m_Shop, "potion", 3));

    EXPECT_EQ(BuyerCurrency(), 70) << "buyer should be debited 3 * BuyPrice(10) = 30.";
    EXPECT_EQ(BuyerInv().CountItem("potion"), 3) << "bought potions should land in the buyer's inventory.";
    EXPECT_EQ(ShopStock().CountItem("potion"), 2) << "stock should drop from 5 to 2.";

    ASSERT_EQ(bought.size(), 1u);
    EXPECT_EQ(static_cast<u64>(bought[0].BuyerEntityID), static_cast<u64>(m_BuyerUUID));
    EXPECT_EQ(static_cast<u64>(bought[0].ShopEntityID), static_cast<u64>(m_ShopUUID));
    EXPECT_EQ(bought[0].ItemDefinitionID, "potion");
    EXPECT_EQ(bought[0].Quantity, 3);
    EXPECT_EQ(bought[0].TotalPrice, 30);
}

TEST_F(InventoryShopTradingTest, BuyFailsWhenBrokeAndChangesNothing)
{
    BuyerCurrency() = 25; // 3 potions cost 30 — affordable stock, unaffordable price

    std::vector<ItemBoughtEvent> bought;
    GetScene().GetGameplayEvents().Subscribe<ItemBoughtEvent>([&](const ItemBoughtEvent& e)
                                                              { bought.push_back(e); });

    EXPECT_FALSE(InventorySystem::BuyItem(&GetScene(), m_Buyer, m_Shop, "potion", 3));

    EXPECT_EQ(BuyerCurrency(), 25) << "a rejected (broke) buy must not debit currency.";
    EXPECT_EQ(BuyerInv().CountItem("potion"), 0);
    EXPECT_EQ(ShopStock().CountItem("potion"), 5) << "stock must be untouched on a rejected buy.";
    EXPECT_TRUE(bought.empty()) << "no ItemBoughtEvent on a rejected buy.";
}

TEST_F(InventoryShopTradingTest, BuyFailsWhenOutOfStockEvenIfAffordable)
{
    // 10 potions cost 100 — exactly affordable, but stock is only 5.
    EXPECT_FALSE(InventorySystem::BuyItem(&GetScene(), m_Buyer, m_Shop, "potion", 10));

    EXPECT_EQ(BuyerCurrency(), 100) << "out-of-stock buy must not debit currency.";
    EXPECT_EQ(BuyerInv().CountItem("potion"), 0);
    EXPECT_EQ(ShopStock().CountItem("potion"), 5);
}

TEST_F(InventoryShopTradingTest, BuyFailsWhenContainerIsNotAShop)
{
    m_Shop.GetComponent<ItemContainerComponent>().IsShop = false;

    EXPECT_FALSE(InventorySystem::BuyItem(&GetScene(), m_Buyer, m_Shop, "potion", 1));

    EXPECT_EQ(BuyerCurrency(), 100);
    EXPECT_EQ(BuyerInv().CountItem("potion"), 0);
    EXPECT_EQ(ShopStock().CountItem("potion"), 5);
}

TEST_F(InventoryShopTradingTest, BuyRollsBackWhenBuyerInventoryHasNoRoom)
{
    // Shrink the buyer to a single slot and occupy it with a non-stackable
    // sword, so a bought sword has nowhere to go.
    auto& inv = BuyerInv();
    inv.SetCapacity(1);
    ASSERT_TRUE(inv.AddItem(MakeStack("sword", 1)));

    EXPECT_FALSE(InventorySystem::BuyItem(&GetScene(), m_Buyer, m_Shop, "sword", 1));

    EXPECT_EQ(BuyerCurrency(), 100) << "a buy that can't be delivered must not debit currency (atomic).";
    EXPECT_EQ(inv.CountItem("sword"), 1) << "buyer keeps only its original sword.";
    EXPECT_EQ(ShopStock().CountItem("sword"), 1) << "shop keeps the undelivered sword.";
}

TEST_F(InventoryShopTradingTest, SellCreditsCurrencyTransfersItemAndPublishesEvent)
{
    ASSERT_TRUE(BuyerInv().AddItem(MakeStack("potion", 4)));

    std::vector<ItemSoldEvent> sold;
    GetScene().GetGameplayEvents().Subscribe<ItemSoldEvent>([&](const ItemSoldEvent& e)
                                                            { sold.push_back(e); });

    ASSERT_TRUE(InventorySystem::SellItem(&GetScene(), m_Buyer, m_Shop, "potion", 2));

    EXPECT_EQ(BuyerCurrency(), 108) << "seller credited 2 * SellPrice(4) = 8.";
    EXPECT_EQ(BuyerInv().CountItem("potion"), 2) << "sold potions leave the seller's inventory.";
    EXPECT_EQ(ShopStock().CountItem("potion"), 7) << "shop stock grows from 5 to 7.";

    ASSERT_EQ(sold.size(), 1u);
    EXPECT_EQ(static_cast<u64>(sold[0].SellerEntityID), static_cast<u64>(m_BuyerUUID));
    EXPECT_EQ(static_cast<u64>(sold[0].ShopEntityID), static_cast<u64>(m_ShopUUID));
    EXPECT_EQ(sold[0].ItemDefinitionID, "potion");
    EXPECT_EQ(sold[0].Quantity, 2);
    EXPECT_EQ(sold[0].TotalPrice, 8);
}

TEST_F(InventoryShopTradingTest, SellUsesHalfBuyPriceBuybackWhenNoSellPrice)
{
    ASSERT_TRUE(BuyerInv().AddItem(MakeStack("sword", 1)));

    EXPECT_EQ(InventorySystem::GetSellPrice("sword", 1), 50) << "no SellPrice -> half of BuyPrice(100).";

    ASSERT_TRUE(InventorySystem::SellItem(&GetScene(), m_Buyer, m_Shop, "sword", 1));
    EXPECT_EQ(BuyerCurrency(), 150) << "seller credited the 50 buyback.";
    EXPECT_EQ(BuyerInv().CountItem("sword"), 0);
}

TEST_F(InventoryShopTradingTest, SellBuybackTruncatesToZeroForCheapItems)
{
    ASSERT_TRUE(BuyerInv().AddItem(MakeStack("gem", 1)));

    // BuyPrice 1, no SellPrice -> 1 / 2 truncates to 0 (documented rounding).
    EXPECT_EQ(InventorySystem::GetSellPrice("gem", 1), 0);

    std::vector<ItemSoldEvent> sold;
    GetScene().GetGameplayEvents().Subscribe<ItemSoldEvent>([&](const ItemSoldEvent& e)
                                                            { sold.push_back(e); });

    // The sale still succeeds and transfers the item — it just pays nothing.
    ASSERT_TRUE(InventorySystem::SellItem(&GetScene(), m_Buyer, m_Shop, "gem", 1));
    EXPECT_EQ(BuyerCurrency(), 100) << "a 0-value buyback leaves currency unchanged.";
    EXPECT_EQ(BuyerInv().CountItem("gem"), 0) << "the item still moves to the shop.";
    EXPECT_EQ(ShopStock().CountItem("gem"), 1);
    ASSERT_EQ(sold.size(), 1u);
    EXPECT_EQ(sold[0].TotalPrice, 0);
}

TEST_F(InventoryShopTradingTest, SellFailsWhenSellerLacksTheItem)
{
    std::vector<ItemSoldEvent> sold;
    GetScene().GetGameplayEvents().Subscribe<ItemSoldEvent>([&](const ItemSoldEvent& e)
                                                            { sold.push_back(e); });

    EXPECT_FALSE(InventorySystem::SellItem(&GetScene(), m_Buyer, m_Shop, "gem", 1));

    EXPECT_EQ(BuyerCurrency(), 100) << "selling something you don't have must not credit currency.";
    EXPECT_EQ(ShopStock().CountItem("gem"), 0);
    EXPECT_TRUE(sold.empty());
}

TEST_F(InventoryShopTradingTest, BuyQuantitySpanningMultipleStacksSplitsAcrossSlots)
{
    // Stock 12 potions across two slots (maxStack 10 -> 10 + 2), then buy all 12.
    auto& stock = ShopStock();
    // BuildScene seeded 5; top up to 12 total.
    ASSERT_TRUE(stock.AddItem(MakeStack("potion", 5))); // -> 10 (stacks with the 5)
    ASSERT_TRUE(stock.AddItem(MakeStack("potion", 2))); // -> new slot of 2
    ASSERT_EQ(stock.CountItem("potion"), 12);

    BuyerCurrency() = 200; // 12 * 10 = 120
    ASSERT_TRUE(InventorySystem::BuyItem(&GetScene(), m_Buyer, m_Shop, "potion", 12));

    EXPECT_EQ(BuyerCurrency(), 80) << "200 - 120.";
    EXPECT_EQ(BuyerInv().CountItem("potion"), 12) << "all 12 land in the buyer, re-stacked.";
    EXPECT_EQ(stock.CountItem("potion"), 0) << "shop emptied of potions.";
}
