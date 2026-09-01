// OLO_TEST_LAYER: L3

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    TEST(GPUSceneGpu, GrowthPreservesBufferIdentityAndShutdownInvalidatesIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GPUScene scene;
        scene.InitializeGPU(/*initialInstanceCapacity=*/1, /*initialGeometryCapacity=*/1);
        const RHI::ResourceHandle instanceBufferBefore = scene.GetInstanceBufferHandle();
        const RHI::ResourceHandle geometryBufferBefore = scene.GetGeometryBufferHandle();
        ASSERT_TRUE(instanceBufferBefore.IsValid());
        ASSERT_TRUE(geometryBufferBefore.IsValid());

        scene.BeginExtraction(1, glm::vec3(0.0f));
        for (u32 index = 0; index < 2; ++index)
        {
            const GPUSceneGeometryKey geometryKey{
                .m_VertexBuffer = 10u + index,
                .m_IndexBuffer = 20u + index,
            };
            scene.ExtractGeometry(
                geometryKey,
                GPUSceneGeometryInput{
                    .m_VertexBuffer = RHI::ResourceHandle{ 10u + index, 1 },
                    .m_IndexBuffer = RHI::ResourceHandle{ 20u + index, 1 },
                    .m_IndexCount = 3,
                    .m_VertexCount = 3,
                });
            scene.ExtractInstance(
                GPUSceneInstanceKey{ .m_EntityId = 100u + index, .m_Geometry = geometryKey },
                GPUSceneInstanceInput{});
        }
        (void)scene.EndExtraction();
        scene.Upload();

        const GPUSceneFrameStats& grown = scene.GetLastFrameUpdate().m_Stats;
        EXPECT_EQ(grown.m_InstanceBufferCapacity, 2u);
        EXPECT_EQ(grown.m_GeometryBufferCapacity, 2u);
        EXPECT_EQ(grown.m_BufferGrowthEvents, 2u);
        EXPECT_EQ(grown.m_UploadBytes, 2u * sizeof(GPUSceneInstance) + 2u * sizeof(GPUSceneGeometry));
        EXPECT_EQ(scene.GetInstanceBufferHandle(), instanceBufferBefore);
        EXPECT_EQ(scene.GetGeometryBufferHandle(), geometryBufferBefore);
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(instanceBufferBefore));
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(geometryBufferBefore));

        scene.BeginExtraction(1, glm::vec3(0.0f));
        for (u32 index = 0; index < 2; ++index)
        {
            const GPUSceneGeometryKey steadyGeometryKey{
                .m_VertexBuffer = 10u + index,
                .m_IndexBuffer = 20u + index,
            };
            scene.ExtractGeometry(
                steadyGeometryKey,
                GPUSceneGeometryInput{
                    .m_VertexBuffer = RHI::ResourceHandle{ 10u + index, 1 },
                    .m_IndexBuffer = RHI::ResourceHandle{ 20u + index, 1 },
                    .m_IndexCount = 3,
                    .m_VertexCount = 3,
                });
            GPUSceneInstanceInput instance;
            if (index == 0)
            {
                instance.m_WorldTransform[3].x = 1.0f;
            }
            scene.ExtractInstance(
                GPUSceneInstanceKey{ .m_EntityId = 100u + index, .m_Geometry = steadyGeometryKey },
                instance);
        }
        (void)scene.EndExtraction();
        scene.Upload();
        const auto& partial = scene.GetLastFrameUpdate().m_Stats;
        EXPECT_EQ(partial.m_BufferGrowthEvents, 0u);
        EXPECT_EQ(partial.m_UploadBytes, sizeof(GPUSceneInstance))
            << "one moving instance must upload one merged record range, not both scene buffers";

        const GPUSceneGeometryKey firstGeometryKey{ .m_VertexBuffer = 10, .m_IndexBuffer = 20 };
        const GPUSceneInstanceKey firstInstanceKey{ .m_EntityId = 100, .m_Geometry = firstGeometryKey };

        // Retire the second records and upload their tombstones. A subsequent
        // shutdown only dirties the still-live first records, so InitializeGPU
        // must explicitly re-seed the already-tombstoned slots as well.
        scene.BeginExtraction(1, glm::vec3(0.0f));
        scene.ExtractGeometry(
            firstGeometryKey,
            GPUSceneGeometryInput{
                .m_VertexBuffer = RHI::ResourceHandle{ 10, 1 },
                .m_IndexBuffer = RHI::ResourceHandle{ 20, 1 },
                .m_IndexCount = 3,
                .m_VertexCount = 3,
            });
        scene.ExtractInstance(firstInstanceKey, GPUSceneInstanceInput{});
        (void)scene.EndExtraction();
        scene.Upload();

        const GPUSceneHandle staleInstance = scene.FindInstance(firstInstanceKey);
        scene.Shutdown();
        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(instanceBufferBefore));
        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(geometryBufferBefore));
        EXPECT_FALSE(scene.IsInstanceHandleLive(staleInstance));

        scene.InitializeGPU(1, 1);
        scene.BeginExtraction(1, glm::vec3(0.0f));
        const GPUSceneGeometryKey geometryKey{ .m_VertexBuffer = 10, .m_IndexBuffer = 20 };
        scene.ExtractGeometry(
            geometryKey,
            GPUSceneGeometryInput{
                .m_VertexBuffer = RHI::ResourceHandle{ 10, 1 },
                .m_IndexBuffer = RHI::ResourceHandle{ 20, 1 },
                .m_IndexCount = 3,
                .m_VertexCount = 3,
            });
        scene.ExtractInstance(GPUSceneInstanceKey{ .m_EntityId = 100, .m_Geometry = geometryKey },
                              GPUSceneInstanceInput{});
        const GPUSceneFrameUpdate restartedUpdate = scene.EndExtraction();

        ASSERT_EQ(restartedUpdate.m_InstanceDirtyRanges.size(), 1u);
        EXPECT_EQ(restartedUpdate.m_InstanceDirtyRanges[0], (GPUSceneDirtyRange{ 0, 3 }))
            << "restart extraction must retain full initialization, including old tombstones";
        ASSERT_EQ(restartedUpdate.m_GeometryDirtyRanges.size(), 1u);
        EXPECT_EQ(restartedUpdate.m_GeometryDirtyRanges[0], (GPUSceneDirtyRange{ 0, 3 }));

        const GPUSceneHandle restarted = scene.FindInstance(firstInstanceKey);
        EXPECT_NE(restarted.m_Index, staleInstance.m_Index)
            << "shutdown slots remain retired across the buffered-frame window";
        EXPECT_NE(restarted, staleInstance);
        EXPECT_FALSE(scene.IsInstanceHandleLive(staleInstance));
        scene.Shutdown();
    }
} // namespace OloEngine::Tests
