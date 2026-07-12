#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FluidIntermediatesPass.h"

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
#include "Platform/OpenGL/OpenGLUtilities.h"

#include <glad/gl.h>

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
                      { return draw.PositionsSSBOId == 0 || draw.VelocitiesSSBOId == 0 ||
                               draw.CountersSSBOId == 0 || draw.ParticleUpperBound == 0 ||
                               !std::isfinite(draw.ParticleRadius) || draw.ParticleRadius <= 0.0f; });
        if (draws.empty())
            return;

        if (!m_Enabled || !IsReadyForExecution() ||
            m_DepthFBO == 0 || m_ThicknessFBO == 0 || m_Width == 0 || m_Height == 0)
        {
            return;
        }

        u32 sceneDepthID = 0;
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTexture(m_SelectedSceneDepthTexture);
        if (sceneDepthID == 0)
            return;

        GLStateGuard guard("FluidIntermediatesPass", GLStateGuard::Policy::Ignore);

        f32 cameraNear = 0.1f;
        f32 cameraFar = 1000.0f;
        ClusteredLighting::ExtractClipPlanes(Renderer3D::GetProjectionMatrix(), cameraNear, cameraFar);

        // The pass renders into raw pass-owned FBOs, so the viewport must be
        // set (and restored) by hand — engine Framebuffer::Bind() would
        // normally do this.
        GLint previousViewport[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, previousViewport);
        glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));

        // Scene depth for behind-geometry discard in both splat shaders
        // (water-identical slot/uniform name so IsKnownTextureBinding passes).
        context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, sceneDepthID);

        auto bindDrawBuffers = [](const FluidRenderData& draw)
        {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_FLUID_POSITIONS, draw.PositionsSSBOId);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_FLUID_VELOCITIES, draw.VelocitiesSSBOId);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_FLUID_COUNTERS, draw.CountersSSBOId);
        };

        // --- 1. Depth splat: nearest sphere-impostor view depth into A ------
        glBindFramebuffer(GL_FRAMEBUFFER, m_DepthFBO);
        {
            // Unbind any stale program for the clears — NVIDIA revalidates the
            // bound program against the new FBO during clears (debug id 131218).
            Utils::GLClearProgramGuard programGuard;
            constexpr f32 kNoFluidSentinel[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            glClearNamedFramebufferfv(m_DepthFBO, GL_COLOR, 0, kNoFluidSentinel);
            constexpr f32 kFarDepth = 1.0f;
            glClearNamedFramebufferfv(m_DepthFBO, GL_DEPTH, 0, &kFarDepth);
        }

        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthFunc(GL_LESS);
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
        glBindFramebuffer(GL_FRAMEBUFFER, m_ThicknessFBO);
        {
            Utils::GLClearProgramGuard programGuard;
            constexpr f32 kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            glClearNamedFramebufferfv(m_ThicknessFBO, GL_COLOR, 0, kZero);
        }

        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetBlendState(true);
        RenderCommand::SetBlendFunc(GL_ONE, GL_ONE);

        m_ThicknessShader->Bind();
        m_SplatVAO->Bind();
        for (const auto& draw : draws)
        {
            UploadDrawUBO(draw, cameraNear, cameraFar);
            bindDrawBuffers(draw);
            RenderCommand::DrawIndexedInstanced(m_SplatVAO, 6, draw.ParticleUpperBound);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // --- 3. Bilateral smooth: A -> B -> A ------------------------------
        // The last-uploaded FluidRenderUBO stays bound; with multiple fluids
        // the final draw's SmoothParams win for every fluid (v1 limitation,
        // matching the composite's single-appearance shading).
        m_SmoothShader->Bind();
        const u32 groupsX = (m_Width + kSmoothLocalSize - 1) / kSmoothLocalSize;
        const u32 groupsY = (m_Height + kSmoothLocalSize - 1) / kSmoothLocalSize;
        u32 smoothSrc = m_DepthTexA;
        u32 smoothDst = m_DepthTexB;
        for (u32 i = 0; i < kSmoothIterations; ++i)
        {
            RenderCommand::BindImageTexture(0, smoothSrc, 0, false, 0, GL_READ_ONLY, GL_R32F);
            RenderCommand::BindImageTexture(1, smoothDst, 0, false, 0, GL_WRITE_ONLY, GL_R32F);
            RenderCommand::DispatchCompute(groupsX, groupsY, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
            std::swap(smoothSrc, smoothDst);
        }
        m_SmoothShader->Unbind();

        // --- Restore state + unbind everything we bound --------------------
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        RenderCommand::BackCull();
        CommandDispatch::InvalidateRenderStateCache();

        context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_FLUID_POSITIONS, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_FLUID_VELOCITIES, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_FLUID_COUNTERS, 0);

        glViewport(previousViewport[0], previousViewport[1],
                   static_cast<GLsizei>(previousViewport[2]), static_cast<GLsizei>(previousViewport[3]));

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

        const auto createTexture = [width, height](GLenum internalFormat, GLint filter)
        {
            u32 id = 0;
            glCreateTextures(GL_TEXTURE_2D, 1, &id);
            glTextureStorage2D(id, 1, internalFormat,
                               static_cast<GLsizei>(width), static_cast<GLsizei>(height));
            glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, filter);
            glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, filter);
            glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            return id;
        };

        // NEAREST on the depth pair: the smooth compute and the composite's
        // normal reconstruction both want unfiltered texel values. LINEAR on
        // thickness: it's a smooth accumulation sampled once per pixel.
        m_DepthTexA = createTexture(GL_R32F, GL_NEAREST);
        m_DepthTexB = createTexture(GL_R32F, GL_NEAREST);
        m_ThicknessTex = createTexture(GL_RG16F, GL_LINEAR);
        m_SplatZTex = createTexture(GL_DEPTH_COMPONENT32F, GL_NEAREST);

        glCreateFramebuffers(1, &m_DepthFBO);
        glNamedFramebufferTexture(m_DepthFBO, GL_COLOR_ATTACHMENT0, m_DepthTexA, 0);
        glNamedFramebufferTexture(m_DepthFBO, GL_DEPTH_ATTACHMENT, m_SplatZTex, 0);
        constexpr GLenum kColor0 = GL_COLOR_ATTACHMENT0;
        glNamedFramebufferDrawBuffers(m_DepthFBO, 1, &kColor0);

        glCreateFramebuffers(1, &m_ThicknessFBO);
        glNamedFramebufferTexture(m_ThicknessFBO, GL_COLOR_ATTACHMENT0, m_ThicknessTex, 0);
        glNamedFramebufferDrawBuffers(m_ThicknessFBO, 1, &kColor0);

        if (glCheckNamedFramebufferStatus(m_DepthFBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
            glCheckNamedFramebufferStatus(m_ThicknessFBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            OLO_CORE_ERROR("FluidIntermediatesPass: fluid intermediate framebuffers incomplete ({}x{})",
                           width, height);
            ReleaseTargets();
        }
    }

    void FluidIntermediatesPass::ReleaseTargets()
    {
        if (m_DepthFBO != 0)
        {
            glDeleteFramebuffers(1, &m_DepthFBO);
            m_DepthFBO = 0;
        }
        if (m_ThicknessFBO != 0)
        {
            glDeleteFramebuffers(1, &m_ThicknessFBO);
            m_ThicknessFBO = 0;
        }

        const auto releaseTexture = [](u32& id)
        {
            if (id != 0)
            {
                glDeleteTextures(1, &id);
                id = 0;
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
