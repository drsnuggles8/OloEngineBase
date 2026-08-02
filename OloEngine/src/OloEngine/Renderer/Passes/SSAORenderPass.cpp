#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <array>
#include <random>

namespace OloEngine
{
    SSAORenderPass::SSAORenderPass()
    {
        SetName("SSAOPass");
    }

    void SSAORenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SelectedAOOutputTexture = {};
        m_SelectedBlurFramebuffer = {};

        if (!m_Settings.SSAOEnabled || m_Settings.ActiveAOTechnique != AOTechnique::SSAO)
            return;

        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        }
        if (blackboard.Scene.SceneNormals.IsValid())
        {
            m_SelectedSceneNormalsTexture = blackboard.Scene.SceneNormals;
            [[maybe_unused]] const auto sceneNormalsRead = builder.Read(blackboard.Scene.SceneNormals, RGReadUsage::ShaderSample);
        }
        if (blackboard.AO.AOBuffer.IsValid())
        {
            m_SelectedAOOutputTexture = blackboard.AO.AOBuffer;
            builder.Write(blackboard.AO.AOBuffer, RGWriteUsage::RenderTarget);
        }

        if (blackboard.Scratch.SSAORaw.IsValid())
        {
            SetPrimaryOutputFramebufferHandle(blackboard.Scratch.SSAORaw);
            // Intra-pass write-then-sample: Pass 1 renders raw SSAO into this
            // framebuffer; Pass 2 (bilateral blur) samples it back as a
            // texture within the same Execute. Graph-owned scratch with no
            // prior writer to chain against.
            builder.AllowSamePassReadWrite(blackboard.Scratch.SSAORaw);
            builder.Write(blackboard.Scratch.SSAORaw, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto rawRead = builder.Read(blackboard.Scratch.SSAORaw, RGReadUsage::RenderTargetRead);
        }

        if (blackboard.Scratch.SSAOBlur.IsValid())
        {
            m_SelectedBlurFramebuffer = blackboard.Scratch.SSAOBlur;
            builder.Write(blackboard.Scratch.SSAOBlur, RGWriteUsage::RenderTarget);
        }

        // Publish the pass-owned raw GL noise texture so it resolves through
        // RenderGraph::GetRegisteredResources() — without this both
        // olo_render_list_targets and olo_render_capture_target are blind to it
        // (issue #607). Import-only: the pass samples it directly by raw id, so
        // there is deliberately no Read/Write declaration to change the graph's
        // ordering or culling.
        //
        // Reached only past the early-out above, so the import appears exactly on
        // the frames the pass runs. That gate (SSAOEnabled + ActiveAOTechnique)
        // is already hashed into the pipeline fingerprint (RenderPipeline.cpp
        // HashBool(SSAOEnabled) / HashU32(ActiveAOTechnique)), so the
        // topology-keyed caches invalidate correctly when it flips
        // (docs/agent-rules/render-pipeline-caches.md).
        if (m_NoiseTexture.IsValid())
        {
            RGResourceDesc noiseDesc =
                RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, kNoiseTargetName);
            noiseDesc.Format = RGResourceFormat::RG16Float;
            noiseDesc.Width = 4;
            noiseDesc.Height = 4;
            [[maybe_unused]] const RGTextureHandle noiseHandle =
                builder.ImportTextureHandle(kNoiseTargetName, m_NoiseTexture, noiseDesc);
        }
    }

    SSAORenderPass::~SSAORenderPass()
    {
        if (m_NoiseTexture.IsValid())
        {
            // Retire the heap descriptors naming this texture BEFORE deleting it.
            // ResourceRegistry deliberately keeps a handle alive across an
            // in-place reload, so a view's own generation cannot detect that its
            // descriptor now names a deleted object — OffsetOf would go on
            // answering a valid offset, and the persistent view cache would keep
            // serving the stale entry on a hit (issue #691 Phase 3).
            RHI::DescriptorHeap::Get().InvalidateResource(m_NoiseTexture);
            RenderCommand::DeleteTexture(m_NoiseTexture);
        }
    }

    void SSAORenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_HalfWidth = std::max(1u, spec.Width / 2);
        m_HalfHeight = std::max(1u, spec.Height / 2);

        // Load SSAO shaders
        m_SSAOShader = Shader::Create("assets/shaders/SSAO.glsl");
        m_SSAOBlurShader = Shader::Create("assets/shaders/SSAO_Blur.glsl");

        // Create 4x4 noise texture for random rotation
        CreateNoiseTexture();

        OLO_CORE_INFO("SSAORenderPass: Initialized with half-res {}x{}", m_HalfWidth, m_HalfHeight);
    }

    void SSAORenderPass::CreateNoiseTexture()
    {
        OLO_PROFILE_FUNCTION();

        // Generate 4x4 random rotation vectors in tangent space (xy rotation, z=0)
        std::mt19937 gen(42); // Fixed seed for deterministic noise
        std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);

        std::array<glm::vec2, 16> noise;
        for (auto& n : noise)
        {
            glm::vec2 v(dist(gen), dist(gen));
            f32 len = glm::length(v);
            n = (len > 1e-6f) ? v / len : glm::vec2(1.0f, 0.0f);
        }

        m_NoiseTexture = RenderCommand::CreateTexture2DHandle(4, 4, RHI::Format::RG16Float);
        RenderCommand::UploadTextureSubImage2D(m_NoiseTexture, 4, 4, RHI::Format::RG32Float, noise.data());
        RenderCommand::SetTextureFilter(m_NoiseTexture, RHI::Filter::Nearest, RHI::Filter::Nearest);
        RenderCommand::SetTextureWrap(m_NoiseTexture, RHI::AddressMode::Repeat);
    }

    void SSAORenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        m_Target = nullptr;

        if (!m_Settings.SSAOEnabled || m_Settings.ActiveAOTechnique != AOTechnique::SSAO || !IsReadyForExecution())
        {
            return;
        }

        // Phase F slice 37 — self-resolving SceneDepth and SceneNormals: look
        // up directly from the render graph blackboard so no per-frame
        // side-channel setter calls are needed from EndScene().
        // Identities (issue #691 step 3, slice 7). These resolve now that the
        // transient planner records a handle alongside the native id — before
        // that, ResolveTextureHandle answered null for every pooled texture and
        // this pass had to stay on driver names.
        RHI::ResourceHandle depthTexture{};
        RHI::ResourceHandle normalsTexture{};
        RHI::ResourceHandle aoOutputTexture{};
        if (m_SelectedSceneDepthTexture.IsValid())
            depthTexture = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (m_SelectedSceneNormalsTexture.IsValid())
            normalsTexture = context.ResolveTextureHandle(m_SelectedSceneNormalsTexture);
        if (m_SelectedAOOutputTexture.IsValid())
            aoOutputTexture = context.ResolveTextureHandle(m_SelectedAOOutputTexture);
        if (!depthTexture.IsValid() || !normalsTexture.IsValid() || !aoOutputTexture.IsValid())
        {
            return;
        }

        // Phase D / H follow-up: resolve the raw SSAO scratch framebuffer from
        // the transient pool via the blackboard. This pass now requires the
        // graph-owned scratch target; the owned fallback framebuffer has been
        // retired.
        Ref<Framebuffer> rawFB;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
            rawFB = context.ResolveFramebuffer(outputHandle);
        Ref<Framebuffer> blurFB;
        if (m_SelectedBlurFramebuffer.IsValid())
            blurFB = context.ResolveFramebuffer(m_SelectedBlurFramebuffer);
        if (!rawFB || !blurFB)
            return;

        m_Target = blurFB;

        // (Dropped the per-frame "inputs depthTex=N" trace: the AO output is
        // double-buffered so the texture ID flips every frame and the dedup
        // never held — fired ~60 times per second. Same broken pattern as the
        // GTAORenderPass / AOApplyRenderPass logs that were removed earlier.)

        // Upload SSAO parameters to UBO
        if (m_SSAOUBO && m_GPUData)
        {
            m_GPUData->Radius = m_Settings.SSAORadius;
            m_GPUData->Bias = m_Settings.SSAOBias;
            m_GPUData->Intensity = m_Settings.SSAOIntensity;
            m_GPUData->Samples = m_Settings.SSAOSamples;
            m_GPUData->ScreenWidth = static_cast<i32>(m_HalfWidth);
            m_GPUData->ScreenHeight = static_cast<i32>(m_HalfHeight);
            m_SSAOUBO->SetData(m_GPUData, SSAOUBOData::GetSize());
            m_SSAOUBO->Bind();
        }

        // --- Pass 1: Generate raw SSAO ---
        rawFB->Bind();
        context.SetViewport(0, 0, m_HalfWidth, m_HalfHeight);
        context.SetClearColor({ 1.0f, 1.0f, 1.0f, 1.0f }); // White = no occlusion
        context.Clear();
        context.SetDepthTest(false);
        context.SetBlendState(false);

        m_SSAOShader->Bind();

        // Heap-bindless where available, slot-based otherwise — one call, and the
        // slot constant keeps its meaning either way (issue #691 Phase 3). The
        // LIFETIME argument is the only judgement each site needs, and it is not
        // cosmetic: a FrameTransient view is retired at the frame boundary so a
        // held offset reports stale, while a Persistent one is memoised and its
        // offset is stable for the object's life.
        //
        // Depth and normals are graph-owned attachments that can be reallocated
        // or aliased between frames, so they are transient. The noise texture is
        // created once in Init and lives as long as the pass.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTexture,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_SCENE_NORMALS, normalsTexture,
                                        RHI::HeapSlotLifetime::FrameTransient);

        // Nearest+Repeat — the combination the two-bool sampler of Phase 1 could
        // not express, and the reason RHI::Filter / RHI::AddressMode exist. Under
        // the heap this sampler state rides in the descriptor rather than on the
        // texture object, which is what a split sampler heap will need.
        RHI::SamplerDesc noiseSampler;
        noiseSampler.MinFilter = RHI::Filter::Nearest;
        noiseSampler.MagFilter = RHI::Filter::Nearest;
        noiseSampler.LinearMipFilter = false;
        noiseSampler.AddressU = RHI::AddressMode::Repeat;
        noiseSampler.AddressV = RHI::AddressMode::Repeat;
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_SSAO_NOISE, m_NoiseTexture,
                                        RHI::HeapSlotLifetime::Persistent, noiseSampler);

        context.FlushHeapOffsets();

        DrawFullscreenTriangle();
        rawFB->Unbind();

        // --- Pass 2: Bilateral blur ---
        blurFB->Bind();
        context.SetViewport(0, 0, m_HalfWidth, m_HalfHeight);
        context.SetClearColor({ 1.0f, 1.0f, 1.0f, 1.0f });
        context.Clear();
        context.SetDepthTest(false);
        context.SetBlendState(false);

        m_SSAOBlurShader->Bind();

        // Bind raw SSAO result at slot 0 (texture from the transient or fallback FB).
        // The attachment's identity, not its GL name — attachments started
        // minting handles in slice 3, so this consumer can take one.
        context.BindTexture(0, rawFB->GetColorAttachmentHandle(0));

        // Bind scene depth at TEX_POSTPROCESS_DEPTH (slot 19) for bilateral edge detection
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTexture);

        DrawFullscreenTriangle();
        blurFB->Unbind();

        // Both operands are identities now, so the self-copy guard compares
        // OBJECTS rather than driver names — a recycled name can no longer make
        // two distinct textures look like the same one and skip a real copy.
        if (const RHI::ResourceHandle blurredAO = blurFB->GetColorAttachmentHandle(0);
            blurredAO.IsValid() && blurredAO != aoOutputTexture)
        {
            RenderCommand::CopyImageSubData(blurredAO, RendererAPI::TextureTargetType::Texture2D,
                                            aoOutputTexture, RendererAPI::TextureTargetType::Texture2D,
                                            m_HalfWidth, m_HalfHeight);
        }

        // Restore full-res viewport (will be set by next pass anyway, but be clean)
        context.SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void SSAORenderPass::DrawFullscreenTriangle() const
    {
        auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);
    }

    void SSAORenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_HalfWidth = std::max(1u, width / 2);
        m_HalfHeight = std::max(1u, height / 2);
        m_Target = nullptr;
    }

    void SSAORenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_HalfWidth = std::max(1u, width / 2);
        m_HalfHeight = std::max(1u, height / 2);
        m_Target = nullptr;
    }

    void SSAORenderPass::OnReset()
    {
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SelectedAOOutputTexture = {};
        m_SelectedBlurFramebuffer = {};
        m_Target = nullptr;
    }
} // namespace OloEngine
