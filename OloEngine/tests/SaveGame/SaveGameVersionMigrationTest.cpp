// OLO_TEST_LAYER: unit
// =============================================================================
// SaveGameVersionMigrationTest.cpp
//
// Locks in the save-game FormatVersion forward-compat behaviour added for
// issue #454: SaveGameHeader::IsValid() used to exact-match FormatVersion,
// rejecting every save produced by an older build outright. It now accepts
// any version in [kMinSupportedSaveGameFormatVersion, kSaveGameFormatVersion],
// and SaveGameSerializer::RestoreSceneState threads that recorded version
// into FArchive::ArArchiveVersion so per-component Serialize() overloads can
// gate fields added after v1 behind `HasFieldsSince(ar, introducedInVersion)`
// (see TerrainComponent / IKTargetComponent in SaveGameComponentSerializer.cpp).
//
// What this test does
// --------------------
//   1. Directly exercises SaveGameComponentSerializer::Serialize() for
//      TerrainComponent and IKTargetComponent with a hand-written pre-gate
//      byte layout (exactly the fields each component's Serialize() reads
//      unconditionally, before its version-gated tail) and asserts the
//      fields introduced later come back at their constructor defaults with
//      no read desync (reader exactly consumes the buffer, no error).
//   2. An end-to-end regression: captures a real scene via
//      SaveGameSerializer::CaptureSceneState, surgically replaces the
//      TerrainComponent's component block with a pre-v3 payload (same
//      technique as (1)), and restores it through
//      SaveGameSerializer::RestoreSceneState with formatVersion = 2 --
//      proving the version threading through DeserializeEntitiesInto's
//      per-component reader actually works end-to-end, not just at the
//      single-component level.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        Entity FindByTag(Scene& scene, const char* tag)
        {
            for (auto e : scene.GetAllEntitiesWith<TagComponent>())
            {
                Entity ent{ e, &scene };
                if (ent.GetComponent<TagComponent>().Tag == tag)
                    return ent;
            }
            return {};
        }

        // Writes exactly the fields TerrainComponent::Serialize() reads
        // unconditionally before its "Format v3" gate -- i.e. what a save
        // produced by a pre-v3 build actually put on disk for this component.
        std::vector<u8> BuildPreV3TerrainPayload(const TerrainComponent& c)
        {
            std::vector<u8> buf;
            FMemoryWriter w(buf);
            w.ArIsSaveGame = true;

            std::string heightmapPath = c.m_HeightmapPath;
            f32 worldSizeX = c.m_WorldSizeX, worldSizeZ = c.m_WorldSizeZ, heightScale = c.m_HeightScale;
            bool proceduralEnabled = c.m_ProceduralEnabled;
            i32 proceduralSeed = c.m_ProceduralSeed;
            u32 proceduralResolution = c.m_ProceduralResolution, proceduralOctaves = c.m_ProceduralOctaves;
            f32 proceduralFrequency = c.m_ProceduralFrequency, proceduralLacunarity = c.m_ProceduralLacunarity, proceduralPersistence = c.m_ProceduralPersistence;
            bool tessellationEnabled = c.m_TessellationEnabled;
            f32 targetTriangleSize = c.m_TargetTriangleSize, morphRegion = c.m_MorphRegion;
            bool streamingEnabled = c.m_StreamingEnabled;
            std::string tileDirectory = c.m_TileDirectory, tileFilePattern = c.m_TileFilePattern;
            f32 tileWorldSize = c.m_TileWorldSize;
            u32 tileResolution = c.m_TileResolution, streamingLoadRadius = c.m_StreamingLoadRadius, streamingMaxTiles = c.m_StreamingMaxTiles;
            bool voxelEnabled = c.m_VoxelEnabled;
            f32 voxelSize = c.m_VoxelSize;

            w << heightmapPath;
            w << worldSizeX << worldSizeZ << heightScale;
            w << proceduralEnabled << proceduralSeed;
            w << proceduralResolution << proceduralOctaves;
            w << proceduralFrequency << proceduralLacunarity << proceduralPersistence;
            w << tessellationEnabled << targetTriangleSize << morphRegion;
            w << streamingEnabled << tileDirectory << tileFilePattern;
            w << tileWorldSize << tileResolution;
            w << streamingLoadRadius << streamingMaxTiles;
            w << voxelEnabled << voxelSize;
            return buf;
        }

        // Writes exactly the fields IKTargetComponent::Serialize() reads
        // unconditionally before its "Format v4" (Chain IK) gate.
        std::vector<u8> BuildPreV4IKTargetPayload(const IKTargetComponent& c)
        {
            std::vector<u8> buf;
            FMemoryWriter w(buf);
            w.ArIsSaveGame = true;

            bool aimIKEnabled = c.AimIKEnabled;
            u32 aimBoneIndex = c.AimBoneIndex;
            f32 aimTargetX = c.AimTarget.x, aimTargetY = c.AimTarget.y, aimTargetZ = c.AimTarget.z;
            f32 aimAxisX = c.AimAxis.x, aimAxisY = c.AimAxis.y, aimAxisZ = c.AimAxis.z;
            f32 aimOffsetX = c.AimOffset.x, aimOffsetY = c.AimOffset.y, aimOffsetZ = c.AimOffset.z;
            f32 aimPoleX = c.AimPoleVector.x, aimPoleY = c.AimPoleVector.y, aimPoleZ = c.AimPoleVector.z;
            u32 aimChainLength = c.AimChainLength;
            f32 aimChainFactor = c.AimChainFactor, aimWeight = c.AimWeight;
            UUID aimTargetEntity = c.AimTargetEntity;

            bool limbIKEnabled = c.LimbIKEnabled;
            u32 limbBoneIndex = c.LimbBoneIndex;
            f32 limbTargetX = c.LimbTarget.x, limbTargetY = c.LimbTarget.y, limbTargetZ = c.LimbTarget.z;
            u32 limbChainLength = c.LimbChainLength;
            f32 limbWeight = c.LimbWeight;
            UUID limbTargetEntity = c.LimbTargetEntity;

            w << aimIKEnabled << aimBoneIndex;
            w << aimTargetX << aimTargetY << aimTargetZ;
            w << aimAxisX << aimAxisY << aimAxisZ;
            w << aimOffsetX << aimOffsetY << aimOffsetZ;
            w << aimPoleX << aimPoleY << aimPoleZ;
            w << aimChainLength << aimChainFactor << aimWeight;
            w << aimTargetEntity;

            w << limbIKEnabled << limbBoneIndex;
            w << limbTargetX << limbTargetY << limbTargetZ;
            w << limbChainLength << limbWeight;
            w << limbTargetEntity;
            return buf;
        }

        // Writes exactly the fields LightProbeVolumeComponent::Serialize() reads
        // unconditionally before its "Format v9" (realtime DDGI, issue #632) gate.
        std::vector<u8> BuildPreV9LightProbeVolumePayload(const LightProbeVolumeComponent& c)
        {
            std::vector<u8> buf;
            FMemoryWriter w(buf);
            w.ArIsSaveGame = true;

            glm::vec3 boundsMin = c.m_BoundsMin, boundsMax = c.m_BoundsMax;
            glm::ivec3 resolution = c.m_Resolution;
            f32 spacing = c.m_Spacing, intensity = c.m_Intensity;
            bool active = c.m_Active, dirty = c.m_Dirty, showDebugProbes = c.m_ShowDebugProbes;
            AssetHandle bakedDataAsset = c.m_BakedDataAsset;

            w << boundsMin << boundsMax << resolution;
            w << spacing << intensity;
            w << active << dirty << showDebugProbes;
            w << bakedDataAsset;
            return buf;
        }

        // Locates the single typeHash+dataSize+payload component block within a
        // captured save-game buffer (SAVE_COMPONENT's on-disk framing) and returns
        // its [start, end) byte range -- start is the typeHash's first byte, end is
        // one past the payload's last byte.
        std::pair<sizet, sizet> FindComponentBlock(const std::vector<u8>& buffer, u32 typeHash)
        {
            u8 needle[sizeof(u32)];
            std::memcpy(needle, &typeHash, sizeof(u32));

            std::vector<sizet> matches;
            for (sizet i = 0; i + sizeof(u32) <= buffer.size(); ++i)
            {
                if (std::memcmp(buffer.data() + i, needle, sizeof(u32)) == 0)
                {
                    matches.push_back(i);
                }
            }
            EXPECT_EQ(matches.size(), 1u) << "Expected exactly one occurrence of the component's typeHash";
            if (matches.size() != 1u)
            {
                return { 0, 0 };
            }

            sizet start = matches[0];

            // Bounds-check before reading dataSize: a typeHash match at the very end of
            // the buffer would otherwise read the size field out of range.
            sizet sizeFieldEnd = start + sizeof(u32) + sizeof(u32);
            EXPECT_LE(sizeFieldEnd, buffer.size()) << "Component frame header runs past the end of the buffer";
            if (sizeFieldEnd > buffer.size())
            {
                return { 0, 0 };
            }

            u32 dataSize = 0;
            std::memcpy(&dataSize, buffer.data() + start + sizeof(u32), sizeof(u32));
            sizet end = sizeFieldEnd + dataSize;
            EXPECT_LE(end, buffer.size()) << "Component payload runs past the end of the buffer";
            if (end > buffer.size())
            {
                return { 0, 0 };
            }
            return { start, end };
        }
    } // namespace

    // ========================================================================
    // Direct Serialize() gating
    // ========================================================================

    TEST(SaveGameVersionMigration, PreV3TerrainPayloadDefaultsNewFieldsNoDesync)
    {
        TerrainComponent seed;
        seed.m_HeightmapPath = "terrain/height.png";
        seed.m_WorldSizeX = 512.0f;
        seed.m_WorldSizeZ = 384.0f;
        seed.m_ProceduralSeed = 7;
        seed.m_TileFilePattern = "chunk_%d_%d.raw";

        std::vector<u8> payload = BuildPreV3TerrainPayload(seed);

        TerrainComponent loaded;
        FMemoryReader reader(payload);
        reader.ArIsSaveGame = true;
        reader.SetArchiveVersion(2); // pre-v3

        SaveGameComponentSerializer::Serialize(reader, loaded);

        EXPECT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd()) << "Reader did not consume exactly the pre-v3 payload -- desync";

        EXPECT_EQ(loaded.m_HeightmapPath, "terrain/height.png");
        EXPECT_FLOAT_EQ(loaded.m_WorldSizeX, 512.0f);
        EXPECT_FLOAT_EQ(loaded.m_WorldSizeZ, 384.0f);
        EXPECT_EQ(loaded.m_ProceduralSeed, 7);
        EXPECT_EQ(loaded.m_TileFilePattern, "chunk_%d_%d.raw");

        // Fields introduced in v3/v5/v6 must stay at their constructor defaults.
        TerrainComponent freshDefault;
        EXPECT_TRUE(loaded.m_LayerRules.empty());
        EXPECT_EQ(loaded.m_AutoMaterial, freshDefault.m_AutoMaterial);
        EXPECT_EQ(loaded.m_SplatmapGenResolution, freshDefault.m_SplatmapGenResolution);
        EXPECT_EQ(loaded.m_ProceduralErosionIterations, freshDefault.m_ProceduralErosionIterations);
        EXPECT_EQ(loaded.m_CollisionEnabled, freshDefault.m_CollisionEnabled);
    }

    TEST(SaveGameVersionMigration, PreV4IKTargetPayloadDefaultsChainIKNoDesync)
    {
        IKTargetComponent seed;
        seed.AimIKEnabled = true;
        seed.AimBoneIndex = 12;
        seed.LimbIKEnabled = true;
        seed.LimbBoneIndex = 34;

        std::vector<u8> payload = BuildPreV4IKTargetPayload(seed);

        IKTargetComponent loaded;
        FMemoryReader reader(payload);
        reader.ArIsSaveGame = true;
        reader.SetArchiveVersion(3); // pre-v4

        SaveGameComponentSerializer::Serialize(reader, loaded);

        EXPECT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd()) << "Reader did not consume exactly the pre-v4 payload -- desync";

        EXPECT_TRUE(loaded.AimIKEnabled);
        EXPECT_EQ(loaded.AimBoneIndex, 12u);
        EXPECT_TRUE(loaded.LimbIKEnabled);
        EXPECT_EQ(loaded.LimbBoneIndex, 34u);

        // Chain IK (v4+) fields must stay at their constructor defaults.
        IKTargetComponent freshDefault;
        EXPECT_EQ(loaded.ChainIKEnabled, freshDefault.ChainIKEnabled);
        EXPECT_EQ(loaded.ChainLength, freshDefault.ChainLength);
        EXPECT_EQ(loaded.ChainIterations, freshDefault.ChainIterations);
        EXPECT_FLOAT_EQ(loaded.ChainTolerance, freshDefault.ChainTolerance);
        EXPECT_FLOAT_EQ(loaded.ChainWeight, freshDefault.ChainWeight);
    }

    TEST(SaveGameVersionMigration, PreV9LightProbeVolumePayloadDefaultsDDGIFieldsNoDesync)
    {
        LightProbeVolumeComponent seed;
        seed.m_BoundsMin = { -8.0f, -2.0f, -8.0f };
        seed.m_BoundsMax = { 8.0f, 4.0f, 8.0f };
        seed.m_Resolution = { 8, 4, 8 };
        seed.m_Spacing = 2.5f;
        seed.m_Intensity = 1.5f;
        seed.m_Active = false;

        std::vector<u8> payload = BuildPreV9LightProbeVolumePayload(seed);

        LightProbeVolumeComponent loaded;
        FMemoryReader reader(payload);
        reader.ArIsSaveGame = true;
        reader.SetArchiveVersion(8); // pre-v9

        SaveGameComponentSerializer::Serialize(reader, loaded);

        EXPECT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd()) << "Reader did not consume exactly the pre-v9 payload -- desync";

        EXPECT_FLOAT_EQ(loaded.m_BoundsMin.x, -8.0f);
        EXPECT_FLOAT_EQ(loaded.m_BoundsMax.y, 4.0f);
        EXPECT_EQ(loaded.m_Resolution, glm::ivec3(8, 4, 8));
        EXPECT_FLOAT_EQ(loaded.m_Spacing, 2.5f);
        EXPECT_FLOAT_EQ(loaded.m_Intensity, 1.5f);
        EXPECT_FALSE(loaded.m_Active);

        // Realtime DDGI (v9+) fields must stay at their constructor defaults
        // (Mode::Baked — the pre-#632 behavior).
        LightProbeVolumeComponent freshDefault;
        EXPECT_EQ(loaded.m_Mode, freshDefault.m_Mode);
        EXPECT_EQ(loaded.m_RaysPerProbe, freshDefault.m_RaysPerProbe);
        EXPECT_FLOAT_EQ(loaded.m_Hysteresis, freshDefault.m_Hysteresis);
        EXPECT_EQ(loaded.m_ProbeCaptureBudget, freshDefault.m_ProbeCaptureBudget);
        EXPECT_EQ(loaded.m_RelightBudget, freshDefault.m_RelightBudget);
        EXPECT_FLOAT_EQ(loaded.m_SelfShadowBias, freshDefault.m_SelfShadowBias);
    }

    // ========================================================================
    // End-to-end: a full pre-v3 save-game restores through RestoreSceneState
    // ========================================================================

    TEST(SaveGameVersionMigration, FullSaveWithPreV3TerrainRestoresThroughRestoreSceneState)
    {
        auto scene = Scene::Create();
        Entity entity = scene->CreateEntity("Terrain");
        auto& terrain = entity.AddComponent<TerrainComponent>();
        terrain.m_HeightmapPath = "levels/valley.png";
        terrain.m_WorldSizeX = 1024.0f;
        terrain.m_WorldSizeZ = 1024.0f;

        // Capture with the current (v6) serializer, then surgically replace the
        // TerrainComponent's on-disk block with a hand-written pre-v3 payload --
        // exactly what a genuine pre-v3 save would have contained for it.
        std::vector<u8> data = SaveGameSerializer::CaptureSceneState(*scene);

        constexpr u32 kTerrainTypeHash = Hash::GenerateFNVHash("TerrainComponent");
        auto [start, end] = FindComponentBlock(data, kTerrainTypeHash);
        ASSERT_LT(start, end);

        std::vector<u8> preV3Payload = BuildPreV3TerrainPayload(terrain);
        u32 preV3Size = static_cast<u32>(preV3Payload.size());

        std::vector<u8> replacement(sizeof(u32) + sizeof(u32) + preV3Payload.size());
        std::memcpy(replacement.data(), &kTerrainTypeHash, sizeof(u32));
        std::memcpy(replacement.data() + sizeof(u32), &preV3Size, sizeof(u32));
        std::memcpy(replacement.data() + 2 * sizeof(u32), preV3Payload.data(), preV3Payload.size());

        data.erase(data.begin() + static_cast<std::ptrdiff_t>(start), data.begin() + static_cast<std::ptrdiff_t>(end));
        data.insert(data.begin() + static_cast<std::ptrdiff_t>(start), replacement.begin(), replacement.end());

        auto loadedScene = Scene::Create();
        ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*loadedScene, data, /*formatVersion=*/2));

        Entity loadedEntity = FindByTag(*loadedScene, "Terrain");
        ASSERT_TRUE(loadedEntity);
        ASSERT_TRUE(loadedEntity.HasComponent<TerrainComponent>());

        const auto& loadedTerrain = loadedEntity.GetComponent<TerrainComponent>();
        EXPECT_EQ(loadedTerrain.m_HeightmapPath, "levels/valley.png");
        EXPECT_FLOAT_EQ(loadedTerrain.m_WorldSizeX, 1024.0f);
        EXPECT_FLOAT_EQ(loadedTerrain.m_WorldSizeZ, 1024.0f);

        TerrainComponent freshDefault;
        EXPECT_TRUE(loadedTerrain.m_LayerRules.empty());
        EXPECT_EQ(loadedTerrain.m_AutoMaterial, freshDefault.m_AutoMaterial);
        EXPECT_EQ(loadedTerrain.m_ProceduralErosionIterations, freshDefault.m_ProceduralErosionIterations);
        EXPECT_EQ(loadedTerrain.m_CollisionEnabled, freshDefault.m_CollisionEnabled);
    }
} // namespace OloEngine::Tests
