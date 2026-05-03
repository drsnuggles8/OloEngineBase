#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Passes/PostProcessRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderWarmup.h"
#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Core/Application.h"

#include <glad/gl.h>

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        void PrepareFullscreenColorPass()
        {
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            constexpr u32 colorAttachment = 0;
            RenderCommand::SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        }
    } // namespace

    std::vector<Ref<Shader>*> PostProcessRenderPass::GetAllShaderRefs()
    {
        return {
            &m_BloomThresholdShader,
            &m_BloomDownsampleShader,
            &m_BloomUpsampleShader,
            &m_BloomCompositeShader,
            &m_VignetteShader,
            &m_ChromaticAberrationShader,
            &m_ColorGradingShader,
            &m_ToneMapShader,
            &m_FXAAShader,
            &m_DOFShader,
            &m_MotionBlurShader,
            &m_TAAShader,
            &m_FogShader,
            &m_FogUpsampleShader,
            &m_SSAOApplyShader,
            &m_PrecipitationShader,
        };
    }
    PostProcessRenderPass::PostProcessRenderPass()
    {
        SetName("PostProcessPass");
        OLO_CORE_INFO("Creating PostProcessRenderPass.");
    }

    void PostProcessRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Create ping-pong framebuffers (RGBA16F, no depth)
        CreatePingPongFramebuffers(spec.Width, spec.Height);

        // Load all effect shaders via table-driven approach
        struct ShaderEntry
        {
            const char* path;
            Ref<Shader>* target;
        };

        static constexpr const char* s_ShaderPaths[] = {
            "assets/shaders/PostProcess_BloomThreshold.glsl",
            "assets/shaders/PostProcess_BloomDownsample.glsl",
            "assets/shaders/PostProcess_BloomUpsample.glsl",
            "assets/shaders/PostProcess_BloomComposite.glsl",
            "assets/shaders/PostProcess_Vignette.glsl",
            "assets/shaders/PostProcess_ChromaticAberration.glsl",
            "assets/shaders/PostProcess_ColorGrading.glsl",
            "assets/shaders/PostProcess_ToneMap.glsl",
            "assets/shaders/PostProcess_FXAA.glsl",
            "assets/shaders/PostProcess_DOF.glsl",
            "assets/shaders/PostProcess_MotionBlur.glsl",
            "assets/shaders/PostProcess_TAA.glsl",
            "assets/shaders/PostProcess_Fog.glsl",
            "assets/shaders/PostProcess_FogUpsample.glsl",
            "assets/shaders/PostProcess_SSAOApply.glsl",
            "assets/shaders/PostProcess_Precipitation.glsl",
        };

        auto shaderRefs = GetAllShaderRefs();
        OLO_CORE_ASSERT(shaderRefs.size() == std::size(s_ShaderPaths), "Shader path/ref count mismatch");

        const u32 totalPPShaders = static_cast<u32>(shaderRefs.size());
        Window& window = Application::Get().GetWindow();
        for (u32 ppIdx = 0; ppIdx < totalPPShaders; ++ppIdx)
        {
            *shaderRefs[ppIdx] = Shader::Create(s_ShaderPaths[ppIdx]);
            ShaderWarmup::RenderProgressFrame(static_cast<f32>(ppIdx + 1) / static_cast<f32>(totalPPShaders), &window, "post-process shaders", static_cast<i32>(ppIdx + 1), static_cast<i32>(totalPPShaders), 2);
        }
        m_PrecipitationScreenUBO = UniformBuffer::Create(PrecipitationScreenUBOData::GetSize(), ShaderBindingLayout::UBO_PRECIPITATION_SCREEN);
        m_TAAUBO = UniformBuffer::Create(TAAUBOData::GetSize(), ShaderBindingLayout::UBO_TAA);

        // Create bloom mip chain
        CreateBloomMipChain(spec.Width, spec.Height);

        // Create half-res fog framebuffers
        m_FogHalfWidth = std::max(1u, spec.Width / 2);
        m_FogHalfHeight = std::max(1u, spec.Height / 2);
        {
            FramebufferSpecification fogSpec;
            fogSpec.Width = m_FogHalfWidth;
            fogSpec.Height = m_FogHalfHeight;
            fogSpec.Samples = 1;
            fogSpec.Attachments = { FramebufferTextureFormat::RGBA16F };
            m_FogHalfResFB = Framebuffer::Create(fogSpec);
            m_FogHistoryFB = Framebuffer::Create(fogSpec);
        }

        // Resource-aware RDG: post-process consumes the accumulated HDR
        // AO-applied scene color (piped via SetInputFramebuffer from AOApplyPass)
        // plus graph-resolved depth / AO / velocity / shadow inputs.
        DeclareRead(ResourceNames::AOApplyColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::SceneDepth, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::AOBuffer, ResourceHandle::Kind::Texture2D);
        DeclareRead(ResourceNames::Velocity, ResourceHandle::Kind::Texture2D);
        DeclareRead(ResourceNames::ShadowMapCSM, ResourceHandle::Kind::Texture2DArray);
        DeclareWrite(ResourceNames::PostProcessColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("PostProcessRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void PostProcessRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void PostProcessRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 39 — self-resolving input framebuffer.  Prefer the most
        // downstream source in the AO-apply → SSS → Scene chain.
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            const auto inputHandle = board->AOApplyColor.IsValid() ? board->AOApplyColor
                                     : board->SSSColor.IsValid()   ? board->SSSColor
                                                                   : board->SceneColor;
            if (inputHandle.IsValid())
            {
                if (auto resolved = context.ResolveFramebuffer(inputHandle))
                    inputFramebuffer = resolved;
            }
        }
        if (!inputFramebuffer)
        {
            OLO_CORE_ERROR("PostProcessRenderPass::Execute: No input framebuffer!");
            return;
        }

        // Phase F slice 25 — all effects are handled by dedicated standalone
        // passes: PostProcessPass is a transparent node. SetupFrameBlackboard
        // imports PostProcessColor directly from the upstream source FB so
        // BloomPass (and other downstream passes) receive valid data without
        // going through this pass at all.
        if (IsAllHandledExternally())
            return;

        const u32 inputColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
        if (inputColorID == 0)
        {
            static u32 s_InvalidPostInputWarnings = 0;
            if (s_InvalidPostInputWarnings++ < 10)
            {
                const auto& inSpec = inputFramebuffer->GetSpecification();
                OLO_CORE_WARN("PostProcessRenderPass: input framebuffer color attachment 0 is invalid (id=0). Size={}x{}, attachmentCount={}",
                              inSpec.Width, inSpec.Height, inSpec.Attachments.Attachments.size());
            }
        }

        // Phase F slice 39 — self-resolving texture inputs.
        u32 sceneDepthTextureID = 0;
        u32 aoTextureID = 0;
        u32 shadowMapCSMTextureID = 0;
        u32 velocityTextureID = 0;
        if (const auto* board = context.GetBlackboard())
        {
            sceneDepthTextureID = context.ResolveTexture(board->SceneDepth);
            aoTextureID = context.ResolveTexture(board->AOBuffer);
            shadowMapCSMTextureID = context.ResolveTexture(board->ShadowMapCSM);
            velocityTextureID = context.ResolveTexture(board->Velocity);
        }

        if (sceneDepthTextureID == 0)
        {
            const u32 fallbackDepthID = inputFramebuffer->GetDepthAttachmentRendererID();
            if (fallbackDepthID != 0)
            {
                sceneDepthTextureID = fallbackDepthID;
            }

            static u32 s_MissingDepthWarnings = 0;
            if (s_MissingDepthWarnings++ < 10)
            {
                OLO_CORE_WARN("PostProcessRenderPass: resolved SceneDepth texture is invalid (id=0), fallback depth id={}",
                              sceneDepthTextureID);
            }
        }

        // SetData() updates the buffer object but does not restore the indexed
        // binding. IBL precompute also uses binding 7, so rebind the post-process
        // UBO before any fullscreen post shader reads PostProcessUBO.
        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        const bool ssaoApplyEnabled = m_Settings.ActiveAOTechnique == AOTechnique::SSAO && m_Settings.SSAOEnabled;
        const bool gtaoApplyEnabled = m_Settings.ActiveAOTechnique == AOTechnique::GTAO && m_Settings.GTAOEnabled;
        const bool aoApplyEnabled = (ssaoApplyEnabled || gtaoApplyEnabled) && aoTextureID != 0 && sceneDepthTextureID != 0;

        // If no effects are enabled, skip — GetTarget() returns the input framebuffer directly
        bool anyEffectEnabled = (!m_BloomHandledExternally && m_Settings.BloomEnabled) ||
                                (!m_VignetteHandledExternally && m_Settings.VignetteEnabled) ||
                                (!m_ChromAbHandledExternally && m_Settings.ChromaticAberrationEnabled) ||
                                (!m_FXAAHandledExternally && m_Settings.FXAAEnabled) ||
                                (!m_DOFHandledExternally && m_Settings.DOFEnabled) ||
                                (!m_MotionBlurHandledExternally && m_Settings.MotionBlurEnabled) ||
                                (!m_TAAHandledExternally && m_Settings.TAAEnabled) ||
                                (!m_ColorGradingHandledExternally && m_Settings.ColorGradingEnabled) ||
                                (!m_FogHandledExternally && m_FogEnabled) ||
                                (!m_PrecipitationHandledExternally && m_PrecipitationScreenEffectsEnabled && m_PrecipitationShader && m_PrecipitationScreenUBO) ||
                                (!m_AOApplyHandledExternally && aoApplyEnabled);

        // Always run tone mapping if we have the shader (it's the core of post-processing)
        if (!anyEffectEnabled && (!m_ToneMapShader || m_ToneMapHandledExternally))
        {
            // No effects and no tone mapping: keep the stable output updated
            // so downstream passes never observe a stale ping-pong target.
            ResolveToOutput(inputFramebuffer);
            return;
        }

        // Set up the ping-pong chain
        Ref<Framebuffer> currentSource = inputFramebuffer;
        bool writeToP = true;

        auto applyEffect = [&](const Ref<Shader>& shader)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            RunEffect(shader, currentSource, dest);
            currentSource = dest;
            writeToP = !writeToP;
        };

        // === Effect chain order ===
        // 0. AO Apply (modulate scene color by AO factor from SSAO or GTAO)
        // Phase F slice 24 — skipped when the standalone AOApplyRenderPass runs before this pass.
        if (!m_AOApplyHandledExternally && m_SSAOApplyShader && aoApplyEnabled)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            dest->Bind();
            PrepareFullscreenColorPass();
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_SSAOApplyShader->Bind();
            u32 srcColorID = currentSource->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcColorID);
            context.BindTexture(ShaderBindingLayout::TEX_SSAO, aoTextureID);

            // Bind full-res scene depth for bilateral upsampling.
            // Bind 0 when unavailable to avoid sampling stale texture state.
            context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 1. Bloom (threshold → downsample → upsample → composite)
        // Phase F slice 23 — skipped when the standalone BloomRenderPass runs.
        if (!m_BloomHandledExternally && m_Settings.BloomEnabled && m_BloomThresholdShader && !m_BloomMipChain.empty())
        {
            ExecuteBloom(currentSource);
            // Composite bloom onto current source
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;

            dest->Bind();
            PrepareFullscreenColorPass();
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_BloomCompositeShader->Bind();
            // Bind scene color at slot 0
            u32 sceneColorID = currentSource->GetColorAttachmentRendererID(0);
            context.BindTexture(0, sceneColorID);
            m_BloomCompositeShader->SetInt("u_SceneColor", 0);
            // Bind bloom result at slot 1
            u32 bloomColorID = m_BloomMipChain[0]->GetColorAttachmentRendererID(0);
            context.BindTexture(1, bloomColorID);
            m_BloomCompositeShader->SetInt("u_BloomColor", 1);

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 2. DOF
        // Phase F slice 22 — skipped when the standalone DOFRenderPass runs.
        if (!m_DOFHandledExternally && m_Settings.DOFEnabled && m_DOFShader && sceneDepthTextureID != 0)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            dest->Bind();
            PrepareFullscreenColorPass();
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_DOFShader->Bind();
            u32 srcColorID = currentSource->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcColorID);

            // Bind scene depth at slot 19
            context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);

            // UBO already has CameraNear/CameraFar from EndScene upload

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 3. Motion Blur
        // Phase F slice 21 — skipped when the standalone MotionBlurRenderPass runs.
        if (!m_MotionBlurHandledExternally && m_Settings.MotionBlurEnabled && m_MotionBlurShader && sceneDepthTextureID != 0)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            dest->Bind();
            PrepareFullscreenColorPass();
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_MotionBlurShader->Bind();
            u32 srcColorID = currentSource->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcColorID);
            m_MotionBlurShader->SetInt("u_Texture", 0);

            context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);
            m_MotionBlurShader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 3.1. TAA — velocity-reprojected temporal accumulation. Runs AFTER
        // motion blur so the accumulated history already reflects any blurred
        // trails; for best AA quality (sub-pixel edges) projection jitter
        // would need to be injected into CameraMatrices (see deferred-renderer
        // docs, future work). Without jitter, TAA still smooths temporal
        // aliasing and reduces crawl during motion.
        // Phase F slice 19 — skipped when the standalone TAARenderPass runs.
        if (!m_TAAHandledExternally && m_Settings.TAAEnabled && m_TAAShader && m_TAAHistoryFB && sceneDepthTextureID != 0)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            dest->Bind();
            PrepareFullscreenColorPass();
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_TAAShader->Bind();

            // slot 0: current
            u32 srcColorID = currentSource->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcColorID);
            m_TAAShader->SetInt("u_Current", 0);

            // slot 1: history (fall back to current on first frame / resize)
            u32 historyID = m_TAAHistoryValid
                                ? m_TAAHistoryFB->GetColorAttachmentRendererID(0)
                                : srcColorID;
            context.BindTexture(1, historyID);
            m_TAAShader->SetInt("u_History", 1);

            // slot 2: velocity (G-Buffer RT3) — zero-bound forces camera-only
            // reprojection path in the shader.
            context.BindTexture(2, velocityTextureID);
            m_TAAShader->SetInt("u_Velocity", 2);

            // slot 19: scene depth for camera-only reconstruction
            context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);
            m_TAAShader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);

            // Upload TAA UBO (binding 32)
            if (m_TAAUBO)
            {
                TAAUBOData taaData;
                taaData.FeedbackSharpnessHasVelocity = glm::vec4(
                    m_Settings.TAAFeedback,
                    m_Settings.TAASharpness,
                    velocityTextureID != 0 ? 1.0f : 0.0f,
                    0.0f);
                taaData.TexelSize = glm::vec4(
                    1.0f / static_cast<f32>(m_FramebufferSpec.Width),
                    1.0f / static_cast<f32>(m_FramebufferSpec.Height),
                    0.0f, 0.0f);
                m_TAAUBO->SetData(&taaData, TAAUBOData::GetSize());
                m_TAAUBO->Bind();
            }

            DrawFullscreenTriangle();
            dest->Unbind();

            // Snapshot resolved output into the persistent history FB for the
            // next frame. A direct texture-to-texture blit is cheaper than an
            // extra fullscreen shader pass.
            glBlitNamedFramebuffer(dest->GetRendererID(),
                                   m_TAAHistoryFB->GetRendererID(),
                                   0, 0, static_cast<GLint>(m_FramebufferSpec.Width),
                                   static_cast<GLint>(m_FramebufferSpec.Height),
                                   0, 0, static_cast<GLint>(m_FramebufferSpec.Width),
                                   static_cast<GLint>(m_FramebufferSpec.Height),
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
            m_TAAHistoryValid = true;

            currentSource = dest;
            writeToP = !writeToP;
        }
        else if (!m_Settings.TAAEnabled)
        {
            // Invalidate history when TAA is toggled off so we don't bleed
            // stale frames back in on re-enable.
            m_TAAHistoryValid = false;
        }
        else
        {
            // TAA requested but a required resource (shader / history FB /
            // scene depth) is missing this frame — the pass did not run, so
            // treat the history as stale. Without this, a subsequent frame
            // that does have the resources would blend in arbitrarily old
            // content (post-resize ghosting, post-hot-reload smears).
            m_TAAHistoryValid = false;
        }

        // 3.25. Precipitation screen-space effects (streaks + lens impacts)
        // Phase F slice 20 — skipped when the standalone PrecipitationRenderPass runs.
        if (!m_PrecipitationHandledExternally && m_PrecipitationScreenEffectsEnabled && m_PrecipitationShader && m_PrecipitationScreenUBO)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            dest->Bind();
            PrepareFullscreenColorPass();
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_PrecipitationShader->Bind();

            // Bind scene color at slot 0
            u32 srcColorID = currentSource->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcColorID);
            m_PrecipitationShader->SetInt("u_Texture", 0);

            // Upload precipitation screen UBO
            {
                PrecipitationScreenUBOData uboData;
                uboData.StreakParams = ScreenSpacePrecipitation::GetStreakParams();
                auto lensData = ScreenSpacePrecipitation::GetLensImpactGPUData();
                for (u32 i = 0; i < ScreenSpacePrecipitation::MAX_LENS_IMPACTS; ++i)
                {
                    uboData.LensImpacts[i].PositionAndSize = lensData[i].PositionAndSize;
                    uboData.LensImpacts[i].TimeParams = lensData[i].TimeParams;
                }
                m_PrecipitationScreenUBO->SetData(&uboData, PrecipitationScreenUBOData::GetSize());
                m_PrecipitationScreenUBO->Bind();
            }

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 3.5. Volumetric Fog — half-res ray-march + temporal reprojection + bilateral upsample
        // Phase F slice 18 — skipped when the standalone FogRenderPass runs.
        if (!m_FogHandledExternally && m_FogEnabled && m_FogShader && m_FogUpsampleShader && sceneDepthTextureID != 0 && m_FogHalfResFB)
        {
            // Pass A: Ray-march at half resolution into m_FogHalfResFB
            //   Output: RGBA16F — RGB = accumulated inscatter, A = transmittance
            m_FogHalfResFB->Bind();
            context.SetViewport(0, 0, m_FogHalfWidth, m_FogHalfHeight);
            PrepareFullscreenColorPass();
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_FogShader->Bind();

            // Bind full-res depth (reads at half-res UV)
            context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);

            // Bind temporal history for reprojection
            if (m_FogHistoryFB)
            {
                u32 historyID = m_FogHistoryFB->GetColorAttachmentRendererID(0);
                context.BindTexture(3, historyID);
            }

            // Bind CSM shadow map for volumetric light shafts
            if (shadowMapCSMTextureID != 0)
            {
                context.BindTexture(ShaderBindingLayout::TEX_SHADOW, shadowMapCSMTextureID);
            }

            DrawFullscreenTriangle();
            m_FogHalfResFB->Unbind();

            // Copy current result to history for next frame's temporal reprojection
            // (swap references — cheap, no GPU copy needed)
            std::swap(m_FogHistoryFB, m_FogHalfResFB);
            // After swap: m_FogHistoryFB has THIS frame's result (for next frame),
            //             m_FogHalfResFB is the OLD history (will be overwritten next frame).
            // We read from m_FogHistoryFB since it has the current frame's output.

            // Pass B: Bilateral upsample half-res fog + composite onto full-res scene
            context.SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            dest->Bind();
            PrepareFullscreenColorPass();
            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_FogUpsampleShader->Bind();

            // Scene color (current source)
            u32 srcColorID = currentSource->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcColorID);
            m_FogUpsampleShader->SetInt("u_SceneColor", 0);

            // Half-res fog result (from the swap, this is now m_FogHistoryFB)
            u32 fogID = m_FogHistoryFB->GetColorAttachmentRendererID(0);
            context.BindTexture(1, fogID);
            m_FogUpsampleShader->SetInt("u_FogTexture", 1);

            // Full-res depth for bilateral edge detection
            context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);
            m_FogUpsampleShader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 4. Chromatic Aberration
        // Phase F slice 17 — skipped when the standalone ChromaticAberrationRenderPass runs.
        if (!m_ChromAbHandledExternally && m_Settings.ChromaticAberrationEnabled && m_ChromaticAberrationShader)
        {
            applyEffect(m_ChromaticAberrationShader);
        }

        // 5. Color Grading
        // Phase F slice 17 — skipped when the standalone ColorGradingRenderPass runs.
        if (!m_ColorGradingHandledExternally && m_Settings.ColorGradingEnabled && m_ColorGradingShader)
        {
            applyEffect(m_ColorGradingShader);
        }

        // 6. Tone Mapping (HDR → LDR)
        // Phase F slice 17 — skipped when the standalone ToneMapRenderPass runs.
        if (!m_ToneMapHandledExternally && m_ToneMapShader)
        {
            applyEffect(m_ToneMapShader);
        }

        // 7. Vignette (operates on LDR)
        // Phase F slice 17 — skipped when the standalone VignetteRenderPass runs.
        if (!m_VignetteHandledExternally && m_Settings.VignetteEnabled && m_VignetteShader)
        {
            applyEffect(m_VignetteShader);
        }

        // 8. FXAA (must be last spatial filter, operates on LDR)
        // Phase F slice 16 — when `m_FXAAHandledExternally` is set,
        // `FXAARenderPass` runs after this pass and reads PostProcessColor.
        if (!m_FXAAHandledExternally && m_Settings.FXAAEnabled && m_FXAAShader)
        {
            applyEffect(m_FXAAShader);
        }

        ResolveToOutput(currentSource);
    }

    void PostProcessRenderPass::RunEffect(const Ref<Shader>& shader, Ref<Framebuffer> srcFB, Ref<Framebuffer> dstFB)
    {
        OLO_PROFILE_FUNCTION();

        dstFB->Bind();
        PrepareFullscreenColorPass();
        RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        RenderCommand::Clear();

        shader->Bind();

        // Bind source framebuffer color attachment as texture at slot 0
        u32 srcColorID = srcFB->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, srcColorID);
        shader->SetInt("u_Texture", 0);

        DrawFullscreenTriangle();

        RenderCommand::SetDepthMask(true);
        dstFB->Unbind();
    }

    void PostProcessRenderPass::ResolveToOutput(const Ref<Framebuffer>& sourceFB)
    {
        if (!sourceFB || !m_OutputFB || sourceFB == m_OutputFB)
        {
            return;
        }

        const auto& srcSpec = sourceFB->GetSpecification();
        const auto& dstSpec = m_OutputFB->GetSpecification();
        glNamedFramebufferReadBuffer(sourceFB->GetRendererID(), GL_COLOR_ATTACHMENT0);
        glNamedFramebufferDrawBuffer(m_OutputFB->GetRendererID(), GL_COLOR_ATTACHMENT0);
        glBlitNamedFramebuffer(sourceFB->GetRendererID(), m_OutputFB->GetRendererID(),
                               0, 0, static_cast<GLint>(srcSpec.Width), static_cast<GLint>(srcSpec.Height),
                               0, 0, static_cast<GLint>(dstSpec.Width), static_cast<GLint>(dstSpec.Height),
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    void PostProcessRenderPass::DrawFullscreenTriangle()
    {
        auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);
    }

    Ref<Framebuffer> PostProcessRenderPass::GetTarget() const
    {
        if (!m_OutputFB)
        {
            return nullptr;
        }

        return m_OutputFB;
    }

    void PostProcessRenderPass::CreatePingPongFramebuffers(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        FramebufferSpecification pingPongSpec;
        pingPongSpec.Width = width;
        pingPongSpec.Height = height;
        pingPongSpec.Samples = 1;
        pingPongSpec.Attachments = {
            FramebufferTextureFormat::RGBA16F
        };

        m_PingFB = Framebuffer::Create(pingPongSpec);
        m_PongFB = Framebuffer::Create(pingPongSpec);
        m_OutputFB = Framebuffer::Create(pingPongSpec);

        // TAA history: same spec, separate persistent FB. Allocation mirrors
        // ping-pong so resize paths share a single code path.
        m_TAAHistoryFB = Framebuffer::Create(pingPongSpec);
        m_TAAHistoryValid = false;

        OLO_CORE_INFO("PostProcessRenderPass: Created ping-pong framebuffers {}x{}", width, height);
    }

    void PostProcessRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("PostProcessRenderPass::SetupFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (!m_PingFB)
        {
            CreatePingPongFramebuffers(width, height);
        }
        else
        {
            m_PingFB->Resize(width, height);
            m_PongFB->Resize(width, height);
            if (m_OutputFB)
            {
                m_OutputFB->Resize(width, height);
            }
            if (m_TAAHistoryFB)
            {
                // History must track viewport size; invalidate on resize so
                // the next TAA invocation reprojects fresh instead of reading
                // texels that no longer correspond to the current geometry.
                m_TAAHistoryFB->Resize(width, height);
                m_TAAHistoryValid = false;
            }
        }

        // Setup or resize half-res fog framebuffers
        m_FogHalfWidth = std::max(1u, width / 2);
        m_FogHalfHeight = std::max(1u, height / 2);
        if (!m_FogHalfResFB)
        {
            FramebufferSpecification fogSpec;
            fogSpec.Width = m_FogHalfWidth;
            fogSpec.Height = m_FogHalfHeight;
            fogSpec.Samples = 1;
            fogSpec.Attachments = { FramebufferTextureFormat::RGBA16F };
            m_FogHalfResFB = Framebuffer::Create(fogSpec);
            m_FogHistoryFB = Framebuffer::Create(fogSpec);
        }
        else
        {
            m_FogHalfResFB->Resize(m_FogHalfWidth, m_FogHalfHeight);
            m_FogHistoryFB->Resize(m_FogHalfWidth, m_FogHalfHeight);
        }
    }

    void PostProcessRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("PostProcessRenderPass::ResizeFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (m_PingFB)
        {
            m_PingFB->Resize(width, height);
        }
        if (m_PongFB)
        {
            m_PongFB->Resize(width, height);
        }
        if (m_OutputFB)
        {
            m_OutputFB->Resize(width, height);
        }
        if (m_TAAHistoryFB)
        {
            m_TAAHistoryFB->Resize(width, height);
            m_TAAHistoryValid = false; // invalidate after resize
        }

        // Resize half-res fog framebuffers
        m_FogHalfWidth = std::max(1u, width / 2);
        m_FogHalfHeight = std::max(1u, height / 2);
        if (m_FogHalfResFB)
        {
            m_FogHalfResFB->Resize(m_FogHalfWidth, m_FogHalfHeight);
        }
        if (m_FogHistoryFB)
        {
            m_FogHistoryFB->Resize(m_FogHalfWidth, m_FogHalfHeight);
        }

        // Resize bloom mip chain
        CreateBloomMipChain(width, height);

        OLO_CORE_INFO("PostProcessRenderPass: Resized to {}x{}", width, height);
    }

    void PostProcessRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            CreatePingPongFramebuffers(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            CreateBloomMipChain(m_FramebufferSpec.Width, m_FramebufferSpec.Height);

            m_FogHalfWidth = std::max(1u, m_FramebufferSpec.Width / 2);
            m_FogHalfHeight = std::max(1u, m_FramebufferSpec.Height / 2);
            FramebufferSpecification fogSpec;
            fogSpec.Width = m_FogHalfWidth;
            fogSpec.Height = m_FogHalfHeight;
            fogSpec.Samples = 1;
            fogSpec.Attachments = { FramebufferTextureFormat::RGBA16F };
            m_FogHalfResFB = Framebuffer::Create(fogSpec);
            m_FogHistoryFB = Framebuffer::Create(fogSpec);

            OLO_CORE_INFO("PostProcessRenderPass reset with dimensions {}x{}",
                          m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        }
    }

    void PostProcessRenderPass::ReloadShader(const std::string& name)
    {
        OLO_PROFILE_FUNCTION();

        for (auto* shaderRef : GetAllShaderRefs())
        {
            if (*shaderRef && (*shaderRef)->GetName() == name)
            {
                (*shaderRef)->Reload();
                return;
            }
        }

        OLO_CORE_WARN("PostProcessRenderPass::ReloadShader: no shader matched name '{}'", name);
    }

    void PostProcessRenderPass::CreateBloomMipChain(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_BloomMipChain.clear();

        u32 mipWidth = width / 2;
        u32 mipHeight = height / 2;

        for (u32 i = 0; i < MAX_BLOOM_MIPS; ++i)
        {
            if (mipWidth < 2 || mipHeight < 2)
            {
                break;
            }

            FramebufferSpecification mipSpec;
            mipSpec.Width = mipWidth;
            mipSpec.Height = mipHeight;
            mipSpec.Samples = 1;
            mipSpec.Attachments = { FramebufferTextureFormat::RGBA16F };

            m_BloomMipChain.push_back(Framebuffer::Create(mipSpec));

            mipWidth /= 2;
            mipHeight /= 2;
        }

        OLO_CORE_INFO("PostProcessRenderPass: Created bloom mip chain with {} levels", m_BloomMipChain.size());
    }

    void PostProcessRenderPass::ExecuteBloom(Ref<Framebuffer> sceneColorFB)
    {
        OLO_PROFILE_FUNCTION();

        if (m_BloomMipChain.empty())
        {
            return;
        }

        // Step 1: Threshold extract — scene HDR → bloom mip 0
        {
            m_BloomMipChain[0]->Bind();
            PrepareFullscreenColorPass();
            const auto& spec = m_BloomMipChain[0]->GetSpecification();
            RenderCommand::SetViewport(0, 0, spec.Width, spec.Height);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();

            m_BloomThresholdShader->Bind();
            u32 srcID = sceneColorFB->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, srcID);
            m_BloomThresholdShader->SetInt("u_Texture", 0);

            DrawFullscreenTriangle();
            m_BloomMipChain[0]->Unbind();
        }

        // Step 2: Progressive downsample
        for (size_t i = 1; i < m_BloomMipChain.size(); ++i)
        {
            auto srcMip = m_BloomMipChain[i - 1];
            auto dstMip = m_BloomMipChain[i];
            const auto& srcSpec = srcMip->GetSpecification();
            const auto& dstSpec = dstMip->GetSpecification();

            dstMip->Bind();
            PrepareFullscreenColorPass();
            RenderCommand::SetViewport(0, 0, dstSpec.Width, dstSpec.Height);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();

            m_BloomDownsampleShader->Bind();
            u32 srcID = srcMip->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, srcID);

            // Update texel size in UBO for this mip level
            if (m_GPUData && m_PostProcessUBO)
            {
                m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(srcSpec.Width);
                m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(srcSpec.Height);
                m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
            }

            DrawFullscreenTriangle();
            dstMip->Unbind();
        }

        // Step 3: Progressive upsample (accumulate back up the chain)
        for (int i = static_cast<int>(m_BloomMipChain.size()) - 2; i >= 0; --i)
        {
            auto srcMip = m_BloomMipChain[static_cast<size_t>(i) + 1]; // smaller mip
            auto dstMip = m_BloomMipChain[static_cast<size_t>(i)];     // larger mip (accumulate into)
            const auto& srcSpec = srcMip->GetSpecification();
            const auto& dstSpec = dstMip->GetSpecification();

            dstMip->Bind();
            PrepareFullscreenColorPass();
            RenderCommand::SetViewport(0, 0, dstSpec.Width, dstSpec.Height);
            // Enable additive blending AFTER Bind() to be immune to Bind() resetting blend state
            RenderCommand::SetBlendState(true);
            RenderCommand::SetBlendFunc(GL_ONE, GL_ONE);
            // Don't clear — we're ADDING to existing content

            m_BloomUpsampleShader->Bind();
            u32 srcID = srcMip->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, srcID);

            // Update texel size in UBO for this mip level
            if (m_GPUData && m_PostProcessUBO)
            {
                m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(srcSpec.Width);
                m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(srcSpec.Height);
                m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
            }

            DrawFullscreenTriangle();
            dstMip->Unbind();
        }

        RenderCommand::SetBlendState(false);
        RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        RenderCommand::SetDepthMask(true);

        // Restore TexelSize to full resolution so subsequent effects (FXAA etc.) don't read stale mip values
        if (m_GPUData && m_PostProcessUBO)
        {
            m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(m_FramebufferSpec.Width);
            m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(m_FramebufferSpec.Height);
            m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
        }

        // Restore full viewport for subsequent passes
        RenderCommand::SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }
} // namespace OloEngine
