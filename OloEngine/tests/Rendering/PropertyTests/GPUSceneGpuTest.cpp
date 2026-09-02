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
        scene.InitializeGPU(GPUSceneCapacities{ .m_Instances = 1, .m_Geometries = 1 });
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
        EXPECT_EQ(grown.m_Instances.m_BufferCapacity, 2u);
        EXPECT_EQ(grown.m_Geometries.m_BufferCapacity, 2u);
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

        scene.InitializeGPU(GPUSceneCapacities{ .m_Instances = 1, .m_Geometries = 1 });
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

    // The material, light and environment registries (#992, #993) follow the
    // instance/geometry buffer contract: growth keeps the RHI identity,
    // m_BufferGrowthEvents counts every kind, the per-kind upload figures sum
    // to the total, a compatible edit to one record re-uploads that record
    // alone, and Shutdown drops all five buffers.
    TEST(GPUSceneGpu, MaterialLightEnvironmentBuffersGrowUploadIncrementallyAndShutDown)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GPUScene scene;
        scene.InitializeGPU(GPUSceneCapacities{
            .m_Instances = 1,
            .m_Geometries = 1,
            .m_Materials = 1,
            .m_Lights = 1,
            .m_Environments = 1,
        });
        const RHI::ResourceHandle instanceBufferBefore = scene.GetInstanceBufferHandle();
        const RHI::ResourceHandle geometryBufferBefore = scene.GetGeometryBufferHandle();
        const RHI::ResourceHandle materialBufferBefore = scene.GetMaterialBufferHandle();
        const RHI::ResourceHandle lightBufferBefore = scene.GetLightBufferHandle();
        const RHI::ResourceHandle environmentBufferBefore = scene.GetEnvironmentBufferHandle();
        ASSERT_TRUE(instanceBufferBefore.IsValid());
        ASSERT_TRUE(geometryBufferBefore.IsValid());
        ASSERT_TRUE(materialBufferBefore.IsValid());
        ASSERT_TRUE(lightBufferBefore.IsValid());
        ASSERT_TRUE(environmentBufferBefore.IsValid());
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(materialBufferBefore));
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(lightBufferBefore));
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(environmentBufferBefore));

        const GPUSceneGeometryKey geometryKey{ .m_VertexBuffer = 10, .m_IndexBuffer = 20 };
        const GPUSceneGeometryInput geometry{
            .m_VertexBuffer = RHI::ResourceHandle{ 10, 1 },
            .m_IndexBuffer = RHI::ResourceHandle{ 20, 1 },
            .m_IndexCount = 3,
            .m_VertexCount = 3,
        };
        const GPUSceneInstanceKey instanceKey{ .m_EntityId = 100, .m_Geometry = geometryKey };
        const GPUSceneMaterialKey firstMaterialKey{
            .m_Owner = 10, .m_Slot = 0, .m_Source = static_cast<u32>(GPUSceneMaterialSource::Imported)
        };
        const GPUSceneMaterialKey secondMaterialKey{
            .m_Owner = 10, .m_Slot = 1, .m_Source = static_cast<u32>(GPUSceneMaterialSource::Imported)
        };
        const GPUSceneLightKey pointLightKey{ .m_EntityId = 200, .m_Type = static_cast<u32>(GPUSceneLightType::Point) };
        const GPUSceneLightKey spotLightKey{ .m_EntityId = 201, .m_Type = static_cast<u32>(GPUSceneLightType::Spot) };
        const GPUSceneEnvironmentKey environmentKey{ .m_Owner = 0 };

        // One whole-scene frame. The first material's roughness and the point
        // light's position are the two compatible edits the later frames make;
        // every other input is byte-identical from frame to frame.
        const auto extractFrame = [&](f32 firstMaterialRoughness, const glm::vec3& pointLightPosition)
        {
            scene.BeginExtraction(1, glm::vec3(0.0f));
            scene.ExtractGeometry(geometryKey, geometry);

            GPUSceneMaterialInput firstMaterial;
            firstMaterial.m_RoughnessFactor = firstMaterialRoughness;
            scene.ExtractMaterial(firstMaterialKey, firstMaterial);
            GPUSceneMaterialInput secondMaterial;
            secondMaterial.m_MetallicFactor = 1.0f;
            scene.ExtractMaterial(secondMaterialKey, secondMaterial);

            GPUSceneLightInput pointLight;
            pointLight.m_Position = pointLightPosition;
            pointLight.m_Range = 5.0f;
            scene.ExtractLight(pointLightKey, pointLight);
            GPUSceneLightInput spotLight;
            spotLight.m_Type = static_cast<u32>(GPUSceneLightType::Spot);
            spotLight.m_Position = glm::vec3(0.0f, 4.0f, 0.0f);
            spotLight.m_Range = 8.0f;
            spotLight.m_InnerCutoffDegrees = 20.0f;
            spotLight.m_OuterCutoffDegrees = 30.0f;
            scene.ExtractLight(spotLightKey, spotLight);

            scene.ExtractEnvironment(environmentKey, GPUSceneEnvironmentInput{});
            scene.ExtractInstance(instanceKey, GPUSceneInstanceInput{ .m_Material = firstMaterialKey });
            (void)scene.EndExtraction();
            scene.Upload();
        };

        // Frame 1: two materials and two lights outgrow capacity 1; the single
        // geometry, instance and environment fit their capacity of 1.
        extractFrame(0.5f, glm::vec3(0.0f));

        const GPUSceneFrameStats& grown = scene.GetLastFrameUpdate().m_Stats;
        EXPECT_EQ(grown.m_BufferGrowthEvents, 2u)
            << "materials and lights grew; instances, geometries and environments fit";
        EXPECT_EQ(grown.m_Instances.m_BufferCapacity, 1u);
        EXPECT_EQ(grown.m_Geometries.m_BufferCapacity, 1u);
        EXPECT_EQ(grown.m_Materials.m_Live, 2u);
        EXPECT_EQ(grown.m_Materials.m_SlotCount, 2u);
        EXPECT_EQ(grown.m_Materials.m_BufferCapacity, 2u);
        EXPECT_EQ(grown.m_Lights.m_Live, 2u);
        EXPECT_EQ(grown.m_Lights.m_SlotCount, 2u);
        EXPECT_EQ(grown.m_Lights.m_BufferCapacity, 2u);
        EXPECT_EQ(grown.m_Environments.m_Live, 1u);
        EXPECT_EQ(grown.m_Environments.m_SlotCount, 1u);
        EXPECT_EQ(grown.m_Environments.m_BufferCapacity, 1u);
        EXPECT_EQ(grown.m_Instances.m_UploadBytes, sizeof(GPUSceneInstance));
        EXPECT_EQ(grown.m_Geometries.m_UploadBytes, sizeof(GPUSceneGeometry));
        EXPECT_EQ(grown.m_Materials.m_UploadBytes, 2u * sizeof(GPUSceneMaterial));
        EXPECT_EQ(grown.m_Lights.m_UploadBytes, 2u * sizeof(GPUSceneLight));
        EXPECT_EQ(grown.m_Environments.m_UploadBytes, sizeof(GPUSceneEnvironment));
        EXPECT_EQ(grown.m_UploadBytes,
                  grown.m_Instances.m_UploadBytes + grown.m_Geometries.m_UploadBytes + grown.m_Materials.m_UploadBytes +
                      grown.m_Lights.m_UploadBytes + grown.m_Environments.m_UploadBytes)
            << "the total must be the sum of the per-kind figures";
        EXPECT_EQ(scene.GetInstanceBufferHandle(), instanceBufferBefore);
        EXPECT_EQ(scene.GetGeometryBufferHandle(), geometryBufferBefore);
        EXPECT_EQ(scene.GetMaterialBufferHandle(), materialBufferBefore);
        EXPECT_EQ(scene.GetLightBufferHandle(), lightBufferBefore);
        EXPECT_EQ(scene.GetEnvironmentBufferHandle(), environmentBufferBefore);
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(materialBufferBefore));
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(lightBufferBefore));
        EXPECT_TRUE(RHI::ResourceRegistry::Get().IsLive(environmentBufferBefore));

        // The instance resolved the canonical material slot, not a mesh-local index.
        const GPUSceneHandle firstMaterial = scene.FindMaterial(firstMaterialKey);
        ASSERT_TRUE(scene.IsMaterialHandleLive(firstMaterial));
        const GPUSceneInstance* instanceRecord = scene.GetInstanceRecord(scene.FindInstance(instanceKey));
        ASSERT_NE(instanceRecord, nullptr);
        EXPECT_EQ(instanceRecord->MaterialIndex, firstMaterial.m_Index);
        EXPECT_EQ(instanceRecord->MaterialGeneration, firstMaterial.m_Generation);

        // Frame 2: only the point light moves.
        extractFrame(0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
        const GPUSceneFrameStats& movedLight = scene.GetLastFrameUpdate().m_Stats;
        EXPECT_EQ(movedLight.m_BufferGrowthEvents, 0u);
        EXPECT_EQ(movedLight.m_Lights.m_UploadBytes, sizeof(GPUSceneLight));
        EXPECT_EQ(movedLight.m_UploadBytes, sizeof(GPUSceneLight))
            << "one moving light must upload one light record and nothing else (#993)";

        // Frame 3: only the first material's roughness changes. Roughness is a
        // compatible edit, so the slot keeps its generation and the instance
        // that references it is not re-uploaded either.
        extractFrame(0.75f, glm::vec3(1.0f, 0.0f, 0.0f));
        const GPUSceneFrameStats& editedMaterial = scene.GetLastFrameUpdate().m_Stats;
        EXPECT_EQ(editedMaterial.m_BufferGrowthEvents, 0u);
        EXPECT_EQ(editedMaterial.m_Materials.m_UploadBytes, sizeof(GPUSceneMaterial));
        EXPECT_EQ(editedMaterial.m_UploadBytes, sizeof(GPUSceneMaterial))
            << "one compatible material edit must upload one material record and nothing else (#992)";
        EXPECT_EQ(scene.FindMaterial(firstMaterialKey), firstMaterial)
            << "a roughness change is a compatible edit and keeps the slot generation";

        scene.Shutdown();
        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(instanceBufferBefore));
        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(geometryBufferBefore));
        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(materialBufferBefore));
        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(lightBufferBefore));
        EXPECT_FALSE(RHI::ResourceRegistry::Get().IsLive(environmentBufferBefore));
        EXPECT_FALSE(scene.IsMaterialHandleLive(firstMaterial));
    }
} // namespace OloEngine::Tests
