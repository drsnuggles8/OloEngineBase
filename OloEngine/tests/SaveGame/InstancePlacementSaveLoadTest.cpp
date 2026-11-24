// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Serialization/Archive.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    TEST(InstancePlacementSaveLoad, RetainsTintPickingAndAnimationPhase)
    {
        InstancedMeshComponent source, restored;
        InstanceData member;
        member.StableID = 48;
        member.EntityID = 17;
        member.Custom = 0.375f;
        member.Color = { 0.25f, 0.5f, 0.75f, 1.0f };
        source.Instances.Add(member);
        std::vector<u8> bytes;
        FMemoryWriter writer(bytes);
        writer.SetArchiveVersion(kSaveGameFormatVersion);
        SaveGameComponentSerializer::Serialize(writer, source);
        ASSERT_FALSE(writer.IsError());
        FMemoryReader reader(bytes);
        reader.SetArchiveVersion(kSaveGameFormatVersion);
        SaveGameComponentSerializer::Serialize(reader, restored);
        ASSERT_FALSE(reader.IsError());
        ASSERT_EQ(restored.Instances.Num(), 1);
        const auto& result = restored.Instances[0];
        EXPECT_EQ(result.StableID, member.StableID);
        EXPECT_EQ(result.EntityID, member.EntityID);
        EXPECT_FLOAT_EQ(result.Custom, member.Custom);
        for (int channel = 0; channel < 4; ++channel)
            EXPECT_FLOAT_EQ(result.Color[channel], member.Color[channel]);
    }
} // namespace OloEngine::Tests
