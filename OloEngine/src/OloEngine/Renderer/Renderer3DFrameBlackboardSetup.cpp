#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <cstdlib>

namespace OloEngine
{
    namespace
    {
        bool IsTruthyEnvironmentVariable(const char* name)
        {
            const char* value = std::getenv(name);
            return value && value[0] != '\0' && value[0] != '0' && value[0] != 'f' && value[0] != 'F';
        }

        bool IsRenderGraphDiagnosticsEnabled()
        {
            static const bool enabled = IsTruthyEnvironmentVariable("OLO_RENDERGRAPH_DIAGNOSTICS");
            return enabled;
        }
    } // namespace

    void Renderer3D::SetupFrameBlackboard()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.RGraph)
            return;

        auto& graph = *s_Data.RGraph;

        // Clear prior-frame handles so stale handles are never accidentally resolved.
        graph.ClearBlackboard();
        graph.ClearImportedResources();

        auto& board = graph.GetBlackboard();

        // ------------------------------------------------------------------
        // Scene outputs
        // ------------------------------------------------------------------
        if (s_Data.ScenePass && s_Data.ScenePass->GetTarget())
        {
            board.SceneColor = graph.ImportFramebuffer(
                ResourceNames::SceneColor, s_Data.ScenePass->GetTarget());

            // Sanity-check: importing must immediately resolve to the same
            // framebuffer. If not, the RenderGraph handle layer is broken.
            // Logged ONCE per change so we notice regressions without
            // spamming the log every frame.
            {
                static u32 s_PrevFbGL = 0;
                static u32 s_PrevTex0 = 0;
                const auto importedFB = s_Data.ScenePass->GetTarget();
                const auto resolveNow = graph.ResolveFramebuffer(board.SceneColor);
                const u32 importedFbGL = importedFB->GetRendererID();
                const u32 importedTex0 = importedFB->GetColorAttachmentRendererID(0);
                const u32 resolveFbGL = resolveNow ? resolveNow->GetRendererID() : 0u;
                const u32 resolveTex0 = resolveNow ? resolveNow->GetColorAttachmentRendererID(0) : 0u;
                if (importedFbGL != resolveFbGL || importedTex0 != resolveTex0)
                {
                    OLO_CORE_ERROR("Renderer3D: SceneColor IMPORT/RESOLVE MISMATCH: handle=(idx={}, gen={}) importedFbGL={} importedTex0={} resolveFbGL={} resolveTex0={}",
                                   board.SceneColor.Index, board.SceneColor.Generation,
                                   importedFbGL, importedTex0, resolveFbGL, resolveTex0);
                }
                else if (importedFbGL != s_PrevFbGL || importedTex0 != s_PrevTex0)
                {
                    if (IsRenderGraphDiagnosticsEnabled())
                    {
                        OLO_CORE_TRACE("Renderer3D: SceneColor IMPORT OK: handle=(idx={}, gen={}) fbGL={} tex0={}",
                                       board.SceneColor.Index, board.SceneColor.Generation, importedFbGL, importedTex0);
                    }
                    s_PrevFbGL = importedFbGL;
                    s_PrevTex0 = importedTex0;
                }
            }

            // AO/Deferred consumers need true geometric depth + view-space
            // normals. In Deferred mode these come from the G-Buffer
            // resolved attachments (not from ScenePass target attachments,
            // which are cleared placeholders for overlay consumers).
            const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
            const bool hasGBuffer = deferredActive && s_Data.ScenePass->GetGBuffer();
            if (deferredActive && !hasGBuffer)
            {
                static bool s_WarnedDeferredAOPlaceholderInputs = false;
                if (!s_WarnedDeferredAOPlaceholderInputs)
                {
                    OLO_CORE_WARN("Renderer3D: Deferred path missing GBuffer; SceneDepth/SceneNormals are imported from ScenePass target placeholder attachments");
                    s_WarnedDeferredAOPlaceholderInputs = true;
                }
            }

            const u32 depthID = hasGBuffer
                                    ? s_Data.ScenePass->GetGBuffer()->GetDepthAttachmentID()
                                    : s_Data.ScenePass->GetTarget()->GetDepthAttachmentRendererID();
            auto sceneDepthDesc = RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::SceneDepth);
            if (deferredActive && !hasGBuffer)
            {
                sceneDepthDesc.IsPlaceholder = true;
                sceneDepthDesc.PlaceholderReason = "Deferred mode fallback to ScenePass depth attachment because GBuffer is unavailable";
            }
            board.SceneDepth = graph.ImportTexture(
                ResourceNames::SceneDepth, depthID, sceneDepthDesc);

            const u32 sceneNormalsID = hasGBuffer
                                           ? s_Data.ScenePass->GetGBuffer()->GetColorAttachmentID(GBuffer::Normal)
                                           : s_Data.ScenePass->GetTarget()->GetColorAttachmentRendererID(2);
            auto sceneNormalsDesc = RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::SceneNormals);
            if (deferredActive && !hasGBuffer)
            {
                sceneNormalsDesc.IsPlaceholder = true;
                sceneNormalsDesc.PlaceholderReason = "Deferred mode fallback to ScenePass normals attachment because GBuffer is unavailable";
            }
            board.SceneNormals = graph.ImportTexture(
                ResourceNames::SceneNormals, sceneNormalsID, sceneNormalsDesc);
        }

        // ------------------------------------------------------------------
        // G-Buffer (deferred path only)
        // ------------------------------------------------------------------
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        if (deferredActive && s_Data.ScenePass && s_Data.ScenePass->GetGBuffer())
        {
            // G-Buffer attachments come from the dedicated GBuffer object,
            // NOT from ScenePass->GetTarget() (which is the lit-output FB in
            // deferred mode and only has a single color attachment).
            //
            // Layout matches GBuffer::AttachmentIndex:
            //   RT0 Albedo   — albedo.rgb + metallic.a
            //   RT1 Normal   — octahedral normal + roughness + AO
            //   RT2 Emissive — emissive HDR
            //   RT3 Velocity — exposed via the dedicated `Velocity` import
            //                  block below; not duplicated here.
            const auto& gbuffer = s_Data.ScenePass->GetGBuffer();
            auto importGBuf = [&](std::string_view name, GBuffer::AttachmentIndex slot) -> RGTextureHandle
            {
                const u32 id = gbuffer->GetColorAttachmentID(slot);
                return graph.ImportTexture(name, id,
                                           RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, name));
            };

            board.GBufferAlbedo = importGBuf(ResourceNames::GBufferAlbedo, GBuffer::Albedo);
            board.GBufferNormal = importGBuf(ResourceNames::GBufferNormal, GBuffer::Normal);
            board.GBufferEmissive = importGBuf(ResourceNames::GBufferEmissive, GBuffer::Emissive);

            // Multisample companion handles. Imported
            // only when MSAA is active so the typed-handle path can drive
            // per-sample shading without going through the raw GBuffer
            // accessor. SceneDepthMS is also exposed here (rather than
            // alongside SceneDepth) because the multisample depth lives on
            // the G-Buffer, not on the lit scene framebuffer.
            if (gbuffer->GetSampleCount() > 1u)
            {
                auto importGBufMS = [&](std::string_view name, GBuffer::AttachmentIndex slot) -> RGTextureHandle
                {
                    const u32 id = gbuffer->GetMSColorAttachmentID(slot);
                    return graph.ImportTexture(name, id,
                                               RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, name));
                };

                board.GBufferAlbedoMS = importGBufMS(ResourceNames::GBufferAlbedoMS, GBuffer::Albedo);
                board.GBufferNormalMS = importGBufMS(ResourceNames::GBufferNormalMS, GBuffer::Normal);
                board.GBufferEmissiveMS = importGBufMS(ResourceNames::GBufferEmissiveMS, GBuffer::Emissive);
                board.VelocityMS = importGBufMS(ResourceNames::VelocityMS, GBuffer::Velocity);

                if (const u32 depthMSID = gbuffer->GetMSDepthAttachmentID(); depthMSID != 0)
                {
                    board.SceneDepthMS = graph.ImportTexture(
                        ResourceNames::SceneDepthMS, depthMSID,
                        RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::SceneDepthMS));
                }
            }
        }

        // ------------------------------------------------------------------
        // Velocity buffer
        // ------------------------------------------------------------------
        {
            u32 velocityID = 0;
            if (s_Data.Settings.Path == RenderingPath::Deferred &&
                s_Data.ScenePass && s_Data.ScenePass->GetGBuffer())
            {
                velocityID = s_Data.ScenePass->GetGBuffer()->GetColorAttachmentID(GBuffer::Velocity);
            }
            else if (s_Data.ScenePass && s_Data.ScenePass->GetTarget())
            {
                velocityID = s_Data.ScenePass->GetTarget()->GetColorAttachmentRendererID(3);
            }
            if (velocityID != 0)
            {
                board.Velocity = graph.ImportTexture(
                    ResourceNames::Velocity, velocityID,
                    RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::Velocity));
            }
        }

        // ------------------------------------------------------------------
        // AO buffer
        // ------------------------------------------------------------------
        {
            u32 aoID = 0;
            if (s_Data.SceneCompositePasses.SSAO && s_Data.PostProcess.SSAOEnabled &&
                s_Data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO)
            {
                aoID = s_Data.SceneCompositePasses.SSAO->GetSSAOTextureID();
            }
            else if (s_Data.SceneCompositePasses.GTAO && s_Data.PostProcess.GTAOEnabled &&
                     s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO)
            {
                aoID = s_Data.SceneCompositePasses.GTAO->GetGTAOTextureID();
            }
            if (aoID != 0)
            {
                board.AOBuffer = graph.ImportTexture(
                    ResourceNames::AOBuffer, aoID,
                    RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::AOBuffer));
            }

            static u32 s_PrevAOID = 0;
            static i32 s_PrevAOTechnique = -1;
            static bool s_PrevSSAOEnabled = false;
            static bool s_PrevGTAOEnabled = false;
            const i32 activeTechnique = static_cast<i32>(s_Data.PostProcess.ActiveAOTechnique);
            if (aoID != s_PrevAOID ||
                activeTechnique != s_PrevAOTechnique ||
                s_Data.PostProcess.SSAOEnabled != s_PrevSSAOEnabled ||
                s_Data.PostProcess.GTAOEnabled != s_PrevGTAOEnabled)
            {
                if (IsRenderGraphDiagnosticsEnabled())
                {
                    OLO_CORE_TRACE("Renderer3D: AO import state: technique={}, ssaoEnabled={}, gtaoEnabled={}, aoTexID={}, aoHandleValid={}",
                                   activeTechnique,
                                   s_Data.PostProcess.SSAOEnabled,
                                   s_Data.PostProcess.GTAOEnabled,
                                   aoID,
                                   board.AOBuffer.IsValid());
                }
                s_PrevAOID = aoID;
                s_PrevAOTechnique = activeTechnique;
                s_PrevSSAOEnabled = s_Data.PostProcess.SSAOEnabled;
                s_PrevGTAOEnabled = s_Data.PostProcess.GTAOEnabled;
            }
        }

        // ------------------------------------------------------------------
        // Shadow maps
        // ------------------------------------------------------------------
        {
            const u32 csmID = s_Data.Shadow.GetCSMRendererID();
            const u32 spotID = s_Data.Shadow.GetSpotRendererID();
            if (csmID != 0)
            {
                board.ShadowMapCSM = graph.ImportTexture(
                    ResourceNames::ShadowMapCSM, csmID,
                    RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2DArray, ResourceNames::ShadowMapCSM));
            }
            if (spotID != 0)
            {
                board.ShadowMapSpot = graph.ImportTexture(
                    ResourceNames::ShadowMapSpot, spotID,
                    RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2DArray, ResourceNames::ShadowMapSpot));
            }
            // Point-light shadow cubemaps — import each active light slot separately.
            for (u32 i = 0; i < ShadowMap::MAX_POINT_SHADOWS; ++i)
            {
                const u32 pointID = s_Data.Shadow.GetPointRendererID(i);
                if (pointID != 0)
                {
                    board.ShadowMapPoint[i] = graph.ImportTexture(
                        ResourceNames::ShadowMapPoint[i], pointID,
                        RGResourceDesc::FromLegacy(ResourceHandle::Kind::TextureCube, ResourceNames::ShadowMapPoint[i]));
                }
            }
        }

        // ------------------------------------------------------------------
        // Post-process chain outputs (Phase D Slice 8)
        // Each post-process output is now declared as a transient framebuffer
        // so the graph's transient pool tracks its lifetime and format.  The
        // real pass-owned FB is injected later (EndScene, after BuildFrameGraph)
        // via OverrideTransientFramebuffer so that ResolveFramebuffer returns
        // the actual GL framebuffer.  This replaces the former ImportFramebuffer
        // calls and enables future lifetime-based aliasing of intermediate
        // post-process buffers.
        // ------------------------------------------------------------------
        // Helper: build a single-attachment transient framebuffer descriptor.
        auto declarePostProcessFB =
            [&](std::string_view name, const Ref<Framebuffer>& target, RGResourceFormat fmt) -> RGFramebufferHandle
        {
            if (!target)
                return {};
            const auto& spec = target->GetSpecification();
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Width = spec.Width > 0 ? spec.Width : 1u;
            desc.Height = spec.Height > 0 ? spec.Height : 1u;
            desc.Format = fmt;
            desc.DebugName = std::string(name);
            // Pass the real pass-owned FB so the graph wires it automatically
            // — no separate OverrideTransientFramebuffer call needed in EndScene.
            return graph.DeclareTransientFramebuffer(name, desc, target);
        };

        if (s_Data.PostProcessPasses.SSS && s_Data.Snow.Enabled && s_Data.Snow.SSSBlurEnabled)
            board.SSSColor = declarePostProcessFB(ResourceNames::SSSColor, s_Data.PostProcessPasses.SSS->GetTarget(), RGResourceFormat::RGBA16Float);

        // AOApplyColor is declared only when SSAO or GTAO is enabled.
        if (s_Data.PostProcessPasses.AOApply)
        {
            const bool ssaoEnabled = s_Data.PostProcess.ActiveAOTechnique == AOTechnique::SSAO && s_Data.PostProcess.SSAOEnabled;
            const bool gtaoEnabled = s_Data.PostProcess.ActiveAOTechnique == AOTechnique::GTAO && s_Data.PostProcess.GTAOEnabled;
            if (ssaoEnabled || gtaoEnabled)
            board.AOApplyColor = declarePostProcessFB(ResourceNames::AOApplyColor, s_Data.PostProcessPasses.AOApply->GetTarget(), RGResourceFormat::RGBA16Float);
        }

        // PostProcessColor is an alias handle to the latest upstream graph
        // resource in the dynamic chain, NOT a separate imported resource.
        // This preserves declaration-derived reachability:
        //   AOApplyColor -> Bloom, SSSColor -> Bloom, or SceneColor -> Bloom.
        // Importing a fresh `PostProcessColor` framebuffer here severs that
        // producer/consumer chain, which lets AO/SSS get culled and can feed
        // stale/black data into the post stack.
        if (board.AOApplyColor.IsValid())
            board.PostProcessColor = board.AOApplyColor;
        else if (board.SSSColor.IsValid())
            board.PostProcessColor = board.SSSColor;
        else
            board.PostProcessColor = board.SceneColor;

        // BloomColor is declared only when Bloom is enabled.
        if (s_Data.PostProcessPasses.Bloom && s_Data.PostProcess.BloomEnabled)
            board.BloomColor = declarePostProcessFB(ResourceNames::BloomColor, s_Data.PostProcessPasses.Bloom->GetTarget(), RGResourceFormat::RGBA16Float);

        // DOFColor is declared only when DOF is enabled.
        if (s_Data.PostProcessPasses.DOF && s_Data.PostProcess.DOFEnabled)
            board.DOFColor = declarePostProcessFB(ResourceNames::DOFColor, s_Data.PostProcessPasses.DOF->GetTarget(), RGResourceFormat::RGBA16Float);

        // MotionBlurColor is declared only when motion blur is enabled.
        if (s_Data.PostProcessPasses.MotionBlur && s_Data.PostProcess.MotionBlurEnabled)
            board.MotionBlurColor = declarePostProcessFB(ResourceNames::MotionBlurColor, s_Data.PostProcessPasses.MotionBlur->GetTarget(), RGResourceFormat::RGBA16Float);

        // TAAColor is declared only when TAA is enabled.
        if (s_Data.PostProcessPasses.TAA && s_Data.PostProcess.TAAEnabled)
            board.TAAColor = declarePostProcessFB(ResourceNames::TAAColor, s_Data.PostProcessPasses.TAA->GetTarget(), RGResourceFormat::RGBA16Float);

        // PrecipitationColor is declared only when screen FX are active.
        const bool precipScreenEnabled = s_Data.Precipitation.Enabled &&
                                         (s_Data.Precipitation.ScreenStreaksEnabled ||
                                          s_Data.Precipitation.LensImpactsEnabled);
        if (s_Data.PostProcessPasses.Precipitation && precipScreenEnabled)
            board.PrecipitationColor = declarePostProcessFB(ResourceNames::PrecipitationColor, s_Data.PostProcessPasses.Precipitation->GetTarget(), RGResourceFormat::RGBA16Float);

        // FogColor is declared only when fog is enabled.
        if (s_Data.PostProcessPasses.Fog && s_Data.Fog.Enabled)
            board.FogColor = declarePostProcessFB(ResourceNames::FogColor, s_Data.PostProcessPasses.Fog->GetTarget(), RGResourceFormat::RGBA16Float);

        // Extracted effect sub-chain. Each handle is
        // declared only when its effect is enabled so downstream consumers
        // can rely on IsValid() as the canonical "effect ran" signal.
        // ToneMap is declared unconditionally (no settings gate).
        if (s_Data.PostProcessPasses.ChromAberration && s_Data.PostProcess.ChromaticAberrationEnabled)
            board.ChromAbColor = declarePostProcessFB(ResourceNames::ChromAbColor, s_Data.PostProcessPasses.ChromAberration->GetTarget(), RGResourceFormat::RGBA16Float);
        if (s_Data.PostProcessPasses.ColorGrading && s_Data.PostProcess.ColorGradingEnabled)
            board.ColorGradingColor = declarePostProcessFB(ResourceNames::ColorGradingColor, s_Data.PostProcessPasses.ColorGrading->GetTarget(), RGResourceFormat::RGBA16Float);
        if (s_Data.PostProcessPasses.ToneMap)
            board.ToneMapColor = declarePostProcessFB(ResourceNames::ToneMapColor, s_Data.PostProcessPasses.ToneMap->GetTarget(), RGResourceFormat::RGBA16Float);
        if (s_Data.PostProcessPasses.Vignette && s_Data.PostProcess.VignetteEnabled)
            board.VignetteColor = declarePostProcessFB(ResourceNames::VignetteColor, s_Data.PostProcessPasses.Vignette->GetTarget(), RGResourceFormat::RGBA8UNorm);

        // Only declare FXAAColor when FXAA is active so
        // downstream consumers can rely on `board.FXAAColor.IsValid()` as
        // the canonical "anti-aliased post-process available" signal.
        if (s_Data.PostProcessPasses.FXAA && s_Data.PostProcess.FXAAEnabled)
            board.FXAAColor = declarePostProcessFB(ResourceNames::FXAAColor, s_Data.PostProcessPasses.FXAA->GetTarget(), RGResourceFormat::RGBA8UNorm);

        if (s_Data.PostProcessPasses.SelectionOutline && s_Data.EnableSelectionOutline)
            board.SelectionOutlineColor = declarePostProcessFB(ResourceNames::SelectionOutlineColor, s_Data.PostProcessPasses.SelectionOutline->GetTarget(), RGResourceFormat::RGBA8UNorm);

        if (s_Data.PostProcessPasses.UIComposite)
            board.UIComposite = declarePostProcessFB(ResourceNames::UIComposite, s_Data.PostProcessPasses.UIComposite->GetTarget(), RGResourceFormat::RGBA8UNorm);

        // Default framebuffer / swapchain target represented as an imported
        // external output resource. Backing framebuffer is null by design;
        // FinalPass presents via RGCommandContext::BindDefaultFramebuffer().
        board.Backbuffer = graph.ImportFramebuffer(
            ResourceNames::Backbuffer, nullptr,
            RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, ResourceNames::Backbuffer));

        // ------------------------------------------------------------------
        // OIT buffers
        // ------------------------------------------------------------------
        // OIT graph resources are imported only when
        // weighted-blended OIT is *actually* active for this frame. Skipping
        // the import when OIT is disabled means transparent contributor
        // passes (Particle / Water / Decal) bail out of their
        // `builder.Write(board.OITAccum, ...)` declarations
        // (`if (board.OITAccum.IsValid())` is already guarded), so the
        // graph never sees write edges into a buffer that nothing reads.
        // OITResolvePass also self-skips via `m_Enabled`. The underlying
        // `OITBuffer` Ref persists across toggles to avoid allocator churn
        // when the user flips the setting interactively.
        const bool oitActive = (s_Data.Settings.Path == RenderingPath::Deferred) &&
                               s_Data.Settings.Deferred.OITEnabled &&
                               s_Data.SceneCompositePasses.OITResolve;
        if (oitActive)
        {
            // Trigger lazy creation so GetOrCreateOITBuffer() allocates the
            // physical GL framebuffer (if not already done).  We then declare
            // a transient MRT framebuffer in the render graph whose
            // MaterializeTransientResources will create an equivalent FB each
            // frame from the transient pool.
            const auto& oitBuf = s_Data.SceneCompositePasses.OITResolve->GetOrCreateOITBuffer();
            if (oitBuf && oitBuf->GetFramebuffer())
            {
                const auto& fbSpec = oitBuf->GetFramebuffer()->GetSpecification();
                const u32 oitW = fbSpec.Width > 0 ? fbSpec.Width : 1u;
                const u32 oitH = fbSpec.Height > 0 ? fbSpec.Height : 1u;

                // Declare as a shared transient MRT framebuffer (RT0 = RGBA16F
                // accumulation, RT1 = RG16F revealage). Both blackboard handles
                // point to the same physical transient FB; passes distinguish
                // the two attachments by colour attachment index (0 and 1).
                RGResourceDesc oitDesc;
                oitDesc.Kind = ResourceHandle::Kind::Framebuffer;
                oitDesc.Width = oitW;
                oitDesc.Height = oitH;
                oitDesc.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::RG16Float };
                oitDesc.DebugName = std::string(ResourceNames::OITBuffer);

                const auto oitHandle = graph.DeclareTransientFramebuffer(ResourceNames::OITBuffer, oitDesc,
                                                                         oitBuf->GetFramebuffer());
                board.OITAccum = oitHandle;
                board.OITRevealage = oitHandle;
            }
        }

        // ------------------------------------------------------------------
        // Temporal histories (imported from prior frame)
        // ------------------------------------------------------------------
        // TAAHistory is owned by TAARenderPass.
        if (s_Data.PostProcessPasses.TAA)
        {
            board.TAAHistory = graph.ImportHistory(
            ResourceNames::TAAHistory, s_Data.PostProcessPasses.TAA->GetTAAHistoryTextureID());
        }

        // FogHistory is owned by FogRenderPass.
        if (s_Data.PostProcessPasses.Fog)
        {
            board.FogHistory = graph.ImportHistory(
            ResourceNames::FogHistory, s_Data.PostProcessPasses.Fog->GetFogHistoryTextureID());
        }

        // ------------------------------------------------------------------
        // IBL resources
        // ------------------------------------------------------------------
        if (s_Data.GlobalIrradianceMapID != 0)
        {
            board.IrradianceMap = graph.ImportTexture(
                ResourceNames::IrradianceMap, s_Data.GlobalIrradianceMapID,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::TextureCube, ResourceNames::IrradianceMap));
        }
        if (s_Data.GlobalPrefilterMapID != 0)
        {
            board.PrefilterMap = graph.ImportTexture(
                ResourceNames::PrefilterMap, s_Data.GlobalPrefilterMapID,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::TextureCube, ResourceNames::PrefilterMap));
        }
        if (s_Data.GlobalBRDFLutMapID != 0)
        {
            board.BrdfLut = graph.ImportTexture(
                ResourceNames::BrdfLut, s_Data.GlobalBRDFLutMapID,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, ResourceNames::BrdfLut));
        }
    }
} // namespace OloEngine
