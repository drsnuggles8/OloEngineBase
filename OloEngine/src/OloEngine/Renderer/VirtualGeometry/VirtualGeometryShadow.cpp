#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryShadow.h"

#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/VirtualShadowMap.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstring>

namespace OloEngine::VirtualGeometryShadow
{
    namespace
    {
        // Primary-created shader resources. Separate ComputeShader instance from the
        // main pass's so the ortho-mode uniforms never leak between the two
        // program objects.
        Ref<ComputeShader> s_CullShader;
        Ref<Shader> s_DepthShader;
        // The VSM route's raster (issue #1149). A separate program from
        // s_DepthShader because the two resolve visibility completely
        // differently: the classic one writes a depth attachment, this one does a
        // page-table lookup and an imageAtomicMin into the physical pool.
        Ref<Shader> s_VsmDepthShader;

        // Pixels per world-space unit of LOD error for an ORTHOGRAPHIC view.
        //
        // The X/Y scale of the VP's linear map into [-1,1] NDC, times half the
        // target resolution. EXACT for an orthographic projection — which is what
        // both the CSM cascades and the VSM clip levels use — and the reason the
        // shadow cut matches the main view's at the same on-screen error.
        [[nodiscard]] f32 OrthoErrorScale(const glm::mat4& viewProjection, u32 resolution)
        {
            f32 const rowScaleX = glm::length(glm::vec3(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0]));
            f32 const rowScaleY = glm::length(glm::vec3(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1]));
            return 0.5f * static_cast<f32>(resolution) * std::max(rowScaleX, rowScaleY);
        }
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

        // Ortho pixels-per-world-unit from the light VP rows.
        f32 const orthoErrorScale = OrthoErrorScale(lightVPRel, shadowResolution);

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

    bool PrepareVirtualShadowMapRoute(ViewResources& resources)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_VsmDepthShader)
            s_VsmDepthShader = Shader::Create("assets/shaders/VSM_VirtualMeshDepth.glsl");
        if (!s_VsmDepthShader)
        {
            // Once, not per frame: the failure is permanent for the session, and
            // the alternative — returning zero draws from inside the raster scope
            // — presents as "virtual geometry casts no shadow" with nothing in
            // the log to explain it.
            static bool s_LoggedMissingShader = false;
            if (!s_LoggedMissingShader)
            {
                OLO_CORE_ERROR("VirtualGeometryShadow: assets/shaders/VSM_VirtualMeshDepth.glsl failed to load — "
                               "virtual geometry will cast NO shadow while Virtual Shadow Maps are enabled "
                               "(the classic CSM path is unaffected)");
                s_LoggedMissingShader = true;
            }
            return false;
        }

        // Owned by the view rather than created in PrepareViews because only this
        // route uses them, and PrepareViews is shared with the classic cascades.
        if (!resources.VsmCamera)
            resources.VsmCamera = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
        if (!resources.VsmPass)
            resources.VsmPass = UniformBuffer::Create(VSM::PassUBO::GetSize(), ShaderBindingLayout::UBO_VIRTUAL_SHADOW_DRAW);
        return resources.VsmCamera && resources.VsmPass;
    }

    u32 RenderVirtualShadowMapLevels(std::span<const VsmClipView> levels, u32 virtualResolution,
                                     const std::function<void()>& bindPhysicalPool,
                                     ViewResources& resources)
    {
        OLO_PROFILE_FUNCTION();
        if (levels.empty() || !s_CullShader || !bindPhysicalPool)
            return 0; // PrepareViews decided there was nothing to draw

        // PrepareVirtualShadowMapRoute is what loads these, before the raster
        // scope opened — a tripwire, not a load site.
        if (!s_VsmDepthShader || !resources.VsmCamera || !resources.VsmPass)
            return 0;

        auto& registry = VirtualMeshRegistry::Get();
        const auto& instances = registry.GetFrameInstances();

        const RHI::ResourceHandle commandBuffer = resources.Commands->GetRHIHandle();
        const RHI::ResourceHandle argsBuffer = resources.Args->GetRHIHandle();

        u32 levelsDrawn = 0;
        for (const VsmClipView& level : levels)
        {
            // ---- The shadow camera the CULL reads ---------------------------
            //
            // The MATH flavour, NOT AdjustProjectionForBackend'd — the opposite
            // of what the CSM path uploads here. It can be, because on this route
            // nothing feeds the camera UBO to a gl_Position: the raster takes its
            // matrix from the VSM globals block. So the cull gets frustum planes
            // and a page footprint from an unflipped, unremapped matrix on both
            // backends, which is what the page-space arithmetic below assumes.
            auto cameraUBOData = ShaderBindingLayout::CameraUBO{};
            cameraUBOData.ViewProjection = level.ViewProjection;
            cameraUBOData.View = glm::mat4(1.0f);
            cameraUBOData.Projection = level.ViewProjection;
            cameraUBOData.ProjectionForReconstruction = level.ViewProjection;
            cameraUBOData.Position = glm::vec3(0.0f);
            cameraUBOData.Pad0 = 0.0f;
            // The clip projections are already render-origin-relative, and so are
            // the instance transforms they multiply, so the origin is zero here.
            cameraUBOData.RenderOrigin = glm::vec3(0.0f);
            resources.VsmCamera->SetData(&cameraUBOData, ShaderBindingLayout::CameraUBO::GetSize());
            resources.VsmCamera->Bind();

            // A command-stream clear, per level: the previous level's commands
            // and counts must not survive into this one's indirect draw.
            resources.Args->ClearData();
            registry.GetClusterBuffer()->Bind();
            registry.GetGroupBuffer()->Bind();
            registry.GetInstanceBuffer()->Bind();
            resources.Commands->Bind();
            resources.Args->Bind();
            resources.Visible->Bind();
            registry.GetSwListBuffer()->Bind(); // OrthoMode disables all SW writes.
            registry.GetGroupStatesBuffer()->Bind();

            s_CullShader->Bind();
            UBOStructures::VirtualClusterCullUBO cullParams{};
            cullParams.OrthoMode = 1;
            cullParams.OcclusionEnabled = 0; // shadows rasterize every caster
            cullParams.OrthoErrorScale = OrthoErrorScale(level.ViewProjection, virtualResolution);
            cullParams.ViewportHeight = static_cast<f32>(virtualResolution);
            cullParams.SwRasterThresholdPixels = 0.0f;
            // THE gate. Without it this route would be the classic per-view
            // replay with sixteen views instead of four — every cluster of every
            // level redrawn every frame, which is precisely the cost a page cache
            // exists to avoid.
            cullParams.VsmPageGate = glm::ivec4(static_cast<i32>(level.ClipLevel),
                                                level.PageOffset.x, level.PageOffset.y, 0);
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

            // ---- Depth-only MDI replay into this level's pages --------------
            //
            // The VSM already bound its raster scope, viewport, render state and
            // page-table working set; the program, the pool image and the
            // per-draw blocks are ours. The level travels in the pass block
            // because the command stream carries clusters, not levels — see
            // VSM_VirtualMeshDepth.glsl.
            s_VsmDepthShader->Bind();
            // Immediately after the program bind and before any draw — see the
            // contract on this parameter in the header.
            bindPhysicalPool();
            VSM::PassUBO passParams{};
            passParams.Params.x = level.ClipLevel;
            resources.VsmPass->SetData(&passParams, VSM::PassUBO::GetSize());
            resources.VsmPass->Bind();
            resources.DrawInfo->Bind();
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
            ++levelsDrawn;

            // WRITE-AFTER-READ, and the reason the classic cascade path needs no
            // such barrier: it gives every view its OWN command / args / visible
            // buffers, because its views record in parallel. These levels are
            // sequential and share ONE set to keep the memory bounded, so the
            // next level's ClearData and cull writes would otherwise race the
            // indirect draws above still reading them. The failure would be a
            // level drawing another level's command counts — flickering shadow
            // pages, not a crash.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);
        }

        RenderCommand::BindVertexArrayRaw(RHI::NullResource);
        return levelsDrawn;
    }

    bool CollectShadowCasterBounds(std::vector<ShadowCasterBounds>& out)
    {
        auto& registry = VirtualMeshRegistry::Get();
        const auto& instances = registry.GetFrameInstances();
        if (instances.empty())
            return false;

        bool any = false;
        for (const auto& instance : instances)
        {
            // An instance with no bounds is skipped rather than treated as
            // infinite: invalidating every page for a part whose cluster spheres
            // were missing would redraw the whole shadow map every frame, which
            // is a far worse failure than the stale silhouette it guards against.
            // Such a part draws nothing anyway — it has no clusters.
            if (!instance.CastShadows || !instance.HasBounds)
                continue;

            ShadowCasterBounds bounds;
            bounds.Min = instance.BoundsMin;
            bounds.Max = instance.BoundsMax;
            bounds.PrevMin = instance.PrevBoundsMin;
            bounds.PrevMax = instance.PrevBoundsMax;
            // By BYTES, over the TRANSFORMS — see ShadowCasterBounds::Moved for
            // why the derived AABBs are the wrong thing to compare, and
            // cpp-coding-quality.md for why it is not `!=` on a glm type.
            bounds.Moved = std::memcmp(&instance.Gpu.Transform, &instance.Gpu.PrevTransform,
                                       sizeof(glm::mat4)) != 0;
            out.push_back(bounds);
            any = true;
        }
        return any;
    }

    void Shutdown()
    {
        s_CullShader = nullptr;
        s_DepthShader = nullptr;
        s_VsmDepthShader = nullptr;
    }
} // namespace OloEngine::VirtualGeometryShadow
