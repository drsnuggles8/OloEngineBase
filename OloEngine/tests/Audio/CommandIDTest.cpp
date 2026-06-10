#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Audio/AudioEvents/CommandID.h"

using namespace OloEngine::Audio;

TEST(CommandIDTest, FromStringDeterministic)
{
    auto id1 = CommandID::FromString("Play_Gunshot");
    auto id2 = CommandID::FromString("Play_Gunshot");
    EXPECT_EQ(id1.ID, id2.ID);
}

TEST(CommandIDTest, EmptyStringInvalid)
{
    auto id = CommandID::FromString("");
    EXPECT_FALSE(id.IsValid());
    EXPECT_EQ(id.ID, 0u);
}

TEST(CommandIDTest, CaseInsensitive)
{
    auto lower = CommandID::FromString("play_gunshot");
    auto upper = CommandID::FromString("PLAY_GUNSHOT");
    auto mixed = CommandID::FromString("Play_Gunshot");
    EXPECT_EQ(lower.ID, upper.ID);
    EXPECT_EQ(lower.ID, mixed.ID);
}

TEST(CommandIDTest, DifferentStringsDifferentIDs)
{
    auto id1 = CommandID::FromString("Play_Gunshot");
    auto id2 = CommandID::FromString("Play_Explosion");
    EXPECT_NE(id1.ID, id2.ID);
}

TEST(CommandIDTest, ValidIDIsValid)
{
    auto id = CommandID::FromString("SomeEvent");
    EXPECT_TRUE(id.IsValid());
    EXPECT_NE(id.ID, 0u);
}

TEST(CommandIDTest, DefaultConstructedInvalid)
{
    CommandID id;
    EXPECT_FALSE(id.IsValid());
    EXPECT_EQ(id.ID, 0u);
}

TEST(CommandIDTest, Comparison)
{
    auto id1 = CommandID::FromString("Alpha");
    auto id2 = CommandID::FromString("Beta");
    auto id3 = CommandID::FromString("Alpha");

    EXPECT_EQ(id1, id3);
    EXPECT_NE(id1, id2);
}

TEST(CommandIDTest, ExplicitConstruction)
{
    CommandID id(42u);
    EXPECT_EQ(id.ID, 42u);
    EXPECT_TRUE(id.IsValid());
}
