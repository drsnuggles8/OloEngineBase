#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SkinDiffusionPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/SkinProfileTable.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <array>
#include <span>

namespace OloEngine
{
    namespace
    {
        // Scene colour only. The scene framebuffer also carries entity IDs, view
        // normals, velocity and the skin-diffuse attachment this pass READS —
        // blending into any of those would be a self-referencing write.
        constexpr std::array<u32, 1> kAttachment0Only = { 0u };
        // The scratch target is single-attachment, but the graph hands back a
        // framebuffer object and the draw-attachment state is global, so it is
        // set explicitly rather than inherited from whatever ran before.
        constexpr std::array<u32, 1> kScratchAttachment0 = { 0u };
    } // namespace

    SkinDiffusionPass::SkinDiffusionPass()
    {
        SetName("SkinDiffusionPass");
    }

    void SkinDiffusionPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        m_SkinDiffuseTexture = {};
        m_SceneDepthTexture = {};
        m_ScratchFramebuffer = {};
        m_ScratchTexture = {};
        m_SceneColorFramebuffer = {};

        if (!m_Settings.Enabled)
            return;

        // Every input is required. A missing one is not a reason to run a
        // degraded version — a diffusion with no depth cannot tell a cheek from
        // the wall behind it — so the pass simply does not claim its resources
        // and the graph culls it.
        if (!blackboard.Scene.SkinDiffuse.IsValid() || !blackboard.Scene.SceneDepthAttachment.IsValid() ||
            !blackboard.Scene.SceneColor.IsValid() || !blackboard.Post.SkinDiffusionScratch.IsValid())
            return;

        m_SkinDiffuseTexture = blackboard.Scene.SkinDiffuse;
        m_SceneDepthTexture = blackboard.Scene.SceneDepthAttachment;
        [[maybe_unused]] const auto diffuseRead = builder.Read(m_SkinDiffuseTexture, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto depthRead = builder.Read(m_SceneDepthTexture, RGReadUsage::ShaderSample);

        constexpr std::string_view versionTag = "SkinDiffusionPass";
        m_ScratchFramebuffer = builder.WriteNewVersion(blackboard.Post.SkinDiffusionScratch,
                                                       RGWriteUsage::RenderTarget, versionTag);
        if (!m_ScratchFramebuffer.IsValid())
            return;
        m_ScratchTexture = builder.CreateFramebufferAttachmentView(
            std::string(ResourceNames::SkinDiffusionScratchTexture) + "@" + std::string(versionTag),
            m_ScratchFramebuffer, 0u);

        // Scene colour is written IN PLACE, by an additive blend. The graph is
        // told about the write so ordering and barriers are right; it is not a
        // new version, because the pass does not replace the image, it adds to
        // it — and a new version would make every downstream consumer rebind.
        m_SceneColorFramebuffer = blackboard.Scene.SceneColor;
        SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
        builder.Write(blackboard.Scene.SceneColor, RGWriteUsage::RenderTarget);
        // A DECLARED same-framebuffer read/write, not an accident. The two
        // resources this pass reads off the scene framebuffer -- the skin-diffuse
        // hand-off (attachment 4) and depth -- live in the same framebuffer as
        // the colour attachment it blends into, so the hazard validator sees a
        // feedback loop unless it is told the overlap is intentional. It IS
        // intentional and it is safe: the draw writes attachment 0 ONLY (the
        // draw-attachment list in Execute is exactly {0}) and samples 4 and
        // depth, which are disjoint subresources that nothing in this pass
        // writes.
        //
        // WriteNewVersion -- the construct the comment on AllowSamePassReadWrite
        // points read-modify-write passes at -- is the wrong tool here for the
        // reason above: renaming scene colour would make every downstream
        // consumer rebind for a pass whose whole design is that it adds to the
        // image in place and leaves the handle alone.
        builder.AllowSamePassReadWrite(blackboard.Scene.SceneColor);
    }

    void SkinDiffusionPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_Shader = Shader::Create("assets/shaders/SkinDiffusion.glsl");
        m_UBO = UniformBuffer::Create(SkinDiffusionUBOData::GetSize(), ShaderBindingLayout::UBO_SSS);
        m_CachedValid.fill(false);
        m_CachedQuality = SkinDiffusionQuality::Count;
    }

    void SkinDiffusionPass::UpdateKernels()
    {
        OLO_PROFILE_FUNCTION();

        const SkinProfileTable& profiles = Renderer3D::GetSkinProfileTable();
        const SkinDiffusionQuality quality =
            IsValidSkinDiffusionQuality(static_cast<i32>(m_Settings.Quality)) ? m_Settings.Quality
                                                                              : SkinDiffusionQuality::Medium;
        // A tier change invalidates every cached kernel: the tap COUNT changes,
        // so the weights are a different table, not a rescaling of the old one.
        const bool qualityChanged = (quality != m_CachedQuality);
        if (qualityChanged)
        {
            m_CachedValid.fill(false);
            m_CachedQuality = quality;
        }

        const f32 radiusScale = std::isfinite(m_Settings.RadiusScale) ? std::max(m_Settings.RadiusScale, 0.0f) : 1.0f;
        const f32 depthScale =
            std::isfinite(m_Settings.DepthRejectionScale) ? std::max(m_Settings.DepthRejectionScale, 0.0f) : 2.0f;

        m_GPUData.PassParams.x = static_cast<f32>(GetSkinDiffusionTapCount(quality));
        m_GPUData.ProjectionParams = glm::vec4(m_ProjectionScaleY, m_DepthLinearizeA, m_DepthLinearizeB, depthScale);

        m_ActiveProfileCount = 0;
        for (u32 slot = 0; slot < kMaxSkinProfileSlots; ++slot)
        {
            const SkinProfileParameters parameters = profiles.GetParametersForSlot(slot);
            const auto index = static_cast<sizet>(slot);
            if (!m_CachedValid[index] || !(m_CachedParameters[index] == parameters))
            {
                m_CachedParameters[index] = parameters;
                m_CachedKernels[index] = BuildSkinDiffusionKernel(parameters, quality);
                m_CachedValid[index] = true;
            }

            const SkinDiffusionKernel& kernel = m_CachedKernels[index];
            // An identity kernel is what a profile authored against transport
            // version 0 produces (BuildSkinDiffusionKernel's own version branch),
            // and it uploads a ZERO support radius — which the shader reads as
            // "this slot does not diffuse" and skips before it fetches anything.
            const bool active = !kernel.IsIdentity() && kernel.SupportRadiusMM > 0.0f;
            m_GPUData.SlotParams[index] = glm::vec4(active ? kernel.SupportRadiusMM : 0.0f, radiusScale, 0.0f, 0.0f);
            if (active)
                ++m_ActiveProfileCount;

            const sizet base = index * static_cast<sizet>(kMaxSkinDiffusionTaps);
            for (u32 tap = 0; tap < kMaxSkinDiffusionTaps; ++tap)
            {
                // Taps past the tier's count are never read (the shader loops to
                // the uploaded tap count), but they are cleared rather than left
                // holding a wider tier's weights: a stale weight behind a live
                // loop bound is the shape of a bug that appears only after a
                // quality change.
                m_GPUData.Taps[base + tap] =
                    (tap < kernel.TapCount) ? kernel.Taps[tap] : glm::vec4(0.0f);
            }
        }
    }

    void SkinDiffusionPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Settings.Enabled || !IsReadyForExecution())
        {
            m_Target = nullptr;
            return;
        }
        if (!m_SkinDiffuseTexture.IsValid() || !m_SceneDepthTexture.IsValid() || !m_ScratchFramebuffer.IsValid())
        {
            m_Target = nullptr;
            return;
        }

        const RHI::ResourceHandle skinDiffuseID = context.ResolveTextureHandle(m_SkinDiffuseTexture);
        const RHI::ResourceHandle depthID = context.ResolveTextureHandle(m_SceneDepthTexture);
        const RHI::ResourceHandle scratchTexID = context.ResolveTextureHandle(m_ScratchTexture);
        Ref<Framebuffer> scratchFB = context.ResolveFramebuffer(m_ScratchFramebuffer);
        Ref<Framebuffer> sceneFB = m_SceneColorFramebuffer.IsValid() ? context.ResolveFramebuffer(m_SceneColorFramebuffer)
                                                                     : Ref<Framebuffer>{};
        if (!skinDiffuseID.IsValid() || !depthID.IsValid() || !scratchTexID.IsValid() || !scratchFB || !sceneFB)
        {
            m_Target = nullptr;
            return;
        }

        UpdateKernels();
        if (m_ActiveProfileCount == 0)
        {
            // No authored profile in this frame asks to be diffused. Both draws
            // would be exact no-ops, so skip them rather than pay two fullscreen
            // passes to add zero — this is the normal state of every scene
            // without skin in it.
            m_Target = nullptr;
            return;
        }

        const auto& sceneSpec = sceneFB->GetSpecification();
        const u32 width = sceneSpec.Width;
        const u32 height = sceneSpec.Height;
        if (width == 0 || height == 0)
        {
            m_Target = nullptr;
            return;
        }

        // NO GLStateGuard HERE, DELIBERATELY. The guard's Restore policy logs every
        // field a pass leaves changed, and it is the right tool for a rare path
        // (DeferredLightingPass uses one around its debug overlay). On a pass that
        // runs EVERY FRAME it emits a dozen trace lines per frame -- 35 000 in one
        // editor session, measured -- which drowns the log the renderer's other
        // diagnostics live in. The state this pass changes is restored explicitly
        // at the end instead, which is both cheaper and the convention
        // PrepareFullscreenPass already sets for a fullscreen draw.
        auto va = MeshPrimitives::GetFullscreenTriangle();

        context.SetViewport(0, 0, width, height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        // Culling OFF for the same reason every other fullscreen pass does it
        // (issue #1002): the fullscreen triangle writes NDC directly, so its
        // apparent winding is opposite between GL and Vulkan and no single
        // front-face setting can serve both.
        context.SetCulling(false);

        m_Shader->Bind();

        // ---- Pass 0: horizontal, into the scratch target --------------------
        m_GPUData.PassParams.y = 0.0f;
        m_GPUData.PassParams.z = static_cast<f32>(width);
        m_GPUData.PassParams.w = static_cast<f32>(height);
        m_UBO->SetData(&m_GPUData, SkinDiffusionUBOData::GetSize());
        m_UBO->Bind();

        scratchFB->Bind();
        RenderCommand::SetFramebufferDrawAttachments(scratchFB->GetRHIHandle(), kScratchAttachment0);
        context.SetBlendState(false);
        // Slot 1 (the "original" input) is bound to the same texture on this
        // pass. The shader does not read it here, and binding something real
        // costs less than a shader variant that declares one sampler fewer.
        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE, skinDiffuseID,
                                         RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_SPECULAR, skinDiffuseID,
                                         RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID,
                                         RHI::HeapSlotLifetime::FrameTransient);
        va->Bind();
        HeapBinding::FlushOffsets();
        RenderCommand::DrawIndexed(va);

        // ---- Pass 1: vertical, ADDED into scene colour ----------------------
        m_GPUData.PassParams.y = 1.0f;
        m_UBO->SetData(&m_GPUData, SkinDiffusionUBOData::GetSize());
        m_UBO->Bind();

        sceneFB->Bind();
        RenderCommand::SetFramebufferDrawAttachments(sceneFB->GetRHIHandle(), kAttachment0Only);
        context.SetBlendState(true);
        // ONE/ONE on colour, ZERO/ONE on alpha. The alpha half is not
        // decoration: scene colour's alpha is snow's transient SSS mask
        // (SnowCommon.glsl), and a plain additive blend would add this pass's
        // zero alpha to it -- harmless -- but a future non-zero would not be.
        // Saying "leave alpha exactly as it is" is the contract, so it is what
        // is written.
        RenderCommand::SetBlendFuncSeparate(RHI::BlendFactor::One, RHI::BlendFactor::One,
                                            RHI::BlendFactor::Zero, RHI::BlendFactor::One);
        RenderCommand::SetBlendEquation(RHI::BlendOp::Add);
        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE, scratchTexID,
                                         RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_SPECULAR, skinDiffuseID,
                                         RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID,
                                         RHI::HeapSlotLifetime::FrameTransient);
        va->Bind();
        HeapBinding::FlushOffsets();
        RenderCommand::DrawIndexed(va);

        // Restore the scene framebuffer's full draw-buffer set. The passes after
        // this one bind it expecting every attachment to be available — the same
        // contract DeferredLightingPass restores to after its own lighting draw.
        u32 colorCount = 0;
        for (const auto& attachment : sceneSpec.Attachments.Attachments)
        {
            const bool isDepth = (attachment.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  attachment.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && attachment.TextureFormat != FramebufferTextureFormat::None)
                ++colorCount;
        }
        RenderCommand::RestoreAllFramebufferDrawAttachments(sceneFB->GetRHIHandle(), colorCount);

        // RESTORE WHAT THIS PASS UNIQUELY CHANGED. Every fullscreen pass leaves the
        // depth test, culling and its own bindings behind -- that is the engine's
        // convention and the next pass sets its own. The BLEND FUNCTION is not part
        // of that convention: this is the only pass that changes it, so leaving
        // ONE/ONE + ZERO/ONE standing would silently re-colour the first later pass
        // that enables blending without setting its own factors.
        RenderCommand::SetBlendFuncSeparate(RHI::BlendFactor::One, RHI::BlendFactor::Zero,
                                            RHI::BlendFactor::One, RHI::BlendFactor::Zero);
        RenderCommand::SetBlendEquation(RHI::BlendOp::Add);
        context.SetBlendState(false);
        // The depth mask, for the same reason PrepareFullscreenPass restores it:
        // a pass that leaves depth writes off makes the NEXT geometry pass render
        // without depth, which looks like a sorting bug several passes away.
        context.SetDepthMask(true);

        m_Target = sceneFB;
    }

    Ref<Framebuffer> SkinDiffusionPass::GetTarget() const
    {
        return m_Settings.Enabled ? m_Target : nullptr;
    }

    void SkinDiffusionPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SkinDiffusionPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SkinDiffusionPass::OnReset()
    {
        m_Target = nullptr;
        m_SkinDiffuseTexture = {};
        m_SceneDepthTexture = {};
        m_ScratchFramebuffer = {};
        m_ScratchTexture = {};
        m_SceneColorFramebuffer = {};
        // The kernel cache is NOT cleared: it is keyed on the authored
        // parameters, which a reset does not change, and rebuilding seven
        // kernels on a scene load is work nobody asked for. SkinProfileTable's
        // own Reset() is what invalidates the slot-to-profile mapping, and a
        // slot whose profile changed fails the parameter comparison next frame.
        m_ActiveProfileCount = 0;
    }
} // namespace OloEngine
