#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FluidIntermediatesPass.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <array>

#include <algorithm>
#include <cmath>
#include <utility>

namespace OloEngine
{
    FluidIntermediatesPass::FluidIntermediatesPass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("FluidIntermediatesPass");
        // Outputs are consumed by FluidCompositePass via raw texture ids
        // (outside graph resource tracking), so reachability culling must
        // never drop this pass. Note this means the pass culls nothing on its
        // own — the "no draws this frame" Setup early-out is the actual gate.
        SetSideEffects(SideEffect::NeverCull);
        OLO_CORE_INFO("Creating FluidIntermediatesPass.");
    }

    FluidIntermediatesPass::~FluidIntermediatesPass()
    {
        ReleaseTargets();
    }

    void FluidIntermediatesPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_DepthSplatShader = Shader::Create("assets/shaders/FluidDepthSplat.glsl");
        m_ThicknessShader = Shader::Create("assets/shaders/FluidThickness.glsl");
        m_SmoothShader = ComputeShader::Create("assets/shaders/compute/FluidSmooth.comp");
        if (!m_DepthSplatShader || !m_ThicknessShader || !m_SmoothShader || !m_SmoothShader->IsValid())
        {
            OLO_CORE_ERROR("FluidIntermediatesPass: Failed to load fluid splat/smooth shaders");
        }

        m_FluidRenderUBO = UniformBuffer::Create(
            UBOStructures::FluidRenderUBO::GetSize(),
            ShaderBindingLayout::UBO_FLUID_RENDER);

        // Splat geometry: unit quad + 6-index IBO, instanced per particle with
        // positions read from the fluid SSBOs (ParticleBatchRenderer GPU-VAO
        // pattern — no per-instance vertex attributes).
        m_SplatVAO = VertexArray::Create();
        {
            f32 quadVertices[] = {
                -0.5f, -0.5f, // bottom-left
                0.5f, -0.5f,  // bottom-right
                0.5f, 0.5f,   // top-right
                -0.5f, 0.5f   // top-left
            };
            auto quadVBO = VertexBuffer::Create(quadVertices, sizeof(quadVertices));
            quadVBO->SetLayout({ { ShaderDataType::Float2, "a_QuadPos" } });
            m_SplatVAO->AddVertexBuffer(quadVBO);

            u32 indices[] = { 0, 1, 2, 2, 3, 0 };
            auto indexBuffer = IndexBuffer::Create(indices, 6);
            m_SplatVAO->SetIndexBuffer(indexBuffer);
        }
    }

    void FluidIntermediatesPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};

        // Declares NOTHING when disabled or no fluid was submitted — the
        // pipeline fingerprint must hash this gate (issue #530 class).
        if (!m_Enabled || m_FrameDraws.empty())
            return;

        if (blackboard.Scene.SceneDepthAttachment.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepthAttachment;
            [[maybe_unused]] const auto sceneDepthRead =
                builder.Read(blackboard.Scene.SceneDepthAttachment, RGReadUsage::ShaderSample);
        }

        // Publish the pass-owned raw GL targets into the graph so they resolve
        // through RenderGraph::GetRegisteredResources() — without this, both
        // olo_render_list_targets and olo_render_capture_target are blind to
        // them and a broken fluid frame cannot be bisected to the splat / smooth
        // stage from an agent session (issue #607 / #630). Import-only: the pass
        // renders into them through its own FBOs, and the composite samples them
        // by raw id, so there is deliberately no Read/Write declaration to change
        // the graph's ordering or culling.
        //
        // Gated on the SAME condition as the early-out above (enabled + pending
        // draws) plus "the targets actually exist", so the imports appear exactly
        // on the frames the pass runs. That makes the set of registered resources
        // depend on HasPendingDraws() — already hashed into the pipeline
        // fingerprint (RenderPipeline.cpp, "Fluid draws gate ..."), so the
        // topology-keyed caches invalidate correctly
        // (docs/agent-rules/render-pipeline-caches.md).
        const auto publishTarget = [&builder](const char* name, RHI::ResourceHandle texture, RGResourceFormat format,
                                              u32 width, u32 height)
        {
            if (!texture.IsValid())
                return;
            RGResourceDesc desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, name);
            desc.Format = format;
            desc.Width = width;
            desc.Height = height;
            [[maybe_unused]] const RGTextureHandle handle = builder.ImportTextureHandle(name, texture, desc);
        };
        publishTarget(kSmoothedDepthTargetName, m_DepthTexA, RGResourceFormat::R32Float, m_Width, m_Height);
        publishTarget(kThicknessTargetName, m_ThicknessTex, RGResourceFormat::RG16Float, m_Width, m_Height);
    }

    void FluidIntermediatesPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        m_RanThisFrame = false;

        if (m_FrameDraws.empty())
            return;

        // One-shot: consume the draw list regardless of the guards below so a
        // skipped frame can never replay stale draws.
        std::vector<FluidRenderData> draws = std::move(m_FrameDraws);
        m_FrameDraws.clear();

        // Drop invalid submissions (missing buffers, zero instances, broken radius).
        std::erase_if(draws, [](const FluidRenderData& draw)
                      { return !draw.PositionsSSBOId.IsValid() || !draw.VelocitiesSSBOId.IsValid() ||
                               !draw.CountersSSBOId.IsValid() || draw.ParticleUpperBound == 0 ||
                               !std::isfinite(draw.ParticleRadius) || draw.ParticleRadius <= 0.0f; });
        if (draws.empty())
            return;

        if (!m_Enabled || !IsReadyForExecution() ||
            !m_DepthFBO.IsValid() || !m_ThicknessFBO.IsValid() || m_Width == 0 || m_Height == 0)
        {
            return;
        }

        RHI::ResourceHandle sceneDepthID{};
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (!sceneDepthID.IsValid())
            return;

        GLStateGuard guard("FluidIntermediatesPass", GLStateGuard::Policy::Ignore);

        f32 cameraNear = 0.1f;
        f32 cameraFar = 1000.0f;
        ClusteredLighting::ExtractClipPlanes(Renderer3D::GetProjectionMatrix(), cameraNear, cameraFar);

        // The pass renders into raw pass-owned FBOs, so the viewport must be
        // set (and restored) by hand — engine Framebuffer::Bind() would
        // normally do this.
        const Viewport previousViewport = RenderCommand::GetViewport();
        RenderCommand::SetViewport(0, 0, m_Width, m_Height);

        // Scene depth for behind-geometry discard in both splat shaders
        // (water-identical slot/uniform name so IsKnownTextureBinding passes).
        // PUBLISH: consumed by BOTH splat shaders drawn below, neither of which is
        // bound at this point, so the seam's program fork has no correct answer
        // here (issue #691).
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_WATER_DEPTH, sceneDepthID,
                                                 RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::FlushOffsets();

        auto bindDrawBuffers = [](const FluidRenderData& draw)
        {
            RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_FLUID_POSITIONS, draw.PositionsSSBOId);
            RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_FLUID_VELOCITIES, draw.VelocitiesSSBOId);
            RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_FLUID_COUNTERS, draw.CountersSSBOId);
        };

        // --- 1. Depth splat: nearest sphere-impostor view depth into A ------
        RenderCommand::BindFramebuffer(m_DepthFBO);
        {
            // The clear-program guard that used to be constructed here now lives
            // inside the backend clear (issue #691) — it is an
            // OpenGL driver hazard, so it is backend knowledge, and keeping it
            // here meant including a Platform/OpenGL header from a render pass.
            constexpr glm::vec4 kNoFluidSentinel(0.0f);
            RenderCommand::ClearFramebufferColorAttachment(m_DepthFBO, 0, kNoFluidSentinel);
            constexpr f32 kFarDepth = 1.0f;
            RenderCommand::ClearFramebufferDepth(m_DepthFBO, kFarDepth);
        }

        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling(); // camera-facing quads — winding is irrelevant

        m_DepthSplatShader->Bind();
        m_SplatVAO->Bind();
        for (const auto& draw : draws)
        {
            UploadDrawUBO(draw, cameraNear, cameraFar);
            bindDrawBuffers(draw);
            RenderCommand::DrawIndexedInstanced(m_SplatVAO, 6, draw.ParticleUpperBound);
        }

        // --- 2. Thickness: additive chord accumulation --------------------
        RenderCommand::BindFramebuffer(m_ThicknessFBO);
        {
            constexpr glm::vec4 kZero(0.0f);
            RenderCommand::ClearFramebufferColorAttachment(m_ThicknessFBO, 0, kZero);
        }

        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetBlendState(true);
        RenderCommand::SetBlendFunc(RHI::BlendFactor::One, RHI::BlendFactor::One);

        m_ThicknessShader->Bind();
        m_SplatVAO->Bind();
        for (const auto& draw : draws)
        {
            UploadDrawUBO(draw, cameraNear, cameraFar);
            bindDrawBuffers(draw);
            RenderCommand::DrawIndexedInstanced(m_SplatVAO, 6, draw.ParticleUpperBound);
        }

        RenderCommand::BindDefaultFramebuffer();

        // --- 3. Bilateral smooth: A -> B -> A ------------------------------
        // The last-uploaded FluidRenderUBO stays bound; with multiple fluids
        // the final draw's SmoothParams win for every fluid (v1 limitation,
        // matching the composite's single-appearance shading).
        m_SmoothShader->Bind();
        const u32 groupsX = (m_Width + kSmoothLocalSize - 1) / kSmoothLocalSize;
        const u32 groupsY = (m_Height + kSmoothLocalSize - 1) / kSmoothLocalSize;
        RHI::ResourceHandle smoothSrc = m_DepthTexA;
        RHI::ResourceHandle smoothDst = m_DepthTexB;
        for (u32 i = 0; i < kSmoothIterations; ++i)
        {
            // A PING-PONG NEEDS A FLUSH PER ITERATION, not one before the loop: src
            // and dst swap every pass, so each iteration stages two different offsets
            // and a hoisted flush would publish only the last pair.
            //
            // Persistent: both depth textures are pass-owned members, and each is read
            // in one iteration and written in the next — two accesses of one handle,
            // which is the case the backend widens residency for.
            HeapBinding::BindImageOrOffset(0, smoothSrc, 0, false, 0, RHI::Access::StorageRead,
                                           RHI::Format::R32Float, RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindImageOrOffset(1, smoothDst, 0, false, 0, RHI::Access::StorageWrite,
                                           RHI::Format::R32Float, RHI::HeapSlotLifetime::Persistent);
            HeapBinding::FlushOffsets();
            RenderCommand::DispatchCompute(groupsX, groupsY, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
            std::swap(smoothSrc, smoothDst);
        }
        m_SmoothShader->Unbind();

        // --- Restore state + unbind everything we bound --------------------
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetBlendFunc(RHI::BlendFactor::SrcAlpha, RHI::BlendFactor::OneMinusSrcAlpha);
        RenderCommand::BackCull();
        CommandDispatch::InvalidateRenderStateCache();

        // Cleared through the same seam so BOTH consumers see it: the slot-based
        // one loses its binding, the bindless one gets the reserved null offset.
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_WATER_DEPTH, RHI::NullResource,
                                                 RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::FlushOffsets();
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_FLUID_POSITIONS, RHI::NullResource);
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_FLUID_VELOCITIES, RHI::NullResource);
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_FLUID_COUNTERS, RHI::NullResource);

        RenderCommand::SetViewport(previousViewport.x, previousViewport.y,
                                   previousViewport.width, previousViewport.height);

        m_LastAppearance = draws.front();
        m_RanThisFrame = true;
    }

    void FluidIntermediatesPass::UploadDrawUBO(const FluidRenderData& draw, f32 cameraNear, f32 cameraFar)
    {
        UBOStructures::FluidRenderUBO ubo{};
        ubo.TintRadius = glm::vec4(draw.Tint, draw.ParticleRadius);
        ubo.AbsorptionParams = glm::vec4(draw.AbsorptionColor, draw.AbsorptionScale);
        ubo.FoamParams = glm::vec4(draw.FoamSpeedThreshold, 1.0f, 0.0f, 0.0f);
        // Depth falloff for the bilateral: a few particle radii keeps blur
        // from bleeding across silhouette discontinuities while still fusing
        // adjacent sphere shells into one surface.
        ubo.SmoothParams = glm::vec4(kDefaultBlurRadiusPx,
                                     std::max(draw.ParticleRadius * 4.0f, 1.0e-3f),
                                     cameraNear, cameraFar);
        ubo.ScreenParams = glm::vec4(static_cast<f32>(m_Width), static_cast<f32>(m_Height),
                                     1.0f / static_cast<f32>(m_Width), 1.0f / static_cast<f32>(m_Height));
        // Counts.z (env-map flag) is only meaningful in the composite, which
        // re-uploads this UBO with its own value.
        ubo.Counts = glm::uvec4(draw.ParticleUpperBound, static_cast<u32>(draw.EntityID), 0u, 0u);
        m_FluidRenderUBO->SetData(&ubo, sizeof(ubo));
        m_FluidRenderUBO->Bind();
    }

    void FluidIntermediatesPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateTargets(width, height);
    }

    void FluidIntermediatesPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        // Immutable-storage textures can't resize in place — recreate.
        CreateTargets(width, height);
    }

    void FluidIntermediatesPass::CreateTargets(u32 width, u32 height)
    {
        ReleaseTargets();

        if (width == 0 || height == 0)
            return;

        m_Width = width;
        m_Height = height;

        const auto createTexture = [width, height](RHI::Format internalFormat, RHI::Filter filter)
        {
            const RHI::ResourceHandle id = RenderCommand::CreateTexture2DHandle(width, height, internalFormat);
            RenderCommand::SetTextureFilter(id, filter, filter);
            RenderCommand::SetTextureWrap(id, RHI::AddressMode::ClampToEdge);
            return id;
        };

        // NEAREST on the depth pair: the smooth compute and the composite's
        // normal reconstruction both want unfiltered texel values. LINEAR on
        // thickness: it's a smooth accumulation sampled once per pixel.
        m_DepthTexA = createTexture(RHI::Format::R32Float, RHI::Filter::Nearest);
        m_DepthTexB = createTexture(RHI::Format::R32Float, RHI::Filter::Nearest);
        m_ThicknessTex = createTexture(RHI::Format::RG16Float, RHI::Filter::Linear);
        m_SplatZTex = createTexture(RHI::Format::D32Float, RHI::Filter::Nearest);

        static constexpr std::array<u32, 1> kColor0 = { 0u };

        m_DepthFBO = RenderCommand::CreateFramebufferHandle();
        RenderCommand::AttachFramebufferColorTexture(m_DepthFBO, 0, m_DepthTexA, 0);
        RenderCommand::AttachFramebufferDepthTexture(m_DepthFBO, m_SplatZTex, 0);
        RenderCommand::SetFramebufferDrawAttachments(m_DepthFBO, kColor0);

        m_ThicknessFBO = RenderCommand::CreateFramebufferHandle();
        RenderCommand::AttachFramebufferColorTexture(m_ThicknessFBO, 0, m_ThicknessTex, 0);
        RenderCommand::SetFramebufferDrawAttachments(m_ThicknessFBO, kColor0);

        if (!RenderCommand::IsFramebufferComplete(m_DepthFBO) ||
            !RenderCommand::IsFramebufferComplete(m_ThicknessFBO))
        {
            OLO_CORE_ERROR("FluidIntermediatesPass: fluid intermediate framebuffers incomplete ({}x{})",
                           width, height);
            ReleaseTargets();
        }
    }

    void FluidIntermediatesPass::ReleaseTargets()
    {
        if (m_DepthFBO.IsValid())
        {
            RenderCommand::DeleteFramebuffer(m_DepthFBO);
            m_DepthFBO = RHI::NullResource;
        }
        if (m_ThicknessFBO.IsValid())
        {
            RenderCommand::DeleteFramebuffer(m_ThicknessFBO);
            m_ThicknessFBO = RHI::NullResource;
        }

        const auto releaseTexture = [](RHI::ResourceHandle& id)
        {
            if (id.IsValid())
            {
                RenderCommand::DeleteTexture(id);
                id = RHI::NullResource;
            }
        };
        releaseTexture(m_DepthTexA);
        releaseTexture(m_DepthTexB);
        releaseTexture(m_ThicknessTex);
        releaseTexture(m_SplatZTex);

        m_Width = 0;
        m_Height = 0;
    }
} // namespace OloEngine
