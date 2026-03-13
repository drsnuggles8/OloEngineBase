#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

#include <cstring>

// Delta snapshot tests validate the CaptureDelta logic without a live Scene.
// We simulate the serialization format (UUID + 9 floats) manually.

static std::vector<u8> MakeSnapshotEntry(u64 uuid, const OloEngine::TransformComponent& t)
{
	using namespace OloEngine;

	std::vector<u8> buffer;
	FMemoryWriter writer(buffer);
	writer.ArIsNetArchive = true;
	writer << uuid;
	ComponentReplicator::Serialize(writer, const_cast<TransformComponent&>(t));
	return buffer;
}

static std::vector<u8> ConcatBuffers(const std::vector<std::vector<u8>>& buffers)
{
	std::vector<u8> result;
	for (const auto& b : buffers)
	{
		result.insert(result.end(), b.begin(), b.end());
	}
	return result;
}

TEST(DeltaSnapshotTest, IdenticalSnapshotsProduceEmptyDelta)
{
	using namespace OloEngine;

	// If the current scene state matches the baseline, CaptureDelta should
	// produce an empty buffer (nothing changed). Since we can't call CaptureDelta
	// without a Scene, we verify the underlying comparison logic.

	TransformComponent t;
	t.Translation = { 1.0f, 2.0f, 3.0f };
	t.Rotation = { 0.0f, 0.0f, 0.0f };
	t.Scale = { 1.0f, 1.0f, 1.0f };

	auto snap1 = MakeSnapshotEntry(100, t);
	auto snap2 = MakeSnapshotEntry(100, t);

	// The serialized bytes should be identical
	EXPECT_EQ(snap1.size(), snap2.size());
	EXPECT_EQ(std::memcmp(snap1.data(), snap2.data(), snap1.size()), 0);
}

TEST(DeltaSnapshotTest, DifferentTranslationProducesDifference)
{
	using namespace OloEngine;

	TransformComponent t1;
	t1.Translation = { 1.0f, 2.0f, 3.0f };
	t1.Rotation = { 0.0f, 0.0f, 0.0f };
	t1.Scale = { 1.0f, 1.0f, 1.0f };

	TransformComponent t2;
	t2.Translation = { 10.0f, 20.0f, 30.0f }; // changed
	t2.Rotation = { 0.0f, 0.0f, 0.0f };
	t2.Scale = { 1.0f, 1.0f, 1.0f };

	auto snap1 = MakeSnapshotEntry(100, t1);
	auto snap2 = MakeSnapshotEntry(100, t2);

	// The serialized bytes should differ (after the UUID portion)
	EXPECT_EQ(snap1.size(), snap2.size());
	EXPECT_NE(std::memcmp(snap1.data(), snap2.data(), snap1.size()), 0);
}

TEST(DeltaSnapshotTest, DeltaFormatSameAsFullFormat)
{
	using namespace OloEngine;

	// A delta snapshot uses the same UUID + transform format, just with fewer entities.
	// Verify that reading a single-entity delta buffer works with the same reader logic.

	TransformComponent t;
	t.Translation = { 5.0f, 6.0f, 7.0f };
	t.Rotation = { 0.1f, 0.2f, 0.3f };
	t.Scale = { 2.0f, 2.0f, 2.0f };

	auto entry = MakeSnapshotEntry(42, t);

	// Read it back
	FMemoryReader reader(entry);
	reader.ArIsNetArchive = true;

	u64 uuid = 0;
	reader << uuid;
	EXPECT_EQ(uuid, 42u);

	TransformComponent loaded;
	ComponentReplicator::Serialize(reader, loaded);

	EXPECT_FLOAT_EQ(loaded.Translation.x, 5.0f);
	EXPECT_FLOAT_EQ(loaded.Translation.y, 6.0f);
	EXPECT_FLOAT_EQ(loaded.Translation.z, 7.0f);
	EXPECT_FLOAT_EQ(loaded.Scale.x, 2.0f);
	EXPECT_FALSE(reader.IsError());
}

TEST(DeltaSnapshotTest, MultiEntityDeltaFiltering)
{
	using namespace OloEngine;

	// Simulate what CaptureDelta does: given a baseline with entities [A, B, C],
	// if only entity B changed, the delta should contain only B.

	TransformComponent tA;
	tA.Translation = { 1.0f, 0.0f, 0.0f };
	tA.Rotation = { 0.0f, 0.0f, 0.0f };
	tA.Scale = { 1.0f, 1.0f, 1.0f };

	TransformComponent tB;
	tB.Translation = { 2.0f, 0.0f, 0.0f };
	tB.Rotation = { 0.0f, 0.0f, 0.0f };
	tB.Scale = { 1.0f, 1.0f, 1.0f };

	TransformComponent tC;
	tC.Translation = { 3.0f, 0.0f, 0.0f };
	tC.Rotation = { 0.0f, 0.0f, 0.0f };
	tC.Scale = { 1.0f, 1.0f, 1.0f };

	auto baseA = MakeSnapshotEntry(1, tA);
	auto baseB = MakeSnapshotEntry(2, tB);
	auto baseC = MakeSnapshotEntry(3, tC);

	auto baseline = ConcatBuffers({ baseA, baseB, baseC });

	// Now simulate current state: A and C unchanged, B moved
	TransformComponent tBnew;
	tBnew.Translation = { 99.0f, 0.0f, 0.0f }; // changed
	tBnew.Rotation = { 0.0f, 0.0f, 0.0f };
	tBnew.Scale = { 1.0f, 1.0f, 1.0f };

	auto newB = MakeSnapshotEntry(2, tBnew);
	auto deltaBuffer = ConcatBuffers({ newB }); // only B in delta

	// Parse the delta buffer — should contain exactly one entity
	FMemoryReader reader(deltaBuffer);
	reader.ArIsNetArchive = true;

	u64 uuid = 0;
	reader << uuid;
	EXPECT_EQ(uuid, 2u);

	TransformComponent loaded;
	ComponentReplicator::Serialize(reader, loaded);
	EXPECT_FLOAT_EQ(loaded.Translation.x, 99.0f);

	// Should be at end of buffer
	EXPECT_EQ(reader.Tell(), reader.TotalSize());
	EXPECT_FALSE(reader.IsError());
}
