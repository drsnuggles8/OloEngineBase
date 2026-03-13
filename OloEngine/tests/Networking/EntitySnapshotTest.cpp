#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

// EntitySnapshot::Capture/Apply require a full Scene context which relies on
// OpenGL and other engine subsystems. These tests validate the underlying
// serialization round-trip that EntitySnapshot builds on, without needing
// a live Scene.

TEST(EntitySnapshotTest, ManualCaptureApplyRoundtrip)
{
    using namespace OloEngine;

    // Simulate what Capture does: write UUID + transform
    TransformComponent original;
    original.Translation = { 10.0f, 20.0f, 30.0f };
    original.Rotation = { 0.0f, 0.0f, 0.0f };
    original.Scale = { 1.0f, 1.0f, 1.0f };

    u64 entityUUID = 12345;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;

    writer << entityUUID;
    ComponentReplicator::Serialize(writer, original);

    EXPECT_GT(buffer.size(), sizeof(u64));

    // Simulate what Apply does: read UUID + transform
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;

    u64 readUUID = 0;
    reader << readUUID;
    EXPECT_EQ(readUUID, entityUUID);

    TransformComponent loaded;
    ComponentReplicator::Serialize(reader, loaded);

    EXPECT_FLOAT_EQ(loaded.Translation.x, 10.0f);
    EXPECT_FLOAT_EQ(loaded.Translation.y, 20.0f);
    EXPECT_FLOAT_EQ(loaded.Translation.z, 30.0f);
    EXPECT_FALSE(reader.IsError());
}

TEST(EntitySnapshotTest, EmptyBufferProducesNoWork)
{
    using namespace OloEngine;

    std::vector<u8> empty;
    // Verify empty data can be given to a reader without crashing
    FMemoryReader reader(empty);
    reader.ArIsNetArchive = true;

    EXPECT_EQ(reader.TotalSize(), 0);
    EXPECT_EQ(reader.Tell(), 0);
}

TEST(EntitySnapshotTest, MultipleEntitiesRoundtrip)
{
    using namespace OloEngine;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;

    // Write two entities
    u64 uuid1 = 111;
    TransformComponent t1;
    t1.Translation = { 1.0f, 2.0f, 3.0f };
    t1.Rotation = { 0.0f, 0.0f, 0.0f };
    t1.Scale = { 1.0f, 1.0f, 1.0f };

    u64 uuid2 = 222;
    TransformComponent t2;
    t2.Translation = { 4.0f, 5.0f, 6.0f };
    t2.Rotation = { 0.0f, 0.0f, 0.0f };
    t2.Scale = { 2.0f, 2.0f, 2.0f };

    writer << uuid1;
    ComponentReplicator::Serialize(writer, t1);
    writer << uuid2;
    ComponentReplicator::Serialize(writer, t2);

    // Read back
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;

    u64 readUUID1 = 0;
    reader << readUUID1;
    EXPECT_EQ(readUUID1, uuid1);

    TransformComponent loaded1;
    ComponentReplicator::Serialize(reader, loaded1);
    EXPECT_FLOAT_EQ(loaded1.Translation.x, 1.0f);

    u64 readUUID2 = 0;
    reader << readUUID2;
    EXPECT_EQ(readUUID2, uuid2);

    TransformComponent loaded2;
    ComponentReplicator::Serialize(reader, loaded2);
    EXPECT_FLOAT_EQ(loaded2.Translation.x, 4.0f);
    EXPECT_FLOAT_EQ(loaded2.Scale.x, 2.0f);

    EXPECT_FALSE(reader.IsError());
}
