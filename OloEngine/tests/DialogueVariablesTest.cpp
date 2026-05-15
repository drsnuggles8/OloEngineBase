#include <gtest/gtest.h>
#include "OloEngine/Dialogue/DialogueVariables.h"

// =============================================================================
// DialogueVariablesTest -- contracts of the typed key-value store used
// by dialogue conditions/actions.
//
// Prior version had 8 TEST_F cases, four of which were per-type permutations
// of the same Set/Get round-trip pattern. Per docs/testing.md
// section 4.7 (type-permutation padding), one TEST that walks all four
// types in a row covers the same contract. Total: 3 TESTs covering
// (1) round-trip on every type, (2) missing-key default behaviour,
// (3) lifecycle (Has + Clear + overwrite).
// =============================================================================

using namespace OloEngine;

TEST(DialogueVariables, RoundTripAcrossAllSupportedTypes)
{
    DialogueVariables vars;

    vars.SetBool("hasKey", true);
    EXPECT_TRUE(vars.GetBool("hasKey"));
    vars.SetBool("hasKey", false);
    EXPECT_FALSE(vars.GetBool("hasKey"));

    vars.SetInt("gold", 100);
    EXPECT_EQ(vars.GetInt("gold"), 100);

    vars.SetFloat("speed", 1.5f);
    EXPECT_FLOAT_EQ(vars.GetFloat("speed"), 1.5f);

    vars.SetString("playerName", "Hero");
    EXPECT_EQ(vars.GetString("playerName"), "Hero");
}

TEST(DialogueVariables, MissingKeysReturnTheCallerProvidedDefault)
{
    DialogueVariables vars;

    EXPECT_FALSE(vars.GetBool("nope"));
    EXPECT_TRUE(vars.GetBool("nope", true));

    EXPECT_EQ(vars.GetInt("nope"), 0);
    EXPECT_EQ(vars.GetInt("nope", 42), 42);

    EXPECT_FLOAT_EQ(vars.GetFloat("nope"), 0.0f);
    EXPECT_FLOAT_EQ(vars.GetFloat("nope", 2.5f), 2.5f);

    EXPECT_EQ(vars.GetString("nope"), "");
    EXPECT_EQ(vars.GetString("nope", "fallback"), "fallback");
}

TEST(DialogueVariables, HasReportsPresenceAndClearWipesEverything)
{
    DialogueVariables vars;

    EXPECT_FALSE(vars.Has("a"));
    vars.SetBool("a", true);
    vars.SetInt("b", 10);
    vars.SetString("c", "hello");
    EXPECT_TRUE(vars.Has("a"));
    EXPECT_TRUE(vars.Has("b"));
    EXPECT_TRUE(vars.Has("c"));

    // Overwrite preserves Has + last write wins.
    vars.SetInt("b", 11);
    vars.SetInt("b", 12);
    EXPECT_EQ(vars.GetInt("b"), 12);

    vars.Clear();
    EXPECT_FALSE(vars.Has("a"));
    EXPECT_FALSE(vars.Has("b"));
    EXPECT_FALSE(vars.Has("c"));
}
