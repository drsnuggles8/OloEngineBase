#include <gtest/gtest.h>
#include "OloEngine/Dialogue/DialogueVariables.h"

using namespace OloEngine;

class DialogueVariablesTest : public ::testing::Test
{
  protected:
    DialogueVariables vars;
};

TEST_F(DialogueVariablesTest, SetAndGetBool)
{
    vars.SetBool("hasKey", true);
    EXPECT_TRUE(vars.GetBool("hasKey"));

    vars.SetBool("hasKey", false);
    EXPECT_FALSE(vars.GetBool("hasKey"));
}

TEST_F(DialogueVariablesTest, SetAndGetInt)
{
    vars.SetInt("gold", 100);
    EXPECT_EQ(vars.GetInt("gold"), 100);

    vars.SetInt("gold", 250);
    EXPECT_EQ(vars.GetInt("gold"), 250);
}

TEST_F(DialogueVariablesTest, SetAndGetString)
{
    vars.SetString("playerName", "Hero");
    EXPECT_EQ(vars.GetString("playerName"), "Hero");

    vars.SetString("playerName", "Villain");
    EXPECT_EQ(vars.GetString("playerName"), "Villain");
}

TEST_F(DialogueVariablesTest, SetAndGetFloat)
{
    vars.SetFloat("speed", 1.5f);
    EXPECT_FLOAT_EQ(vars.GetFloat("speed"), 1.5f);

    vars.SetFloat("speed", 3.14f);
    EXPECT_FLOAT_EQ(vars.GetFloat("speed"), 3.14f);
}

TEST_F(DialogueVariablesTest, GetMissingKeyReturnsDefault)
{
    EXPECT_FALSE(vars.GetBool("nonexistent"));
    EXPECT_TRUE(vars.GetBool("nonexistent", true));

    EXPECT_EQ(vars.GetInt("nonexistent"), 0);
    EXPECT_EQ(vars.GetInt("nonexistent", 42), 42);

    EXPECT_FLOAT_EQ(vars.GetFloat("nonexistent"), 0.0f);
    EXPECT_FLOAT_EQ(vars.GetFloat("nonexistent", 2.5f), 2.5f);

    EXPECT_EQ(vars.GetString("nonexistent"), "");
    EXPECT_EQ(vars.GetString("nonexistent", "fallback"), "fallback");
}

TEST_F(DialogueVariablesTest, HasKey)
{
    EXPECT_FALSE(vars.Has("testKey"));

    vars.SetBool("testKey", true);
    EXPECT_TRUE(vars.Has("testKey"));

    vars.SetInt("intKey", 5);
    EXPECT_TRUE(vars.Has("intKey"));
}

TEST_F(DialogueVariablesTest, Clear)
{
    vars.SetBool("a", true);
    vars.SetInt("b", 10);
    vars.SetString("c", "hello");

    vars.Clear();

    EXPECT_FALSE(vars.Has("a"));
    EXPECT_FALSE(vars.Has("b"));
    EXPECT_FALSE(vars.Has("c"));
}

TEST_F(DialogueVariablesTest, OverwriteValue)
{
    vars.SetBool("flag", true);
    EXPECT_TRUE(vars.GetBool("flag"));

    vars.SetBool("flag", false);
    EXPECT_FALSE(vars.GetBool("flag"));

    vars.SetInt("counter", 1);
    vars.SetInt("counter", 2);
    vars.SetInt("counter", 3);
    EXPECT_EQ(vars.GetInt("counter"), 3);
}
