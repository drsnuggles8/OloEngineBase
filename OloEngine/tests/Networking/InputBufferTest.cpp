#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Prediction/NetworkInputBuffer.h"

TEST(InputBufferTest, PushAndRetrieveByTick)
{
	using namespace OloEngine;
	NetworkInputBuffer buffer;

	std::vector<u8> data = { 1, 2, 3 };
	buffer.Push(10, 42, data);

	const auto* cmd = buffer.GetByTick(10);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd->Tick, 10u);
	EXPECT_EQ(cmd->EntityUUID, 42u);
	EXPECT_EQ(cmd->Data, data);
}

TEST(InputBufferTest, GetByTickReturnsNullForMissing)
{
	using namespace OloEngine;
	NetworkInputBuffer buffer;

	EXPECT_EQ(buffer.GetByTick(99), nullptr);
}

TEST(InputBufferTest, GetUnconfirmedInputsReturnsAfterTick)
{
	using namespace OloEngine;
	NetworkInputBuffer buffer;

	buffer.Push(1, 100, { 0x01 });
	buffer.Push(2, 100, { 0x02 });
	buffer.Push(3, 100, { 0x03 });
	buffer.Push(4, 100, { 0x04 });

	auto unconfirmed = buffer.GetUnconfirmedInputs(2);
	ASSERT_EQ(unconfirmed.size(), 2u);
	EXPECT_EQ(unconfirmed[0]->Tick, 3u);
	EXPECT_EQ(unconfirmed[1]->Tick, 4u);
}

TEST(InputBufferTest, DiscardUpToRemovesOldEntries)
{
	using namespace OloEngine;
	NetworkInputBuffer buffer;

	buffer.Push(1, 100, { 0x01 });
	buffer.Push(2, 100, { 0x02 });
	buffer.Push(3, 100, { 0x03 });

	buffer.DiscardUpTo(2);

	EXPECT_EQ(buffer.GetByTick(1), nullptr);
	EXPECT_EQ(buffer.GetByTick(2), nullptr);
	ASSERT_NE(buffer.GetByTick(3), nullptr);
	EXPECT_EQ(buffer.GetByTick(3)->Tick, 3u);
}

TEST(InputBufferTest, PushOverwritesOnCapacityOverflow)
{
	using namespace OloEngine;
	NetworkInputBuffer buffer(4); // Capacity of 4

	buffer.Push(1, 100, { 0x01 });
	buffer.Push(2, 100, { 0x02 });
	buffer.Push(3, 100, { 0x03 });
	buffer.Push(4, 100, { 0x04 });
	buffer.Push(5, 100, { 0x05 }); // Overwrites tick 1

	EXPECT_EQ(buffer.GetByTick(1), nullptr);
	ASSERT_NE(buffer.GetByTick(5), nullptr);
	EXPECT_EQ(buffer.GetByTick(5)->Data, std::vector<u8>{ 0x05 });
}

TEST(InputBufferTest, ClearRemovesAll)
{
	using namespace OloEngine;
	NetworkInputBuffer buffer;

	buffer.Push(1, 100, { 0x01 });
	buffer.Push(2, 100, { 0x02 });

	buffer.Clear();

	EXPECT_EQ(buffer.GetByTick(1), nullptr);
	EXPECT_EQ(buffer.GetByTick(2), nullptr);
	EXPECT_TRUE(buffer.GetUnconfirmedInputs(0).empty());
}

TEST(InputBufferTest, GetUnconfirmedInputsOrderedByTick)
{
	using namespace OloEngine;
	NetworkInputBuffer buffer;

	// Push out of order (shouldn't happen in practice, but buffer must handle gracefully)
	buffer.Push(5, 100, { 0x05 });
	buffer.Push(3, 100, { 0x03 });
	buffer.Push(7, 100, { 0x07 });

	auto unconfirmed = buffer.GetUnconfirmedInputs(0);
	ASSERT_EQ(unconfirmed.size(), 3u);
	// Should be sorted by tick
	EXPECT_EQ(unconfirmed[0]->Tick, 3u);
	EXPECT_EQ(unconfirmed[1]->Tick, 5u);
	EXPECT_EQ(unconfirmed[2]->Tick, 7u);
}
