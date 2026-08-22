#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryPass.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
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

        // Mesh-shader hardware raster path (issue #813). Same three-layer
        // demotion shape as the int64 variant above: capability says no → never
        // built; capability says yes but the shader fails → loud warn + MDI;
        // runtime override (VirtualHwRasterMode::ForceMdi) → per-frame routing
        // in Execute. The decision must be loudly observable — a silent
        // fallback here would make every later measurement a measurement of
        // the wrong path.
        if (RenderCommand::SupportsMeshShaders())
        {
            m_MeshletShader = Shader::Create("assets/shaders/VirtualMeshletGBuffer.glsl");
            if (!m_MeshletShader || !m_MeshletShader->IsReady())
            {
                OLO_CORE_WARN("VirtualGeometryPass: mesh shaders advertised but VirtualMeshletGBuffer.glsl failed "
                              "to compile; hardware raster falls back to the MDI vertex pipeline");
                m_MeshletShader = nullptr;
            }
            else
            {
                OLO_CORE_INFO("VirtualGeometryPass: hardware raster path = mesh shaders (VK_EXT_mesh_shader)");
            }
        }
        else
        {
            OLO_CORE_INFO("VirtualGeometryPass: hardware raster path = MDI vertex pipeline "
                          "(device/backend has no mesh-shader support)");
        }
        // Publish the EFFECTIVE route so observers (the MCP stats echo, an A/B
        // harness) see the demotion above, not just the raw device capability.
        VirtualMeshRegistry::Get().SetMeshRasterAvailable(m_MeshletShader != nullptr);
    }

    void VirtualGeometryPass::UploadCullParams(const UBOStructures::VirtualClusterCullUBO& params)
    {
        if (!m_CullParamsUBO)
        {
            m_CullParamsUBO = UniformBuffer::Create(UBOStructures::VirtualClusterCullUBO::GetSize(),
                                                    ShaderBindingLayout::UBO_VIRTUAL_CLUSTER_CULL);
        }
        m_CullParamsUBO->SetData(&params, sizeof(params));
        m_CullParamsUBO->Bind();
    }

    void VirtualGeometryPass::UploadRasterParams(const UBOStructures::VirtualRasterUBO& params)
    {
        if (!m_RasterParamsUBO)
        {
            m_RasterParamsUBO = UniformBuffer::Create(UBOStructures::VirtualRasterUBO::GetSize(),
                                                      ShaderBindingLayout::UBO_VIRTUAL_RASTER);
        }
        m_RasterParamsUBO->SetData(&params, sizeof(params));
        m_RasterParamsUBO->Bind();
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

        // Publish the cluster/LOD/overdraw debug capture target (issue #629) when a
        // debug mode is active. Named so olo_render_capture_target /
        // olo_render_list_targets resolve it.
        //
        // The targets are CREATED here, not only in Execute (issue #607). Setup runs
        // on a graph rebuild, and the rebuild is what a debug-mode change triggers
        // (Renderer3D::RenderPipeline::ComputeBlackboardFingerprint hashes the mode +
        // the debug texture id — see the comment there). If the import waited for
        // Execute to lazily create the texture, the FIRST rebuild would see id 0 and
        // import nothing, so the resource only appeared if some LATER, unrelated
        // change happened to rebuild the graph again. Over MCP that read as
        // "Unknown render-graph resource 'VirtualGeometryDebug'" forever: the mode
        // was only settable from the Statistics panel, whose next ImGui interaction
        // happened to dirty the graph. Creating the texture in Setup makes the
        // target importable on the very rebuild the mode change caused.
        // (Requires a live GL context — Setup runs inside Renderer3D::EndScene on
        // the render thread, same as Execute.)
        auto& registry = VirtualMeshRegistry::Get();
        if (registry.GetDebugMode() != VirtualDebugMode::Off)
        {
            if (m_ScenePass)
            {
                if (const Ref<GBuffer>& gbuffer = m_ScenePass->GetGBuffer(); gbuffer && gbuffer->GetFramebuffer())
                    registry.EnsureDebugTargets(gbuffer->GetWidth(), gbuffer->GetHeight());
            }

            if (registry.GetDebugColorTexture().IsValid())
            {
                // ImportTextureHandle, not ImportTexture: the debug target is an
                // identity now. The MCP capture endpoints still find it because
                // Debug::NativeTextureIdForDiagnostics falls back to the identity
                // when a handle-imported resource resolves natively to 0 (#736).
                [[maybe_unused]] const RGTextureHandle debugTarget = builder.ImportTextureHandle(
                    "VirtualGeometryDebug", registry.GetDebugColorTexture(),
                    RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, "VirtualGeometryDebug"));
            }
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

        // ── Phase-1 Hi-Z inputs (issue #682): the RETAINED pyramid, built at the
        // tail of the previous EndScene from that frame's FINAL depth — so it
        // already contains virtual geometry, which is what lets a VG occluder
        // cull the VG behind it. Read BEFORE anything rebuilds the pyramid
        // in-place (BuildCurrentOcclusionHZB below overwrites this very texture).
        // Gated on the same global toggle as the instance HZB cull (off by
        // default); non-usable on frame 0 → frustum + cone only, no phase 2.
        const GPUFrustumCuller::HZBOcclusionInputs prevHZB = Renderer3D::GetRetainedOcclusionHZB();
        // Two-phase is OFF while the culling camera is frozen (issue #726):
        // BuildCurrentOcclusionHZB below rebuilds the retained pyramid IN PLACE
        // from this frame's depth, which once frozen is the observer's depth --
        // it would destroy the very pyramid phase 1 tested against and re-admit
        // clusters the frozen camera occluded. Single-phase against the frozen
        // pyramid is the honest cut.
        bool const twoPhase = prevHZB.IsUsable() && !Renderer3D::IsCullingCameraFrozen();

        // ── 1. DAG-cut + cull compute, one dispatch per instance ──
        // The cull program's whole parameter set (issue #691 Phase 7): what
        // used to be ~18 bare uniforms poked through ComputeShader::Set* is one
        // std140 block re-uploaded before every dispatch. Declared out here
        // because both phases and the occlusion lambda below fill it.
        UBOStructures::VirtualClusterCullUBO cullParams{};

        // Culling-camera override for the MAIN view (issue #726). Always filled,
        // frozen or not, so the override path in VirtualClusterCull.comp is the
        // one this view exercises every frame rather than a branch that only
        // runs in debug mode. The ortho shadow cascades deliberately leave these
        // at zero and keep reading the camera UBO's light matrices.
        const auto applyCullCameraOverride = [](UBOStructures::VirtualClusterCullUBO& params)
        {
            params.CullViewProjection =
                RHI::AdjustProjectionForBackend(Renderer3D::GetCullViewProjectionRelative());
            params.CullCameraPosition = glm::vec4(Renderer3D::GetCullViewPositionRelative(), 1.0f);
            params.CullProjParams = glm::vec4(Renderer3D::GetCullProjParams(), 0.0f, 0.0f);
        };

        // Re-bind every SSBO the dispatch touches: binding points are
        // process-global GL state shared with other subsystems.
        const auto bindCullResources = [&registry]()
        {
            registry.GetClusterBuffer()->Bind();
            registry.GetGroupBuffer()->Bind();
            registry.GetInstanceBuffer()->Bind();
            registry.GetCommandBuffer()->Bind();
            registry.GetArgsBuffer()->Bind();
            registry.GetVisibleBuffer()->Bind();
            registry.GetSwListBuffer()->Bind();
            registry.GetGroupStatesBuffer()->Bind();
            registry.GetRejectListBuffer()->Bind();
            // The hardware MDI path pulls vertices from SSBO 39 in
            // VirtualMeshGBuffer.glsl. Bind it here rather than relying on the
            // software-raster block (which binds it for its own use) having run
            // first — since issue #682 the phase-1 MDI happens BEFORE that block.
            if (registry.GetVertexBuffer())
                registry.GetVertexBuffer()->Bind();
            // Same rule for the cluster-local index pool at SSBO 42 (#813): the
            // MESH-shader raster path reads it during the hardware phases, and
            // the only other bind lives in the software-raster block — which
            // runs after phase 1 and not at all under swRasterMode=disabled, so
            // relying on it is exactly the stale-binding accident the comment
            // above exists to prevent for binding 39.
            if (registry.GetIndexBuffer().IsValid())
            {
                RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_VIRTUAL_INDICES,
                                                 registry.GetIndexBuffer());
            }
            CommandDispatch::BindSceneResources(); // camera UBO at binding 0
        };
        bindCullResources();

        // Binds one occlusion pyramid's uniforms + texture on the cull program.
        // Texture unit 0 is ALSO u_AlbedoMap for the material draws below. This raw bind
        // happens behind CommandDispatch's redundant-bind cache, which would go on believing
        // slot 0 still holds the last albedo it bound — and then SKIP the real bind for any
        // material whose albedo has that same GL ID, leaving the HZB depth pyramid live in
        // u_AlbedoMap. Tell the cache the slot is dirty so the next material bind is real.
        const auto bindOcclusionInputs = [this, &cullParams](const GPUFrustumCuller::HZBOcclusionInputs& hzb)
        {
            cullParams.OcclusionEnabled = hzb.IsUsable() ? 1 : 0;
            if (!hzb.IsUsable())
                return;
            // Persistent: the retained occlusion pyramid is renderer-owned, not
            // graph-pooled. The shader is bound by the caller before this lambda
            // runs, so the seam's program fork is already correct
            // (issue #691 Phase 3).
            HeapBinding::BindTextureOrOffset(0, hzb.HZBTexture, RHI::HeapSlotLifetime::Persistent);
            // Still needed on the SLOT path, and harmless on the heap path: the
            // fallback issues a real bind behind CommandDispatch's redundant-bind
            // cache, which would otherwise skip a later material bind that happens
            // to reuse this GL id and leave the depth pyramid live in u_AlbedoMap.
            CommandDispatch::InvalidateTextureSlot(0);
            // The SetInt('u_HZB', 0) companion is gone: redundant against the
            // shader's own layout(binding = 0), and a per-frame 'uniform not found'
            // warning under the bindless variant where the name is a #define.
            // A8 seam, shader-reconstruction flavour (#691 Phase 7): same
            // contract as GPUFrustumCuller's uploads — VirtualClusterCull.comp
            // samples the HZB at `ndc.xy * 0.5 + 0.5` against a pyramid built
            // from row-mirrored depth. Row flip only. Identity on GL.
            cullParams.OcclusionViewProjection = RHI::AdjustProjectionForShaderReconstruction(hzb.PrevViewProjection);
            cullParams.HZBSize = hzb.HZBSize;
            cullParams.HZBUVFactor = hzb.HZBUVFactor;
            cullParams.HZBMipCount = static_cast<i32>(hzb.MipCount);
            cullParams.OcclusionDepthBias = hzb.DepthBias;
        };

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

        u32 const instanceCount = static_cast<u32>(instances.size());
        u32 const frameClusterCount = registry.GetTotalFrameClusterCount();
        bool const swEnabled = swThresholdPixels > 0.0f && registry.GetVisbufferBuffer();

        // Re-zero the per-instance draw args (BOTH phase regions) + the SW and
        // reject list headers for the MAIN view: the shadow cascades ran their
        // own culls into the same buffers earlier this frame (PrepareFrame's
        // zeroing only happens on its first call).
        {
            // Non-const Ref copies: the registry hands these out as const refs and
            // SetData mutates (cheap refcount bumps).
            Ref<StorageBuffer> argsBuffer = registry.GetArgsBuffer();
            Ref<StorageBuffer> swListBuffer = registry.GetSwListBuffer();
            Ref<StorageBuffer> rejectListBuffer = registry.GetRejectListBuffer();
            std::vector<VirtualDrawArgs> const zeroArgs(static_cast<sizet>(instanceCount) * 2);
            argsBuffer->SetData(zeroArgs.data(),
                                static_cast<u32>(zeroArgs.size() * sizeof(VirtualDrawArgs)), 0);
            u32 const zeroHeader[4] = { 0, 0, 0, 0 };
            swListBuffer->SetData(zeroHeader, sizeof(zeroHeader), 0);
            rejectListBuffer->SetData(zeroHeader, sizeof(zeroHeader), 0);
        }

        // ── Cull phase 1: full DAG cut + frustum + cone, then the Hi-Z test
        // against the PREVIOUS frame's pyramid. u_OrthoMode defaults to 0 here —
        // perspective main view; the shadow cull binds its own program and forces
        // it off. u_WriteRejected turns an occlusion hit into a deferral rather
        // than a drop, so it is only set when phase 2 is actually going to run:
        // a reject nobody re-tests would be a hole. ──
        m_CullShader->Bind();
        // The former bare uniforms, now ONE std140 block refilled per dispatch
        // (issue #691 Phase 7). Note this is a FRESH struct per phase, so a
        // control the phase does not set reads 0 — which is what the old
        // per-program uniform state gave only by accident, and is why phase 2
        // had to explicitly zero u_DebugDrawClusters below.
        cullParams = UBOStructures::VirtualClusterCullUBO{};
        cullParams.ViewportHeight = static_cast<f32>(gbuffer->GetHeight());
        cullParams.SwRasterThresholdPixels = swThresholdPixels;
        cullParams.Phase2 = 0;
        cullParams.WriteRejected = twoPhase ? 1 : 0;
        // Cluster-bounds visualization (#725) — phase 1 only. Phase 2 re-tests an
        // already-classified subset, so emitting there would draw a second sphere
        // over most of the same clusters and make the "drawn" set look larger
        // than the cut actually is.
        cullParams.DebugDrawClusters = static_cast<i32>(m_ClusterBoundsDebugMode);
        cullParams.DebugDrawClusterStride = std::max(m_ClusterBoundsDebugStride, 1u);
        cullParams.RejectCapacity = frameClusterCount;
        cullParams.CommandSlotBase = 0u;
        cullParams.ArgsSlotBase = 0u;
        applyCullCameraOverride(cullParams);
        bindOcclusionInputs(prevHZB);
        // Once, before the loop. bindOcclusionInputs stages the HZB offset a
        // single time above, so re-publishing it per instance uploaded the same
        // unchanged table N times (issue #691 Phase 3).
        HeapBinding::FlushOffsets();
        for (sizet i = 0; i < instances.size(); ++i)
        {
            cullParams.InstanceIndex = static_cast<u32>(i);
            UploadCullParams(cullParams);
            u32 const groups = (instances[i].Gpu.ClusterCount + 63u) / 64u;
            RenderCommand::DispatchCompute(groups, 1, 1);
        }

        // Command for the indirect-args read, ShaderStorage for the vertex-
        // pulling reads of the visible/instance/vertex buffers.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

        // ── Hardware raster target + state ──
        // Non-per-sample MSAA draws straight into the resolved FBO (ScenePass
        // already resolved it); per-sample MSAA and the non-MSAA path draw into
        // the primary FBO (multisample or single-sample respectively).
        Ref<Framebuffer> targetFB = (msaa && !perSampleMSAA) ? gbuffer->GetSamplingFramebuffer()
                                                             : gbuffer->GetFramebuffer();
        if (!targetFB)
            targetFB = gbuffer->GetFramebuffer();

        if (!m_DrawInfoUBO)
        {
            m_DrawInfoUBO = UniformBuffer::Create(sizeof(VirtualDrawInfoGpu), ShaderBindingLayout::UBO_VIRTUAL_DRAW);
        }

        // Bind the G-Buffer target + the raster state every draw block needs.
        // Replayed before phase 1 and again before phase 2 / the resolve, because
        // the mid-pass Hi-Z build and phase-2 cull rebind program, SSBOs and
        // textures in between (same discipline GPUDrivenOcclusionPass uses).
        const auto bindDrawTarget = [&targetFB, &gbuffer]()
        {
            targetFB->Bind();
            {
                // All five G-Buffer MRTs, same set SceneRenderPass draws
                RenderCommand::RestoreAllFramebufferDrawAttachments(targetFB->GetRHIHandle(), GBuffer::Count);
            }

            // State set UNCONDITIONALLY, deliberately bypassing the context
            // caches: this pass runs between bucket executions whose dispatchers
            // track state in their own caches — a cached "already true" there
            // can leave the real depth test/mask off, which silently drops every
            // depth write (color still lands) and downstream sky/overlay passes
            // then overdraw the clusters. The facade preserves that property:
            // none of these setters early-out on a cached value, they only gate
            // the profiler StateChanges counter. Same discipline as
            // GPUDrivenOcclusionPass.
            RenderCommand::SetViewport(0, 0, gbuffer->GetWidth(), gbuffer->GetHeight());
            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
            RenderCommand::SetDepthMask(true);
            RenderCommand::SetBlendState(false);
            RenderCommand::EnableCulling();
            RenderCommand::SetCullFace(RHI::CullMode::Back);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
            // Per-attachment color masks can be left disabled by earlier passes
            // (per-attachment mask state is indexed and survives a global mask);
            // a masked RT1/RT2 silently drops normal/emissive writes while RT0 +
            // depth land — the lighting pass then shades clusters with cleared
            // G-Buffer data.
            RenderCommand::SetColorMask(true, true, true, true);
            for (u32 attachment = 0; attachment < GBuffer::Count; ++attachment)
            {
                RenderCommand::SetColorMaskForAttachment(attachment, true, true, true, true);
            }
        };

        // Mesh-shader routing (issue #813), decided once per frame and loudly
        // logged at Init: on a capable device the hardware raster replays each
        // instance's visible-cluster segment through the task/mesh pipeline
        // (one task workgroup reads the cull's DrawCount and launches one mesh
        // workgroup per visible cluster) instead of the MDI vertex pipeline.
        // Per-INSTANCE, not per-cluster: a mesh cooked beyond the meshlet
        // limits (kMeshletMaxVertices/kMeshletMaxTriangles) stays on MDI.
        // ForceMdi is the A/B + parity-test lever.
        bool const meshRaster = m_MeshletShader && registry.GetHwRasterMode() == VirtualHwRasterMode::Auto;

        // One indirect draw per instance over one phase's command region —
        // MDI-count for the vertex pipeline, a single task launch for the mesh
        // pipeline. `commandSlotBase` / `argsInstanceBase` select the region:
        // 0/0 for phase 1, the frame's cluster/instance counts for phase 2.
        const auto drawHardwarePhase = [&](u32 commandSlotBase, u32 argsInstanceBase)
        {
            m_GBufferShader->Bind();
            if (debugActive)
            {
                // Image units 0/1 (separate namespace from the sampler texture units).
                //
                // DELIBERATELY NOT CONVERTED to the heap (issue #691 Phase 3). These feed
                // VirtualMeshGBuffer.glsl, and the bindless compile route produces no
                // SPIR-V and therefore never runs Reflect() — so a G-Buffer shader taken
                // down it would have m_IsDeferredCapable stay false and be misrouted out
                // of the deferred producer bucket into the forward-overlay fallback.
                // OpenGLShader::CreateProgramFromRawGLSL detects and errors on exactly
                // this. Converting the CALL SITE alone would be safe (the seam falls back
                // for a program that is not the bindless variant) and would buy nothing
                // but a lower ratchet number, which is the opposite of what the counter
                // is for. Revisit when the bindless route gains a reflection source.
                RenderCommand::BindImageTexture(0, registry.GetDebugColorTexture(), 0, false, 0,
                                                RHI::Access::StorageWrite, RHI::Format::RGBA8UNorm);
                RenderCommand::BindImageTexture(1, registry.GetDebugCountTexture(), 0, false, 0,
                                                RHI::Access::StorageReadWrite, RHI::Format::R32UInt);
            }
            m_DrawInfoUBO->Bind();

            const RHI::ResourceHandle commandBuffer = registry.GetCommandBuffer()->GetRHIHandle();
            const RHI::ResourceHandle argsBuffer = registry.GetArgsBuffer()->GetRHIHandle();
            // Which G-Buffer program is bound right now. Instances can route
            // to different pipelines (a non-meshlet-compatible mesh stays on
            // MDI), so the loop re-binds only when the route actually changes.
            bool meshShaderBound = false;
            sizet const instanceCount = instances.size();
            for (sizet i = 0; i < instanceCount; ++i)
            {
                // Per-instance route — see ShouldUseMeshRaster for why the
                // cluster ceiling is a per-FRAME gate here and not folded into
                // the per-part IsMeshletCompatible stamp.
                bool const useMesh = ShouldUseMeshRaster(meshRaster, instances[i].MeshletCompatible,
                                                         instances[i].Gpu.ClusterCount);
                if (useMesh != meshShaderBound)
                {
                    (useMesh ? m_MeshletShader : m_GBufferShader)->Bind();
                    meshShaderBound = useMesh;
                }

                const auto& mat = FrameDataBufferManager::Get().GetMaterialData(
                    static_cast<u16>(instances[i].MaterialDataIndex));
                CommandDispatch::UploadMaterialForDirectDraw(mat, static_cast<u16>(instances[i].MaterialDataIndex));

                u32 const segmentBase = commandSlotBase + instances[i].Gpu.CommandBase;
                // ArgsSlot is where the TASK stage reads its DrawCount (the MDI
                // vertex stage never reads it) and MaxClusters is its launch
                // clamp — both uploaded identically on both routes so the two
                // paths see the same block contents. The viewport fields belong
                // to the resolve/shadow stages and stay 0 here.
                VirtualDrawInfoGpu drawInfo{};
                drawInfo.InstanceIndex = static_cast<u32>(i);
                drawInfo.CommandBase = segmentBase;
                drawInfo.ArgsSlot = static_cast<u32>(argsInstanceBase + i);
                drawInfo.MaxClusters = instances[i].Gpu.ClusterCount;
                m_DrawInfoUBO->SetData(&drawInfo, sizeof(drawInfo));

                // Two-sided materials must not backface-cull. Foliage is a single sheet of quads
                // meant to be seen from both sides, so culling drops half of every leaf — which is
                // what shredded Sponza's plants. The classic path does the same thing via
                // Renderer3DDrawHelpers::BuildRenderState; this loop drives raw GL, so it has to
                // toggle the state itself.
                if (instances[i].TwoSided)
                {
                    RenderCommand::DisableCulling();
                }
                else
                {
                    RenderCommand::EnableCulling();
                }

                if (useMesh)
                {
                    // One task workgroup: it reads args[drawInfo.z].DrawCount
                    // and launches one mesh workgroup per visible cluster —
                    // the GPU-side count never touches the CPU, same property
                    // as the MDI parameter buffer.
                    RenderCommand::DrawMeshTasks(1u, 1u, 1u);
                }
                else
                {
                    RenderCommand::MultiDrawElementsIndirectCountRaw(
                        registry.GetVao(), commandBuffer,
                        segmentBase * 32u, // this instance's command segment, in this phase's region
                        argsBuffer,
                        static_cast<u32>((argsInstanceBase + i) * sizeof(VirtualDrawArgs)),
                        instances[i].Gpu.ClusterCount, 32u);
                }
            }
            RenderCommand::EnableCulling(); // restore the pass-wide default
        };

        // ── 2. Phase-1 hardware raster ──
        bindDrawTarget();
        drawHardwarePhase(0u, 0u);
        // Unbind so the depth attachment can be sampled by the Hi-Z build below
        // without an attachment/sampler feedback loop.
        targetFB->Unbind();

        // ── 3. Cull phase 2 (issue #682) ──
        // The phase-1 clusters are now in the depth buffer. Rebuild the pyramid
        // from it (opaque scene + phase-1 virtual geometry) and re-test only the
        // reject list. Software-rasterized clusters are NOT in that depth yet —
        // they live in the visibility buffer until the resolve below — which
        // simply makes the pyramid a weaker occluder set, i.e. conservative.
        if (twoPhase)
        {
            // The phase-1 draws just wrote this depth through the fixed-function
            // pipeline; the Hi-Z build samples it as a texture. Order the
            // framebuffer-write -> texture-fetch explicitly (a texture barrier).
            RenderCommand::TextureBarrier();
            const GPUFrustumCuller::HZBOcclusionInputs currentHZB = Renderer3D::BuildCurrentOcclusionHZB(
                gbuffer->GetDepthAttachmentHandle(), gbuffer->GetWidth(), gbuffer->GetHeight());

            // The HZB build bound its own program / SSBOs / images.
            bindCullResources();
            m_CullShader->Bind();
            cullParams = UBOStructures::VirtualClusterCullUBO{};
            cullParams.ViewportHeight = static_cast<f32>(gbuffer->GetHeight());
            cullParams.SwRasterThresholdPixels = swThresholdPixels;
            cullParams.Phase2 = 1;
            cullParams.WriteRejected = 0;
            // Explicitly OFF for phase 2: emitting here would draw a second
            // sphere over every disoccluded cluster. Since #691 Phase 7 the
            // fresh struct already zeroes it — this assignment stays as the
            // statement of intent, not as the mechanism.
            cullParams.DebugDrawClusters = 0;
            cullParams.RejectCapacity = frameClusterCount;
            cullParams.CommandSlotBase = frameClusterCount;
            cullParams.ArgsSlotBase = instanceCount;
            applyCullCameraOverride(cullParams);
            // If the rebuild failed, occlusion goes off for phase 2 and EVERY
            // reject is emitted — a phase-1 reject that is never re-tested is
            // exactly the hole this scheme must not have.
            bindOcclusionInputs(currentHZB);

            // One thread per reject, dispatched at the worst case (every cluster
            // rejected); threads past the live count early-out.
            u32 const rejectGroups = (frameClusterCount + 63u) / 64u;
            u32 const groupsX = std::min(rejectGroups, 4096u);
            u32 const groupsY = (rejectGroups + groupsX - 1u) / std::max(groupsX, 1u);
            // BRACED, and it matters: unbraced, the guard covered only the flush
            // and the dispatch below ran even with no clusters — groupsX = 0 and
            // groupsY = (0 + 0 - 1u) / 1 = 4294967295, i.e. a 4-billion-group
            // dispatch on an empty frame.
            if (rejectGroups > 0)
            {
                // Publish the HZB offset staged by bindOcclusionInputs before the
                // dispatch reads it (issue #691 Phase 3).
                HeapBinding::FlushOffsets();
                // One dispatch, one refill. u_InstanceIndex stays 0 here and
                // that is correct: Phase2() returns before main() ever reads it
                // — the reject record carries its own InstanceIndex. Under the
                // old bare uniforms this slot merely retained whatever the last
                // phase-1 instance had left in the program.
                UploadCullParams(cullParams);
                RenderCommand::DispatchCompute(groupsX, groupsY, 1);
            }

            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);
        }

        // ── 4. Compute software rasterizer (portable two-pass 2x32 scheme):
        // phase 0 atomicMin-compacts depth, phase 1 writes the winning
        // payloads. Dispatched at the conservative total-cluster upper bound;
        // workgroups beyond the live SW count early-out (issue #551 idiom).
        // Runs ONCE, after both cull phases, over the union work list — the two
        // phases append to the same SW list, so a single dispatch covers both.
        if (swEnabled)
        {
            registry.GetVertexBuffer()->Bind();
            registry.GetVisbufferBuffer()->Bind();
            RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_VIRTUAL_INDICES,
                                             registry.GetIndexBuffer());

            u32 const maxSwRecords = frameClusterCount;
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
            // Former bare uniforms, one std140 block (issue #691 Phase 7).
            UBOStructures::VirtualRasterUBO rasterParams{};
            rasterParams.ViewportWidth = registry.GetVisbufferWidth();
            rasterParams.ViewportHeight = registry.GetVisbufferHeight();
            if (useInt64)
            {
                // One atomicMin per covered pixel resolves depth + payload
                // together. u_Phase is unread by this variant; the shared block
                // still carries it (declared verbatim in both), so it simply
                // stays at its zeroed value.
                UploadRasterParams(rasterParams);
                RenderCommand::DispatchCompute(groupsX, groupsY, 1);
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
            }
            else
            {
                // Phase 0 atomic-min-compacts the depth word; phase 1 plain-writes
                // the winning payload where the depth bits match.
                rasterParams.Phase = 0;
                UploadRasterParams(rasterParams);
                RenderCommand::DispatchCompute(groupsX, groupsY, 1);
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
                rasterParams.Phase = 1;
                UploadRasterParams(rasterParams);
                RenderCommand::DispatchCompute(groupsX, groupsY, 1);
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
            }
        }

        // ── 5. Phase-2 hardware raster + visibility-buffer material resolve ──
        if (twoPhase || swEnabled)
        {
            bindDrawTarget();

            if (twoPhase)
            {
                drawHardwarePhase(frameClusterCount, instanceCount);
            }

            // One fullscreen draw per instance (the material model binds textures
            // per draw). Depth test + write stay on: gl_FragDepth replays the
            // visibility buffer's depth, composing SW-rasterized clusters with the
            // HW-rasterized ones from both phases.
            if (swEnabled)
            {
                RenderCommand::DisableCulling(); // fullscreen triangle
                m_ResolveShader->Bind();
                registry.GetSwListBuffer()->Bind();
                registry.GetVisbufferBuffer()->Bind();

                const auto fullscreen = MeshPrimitives::GetFullscreenTriangle();
                for (sizet i = 0; i < instances.size(); ++i)
                {
                    const auto& mat = FrameDataBufferManager::Get().GetMaterialData(
                        static_cast<u16>(instances[i].MaterialDataIndex));
                    CommandDispatch::UploadMaterialForDirectDraw(mat, static_cast<u16>(instances[i].MaterialDataIndex));

                    VirtualDrawInfoGpu drawInfo{};
                    drawInfo.InstanceIndex = static_cast<u32>(i);
                    drawInfo.CommandBase = instances[i].Gpu.CommandBase;
                    drawInfo.ViewportWidth = registry.GetVisbufferWidth();
                    drawInfo.ViewportHeight = registry.GetVisbufferHeight();
                    m_DrawInfoUBO->SetData(&drawInfo, sizeof(drawInfo));

                    fullscreen->Bind();
                    // UploadMaterialForDirectDraw above writes this instance's
                    // per-material heap offsets into the material UBO; publish
                    // whatever it also staged on the shared table before the draw
                    // reads it (issue #691 Phase 3).
                    context.FlushHeapOffsets();
                    context.DrawIndexed(fullscreen);
                }
                RenderCommand::EnableCulling();
            }

            targetFB->Unbind();
        }

        // Overdraw debug: colorize the accumulated per-pixel fragment count into
        // the colour target (heat ramp) so it captures as a readable image. The
        // cluster/LOD modes wrote the colour target directly during the draws.
        if (debugMode == VirtualDebugMode::Overdraw && m_ColorizeShader)
        {
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess);
            // THE SHADER IS BOUND FIRST, and under the heap that ordering is
            // load-bearing: the binding seam asks Shader::IsBoundProgramBindless() to
            // choose between writing an offset and issuing a bind, and that flag
            // describes the program in flight. Binding the images first would take the
            // fallback path even with the heap enabled.
            m_ColorizeShader->Bind();
            // Shares VirtualRasterParams with the SW raster (issue #691 Phase 7);
            // the colorize's former u_Width/u_Height ARE ViewportWidth/Height.
            UBOStructures::VirtualRasterUBO colorizeParams{};
            colorizeParams.ViewportWidth = registry.GetDebugWidth();
            colorizeParams.ViewportHeight = registry.GetDebugHeight();
            colorizeParams.OverdrawScale = 8.0f;
            UploadRasterParams(colorizeParams);
            // Persistent: the debug targets are registry-owned and survive the frame.
            HeapBinding::BindImageOrOffset(0, registry.GetDebugColorTexture(), 0, false, 0,
                                           RHI::Access::StorageWrite, RHI::Format::RGBA8UNorm,
                                           RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindImageOrOffset(1, registry.GetDebugCountTexture(), 0, false, 0,
                                           RHI::Access::StorageRead, RHI::Format::R32UInt,
                                           RHI::HeapSlotLifetime::Persistent);
            HeapBinding::FlushOffsets();
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
        const auto copyExport = [&context, &gbuffer](const RGTextureHandle handle,
                                                     RHI::ResourceHandle sourceTextureID,
                                                     RendererAPI::TextureTargetType target)
        {
            if (!handle.IsValid() || !sourceTextureID.IsValid())
                return;
            const RHI::ResourceHandle exportedID = context.ResolveTextureHandle(handle);
            if (!exportedID.IsValid() || exportedID == sourceTextureID)
                return;
            RenderCommand::CopyImageSubData(sourceTextureID, target, exportedID, target,
                                            gbuffer->GetWidth(), gbuffer->GetHeight());
        };
        copyExport(m_SelectedSceneDepth, gbuffer->GetDepthAttachmentHandle(), RendererAPI::TextureTargetType::Texture2D);
        copyExport(m_SelectedVelocity, gbuffer->GetColorAttachmentHandle(GBuffer::Velocity), RendererAPI::TextureTargetType::Texture2D);
        copyExport(m_SelectedGBufferAlbedo, gbuffer->GetColorAttachmentHandle(GBuffer::Albedo), RendererAPI::TextureTargetType::Texture2D);
        copyExport(m_SelectedGBufferNormal, gbuffer->GetColorAttachmentHandle(GBuffer::Normal), RendererAPI::TextureTargetType::Texture2D);
        copyExport(m_SelectedGBufferEmissive, gbuffer->GetColorAttachmentHandle(GBuffer::Emissive), RendererAPI::TextureTargetType::Texture2D);

        // Per-sample lighting samples the MULTISAMPLE G-Buffer, so re-export those
        // attachments too (they carry the clusters we just drew into the MS FBO).
        if (perSampleMSAA)
        {
            constexpr auto kMS = RendererAPI::TextureTargetType::Texture2DMultisample;
            copyExport(m_SelectedSceneDepthMS, gbuffer->GetMSDepthAttachmentHandle(), kMS);
            copyExport(m_SelectedVelocityMS, gbuffer->GetMSColorAttachmentHandle(GBuffer::Velocity), kMS);
            copyExport(m_SelectedGBufferAlbedoMS, gbuffer->GetMSColorAttachmentHandle(GBuffer::Albedo), kMS);
            copyExport(m_SelectedGBufferNormalMS, gbuffer->GetMSColorAttachmentHandle(GBuffer::Normal), kMS);
            copyExport(m_SelectedGBufferEmissiveMS, gbuffer->GetMSColorAttachmentHandle(GBuffer::Emissive), kMS);
        }
    }
} // namespace OloEngine
