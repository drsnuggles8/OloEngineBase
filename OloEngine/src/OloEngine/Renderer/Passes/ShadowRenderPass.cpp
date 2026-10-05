#include "OloEnginePCH.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Shadow/TerrainShadowRevision.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryShadow.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Groom/GroomShadowWidening.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <cstdio>
#include <cstring>
#include <limits>

namespace OloEngine
{
    struct ShadowMeshBatch
    {
        RHI::ResourceHandle drawVao;
        u32 indexCount;
        u32 baseIndex;
        bool twoSided; // rendered with culling disabled instead of front-cull (issue #650)
        TArray64<InstanceData> instances;
    };
    template<>
    struct TIsTriviallyRelocatable<ShadowMeshBatch>
    {
        // Scalar draw identity plus an audited heap-owned instance array.
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(ShadowMeshBatch::drawVao)> &&
                                      TIsTriviallyRelocatable_V<decltype(ShadowMeshBatch::indexCount)> &&
                                      TIsTriviallyRelocatable_V<decltype(ShadowMeshBatch::baseIndex)> &&
                                      TIsTriviallyRelocatable_V<decltype(ShadowMeshBatch::twoSided)> &&
                                      TIsTriviallyRelocatable_V<decltype(ShadowMeshBatch::instances)>;
    };

    ShadowRenderPass::ShadowRenderPass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("ShadowRenderPass");
    }

    void ShadowRenderPass::AppendDeclarationInputs(RGDeclarationKey& key) const
    {
        key.Add(m_ShadowMap && m_ShadowMap->IsEnabled());
    }

    void ShadowRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        // Skip declaring writes when shadow rendering is off — otherwise the
        // graph records dependency edges that never materialise and consumers
        // may end up reading uncleared shadow maps. Execute() already gates
        // on the same condition.
        if (!m_ShadowMap || !m_ShadowMap->IsEnabled())
            return;

        if (blackboard.Shadows.ShadowMapCSM.IsValid())
        {
            for (u32 cascade = 0; cascade < ShadowMap::MAX_CSM_CASCADES; ++cascade)
            {
                if (const auto cascadeView = blackboard.Shadows.ShadowMapCSMCascades[cascade]; cascadeView.IsValid())
                {
                    builder.Write(cascadeView, RGWriteUsage::DepthStencil);
                }
                else
                {
                    builder.Write(blackboard.Shadows.ShadowMapCSM, RGWriteUsage::DepthStencil, RGSubresourceRange::Layer(cascade));
                }
            }
        }

        // The local-light shadow atlas is one depth target: every prioritised
        // spot / point-face tile renders into sub-rects of it via per-entry
        // viewports, so a single DepthStencil write declaration covers all of
        // them (issue #435).
        if (blackboard.Shadows.ShadowMapAtlas.IsValid())
        {
            builder.Write(blackboard.Shadows.ShadowMapAtlas, RGWriteUsage::DepthStencil);
        }
    }

    ShadowRenderPass::~ShadowRenderPass() = default;

    void ShadowRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Create a depth-only framebuffer. The internal depth texture created by
        // Invalidate() will be replaced per-cascade via AttachDepthTextureArrayLayer.
        FramebufferSpecification shadowSpec;
        shadowSpec.Width = spec.Width;
        shadowSpec.Height = spec.Height;
        shadowSpec.Attachments = { FramebufferTextureFormat::ShadowDepth };
        m_ShadowFramebuffer = Framebuffer::Create(shadowSpec);

        // The groom route into the Virtual Shadow Map (issue #1323). A SECOND
        // shader rather than the cascade one because the VSM has no depth
        // attachment: its fragment stage resolves a page table and does an
        // imageAtomicMin into the physical pool. The vertex maths is shared
        // verbatim through include/GroomShadowWidening.glsl, so the two
        // techniques cannot drift about where a coat's shadow is.
        //
        // ONE params UBO, not one per item, because the VSM raster is a single
        // SEQUENTIAL region -- its clip levels are drawn one after another.
        // The cascade region forks, which is the only reason that one is a
        // per-item pool.
        m_GroomVsmDepthShader = Shader::Create("assets/shaders/VSM_GroomDepth.glsl");
        m_FamilyViewUBO = UniformBuffer::Create(sizeof(FamilyViewParams), ShaderBindingLayout::UBO_USER_0);
        m_FamilyVsmShaders.Terrain = Shader::Create("assets/shaders/VSM_Terrain_Depth.glsl");
        m_FamilyVsmShaders.Voxel = Shader::Create("assets/shaders/VSM_Terrain_VoxelDepth.glsl");
        m_FamilyVsmShaders.VoxelQuad = Shader::Create("assets/shaders/VSM_Terrain_VoxelGreedyDepth.glsl");
        m_FamilyVsmShaders.Foliage = Shader::Create("assets/shaders/VSM_Foliage_Depth.glsl");
        m_FamilyVsmShaders.FoliageImpostor = Shader::Create("assets/shaders/VSM_Foliage_Impostor_Depth.glsl");
        m_GroomVsmParamsUBO = UniformBuffer::Create(UBOStructures::GroomShadowParamsUBO::GetSize(),
                                                    ShaderBindingLayout::UBO_USER_0);
    }

    void ShadowRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        (void)context;

        // The opaque copies describe THIS frame's split or nothing (#1533): a
        // frame that renders no groom-split region -- no groom caster, VSM
        // owning the sun, no local shadow, shadows off -- must not leave a groom
        // sampling a copy an earlier frame wrote.
        if (m_ShadowMap)
        {
            m_ShadowMap->SetOpaqueCopyWritten(ShadowMap::OpaqueCopy::Cascades, false);
            m_ShadowMap->SetOpaqueCopyWritten(ShadowMap::OpaqueCopy::Atlas, false);
        }

        // Virtual geometry counts as a caster HERE too, not only in the per-cascade skip below.
        //
        // This is the outer door: with all five classic caster lists empty, Execute() RETURNS and
        // the shadow pass does not run at all. Virtual-geometry casters never enter those lists —
        // they live in VirtualMeshRegistry and are culled on the GPU — so a scene whose only
        // shadow casters are VirtualMeshComponents produced NO SHADOW MAP WHATSOEVER, and did so
        // silently (the "no shadow casters submitted" warning below is suppressed precisely
        // because, as far as this list is concerned, nothing was submitted).
        //
        // Nanite shadows have therefore only ever worked by accident: every scene and every test
        // that exercised them happened to contain a classic MeshComponent — a ground plane —
        // holding this gate open. Delete the ground from the Nanite stress scene and 24 dragons
        // stop casting, which is exactly how this surfaced.
        // Root-cause early-out for issue #522: when no light requested shadows this
        // frame the CSM/spot/point matrices are stale (identity), so rendering any
        // caster against them is pure waste (GPU + ×N cascade/face re-submission).
        // Scene already skips caster submission in this case, but gate here too so a
        // caster leaking through any other path can never paint a stale shadow map.
        const bool shadowsRequested = m_ShadowMap && m_ShadowMap->AnyShadowsRequested();

        // Grooms (issue #1323), gathered HERE rather than submitted by Scene:
        // the buffers a groom caster draws are built from cooked curves by
        // GroomRenderPass's cache, and that pass runs AFTER this one -- which is
        // precisely why a coat cast no shadow in any technique. So this pass
        // borrows the geometry through GroomRenderPass::AcquireShadowCaster.
        //
        // GATED ON SHADOWS BEING ACTIVE so a frame with shadows off does not
        // pay a cache build this pass will not use. GroomRenderPass acquires the
        // same entries moments later either way, and a GPU-deformed coat's
        // frame buffer is uploaded once per frame whichever pass asks first, so
        // nothing is built twice and nothing is built that was not going to be.
        if (shadowsRequested && m_ShadowMap && m_ShadowMap->IsEnabled())
        {
            CollectGroomCasters();
        }

        // Virtual geometry counts as a caster here too, and since #1323 so do grooms.
        const bool hasCasters = !m_MeshCasters.IsEmpty() || !m_SkinnedCasters.IsEmpty() ||
                                !m_TerrainCasters.IsEmpty() || !m_VoxelCasters.IsEmpty() ||
                                !m_FoliageCasters.IsEmpty() || !m_GroomCasters.IsEmpty() ||
                                AnyVirtualShadowCaster();

        const bool retireFamilyPages = m_ShadowMap && m_ShadowMap->IsVirtualShadowMapActive() &&
                                       std::ranges::any_of(m_FamilyCaches, [](const ShadowFamilyCache& cache)
                                                           { return cache.HasPreviousCasters(); });
        if (!m_ShadowMap || !m_ShadowMap->IsEnabled() || !shadowsRequested || (!hasCasters && !retireFamilyPages))
        {
            // Only warn about the genuinely suspicious case: shadows enabled AND
            // requested by a light, yet nothing was submitted to cast them.
            if (!m_WarnedOnce && shadowsRequested && !hasCasters && m_ShadowMap && m_ShadowMap->IsEnabled())
            {
                OLO_CORE_WARN("ShadowRenderPass::Execute skipped: no shadow casters submitted");
                m_WarnedOnce = true;
            }
            // Clear caster lists for next frame
            m_MeshCasters.Reset();
            m_SkinnedCasters.Reset();
            m_TerrainCasters.Reset();
            m_VoxelCasters.Reset();
            m_FoliageCasters.Reset();
            m_GroomCasters.Reset();
            return;
        }

        if (!m_ShadowFramebuffer)
        {
            OLO_CORE_ERROR("ShadowRenderPass::Execute: Shadow framebuffer not initialized!");
            return;
        }

        const u32 resolution = m_ShadowMap->GetResolution();

        // The CSM resolution is scene-driven (ShadowMap::SetSettings recreates
        // the textures on load), but nothing resized THIS framebuffer with it —
        // it kept its startup spec. GL forgave the mismatch (an FBO has no
        // intrinsic size; glClear covers the whole attached 4096 layer), but on
        // Vulkan the framebuffer spec IS the rendering scope's render area, so
        // a stale 1024 spec cleared and rendered only the top-left quarter of
        // each 4096 cascade while sampling spanned the full layer — depth 0
        // everywhere else, every fragment fully shadowed, and the whole
        // directional-light term vanished from the frame (#691; found
        // via olo_render_capture_target on ShadowMapCSMCascade0: GL all-white,
        // Vulkan black with one white quarter).
        if (m_ShadowFramebuffer->GetSpecification().Width != resolution ||
            m_ShadowFramebuffer->GetSpecification().Height != resolution)
        {
            m_ShadowFramebuffer->Resize(resolution, resolution);
        }

        // Save current viewport
        const auto prevViewport = RenderCommand::GetViewport();

        // Bind shadow framebuffer and set viewport to shadow resolution
        m_ShadowFramebuffer->Bind();
        RenderCommand::SetViewport(0, 0, resolution, resolution);

        // Render state for shadow rendering: depth test on, depth write on, no color
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetColorMask(false, false, false, false);

        // Use front-face culling during shadow pass to reduce peter-panning
        RenderCommand::EnableCulling();
        RenderCommand::FrontCull();

        // ── Directional light: Virtual Shadow Maps (issue #702) ──
        //
        // Replaces the four fixed cascades below when enabled. The atlas block
        // after it still runs, but with VSM LocalLights on (the default) the
        // atlas has zero entries (Scene.cpp sets the count to 0) and does
        // nothing; with LocalLights off, a scene can use VSM for the sun and the
        // priority-ranked atlas for its spots and points.
        //
        // The page management runs HERE, at the start of the frame, on the pages
        // VirtualShadowMapMarkPass marked at the end of the last one; only the
        // marking needs the scene depth buffer, which does not exist yet. See the
        // frame-ordering comment in VirtualShadowMap.h.
        const bool useVirtualShadowMap = m_ShadowMap->IsVirtualShadowMapActive();
        if (useVirtualShadowMap)
        {
            // Clear the CSM cascades anyway, before doing any VSM work.
            //
            // VSM receivers use the physical pool. Clear unused CSM depth so
            // consumers that explicitly request legacy cascades see defined far
            // depth while VSM owns the directional light.
            //
            // Done here, while m_ShadowFramebuffer is still the bound target: the
            // VSM raster binds a framebuffer of its own.
            if (const auto& csmArray = m_ShadowMap->GetCSMTextureArray(); csmArray)
            {
                RenderCommand::DisableScissorTest();
                for (u32 cascade = 0; cascade < ShadowMap::MAX_CSM_CASCADES; ++cascade)
                {
                    m_ShadowFramebuffer->AttachDepthTextureArrayLayer(csmArray->GetRHIHandle(), cascade);
                    RenderCommand::ClearDepthOnly();
                }
            }

            auto& vsm = m_ShadowMap->GetVirtualShadowMap();

            // ── Virtual geometry into the VSM pages (issue #1149) ──
            //
            // Prepared FIRST, ahead of everything else in this block, because it
            // is what runs VirtualMeshRegistry::PrepareFrame — and the swept
            // bounds the invalidation below needs come out of that frame's
            // instance list. One ViewResources for every clip level: the VSM
            // raster is sequential, so the levels reuse one set.
            const bool virtualPrepared = VirtualGeometryShadow::PrepareViews(
                std::span<VirtualGeometryShadow::ViewResources>(&m_VsmVirtualResources, 1));
            // Collected whether or not anything is left to draw: this also
            // CLEARS the list, and the departure half of the invalidation below
            // has to run on a frame with NO virtual casters at all — which is
            // precisely the frame the last one was deleted on.
            const bool haveVirtualCasters = CollectVirtualCasterBounds();

            // BEFORE UpdatePages, which consumes the invalidations: running it
            // after would allocate and clear this frame's pages first, leaving a
            // mover's old silhouette baked into a page now marked clean.
            vsm.SubmitDynamicInvalidations({ m_MeshCasters.GetData(), static_cast<sizet>(m_MeshCasters.Num()) }, { m_SkinnedCasters.GetData(), static_cast<sizet>(m_SkinnedCasters.Num()) }, Renderer3D::GetRenderOrigin());
            // Unconditional, for the same reason: with both lists empty it does
            // nothing, and with only the PREVIOUS list populated it is the only
            // thing that retires a deleted caster's shadow.
            SubmitVirtualDynamicInvalidations(vsm);
            // Grooms too (#1523), and unconditionally for the same departure
            // reason: m_GroomCasters was collected at the top of Execute.
            SubmitGroomDynamicInvalidations(vsm);
            SubmitFamilyDynamicInvalidations(vsm);
            vsm.UpdatePages();

            const bool vsmVirtualCasters =
                virtualPrepared && haveVirtualCasters &&
                VirtualGeometryShadow::PrepareVirtualShadowMapRoute(m_VsmVirtualResources);

            const auto uploadBones = [](const ShadowSkinnedCaster& caster, UniformBuffer& animUBO) -> bool
            {
                animUBO.Bind();
                if (caster.boneCount == 0)
                    return true; // nothing to upload, not a failure
                const auto count = std::min(caster.boneCount,
                                            static_cast<u32>(ShaderBindingLayout::AnimationUBO::MAX_BONES));
                // Range-checked: this uploads `count` matrices from the returned
                // pointer, and GetBoneMatrixPtr validates only the first one.
                const glm::mat4* boneMatrices = FrameDataBufferManager::Get().GetBoneMatrixRange(caster.boneBufferOffset, count);
                if (!boneMatrices)
                    return false; // GetBoneMatrixRange already logged the range
                animUBO.SetData(boneMatrices, count * sizeof(glm::mat4));
                return true;
            };

            // The clip levels worth a cluster-cull dispatch: those whose ortho
            // frustum any shadow-casting virtual instance actually reaches. A
            // level nothing touches is dropped here rather than dispatched and
            // rejected thread by thread on the GPU — sixteen levels against four
            // cascades is the one place this route could cost more than the one
            // it replaces, and this is what keeps it from doing so.
            bool virtualLevelsToDraw = false;
            if (vsmVirtualCasters)
            {
                BuildVirtualClipViews(vsm);
                virtualLevelsToDraw = !m_VsmClipViews.IsEmpty();
            }

            // ONE external route that runs BOTH families, not two seams.
            // RenderCasters takes a single ExternalCasterRenderer, and the
            // scope it is invoked in -- framebuffer, viewport, page table,
            // dirty-page pyramid -- is what both need; composing here keeps
            // grooms inside that scope without widening the VSM's own API for
            // a second caller (issue #1323).
            const bool groomLevelsToDraw = !m_GroomCasters.IsEmpty() && m_GroomVsmDepthShader &&
                                           m_GroomVsmDepthShader->IsReady() && m_GroomVsmParamsUBO;
            VirtualShadowMap::ExternalCasterRenderer renderVirtualCasters;
            const bool familyViewsToDraw = !m_TerrainCasters.IsEmpty() || !m_VoxelCasters.IsEmpty() || !m_FoliageCasters.IsEmpty();
            if (familyViewsToDraw)
                EnsureItemResources(1, 64);
            if (virtualLevelsToDraw || groomLevelsToDraw || familyViewsToDraw)
            {
                renderVirtualCasters = [this, &vsm, virtualLevelsToDraw, groomLevelsToDraw, familyViewsToDraw]()
                {
                    u32 drawn = 0;
                    if (virtualLevelsToDraw)
                    {
                        drawn += VirtualGeometryShadow::RenderVirtualShadowMapLevels(
                            { m_VsmClipViews.GetData(), static_cast<sizet>(m_VsmClipViews.Num()) }, VSM::kVirtualResolution,
                            [&vsm]()
                            { vsm.BindPhysicalPoolImage(); }, m_VsmVirtualResources);
                    }
                    if (groomLevelsToDraw)
                    {
                        drawn += RenderGroomVirtualShadowLevels(vsm);
                    }
                    if (familyViewsToDraw)
                        drawn += RenderFamilyVirtualViews(vsm, false);
                    return drawn;
                };
            }

            vsm.RenderCasters({ m_MeshCasters.GetData(), static_cast<sizet>(m_MeshCasters.Num()) }, { m_SkinnedCasters.GetData(), static_cast<sizet>(m_SkinnedCasters.Num()) }, Renderer3D::GetRenderOrigin(), uploadBones,
                              renderVirtualCasters, familyViewsToDraw ? VirtualShadowMap::ExternalCasterRenderer([this, &vsm]()
                                                                                                                 { return RenderFamilyVirtualViews(vsm, true); })
                                                                      : VirtualShadowMap::ExternalCasterRenderer{});
            vsm.EndFrame();

            // RenderCasters binds a framebuffer of its own (the virtual-resolution
            // raster scope) and leaves the DEFAULT one bound. Restore this pass's
            // target and viewport, or the local-light atlas block below clears and
            // renders into the back buffer instead of the atlas.
            m_ShadowFramebuffer->Bind();
            RenderCommand::SetViewport(0, 0, resolution, resolution);
            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthMask(true);
            RenderCommand::SetColorMask(false, false, false, false);
            RenderCommand::EnableCulling();
            RenderCommand::FrontCull();
        }

        // Each active shadow view records all caster families in one item.
        // Upload objects and cull outputs are prepared before either region.
        const bool recordingInstancedDraws = RendererProfiler::GetInstance().IsRecordingInstancedDraws();

        // The largest upload any item can make: one batch holds at most every
        // static caster, and the per-caster uploads (skinned, voxel) hold one.
        const u32 itemInstanceCapacity = std::max<u32>(64u, static_cast<u32>(m_MeshCasters.Num()));

        // Resolved per region, not once up front, so a frame makes exactly the
        // library lookups it made before (a VSM frame with no atlas entries made
        // none, and the skinned lookup only ever ran with skinned casters present).
        // See ShadowCasterShaders for why an item does not do this itself.
        ShadowCasterShaders casterShaders;
        const auto resolveCasterShaders = [this, &casterShaders]()
        {
            casterShaders.Mesh = Renderer3D::GetShaderLibrary().Get("ShadowDepth");
            if (!m_SkinnedCasters.IsEmpty())
            {
                casterShaders.Skinned = Renderer3D::GetShaderLibrary().Get("ShadowDepthSkinned");
            }
            casterShaders.Voxel = Renderer3D::GetVoxelDepthShader();
            casterShaders.VoxelQuad = Renderer3D::GetVoxelGreedyDepthShader();
            if (!m_TerrainCasters.IsEmpty())
            {
                casterShaders.Terrain = Renderer3D::GetShaderLibrary().Get("Terrain_Depth");
                if (!casterShaders.Terrain)
                    casterShaders.Terrain = Renderer3D::GetTerrainDepthShader();
            }
            if (!m_GroomCasters.IsEmpty())
            {
                casterShaders.Groom = Renderer3D::GetShaderLibrary().Get("GroomStrandDepth");
                // READY, not merely non-null, and dropped here so ONE predicate
                // serves the draw, the tally and the panel. OpenGLShader::Bind()
                // returns WITHOUT issuing glUseProgram on a Failed program, so
                // drawing the groom VAOs after it would replay them through
                // whichever depth program the previous caster family left bound --
                // garbage occluder depth in the cascade rather than simply casting
                // nothing. Nulling it makes the family ABSENT, which is a state
                // the counters report honestly. Same check the VSM route makes.
                if (casterShaders.Groom && !casterShaders.Groom->IsReady())
                {
                    casterShaders.Groom = nullptr;
                }
            }
        };

        // THE OPAQUE COPIES (#1533). A groom's strands must be shadowed by the
        // body they grow on, and a map with the fur in it cannot say so: sampled
        // at a strand, it occludes the coat with its own strands, which the
        // density volume already counts -- so grooms used to sample it at the
        // coat's light-exit point, OUTSIDE the body, and the body shadowed none
        // of its fur. So a region with a groom caster renders its opaque
        // casters, copies the map into ShadowMap's opaque copy, and then takes
        // its grooms on top, uncleared. The strands sample the copy where they
        // are; every other receiver samples the whole map.
        const bool splitForGrooms =
            !m_GroomCasters.IsEmpty() && m_HasGroomReceivers && !Levers::FaultGroomShadowAtCoatExit();
        const auto recordRegionForGrooms =
            [&](const ShadowPassType type, const ShadowMap::OpaqueCopy which,
                const std::function<void(const ActiveShadowView&)>& selectTarget, const bool clearPerItem,
                const std::function<void()>& copyToOpaque)
        {
            if (splitForGrooms && casterShaders.Groom && m_ShadowMap->EnsureOpaqueCopy(which))
            {
                RecordShadowRegion(type, casterShaders, recordingInstancedDraws, itemInstanceCapacity, selectTarget,
                                   clearPerItem, ShadowCasterFilter::NoGrooms);
                copyToOpaque();
                RecordShadowRegion(type, casterShaders, recordingInstancedDraws, itemInstanceCapacity, selectTarget,
                                   /*clearPerItem=*/false, ShadowCasterFilter::GroomsOnly);
                m_ShadowMap->SetOpaqueCopyWritten(which, true);
                return;
            }
            RecordShadowRegion(type, casterShaders, recordingInstancedDraws, itemInstanceCapacity, selectTarget,
                               clearPerItem);
        };

        // Render CSM cascades (skipped entirely when VSM owns the directional light)
        if (const auto& csmArray = m_ShadowMap->GetCSMTextureArray(); csmArray && !useVirtualShadowMap)
        {
            if (!m_LoggedOnce)
            {
                OLO_CORE_INFO("ShadowRenderPass: Rendering {} CSM cascades, resolution={}, FBO={}, textureID={}",
                              ShadowMap::MAX_CSM_CASCADES, resolution, m_ShadowFramebuffer->GetRHIHandle(), csmArray->GetRHIHandle());
                m_LoggedOnce = true;
            }

            // Collect the cascades that will render this frame — the region's
            // items, in cascade order — before any of them records.
            m_ActiveViews.Reset();
            for (u32 cascade = 0; cascade < ShadowMap::MAX_CSM_CASCADES; ++cascade)
            {
                const glm::mat4& lightVP = m_ShadowMap->GetCSMMatrix(cascade);
                const Frustum cascadeFrustum(lightVP);

                // Skip cascade if no bounded casters pass the frustum test and no unbounded
                // casters (terrain, foliage, voxel, VIRTUAL GEOMETRY) exist.
                //
                // Virtual geometry belongs in the UNBOUNDED set, and leaving it out was a silent
                // correctness bug: this early-`continue` skips the cascade entirely, so a scene
                // whose only shadow casters were VirtualMeshComponents cast NO SHADOWS AT ALL —
                // RenderCascadeOrFace (which is what calls VirtualGeometryShadow::RenderCascade)
                // never ran. It only looked like it worked because every test scene happened to
                // contain at least one classic MeshComponent — a ground plane — holding the
                // cascade open for it. Delete the ground and the Nanite shadows vanish with it,
                // which is exactly how this was found.
                //
                // "Unbounded" rather than frustum-tested because the registry's per-instance
                // bounds are not in the CPU-side caster lists at all; the cluster cull does the
                // culling on the GPU, per cluster, against this same cascade frustum. Asking
                // whether ANY virtual instance casts is the cheap, conservative CPU-side answer —
                // and it is precisely the question VirtualGeometryShadow::RenderCascade itself
                // asks before doing any work, so a cascade opened here for virtual geometry that
                // then turns out to be empty costs a framebuffer attach and a depth clear, not a
                // dispatch.
                const bool hasUnbounded = !m_TerrainCasters.IsEmpty() ||
                                          !m_FoliageCasters.IsEmpty() ||
                                          !m_VoxelCasters.IsEmpty() ||
                                          AnyVirtualShadowCaster();
                if (!hasUnbounded)
                {
                    const bool anyMesh = std::ranges::any_of(m_MeshCasters,
                                                             [&](const ShadowMeshCaster& c)
                                                             { return !ShouldCull(c.WorldBounds, cascadeFrustum); });
                    const bool anySkinned = !anyMesh && std::ranges::any_of(m_SkinnedCasters,
                                                                            [&](const ShadowSkinnedCaster& c)
                                                                            { return !ShouldCull(c.WorldBounds, cascadeFrustum); });
                    // GROOMS ARE BOUNDED CASTERS (#1323), so they belong in
                    // this test rather than in the unbounded set above: the
                    // strand build publishes the box the emitted centrelines
                    // actually occupy, in THIS pose. Leaving them out would
                    // skip a cascade whose only caster is a coat -- the exact
                    // hole virtual geometry had from #702 to #1149, one caster
                    // family over.
                    const bool anyGroom = !anyMesh && !anySkinned &&
                                          std::ranges::any_of(m_GroomCasters,
                                                              [&](const ShadowGroomCaster& c)
                                                              { return !ShouldCull(c.WorldBounds, cascadeFrustum); });
                    if (!anyMesh && !anySkinned && !anyGroom)
                        continue; // No work for this cascade — skip all GL state changes
                }

                m_ActiveViews.Add({ cascade, lightVP, cascadeFrustum });
            }

            if (!m_ActiveViews.IsEmpty())
            {
                resolveCasterShaders();
                const auto selectLayer = [&](const ActiveShadowView& view)
                {
                    // The layer selection and the depth clear are per ITEM,
                    // not per region: on Vulkan the framebuffer's attachment
                    // selection and the pending clear live in the recording
                    // context (amendment (92) rule 2), and one layer per
                    // cascade keeps the items' writes disjoint (rule 5).
                    // Inline, this is the attach / clear / draw sequence the
                    // sequential loop ran per cascade. Classic, terrain,
                    // foliage and virtual geometry share this item's layer.
                    m_ShadowFramebuffer->AttachDepthTextureArrayLayer(csmArray->GetRHIHandle(), view.Index);
                };
                // ONLY THE RECEIVERS' TEXELS of each cascade: a strand samples
                // where its own coat projects, so the rest of a layer is never
                // read. At the dog's 4096^2 cascades the whole-layer copy cost
                // ~0.3 ms of GPU; the coat's rect is a few percent of a layer.
                const u32 side = m_ShadowMap->GetResolution();
                recordRegionForGrooms(ShadowPassType::CSM, ShadowMap::OpaqueCopy::Cascades, selectLayer,
                                      /*clearPerItem=*/true,
                                      [&]()
                                      {
                                          // Into the same array's opaque layers (#1533,
                                          // ShadowMap::OPAQUE_CSM_LAYER_BASE).
                                          for (const ActiveShadowView& view : m_ActiveViews)
                                          {
                                              u32 x = 0;
                                              u32 y = 0;
                                              u32 width = 0;
                                              u32 height = 0;
                                              if (!GroomReceiverTexelRect(view.LightVP, 0u, 0u, side, x, y, width, height))
                                              {
                                                  continue;
                                              }
                                              const auto layer = static_cast<i32>(view.Index);
                                              const auto opaqueLayer =
                                                  static_cast<i32>(view.Index + ShadowMap::OPAQUE_CSM_LAYER_BASE);
                                              RenderCommand::CopyImageSubDataRegion(
                                                  csmArray->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2DArray,
                                                  0, static_cast<i32>(x), static_cast<i32>(y), layer, csmArray->GetRHIHandle(),
                                                  RendererAPI::TextureTargetType::Texture2DArray, 0, static_cast<i32>(x),
                                                  static_cast<i32>(y), opaqueLayer, width, height);
                                          }
                                      });
            }
        }

        // Render the local-light shadow atlas (issue #435): attach the atlas
        // once, clear it whole (glClear ignores the viewport), then render
        // each entry — a spot tile or one point-light cube face — with the
        // viewport set to its packed sub-rect. Rasterisation is clipped by
        // the viewport, so tiles can't bleed into each other.
        if (const auto& atlas = m_ShadowMap->GetAtlasTextureArray();
            atlas && m_ShadowMap->GetAtlasEntryCount() > 0)
        {
            m_ShadowFramebuffer->AttachDepthTextureArrayLayer(atlas->GetRHIHandle(), 0);
            // glClear honours the scissor box (not the viewport) — force it
            // off so the whole-atlas clear can't be clipped by leaked state.
            RenderCommand::DisableScissorTest();
            RenderCommand::ClearDepthOnly();
            // The attach and the clear stay BEFORE the fork, unlike the CSM
            // region's: every entry is a tile of this one layer, and the fork
            // materialises the pending clear on the primary and pre-transitions
            // the attachment, so the items open their scopes with identity
            // transitions on a shared target — the case amendment (92) rule 5
            // was written for.

            // Collect the entries that will render — the region's items, in
            // entry order. An entry without a tile is skipped, as before.
            m_ActiveViews.Reset();
            const u32 entryCount = m_ShadowMap->GetAtlasEntryCount();
            for (u32 entry = 0; entry < entryCount; ++entry)
            {
                if (m_ShadowMap->GetAtlasEntryRect(entry).Size == 0)
                    continue;

                const glm::mat4& lightVP = m_ShadowMap->GetAtlasEntryMatrix(entry);
                m_ActiveViews.Add({ entry, lightVP, Frustum(lightVP) });
            }

            if (!m_ActiveViews.IsEmpty())
            {
                resolveCasterShaders();
                const auto selectTile = [&](const ActiveShadowView& view)
                {
                    // The viewport is per item: the fork seeds every item with
                    // the full-atlas viewport the prologue set (rule 4) and the
                    // item narrows it to its own tile.
                    const auto& rect = m_ShadowMap->GetAtlasEntryRect(view.Index);
                    RenderCommand::SetViewport(rect.X, rect.Y, rect.Size, rect.Size);
                };
                // Per entry, the receivers' texels inside its tile. The tile's
                // rect is in the same texel space the sampling reads it in
                // (TileScaleOffset), which is the space the tile was drawn in
                // on every backend whose local shadows work at all. The groom
                // half re-attaches the layer, uncleared, because the copies
                // closed the scope.
                recordRegionForGrooms(ShadowPassType::Atlas, ShadowMap::OpaqueCopy::Atlas, selectTile,
                                      /*clearPerItem=*/false,
                                      [&]()
                                      {
                                          // Into the atlas array's opaque layer (#1533).
                                          for (const ActiveShadowView& view : m_ActiveViews)
                                          {
                                              const auto& tile = m_ShadowMap->GetAtlasEntryRect(view.Index);
                                              u32 x = 0;
                                              u32 y = 0;
                                              u32 width = 0;
                                              u32 height = 0;
                                              if (!GroomReceiverTexelRect(view.LightVP, tile.X, tile.Y, tile.Size, x, y, width,
                                                                          height))
                                              {
                                                  continue;
                                              }
                                              RenderCommand::CopyImageSubDataRegion(
                                                  atlas->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2DArray, 0,
                                                  static_cast<i32>(x), static_cast<i32>(y), 0, atlas->GetRHIHandle(),
                                                  RendererAPI::TextureTargetType::Texture2DArray, 0, static_cast<i32>(x),
                                                  static_cast<i32>(y), static_cast<i32>(ShadowMap::OPAQUE_ATLAS_LAYER),
                                                  width, height);
                                          }
                                          m_ShadowFramebuffer->AttachDepthTextureArrayLayer(atlas->GetRHIHandle(), 0);
                                      });
            }
        }

        // Restore state
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::BackCull();
        m_ShadowFramebuffer->Unbind();
        RenderCommand::SetViewport(prevViewport.x, prevViewport.y, prevViewport.width, prevViewport.height);

        // Clear caster lists for next frame (vectors keep their allocation)
        m_MeshCasters.Reset();
        m_SkinnedCasters.Reset();
        m_TerrainCasters.Reset();
        m_VoxelCasters.Reset();
        m_FoliageCasters.Reset();
        m_GroomCasters.Reset();
    }

    void ShadowRenderPass::RecordShadowRegion(const ShadowPassType type, const ShadowCasterShaders& shaders,
                                              const bool recordingInstancedDraws,
                                              const u32 instanceCapacity,
                                              const std::function<void(const ActiveShadowView&)>& selectTarget,
                                              const bool clearPerItem,
                                              const ShadowCasterFilter filter)
    {
        const auto activeCount = static_cast<u32>(m_ActiveViews.Num());
        m_CasterFilter = filter;
        EnsureItemResources(activeCount, instanceCapacity);
        if (static_cast<sizet>(m_VirtualItemResources.Num()) < activeCount)
            m_VirtualItemResources.SetNum(static_cast<i64>(activeCount), EAllowShrinking::No);
        // The groom half of a split region draws no virtual geometry (#1533),
        // so it prepares none: the registry's frame and residency work ran for
        // the opaque half already.
        const bool virtualCasters =
            filter != ShadowCasterFilter::GroomsOnly &&
            VirtualGeometryShadow::PrepareViews(
                std::span<VirtualGeometryShadow::ViewResources>(m_VirtualItemResources.GetData(), activeCount));

        // Foliage GPU culling for every view in this region (issue #1235), run
        // HERE and not inside recordItem. The region records in parallel on
        // Vulkan, and a cull dispatched from an item would upload its header
        // through a one-shot submit and write a buffer a sibling item also
        // writes -- both hard errors under amendment (92) rule 6, and both
        // observed before this moved out. Each item's draws then only READ the
        // slot this loop filled for it.
        for (auto& caster : m_FoliageCasters)
        {
            if (!caster.renderer || filter == ShadowCasterFilter::GroomsOnly)
                continue;
            caster.renderer->ResetShadowViewCulling();
            for (u32 item = 0; item < activeCount; ++item)
            {
                // lightVP is world-space, which is what MakeCullInputs converts
                // from. The DISTANCE half of the test still uses the main view's
                // position: the plants a cascade must draw are the ones the
                // beauty pass draws, and those are chosen by distance from the
                // VIEWER, not from the light.
                // The CULLING camera's position, not the render camera's — the
                // same source Scene.cpp's main-view cull uses. With the observer
                // frozen (#726) the beauty pass draws the frozen survivor set,
                // and a cascade measuring distance from the LIVE camera would
                // cull away the casters of everything the frozen view still
                // draws: the plants would keep their pixels and lose their
                // shadows, which reads as a lighting bug rather than as the
                // debug tool doing something.
                caster.renderer->DispatchShadowViewCulling(
                    item, caster.renderer->MakeCullInputs(m_ActiveViews[item].LightVP,
                                                          Renderer3D::GetCullViewPosition()));
            }
        }
        // THE GROOM DRAW TALLY, counted HERE and not inside recordItem: the
        // region forks, so an item incrementing a shared counter would race its
        // siblings. The same cull test the items will run, on the render
        // thread, over a handful of casters -- so the number is exact rather
        // than an upper bound, which matters because a ZERO next to a non-zero
        // GroomsCasting is what detects a family that never reached this
        // technique (virtual-geometry-into-a-second-shadow-technique.md).
        // GATED ON THE SHADER, because RenderCascadeOrFace is. A tally that
        // counted draws the recording will not issue would make the INFO line,
        // the panel and the "this technique drew none of it" detector all
        // report a wired family when the shader failed to resolve -- which is
        // the single thing these three counters exist to detect.
        if (m_GroomPass != nullptr && !m_GroomCasters.IsEmpty() && shaders.Groom && filter != ShadowCasterFilter::NoGrooms)
        {
            u32 groomDraws = 0;
            u64 segmentsCast = 0;
            u64 segmentsWhole = 0;
            const ShadowMap& groomShadowMap = Renderer3D::GetShadowMap();
            for (u32 item = 0; item < activeCount; ++item)
            {
                const ActiveShadowView& view = m_ActiveViews[item];
                // The resolution RenderCascadeOrFace hands the widening.
                const f32 resolution = static_cast<f32>(type == ShadowPassType::CSM
                                                            ? groomShadowMap.GetResolution()
                                                            : groomShadowMap.GetAtlasEntryRect(view.Index).Size);
                for (const auto& caster : m_GroomCasters)
                {
                    if (caster.vaoID.IsValid() && caster.indexCount > 0u &&
                        !ShouldCull(caster.WorldBounds, view.CullFrustum))
                    {
                        ++groomDraws;
                        // Six indices a segment. The world-space matrix with a
                        // zero origin is the same map the draw makes with the
                        // render-relative one.
                        segmentsCast += GroomCasterViewIndexCount(caster, view.LightVP, glm::vec3(0.0f), resolution) / 6u;
                        segmentsWhole += caster.indexCount / 6u;
                    }
                }
            }
            GroomShadowCasterStats& groomStats = m_GroomPass->MutableSceneShadowStats();
            groomStats.SegmentsCast += segmentsCast;
            groomStats.SegmentsWhole += segmentsWhole;
            if (type == ShadowPassType::CSM)
            {
                groomStats.CascadeDraws += groomDraws;
            }
            else
            {
                groomStats.AtlasDraws += groomDraws;
            }
        }

        const auto recordItem = [&](const u32 item)
        {
            const ActiveShadowView& view = m_ActiveViews[item];
            selectTarget(view);
            if (clearPerItem)
                RenderCommand::ClearDepthOnly();
            RenderCascadeOrFace(view.LightVP, type, view.Index, &view.CullFrustum,
                                shaders, m_ItemResources[item], recordingInstancedDraws ? &m_ItemTallies[item] : nullptr,
                                virtualCasters ? &m_VirtualItemResources[item] : nullptr, item);
        };
        RenderCommand::RecordParallel(activeCount, recordItem, instanceCapacity);
        ReplayProfilerTallies(type, recordingInstancedDraws);
        m_CasterFilter = ShadowCasterFilter::All;
    }

    void ShadowRenderPass::EnsureItemResources(u32 count, u32 instanceCapacity)
    {
        OLO_PROFILE_FUNCTION();

        // Same sizes and binding points as the UBOs ShadowMap::Init creates for
        // the (now VSM-only) shared pair; the instance buffer takes its default
        // capacity and grows on the first oversized batch like the engine-wide one.
        while (static_cast<sizet>(m_ItemResources.Num()) < count)
        {
            ItemResources resources;
            resources.Camera = UniformBuffer::Create(
                ShaderBindingLayout::CameraUBO::GetSize(),
                ShaderBindingLayout::UBO_CAMERA);
            resources.Animation = UniformBuffer::Create(
                ShaderBindingLayout::AnimationUBO::GetSize(),
                ShaderBindingLayout::UBO_ANIMATION);
            // The groom caster in flight (#1323). PER ITEM for the reason the
            // other two are: a UBO versions its bytes per object, so two items
            // writing one object would interleave (amendment (92) rule 6). It
            // is created here, on the render thread, because rule 7 refuses
            // resource creation on an item context.
            resources.Groom = UniformBuffer::Create(
                UBOStructures::GroomShadowParamsUBO::GetSize(),
                ShaderBindingLayout::UBO_USER_0);
            // Sized HERE, on the render thread: an item may not grow its buffer
            // (StorageBuffer::Resize creates and reclaims GPU memory — amendment
            // (92) rule 7), so the capacity covers the largest batch any item can
            // upload this frame.
            resources.Instances = Ref<InstanceBuffer>::Create(instanceCapacity);
            m_ItemResources.Add(std::move(resources));
        }
        for (u32 item = 0; item < count; ++item)
        {
            // Grown on the render thread when this frame's casters outnumber
            // an earlier frame's (the buffer keeps its capacity across frames).
            m_ItemResources[item].Instances->EnsureCapacity(instanceCapacity);
        }
        if (static_cast<sizet>(m_ItemTallies.Num()) < static_cast<sizet>(m_ItemResources.Num()))
        {
            m_ItemTallies.SetNum(static_cast<i64>(m_ItemResources.Num()), EAllowShrinking::No);
        }
    }

    void ShadowRenderPass::ReplayProfilerTallies(ShadowPassType type, bool recording)
    {
        if (!recording)
            return;

        OLO_PROFILE_FUNCTION();

        auto& profiler = RendererProfiler::GetInstance();
        // The source label tags every shadow batch with the cascade / entry index
        // so the profiler's "Instanced Draws" tab can show e.g. "Shadow CSM
        // cascade 1" — making it obvious which shadow target a batched draw is
        // filling in.
        const char* kind = (type == ShadowPassType::CSM) ? "CSM cascade" : "Atlas entry";
        const auto itemCount = static_cast<sizet>(m_ActiveViews.Num());
        for (sizet item = 0; item < itemCount; ++item)
        {
            auto& tally = m_ItemTallies[item];
            if (tally.InstancedDraws.IsEmpty())
                continue;

            char sourceLabel[64];
            std::snprintf(sourceLabel, sizeof(sourceLabel), "Shadow %s %u", kind, m_ActiveViews[item].Index);
            for (const auto& draw : tally.InstancedDraws)
            {
                // EntityIDs intentionally null — shadow casters carry raw VAOs +
                // transforms, not entity refs, so per-instance picking isn't
                // meaningful here.
                profiler.RecordInstancedDraw(
                    /*meshHandle=*/0,
                    draw.VertexArrayIndex,
                    draw.IndexCount,
                    draw.InstanceCount,
                    /*entityIDs=*/nullptr,
                    /*fromAutoBatching=*/true,
                    sourceLabel);
            }
            tally.InstancedDraws.Reset();
        }
    }

    void ShadowRenderPass::RenderCascadeOrFace(const glm::mat4& lightVP, ShadowPassType type, u32 layerOrLight,
                                               const Frustum* cullFrustum,
                                               const ShadowCasterShaders& shaders, ItemResources& resources,
                                               ItemProfilerTally* tally, VirtualGeometryShadow::ViewResources* virtualResources,
                                               u32 shadowViewIndex, VirtualShadowMap* familyVsm) const
    {
        OLO_PROFILE_FUNCTION();

        // Camera-relative (issue #429): render the shadow map in the same
        // render-relative space as the main pass. lightVP is world-space; shift
        // it to map (worldPos - origin) -> light clip (matching the sampling
        // matrices ShadowMap::UploadUBO shifts by the same origin), and shift
        // the casters below by the same origin. No-op near origin.
        const glm::vec3 renderOrigin = Renderer3D::GetRenderOrigin();
        const glm::mat4 lightVPRel = MakeViewProjectionRelative(lightVP, renderOrigin);

        // Shadow shaders read transforms from an InstanceBuffer at
        // SSBO_INSTANCE_DATA = 15 (no more shadow-specific UBO at binding 3).
        // It is THIS ITEM's buffer, not Renderer3D::GetModelInstanceBuffer():
        // the engine-wide one is a single object, and an object has one writer
        // per region (amendment (92) rule 6). Static mesh casters use the
        // auto-batched path below; the helper lambda covers the skinned /
        // terrain / voxel paths where per-caster state (bones, heightmap,
        // terrain UBO) blocks batching.
        Ref<InstanceBuffer>& instanceBuffer = resources.Instances;
        auto uploadShadowModelUBO = [&instanceBuffer, &renderOrigin](const glm::mat4& worldTransform)
        {
            if (!instanceBuffer)
                return;
            const glm::mat4 relTransform = MakeModelRelative(worldTransform, renderOrigin);
            InstanceData inst;
            inst.Transform = relTransform;
            inst.Normal = glm::mat4(1.0f);     // Shadow depth shaders don't use normals
            inst.PrevTransform = relTransform; // shadow casters have no motion-vector use today
            inst.EntityID = -1;
            const std::span<const InstanceData> oneInstance(&inst, 1);
            instanceBuffer->Upload(oneInstance);
            instanceBuffer->Bind();
        };

        if (tally)
        {
            tally->InstancedDraws.Reset();
        }

        // Upload light VP to this item's shadow camera UBO (binding 0).
        // A8 seam, rasterizer flavour: caster vertex stages feed this to
        // gl_Position, so the shadow map renders y-flipped/z-[0,1] on Vulkan —
        // its depth CONTENTS stay GL-identical (the z half's whole point).
        // The SAMPLING matrices (ShadowMap::UploadUBO) are the shader-
        // reconstruction side of the same contract and carry the row flip that
        // matches what this pass stores — both halves must move together.
        auto cameraUBOData = ShaderBindingLayout::CameraUBO{};
        cameraUBOData.ViewProjection = RHI::AdjustProjectionForBackend(lightVPRel);
        cameraUBOData.View = glm::mat4(1.0f);
        cameraUBOData.Projection = RHI::AdjustProjectionForBackend(lightVPRel);
        // Caster shaders that reconstruct an absolute world position from the
        // relative one (terrain snow-height displacement in Terrain_Depth.glsl:
        // worldP = (u_Model*pos).xyz + u_RenderOrigin) need the real origin here;
        // left at its 0 default the snow clip-region UV falls outside the map far
        // from origin and the displaced caster geometry no longer matches the lit
        // surface, detaching the shadow. No-op near origin (issue #429).
        cameraUBOData.RenderOrigin = renderOrigin;
        // Every target — CSM cascades and atlas entries alike — renders
        // standard projective depth now (the old linear-distance point-cubemap
        // path died with the shadow atlas, issue #435), so no light position /
        // far plane needs to ride the camera UBO.
        cameraUBOData.Position = glm::vec3(0.0f);
        cameraUBOData.Pad0 = 0.0f;
        // Reconstruction flavour of the same matrix (#691) — no known
        // caster shader reads it under this camera, but the member must never
        // be a zero/identity mismatch with Projection on any writer.
        cameraUBOData.ProjectionForReconstruction = RHI::AdjustProjectionForShaderReconstruction(lightVPRel);

        Ref<UniformBuffer>& cameraUBO = resources.Camera;
        cameraUBO->SetData(&cameraUBOData, ShaderBindingLayout::CameraUBO::GetSize());
        cameraUBO->Bind();

        // The two halves of a groom-split cascade (#1533): its opaque casters,
        // then -- after ShadowRenderPass copies the layer -- its grooms.
        const bool drawOpaque = m_CasterFilter != ShadowCasterFilter::GroomsOnly;
        const bool drawGrooms = m_CasterFilter != ShadowCasterFilter::NoGrooms;

        // ── Static meshes (auto-batched by shared VAO + index range) ──
        {
            const Ref<Shader>& shadowShader = shaders.Mesh;
            if (drawOpaque && shadowShader && !m_MeshCasters.IsEmpty())
            {
                // Casters sharing (drawVao, indexCount, baseIndex) all read the
                // same submesh range, so they can collapse into a single
                // glDrawElementsInstanced. The shadow VS reads
                // instances[gl_InstanceIndex].Transform from the SSBO.
                // thread_local, which is what makes it per ITEM when the region
                // forks: a worker owns its own list, and inline there is one thread.
                thread_local TArray64<ShadowMeshBatch> batches;
                batches.Reset();

                for (const auto& caster : m_MeshCasters)
                {
                    if (cullFrustum && ShouldCull(caster.WorldBounds, *cullFrustum))
                        continue;

                    RHI::ResourceHandle const drawVao = caster.shadowVaoID.IsValid() ? caster.shadowVaoID : caster.vaoID;
                    const glm::mat4 relTransform = MakeModelRelative(caster.transform, renderOrigin);
                    InstanceData inst;
                    inst.Transform = relTransform;
                    inst.Normal = glm::mat4(1.0f);
                    inst.PrevTransform = relTransform;
                    inst.EntityID = -1;

                    // twoSided is part of the batch key: single- and two-sided casters need
                    // different cull state at draw time, so they must not share an instanced draw.
                    auto it = std::ranges::find_if(batches,
                                                   [&](const ShadowMeshBatch& b)
                                                   { return b.drawVao == drawVao && b.indexCount == caster.indexCount &&
                                                            b.baseIndex == caster.baseIndex && b.twoSided == caster.twoSided; });
                    if (it == batches.end())
                    {
                        batches.Add({ drawVao, caster.indexCount, caster.baseIndex, caster.twoSided, { inst } });
                    }
                    else
                    {
                        it->instances.Add(inst);
                    }
                }

                if (!batches.IsEmpty())
                {
                    shadowShader->Bind();
                    // Profiler records are TALLIED here and handed to
                    // RendererProfiler after the join (ReplayProfilerTallies),
                    // which also builds the per-target source label. A null
                    // tally means the profiler is not recording, the same
                    // "cheap when off" gate the direct call had.
                    bool cullingDisabled = false; // track so we only touch GL state on a change
                    for (const auto& batch : batches)
                    {
                        // Two-sided casters render with culling DISABLED so a single-sided planar
                        // mesh lit from the front still casts (issue #650); single-sided casters
                        // keep the pass's front-face cull (peter-panning). Every item starts from
                        // the FrontCull the pass set once up front — the fork seeds each item
                        // with the render thread's recorded state as of the fork (amendment (92)
                        // rule 4), and inline the state simply carries over — so restore it
                        // whenever we leave a two-sided batch.
                        if (batch.twoSided && !cullingDisabled)
                        {
                            RenderCommand::DisableCulling();
                            cullingDisabled = true;
                        }
                        else if (!batch.twoSided && cullingDisabled)
                        {
                            RenderCommand::EnableCulling();
                            RenderCommand::FrontCull();
                            cullingDisabled = false;
                        }

                        if (instanceBuffer)
                        {
                            instanceBuffer->Upload(std::span<const InstanceData>(batch.instances.GetData(),
                                                                                 static_cast<sizet>(batch.instances.Num())));
                            instanceBuffer->Bind();
                        }
                        // Single-instance groups still go through the instanced
                        // call — gl_InstanceIndex is 0 either way and the
                        // driver handles count==1 cheaply.
                        RenderCommand::DrawIndexedInstancedRaw(batch.drawVao, batch.indexCount, batch.baseIndex,
                                                               static_cast<u32>(batch.instances.Num()));
                        if (tally)
                        {
                            tally->InstancedDraws.Add({ batch.drawVao.Index,
                                                        batch.indexCount,
                                                        static_cast<u32>(batch.instances.Num()) });
                        }
                    }

                    // Restore the pass's front-face cull if a two-sided batch left culling
                    // disabled — the skinned / voxel casters below in this item rely on the
                    // FrontCull state the pass set once at the top. Each view's
                    // terrain/foliage/virtual-geometry work follows inside this item.
                    if (cullingDisabled)
                    {
                        RenderCommand::EnableCulling();
                        RenderCommand::FrontCull();
                    }
                }
            }
        }

        // ── Skinned meshes ──
        if (drawOpaque && !m_SkinnedCasters.IsEmpty())
        {
            const Ref<Shader>& skinnedShadowShader = shaders.Skinned;
            if (skinnedShadowShader)
            {
                skinnedShadowShader->Bind();
                // This item's animation UBO, for the same reason as the instance
                // buffer: the bones of every skinned caster of this target go
                // through one object, and that object must be this item's alone.
                Ref<UniformBuffer>& animUBO = resources.Animation;
                animUBO->Bind();

                for (const auto& caster : m_SkinnedCasters)
                {
                    if (cullFrustum && ShouldCull(caster.WorldBounds, *cullFrustum))
                        continue;
                    uploadShadowModelUBO(caster.transform);

                    if (caster.boneCount > 0)
                    {
                        // A read of the frame's bone array, filled before the graph
                        // executes; nothing writes it while a region is open.
                        auto count = std::min(caster.boneCount, static_cast<u32>(ShaderBindingLayout::AnimationUBO::MAX_BONES));
                        const glm::mat4* boneMatrices = FrameDataBufferManager::Get().GetBoneMatrixRange(caster.boneBufferOffset, count);
                        if (!boneMatrices)
                        {
                            // The UBO still holds the previous caster's pose, so
                            // drawing would cast the wrong silhouette rather than
                            // none. GetBoneMatrixRange already logged the range.
                            continue;
                        }
                        animUBO->SetData(boneMatrices, count * sizeof(glm::mat4));
                    }

                    RenderCommand::DrawIndexedRaw(caster.vaoID, caster.indexCount, caster.baseIndex);
                }
            }
        }

        // ── Voxel meshes ──
        // Two depth shaders: the marching-cubes triangle soup and the packed-
        // quad instanced path (issue #727). Casters are interleaved in one list,
        // so bind lazily and only when the shader actually changes.
        if (drawOpaque && !m_VoxelCasters.IsEmpty())
        {
            const Ref<Shader>& voxelDepthShader = shaders.Voxel;
            const Ref<Shader>& voxelQuadDepthShader = shaders.VoxelQuad;
            const Shader* boundDepthShader = nullptr;

            for (const auto& caster : m_VoxelCasters)
            {
                if (cullFrustum && ShouldCull(caster.WorldBounds, *cullFrustum))
                    continue;
                const Ref<Shader>& depthShader = (caster.instanceCount > 0) ? voxelQuadDepthShader : voxelDepthShader;
                if (!depthShader)
                {
                    continue;
                }
                if (depthShader.get() != boundDepthShader)
                {
                    depthShader->Bind();
                    if (familyVsm)
                        familyVsm->BindPhysicalPoolImage();
                    boundDepthShader = depthShader.get();
                }

                uploadShadowModelUBO(caster.transform);
                if (caster.instanceCount > 0)
                {
                    RenderCommand::DrawIndexedInstancedRaw(caster.vaoID, caster.indexCount, 0, caster.instanceCount);
                }
                else
                {
                    RenderCommand::DrawIndexedRaw(caster.vaoID, caster.indexCount);
                }
            }
        }

        // ── Grooms (issue #1323) ──
        //
        // ITEM-SAFE, unlike terrain / foliage / virtual geometry: the only
        // object a groom draw writes is this item's own params UBO, created by
        // EnsureItemResources on the render thread. So it records inside the
        // parallel half rather than in the sequential tail, and the Vulkan
        // cascade fork stays legal -- no resource is created inside an item,
        // which is the trap the issue calls out by name.
        //
        // The resolution the widening is measured in is THIS view's: a cascade
        // texel and an atlas tile texel are different sizes, and a floor
        // expressed in texels has to know which.
        if (drawGrooms && shaders.Groom && resources.Groom && !m_GroomCasters.IsEmpty())
        {
            auto& groomShadowMap = Renderer3D::GetShadowMap();
            const u32 groomViewResolution = (type == ShadowPassType::CSM)
                                                ? groomShadowMap.GetResolution()
                                                : groomShadowMap.GetAtlasEntryRect(layerOrLight).Size;
            shaders.Groom->Bind();
            // Its own GPU bracket (#1533 E4): a coat's share of the shadow
            // pass is the scene-shadow cost of fur, and it is otherwise folded
            // into every other caster's. Not inside a parallel item: the timer
            // pool is not thread-safe (the GTAO sub-passes skip it the same way).
            auto* groomTimers = RenderCommand::IsRecordingParallelItem() ? nullptr : &GPUPassTimerPool::GetInstance();
            if (groomTimers)
                groomTimers->BeginSubPass("GroomCasters");
            RenderGroomCasters(cullFrustum, lightVPRel, renderOrigin, static_cast<f32>(groomViewResolution), 0,
                               *resources.Groom);
            if (groomTimers)
                groomTimers->EndSubPass();
        }

        // ── Terrain patches ──
        if (drawOpaque && !m_TerrainCasters.IsEmpty())
        {
            const auto& terrainDepthShader = shaders.Terrain;
            if (terrainDepthShader)
            {
                terrainDepthShader->Bind();
                // A heightfield is an open surface: front-face culling removes
                // its entire upper side under an overhead light.
                RenderCommand::DisableCulling();
                if (familyVsm)
                    familyVsm->BindPhysicalPoolImage();
                auto terrainUBO = Renderer3D::GetTerrainUBO();

                for (const auto& caster : m_TerrainCasters)
                {
                    if (cullFrustum && ShouldCull(caster.WorldBounds, *cullFrustum))
                        continue;
                    uploadShadowModelUBO(caster.transform);

                    if (caster.heightmapTextureID.IsValid())
                    {
                        // Persistent: a terrain heightmap is an asset-owned texture,
                        // not a graph-pooled target. The shader was bound once above
                        // the loop, so the seam's program fork is already correct
                        // (issue #691).
                        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_TERRAIN_HEIGHTMAP,
                                                         caster.heightmapTextureID,
                                                         RHI::HeapSlotLifetime::Persistent);
                    }

                    if (terrainUBO)
                    {
                        terrainUBO->SetData(&caster.terrainUBO, ShaderBindingLayout::TerrainUBO::GetSize());
                        terrainUBO->Bind();
                    }

                    // A FLUSH PER CASTER, not per pass: each iteration rebinds the
                    // heightmap slot to a DIFFERENT terrain's texture, so a flush
                    // hoisted out of this loop would publish only the last caster's
                    // offset and every earlier patch would displace against it.
                    HeapBinding::FlushOffsets();
                    RenderCommand::DrawIndexedPatchesRaw(caster.vaoID, caster.indexCount, caster.patchVertexCount);
                }
                RenderCommand::EnableCulling();
                RenderCommand::FrontCull();
            }
        }

        // ── Foliage ──
        // Its own GPU bracket, like the grooms' (#1533 E4): a lawn's share of
        // the shadow pass is otherwise folded into every other caster's.
        auto* foliageTimers = (drawOpaque && !m_FoliageCasters.IsEmpty() && !RenderCommand::IsRecordingParallelItem())
                                  ? &GPUPassTimerPool::GetInstance()
                                  : nullptr;
        if (foliageTimers)
            foliageTimers->BeginSubPass("FoliageCasters");
        if (drawOpaque && !m_FoliageCasters.IsEmpty())
            RenderCommand::DisableCulling();
        for (const auto& caster : m_FoliageCasters)
        {
            if (drawOpaque && caster.renderer && caster.depthShader)
            {
                const auto& foliageShader = shaders.Foliage ? shaders.Foliage : caster.depthShader;
                foliageShader->Bind();
                // Draws from the slot RecordShadowRegion culled for THIS item.
                // Read-only — see FoliageRenderer::RenderShadows.
                caster.renderer->RenderShadows(foliageShader, caster.time, shadowViewIndex, shaders.FoliageImpostor,
                                               familyVsm ? std::function<void()>([familyVsm]()
                                                                                 { familyVsm->BindPhysicalPoolImage(); })
                                                         : std::function<void()>{});
            }
        }
        if (foliageTimers)
            foliageTimers->EndSubPass();
        if (drawOpaque && !m_FoliageCasters.IsEmpty())
        {
            RenderCommand::EnableCulling();
            RenderCommand::FrontCull();
        }

        // ── Virtualized geometry (#629): GPU-driven cluster casters ──
        // CSM cascades AND local-light atlas entries (spot tiles / point-light
        // cube faces). Runs its own cluster cull against this view's light VP,
        // then replays the compacted segments with the depth-only virtual-mesh
        // shader into the current target + viewport (the caller already set the
        // atlas tile viewport). Reads the shadow camera UBO bound above. The
        // ortho-style error scale is only approximate for the atlas' perspective
        // VPs, but the DAG cut is watertight at any threshold, so shadows stay
        // crack-free; a distance-accurate perspective error scale is a refinement.
        auto& shadowMap = Renderer3D::GetShadowMap();
        if (drawOpaque && virtualResources)
        {
            u32 const shadowViewResolution = (type == ShadowPassType::CSM)
                                                 ? shadowMap.GetResolution()
                                                 : shadowMap.GetAtlasEntryRect(layerOrLight).Size;
            VirtualGeometryShadow::RenderCascade(lightVPRel, shadowViewResolution, *virtualResources);
        }
    }

    f32 ShadowRenderPass::WidestShadowTexelMetres() const
    {
        if (m_ShadowMap == nullptr)
        {
            return 0.0f;
        }
        const f32 resolution = static_cast<f32>(std::max(1u, m_ShadowMap->GetResolution()));
        f32 widest = 0.0f;
        for (u32 cascade = 0; cascade < ShadowMap::MAX_CSM_CASCADES; ++cascade)
        {
            const glm::mat4& lightVP = m_ShadowMap->GetCSMMatrix(cascade);
            // Row 0 read as a row vector over world xyz. For an orthographic
            // cascade this is 1/halfExtent, so a half width of
            // `texels / resolution` in NDC is `texels / (resolution * lenRow0)`
            // world metres -- the same derivation PBRCommon.glsl spells out for
            // the depth bias. A degenerate or identity matrix (no light has
            // requested shadows yet) gives a tiny length, which the guard drops
            // rather than turning into an enormous pad.
            const f32 lenRow0 = glm::length(glm::vec3(lightVP[0][0], lightVP[1][0], lightVP[2][0]));
            if (!(lenRow0 > 1.0e-6f) || !std::isfinite(lenRow0))
            {
                continue;
            }
            widest = std::max(widest, 1.0f / (resolution * lenRow0));
        }
        return widest;
    }

    bool ShadowRenderPass::GroomReceiverTexelRect(const glm::mat4& lightVP, const u32 tileX, const u32 tileY,
                                                  const u32 tileSize, u32& x, u32& y, u32& width, u32& height) const
    {
        return ShadowReceiverTexelRect(m_GroomReceiverBoundsUnknown ? NoBounds : m_GroomReceiverBounds, lightVP, tileX,
                                       tileY, tileSize, x, y, width, height);
    }

    bool ShadowReceiverTexelRect(const BoundingBox& bounds, const glm::mat4& lightVP, const u32 tileX, const u32 tileY,
                                 const u32 tileSize, u32& x, u32& y, u32& width, u32& height)
    {
        const auto whole = [&]()
        {
            x = tileX;
            y = tileY;
            width = tileSize;
            height = tileSize;
            return tileSize > 0u;
        };
        if (bounds.Min.x == NoBounds.Min.x)
        {
            return whole();
        }
        // The matrix the receivers sample through (ShadowMap uploads the
        // cascade and atlas matrices through the same seam): identity on GL,
        // the row flip on Vulkan, where the map's memory row 0 is the light
        // view's TOP row. The copy addresses memory rows, so the rect must be
        // in the sampled rows, not the GL-convention ones.
        const glm::mat4 sampled = RHI::AdjustProjectionForShaderReconstruction(lightVP);
        glm::vec2 lo(std::numeric_limits<f32>::max());
        glm::vec2 hi(std::numeric_limits<f32>::lowest());
        for (u32 corner = 0; corner < 8u; ++corner)
        {
            const glm::vec3 p((corner & 1u) ? bounds.Max.x : bounds.Min.x, (corner & 2u) ? bounds.Max.y : bounds.Min.y,
                              (corner & 4u) ? bounds.Max.z : bounds.Min.z);
            const glm::vec4 clip = sampled * glm::vec4(p, 1.0f);
            // A corner behind a perspective light has no texel: the whole tile.
            if (!(clip.w > 1.0e-6f))
            {
                return whole();
            }
            // The sampling's own mapping (projCoords * 0.5 + 0.5).
            const glm::vec2 uv = (glm::vec2(clip) / clip.w) * 0.5f + 0.5f;
            lo = glm::min(lo, uv);
            hi = glm::max(hi, uv);
        }
        if (!std::isfinite(lo.x) || !std::isfinite(lo.y) || !std::isfinite(hi.x) || !std::isfinite(hi.y))
        {
            return whole();
        }
        // The filter kernels read past the coat's own texels -- PCF a few, the
        // PCSS blocker search further -- so the rect is padded generously.
        constexpr f32 kPadTexels = 96.0f;
        const auto size = static_cast<f32>(tileSize);
        const f32 x0 = std::clamp(std::floor(lo.x * size) - kPadTexels, 0.0f, size);
        const f32 x1 = std::clamp(std::ceil(hi.x * size) + kPadTexels, 0.0f, size);
        const f32 y0 = std::clamp(std::floor(lo.y * size) - kPadTexels, 0.0f, size);
        const f32 y1 = std::clamp(std::ceil(hi.y * size) + kPadTexels, 0.0f, size);
        if (!(x1 > x0) || !(y1 > y0))
        {
            return false;
        }
        x = tileX + static_cast<u32>(x0);
        y = tileY + static_cast<u32>(y0);
        width = static_cast<u32>(x1 - x0);
        height = static_cast<u32>(y1 - y0);
        return true;
    }

    void ShadowRenderPass::CollectGroomCasters()
    {
        OLO_PROFILE_FUNCTION();

        m_GroomCasters.Reset();
        m_GroomReceiverBounds = NoBounds;
        m_HasGroomReceivers = false;
        m_GroomReceiverBoundsUnknown = false;
        if (m_GroomPass == nullptr)
        {
            return;
        }

        GroomShadowCasterStats& stats = m_GroomPass->MutableSceneShadowStats();
        stats.VirtualShadowMapActive = m_ShadowMap != nullptr && m_ShadowMap->IsVirtualShadowMapActive();
        // The VSM's local-light LAYER pool is rasterised by GPU-driven mesh
        // batches that a groom caster never enters, so while it serves lamps a
        // groom casts only the sun's shadow (see the counter's comment). Read
        // once here and reported below only if a groom actually casts.
        const u32 vsmLocalLights = (stats.VirtualShadowMapActive && m_ShadowMap != nullptr)
                                       ? m_ShadowMap->GetVirtualShadowMap().GetLocalLightCount()
                                       : 0u;

        // THE SAME REQUEST LIST GroomRenderPass DRAWS FROM, read from
        // Renderer3D rather than handed in: the two passes must agree about
        // which grooms exist this frame, and a second SetRequests call would be
        // a second copy to keep in step. Scene publishes it once per frame,
        // before the graph executes.
        for (const auto& request : Renderer3D::GetGroomStrandRequests())
        {
            if (!request.Groom || (!request.CastsSceneShadow && !request.ReceivesSceneShadow))
            {
                continue;
            }
            if (request.CastsSceneShadow)
            {
                ++stats.GroomsAskedToCast;
            }

            // ACQUIRING CAN BUILD, which is why this is on the render thread
            // and outside every parallel region (amendment (92) rule 7). A
            // receiver is acquired too, for its box; the strand pass acquires
            // the same cache entry right after, so nothing is built twice.
            GroomRenderPass::ShadowCasterGeometry geometry;
            const bool acquired = m_GroomPass->AcquireShadowCaster(request, geometry) && geometry.Vao.IsValid() &&
                                  geometry.IndexCount > 0u;

            // THE RECEIVERS' BOX (#1533): the opaque copies need cover no more.
            if (request.ReceivesSceneShadow)
            {
                m_HasGroomReceivers = true;
                if (acquired && geometry.BoundsValid)
                {
                    const BoundingBox world =
                        BoundingBox{ geometry.BoundsMin, geometry.BoundsMax }.Transform(request.Transform);
                    const bool first = m_GroomReceiverBounds.Min.x == NoBounds.Min.x;
                    m_GroomReceiverBounds.Min = first ? world.Min : glm::min(m_GroomReceiverBounds.Min, world.Min);
                    m_GroomReceiverBounds.Max = first ? world.Max : glm::max(m_GroomReceiverBounds.Max, world.Max);
                }
                else
                {
                    m_GroomReceiverBoundsUnknown = true;
                }
            }
            if (!request.CastsSceneShadow)
            {
                continue;
            }
            if (!acquired)
            {
                // A coat that renders and casts nothing is a different fault
                // from a family that was never wired into a technique, and the
                // panel has to be able to tell them apart.
                ++stats.GroomsWithoutGeometry;
                continue;
            }

            ShadowGroomCaster caster;
            caster.vaoID = geometry.Vao;
            caster.indexCount = geometry.IndexCount;
            caster.runs = geometry.CasterRuns.data();
            caster.runCount = static_cast<u32>(geometry.CasterRuns.size());
            caster.transform = request.Transform;
            // THE WIDTH THE COAT IS DRAWN AT. Since #1428 the per-role coverage
            // compensation is baked into the stream's radii, so the only lever
            // left is the per-groom authoring scale the strand draw applies too.
            caster.widthScale = request.WidthScale;
            const f32 axisX = glm::length(glm::vec3(request.Transform[0]));
            const f32 axisY = glm::length(glm::vec3(request.Transform[1]));
            const f32 axisZ = glm::length(glm::vec3(request.Transform[2]));
            // The mean axis length, matching GroomRenderPass and
            // GroomCoverage::ProjectGroom. One scalar cannot describe an
            // anisotropically scaled strand, and the places that scale a width
            // must at least be wrong the same way.
            caster.objectScale = (axisX + axisY + axisZ) / 3.0f;
            // Sanitised, not trusted: this comes from a component and reaches
            // a divisor in the widening. A non-finite or negative floor would
            // make the half width NaN and the whole coat vanish from the map.
            caster.minWidthTexels = std::isfinite(request.ShadowWidthTexels)
                                        ? std::clamp(request.ShadowWidthTexels, 0.0f, 16.0f)
                                        : 1.0f;
            caster.deformBuffer = geometry.DeformBuffer;
            caster.deformModes = geometry.DeformModes;
            caster.deformBases = geometry.DeformBases;
            // Identity across frames for the VSM page invalidation: the entity
            // and the groom it wears, mixed so two coats on one entity differ.
            caster.key = (static_cast<u64>(request.Handle) * 0x9E3779B97F4A7C15ull) ^
                         static_cast<u64>(static_cast<u32>(request.EntityID));
            caster.deforming = GroomRenderPass::IsDeformed(request);

            // THE COAT IN THIS POSE, not the asset's bind-pose bounds and not a
            // GPU-deformed stream's bind-local ones: the posed roots padded by
            // the longest strand's reach (see GroomRenderPass::PosedObjectBounds).
            // It already includes each strand's own radius, so the only thing
            // left to pad for is the light-space WIDTH FLOOR, which is a property
            // of the shadow map rather than of the groom and is applied in clip
            // space after this test runs: one texel of the coarsest cascade, in
            // world metres, scaled by the configured floor -- the largest the
            // widening can be for any view this caster is tested against.
            if (geometry.BoundsValid)
            {
                const BoundingBox objectBounds{ geometry.BoundsMin, geometry.BoundsMax };
                BoundingBox world = objectBounds.Transform(request.Transform);
                const f32 pad = WidestShadowTexelMetres() * caster.minWidthTexels;
                if (std::isfinite(pad) && pad > 0.0f)
                {
                    world.Min -= glm::vec3(pad);
                    world.Max += glm::vec3(pad);
                }
                caster.WorldBounds = world;
            }

            m_GroomCasters.Add(caster);
            ++stats.GroomsCasting;
        }

        if (stats.GroomsCasting > 0u && vsmLocalLights > 0u)
        {
            stats.VirtualShadowLocalLightsWithoutGrooms = vsmLocalLights;
            // Once per session, and a WARNING: a lamp whose shadow silently omits
            // the coat reads as a lighting bug two subsystems away.
            if (!m_WarnedGroomVsmLocalLights)
            {
                m_WarnedGroomVsmLocalLights = true;
                OLO_CORE_WARN("ShadowRenderPass: {} groom caster(s) do not reach the Virtual Shadow Map's local-light "
                              "layers, so {} lamp(s) cast no groom shadow. The sun's does. Turn off the VSM's "
                              "LocalLights to route lamps through the shadow atlas, where grooms cast.",
                              stats.GroomsCasting, vsmLocalLights);
            }
        }
    }

    namespace
    {
        // The index ranges one shadow view draws of a groom caster (#1533), in
        // order, to `emit(firstIndex, count)`: a prefix of each run, as
        // DecideGroomCasterRun allows it here, with a run cast WHOLE merged into
        // the range of the run after it -- they are contiguous in the order --
        // so a coat whose runs are mostly whole costs few draws. Returns the
        // index total. The stats and the draw both go through this, so the
        // count a panel reports is the count drawn.
        template<typename Emit>
        u32 VisitGroomCasterRanges(const ShadowGroomCaster& caster, const glm::mat4& viewProjection,
                                   const glm::vec3& origin, f32 resolutionTexels, Emit&& emit)
        {
            if (caster.runs == nullptr || caster.runCount == 0u)
            {
                emit(0u, caster.indexCount);
                return caster.indexCount;
            }
            const std::optional<f32> forced = Levers::GroomShadowCasterFraction();
            GroomCasterPlacement placement;
            placement.Transform = caster.transform;
            placement.ObjectScale = caster.objectScale;
            placement.WidthScale = caster.widthScale;
            placement.MinWidthTexels = caster.minWidthTexels;
            placement.CullMin = caster.WorldBounds.Min;
            placement.CullMax = caster.WorldBounds.Max;
            const GroomCasterView view{ viewProjection, origin, resolutionTexels };

            u32 total = 0;
            u32 rangeFirst = 0;
            u32 rangeCount = 0;
            for (const GroomCasterRun& run : std::span<const GroomCasterRun>(caster.runs, caster.runCount))
            {
                const u32 count = forced ? GroomCasterIndexCount(run.Prefix, *forced)
                                         : DecideGroomCasterRun(run, placement, view).IndexCount;
                if (count == 0u)
                {
                    continue;
                }
                if (rangeCount > 0u && rangeFirst + rangeCount == run.FirstIndex)
                {
                    rangeCount += count;
                }
                else
                {
                    if (rangeCount > 0u)
                    {
                        emit(rangeFirst, rangeCount);
                    }
                    rangeFirst = run.FirstIndex;
                    rangeCount = count;
                }
                total += count;
            }
            if (rangeCount > 0u)
            {
                emit(rangeFirst, rangeCount);
            }
            return total;
        }
    } // namespace

    u32 ShadowRenderPass::GroomCasterViewIndexCount(const ShadowGroomCaster& caster, const glm::mat4& viewProjection,
                                                    const glm::vec3& origin, f32 resolutionTexels)
    {
        return VisitGroomCasterRanges(caster, viewProjection, origin, resolutionTexels, [](u32, u32) {});
    }

    void ShadowRenderPass::RenderGroomCasters(const Frustum* cullFrustum, const glm::mat4& viewProjection,
                                              const glm::vec3& renderOrigin, f32 resolutionTexels, i32 clipLevel,
                                              UniformBuffer& paramsUBO) const
    {
        if (m_GroomCasters.IsEmpty())
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        // Ribbons are two-sided by construction: a widened quad has no
        // meaningful winding, and the pass's front-face cull would drop half of
        // every coat. Restored below, because the caster families after this one
        // rely on the FrontCull the pass set once up front.
        RenderCommand::DisableCulling();

        for (const auto& caster : m_GroomCasters)
        {
            if (!caster.vaoID.IsValid() || caster.indexCount == 0u)
            {
                continue;
            }
            if (cullFrustum != nullptr && ShouldCull(caster.WorldBounds, *cullFrustum))
            {
                continue;
            }

            UBOStructures::GroomShadowParamsUBO params;
            params.Model = MakeModelRelative(caster.transform, renderOrigin);
            params.Width = glm::vec4(caster.widthScale, caster.objectScale, resolutionTexels, caster.minWidthTexels);
            params.Modes = glm::ivec4(clipLevel, 0, 0, 0);
            // GPU strand deformation (#1427): the SAME lanes and the same buffer
            // the strand draw uses, so a bound coat casts from the pose the
            // camera sees. BOUND BEFORE EVERY DRAW, never trusted to survive:
            // the binding is shared with the terrain VT under a rebound-per-use
            // rule (ShaderBindingLayout.h, SSBO_GROOM_DEFORMATION), and the
            // groom pass guarantees a buffer here -- the zeroed placeholder at
            // mode 0 -- so the declared block always has an occupant.
            params.DeformModes = caster.deformModes;
            params.DeformBases = caster.deformBases;
            if (caster.deformBuffer != nullptr)
            {
                caster.deformBuffer->Bind();
            }
            // UPLOAD, THEN BIND -- in that order, every draw. The Vulkan
            // backend's UBOs are arena-versioned: SetData mints a NEW
            // allocation (ADR 0011 section 4), so binding first publishes the
            // address of the PREVIOUS one and every ribbon is widened by
            // another caster's numbers. Same order, same reason, as
            // GroomRenderPass's own params upload.
            paramsUBO.SetData(&params, UBOStructures::GroomShadowParamsUBO::GetSize());
            paramsUBO.Bind();

            // The share of each run this view needs (#1533): a prefix of each
            // run of the caster order, a uniform random share of that run's
            // strands, in as few draws as the runs cast whole allow.
            (void)VisitGroomCasterRanges(caster, viewProjection, renderOrigin, resolutionTexels,
                                         [&](u32 firstIndex, u32 count)
                                         { RenderCommand::DrawIndexedRaw(caster.vaoID, count, firstIndex); });
        }

        RenderCommand::EnableCulling();
        RenderCommand::FrontCull();
    }

    u32 ShadowRenderPass::RenderGroomVirtualShadowLevels(VirtualShadowMap& vsm)
    {
        if (m_GroomCasters.IsEmpty() || !m_GroomVsmDepthShader || !m_GroomVsmParamsUBO)
        {
            return 0;
        }

        OLO_PROFILE_FUNCTION();

        const glm::vec3 renderOrigin = Renderer3D::GetRenderOrigin();
        const auto& clips = vsm.GetClipProjections();

        u32 levelDraws = 0;
        u64 segmentsCast = 0;
        u64 segmentsWhole = 0;
        for (u32 level = 0; level < VSM::kClipLevels; ++level)
        {
            // Only the levels a coat actually reaches. A level nothing touches
            // is dropped here rather than rasterised and discarded page by page
            // on the GPU -- sixteen clip levels against four cascades is the one
            // place this route could cost more than the one it replaces.
            //
            // The clip projections are RENDER-RELATIVE, so the caster's world
            // box is shifted by the same origin before the test. Getting that
            // backwards is invisible near the world origin, which is where every
            // test scene sits, and wrong everywhere else (issue #429).
            const bool reached = std::ranges::any_of(
                m_GroomCasters,
                [&](const ShadowGroomCaster& caster)
                {
                    if (caster.WorldBounds.Min.x >= std::numeric_limits<f32>::max())
                    {
                        return true; // no bounds -- include in every level
                    }
                    return VirtualShadowMap::BoundsReachClipLevel(clips[level].ViewProjection,
                                                                  caster.WorldBounds.Min - renderOrigin,
                                                                  caster.WorldBounds.Max - renderOrigin);
                });
            if (!reached)
            {
                continue;
            }

            // THE POOL IMAGE IS RE-BOUND AFTER OUR PROGRAM, EVERY LEVEL.
            // BindPhysicalPoolImage forks on whether the program currently in
            // flight is bindless, so it cannot be hoisted out of the shader
            // switch -- and it is not enough that the mesh raster bound it a
            // moment ago, because in a scene whose only casters are grooms the
            // mesh raster returns before binding anything. The failure is every
            // imageAtomicMin being discarded: a silently unshadowed frame with
            // no error anywhere (virtual-geometry-into-a-second-shadow-technique.md
            // section 2).
            m_GroomVsmDepthShader->Bind();
            vsm.BindPhysicalPoolImage();

            // NO FRUSTUM here, deliberately: the level was already chosen
            // by the reach test above, and the fragment stage discards any
            // page that is not allocated and dirty anyway. A second CPU cull
            // would only remove draws the raster already throws away.
            RenderGroomCasters(/*cullFrustum=*/nullptr, clips[level].ViewProjection, renderOrigin,
                               static_cast<f32>(VSM::kVirtualResolution), static_cast<i32>(level),
                               *m_GroomVsmParamsUBO);
            ++levelDraws;
            // One sequential region, so the tally is kept as it draws.
            for (const auto& caster : m_GroomCasters)
            {
                if (caster.vaoID.IsValid() && caster.indexCount > 0u)
                {
                    segmentsCast += GroomCasterViewIndexCount(caster, clips[level].ViewProjection, renderOrigin,
                                                              static_cast<f32>(VSM::kVirtualResolution)) /
                                    6u;
                    segmentsWhole += caster.indexCount / 6u;
                }
            }
        }

        if (levelDraws > 0 && m_GroomPass != nullptr)
        {
            GroomShadowCasterStats& stats = m_GroomPass->MutableSceneShadowStats();
            stats.VirtualShadowLevelDraws += levelDraws;
            stats.SegmentsCast += segmentsCast;
            stats.SegmentsWhole += segmentsWhole;
        }
        return levelDraws;
    }

    bool ShadowRenderPass::CollectVirtualCasterBounds()
    {
        OLO_PROFILE_FUNCTION();
        m_VsmVirtualBounds.Reset();
        return VirtualGeometryShadow::CollectShadowCasterBounds(m_VsmVirtualBounds);
    }

    void ShadowRenderPass::BuildVirtualClipViews(const VirtualShadowMap& vsm)
    {
        OLO_PROFILE_FUNCTION();
        m_VsmClipViews.Reset();
        if (m_VsmVirtualBounds.IsEmpty())
            return;

        // The same eight-corner NDC test VSM_CullCasters.comp runs per caster,
        // asked once per level over the virtual instances. The SWEPT bounds, so
        // a level a mover is leaving still gets the dispatch that redraws the
        // pages it is vacating.
        const auto reachesLevel = [this](const glm::mat4& viewProjection)
        {
            for (const auto& caster : m_VsmVirtualBounds)
            {
                const glm::vec3 sweptMin = glm::min(caster.Min, caster.PrevMin);
                const glm::vec3 sweptMax = glm::max(caster.Max, caster.PrevMax);
                if (VirtualShadowMap::BoundsReachClipLevel(viewProjection, sweptMin, sweptMax))
                    return true;
            }
            return false;
        };

        const auto& clips = vsm.GetClipProjections();
        for (u32 level = 0; level < VSM::kClipLevels; ++level)
        {
            if (!reachesLevel(clips[level].ViewProjection))
                continue;
            VirtualGeometryShadow::VsmClipView view;
            view.ViewProjection = clips[level].ViewProjection;
            view.PageOffset = clips[level].PageOffset;
            view.ClipLevel = level;
            m_VsmClipViews.Add(view);
        }
    }

    void ShadowRenderPass::SubmitVirtualDynamicInvalidations(VirtualShadowMap& vsm)
    {
        OLO_PROFILE_FUNCTION();

        // ---- Movers and arrivals --------------------------------------------
        for (const auto& caster : m_VsmVirtualBounds)
        {
            const auto previous = m_PrevVirtualCasters.find(caster.Key);
            const bool isNew = previous == m_PrevVirtualCasters.end();

            // ShadowCasterBounds::Moved compares the TRANSFORMS, not these boxes:
            // a rotation about the centre of a symmetric caster leaves the world
            // AABB bit-identical while changing the silhouette the shadow map
            // holds, and comparing the boxes would freeze such a caster's shadow
            // at its first angle.
            if (!isNew && !caster.Moved)
                continue;

            // The SWEPT volume, not the poses separately: a caster that moved
            // further than its own size in one frame leaves pages dirty between
            // the two, and those are exactly the ones holding its old silhouette.
            // Last frame's footprint joins the sweep as well, because a caster
            // can be re-enabled somewhere else entirely.
            glm::vec3 sweptMin = glm::min(caster.Min, caster.PrevMin);
            glm::vec3 sweptMax = glm::max(caster.Max, caster.PrevMax);
            if (!isNew)
            {
                sweptMin = glm::min(sweptMin, previous->second.Min);
                sweptMax = glm::max(sweptMax, previous->second.Max);
            }
            vsm.AddDynamicInvalidation(sweptMin, sweptMax);
        }

        // ---- Departures ------------------------------------------------------
        //
        // A caster that was deleted, or had CastShadows unticked, is simply GONE
        // from this frame's list — there is nothing to compare it against, so the
        // loop above can never reach it and its silhouette would sit in a cached
        // page until something else happened to evict it. Its LAST known
        // footprint is the only record of where that silhouette is.
        for (const auto& [key, footprint] : m_PrevVirtualCasters)
        {
            const bool stillPresent = std::ranges::any_of(m_VsmVirtualBounds,
                                                          [key](const auto& caster)
                                                          { return caster.Key == key; });
            if (!stillPresent)
                vsm.AddDynamicInvalidation(footprint.Min, footprint.Max);
        }

        m_PrevVirtualCasters.clear();
        for (const auto& caster : m_VsmVirtualBounds)
            m_PrevVirtualCasters.emplace(caster.Key, VirtualCasterFootprint{ caster.Min, caster.Max });
    }

    void ShadowRenderPass::SubmitGroomDynamicInvalidations(VirtualShadowMap& vsm)
    {
        OLO_PROFILE_FUNCTION();

        // AddDynamicInvalidation takes RENDER-RELATIVE bounds; a caster's world
        // box is absolute (issue #429). Invisible near the origin, where every
        // test scene sits, and a stale shadow everywhere else.
        const glm::vec3 renderOrigin = Renderer3D::GetRenderOrigin();
        const auto hasBounds = [](const BoundingBox& box)
        { return box.Min.x < std::numeric_limits<f32>::max(); };
        u32 invalidations = 0;
        // The negative control's fault: track the footprints, submit nothing.
        const bool submit = !Levers::FaultSkipGroomVsmInvalidation();

        // ---- Movers and arrivals --------------------------------------------
        for (const auto& caster : m_GroomCasters)
        {
            if (!hasBounds(caster.WorldBounds))
            {
                continue; // no box to invalidate; CollectGroomCasters found no bounds
            }
            const auto previous = m_PrevGroomCasters.find(caster.key);
            const bool isNew = previous == m_PrevGroomCasters.end();
            // A BOUND coat moves with its body's pose every frame, so it is always
            // a mover; an unbound one moves when its transform does. Compared
            // bit-exactly, like ShadowCasterBounds::Moved: a rotation about the
            // centre of a symmetric coat leaves the box identical and the
            // silhouette not.
            const bool moved = caster.deforming || isNew ||
                               !Math::BitwiseEqual(previous->second.Transform, caster.transform);
            if (!moved)
            {
                continue;
            }
            // The SWEPT volume: last frame's footprint joins this frame's, because
            // the old silhouette is exactly what sits in the cached pages.
            glm::vec3 sweptMin = caster.WorldBounds.Min;
            glm::vec3 sweptMax = caster.WorldBounds.Max;
            if (!isNew)
            {
                sweptMin = glm::min(sweptMin, previous->second.Min);
                sweptMax = glm::max(sweptMax, previous->second.Max);
            }
            if (submit)
            {
                vsm.AddDynamicInvalidation(sweptMin - renderOrigin, sweptMax - renderOrigin);
                ++invalidations;
            }
        }

        // ---- Departures ------------------------------------------------------
        //
        // A coat whose component was removed, whose CastShadows was unticked or
        // whose entity was deleted is simply GONE from this frame's list; its
        // last footprint is the only record of where its silhouette is.
        for (const auto& [key, footprint] : m_PrevGroomCasters)
        {
            const bool stillPresent = std::ranges::any_of(m_GroomCasters,
                                                          [key](const ShadowGroomCaster& caster)
                                                          { return caster.key == key; });
            if (!stillPresent && submit)
            {
                vsm.AddDynamicInvalidation(footprint.Min - renderOrigin, footprint.Max - renderOrigin);
                ++invalidations;
            }
        }

        m_PrevGroomCasters.clear();
        for (const auto& caster : m_GroomCasters)
        {
            if (hasBounds(caster.WorldBounds))
            {
                m_PrevGroomCasters.emplace(caster.key, GroomCasterFootprint{ caster.WorldBounds.Min,
                                                                             caster.WorldBounds.Max, caster.transform });
            }
        }

        if (invalidations > 0u && m_GroomPass != nullptr)
        {
            m_GroomPass->MutableSceneShadowStats().VirtualShadowInvalidations += invalidations;
        }
    }

    bool ShadowRenderPass::AnyVirtualShadowCaster()
    {
        // Read SUBMISSIONS, not frame instances.
        //
        // GetFrameInstances() is the wrong list and reading it here is a chicken-and-egg: it is
        // populated by VirtualMeshRegistry::PrepareFrame(), which runs INSIDE
        // VirtualGeometryShadow::RenderCascade — i.e. after this gate has already decided
        // whether to run at all. Gating on it means the gate is always closed, the cascade never
        // renders, PrepareFrame never runs, and the list stays empty forever. (The first version
        // of this fix did exactly that and changed nothing.)
        //
        // GetSubmissions() is filled by Scene::OnUpdateRender via Renderer3D::SubmitVirtualMesh,
        // which happens BEFORE the render graph executes — so it is the only list that is
        // actually populated at gate time, and it already carries the per-entity CastShadows flag.
        //
        // Deliberately does NOT call PrepareFrame() itself: this is only the cheap "is it worth
        // opening the shadow pass at all" question, and RenderCascade does the real preparation
        // when a cascade actually renders.
        const auto& submissions = VirtualMeshRegistry::Get().GetSubmissions();
        return std::ranges::any_of(submissions,
                                   [](const VirtualMeshSubmission& s)
                                   { return s.CastShadows; });
    }

    // Returns true when the caster has valid world bounds AND those bounds lie
    // entirely outside the frustum, meaning it can safely be skipped.
    // Casters with NoBounds (Min.x == FLT_MAX) are never culled.
    bool ShadowRenderPass::ShouldCull(const BoundingBox& worldBounds, const Frustum& frustum)
    {
        if (worldBounds.Min.x >= std::numeric_limits<f32>::max())
            return false; // No bounds provided — always include
        return !frustum.IsBoxVisible(worldBounds.Min, worldBounds.Max);
    }

    // Shadow caster submission methods
    void ShadowRenderPass::AddMeshCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                         RHI::ResourceHandle shadowVaoID, const BoundingBox& worldBounds, bool twoSided)
    {
        m_MeshCasters.Add({ vaoID, indexCount, baseIndex, transform, shadowVaoID, worldBounds, twoSided });
    }

    void ShadowRenderPass::AddSkinnedCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 baseIndex, const glm::mat4& transform,
                                            u32 boneBufferOffset, u32 boneCount, const BoundingBox& worldBounds)
    {
        m_SkinnedCasters.Add({ vaoID, indexCount, baseIndex, transform, boneBufferOffset, boneCount, worldBounds });
    }

    void ShadowRenderPass::SubmitFamilyDynamicInvalidations(VirtualShadowMap& vsm)
    {
        const auto key = [](const auto& caster)
        {
            if (caster.Key != 0)
                return caster.Key;
            u64 value = (static_cast<u64>(caster.vaoID.Generation) << 32) | caster.vaoID.Index;
            const auto* bytes = reinterpret_cast<const u8*>(&caster.transform);
            for (sizet i = 0; i < sizeof(caster.transform); ++i)
                value = (value ^ bytes[i]) * 1099511628211ull;
            return value;
        };
        const auto invalidate = [&vsm](const BoundingBox& bounds)
        {
            const glm::vec3 origin = Renderer3D::GetRenderOrigin();
            if (bounds.Min.x < std::numeric_limits<f32>::max())
                vsm.AddDynamicInvalidation(bounds.Min - origin, bounds.Max - origin);
            else
                // Legacy external submissions can omit a box. Include every
                // page conservatively rather than retain an unknown silhouette.
                vsm.AddDynamicInvalidation(glm::vec3(-1e20f), glm::vec3(1e20f));
        };
        TArray64<ShadowFamilyFootprint> footprints;
        for (const auto& caster : m_TerrainCasters)
        {
            // Tessellation/morph and authored height-content revisions both
            // change the silhouette without moving the model matrix.
            u64 revision = caster.Revision ^ (static_cast<u64>(caster.heightmapTextureID.Generation) << 32) ^ caster.heightmapTextureID.Index;
            revision = TerrainShadowRevision(revision, caster.terrainUBO);
            footprints.Add({ key(caster), caster.WorldBounds, caster.transform, revision,
                             Renderer3D::GetSnowAccumulationSettings().Enabled || caster.WorldBounds.Min.x >= std::numeric_limits<f32>::max() });
        }
        m_FamilyCaches[0].Update({ footprints.GetData(), static_cast<sizet>(footprints.Num()) }, invalidate);
        footprints.Reset();
        for (const auto& caster : m_VoxelCasters)
        {
            footprints.Add({ key(caster), caster.WorldBounds, caster.transform, caster.Revision ^ caster.indexCount,
                             (caster.instanceCount > 0 && caster.Revision == 0) || caster.WorldBounds.Min.x >= std::numeric_limits<f32>::max() });
        }
        m_FamilyCaches[1].Update({ footprints.GetData(), static_cast<sizet>(footprints.Num()) }, invalidate);
        footprints.Reset();
        for (const auto& caster : m_FoliageCasters)
        {
            if (!caster.renderer)
                continue;
            const auto bounds = caster.renderer->GetShadowBounds();
            if (bounds.Min.x >= std::numeric_limits<f32>::max())
                continue;
            // Wind, interactions, card facing and density/mesh LOD depend on
            // time and the main eye even when the terrain itself is stationary.
            footprints.Add({ static_cast<u64>(reinterpret_cast<uintptr_t>(caster.renderer)), bounds,
                             glm::mat4(1.0f), caster.renderer->GetInstanceRegistry().GetGeneration(), true });
        }
        m_FamilyCaches[2].Update({ footprints.GetData(), static_cast<sizet>(footprints.Num()) }, invalidate);
    }

    u32 ShadowRenderPass::RenderFamilyVirtualViews(VirtualShadowMap& vsm, bool local)
    {
        const glm::vec3 origin = Renderer3D::GetRenderOrigin();
        const glm::mat4 toWorld = glm::translate(glm::mat4(1.0f), -origin);
        const u32 count = local ? vsm.GetLocalLayerCount() : VSM::kClipLevels;
        BoundingBox bounds = NoBounds;
        bool unbounded = false;
        const auto include = [&](const BoundingBox& box)
        {
            if (box.Min.x >= std::numeric_limits<f32>::max())
                unbounded = true;
            else
                bounds = bounds.Min.x >= std::numeric_limits<f32>::max() ? box : bounds.Union(box);
        };
        for (const auto& caster : m_TerrainCasters)
            include(caster.WorldBounds);
        for (const auto& caster : m_VoxelCasters)
            include(caster.WorldBounds);
        for (const auto& caster : m_FoliageCasters)
        {
            if (caster.renderer)
            {
                const auto foliageBounds = caster.renderer->GetShadowBounds();
                if (foliageBounds.Min.x < std::numeric_limits<f32>::max())
                    include(foliageBounds);
            }
        }
        if (!unbounded && bounds.Min.x >= std::numeric_limits<f32>::max())
            return 0;
        u32 drawn = 0;
        for (u32 view = 0; view < count; ++view)
        {
            glm::mat4 vp;
            if (local)
            {
                const auto& layer = vsm.GetLocalLayer(view);
                if (layer.Params.w < 0.5f)
                    continue;
                vp = layer.ViewProjection;
            }
            else
                vp = vsm.GetClipProjections()[view].ViewProjection;
            const glm::mat4 worldVP = vp * toWorld;
            const Frustum frustum(worldVP);
            if (!unbounded && ShouldCull(bounds, frustum))
                continue;
            FamilyViewParams params;
            params.View = glm::ivec4(local ? 1 : 0, static_cast<i32>(view), unbounded ? 1 : 0, 0);
            params.BoundsMin = glm::vec4(bounds.Min - origin, 0.0f);
            params.BoundsMax = glm::vec4(bounds.Max - origin, 0.0f);
            m_FamilyViewUBO->SetData(&params, sizeof(params));
            m_FamilyViewUBO->Bind();
            for (const auto& caster : m_FoliageCasters)
            {
                if (!caster.renderer)
                    continue;
                caster.renderer->ResetShadowViewCulling();
                caster.renderer->DispatchShadowViewCulling(0, caster.renderer->MakeCullInputs(worldVP, CommandDispatch::GetViewPosition()));
            }
            RenderCascadeOrFace(worldVP, local ? ShadowPassType::Atlas : ShadowPassType::CSM, view, &frustum,
                                m_FamilyVsmShaders, m_ItemResources[0], nullptr, nullptr, 0, &vsm);
            // The next view rewrites the foliage compacted streams and draw
            // args the previous draw reads. Views are sequential, with explicit
            // GPU ordering as well as command-ordered UBO uploads.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command |
                                         MemoryBarrierFlags::VertexAttribArray);
            ++drawn;
        }
        return drawn;
    }

    void ShadowRenderPass::AddTerrainCaster(RHI::ResourceHandle vaoID, u32 indexCount, u32 patchVertexCount,
                                            const glm::mat4& transform, RHI::ResourceHandle heightmapTextureID,
                                            const ShaderBindingLayout::TerrainUBO& terrainUBO,
                                            const ShadowCasterFootprint& footprint)
    {
        m_TerrainCasters.Add({ vaoID, indexCount, patchVertexCount, transform, heightmapTextureID, terrainUBO, footprint.Bounds, footprint.Revision, footprint.Key });
    }

    void ShadowRenderPass::AddVoxelCaster(RHI::ResourceHandle vaoID, u32 indexCount, const glm::mat4& transform,
                                          u32 instanceCount, const ShadowCasterFootprint& footprint)
    {
        m_VoxelCasters.Add({ vaoID, indexCount, instanceCount, transform, footprint.Bounds, footprint.Revision, footprint.Key });
    }

    void ShadowRenderPass::AddFoliageCaster(FoliageRenderer* renderer, const Ref<Shader>& depthShader, f32 time)
    {
        m_FoliageCasters.Add({ renderer, depthShader, time });
    }

    Ref<Framebuffer> ShadowRenderPass::GetTarget() const
    {
        OLO_PROFILE_FUNCTION();
        return m_ShadowFramebuffer;
    }

    void ShadowRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        // Shadow pass resolution is managed by ShadowMap::m_Settings, not the framebuffer spec
        ResizeFramebuffer(width, height);
    }

    void ShadowRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ShadowRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        m_WarnedOnce = false;
        m_LoggedOnce = false;
        // The per-item pool is GPU resources of the same generation as the
        // framebuffer being rebuilt below; drop it with the framebuffer and let
        // the next Execute recreate what it needs (render thread, before the fork).
        m_ItemResources.Reset();
        m_ItemTallies.Reset();
        m_ActiveViews.Reset();
        // The groom caster list points at VAOs and deformation buffers owned by
        // GroomRenderPass, which resets on the same pipeline reset.
        m_GroomCasters.Reset();
        m_PrevGroomCasters.clear();
        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            Init(m_FramebufferSpec);
        }
    }
} // namespace OloEngine
