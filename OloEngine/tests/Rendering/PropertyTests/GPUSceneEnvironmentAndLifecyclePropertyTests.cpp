// OLO_TEST_LAYER: L1

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "Rendering/PropertyTests/GPUSceneRecordTestHelpers.h"

#include <initializer_list>
#include <string>
#include <vector>

// The environment registry (issue #993) and the lifecycle rules shared by
// every record kind: an owner-token change and Reset() tombstone materials,
// lights and environments together, and the per-kind stats count it.

namespace OloEngine::Tests
{
    using namespace GPUSceneRecordTesting;

    TEST(GPUScene, EnvironmentOwnerZeroOccupiesOneSlotAndIntensityEditIsCompatible)
    {
        GPUScene scene;
        const GPUSceneEnvironmentKey globalKey{}; // owner 0: the renderer's published global IBL
        GPUSceneEnvironmentInput environment{ .m_Environment = TextureRef(20, 1, 100),
                                              .m_Irradiance = TextureRef(21, 1, 101),
                                              .m_Prefilter = TextureRef(22, 3, 102),
                                              .m_BRDFLut = TextureRef(23, 1, 103),
                                              .m_Intensity = 1.0f };
        auto extract = [&]() -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            scene.ExtractEnvironment(globalKey, environment);
            return scene.EndExtraction();
        };

        // Publishing the same key twice in one frame is one slot; the last input wins.
        scene.BeginExtraction(1, glm::vec3(0.0f));
        GPUSceneEnvironmentInput earlier = environment;
        earlier.m_Intensity = 0.1f;
        scene.ExtractEnvironment(globalKey, earlier);
        scene.ExtractEnvironment(globalKey, environment);
        const GPUSceneFrameUpdate first = scene.EndExtraction();
        EXPECT_TRUE(DirtyRangesAre(first.m_EnvironmentDirtyRanges, { { 0, 1 } }));
        EXPECT_EQ(first.m_Stats.m_Environments.m_Live, 1u);
        EXPECT_EQ(first.m_Stats.m_Environments.m_SlotCount, 1u);

        const GPUSceneHandle handle = scene.FindEnvironment(globalKey);
        ASSERT_TRUE(handle.IsValid());
        EXPECT_EQ(handle.m_Index, 0u);
        EXPECT_EQ(handle.m_Generation, 1u);
        const GPUSceneEnvironment* record = scene.GetEnvironmentRecord(handle);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(Math::BitwiseEqual(*record, EncodeGPUSceneEnvironment(environment, 0, 1)));
        EXPECT_FLOAT_EQ(record->Intensity, 1.0f);
        EXPECT_EQ(record->PrefilterIndex, 22u);
        EXPECT_EQ(record->PrefilterGeneration, 3u);
        EXPECT_EQ(record->PrefilterHeapOffset, 102u);
        constexpr u32 fullyLit =
            GPUSceneEnvironmentFlagActive | GPUSceneEnvironmentFlagIBL | GPUSceneEnvironmentFlagEnvironmentMap;
        EXPECT_EQ(record->Flags, fullyLit);

        environment.m_Intensity = 0.5f;
        const GPUSceneFrameUpdate dimmed = extract();
        EXPECT_TRUE(DirtyRangesAre(dimmed.m_EnvironmentDirtyRanges, { { 0, 1 } }));
        EXPECT_EQ(scene.FindEnvironment(globalKey), handle) << "intensity is not part of the environment's identity";
        record = scene.GetEnvironmentRecord(handle);
        ASSERT_NE(record, nullptr);
        EXPECT_FLOAT_EQ(record->Intensity, 0.5f);
        EXPECT_TRUE(extract().m_EnvironmentDirtyRanges.empty());
    }

    TEST(GPUScene, EnvironmentTextureSwapAdvancesGenerationInPlaceAndRemovalTombstones)
    {
        GPUScene scene;
        const GPUSceneEnvironmentKey globalKey{};
        GPUSceneEnvironmentInput environment{ .m_Environment = TextureRef(20, 1, 100),
                                              .m_Irradiance = TextureRef(21, 1, 101),
                                              .m_Prefilter = TextureRef(22, 1, 102),
                                              .m_BRDFLut = TextureRef(23, 1, 103) };
        auto extract = [&](bool published) -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            if (published)
            {
                scene.ExtractEnvironment(globalKey, environment);
            }
            return scene.EndExtraction();
        };

        (void)extract(true);
        const GPUSceneHandle original = scene.FindEnvironment(globalKey);
        ASSERT_TRUE(original.IsValid());

        // Dropping the BRDF LUT is a texture identity change: same slot, next
        // generation, IBL flag gone.
        environment.m_BRDFLut = GPUSceneTextureRef{};
        const GPUSceneFrameUpdate swapped = extract(true);
        const GPUSceneHandle replaced = scene.FindEnvironment(globalKey);
        EXPECT_EQ(replaced.m_Index, original.m_Index);
        EXPECT_EQ(replaced.m_Generation, original.m_Generation + 1u);
        EXPECT_FALSE(scene.IsEnvironmentHandleLive(original));
        EXPECT_TRUE(scene.IsEnvironmentHandleLive(replaced));
        EXPECT_TRUE(DirtyRangesAre(swapped.m_EnvironmentDirtyRanges, { { 0, 1 } }));
        const GPUSceneEnvironment* record = scene.GetEnvironmentRecord(replaced);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->Generation, replaced.m_Generation);
        EXPECT_EQ(record->BRDFLutHeapOffset, GPUSceneHeapOffsetUnresolved);
        EXPECT_EQ(record->Flags & GPUSceneEnvironmentFlagIBL, 0u);
        EXPECT_NE(record->Flags & GPUSceneEnvironmentFlagEnvironmentMap, 0u);

        // A re-created texture (same index, next RHI generation) is a swap too.
        environment.m_Prefilter.m_Handle.Generation += 1;
        (void)extract(true);
        const GPUSceneHandle recreated = scene.FindEnvironment(globalKey);
        EXPECT_EQ(recreated.m_Index, original.m_Index);
        EXPECT_EQ(recreated.m_Generation, original.m_Generation + 2u);
        EXPECT_FALSE(scene.IsEnvironmentHandleLive(replaced));

        const GPUSceneFrameUpdate removed = extract(false);
        EXPECT_FALSE(scene.IsEnvironmentHandleLive(recreated));
        EXPECT_EQ(scene.GetEnvironmentRecord(recreated), nullptr);
        EXPECT_FALSE(scene.FindEnvironment(globalKey).IsValid());
        EXPECT_TRUE(DirtyRangesAre(removed.m_EnvironmentDirtyRanges, { { 0, 1 } })) << "the tombstone must be uploaded";
        EXPECT_EQ(removed.m_Stats.m_Environments.m_Live, 0u);
        EXPECT_EQ(removed.m_Stats.m_Environments.m_RetiredSlots, 1u);
        EXPECT_EQ(removed.m_Stats.m_Environments.m_SlotCount, 1u);
    }

    TEST(GPUScene, OwnerChangeResetsMaterialsLightsAndEnvironmentsTogether)
    {
        GPUScene scene;
        const GPUSceneMaterialKey materialA = ImportedMaterial(1);
        const GPUSceneMaterialKey materialB = ImportedMaterial(2);
        const GPUSceneLightKey lightA{ .m_EntityId = 1, .m_Type = kPoint };
        const GPUSceneLightKey lightB{ .m_EntityId = 2, .m_Type = kSpot };
        const GPUSceneEnvironmentKey globalKey{};
        GPUSceneMaterialInput material;
        material.m_Albedo = TextureRef(5, 1, 7);
        const GPUSceneEnvironmentInput environment{ .m_Environment = TextureRef(20, 1, 100) };
        auto stage = [&]()
        {
            scene.ExtractMaterial(materialA, material);
            scene.ExtractMaterial(materialB, material);
            scene.ExtractLight(lightA, GPUSceneLightInput{ .m_Type = kPoint });
            scene.ExtractLight(lightB, GPUSceneLightInput{ .m_Type = kSpot });
            scene.ExtractEnvironment(globalKey, environment);
        };

        scene.BeginExtraction(1, glm::vec3(0.0f));
        stage();
        (void)scene.EndExtraction();
        const GPUSceneHandle staleMaterial = scene.FindMaterial(materialA);
        const GPUSceneHandle staleLight = scene.FindLight(lightA);
        const GPUSceneHandle staleEnvironment = scene.FindEnvironment(globalKey);
        ASSERT_TRUE(staleMaterial.IsValid());
        ASSERT_TRUE(staleLight.IsValid());
        ASSERT_TRUE(staleEnvironment.IsValid());

        // The owner change resets inside BeginExtraction. Until EndExtraction every
        // old record is a tombstone: dead handles, nothing to find, the old slots dirty.
        scene.BeginExtraction(2, glm::vec3(0.0f));
        EXPECT_FALSE(scene.IsMaterialHandleLive(staleMaterial));
        EXPECT_FALSE(scene.IsLightHandleLive(staleLight));
        EXPECT_FALSE(scene.IsEnvironmentHandleLive(staleEnvironment));
        EXPECT_EQ(scene.GetMaterialRecord(staleMaterial), nullptr);
        EXPECT_FALSE(scene.FindMaterial(materialA).IsValid());
        EXPECT_FALSE(scene.FindLight(lightB).IsValid());
        const GPUSceneFrameUpdate reset = scene.GetLastFrameUpdate();
        EXPECT_TRUE(DirtyRangesAre(reset.m_MaterialDirtyRanges, { { 0, 2 } }));
        EXPECT_TRUE(DirtyRangesAre(reset.m_LightDirtyRanges, { { 0, 2 } }));
        EXPECT_TRUE(DirtyRangesAre(reset.m_EnvironmentDirtyRanges, { { 0, 1 } }));
        EXPECT_EQ(reset.m_Stats.m_Materials.m_Live, 0u);
        EXPECT_EQ(reset.m_Stats.m_Materials.m_RetiredSlots, 2u);
        EXPECT_EQ(reset.m_Stats.m_Lights.m_Live, 0u);
        EXPECT_EQ(reset.m_Stats.m_Lights.m_RetiredSlots, 2u);
        EXPECT_EQ(reset.m_Stats.m_Environments.m_Live, 0u);
        EXPECT_EQ(reset.m_Stats.m_Environments.m_RetiredSlots, 1u);

        // The heap resolved the same texture to a different offset after the reset.
        material.m_Albedo.m_HeapOffset = 8;
        stage();
        const GPUSceneFrameUpdate replacement = scene.EndExtraction();
        const GPUSceneHandle freshMaterial = scene.FindMaterial(materialA);
        EXPECT_EQ(freshMaterial.m_Index, 2u) << "the retired slots are still visible to a buffered frame";
        EXPECT_EQ(scene.FindMaterial(materialB).m_Index, 3u);
        EXPECT_EQ(scene.FindLight(lightA).m_Index, 2u);
        EXPECT_EQ(scene.FindLight(lightB).m_Index, 3u);
        EXPECT_EQ(scene.FindEnvironment(globalKey).m_Index, 1u);
        EXPECT_FALSE(scene.IsMaterialHandleLive(staleMaterial));
        EXPECT_TRUE(DirtyRangesAre(replacement.m_MaterialDirtyRanges, { { 0, 4 } }))
            << "tombstones and fresh records upload together";
        EXPECT_TRUE(DirtyRangesAre(replacement.m_LightDirtyRanges, { { 0, 4 } }));
        EXPECT_TRUE(DirtyRangesAre(replacement.m_EnvironmentDirtyRanges, { { 0, 2 } }));
        EXPECT_EQ(replacement.m_Stats.m_Materials.m_SlotCount, 4u);
        EXPECT_EQ(replacement.m_Stats.m_Materials.m_Live, 2u);
        const GPUSceneMaterial* record = scene.GetMaterialRecord(freshMaterial);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->Generation, 1u) << "a fresh slot starts at generation 1";
        EXPECT_EQ(record->AlbedoHeapOffset, 8u) << "the offset is re-resolved from the input, never carried over";
    }

    TEST(GPUScene, ResetTombstonesEveryKindAndATombstoneCarriesNoHeapOffset)
    {
        // A tombstone is the default record plus StableIndex and Generation
        // (GPUScene.cpp, MakeTombstone). Dead records are not readable through
        // the registry, so the "offsets are dropped with the record" half of
        // the reset rule is pinned on the default record itself.
        const GPUSceneMaterial deadMaterial{};
        for (u32 offset : { deadMaterial.AlbedoHeapOffset, deadMaterial.MetallicRoughnessHeapOffset,
                            deadMaterial.NormalHeapOffset, deadMaterial.OcclusionHeapOffset,
                            deadMaterial.EmissiveHeapOffset, deadMaterial.SpecularHeapOffset })
        {
            EXPECT_EQ(offset, GPUSceneHeapOffsetUnresolved);
        }
        EXPECT_EQ(deadMaterial.Flags, 0u) << "a tombstone is not Active";
        EXPECT_EQ(deadMaterial.AlbedoTextureIndex, RHI::ResourceHandle::InvalidIndex);
        const GPUSceneEnvironment deadEnvironment{};
        for (u32 offset : { deadEnvironment.EnvironmentHeapOffset, deadEnvironment.IrradianceHeapOffset,
                            deadEnvironment.PrefilterHeapOffset, deadEnvironment.BRDFLutHeapOffset })
        {
            EXPECT_EQ(offset, GPUSceneHeapOffsetUnresolved);
        }
        EXPECT_EQ(deadEnvironment.Flags, 0u);
        EXPECT_EQ(GPUSceneLight{}.Flags, 0u);

        // The explicit Reset() runs outside a frame and publishes the tombstone
        // ranges through GetLastFrameUpdate at once, without an EndExtraction;
        // the next frame, under any owner token, lands in fresh slots.
        GPUScene scene;
        const GPUSceneMaterialKey materialKey = ImportedMaterial(1);
        const GPUSceneLightKey lightKey{ .m_EntityId = 1, .m_Type = kPoint };
        const GPUSceneEnvironmentKey globalKey{};
        auto extract = [&](u64 owner) -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(owner, glm::vec3(0.0f));
            scene.ExtractMaterial(materialKey, GPUSceneMaterialInput{});
            scene.ExtractLight(lightKey, GPUSceneLightInput{});
            scene.ExtractEnvironment(globalKey, GPUSceneEnvironmentInput{});
            return scene.EndExtraction();
        };

        (void)extract(1);
        const GPUSceneHandle staleMaterial = scene.FindMaterial(materialKey);
        const GPUSceneHandle staleLight = scene.FindLight(lightKey);
        const GPUSceneHandle staleEnvironment = scene.FindEnvironment(globalKey);
        scene.Reset();
        EXPECT_FALSE(scene.IsMaterialHandleLive(staleMaterial));
        EXPECT_FALSE(scene.IsLightHandleLive(staleLight));
        EXPECT_FALSE(scene.IsEnvironmentHandleLive(staleEnvironment));
        EXPECT_TRUE(DirtyRangesAre(scene.GetLastFrameUpdate().m_MaterialDirtyRanges, { { 0, 1 } }));
        EXPECT_TRUE(DirtyRangesAre(scene.GetLastFrameUpdate().m_LightDirtyRanges, { { 0, 1 } }));
        EXPECT_TRUE(DirtyRangesAre(scene.GetLastFrameUpdate().m_EnvironmentDirtyRanges, { { 0, 1 } }));

        const GPUSceneFrameUpdate replacement = extract(99);
        EXPECT_EQ(scene.FindMaterial(materialKey).m_Index, 1u);
        EXPECT_EQ(scene.FindLight(lightKey).m_Index, 1u);
        EXPECT_EQ(scene.FindEnvironment(globalKey).m_Index, 1u);
        EXPECT_EQ(replacement.m_Stats.m_Materials.m_RetiredSlots, 1u);
        EXPECT_EQ(replacement.m_Stats.m_Lights.m_RetiredSlots, 1u);
        EXPECT_EQ(replacement.m_Stats.m_Environments.m_RetiredSlots, 1u);
    }

    TEST(GPUScene, KindStatsCountLiveFreeAndRetiredSlotsPerKind)
    {
        GPUScene scene;
        auto extract = [&](std::initializer_list<u64> materialOwners,
                           std::initializer_list<u64> lightEntities) -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            for (u64 owner : materialOwners)
            {
                scene.ExtractMaterial(ImportedMaterial(owner), GPUSceneMaterialInput{});
            }
            for (u64 entity : lightEntities)
            {
                scene.ExtractLight(GPUSceneLightKey{ .m_EntityId = entity, .m_Type = kPoint }, GPUSceneLightInput{});
            }
            scene.ExtractEnvironment(GPUSceneEnvironmentKey{}, GPUSceneEnvironmentInput{});
            return scene.EndExtraction();
        };
        const auto expectKind = [](const GPUSceneKindStats& stats, u32 live, u32 slots, u32 freeSlots, u32 retired)
        {
            EXPECT_EQ(stats.m_Live, live);
            EXPECT_EQ(stats.m_SlotCount, slots);
            EXPECT_EQ(stats.m_FreeSlots, freeSlots);
            EXPECT_EQ(stats.m_RetiredSlots, retired);
            EXPECT_EQ(stats.m_BufferCapacity, 0u) << "CPU-only registry: no buffer";
            EXPECT_EQ(stats.m_UploadBytes, 0u);
        };

        const GPUSceneFrameUpdate frame1 = extract({ 1, 2, 3 }, { 1, 2 });
        expectKind(frame1.m_Stats.m_Materials, 3, 3, 0, 0);
        expectKind(frame1.m_Stats.m_Lights, 2, 2, 0, 0);
        expectKind(frame1.m_Stats.m_Environments, 1, 1, 0, 0);

        // Frame 2 drops material 2 and light 1: both retire until frame 4.
        const GPUSceneFrameUpdate frame2 = extract({ 1, 3 }, { 2 });
        expectKind(frame2.m_Stats.m_Materials, 2, 3, 0, 1);
        expectKind(frame2.m_Stats.m_Lights, 1, 2, 0, 1);
        expectKind(frame2.m_Stats.m_Environments, 1, 1, 0, 0);

        const GPUSceneFrameUpdate frame3 = extract({ 1, 3 }, { 2 });
        expectKind(frame3.m_Stats.m_Materials, 2, 3, 0, 1);
        expectKind(frame3.m_Stats.m_Lights, 1, 2, 0, 1);

        const GPUSceneFrameUpdate frame4 = extract({ 1, 3 }, { 2 });
        expectKind(frame4.m_Stats.m_Materials, 2, 3, 1, 0);
        expectKind(frame4.m_Stats.m_Lights, 1, 2, 1, 0);

        // Frame 5: a new material takes the freed slot 1 with generation 2; the lights keep their free slot.
        const GPUSceneFrameUpdate frame5 = extract({ 1, 3, 9 }, { 2 });
        expectKind(frame5.m_Stats.m_Materials, 3, 3, 0, 0);
        expectKind(frame5.m_Stats.m_Lights, 1, 2, 1, 0);
        expectKind(frame5.m_Stats.m_Environments, 1, 1, 0, 0);
        EXPECT_EQ(scene.FindMaterial(ImportedMaterial(9)), (GPUSceneHandle{ 1, 2 }));
    }
} // namespace OloEngine::Tests
