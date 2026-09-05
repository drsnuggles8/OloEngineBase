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
        // Primary-created shader resources. Separate ComputeShader instance from the
        // main pass's so the ortho-mode uniforms never leak between the two
        // program objects.
        Ref<ComputeShader> s_CullShader;
        Ref<Shader> s_DepthShader;
    } // namespace

    bool PrepareViews(std::span<ViewResources> views)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(!RenderCommand::GetRendererAPI().IsRecordingParallelItem(), "Prepare virtual shadow resources before the fork");
        auto& registry = VirtualMeshRegistry::Get();
        if (views.empty() || !registry.PrepareFrame(Renderer3D::GetRenderOrigin()))
            return false;
        registry.ProcessResidency();
        const auto& instances = registry.GetFrameInstances();
        if (std::ranges::none_of(instances, [](const auto& instance)
                                 { return instance.CastShadows; }))
            return false;
        if (!s_CullShader)
            s_CullShader = ComputeShader::Create("assets/shaders/compute/VirtualClusterCull.comp");
        if (!s_DepthShader)
            s_DepthShader = Shader::Create("assets/shaders/VirtualMeshShadowDepth.glsl");
        if (!s_CullShader || !s_DepthShader)
            return false;
        const auto ensureOutput = [](Ref<StorageBuffer>& buffer, const Ref<StorageBuffer>& source)
        {
            if (!buffer)
                buffer = StorageBuffer::Create(source->GetSize(), source->GetBinding(), StorageBufferUsage::DynamicCopy);
            else if (buffer->GetSize() < source->GetSize())
                buffer->Resize(source->GetSize());
        };
        for (auto& view : views)
        {
            ensureOutput(view.Commands, registry.GetCommandBuffer());
            ensureOutput(view.Args, registry.GetArgsBuffer());
            ensureOutput(view.Visible, registry.GetVisibleBuffer());
            if (!view.CullParams)
                view.CullParams = UniformBuffer::Create(UBOStructures::VirtualClusterCullUBO::GetSize(), ShaderBindingLayout::UBO_VIRTUAL_CLUSTER_CULL);
            if (!view.DrawInfo)
                view.DrawInfo = UniformBuffer::Create(sizeof(VirtualDrawInfoGpu), ShaderBindingLayout::UBO_VIRTUAL_DRAW);
        }
        return true;
    }

    void RenderCascade(const glm::mat4& lightVPRel, u32 shadowResolution, ViewResources& resources)
    {
        OLO_PROFILE_FUNCTION();
        auto& registry = VirtualMeshRegistry::Get();
        const auto& instances = registry.GetFrameInstances();
        // A command-stream clear keeps compute writes and indirect consumers on
        // the persistent buffer; CPU SetData would publish a draw-time snapshot.
        resources.Args->ClearData();
        registry.GetClusterBuffer()->Bind();
        registry.GetGroupBuffer()->Bind();
        registry.GetInstanceBuffer()->Bind();
        resources.Commands->Bind();
        resources.Args->Bind();
        resources.Visible->Bind();
        registry.GetSwListBuffer()->Bind(); // OrthoMode disables all SW writes.
        // These GPU atomic ORs preserve residency requests/touches from every
        // view. No recording item uploads this shared object. The cull barrier
        // below orders its GPU accesses before the next view's cull.
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
            resources.CullParams->SetData(&cullParams, sizeof(cullParams));
            resources.CullParams->Bind();
            u32 const groups = (instances[i].Gpu.ClusterCount + 63u) / 64u;
            RenderCommand::DispatchCompute(groups, 1, 1);
        }
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

        // Depth-only MDI replay. The shadow pass already configured the target
        // FBO, viewport, front-face culling and color masks; only the program,
        // per-draw UBO and SSBO bindings are ours.
        s_DepthShader->Bind();
        resources.DrawInfo->Bind();
        const RHI::ResourceHandle commandBuffer = resources.Commands->GetRHIHandle();
        const RHI::ResourceHandle argsBuffer = resources.Args->GetRHIHandle();
        for (sizet i = 0; i < instances.size(); ++i)
        {
            if (!instances[i].CastShadows)
                continue;
            VirtualDrawInfoGpu drawInfo{};
            drawInfo.InstanceIndex = static_cast<u32>(i);
            drawInfo.CommandBase = instances[i].Gpu.CommandBase;
            resources.DrawInfo->SetData(&drawInfo, sizeof(drawInfo));
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
        s_DepthShader = nullptr;
    }
} // namespace OloEngine::VirtualGeometryShadow
