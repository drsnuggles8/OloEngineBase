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
        // VirtualClusterCull.comp's former bare uniforms (issue #691),
        // at UBO_VIRTUAL_CLUSTER_CULL. Refilled per instance dispatch.
        Ref<UniformBuffer> s_CullParamsUBO;
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
            s_DrawInfoUBO = UniformBuffer::Create(sizeof(VirtualDrawInfoGpu), ShaderBindingLayout::UBO_VIRTUAL_DRAW);
        }
        if (!s_CullShader || !s_DepthShader)
            return;

        // Zero this cascade's draw counts (the same args buffer the main view
        // uses later — it re-zeros + re-culls after the shadow pass).
        Ref<StorageBuffer> argsStorage = registry.GetArgsBuffer();
        std::vector<VirtualDrawArgs> const zeroArgs(instances.size());
        argsStorage->SetData(zeroArgs.data(),
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
        // Former bare uniforms, now one std140 block refilled per dispatch
        // (issue #691). The struct is value-initialised, so every
        // two-phase / debug control this path never set is a deterministic 0 —
        // which is exactly the single-phase, no-debug behaviour the shadow cull
        // relied on when it simply skipped those Set* calls.
        if (!s_CullParamsUBO)
        {
            s_CullParamsUBO = UniformBuffer::Create(UBOStructures::VirtualClusterCullUBO::GetSize(),
                                                    ShaderBindingLayout::UBO_VIRTUAL_CLUSTER_CULL);
        }
        UBOStructures::VirtualClusterCullUBO cullParams{};
        cullParams.OrthoMode = 1;
        cullParams.OcclusionEnabled = 0; // shadows rasterize every caster (also gated by ortho mode)
        cullParams.OrthoErrorScale = orthoErrorScale;
        cullParams.ViewportHeight = static_cast<f32>(shadowResolution);
        cullParams.SwRasterThresholdPixels = 0.0f;
        for (sizet i = 0; i < instances.size(); ++i)
        {
            if (!instances[i].CastShadows)
                continue;
            cullParams.InstanceIndex = static_cast<u32>(i);
            s_CullParamsUBO->SetData(&cullParams, sizeof(cullParams));
            s_CullParamsUBO->Bind();
            u32 const groups = (instances[i].Gpu.ClusterCount + 63u) / 64u;
            RenderCommand::DispatchCompute(groups, 1, 1);
        }
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

        // Depth-only MDI replay. The shadow pass already configured the target
        // FBO, viewport, front-face culling and color masks; only the program,
        // per-draw UBO and SSBO bindings are ours.
        s_DepthShader->Bind();
        s_DrawInfoUBO->Bind();
        const RHI::ResourceHandle commandBuffer = registry.GetCommandBuffer()->GetRHIHandle();
        const RHI::ResourceHandle argsBuffer = registry.GetArgsBuffer()->GetRHIHandle();
        for (sizet i = 0; i < instances.size(); ++i)
        {
            if (!instances[i].CastShadows)
                continue;
            VirtualDrawInfoGpu drawInfo{};
            drawInfo.InstanceIndex = static_cast<u32>(i);
            drawInfo.CommandBase = instances[i].Gpu.CommandBase;
            s_DrawInfoUBO->SetData(&drawInfo, sizeof(drawInfo));
            RenderCommand::MultiDrawElementsIndirectCountRaw(
                registry.GetVao(), commandBuffer,
                instances[i].Gpu.CommandBase * 32u,
                argsBuffer, static_cast<u32>(i * sizeof(VirtualDrawArgs)),
                instances[i].Gpu.ClusterCount, 32u);
        }
        RenderCommand::BindVertexArrayRaw(RHI::NullResource);
    }

    void Shutdown()
    {
        s_CullShader = nullptr;
        s_CullParamsUBO = nullptr;
        s_DepthShader = nullptr;
        s_DrawInfoUBO = nullptr;
    }
} // namespace OloEngine::VirtualGeometryShadow
