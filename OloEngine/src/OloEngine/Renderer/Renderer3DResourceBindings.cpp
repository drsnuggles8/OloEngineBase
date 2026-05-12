#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    auto Renderer3D::GetRenderStreamNode(RenderStreamType stream) -> CommandBufferRenderPass*
    {
        if (!s_Data.Pipeline)
        {
            return nullptr;
        }

        return s_Data.Pipeline->GetRenderStreamNode(stream);
    }

    void Renderer3D::SetRenderScale(f32 scale)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.RGraph)
        {
            return;
        }

        s_Data.RGraph->SetRenderScale(scale);

        // Upload immediately so the DRS UBO reflects the new scale even if called
        // outside the normal `RenderPipeline::PrepareFrame(...)` path (e.g. from
        // the settings panel).
        const glm::vec2 bounds = s_Data.RGraph->GetRenderScaleBounds();
        s_Data.SceneEffectsGPU.DRSData.RenderScaleBounds = bounds;
        if (s_Data.SceneEffectsGPU.DRS)
        {
            s_Data.SceneEffectsGPU.DRS->SetData(&s_Data.SceneEffectsGPU.DRSData, DRSUBOData::GetSize());
        }
    }

    ShaderLibrary& Renderer3D::GetShaderLibrary()
    {
        return m_ShaderLibrary;
    }
} // namespace OloEngine
