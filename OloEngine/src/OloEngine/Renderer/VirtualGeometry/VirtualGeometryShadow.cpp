#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryShadow.h"

#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"

#include <glad/gl.h>
#include <glm/geometric.hpp>

#include <algorithm>

namespace OloEngine::VirtualGeometryShadow
{
    namespace
    {
        // Lazily-created GL resources. Separate ComputeShader instance from the
        // main pass's so the ortho-mode uniforms never leak between the two
        // program objects.
        Ref<ComputeShader> s_CullShader;
        Ref<Shader> s_DepthShader;
        Ref<UniformBuffer> s_DrawInfoUBO;
    } // namespace

    void RenderCascade(const glm::mat4& lightVPRel, u32 shadowResolution)
    {
        OLO_PROFILE_FUNCTION();

        auto& registry = VirtualMeshRegistry::Get();
        if (!registry.PrepareFrame(Renderer3D::GetRenderOrigin()))
            return;
        registry.ProcessResidency(); // idempotent — first caller this frame wins

        const auto& instances = registry.GetFrameInstances();
        bool anyCaster = false;
        for (const auto& instance : instances)
        {
            anyCaster = anyCaster || instance.CastShadows;
        }
        if (!anyCaster)
            return;

        if (!s_CullShader)
        {
            s_CullShader = ComputeShader::Create("assets/shaders/compute/VirtualClusterCull.comp");
        }
        if (!s_DepthShader)
        {
            s_DepthShader = Shader::Create("assets/shaders/VirtualMeshShadowDepth.glsl");
        }
        if (!s_DrawInfoUBO)
        {
            s_DrawInfoUBO = UniformBuffer::Create(16, ShaderBindingLayout::UBO_VIRTUAL_DRAW);
        }
        if (!s_CullShader || !s_DepthShader)
            return;

        // Zero this cascade's draw counts (the same args buffer the main view
        // uses later — it re-zeros + re-culls after the shadow pass).
        Ref<StorageBuffer> argsBuffer = registry.GetArgsBuffer();
        std::vector<VirtualDrawArgs> const zeroArgs(instances.size());
        argsBuffer->SetData(zeroArgs.data(),
                            static_cast<u32>(zeroArgs.size() * sizeof(VirtualDrawArgs)), 0);

        registry.GetClusterBuffer()->Bind();
        registry.GetGroupBuffer()->Bind();
        registry.GetInstanceBuffer()->Bind();
        registry.GetCommandBuffer()->Bind();
        registry.GetArgsBuffer()->Bind();
        registry.GetVisibleBuffer()->Bind();
        registry.GetSwListBuffer()->Bind();
        registry.GetGroupStatesBuffer()->Bind();

        // Ortho pixels-per-world-unit from the light VP rows (X/Y scale of the
        // linear map into [-1,1] NDC, times half the shadow resolution).
        f32 const rowScaleX = glm::length(glm::vec3(lightVPRel[0][0], lightVPRel[1][0], lightVPRel[2][0]));
        f32 const rowScaleY = glm::length(glm::vec3(lightVPRel[0][1], lightVPRel[1][1], lightVPRel[2][1]));
        f32 const orthoErrorScale = 0.5f * static_cast<f32>(shadowResolution) * std::max(rowScaleX, rowScaleY);

        s_CullShader->Bind();
        s_CullShader->SetInt("u_OrthoMode", 1);
        s_CullShader->SetInt("u_OcclusionEnabled", 0); // shadows rasterize every caster (also gated by ortho mode)
        s_CullShader->SetFloat("u_OrthoErrorScale", orthoErrorScale);
        s_CullShader->SetFloat("u_ViewportHeight", static_cast<f32>(shadowResolution));
        s_CullShader->SetFloat("u_SwRasterThresholdPixels", 0.0f);
        for (sizet i = 0; i < instances.size(); ++i)
        {
            if (!instances[i].CastShadows)
                continue;
            s_CullShader->SetUint("u_InstanceIndex", static_cast<u32>(i));
            u32 const groups = (instances[i].Gpu.ClusterCount + 63u) / 64u;
            RenderCommand::DispatchCompute(groups, 1, 1);
        }
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

        // Depth-only MDI replay. The shadow pass already configured the target
        // FBO, viewport, front-face culling and color masks; only the program,
        // per-draw UBO and SSBO bindings are ours.
        s_DepthShader->Bind();
        s_DrawInfoUBO->Bind();
        u32 const commandBufferID = registry.GetCommandBuffer()->GetRendererID();
        u32 const argsBufferID = registry.GetArgsBuffer()->GetRendererID();
        for (sizet i = 0; i < instances.size(); ++i)
        {
            if (!instances[i].CastShadows)
                continue;
            u32 const drawInfo[4] = { static_cast<u32>(i), instances[i].Gpu.CommandBase, 0u, 0u };
            s_DrawInfoUBO->SetData(drawInfo, sizeof(drawInfo));
            RenderCommand::MultiDrawElementsIndirectCountRaw(
                registry.GetVaoID(), commandBufferID,
                instances[i].Gpu.CommandBase * 32u,
                argsBufferID, static_cast<u32>(i * sizeof(VirtualDrawArgs)),
                instances[i].Gpu.ClusterCount, 32u);
        }
        ::glBindVertexArray(0);
    }

    void Shutdown()
    {
        s_CullShader = nullptr;
        s_DepthShader = nullptr;
        s_DrawInfoUBO = nullptr;
    }
} // namespace OloEngine::VirtualGeometryShadow
