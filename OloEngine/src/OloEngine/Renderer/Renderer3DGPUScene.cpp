#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include "OloEngine/Renderer/MeshSource.h"

namespace OloEngine
{
    void Renderer3D::BeginGPUSceneExtraction(u64 ownerToken)
    {
        OLO_CORE_ASSERT(!s_Data.GPUSceneExtractionActive,
                        "Renderer3D::BeginGPUSceneExtraction called twice before EndScene");
        s_Data.SceneGPU.BeginExtraction(ownerToken, s_Data.RenderOrigin);
        s_Data.GPUSceneExtractionActive = true;
    }

    void Renderer3D::ExtractGPUSceneMesh(u64 stableEntityId, u64 stableInstanceId,
                                         const Ref<MeshSource>& meshSource, u32 submeshIndex,
                                         const glm::mat4& worldTransform, u32 visibilityMask,
                                         u32 flags)
    {
        if (!s_Data.GPUSceneExtractionActive || !meshSource || !meshSource->GetVertexArray() || submeshIndex >= static_cast<u32>(meshSource->GetSubmeshes().Num()))
        {
            return;
        }

        const Ref<VertexBuffer>& vertexBuffer = meshSource->GetVertexBuffer();
        const Ref<IndexBuffer>& indexBuffer = meshSource->GetIndexBuffer();
        if (!vertexBuffer || !indexBuffer)
        {
            return;
        }

        const RHI::ResourceHandle vertexHandle = vertexBuffer->GetRHIHandle();
        const RHI::ResourceHandle indexHandle = indexBuffer->GetRHIHandle();
        if (!vertexHandle.IsValid() || !indexHandle.IsValid())
        {
            return;
        }

        const Submesh& submesh = meshSource->GetSubmeshes()[static_cast<i32>(submeshIndex)];
        const GPUSceneGeometryKey geometryKey{
            .m_VertexBuffer = RHI::HashKey(vertexHandle),
            .m_IndexBuffer = RHI::HashKey(indexHandle),
            .m_SubmeshIndex = submeshIndex,
        };
        s_Data.SceneGPU.ExtractGeometry(
            geometryKey,
            GPUSceneGeometryInput{
                .m_VertexBuffer = vertexHandle,
                .m_IndexBuffer = indexHandle,
                .m_VertexAddress = vertexBuffer->GetDeviceAddress(),
                .m_IndexAddress = indexBuffer->GetDeviceAddress(),
                .m_VertexFormat = std::to_underlying(GPUSceneVertexFormat::OloVertex),
                .m_IndexFormat = std::to_underlying(GPUSceneIndexFormat::UInt32),
                .m_FirstIndex = submesh.m_BaseIndex,
                .m_IndexCount = submesh.m_IndexCount,
                .m_BaseVertex = static_cast<i32>(submesh.m_BaseVertex),
                .m_VertexCount = submesh.m_VertexCount,
            });
        s_Data.SceneGPU.ExtractInstance(
            GPUSceneInstanceKey{
                .m_EntityId = stableEntityId,
                .m_Geometry = geometryKey,
                .m_InstanceId = stableInstanceId,
            },
            GPUSceneInstanceInput{
                .m_WorldTransform = worldTransform,
                .m_MaterialIndex = submesh.m_MaterialIndex,
                .m_VisibilityMask = visibilityMask,
                .m_Flags = flags,
            });
    }

    void Renderer3D::ReportUnsupportedGPUScene(GPUSceneUnsupportedCategory category, u32 count)
    {
        if (s_Data.GPUSceneExtractionActive && count > 0)
        {
            s_Data.SceneGPU.ReportUnsupported(category, count);
        }
    }

    void Renderer3D::ResetGPUScene()
    {
        s_Data.SceneGPU.Reset();
        s_Data.GPUSceneExtractionActive = false;
    }

    const GPUSceneFrameStats& Renderer3D::GetGPUSceneStats()
    {
        return s_Data.SceneGPU.GetLastFrameUpdate().m_Stats;
    }
} // namespace OloEngine
