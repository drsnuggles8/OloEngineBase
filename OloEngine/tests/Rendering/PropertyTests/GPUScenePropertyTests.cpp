// OLO_TEST_LAYER: L1

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/GPUScene/GPUScene.h"

namespace OloEngine::Tests
{
    TEST(GPUScene, AllocationPoliciesGrowGeometricallyAndNeverWrapGeneration)
    {
        EXPECT_EQ(GPUSceneAllocationPolicy::GrowCapacity(0, 1), 1u);
        EXPECT_EQ(GPUSceneAllocationPolicy::GrowCapacity(64, 65), 128u);
        EXPECT_EQ(GPUSceneAllocationPolicy::GrowCapacity(128, 65), 128u);

        constexpr u32 maxGeneration = std::numeric_limits<u32>::max();
        EXPECT_EQ(GPUSceneAllocationPolicy::NextGeneration(maxGeneration - 1u), maxGeneration);
        EXPECT_EQ(GPUSceneAllocationPolicy::NextGeneration(maxGeneration), 0u)
            << "generation rollover must retire the slot, never resurrect a stale handle";
        EXPECT_EQ(GPUSceneAllocationPolicy::RetirementReadyFrame(10), 12u);
        EXPECT_EQ(GPUSceneAllocationPolicy::RetirementReadyFrame(maxGeneration),
                  static_cast<u64>(maxGeneration) + GPUSceneAllocationPolicy::RetirementFrameCount);
    }

    TEST(GPUScene, InstanceHandlesStayStableAndRejectReusedSlotsAfterFrameSafeRetirement)
    {
        GPUScene scene;
        const GPUSceneGeometryKey geometryKey{ .m_VertexBuffer = 11, .m_IndexBuffer = 12, .m_SubmeshIndex = 0 };
        const GPUSceneGeometryInput geometry{ .m_VertexBuffer = RHI::ResourceHandle{ 1, 1 },
                                              .m_IndexBuffer = RHI::ResourceHandle{ 2, 1 },
                                              .m_IndexCount = 36,
                                              .m_VertexCount = 24 };
        const GPUSceneInstanceKey key4{ .m_EntityId = 4, .m_Geometry = geometryKey };
        const GPUSceneInstanceKey key5{ .m_EntityId = 5, .m_Geometry = geometryKey };
        const GPUSceneInstanceKey key10{ .m_EntityId = 10, .m_Geometry = geometryKey };
        const GPUSceneInstanceKey key20{ .m_EntityId = 20, .m_Geometry = geometryKey };

        scene.BeginExtraction(1, glm::vec3(0.0f));
        scene.ExtractGeometry(geometryKey, geometry);
        scene.ExtractInstance(key20, GPUSceneInstanceInput{});
        scene.ExtractInstance(key10, GPUSceneInstanceInput{});
        (void)scene.EndExtraction();

        const GPUSceneHandle first = scene.FindInstance(key10);
        const GPUSceneHandle second = scene.FindInstance(key20);
        ASSERT_TRUE(first.IsValid());
        ASSERT_TRUE(second.IsValid());
        EXPECT_EQ(first.m_Index, 0u) << "new slots must be allocated in stable-key order";
        EXPECT_EQ(second.m_Index, 1u);

        scene.BeginExtraction(1, glm::vec3(0.0f));
        scene.ExtractGeometry(geometryKey, geometry);
        scene.ExtractInstance(key20, GPUSceneInstanceInput{});
        scene.ExtractInstance(key10, GPUSceneInstanceInput{});
        (void)scene.EndExtraction();
        EXPECT_EQ(scene.FindInstance(key10), first);
        EXPECT_EQ(scene.FindInstance(key20), second);

        scene.BeginExtraction(1, glm::vec3(0.0f));
        scene.ExtractGeometry(geometryKey, geometry);
        scene.ExtractInstance(key20, GPUSceneInstanceInput{});
        (void)scene.EndExtraction();
        EXPECT_FALSE(scene.IsInstanceHandleLive(first));

        scene.BeginExtraction(1, glm::vec3(0.0f));
        scene.ExtractGeometry(geometryKey, geometry);
        scene.ExtractInstance(key5, GPUSceneInstanceInput{});
        scene.ExtractInstance(key20, GPUSceneInstanceInput{});
        (void)scene.EndExtraction();

        const GPUSceneHandle beforeRetirementCompletes = scene.FindInstance(key5);
        EXPECT_NE(beforeRetirementCompletes.m_Index, first.m_Index);
        EXPECT_EQ(scene.GetLastFrameUpdate().m_Stats.m_Instances.m_RetiredSlots, 1u);
        EXPECT_FALSE(scene.IsInstanceHandleLive(first));

        scene.BeginExtraction(1, glm::vec3(0.0f));
        scene.ExtractGeometry(geometryKey, geometry);
        scene.ExtractInstance(key4, GPUSceneInstanceInput{});
        scene.ExtractInstance(key5, GPUSceneInstanceInput{});
        scene.ExtractInstance(key20, GPUSceneInstanceInput{});
        (void)scene.EndExtraction();

        const GPUSceneHandle reused = scene.FindInstance(key4);
        EXPECT_EQ(reused.m_Index, first.m_Index);
        EXPECT_NE(reused.m_Generation, first.m_Generation);
        EXPECT_FALSE(scene.IsInstanceHandleLive(first));
        EXPECT_TRUE(scene.IsInstanceHandleLive(reused));
    }

    TEST(GPUScene, OneEntityCanOwnStableInstancesOfDistinctGeometry)
    {
        GPUScene scene;
        const GPUSceneGeometryKey geometryA{ .m_VertexBuffer = 11, .m_IndexBuffer = 12 };
        const GPUSceneGeometryKey geometryB{ .m_VertexBuffer = 21, .m_IndexBuffer = 22 };
        const GPUSceneGeometryInput inputA{ .m_VertexBuffer = RHI::ResourceHandle{ 1, 1 },
                                            .m_IndexBuffer = RHI::ResourceHandle{ 2, 1 },
                                            .m_IndexCount = 3 };
        const GPUSceneGeometryInput inputB{ .m_VertexBuffer = RHI::ResourceHandle{ 3, 1 },
                                            .m_IndexBuffer = RHI::ResourceHandle{ 4, 1 },
                                            .m_IndexCount = 6 };
        const GPUSceneInstanceKey instanceA{ .m_EntityId = 10, .m_Geometry = geometryA };
        const GPUSceneInstanceKey instanceB{ .m_EntityId = 10, .m_Geometry = geometryB };

        auto extract = [&](bool reverse)
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            scene.ExtractGeometry(geometryA, inputA);
            scene.ExtractGeometry(geometryB, inputB);
            if (reverse)
            {
                scene.ExtractInstance(instanceB, GPUSceneInstanceInput{});
                scene.ExtractInstance(instanceA, GPUSceneInstanceInput{});
            }
            else
            {
                scene.ExtractInstance(instanceA, GPUSceneInstanceInput{});
                scene.ExtractInstance(instanceB, GPUSceneInstanceInput{});
            }
            (void)scene.EndExtraction();
        };

        extract(false);
        const GPUSceneHandle handleA = scene.FindInstance(instanceA);
        const GPUSceneHandle handleB = scene.FindInstance(instanceB);
        ASSERT_TRUE(handleA.IsValid());
        ASSERT_TRUE(handleB.IsValid());
        EXPECT_NE(handleA, handleB);

        extract(true);
        EXPECT_EQ(scene.FindInstance(instanceA), handleA);
        EXPECT_EQ(scene.FindInstance(instanceB), handleB);
    }

    TEST(GPUScene, UnsupportedSubmissionsAreCountedByReason)
    {
        GPUScene scene;
        scene.BeginExtraction(1, glm::vec3(0.0f));
        scene.ReportUnsupported(GPUSceneUnsupportedCategory::Virtualized, 2);
        scene.ReportUnsupported(GPUSceneUnsupportedCategory::SoftwareRaster, 2);
        scene.ReportUnsupported(GPUSceneUnsupportedCategory::Skinned, 3);
        scene.ReportUnsupported(GPUSceneUnsupportedCategory::LegacyModel, 1);
        scene.ReportUnsupported(GPUSceneUnsupportedCategory::LegacySubmesh, 1);
        scene.ReportUnsupported(GPUSceneUnsupportedCategory::Tiles, 4);
        scene.ReportUnsupported(GPUSceneUnsupportedCategory::Cloth, 1);

        const GPUSceneFrameUpdate update = scene.EndExtraction();
        EXPECT_EQ(update.m_Stats.m_UnsupportedTotal, 14u);
        EXPECT_EQ(update.m_Stats.m_UnsupportedCounts[static_cast<sizet>(
                      GPUSceneUnsupportedCategory::Virtualized)],
                  2u);
        EXPECT_EQ(update.m_Stats.m_UnsupportedCounts[static_cast<sizet>(
                      GPUSceneUnsupportedCategory::SoftwareRaster)],
                  2u);
        EXPECT_EQ(update.m_Stats.m_UnsupportedCounts[static_cast<sizet>(
                      GPUSceneUnsupportedCategory::Skinned)],
                  3u);
        EXPECT_EQ(update.m_Stats.m_UnsupportedCounts[static_cast<sizet>(
                      GPUSceneUnsupportedCategory::Tiles)],
                  4u);
        EXPECT_STREQ(GetGPUSceneUnsupportedCategoryName(GPUSceneUnsupportedCategory::SoftwareRaster),
                     "Software raster");
        EXPECT_STREQ(GetGPUSceneUnsupportedCategoryName(GPUSceneUnsupportedCategory::LegacyModel),
                     "Legacy model");
    }

    TEST(GPUScene, OwnerChangeInvalidatesTemporalIdentityEvenForTheSameKeys)
    {
        GPUScene scene;
        const GPUSceneGeometryKey geometryKey{ .m_VertexBuffer = 11, .m_IndexBuffer = 12 };
        const GPUSceneGeometryInput geometry{
            .m_VertexBuffer = RHI::ResourceHandle{ 1, 1 },
            .m_IndexBuffer = RHI::ResourceHandle{ 2, 1 },
            .m_IndexCount = 3,
            .m_VertexCount = 3,
        };
        const GPUSceneInstanceKey instanceKey{ .m_EntityId = 10, .m_Geometry = geometryKey };

        scene.BeginExtraction(0, glm::vec3(0.0f));
        scene.ExtractGeometry(geometryKey, geometry);
        scene.ExtractInstance(instanceKey, GPUSceneInstanceInput{});
        (void)scene.EndExtraction();
        const GPUSceneHandle firstOwner = scene.FindInstance(instanceKey);

        scene.BeginExtraction(200, glm::vec3(0.0f));
        scene.ExtractGeometry(geometryKey, geometry);
        scene.ExtractInstance(instanceKey, GPUSceneInstanceInput{});
        const GPUSceneFrameUpdate replacement = scene.EndExtraction();
        const GPUSceneHandle secondOwner = scene.FindInstance(instanceKey);

        EXPECT_NE(secondOwner.m_Index, firstOwner.m_Index)
            << "an owner reset must not recycle a slot still visible to a buffered frame";
        EXPECT_NE(secondOwner, firstOwner);
        EXPECT_FALSE(scene.IsInstanceHandleLive(firstOwner));
        EXPECT_TRUE(scene.IsInstanceHandleLive(secondOwner));
        ASSERT_EQ(replacement.m_InstanceDirtyRanges.size(), 1u);
        EXPECT_EQ(replacement.m_InstanceDirtyRanges[0], (GPUSceneDirtyRange{ 0, 2 }));
    }

    TEST(GPUScene, RecordsMatchStd430AndOwnCameraRelativeTransformHistory)
    {
        static_assert(sizeof(GPUSceneInstance) == 128);
        static_assert(offsetof(GPUSceneInstance, CurrentTransform) == 0);
        static_assert(offsetof(GPUSceneInstance, PreviousTransform) == 48);
        static_assert(offsetof(GPUSceneInstance, GeometryIndex) == 96);
        static_assert(offsetof(GPUSceneInstance, VisibilityMask) == 112);
        static_assert(sizeof(GPUSceneGeometry) == 64);
        static_assert(offsetof(GPUSceneGeometry, VertexAddress) == 16);
        static_assert(offsetof(GPUSceneGeometry, VertexFormat) == 32);
        static_assert(offsetof(GPUSceneGeometry, BaseVertex) == 48);

        GPUScene scene;
        const GPUSceneGeometryKey geometryKey{ .m_VertexBuffer = 11, .m_IndexBuffer = 12 };
        const GPUSceneGeometryInput geometry{ .m_VertexBuffer = RHI::ResourceHandle{ 1, 2 },
                                              .m_IndexBuffer = RHI::ResourceHandle{ 3, 4 },
                                              .m_VertexAddress = 0x1000,
                                              .m_IndexAddress = 0x2000,
                                              .m_VertexFormat = 7,
                                              .m_IndexFormat = 32,
                                              .m_FirstIndex = 6,
                                              .m_IndexCount = 36,
                                              .m_BaseVertex = -2,
                                              .m_VertexCount = 24 };
        const GPUSceneInstanceKey instanceKey{ .m_EntityId = 10, .m_Geometry = geometryKey };

        auto extract = [&](f32 worldX, f32 originX) -> GPUSceneFrameUpdate
        {
            GPUSceneInstanceInput instance;
            instance.m_WorldTransform[3].x = worldX;
            scene.BeginExtraction(1, glm::vec3(originX, 0.0f, 0.0f));
            scene.ExtractGeometry(geometryKey, geometry);
            scene.ExtractInstance(instanceKey, instance);
            return scene.EndExtraction();
        };

        const auto firstUpdate = extract(1010.0f, 1000.0f);
        ASSERT_EQ(firstUpdate.m_InstanceDirtyRanges.size(), 1u);
        EXPECT_EQ(firstUpdate.m_InstanceDirtyRanges[0], (GPUSceneDirtyRange{ 0, 1 }));

        const GPUSceneHandle handle = scene.FindInstance(instanceKey);
        const GPUSceneInstance* first = scene.GetInstanceRecord(handle);
        ASSERT_NE(first, nullptr);
        EXPECT_FLOAT_EQ(first->CurrentTransform.Row0.w, 10.0f);
        EXPECT_FLOAT_EQ(first->PreviousTransform.Row0.w, 10.0f);
        EXPECT_EQ(first->GeometryIndex, 0u);
        EXPECT_EQ(first->GeometryGeneration, 1u);
        EXPECT_EQ(first->StableIndex, handle.m_Index);
        EXPECT_EQ(first->Generation, handle.m_Generation);

        extract(1020.0f, 1000.0f);
        const GPUSceneInstance* moved = scene.GetInstanceRecord(handle);
        ASSERT_NE(moved, nullptr);
        EXPECT_FLOAT_EQ(moved->CurrentTransform.Row0.w, 20.0f);
        EXPECT_FLOAT_EQ(moved->PreviousTransform.Row0.w, 10.0f);

        extract(1020.0f, 1010.0f);
        const GPUSceneInstance* rebased = scene.GetInstanceRecord(handle);
        ASSERT_NE(rebased, nullptr);
        EXPECT_FLOAT_EQ(rebased->CurrentTransform.Row0.w, 10.0f);
        EXPECT_FLOAT_EQ(rebased->PreviousTransform.Row0.w, 10.0f)
            << "both temporal transforms must use this frame's render origin";

        const auto unchangedUpdate = extract(1020.0f, 1010.0f);
        EXPECT_TRUE(unchangedUpdate.m_InstanceDirtyRanges.empty());
        EXPECT_TRUE(unchangedUpdate.m_GeometryDirtyRanges.empty());

        const GPUSceneGeometry* geometryRecord = scene.GetGeometryRecord(scene.FindGeometry(geometryKey));
        ASSERT_NE(geometryRecord, nullptr);
        EXPECT_EQ(geometryRecord->VertexBufferIndex, 1u);
        EXPECT_EQ(geometryRecord->VertexBufferGeneration, 2u);
        EXPECT_EQ(geometryRecord->IndexBufferIndex, 3u);
        EXPECT_EQ(geometryRecord->IndexBufferGeneration, 4u);
        EXPECT_EQ(geometryRecord->VertexAddress, 0x1000u);
        EXPECT_EQ(geometryRecord->IndexAddress, 0x2000u);
        EXPECT_EQ(geometryRecord->FirstIndex, 6u);
        EXPECT_EQ(geometryRecord->IndexCount, 36u);
        EXPECT_EQ(geometryRecord->BaseVertex, -2);
        EXPECT_EQ(geometryRecord->Generation, scene.FindGeometry(geometryKey).m_Generation);
    }

    TEST(GPUScene, DirtyRangesArePartialAndResetInvalidatesBothRegistries)
    {
        GPUScene scene;
        const GPUSceneGeometryKey geometryKey{ .m_VertexBuffer = 11, .m_IndexBuffer = 12 };
        const GPUSceneGeometryInput geometry{ .m_VertexBuffer = RHI::ResourceHandle{ 1, 1 },
                                              .m_IndexBuffer = RHI::ResourceHandle{ 2, 1 },
                                              .m_IndexCount = 3,
                                              .m_VertexCount = 3 };

        auto extract = [&](f32 middleX) -> GPUSceneFrameUpdate
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            scene.ExtractGeometry(geometryKey, geometry);
            for (u64 entityId : { 10u, 20u, 30u })
            {
                GPUSceneInstanceInput instance;
                instance.m_WorldTransform[3].x = entityId == 20 ? middleX : 0.0f;
                scene.ExtractInstance(
                    GPUSceneInstanceKey{ .m_EntityId = entityId, .m_Geometry = geometryKey }, instance);
            }
            return scene.EndExtraction();
        };

        const auto initial = extract(0.0f);
        ASSERT_EQ(initial.m_InstanceDirtyRanges.size(), 1u);
        EXPECT_EQ(initial.m_InstanceDirtyRanges[0], (GPUSceneDirtyRange{ 0, 3 }));
        ASSERT_EQ(initial.m_GeometryDirtyRanges.size(), 1u);
        EXPECT_EQ(initial.m_GeometryDirtyRanges[0], (GPUSceneDirtyRange{ 0, 1 }));

        const auto stable = extract(0.0f);
        EXPECT_TRUE(stable.m_InstanceDirtyRanges.empty());
        EXPECT_TRUE(stable.m_GeometryDirtyRanges.empty());

        const auto partial = extract(5.0f);
        ASSERT_EQ(partial.m_InstanceDirtyRanges.size(), 1u);
        EXPECT_EQ(partial.m_InstanceDirtyRanges[0], (GPUSceneDirtyRange{ 1, 1 }));

        const GPUSceneInstanceKey firstInstanceKey{ .m_EntityId = 10, .m_Geometry = geometryKey };
        const GPUSceneHandle staleInstance = scene.FindInstance(firstInstanceKey);
        const GPUSceneHandle staleGeometry = scene.FindGeometry(geometryKey);
        scene.Reset();
        EXPECT_FALSE(scene.IsInstanceHandleLive(staleInstance));
        EXPECT_FALSE(scene.IsGeometryHandleLive(staleGeometry));
        ASSERT_EQ(scene.GetLastFrameUpdate().m_InstanceDirtyRanges.size(), 1u);
        EXPECT_EQ(scene.GetLastFrameUpdate().m_InstanceDirtyRanges[0], (GPUSceneDirtyRange{ 0, 3 }));
        ASSERT_EQ(scene.GetLastFrameUpdate().m_GeometryDirtyRanges.size(), 1u);
        EXPECT_EQ(scene.GetLastFrameUpdate().m_GeometryDirtyRanges[0], (GPUSceneDirtyRange{ 0, 1 }));

        const auto afterReset = extract(0.0f);
        const GPUSceneHandle reusedInstance = scene.FindInstance(firstInstanceKey);
        const GPUSceneHandle reusedGeometry = scene.FindGeometry(geometryKey);
        EXPECT_NE(reusedInstance.m_Index, staleInstance.m_Index);
        EXPECT_NE(reusedGeometry.m_Index, staleGeometry.m_Index);
        EXPECT_EQ(afterReset.m_Stats.m_Instances.m_RetiredSlots, 3u);
        EXPECT_EQ(afterReset.m_Stats.m_Geometries.m_RetiredSlots, 1u);
        ASSERT_EQ(afterReset.m_InstanceDirtyRanges.size(), 1u);
        EXPECT_EQ(afterReset.m_InstanceDirtyRanges[0], (GPUSceneDirtyRange{ 0, 6 }));
        ASSERT_EQ(afterReset.m_GeometryDirtyRanges.size(), 1u);
        EXPECT_EQ(afterReset.m_GeometryDirtyRanges[0], (GPUSceneDirtyRange{ 0, 2 }));
    }
} // namespace OloEngine::Tests
