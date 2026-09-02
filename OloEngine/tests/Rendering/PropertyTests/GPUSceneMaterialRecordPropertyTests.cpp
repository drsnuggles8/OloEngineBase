// OLO_TEST_LAYER: L1

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "Rendering/PropertyTests/GPUSceneRecordTestHelpers.h"

#include <array>
#include <initializer_list>
#include <string>
#include <vector>

// The identity and generation rules of the material registry (issue #992),
// pinned against the contract at the top of GPUSceneTypes.h. Instances and
// geometries are pinned by GPUScenePropertyTests.cpp; this file only touches
// them where an instance resolves its material slot.

namespace OloEngine::Tests
{
    using namespace GPUSceneRecordTesting;

    TEST(GPUScene, MaterialSlotsFollowKeyOrderAndStayStableAcrossFrames)
    {
        GPUScene scene;
        // Key order is (owner, slot, source). The four keys below are listed in
        // that order; every extraction below scrambles it.
        const GPUSceneMaterialKey defaultKey{};
        const GPUSceneMaterialKey imported0 = ImportedMaterial(10, 0);
        const GPUSceneMaterialKey override0{ .m_Owner = 10, .m_Slot = 0, .m_Source = kEntityOverride };
        const GPUSceneMaterialKey imported1 = ImportedMaterial(10, 1);
        const std::vector<GPUSceneMaterialKey> keyOrder{ defaultKey, imported0, override0, imported1 };

        auto extract = [&](std::initializer_list<GPUSceneMaterialKey> order) -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            for (const GPUSceneMaterialKey& key : order)
            {
                scene.ExtractMaterial(key, GPUSceneMaterialInput{});
            }
            return scene.EndExtraction();
        };

        const GPUSceneFrameUpdate first = extract({ imported1, override0, defaultKey, imported0 });
        EXPECT_TRUE(DirtyRangesAre(first.m_MaterialDirtyRanges, { { 0, 4 } }));
        std::vector<GPUSceneHandle> handles;
        for (sizet i = 0, count = keyOrder.size(); i < count; ++i)
        {
            const GPUSceneHandle handle = scene.FindMaterial(keyOrder[i]);
            ASSERT_TRUE(handle.IsValid());
            EXPECT_EQ(handle.m_Index, static_cast<u32>(i))
                << "slots follow (owner, slot, source) order, not extraction order";
            EXPECT_EQ(handle.m_Generation, 1u);
            const GPUSceneMaterial* record = scene.GetMaterialRecord(handle);
            ASSERT_NE(record, nullptr);
            EXPECT_EQ(record->StableIndex, handle.m_Index);
            EXPECT_EQ(record->Generation, handle.m_Generation);
            EXPECT_NE(record->Flags & GPUSceneMaterialFlagActive, 0u);
            handles.push_back(handle);
        }

        const GPUSceneFrameUpdate second = extract({ imported0, defaultKey, override0, imported1 });
        EXPECT_TRUE(second.m_MaterialDirtyRanges.empty()) << "an unchanged material is not re-uploaded";
        for (sizet i = 0, count = keyOrder.size(); i < count; ++i)
        {
            EXPECT_EQ(scene.FindMaterial(keyOrder[i]), handles[i]);
        }
    }

    TEST(GPUScene, CompatibleMaterialEditDirtiesOnlyItsSlotAndKeepsTheHandle)
    {
        GPUScene scene;
        const GPUSceneMaterialKey keyA = ImportedMaterial(1);
        const GPUSceneMaterialKey keyB = ImportedMaterial(2);
        const GPUSceneMaterialKey keyC = ImportedMaterial(3);
        const GPUSceneInstanceKey instanceKey{ .m_EntityId = 10, .m_Geometry = kGeometryKey };
        GPUSceneMaterialInput materialB;
        materialB.m_Albedo = Texture(5, 1, 7);

        auto extract = [&]() -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            scene.ExtractGeometry(kGeometryKey, kGeometry);
            scene.ExtractMaterial(keyA, GPUSceneMaterialInput{});
            scene.ExtractMaterial(keyB, materialB);
            scene.ExtractMaterial(keyC, GPUSceneMaterialInput{});
            scene.ExtractInstance(instanceKey, GPUSceneInstanceInput{ .m_Material = keyB });
            return scene.EndExtraction();
        };

        (void)extract();
        const GPUSceneHandle handleB = scene.FindMaterial(keyB);
        ASSERT_TRUE(handleB.IsValid());
        EXPECT_EQ(handleB.m_Index, 1u);

        // Called after every edit of materialB below: slot 1 is dirty, nothing
        // else moves, and the record is the encoder's output for the new input.
        const auto expectCompatible = [&](const char* edit)
        {
            SCOPED_TRACE(edit);
            const GPUSceneFrameUpdate update = extract();
            EXPECT_TRUE(DirtyRangesAre(update.m_MaterialDirtyRanges, { { 1, 1 } }));
            EXPECT_TRUE(update.m_InstanceDirtyRanges.empty())
                << "the generation is unchanged, so the instance's MaterialGeneration is too";
            EXPECT_EQ(scene.FindMaterial(keyB), handleB);
            const GPUSceneMaterial* record = scene.GetMaterialRecord(handleB);
            ASSERT_NE(record, nullptr);
            EXPECT_TRUE(Math::BitwiseEqual(
                *record, EncodeGPUSceneMaterial(materialB, handleB.m_Index, handleB.m_Generation)));
        };
        materialB.m_BaseColorFactor = glm::vec4(0.2f, 0.4f, 0.6f, 1.0f);
        expectCompatible("base colour factor");
        materialB.m_EmissiveFactor = glm::vec4(1.0f, 0.5f, 0.0f, 0.0f);
        expectCompatible("emissive factor");
        materialB.m_MetallicFactor = 0.75f;
        materialB.m_RoughnessFactor = 0.25f;
        expectCompatible("metallic and roughness");
        materialB.m_Shininess = 64.0f;
        expectCompatible("legacy shininess");
        materialB.m_AlphaCutoff = 0.3f;
        expectCompatible("alpha cutoff");
        materialB.m_Flags |= GPUSceneMaterialFlagTwoSided;
        expectCompatible("two-sided flag");
        materialB.m_Flags |= GPUSceneMaterialFlagDisableShadowCasting;
        expectCompatible("shadow-casting flag");
        materialB.m_Albedo.m_HeapOffset = 9;
        expectCompatible("albedo heap offset");

        const GPUSceneMaterial* record = scene.GetMaterialRecord(handleB);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->Generation, 1u);
        EXPECT_EQ(record->AlbedoHeapOffset, 9u) << "a heap re-resolve is stored without touching identity";
        EXPECT_NE(record->Flags & GPUSceneMaterialFlagAlbedoMap, 0u);
        EXPECT_TRUE(extract().m_MaterialDirtyRanges.empty());

        // An invalid texture handle encodes as absent whatever heap offset rides
        // with it, so this is not even a dirty record.
        materialB.m_Normal = Texture(RHI::ResourceHandle::InvalidIndex, 0, 3);
        EXPECT_TRUE(extract().m_MaterialDirtyRanges.empty());
        record = scene.GetMaterialRecord(handleB);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->NormalHeapOffset, GPUSceneHeapOffsetUnresolved);
        EXPECT_EQ(record->Flags & GPUSceneMaterialFlagNormalMap, 0u);
    }

    TEST(GPUScene, IncompatibleMaterialEditAdvancesGenerationInPlaceAndStalesTheInstance)
    {
        GPUScene scene;
        const GPUSceneMaterialKey keyA = ImportedMaterial(1);
        const GPUSceneMaterialKey keyB = ImportedMaterial(2);
        const GPUSceneInstanceKey instanceKey{ .m_EntityId = 10, .m_Geometry = kGeometryKey };
        GPUSceneMaterialInput materialB;

        auto extract = [&]() -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            scene.ExtractGeometry(kGeometryKey, kGeometry);
            scene.ExtractMaterial(keyA, GPUSceneMaterialInput{});
            scene.ExtractMaterial(keyB, materialB);
            scene.ExtractInstance(instanceKey, GPUSceneInstanceInput{ .m_Material = keyB });
            return scene.EndExtraction();
        };

        (void)extract();
        const GPUSceneHandle handleA = scene.FindMaterial(keyA);
        const GPUSceneHandle instanceHandle = scene.FindInstance(instanceKey);
        ASSERT_TRUE(handleA.IsValid());
        ASSERT_TRUE(instanceHandle.IsValid());
        const GPUSceneInstance* instance = scene.GetInstanceRecord(instanceHandle);
        ASSERT_NE(instance, nullptr);
        EXPECT_EQ(instance->MaterialIndex, 1u);
        EXPECT_EQ(instance->MaterialGeneration, 1u);

        // Called after every edit of materialB below: same slot, next generation,
        // the old handle dead, and the instance re-uploaded with the new generation.
        u32 expectedGeneration = 1;
        const auto expectIncompatible = [&](const char* edit)
        {
            SCOPED_TRACE(edit);
            const GPUSceneHandle before = scene.FindMaterial(keyB);
            const GPUSceneFrameUpdate update = extract();
            const GPUSceneHandle after = scene.FindMaterial(keyB);
            ++expectedGeneration;

            EXPECT_EQ(after.m_Index, before.m_Index) << "an incompatible edit keeps the slot";
            EXPECT_EQ(after.m_Generation, expectedGeneration);
            EXPECT_FALSE(scene.IsMaterialHandleLive(before));
            EXPECT_TRUE(scene.IsMaterialHandleLive(after));
            EXPECT_EQ(scene.GetMaterialRecord(before), nullptr);
            const GPUSceneMaterial* record = scene.GetMaterialRecord(after);
            ASSERT_NE(record, nullptr);
            EXPECT_EQ(record->Generation, after.m_Generation);
            EXPECT_TRUE(Math::BitwiseEqual(
                *record, EncodeGPUSceneMaterial(materialB, after.m_Index, after.m_Generation)));
            EXPECT_TRUE(DirtyRangesAre(update.m_MaterialDirtyRanges, { { 1, 1 } }));

            EXPECT_TRUE(DirtyRangesAre(update.m_InstanceDirtyRanges, { { 0, 1 } }))
                << "only MaterialGeneration changed, and that must re-upload the instance";
            EXPECT_EQ(scene.FindInstance(instanceKey), instanceHandle) << "the instance's own identity survives";
            instance = scene.GetInstanceRecord(instanceHandle);
            ASSERT_NE(instance, nullptr);
            EXPECT_EQ(instance->MaterialIndex, after.m_Index);
            EXPECT_EQ(instance->MaterialGeneration, after.m_Generation);
            EXPECT_EQ(scene.FindMaterial(keyA), handleA) << "the neighbouring slot is untouched";
        };
        materialB.m_ClosureVersion = 2;
        expectIncompatible("closure version");
        materialB.m_Albedo = Texture(5, 1, 7);
        expectIncompatible("albedo texture added");
        materialB.m_Albedo.m_Handle.Generation = 2;
        expectIncompatible("albedo texture re-created (RHI generation only)");
        materialB.m_AlphaMode = 1;
        expectIncompatible("alpha mode");
        materialB.m_Flags ^= GPUSceneMaterialFlagPBR;
        expectIncompatible("PBR classification flag");
    }

    TEST(GPUScene, MaterialRemovalRetiresTheSlotBeforeReuse)
    {
        GPUScene scene;
        const GPUSceneMaterialKey key4 = ImportedMaterial(4);
        const GPUSceneMaterialKey key5 = ImportedMaterial(5);
        const GPUSceneMaterialKey key10 = ImportedMaterial(10);
        const GPUSceneMaterialKey key20 = ImportedMaterial(20);

        auto extract = [&](std::initializer_list<GPUSceneMaterialKey> keys) -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            for (const GPUSceneMaterialKey& key : keys)
            {
                scene.ExtractMaterial(key, GPUSceneMaterialInput{});
            }
            return scene.EndExtraction();
        };

        (void)extract({ key20, key10 }); // frame 1
        const GPUSceneHandle first = scene.FindMaterial(key10);
        const GPUSceneHandle second = scene.FindMaterial(key20);
        ASSERT_TRUE(first.IsValid());
        ASSERT_TRUE(second.IsValid());
        EXPECT_EQ(first.m_Index, 0u);
        EXPECT_EQ(second.m_Index, 1u);

        (void)extract({ key20, key10 }); // frame 2
        EXPECT_EQ(scene.FindMaterial(key10), first);

        // Frame 3: key10 is gone. Its slot is retired until frame 3 + RetirementFrameCount = 5.
        const GPUSceneFrameUpdate removal = extract({ key20 });
        EXPECT_TRUE(DirtyRangesAre(removal.m_MaterialDirtyRanges, { { 0, 1 } })) << "the tombstone must be uploaded";
        EXPECT_FALSE(scene.IsMaterialHandleLive(first));
        EXPECT_EQ(scene.GetMaterialRecord(first), nullptr);
        EXPECT_FALSE(scene.FindMaterial(key10).IsValid());
        EXPECT_EQ(removal.m_Stats.m_Materials.m_Live, 1u);
        EXPECT_EQ(removal.m_Stats.m_Materials.m_SlotCount, 2u);
        EXPECT_EQ(removal.m_Stats.m_Materials.m_RetiredSlots, 1u);
        EXPECT_EQ(removal.m_Stats.m_Materials.m_FreeSlots, 0u);

        // Frame 4: still retired, so a new key appends.
        const GPUSceneFrameUpdate beforeReady = extract({ key5, key20 });
        EXPECT_EQ(scene.FindMaterial(key5).m_Index, 2u)
            << "a retired slot is not handed out while a buffered frame may read it";
        EXPECT_EQ(beforeReady.m_Stats.m_Materials.m_RetiredSlots, 1u);
        EXPECT_FALSE(scene.IsMaterialHandleLive(first));

        // Frame 5: slot 0 is free again and key4, which sorts first, takes it with the next generation.
        const GPUSceneFrameUpdate reuse = extract({ key4, key5, key20 });
        const GPUSceneHandle reused = scene.FindMaterial(key4);
        EXPECT_EQ(reused.m_Index, first.m_Index);
        EXPECT_EQ(reused.m_Generation, first.m_Generation + 1u);
        EXPECT_FALSE(scene.IsMaterialHandleLive(first));
        EXPECT_TRUE(scene.IsMaterialHandleLive(reused));
        const GPUSceneMaterial* record = scene.GetMaterialRecord(reused);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->Generation, reused.m_Generation);
        EXPECT_EQ(reuse.m_Stats.m_Materials.m_RetiredSlots, 0u);
        EXPECT_EQ(reuse.m_Stats.m_Materials.m_FreeSlots, 0u);
        EXPECT_EQ(reuse.m_Stats.m_Materials.m_SlotCount, 3u);
    }

    TEST(GPUScene, InstanceWithoutAnExtractedMaterialCarriesNoMaterialSlot)
    {
        GPUScene scene;
        const GPUSceneMaterialKey extracted = ImportedMaterial(1);
        const GPUSceneMaterialKey never = ImportedMaterial(2);
        const GPUSceneInstanceKey withMaterial{ .m_EntityId = 10, .m_Geometry = kGeometryKey };
        const GPUSceneInstanceKey withoutMaterial{ .m_EntityId = 11, .m_Geometry = kGeometryKey };

        scene.BeginExtraction(1, glm::vec3(0.0f));
        scene.ExtractGeometry(kGeometryKey, kGeometry);
        scene.ExtractMaterial(extracted, GPUSceneMaterialInput{});
        scene.ExtractInstance(withMaterial, GPUSceneInstanceInput{ .m_Material = extracted });
        scene.ExtractInstance(withoutMaterial, GPUSceneInstanceInput{ .m_Material = never });
        (void)scene.EndExtraction();

        const GPUSceneInstance* resolved = scene.GetInstanceRecord(scene.FindInstance(withMaterial));
        ASSERT_NE(resolved, nullptr);
        EXPECT_EQ(resolved->MaterialIndex, scene.FindMaterial(extracted).m_Index);
        EXPECT_EQ(resolved->MaterialGeneration, scene.FindMaterial(extracted).m_Generation);

        // A key nobody extracted this frame is not an error: the instance carries
        // the invalid slot and generation 0, which no material record can match.
        const GPUSceneInstance* unresolved = scene.GetInstanceRecord(scene.FindInstance(withoutMaterial));
        ASSERT_NE(unresolved, nullptr);
        EXPECT_EQ(unresolved->MaterialIndex, GPUSceneHandle::InvalidIndex);
        EXPECT_EQ(unresolved->MaterialGeneration, 0u);
        EXPECT_FALSE(scene.FindMaterial(never).IsValid());
    }

    TEST(GPUScene, IsMaterialStagedIsTrueOnlyInsideTheExtractionWindow)
    {
        GPUScene scene;
        const GPUSceneMaterialKey staged = ImportedMaterial(1);
        const GPUSceneMaterialKey other = ImportedMaterial(2);
        EXPECT_FALSE(scene.IsMaterialStaged(staged)) << "nothing is staged before the first BeginExtraction";

        scene.BeginExtraction(1, glm::vec3(0.0f));
        EXPECT_FALSE(scene.IsMaterialStaged(staged));
        scene.ExtractMaterial(staged, GPUSceneMaterialInput{});
        EXPECT_TRUE(scene.IsMaterialStaged(staged));
        EXPECT_FALSE(scene.IsMaterialStaged(other));
        (void)scene.EndExtraction();
        EXPECT_FALSE(scene.IsMaterialStaged(staged)) << "staging ends with the frame";
        EXPECT_TRUE(scene.FindMaterial(staged).IsValid()) << "... although the material is live";

        scene.BeginExtraction(1, glm::vec3(0.0f));
        EXPECT_FALSE(scene.IsMaterialStaged(staged)) << "a live material is not staged until this frame extracts it";
        scene.ExtractMaterial(staged, GPUSceneMaterialInput{});
        EXPECT_TRUE(scene.IsMaterialStaged(staged));
        (void)scene.EndExtraction();
    }

    TEST(GPUScene, EveryMaterialTextureLaneIsPartOfIdentity)
    {
        // Each of the six texture lanes is a swap when it changes. A lane the
        // encoder carries but the identity projection ignores would keep the
        // generation and let temporal history survive a texture change.
        constexpr std::array<GPUSceneTextureRef GPUSceneMaterialInput::*, 6> kLanes{
            &GPUSceneMaterialInput::m_Albedo,
            &GPUSceneMaterialInput::m_MetallicRoughness,
            &GPUSceneMaterialInput::m_Normal,
            &GPUSceneMaterialInput::m_Occlusion,
            &GPUSceneMaterialInput::m_Emissive,
            &GPUSceneMaterialInput::m_Specular,
        };
        static_assert(GPUSceneMaterialTextureIdentity(GPUSceneMaterial{}).size() == 2 * kLanes.size(),
                      "one index and one generation per lane");

        GPUScene scene;
        const GPUSceneMaterialKey key = ImportedMaterial(1);
        GPUSceneMaterialInput material;
        auto extract = [&]() -> GPUSceneHandle
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            scene.ExtractMaterial(key, material);
            (void)scene.EndExtraction();
            return scene.FindMaterial(key);
        };

        GPUSceneHandle previous = extract();
        u32 textureIndex = 100;
        for (sizet lane = 0; lane < kLanes.size(); ++lane)
        {
            SCOPED_TRACE(::testing::Message() << "texture lane " << lane);
            material.*kLanes[lane] = Texture(textureIndex++, 1, 7);
            const GPUSceneHandle next = extract();
            EXPECT_EQ(next.m_Index, previous.m_Index) << "a texture swap keeps the slot";
            EXPECT_EQ(next.m_Generation, previous.m_Generation + 1u) << "... and advances the generation";
            EXPECT_FALSE(scene.IsMaterialHandleLive(previous));
            previous = next;
        }
    }
} // namespace OloEngine::Tests
