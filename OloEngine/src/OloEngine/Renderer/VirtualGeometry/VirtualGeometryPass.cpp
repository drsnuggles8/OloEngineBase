#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryPass.h"

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"

#include <glad/gl.h>

#include <array>
#include <string>

namespace OloEngine
{
    namespace
    {
        // Insert a preprocessor #define immediately after the shader's #version
        // directive (GLSL requires #version to be the first non-comment token),
        // so the raster shader's `#ifdef VIRTUAL_RASTER_INT64` block selects the
        // single-pass 64-bit atomic layout + enables its required extensions.
        [[nodiscard]] std::string InjectDefineAfterVersion(const std::string& source, const std::string& define)
        {
            sizet const versionPos = source.find("#version");
            if (versionPos == std::string::npos)
                return source;
            sizet const lineEnd = source.find('\n', versionPos);
            if (lineEnd == std::string::npos)
                return source;
            return source.substr(0, lineEnd + 1) + define + "\n" + source.substr(lineEnd + 1);
        }
    } // namespace

    VirtualGeometryPass::VirtualGeometryPass()
    {
        SetName("VirtualGeometryPass");
    }

    void VirtualGeometryPass::Init(const FramebufferSpecification& spec)
    {
        m_FramebufferSpec = spec;
        m_CullShader = ComputeShader::Create("assets/shaders/compute/VirtualClusterCull.comp");
        m_RasterShader = ComputeShader::Create("assets/shaders/compute/VirtualClusterRaster.comp");
        m_GBufferShader = Shader::Create("assets/shaders/VirtualMeshGBuffer.glsl");
        m_ResolveShader = Shader::Create("assets/shaders/VirtualVisibilityResolve.glsl");
        m_ColorizeShader = ComputeShader::Create("assets/shaders/compute/VirtualDebugColorize.comp");

        // Single-pass 64-bit visibility path: only when the driver exposes both
        // 64-bit shader ints and 64-bit atomics. Compiled as a define-injected
        // variant of the SAME source as the portable rasterizer so the two paths
        // cannot drift. The portable two-pass shader stays compiled as the
        // universal fallback and the parity/force-portable override. The resolve
        // shader needs NO variant: the int64 packing (depthBits<<32 | payload)
        // is byte-identical to the portable uvec2{.x=payload, .y=depth} on
        // little-endian GPUs, so it reads either layout correctly (issue #629).
        m_Int64AtomicsSupported = RenderCommand::SupportsInt64ShaderAtomics();
        if (m_Int64AtomicsSupported)
        {
            ComputeShader::SourceLoadResult const loaded =
                ComputeShader::LoadSourceFromFile("assets/shaders/compute/VirtualClusterRaster.comp");
            if (loaded.IsValid())
            {
                std::string const int64Source = InjectDefineAfterVersion(loaded.Source, "#define VIRTUAL_RASTER_INT64 1");
                m_RasterShaderInt64 = ComputeShader::CreateFromSource(loaded.Name + "_Int64", int64Source);
            }

            if (!m_RasterShaderInt64 || !m_RasterShaderInt64->IsValid())
            {
                OLO_CORE_WARN("VirtualGeometryPass: 64-bit atomics advertised but the int64 raster variant failed "
                              "to compile; falling back to the portable two-pass 2x32 path");
                m_RasterShaderInt64 = nullptr;
                m_Int64AtomicsSupported = false;
            }
            else
            {
                OLO_CORE_INFO("VirtualGeometryPass: using single-pass 64-bit atomic software rasterizer");
            }
        }
    }

    void VirtualGeometryPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_SelectedSceneDepth = {};
        m_SelectedVelocity = {};
        m_SelectedGBufferAlbedo = {};
        m_SelectedGBufferNormal = {};
        m_SelectedGBufferEmissive = {};
        m_SelectedGBufferAlbedoMS = {};
        m_SelectedGBufferNormalMS = {};
        m_SelectedGBufferEmissiveMS = {};
        m_SelectedVelocityMS = {};
        m_SelectedSceneDepthMS = {};

        // Deferred-path only: the G-Buffer is the integration point.
        if (Renderer3D::GetRendererSettings().Path != RenderingPath::Deferred)
            return;

        // Declared UNCONDITIONALLY in Deferred (not gated on this frame's
        // submission count) so the topology stays stable when the instance
        // list transitions empty <-> non-empty without a graph rebuild.
        // TransferDest writes on the exported scene/G-Buffer textures order
        // this pass after ScenePass (their prior writer) and before every
        // downstream reader (DeferredLightingPass / GTAO / SSR / TAA), exactly
        // like DeferredGPUOcclusionPass. Execute re-copies the attachments
        // over the exports after our draws; handles that alias the physical
        // attachment self-skip the copy.
        const auto declareExport = [&builder](const RGTextureHandle handle, RGTextureHandle& stored)
        {
            if (handle.IsValid())
            {
                stored = handle;
                builder.Write(handle, RGWriteUsage::TransferDest);
            }
        };
        declareExport(board.Scene.SceneDepth, m_SelectedSceneDepth);
        declareExport(board.GBuffer.Velocity, m_SelectedVelocity);
        declareExport(board.GBuffer.GBufferAlbedo, m_SelectedGBufferAlbedo);
        declareExport(board.GBuffer.GBufferNormal, m_SelectedGBufferNormal);
        declareExport(board.GBuffer.GBufferEmissive, m_SelectedGBufferEmissive);
        // MSAA per-sample companions (present only when the G-Buffer is
        // multisample); declared unconditionally like the resolved set so a
        // runtime MSAA toggle doesn't force a graph rebuild.
        declareExport(board.GBuffer.GBufferAlbedoMS, m_SelectedGBufferAlbedoMS);
        declareExport(board.GBuffer.GBufferNormalMS, m_SelectedGBufferNormalMS);
        declareExport(board.GBuffer.GBufferEmissiveMS, m_SelectedGBufferEmissiveMS);
        declareExport(board.GBuffer.VelocityMS, m_SelectedVelocityMS);
        declareExport(board.GBuffer.SceneDepthMS, m_SelectedSceneDepthMS);

        // Publish the cluster/LOD/overdraw debug capture target (issue #629) when
        // a debug mode is active and its texture already exists (created lazily in
        // Execute, so it becomes capturable from the second debug frame on).
        // Named so olo_render_capture_target / olo_render_list_targets resolve it.
        auto& registry = VirtualMeshRegistry::Get();
        if (registry.GetDebugMode() != VirtualDebugMode::Off && registry.GetDebugColorTextureID() != 0)
        {
            [[maybe_unused]] const RGTextureHandle debugTarget = builder.ImportTexture(
                "VirtualGeometryDebug", registry.GetDebugColorTextureID(),
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "VirtualGeometryDebug"));
        }
    }

    void VirtualGeometryPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (Renderer3D::GetRendererSettings().Path != RenderingPath::Deferred)
            return;
        if (!m_CullShader || !m_GBufferShader || !m_ScenePass)
            return;

        const Ref<GBuffer>& gbuffer = m_ScenePass->GetGBuffer();
        if (!gbuffer || !gbuffer->GetFramebuffer())
            return;

        // MSAA (issue #629): per-sample lighting rasterizes into the multisample
        // G-Buffer (resolved afterwards); otherwise virtual geometry draws into
        // the already-resolved G-Buffer — the same rule DeferredGPUOcclusionPass
        // uses. The compute software rasterizer is single-sample, so under MSAA
        // every cluster takes the hardware MDI path (the fixed-function rasterizer
        // handles per-sample coverage).
        bool const msaa = gbuffer->GetSampleCount() > 1;
        bool const perSampleMSAA = m_PerSampleLighting && msaa;

        auto& registry = VirtualMeshRegistry::Get();
        if (!registry.PrepareFrame(Renderer3D::GetRenderOrigin()))
            return;
        registry.EnsureVisbuffer(gbuffer->GetWidth(), gbuffer->GetHeight());

        // Streaming residency: consume last frame's page requests/touches and
        // upload/evict under the budget before this frame's cull consults the
        // resident bits. (The shadow pass may have run these already this
        // frame — both calls are idempotent.)
        registry.ProcessResidency();

        // Debug visualization (issue #629): when a mode is active, both raster
        // paths imageStore/imageAtomicAdd per-pixel cluster/LOD/overdraw data into
        // the registry's debug targets (bound below, cleared per frame here).
        VirtualDebugMode const debugMode = registry.GetDebugMode();
        bool const debugActive = debugMode != VirtualDebugMode::Off;
        if (debugActive)
            registry.EnsureDebugTargets(gbuffer->GetWidth(), gbuffer->GetHeight());
        u32 const debugModeInt = debugActive ? static_cast<u32>(debugMode) : 0u;
        // Publish the mode via UBO_VIRTUAL_DEBUG (a scalar uniform must live in a
        // block for the graphics shaders' SPIR-V path). Both raster shaders read it.
        if (!m_DebugInfoUBO)
            m_DebugInfoUBO = UniformBuffer::Create(16, ShaderBindingLayout::UBO_VIRTUAL_DEBUG);
        u32 const debugInfo[4] = { debugModeInt, 0u, 0u, 0u };
        m_DebugInfoUBO->SetData(debugInfo, sizeof(debugInfo));
        m_DebugInfoUBO->Bind();

        GLStateGuard guard("VirtualGeometryPass", GLStateGuard::Policy::Ignore);

        // ── Hi-Z occlusion inputs (issue #629): build a max-reduction depth
        // pyramid from THIS frame's scene depth (occluders already drawn by
        // ScenePass) so the cull can drop clusters hidden behind opaque geometry.
        // Gated on the same global toggle as the instance HZB cull (off by
        // default). Built BEFORE the cull SSBO binds below — the HZB build binds
        // its own program/SSBOs/textures and would otherwise clobber them.
        bool occlusionUsable = false;
        u32 hzbTextureID = 0;
        glm::vec2 hzbSize{ 0.0f };
        glm::vec2 hzbUVFactor{ 1.0f };
        i32 hzbMipCount = 0;
        f32 occlusionDepthBias = 0.0f;
        if (Renderer3D::IsHZBOcclusionCullingEnabled())
        {
            ::glTextureBarrier(); // order ScenePass's depth write -> HZB texture fetch
            const auto hzb = Renderer3D::BuildCurrentOcclusionHZB(
                gbuffer->GetDepthAttachmentID(), gbuffer->GetWidth(), gbuffer->GetHeight());
            if (hzb.IsUsable())
            {
                occlusionUsable = true;
                hzbTextureID = hzb.HZBTextureID;
                hzbSize = hzb.HZBSize;
                hzbUVFactor = hzb.HZBUVFactor;
                hzbMipCount = static_cast<i32>(hzb.MipCount);
                occlusionDepthBias = hzb.DepthBias;
            }
        }

        // ── 1. DAG-cut + cull compute, one dispatch per instance ──
        // Re-bind every SSBO the dispatch touches: binding points are
        // process-global GL state shared with other subsystems.
        registry.GetClusterBuffer()->Bind();
        registry.GetGroupBuffer()->Bind();
        registry.GetInstanceBuffer()->Bind();
        registry.GetCommandBuffer()->Bind();
        registry.GetArgsBuffer()->Bind();
        registry.GetVisibleBuffer()->Bind();
        registry.GetSwListBuffer()->Bind();
        registry.GetGroupStatesBuffer()->Bind();
        CommandDispatch::BindSceneResources(); // camera UBO at binding 0

        // Software-raster routing threshold: 0 disables (all clusters take the
        // hardware path); ForceSoftware routes everything near-plane-safe.
        VirtualSwRasterMode const swMode = registry.GetSwRasterMode();
        f32 swThresholdPixels = 0.0f;
        if (swMode == VirtualSwRasterMode::Auto)
        {
            swThresholdPixels = registry.GetSwRasterThresholdPixels();
        }
        else if (swMode == VirtualSwRasterMode::ForceSoftware)
        {
            swThresholdPixels = 1.0e9f;
        }
        // The software rasterizer writes a single-sample visibility buffer, which
        // cannot represent MSAA coverage — route every cluster through the
        // hardware MDI path when the G-Buffer is multisample.
        if (msaa)
            swThresholdPixels = 0.0f;

        const auto& instances = registry.GetFrameInstances();

        // Re-zero the per-instance draw args + SW list for the MAIN view: the
        // shadow cascades ran their own culls into the same buffers earlier
        // this frame (PrepareFrame's zeroing only happens on its first call).
        {
            Ref<StorageBuffer> argsBuffer = registry.GetArgsBuffer();
            Ref<StorageBuffer> swListBuffer = registry.GetSwListBuffer();
            std::vector<VirtualDrawArgs> const zeroArgs(instances.size());
            argsBuffer->SetData(zeroArgs.data(),
                                static_cast<u32>(zeroArgs.size() * sizeof(VirtualDrawArgs)), 0);
            u32 const zeroHeader[4] = { 0, 0, 0, 0 };
            swListBuffer->SetData(zeroHeader, sizeof(zeroHeader), 0);
        }

        m_CullShader->Bind();
        m_CullShader->SetFloat("u_ViewportHeight", static_cast<f32>(gbuffer->GetHeight()));
        m_CullShader->SetFloat("u_SwRasterThresholdPixels", swThresholdPixels);
        // Hi-Z occlusion uniforms (u_OrthoMode defaults to 0 here — perspective
        // main view; the shadow cull binds its own program and forces it off).
        m_CullShader->SetInt("u_OcclusionEnabled", occlusionUsable ? 1 : 0);
        if (occlusionUsable)
        {
            ::glBindTextureUnit(0, hzbTextureID);
            // Unit 0 is ALSO u_AlbedoMap for the material draws below. This raw bind happens
            // behind CommandDispatch's redundant-bind cache, which would go on believing slot 0
            // still holds the last albedo it bound — and then SKIP the real bind for any
            // material whose albedo has that same GL ID, leaving the HZB depth pyramid live in
            // u_AlbedoMap. Tell the cache the slot is dirty so the next material bind is real.
            CommandDispatch::InvalidateTextureSlot(0);
            m_CullShader->SetInt("u_HZB", 0);
            m_CullShader->SetFloat2("u_HZBSize", hzbSize);
            m_CullShader->SetFloat2("u_HZBUVFactor", hzbUVFactor);
            m_CullShader->SetInt("u_HZBMipCount", hzbMipCount);
            m_CullShader->SetFloat("u_OcclusionDepthBias", occlusionDepthBias);
        }
        for (sizet i = 0; i < instances.size(); ++i)
        {
            m_CullShader->SetUint("u_InstanceIndex", static_cast<u32>(i));
            u32 const groups = (instances[i].Gpu.ClusterCount + 63u) / 64u;
            RenderCommand::DispatchCompute(groups, 1, 1);
        }

        // Command for the indirect-args read, ShaderStorage for the vertex-
        // pulling reads of the visible/instance/vertex buffers.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

        // ── 1b. Compute software rasterizer (portable two-pass 2x32 scheme):
        // phase 0 atomicMin-compacts depth, phase 1 writes the winning
        // payloads. Dispatched at the conservative total-cluster upper bound;
        // workgroups beyond the live SW count early-out (issue #551 idiom).
        if (swThresholdPixels > 0.0f && registry.GetVisbufferBuffer())
        {
            registry.GetVertexBuffer()->Bind();
            registry.GetVisbufferBuffer()->Bind();
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_VIRTUAL_INDICES,
                               registry.GetIndexBufferID());

            u32 const maxSwRecords = registry.GetTotalFrameClusterCount();
            u32 const groupsX = std::min(maxSwRecords, 4096u);
            u32 const groupsY = (maxSwRecords + groupsX - 1u) / std::max(groupsX, 1u);

            // Single-pass 64-bit atomic path when the driver supports it and the
            // parity/force-portable override is off; the portable two-pass 2x32
            // path otherwise. Both write the same visibility-buffer bytes, so the
            // resolve pass downstream is identical either way.
            bool const useInt64 =
                m_Int64AtomicsSupported && m_RasterShaderInt64 && !registry.GetForcePortableSwRaster();
            const Ref<ComputeShader>& rasterShader = useInt64 ? m_RasterShaderInt64 : m_RasterShader;

            rasterShader->Bind();
            rasterShader->SetUint("u_ViewportWidth", registry.GetVisbufferWidth());
            rasterShader->SetUint("u_ViewportHeight", registry.GetVisbufferHeight());
            if (useInt64)
            {
                // One atomicMin per covered pixel resolves depth + payload
                // together. u_Phase is compiled out of this variant, so don't set
                // it (would warn on a stripped uniform every frame).
                RenderCommand::DispatchCompute(groupsX, groupsY, 1);
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
            }
            else
            {
                // Phase 0 atomic-min-compacts the depth word; phase 1 plain-writes
                // the winning payload where the depth bits match.
                rasterShader->SetUint("u_Phase", 0);
                RenderCommand::DispatchCompute(groupsX, groupsY, 1);
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
                rasterShader->SetUint("u_Phase", 1);
                RenderCommand::DispatchCompute(groupsX, groupsY, 1);
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
            }
        }

        // ── 2. Hardware raster: one MDI-count call per instance ──
        // Non-per-sample MSAA draws straight into the resolved FBO (ScenePass
        // already resolved it); per-sample MSAA and the non-MSAA path draw into
        // the primary FBO (multisample or single-sample respectively).
        Ref<Framebuffer> targetFB = (msaa && !perSampleMSAA) ? gbuffer->GetSamplingFramebuffer()
                                                             : gbuffer->GetFramebuffer();
        if (!targetFB)
            targetFB = gbuffer->GetFramebuffer();
        targetFB->Bind();
        {
            // All five G-Buffer MRTs, same set SceneRenderPass draws
            std::array<GLenum, GBuffer::Count> drawBufs{};
            for (u32 a = 0; a < GBuffer::Count; ++a)
                drawBufs[a] = GL_COLOR_ATTACHMENT0 + a;
            glNamedFramebufferDrawBuffers(targetFB->GetRendererID(), static_cast<GLsizei>(GBuffer::Count), drawBufs.data());
        }

        // Raw GL state, deliberately bypassing the context caches: this pass
        // runs between bucket executions whose dispatchers track state in
        // their own caches — a cached "already true" here can leave the real
        // GL depth test/mask off, which silently drops every depth write
        // (color still lands) and downstream sky/overlay passes then overdraw
        // the clusters. Same raw-GL discipline as GPUDrivenOcclusionPass.
        ::glViewport(0, 0, static_cast<GLsizei>(gbuffer->GetWidth()), static_cast<GLsizei>(gbuffer->GetHeight()));
        ::glEnable(GL_DEPTH_TEST);
        ::glDepthFunc(GL_LESS);
        ::glDepthMask(GL_TRUE);
        ::glDisable(GL_BLEND);
        ::glEnable(GL_CULL_FACE);
        ::glCullFace(GL_BACK);
        ::glDisable(GL_STENCIL_TEST);
        ::glDisable(GL_SCISSOR_TEST);
        ::glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        // Per-attachment color masks can be left disabled by earlier passes
        // (glColorMaski state is indexed and survives a plain glColorMask);
        // a masked RT1/RT2 silently drops normal/emissive writes while RT0 +
        // depth land — the lighting pass then shades clusters with cleared
        // G-Buffer data.
        ::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        for (u32 attachment = 0; attachment < GBuffer::Count; ++attachment)
        {
            ::glColorMaski(attachment, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }

        m_GBufferShader->Bind();
        if (debugActive)
        {
            // Image units 0/1 (separate namespace from the sampler texture units).
            ::glBindImageTexture(0, registry.GetDebugColorTextureID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
            ::glBindImageTexture(1, registry.GetDebugCountTextureID(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        }

        if (!m_DrawInfoUBO)
        {
            m_DrawInfoUBO = UniformBuffer::Create(16, ShaderBindingLayout::UBO_VIRTUAL_DRAW);
        }
        m_DrawInfoUBO->Bind();

        u32 const commandBufferID = registry.GetCommandBuffer()->GetRendererID();
        u32 const argsBufferID = registry.GetArgsBuffer()->GetRendererID();
        for (sizet i = 0; i < instances.size(); ++i)
        {
            const auto& mat = FrameDataBufferManager::Get().GetMaterialData(
                static_cast<u16>(instances[i].MaterialDataIndex));
            CommandDispatch::UploadMaterialForDirectDraw(mat, static_cast<u16>(instances[i].MaterialDataIndex));

            u32 const drawInfo[4] = { static_cast<u32>(i), instances[i].Gpu.CommandBase, 0u, 0u };
            m_DrawInfoUBO->SetData(drawInfo, sizeof(drawInfo));

            // Two-sided materials must not backface-cull. Foliage is a single sheet of quads
            // meant to be seen from both sides, so culling drops half of every leaf — which is
            // what shredded Sponza's plants. The classic path does the same thing via
            // Renderer3DDrawHelpers::BuildRenderState; this loop drives raw GL, so it has to
            // toggle the state itself.
            if (instances[i].TwoSided)
            {
                ::glDisable(GL_CULL_FACE);
            }
            else
            {
                ::glEnable(GL_CULL_FACE);
            }

            RenderCommand::MultiDrawElementsIndirectCountRaw(
                registry.GetVaoID(), commandBufferID,
                instances[i].Gpu.CommandBase * 32u, // this instance's command segment
                argsBufferID, static_cast<u32>(i * sizeof(VirtualDrawArgs)),
                instances[i].Gpu.ClusterCount, 32u);
        }
        ::glEnable(GL_CULL_FACE); // restore the pass-wide default for the resolve draws below

        // ── 3. Visibility-buffer material resolve: one fullscreen draw per
        // instance (the material model binds textures per draw). Depth test +
        // write stay on: gl_FragDepth replays the visibility buffer's depth,
        // composing SW-rasterized clusters with the HW-rasterized ones.
        if (swThresholdPixels > 0.0f && registry.GetVisbufferBuffer())
        {
            ::glDisable(GL_CULL_FACE); // fullscreen triangle
            m_ResolveShader->Bind();
            registry.GetSwListBuffer()->Bind();
            registry.GetVisbufferBuffer()->Bind();

            const auto fullscreen = MeshPrimitives::GetFullscreenTriangle();
            for (sizet i = 0; i < instances.size(); ++i)
            {
                const auto& mat = FrameDataBufferManager::Get().GetMaterialData(
                    static_cast<u16>(instances[i].MaterialDataIndex));
                CommandDispatch::UploadMaterialForDirectDraw(mat, static_cast<u16>(instances[i].MaterialDataIndex));

                u32 const drawInfo[4] = { static_cast<u32>(i), instances[i].Gpu.CommandBase,
                                          registry.GetVisbufferWidth(), registry.GetVisbufferHeight() };
                m_DrawInfoUBO->SetData(drawInfo, sizeof(drawInfo));

                fullscreen->Bind();
                context.DrawIndexed(fullscreen);
            }
            ::glEnable(GL_CULL_FACE);
        }

        targetFB->Unbind();

        // Overdraw debug: colorize the accumulated per-pixel fragment count into
        // the colour target (heat ramp) so it captures as a readable image. The
        // cluster/LOD modes wrote the colour target directly during the draws.
        if (debugMode == VirtualDebugMode::Overdraw && m_ColorizeShader)
        {
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);
            ::glBindImageTexture(0, registry.GetDebugColorTextureID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
            ::glBindImageTexture(1, registry.GetDebugCountTextureID(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
            m_ColorizeShader->Bind();
            m_ColorizeShader->SetUint("u_Width", registry.GetDebugWidth());
            m_ColorizeShader->SetUint("u_Height", registry.GetDebugHeight());
            m_ColorizeShader->SetFloat("u_OverdrawScale", 8.0f);
            u32 const gx = (registry.GetDebugWidth() + 7u) / 8u;
            u32 const gy = (registry.GetDebugHeight() + 7u) / 8u;
            RenderCommand::DispatchCompute(gx, gy, 1);
        }
        if (debugActive)
        {
            // Publish the debug-colour writes to the subsequent capture read.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
        }

        // Per-sample MSAA rasterized into the multisample G-Buffer — resolve so
        // the single-sample export copies below (and AO / SSR) see the clusters.
        // Non-per-sample MSAA drew straight into the resolved FBO, so no resolve.
        // Resolve() mutates, but we borrow the G-Buffer as a const ref — take a
        // non-const Ref copy (cheap refcount bump) to call it.
        if (perSampleMSAA)
        {
            Ref<GBuffer> resolveTarget = gbuffer;
            resolveTarget->Resolve();
        }

        // Re-export the scene/G-Buffer textures so lighting / GTAO / SSR / TAA
        // and the editor grid see the clusters we just drew — ScenePass copied
        // its exports before we ran (DeferredGPUOcclusionPass idiom). Handles
        // that alias the live attachment self-skip.
        const auto copyExport = [&context, &gbuffer](const RGTextureHandle handle, u32 sourceTextureID, GLenum target)
        {
            if (!handle.IsValid() || sourceTextureID == 0u)
                return;
            u32 const exportedID = context.ResolveTexture(handle);
            if (exportedID == 0u || exportedID == sourceTextureID)
                return;
            ::glCopyImageSubData(sourceTextureID, target, 0, 0, 0, 0,
                                 exportedID, target, 0, 0, 0, 0,
                                 static_cast<GLsizei>(gbuffer->GetWidth()),
                                 static_cast<GLsizei>(gbuffer->GetHeight()), 1);
        };
        copyExport(m_SelectedSceneDepth, gbuffer->GetDepthAttachmentID(), GL_TEXTURE_2D);
        copyExport(m_SelectedVelocity, gbuffer->GetColorAttachmentID(GBuffer::Velocity), GL_TEXTURE_2D);
        copyExport(m_SelectedGBufferAlbedo, gbuffer->GetColorAttachmentID(GBuffer::Albedo), GL_TEXTURE_2D);
        copyExport(m_SelectedGBufferNormal, gbuffer->GetColorAttachmentID(GBuffer::Normal), GL_TEXTURE_2D);
        copyExport(m_SelectedGBufferEmissive, gbuffer->GetColorAttachmentID(GBuffer::Emissive), GL_TEXTURE_2D);

        // Per-sample lighting samples the MULTISAMPLE G-Buffer, so re-export those
        // attachments too (they carry the clusters we just drew into the MS FBO).
        if (perSampleMSAA)
        {
            constexpr GLenum kMS = GL_TEXTURE_2D_MULTISAMPLE;
            copyExport(m_SelectedSceneDepthMS, gbuffer->GetMSDepthAttachmentID(), kMS);
            copyExport(m_SelectedVelocityMS, gbuffer->GetMSColorAttachmentID(GBuffer::Velocity), kMS);
            copyExport(m_SelectedGBufferAlbedoMS, gbuffer->GetMSColorAttachmentID(GBuffer::Albedo), kMS);
            copyExport(m_SelectedGBufferNormalMS, gbuffer->GetMSColorAttachmentID(GBuffer::Normal), kMS);
            copyExport(m_SelectedGBufferEmissiveMS, gbuffer->GetMSColorAttachmentID(GBuffer::Emissive), kMS);
        }
    }
} // namespace OloEngine
