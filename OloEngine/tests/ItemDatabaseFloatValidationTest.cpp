// OLO_TEST_LAYER: unit
//
// Guards ItemDatabase::LoadFromDirectory against non-finite floats in hand-edited or
// corrupt .oloitem files. A NaN/inf Weight poisons Inventory::GetTotalWeight (def->Weight
// * StackCount), breaking carry-capacity checks; a NaN attribute modifier corrupts stat
// math. The loader must substitute safe fallbacks. See cpp-coding-quality.md §2b.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Gameplay/Inventory/Item.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace OloEngine;

namespace
{
    class ItemDatabaseFloatValidationTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            ItemDatabase::Clear();
            m_Dir = std::filesystem::temp_directory_path() / "olo_item_floatval_test";
            std::error_code ec;
            std::filesystem::remove_all(m_Dir, ec);
            std::filesystem::create_directories(m_Dir, ec);
        }

        void TearDown() override
        {
            ItemDatabase::Clear();
            std::error_code ec;
            std::filesystem::remove_all(m_Dir, ec);
        }

        void WriteItem(const std::string& fileName, const std::string& body)
        {
            std::ofstream out(m_Dir / fileName);
            out << body;
            out.close();
            ItemDatabase::LoadFromDirectory(m_Dir.string());
        }

        std::filesystem::path m_Dir;
    };

    TEST_F(ItemDatabaseFloatValidationTest, NaNWeightFallsBackToZero)
    {
        WriteItem("nan_weight.oloitem",
                  "ItemDefinition:\n"
                  "  ItemID: i_nan_weight\n"
                  "  Weight: .nan\n");

        const auto* def = ItemDatabase::Get("i_nan_weight");
        ASSERT_NE(def, nullptr);
        EXPECT_TRUE(std::isfinite(def->Weight));
        EXPECT_FLOAT_EQ(def->Weight, 0.0f);
    }

    TEST_F(ItemDatabaseFloatValidationTest, NegativeWeightIsClampedToZero)
    {
        WriteItem("neg_weight.oloitem",
                  "ItemDefinition:\n"
                  "  ItemID: i_neg_weight\n"
                  "  Weight: -2.5\n");

        const auto* def = ItemDatabase::Get("i_neg_weight");
        ASSERT_NE(def, nullptr);
        EXPECT_FLOAT_EQ(def->Weight, 0.0f);
    }

    TEST_F(ItemDatabaseFloatValidationTest, InfAttributeModifierFallsBackToZero)
    {
        WriteItem("inf_modifier.oloitem",
                  "ItemDefinition:\n"
                  "  ItemID: i_inf_mod\n"
                  "  Weight: 1.0\n"
                  "  AttributeModifiers:\n"
                  "    - Attribute: AttackPower\n"
                  "      Value: .inf\n");

        const auto* def = ItemDatabase::Get("i_inf_mod");
        ASSERT_NE(def, nullptr);
        ASSERT_EQ(def->AttributeModifiers.size(), 1u);
        EXPECT_EQ(def->AttributeModifiers[0].first, "AttackPower");
        EXPECT_TRUE(std::isfinite(def->AttributeModifiers[0].second));
        EXPECT_FLOAT_EQ(def->AttributeModifiers[0].second, 0.0f);
    }

    TEST_F(ItemDatabaseFloatValidationTest, ValidValuesIncludingNegativeModifierAreKept)
    {
        WriteItem("valid.oloitem",
                  "ItemDefinition:\n"
                  "  ItemID: i_valid\n"
                  "  Weight: 3.5\n"
                  "  AttributeModifiers:\n"
                  "    - Attribute: Defense\n"
                  "      Value: -10.0\n"); // negative modifiers (debuffs) are legitimate

        const auto* def = ItemDatabase::Get("i_valid");
        ASSERT_NE(def, nullptr);
        EXPECT_FLOAT_EQ(def->Weight, 3.5f);
        ASSERT_EQ(def->AttributeModifiers.size(), 1u);
        EXPECT_FLOAT_EQ(def->AttributeModifiers[0].second, -10.0f);
    }
} // namespace
