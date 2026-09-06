// OLO_TEST_LAYER: unit
// =============================================================================
// ExperienceCurveTest.cpp
//
// Unit tests for the ExperienceCurve asset (issue #635): the Formula and
// Table XP-per-level modes, the >= 1 floor that protects the level-up drain
// loop from a malformed curve, the engine-default linear curve, Sanitize()'s
// documented clamp ranges, and the ExperienceCurveSerializer string
// round-trip including hand-injected NaN/Inf YAML falling back sanely.
//
// Formula contract (ExperienceCurve.h / .cpp):
//   XP to advance FROM level L = round(m_BaseXP * pow(L, m_Exponent)),
//   clamped into [1, 2e9]. Table mode: m_Table[L-1], levels past the end
//   reuse the LAST entry, every entry floored to 1.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Gameplay/Progression/ExperienceCurve.h"

#include <cmath>
#include <limits>
#include <string>

using namespace OloEngine;

// ============================================================================
// Formula mode
// ============================================================================

TEST(ExperienceCurveTest, FormulaModeMatchesRoundedPowerLaw)
{
    ExperienceCurve curve;
    curve.m_Mode = ExperienceCurve::CurveMode::Formula;
    curve.m_BaseXP = 100.0f;
    curve.m_Exponent = 1.5f;

    // Derivations for round(100 * L^1.5), using L^1.5 == L * sqrt(L):
    //   L=1: 100 * 1.0            = 100
    //   L=2: 100 * 2.8284271247.. = 282.84.. -> 283
    //   L=3: 100 * 5.1961524227.. = 519.61.. -> 520
    //   L=4: 100 * 8.0            = 800
    //   L=9: 100 * 27.0           = 2700
    EXPECT_EQ(curve.GetXPForLevelUp(1), 100) << "L=1: 100 * 1^1.5 must be exactly 100";
    EXPECT_EQ(curve.GetXPForLevelUp(2), 283) << "L=2: round(100 * 2^1.5) = round(282.84..) must be 283";
    EXPECT_EQ(curve.GetXPForLevelUp(3), 520) << "L=3: round(100 * 3^1.5) = round(519.61..) must be 520";
    EXPECT_EQ(curve.GetXPForLevelUp(4), 800) << "L=4: 100 * 4^1.5 = 100 * 8 must be exactly 800";
    EXPECT_EQ(curve.GetXPForLevelUp(9), 2700) << "L=9: 100 * 9^1.5 = 100 * 27 must be exactly 2700";
}

TEST(ExperienceCurveTest, FormulaModeSquareExponent)
{
    ExperienceCurve curve;
    curve.m_Mode = ExperienceCurve::CurveMode::Formula;
    curve.m_BaseXP = 50.0f;
    curve.m_Exponent = 2.0f;

    // round(50 * L^2): L=3 -> 50*9 = 450, L=4 -> 50*16 = 800.
    EXPECT_EQ(curve.GetXPForLevelUp(3), 450) << "50 * 3^2 must be exactly 450";
    EXPECT_EQ(curve.GetXPForLevelUp(4), 800) << "50 * 4^2 must be exactly 800";
}

TEST(ExperienceCurveTest, FormulaModeClampsToI32SafeCeiling)
{
    ExperienceCurve curve;
    curve.m_Mode = ExperienceCurve::CurveMode::Formula;
    curve.m_BaseXP = 1e7f;    // documented max
    curve.m_Exponent = 10.0f; // documented max

    // 1e7 * 1000^10 = 1e37 vastly exceeds kMaxXPStep = 2e9; the clamp keeps
    // the result inside i32 so the drain loop's subtraction can't overflow.
    EXPECT_EQ(curve.GetXPForLevelUp(1000), 2000000000)
        << "an astronomically steep formula must clamp to the documented 2e9 ceiling";
}

TEST(ExperienceCurveTest, FormulaLevelBelowOneClampsToLevelOne)
{
    ExperienceCurve curve;
    curve.m_Mode = ExperienceCurve::CurveMode::Formula;
    curve.m_BaseXP = 100.0f;
    curve.m_Exponent = 1.5f;

    EXPECT_EQ(curve.GetXPForLevelUp(0), curve.GetXPForLevelUp(1))
        << "level 0 must be treated as level 1";
    EXPECT_EQ(curve.GetXPForLevelUp(-5), 100)
        << "a negative level must clamp to level 1 -> round(100 * 1^1.5) = 100";
}

TEST(ExperienceCurveTest, GarbageMembersStillYieldAtLeastOneXP)
{
    // GetXPForLevelUp validates its inputs in-place (before any Sanitize()
    // call): non-finite / sub-1 BaseXP falls back to 100, non-finite /
    // negative Exponent falls back to 1.0, oversized Exponent clamps to 10.
    ExperienceCurve curve;
    curve.m_Mode = ExperienceCurve::CurveMode::Formula;

    curve.m_BaseXP = std::numeric_limits<f32>::quiet_NaN();
    curve.m_Exponent = 1.5f;
    EXPECT_EQ(curve.GetXPForLevelUp(1), 100)
        << "NaN BaseXP must fall back to 100 -> round(100 * 1^1.5) = 100";

    curve.m_BaseXP = -50.0f;
    EXPECT_EQ(curve.GetXPForLevelUp(1), 100)
        << "negative BaseXP must fall back to 100";

    curve.m_BaseXP = 100.0f;
    curve.m_Exponent = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_EQ(curve.GetXPForLevelUp(2), 200)
        << "NaN Exponent must fall back to 1.0 -> round(100 * 2^1) = 200";

    curve.m_Exponent = -3.0f;
    EXPECT_EQ(curve.GetXPForLevelUp(2), 200)
        << "negative Exponent must fall back to 1.0 -> round(100 * 2^1) = 200";

    curve.m_Exponent = 50.0f;
    EXPECT_EQ(curve.GetXPForLevelUp(2), 102400)
        << "oversized Exponent must clamp to 10 -> round(100 * 2^10) = 102400";

    // Every garbage combination must still return >= 1 (the documented floor
    // that prevents divide-by-zero / infinite-levels-from-one-XP-drip).
    curve.m_BaseXP = std::numeric_limits<f32>::infinity();
    curve.m_Exponent = -std::numeric_limits<f32>::infinity();
    EXPECT_GE(curve.GetXPForLevelUp(1), 1) << "all-garbage formula members must still yield >= 1 XP";
}

// ============================================================================
// Table mode
// ============================================================================

TEST(ExperienceCurveTest, TableModeUsesEntriesAndReusesLastPastEnd)
{
    ExperienceCurve curve;
    curve.m_Mode = ExperienceCurve::CurveMode::Table;
    curve.m_Table = { 100, 200, 300 };

    EXPECT_EQ(curve.GetXPForLevelUp(1), 100) << "m_Table[0] is the XP to advance FROM level 1";
    EXPECT_EQ(curve.GetXPForLevelUp(2), 200) << "m_Table[1] is the XP to advance FROM level 2";
    EXPECT_EQ(curve.GetXPForLevelUp(3), 300) << "m_Table[2] is the XP to advance FROM level 3";
    EXPECT_EQ(curve.GetXPForLevelUp(4), 300) << "level 4 is past the table end and must reuse the LAST entry";
    EXPECT_EQ(curve.GetXPForLevelUp(999), 300) << "any level past the end must reuse the last entry";
    EXPECT_EQ(curve.GetXPForLevelUp(0), 100) << "level <= 0 must clamp to level 1 -> m_Table[0]";
}

TEST(ExperienceCurveTest, TableModeFloorsGarbageEntriesToOne)
{
    ExperienceCurve curve;
    curve.m_Mode = ExperienceCurve::CurveMode::Table;
    curve.m_Table = { 0, -7 };

    EXPECT_EQ(curve.GetXPForLevelUp(1), 1) << "a zero table entry must be floored to 1 at read time";
    EXPECT_EQ(curve.GetXPForLevelUp(2), 1) << "a negative table entry must be floored to 1 at read time";
}

TEST(ExperienceCurveTest, TableModeEmptyTableFallsBackToFormulaBranch)
{
    ExperienceCurve curve;
    curve.m_Mode = ExperienceCurve::CurveMode::Table;
    curve.m_Table.clear();
    curve.m_BaseXP = 100.0f;
    curve.m_Exponent = 1.5f;

    // The (Table && !empty) guard fails, so the formula path answers:
    // round(100 * 2^1.5) = 283 (derived in FormulaModeMatchesRoundedPowerLaw).
    EXPECT_EQ(curve.GetXPForLevelUp(2), 283)
        << "Table mode with an empty table must fall through to the formula";
}

// ============================================================================
// Max level + engine-default curve
// ============================================================================

TEST(ExperienceCurveTest, GetMaxLevelNeverBelowOne)
{
    ExperienceCurve curve;
    curve.m_MaxLevel = 0;
    EXPECT_EQ(curve.GetMaxLevel(), 1) << "MaxLevel 0 must read back as 1";
    curve.m_MaxLevel = -5;
    EXPECT_EQ(curve.GetMaxLevel(), 1) << "negative MaxLevel must read back as 1";
    curve.m_MaxLevel = 50;
    EXPECT_EQ(curve.GetMaxLevel(), 50) << "a valid MaxLevel must pass through unchanged";
}

TEST(ExperienceCurveTest, DefaultCurveIsLinearHundredPerLevel)
{
    // Engine-default curve (no asset assigned): 100 XP per current level.
    EXPECT_EQ(ExperienceCurve::DefaultXPForLevelUp(1), 100);
    EXPECT_EQ(ExperienceCurve::DefaultXPForLevelUp(5), 500);
    EXPECT_EQ(ExperienceCurve::DefaultXPForLevelUp(0), 100) << "level <= 0 must clamp to level 1";
    EXPECT_EQ(ExperienceCurve::DefaultXPForLevelUp(-3), 100) << "negative level must clamp to level 1";
    // Saturation: level clamps to 20'000'000 so 100 * level stays inside i32
    // (100 * 2e7 = 2e9 < INT32_MAX = 2147483647).
    EXPECT_EQ(ExperienceCurve::DefaultXPForLevelUp(30000000), 2000000000)
        << "huge level must saturate at 100 * 20'000'000 = 2'000'000'000";
    EXPECT_EQ(ExperienceCurve::kDefaultMaxLevel, 99);
}

// ============================================================================
// Sanitize()
// ============================================================================

TEST(ExperienceCurveTest, SanitizeClampsEveryMemberIntoDocumentedRange)
{
    ExperienceCurve curve;

    curve.m_MaxLevel = -5;
    curve.m_BaseXP = 0.5f;
    curve.m_Exponent = -1.0f;
    curve.m_Table = { 0, -7, 50 };
    curve.Sanitize();
    EXPECT_EQ(curve.m_MaxLevel, 1) << "MaxLevel must clamp up to 1";
    EXPECT_FLOAT_EQ(curve.m_BaseXP, 1.0f) << "BaseXP must clamp up to 1";
    EXPECT_FLOAT_EQ(curve.m_Exponent, 0.0f) << "Exponent must clamp up to 0";
    ASSERT_EQ(curve.m_Table.size(), 3u);
    EXPECT_EQ(curve.m_Table[0], 1) << "zero table entry must clamp up to 1";
    EXPECT_EQ(curve.m_Table[1], 1) << "negative table entry must clamp up to 1";
    EXPECT_EQ(curve.m_Table[2], 50) << "valid table entry must pass through";

    curve.m_MaxLevel = 2000;
    curve.m_BaseXP = 2e7f;
    curve.m_Exponent = 20.0f;
    curve.Sanitize();
    EXPECT_EQ(curve.m_MaxLevel, 1000) << "MaxLevel must clamp down to 1000";
    EXPECT_FLOAT_EQ(curve.m_BaseXP, 1e7f) << "BaseXP must clamp down to 1e7";
    EXPECT_FLOAT_EQ(curve.m_Exponent, 10.0f) << "Exponent must clamp down to 10";

    curve.m_BaseXP = std::numeric_limits<f32>::quiet_NaN();
    curve.m_Exponent = std::numeric_limits<f32>::quiet_NaN();
    curve.Sanitize();
    EXPECT_FLOAT_EQ(curve.m_BaseXP, 100.0f) << "NaN BaseXP must reset to the 100 default";
    EXPECT_FLOAT_EQ(curve.m_Exponent, 1.5f) << "NaN Exponent must reset to the 1.5 default";
}

// ============================================================================
// Serializer string round-trip
// ============================================================================

TEST(ExperienceCurveTest, SerializerRoundTripPreservesEveryField)
{
    auto original = Ref<ExperienceCurve>::Create();
    original->m_Mode = ExperienceCurve::CurveMode::Table;
    original->m_MaxLevel = 42;
    original->m_BaseXP = 123.5f;
    original->m_Exponent = 2.25f;
    original->m_Table = { 150, 250, 350 };

    ExperienceCurveSerializer serializer;
    const std::string yaml = serializer.TestSerializeToYAML(original);
    ASSERT_FALSE(yaml.empty()) << "TestSerializeToYAML produced an empty string";

    auto reloaded = Ref<ExperienceCurve>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, reloaded))
        << "deserialize rejected the just-serialized curve. YAML:\n"
        << yaml;

    EXPECT_EQ(reloaded->m_Mode, ExperienceCurve::CurveMode::Table) << "Mode dropped in round-trip";
    EXPECT_EQ(reloaded->m_MaxLevel, 42) << "MaxLevel dropped in round-trip";
    EXPECT_NEAR(reloaded->m_BaseXP, 123.5f, 1e-4f) << "BaseXP dropped in round-trip";
    EXPECT_NEAR(reloaded->m_Exponent, 2.25f, 1e-4f) << "Exponent dropped in round-trip";
    ASSERT_EQ(reloaded->m_Table.size(), 3u) << "Table entry count dropped in round-trip";
    EXPECT_EQ(reloaded->m_Table[0], 150);
    EXPECT_EQ(reloaded->m_Table[1], 250);
    EXPECT_EQ(reloaded->m_Table[2], 350);
}

TEST(ExperienceCurveTest, SerializerFallsBackSanelyOnNaNInfInjection)
{
    // Hand-edited content asset: yaml-cpp parses .nan / .inf cleanly, so the
    // serializer's SanitizeFinite + Sanitize() are the only line of defence.
    const std::string yaml =
        "ExperienceCurve:\n"
        "  Mode: Formula\n"
        "  MaxLevel: 5000\n"
        "  BaseXP: .nan\n"
        "  Exponent: .inf\n";

    ExperienceCurveSerializer serializer;
    auto curve = Ref<ExperienceCurve>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, curve))
        << "NaN/Inf-injected but structurally valid YAML must load (with fallbacks), not reject";

    EXPECT_FLOAT_EQ(curve->m_BaseXP, 100.0f) << "NaN BaseXP must fall back to the 100 default";
    EXPECT_FLOAT_EQ(curve->m_Exponent, 1.5f) << ".inf Exponent must fall back to the 1.5 default";
    EXPECT_EQ(curve->m_MaxLevel, 1000) << "out-of-range MaxLevel must clamp to the documented 1000 max";
    EXPECT_GE(curve->GetXPForLevelUp(1), 1) << "the loaded curve must still satisfy the >= 1 floor";
}

TEST(ExperienceCurveTest, SerializerTableModeWithEmptyTableLoadsAsFormula)
{
    // Table mode with no (or an empty) Table sequence would make every level
    // silently fall through to the formula anyway — the serializer makes that
    // explicit by demoting the mode on load.
    const std::string yamlNoTable =
        "ExperienceCurve:\n"
        "  Mode: Table\n"
        "  MaxLevel: 10\n"
        "  BaseXP: 100\n"
        "  Exponent: 1.5\n";

    ExperienceCurveSerializer serializer;
    auto curve = Ref<ExperienceCurve>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yamlNoTable, curve))
        << "Table mode without a Table key must load (as Formula), not reject";
    EXPECT_EQ(curve->m_Mode, ExperienceCurve::CurveMode::Formula)
        << "Table mode with an empty table must demote to Formula on load";

    const std::string yamlEmptyTable =
        "ExperienceCurve:\n"
        "  Mode: Table\n"
        "  Table: []\n";
    auto curve2 = Ref<ExperienceCurve>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yamlEmptyTable, curve2));
    EXPECT_EQ(curve2->m_Mode, ExperienceCurve::CurveMode::Formula)
        << "Table mode with 'Table: []' must demote to Formula on load";
}

TEST(ExperienceCurveTest, SerializerSanitizesGarbageTableEntriesOnLoad)
{
    const std::string yaml =
        "ExperienceCurve:\n"
        "  Mode: Table\n"
        "  Table: [0, -7, 50]\n";

    ExperienceCurveSerializer serializer;
    auto curve = Ref<ExperienceCurve>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, curve));
    ASSERT_EQ(curve->m_Table.size(), 3u);
    EXPECT_EQ(curve->m_Table[0], 1) << "zero entry must be floored to 1 by the load-time Sanitize()";
    EXPECT_EQ(curve->m_Table[1], 1) << "negative entry must be floored to 1 by the load-time Sanitize()";
    EXPECT_EQ(curve->m_Table[2], 50);
}

TEST(ExperienceCurveTest, SerializerRejectsMalformedYAML)
{
    ExperienceCurveSerializer serializer;

    auto curve = Ref<ExperienceCurve>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML("key: [unclosed sequence", curve))
        << "a YAML syntax error must be rejected";

    auto curve2 = Ref<ExperienceCurve>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML("SomethingElse: 1\n", curve2))
        << "well-formed YAML without the 'ExperienceCurve' root key must be rejected";
}
