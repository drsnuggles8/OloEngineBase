#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Replication/SnapshotBuffer.h"

TEST(SnapshotBufferTest, DefaultConstructionIsEmpty)
{
    using namespace OloEngine;

    SnapshotBuffer buffer;
    EXPECT_TRUE(buffer.IsEmpty());
    EXPECT_EQ(buffer.Size(), 0u);
    EXPECT_EQ(buffer.Capacity(), SnapshotBuffer::kDefaultCapacity);
    EXPECT_EQ(buffer.GetLatest(), nullptr);
}

TEST(SnapshotBufferTest, PushAndRetrieveLatest)
{
    using namespace OloEngine;

    SnapshotBuffer buffer(4);

    std::vector<u8> data1 = { 1, 2, 3 };
    buffer.Push(1, data1);
    EXPECT_EQ(buffer.Size(), 1u);
    EXPECT_FALSE(buffer.IsEmpty());

    const auto* latest = buffer.GetLatest();
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->Tick, 1u);
    EXPECT_EQ(latest->Data, data1);
}

TEST(SnapshotBufferTest, PushOverwritesOldest)
{
    using namespace OloEngine;

    SnapshotBuffer buffer(3);

    buffer.Push(1, { 0x01 });
    buffer.Push(2, { 0x02 });
    buffer.Push(3, { 0x03 });
    EXPECT_EQ(buffer.Size(), 3u);

    // This should overwrite tick 1
    buffer.Push(4, { 0x04 });
    EXPECT_EQ(buffer.Size(), 3u); // capped at capacity

    // Tick 1 should be gone
    EXPECT_EQ(buffer.GetByTick(1), nullptr);

    // Ticks 2, 3, 4 should exist
    EXPECT_NE(buffer.GetByTick(2), nullptr);
    EXPECT_NE(buffer.GetByTick(3), nullptr);
    EXPECT_NE(buffer.GetByTick(4), nullptr);

    const auto* latest = buffer.GetLatest();
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->Tick, 4u);
}

TEST(SnapshotBufferTest, GetByTickFindsCorrectEntry)
{
    using namespace OloEngine;

    SnapshotBuffer buffer(8);
    buffer.Push(10, { 0xAA });
    buffer.Push(20, { 0xBB });
    buffer.Push(30, { 0xCC });

    const auto* entry = buffer.GetByTick(20);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->Tick, 20u);
    EXPECT_EQ(entry->Data.size(), 1u);
    EXPECT_EQ(entry->Data[0], 0xBB);

    EXPECT_EQ(buffer.GetByTick(99), nullptr);
}

TEST(SnapshotBufferTest, GetBracketingEntriesNeedsTwoEntries)
{
    using namespace OloEngine;

    SnapshotBuffer buffer(4);
    EXPECT_FALSE(buffer.GetBracketingEntries(5).has_value());

    buffer.Push(10, { 0x01 });
    EXPECT_FALSE(buffer.GetBracketingEntries(10).has_value()); // only 1 entry
}

TEST(SnapshotBufferTest, GetBracketingEntriesInterpolation)
{
    using namespace OloEngine;

    SnapshotBuffer buffer(8);
    buffer.Push(10, { 0x0A });
    buffer.Push(20, { 0x14 });
    buffer.Push(30, { 0x1E });

    // Tick 15 should bracket between tick 10 and tick 20
    auto bracket = buffer.GetBracketingEntries(15);
    ASSERT_TRUE(bracket.has_value());
    EXPECT_EQ(bracket->Before->Tick, 10u);
    EXPECT_EQ(bracket->After->Tick, 20u);

    // Tick 20 should bracket with Before=20, After=30
    bracket = buffer.GetBracketingEntries(20);
    ASSERT_TRUE(bracket.has_value());
    EXPECT_LE(bracket->Before->Tick, 20u);
    EXPECT_GE(bracket->After->Tick, 20u);
}

TEST(SnapshotBufferTest, ClearResetsState)
{
    using namespace OloEngine;

    SnapshotBuffer buffer(4);
    buffer.Push(1, { 0x01 });
    buffer.Push(2, { 0x02 });
    EXPECT_EQ(buffer.Size(), 2u);

    buffer.Clear();
    EXPECT_TRUE(buffer.IsEmpty());
    EXPECT_EQ(buffer.Size(), 0u);
    EXPECT_EQ(buffer.GetLatest(), nullptr);
}

TEST(SnapshotBufferTest, CustomCapacity)
{
    using namespace OloEngine;

    SnapshotBuffer buffer(128);
    EXPECT_EQ(buffer.Capacity(), 128u);

    SnapshotBuffer small(2);
    EXPECT_EQ(small.Capacity(), 2u);

    small.Push(1, { 0x01 });
    small.Push(2, { 0x02 });
    small.Push(3, { 0x03 }); // overwrites tick 1
    EXPECT_EQ(small.Size(), 2u);
    EXPECT_EQ(small.GetByTick(1), nullptr);
    EXPECT_NE(small.GetByTick(3), nullptr);
}
