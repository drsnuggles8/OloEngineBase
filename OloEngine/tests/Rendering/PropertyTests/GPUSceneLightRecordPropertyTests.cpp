// OLO_TEST_LAYER: L1

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "Rendering/PropertyTests/GPUSceneRecordTestHelpers.h"

#include <initializer_list>
#include <string>
#include <vector>

// The identity rules of the light registry (issue #993): a light keeps its
// slot through every compatible edit, a type change is a new key, positions
// are render-relative, and no authored field is dropped.

namespace OloEngine::Tests
{
    using namespace GPUSceneRecordTesting;

    TEST(GPUScene, LightHandleSurvivesCompatibleEdits)
    {
        GPUScene scene;
        const GPUSceneLightKey pointKey{ .m_EntityId = 3, .m_Type = kPoint };
        const GPUSceneLightKey spotKey{ .m_EntityId = 7, .m_Type = kSpot };
        GPUSceneLightInput spot{ .m_Type = kSpot,
                                 .m_Position = glm::vec3(1.0f, 2.0f, 3.0f),
                                 .m_Range = 10.0f,
                                 .m_InnerCutoffDegrees = 12.5f,
                                 .m_OuterCutoffDegrees = 17.5f,
                                 .m_Attenuation = 0.05f };

        auto extract = [&]() -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            scene.ExtractLight(spotKey, spot);
            scene.ExtractLight(pointKey, GPUSceneLightInput{});
            return scene.EndExtraction();
        };

        (void)extract();
        const GPUSceneHandle handle = scene.FindLight(spotKey);
        ASSERT_TRUE(handle.IsValid());
        EXPECT_EQ(handle.m_Index, 1u) << "(3, Point) sorts before (7, Spot)";

        // Called after every edit of the spot below: slot 1 dirty, same handle,
        // record equal to the encoder's output for the new input.
        const auto expectStable = [&](const char* edit)
        {
            SCOPED_TRACE(edit);
            const GPUSceneFrameUpdate update = extract();
            EXPECT_TRUE(DirtyRangesAre(update.m_LightDirtyRanges, { { 1, 1 } }));
            EXPECT_EQ(scene.FindLight(spotKey), handle);
            const GPUSceneLight* record = scene.GetLightRecord(handle);
            ASSERT_NE(record, nullptr);
            EXPECT_TRUE(Math::BitwiseEqual(
                *record, EncodeGPUSceneLight(spot, glm::vec3(0.0f), handle.m_Index, handle.m_Generation)));
        };
        spot.m_Position = glm::vec3(4.0f, 5.0f, 6.0f);
        expectStable("movement");
        spot.m_Color = glm::vec3(1.0f, 0.5f, 0.25f);
        expectStable("colour");
        spot.m_Intensity = 3.5f;
        expectStable("intensity");
        spot.m_Range = 25.0f;
        expectStable("range");
        spot.m_InnerCutoffDegrees = 20.0f;
        spot.m_OuterCutoffDegrees = 30.0f;
        expectStable("cone");
        spot.m_CastShadows = true;
        expectStable("cast shadows");

        const GPUSceneLight* record = scene.GetLightRecord(handle);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->Generation, 1u) << "no light edit is incompatible in place";
        EXPECT_NE(record->Flags & GPUSceneLightFlagCastShadows, 0u);
        EXPECT_TRUE(extract().m_LightDirtyRanges.empty());
    }

    TEST(GPUScene, LightTypeChangeRetiresTheOldSlotAndReuseAdvancesTheGeneration)
    {
        GPUScene scene;
        const GPUSceneLightKey pointKey{ .m_EntityId = 7, .m_Type = kPoint };
        const GPUSceneLightKey spotKey{ .m_EntityId = 7, .m_Type = kSpot };
        const GPUSceneLightKey laterKey{ .m_EntityId = 9, .m_Type = kPoint };
        const GPUSceneLightKey earlierKey{ .m_EntityId = 1, .m_Type = kPoint };

        auto extract = [&](std::initializer_list<GPUSceneLightKey> keys) -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            for (const GPUSceneLightKey& key : keys)
            {
                scene.ExtractLight(key, GPUSceneLightInput{ .m_Type = key.m_Type });
            }
            return scene.EndExtraction();
        };

        (void)extract({ pointKey }); // frame 1
        const GPUSceneHandle asPoint = scene.FindLight(pointKey);
        ASSERT_TRUE(asPoint.IsValid());
        EXPECT_EQ(asPoint.m_Index, 0u);

        // Frame 2: the same entity is now a spot light. The type is part of the
        // key, so the point slot dies (retired until frame 4) and the spot appends.
        const GPUSceneFrameUpdate retyped = extract({ spotKey });
        const GPUSceneHandle asSpot = scene.FindLight(spotKey);
        ASSERT_TRUE(asSpot.IsValid());
        EXPECT_EQ(asSpot.m_Index, 1u);
        EXPECT_EQ(asSpot.m_Generation, 1u);
        EXPECT_FALSE(scene.IsLightHandleLive(asPoint));
        EXPECT_FALSE(scene.FindLight(pointKey).IsValid());
        EXPECT_TRUE(DirtyRangesAre(retyped.m_LightDirtyRanges, { { 0, 2 } })) << "tombstone plus the new record";
        EXPECT_EQ(retyped.m_Stats.m_Lights.m_RetiredSlots, 1u);
        EXPECT_EQ(retyped.m_Stats.m_Lights.m_Live, 1u);

        // Frame 3: slot 0 is still retired, so another light appends.
        (void)extract({ spotKey, laterKey });
        EXPECT_EQ(scene.FindLight(laterKey).m_Index, 2u);
        EXPECT_EQ(scene.GetLastFrameUpdate().m_Stats.m_Lights.m_RetiredSlots, 1u);

        // Frame 4: slot 0 is released; the lowest key takes it with the next generation.
        const GPUSceneFrameUpdate reuse = extract({ spotKey, laterKey, earlierKey });
        const GPUSceneHandle reused = scene.FindLight(earlierKey);
        EXPECT_EQ(reused.m_Index, asPoint.m_Index);
        EXPECT_EQ(reused.m_Generation, asPoint.m_Generation + 1u);
        EXPECT_FALSE(scene.IsLightHandleLive(asPoint));
        EXPECT_TRUE(scene.IsLightHandleLive(reused));
        EXPECT_EQ(scene.FindLight(spotKey), asSpot);
        EXPECT_EQ(reuse.m_Stats.m_Lights.m_RetiredSlots, 0u);
        EXPECT_EQ(reuse.m_Stats.m_Lights.m_Live, 3u);
    }

    TEST(GPUScene, LightSlotsFollowKeyOrderNotExtractionOrder)
    {
        GPUScene scene;
        // Key order is (entity, type): entity 1's directional (0) and spot (2)
        // lights sort before entity 2's point light whatever the extraction order.
        const std::vector<GPUSceneLightKey> keyOrder{ { .m_EntityId = 1, .m_Type = kDirectional },
                                                      { .m_EntityId = 1, .m_Type = kSpot },
                                                      { .m_EntityId = 2, .m_Type = kPoint },
                                                      { .m_EntityId = 3, .m_Type = kSphereArea } };

        auto extract = [&](bool reverse)
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            for (sizet i = 0, count = keyOrder.size(); i < count; ++i)
            {
                const GPUSceneLightKey& key = keyOrder[reverse ? keyOrder.size() - 1 - i : i];
                scene.ExtractLight(key, GPUSceneLightInput{ .m_Type = key.m_Type });
            }
            (void)scene.EndExtraction();
        };

        extract(true);
        std::vector<GPUSceneHandle> handles;
        for (sizet i = 0, count = keyOrder.size(); i < count; ++i)
        {
            const GPUSceneHandle handle = scene.FindLight(keyOrder[i]);
            ASSERT_TRUE(handle.IsValid());
            EXPECT_EQ(handle.m_Index, static_cast<u32>(i));
            handles.push_back(handle);
        }

        extract(false);
        for (sizet i = 0, count = keyOrder.size(); i < count; ++i)
        {
            EXPECT_EQ(scene.FindLight(keyOrder[i]), handles[i]);
        }
        EXPECT_TRUE(scene.GetLastFrameUpdate().m_LightDirtyRanges.empty());
    }

    TEST(GPUScene, LightRecordsAreRenderRelativeAndFollowTheOrigin)
    {
        GPUScene scene;
        const GPUSceneLightKey spotKey{ .m_EntityId = 1, .m_Type = kSpot };
        const GPUSceneLightKey sunKey{ .m_EntityId = 2, .m_Type = kDirectional };
        const glm::vec3 worldPosition(1010.0f, 5.0f, -3.0f);
        const GPUSceneLightInput spot{ .m_Type = kSpot,
                                       .m_Position = worldPosition,
                                       .m_Direction = glm::vec3(0.3f, -1.0f, 0.2f),
                                       .m_Range = 12.0f,
                                       .m_InnerCutoffDegrees = 15.0f,
                                       .m_OuterCutoffDegrees = 25.0f,
                                       .m_Attenuation = 0.02f };
        // A directional light authored with a position and a range: neither may reach the record.
        const GPUSceneLightInput sun{ .m_Type = kDirectional,
                                      .m_Position = worldPosition,
                                      .m_Direction = glm::vec3(0.3f, -1.0f, 0.2f),
                                      .m_Range = 12.0f };

        auto extract = [&](f32 originX) -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(originX, 0.0f, 0.0f));
            scene.ExtractLight(sunKey, sun);
            scene.ExtractLight(spotKey, spot);
            return scene.EndExtraction();
        };

        (void)extract(1000.0f);
        const GPUSceneHandle spotHandle = scene.FindLight(spotKey);
        ASSERT_TRUE(spotHandle.IsValid());
        EXPECT_EQ(spotHandle.m_Index, 0u) << "(1, Spot) sorts before (2, Directional)";
        const GPUSceneLight* record = scene.GetLightRecord(spotHandle);
        ASSERT_NE(record, nullptr);
        const glm::vec3 origin(1000.0f, 0.0f, 0.0f);
        EXPECT_TRUE(Math::BitwiseEqual(
            *record, EncodeGPUSceneLight(spot, origin, spotHandle.m_Index, spotHandle.m_Generation)));
        EXPECT_TRUE(SameVec4(record->PositionAndRange, glm::vec4(10.0f, 5.0f, -3.0f, 12.0f))) << "1010 - 1000 on x";

        const GPUSceneLight* sunRecord = scene.GetLightRecord(scene.FindLight(sunKey));
        ASSERT_NE(sunRecord, nullptr);
        EXPECT_TRUE(SameVec4(sunRecord->PositionAndRange, glm::vec4(0.0f)));

        // A stationary light under a moved origin is a different record; the sun
        // has no position, so only slot 0 moves.
        const GPUSceneFrameUpdate rebased = extract(1005.0f);
        EXPECT_TRUE(DirtyRangesAre(rebased.m_LightDirtyRanges, { { 0, 1 } }));
        record = scene.GetLightRecord(spotHandle);
        ASSERT_NE(record, nullptr);
        EXPECT_FLOAT_EQ(record->PositionAndRange.x, 5.0f);
        EXPECT_TRUE(extract(1005.0f).m_LightDirtyRanges.empty());
    }

    TEST(GPUScene, LightRecordCarriesEveryAuthoredFieldOfEachType)
    {
        GPUScene scene;
        const glm::vec3 position(3.0f, -2.0f, 8.5f);
        const glm::vec3 direction(0.25f, -0.5f, 2.0f); // authored, deliberately not unit length
        const glm::vec3 color(0.9f, 0.6f, 0.3f);
        const auto authored = [&](u32 type, bool castShadows)
        {
            return GPUSceneLightInput{ .m_Type = type,
                                       .m_Position = position,
                                       .m_Direction = direction,
                                       .m_Color = color,
                                       .m_Intensity = 4.5f,
                                       .m_Range = 30.0f,
                                       .m_Radius = 0.75f,
                                       .m_InnerCutoffDegrees = 21.0f,
                                       .m_OuterCutoffDegrees = 33.0f,
                                       .m_Attenuation = 0.07f,
                                       .m_SpotFalloff = 1.25f,
                                       .m_CastShadows = castShadows };
        };

        scene.BeginExtraction(1, glm::vec3(0.0f));
        for (u32 type = kDirectional; type <= kSphereArea; ++type)
        {
            scene.ExtractLight(GPUSceneLightKey{ .m_EntityId = 1, .m_Type = type }, authored(type, type % 2 == 0));
        }
        (void)scene.EndExtraction();
        const auto record = [&](u32 type)
        { return scene.GetLightRecord(scene.FindLight(GPUSceneLightKey{ .m_EntityId = 1, .m_Type = type })); };
        const glm::vec4 colorAndIntensity(color, 4.5f);
        constexpr u32 shadowed = GPUSceneLightFlagActive | GPUSceneLightFlagCastShadows;

        const GPUSceneLight* directional = record(kDirectional);
        ASSERT_NE(directional, nullptr);
        EXPECT_TRUE(SameVec4(directional->PositionAndRange, glm::vec4(0.0f)));
        EXPECT_TRUE(SameVec4(directional->DirectionAndRadius, glm::vec4(direction, 0.0f)));
        EXPECT_TRUE(SameVec4(directional->ColorAndIntensity, colorAndIntensity));
        EXPECT_TRUE(SameVec4(directional->ShapeParams, glm::vec4(0.0f)));
        EXPECT_EQ(directional->Type, kDirectional);
        EXPECT_EQ(directional->Flags, shadowed);

        const GPUSceneLight* point = record(kPoint);
        ASSERT_NE(point, nullptr);
        EXPECT_TRUE(SameVec4(point->PositionAndRange, glm::vec4(position, 30.0f)));
        EXPECT_TRUE(SameVec4(point->DirectionAndRadius, glm::vec4(direction, 0.0f)));
        EXPECT_TRUE(SameVec4(point->ColorAndIntensity, colorAndIntensity));
        EXPECT_TRUE(SameVec4(point->ShapeParams, glm::vec4(0.0f, 0.0f, 0.07f, 0.0f)));
        EXPECT_EQ(point->Type, kPoint);
        EXPECT_EQ(point->Flags, static_cast<u32>(GPUSceneLightFlagActive));

        // The cosines use the expression the raster packing used, so they are
        // bit-identical to it; the falloff is whatever was authored.
        const GPUSceneLight* spot = record(kSpot);
        ASSERT_NE(spot, nullptr);
        EXPECT_TRUE(SameVec4(spot->PositionAndRange, glm::vec4(position, 30.0f)));
        EXPECT_TRUE(SameVec4(spot->DirectionAndRadius, glm::vec4(direction, 0.0f)));
        EXPECT_TRUE(SameVec4(spot->ColorAndIntensity, colorAndIntensity));
        EXPECT_TRUE(SameVec4(spot->ShapeParams,
                             glm::vec4(glm::cos(glm::radians(21.0f)), glm::cos(glm::radians(33.0f)), 0.07f, 1.25f)));
        EXPECT_EQ(spot->Type, kSpot);
        EXPECT_EQ(spot->Flags, shadowed);

        // A sphere area light has no attenuation lane, exactly like the raster
        // structs it replaces (GPUSphereAreaLight, MultiLightData with w=3).
        const GPUSceneLight* sphere = record(kSphereArea);
        ASSERT_NE(sphere, nullptr);
        EXPECT_TRUE(SameVec4(sphere->PositionAndRange, glm::vec4(position, 30.0f)));
        EXPECT_TRUE(SameVec4(sphere->DirectionAndRadius, glm::vec4(direction, 0.75f)));
        EXPECT_TRUE(SameVec4(sphere->ColorAndIntensity, colorAndIntensity));
        EXPECT_TRUE(SameVec4(sphere->ShapeParams, glm::vec4(0.0f)));
        EXPECT_EQ(sphere->Type, kSphereArea);
        EXPECT_EQ(sphere->Flags, static_cast<u32>(GPUSceneLightFlagActive));
    }
} // namespace OloEngine::Tests
