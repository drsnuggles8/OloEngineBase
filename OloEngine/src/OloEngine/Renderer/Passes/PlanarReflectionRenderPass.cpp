#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/PlanarReflectionRenderPass.h"

#include "OloEngine/Renderer/PlanarReflection.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    PlanarReflectionRenderPass::PlanarReflectionRenderPass()
    {
        SetName("PlanarReflectionPass");
        // Owned framebuffer is not a graph resource, so the scheduler can't see
        // an observable output and would cull the pass — keep it alive.
        SetSideEffects(SideEffect::NeverCull);
        OLO_CORE_INFO("Creating PlanarReflectionRenderPass.");
    }

    void PlanarReflectionRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_Width = spec.Width / kResolutionDivisor;
        m_Height = spec.Height / kResolutionDivisor;

        // Binding-43 UBO consumed by Water.glsl. Created up front (cheap) so the
        // disabled path can still publish enabled=0 every frame.
        m_ReflectionUBO = UniformBuffer::Create(UBOData::GetSize(), ShaderBindingLayout::UBO_PLANAR_REFLECTION);

        OLO_CORE_INFO("PlanarReflectionRenderPass: Initialized (target {}x{})", m_Width, m_Height);
    }

    void PlanarReflectionRenderPass::EnsureFramebuffer()
    {
        if (m_Width == 0 || m_Height == 0)
            return;

        if (!m_ReflectionFB)
        {
            FramebufferSpecification spec;
            spec.Width = m_Width;
            spec.Height = m_Height;
            // HDR colour so the reflected lit scene keeps its dynamic range, plus
            // a depth attachment so the re-rendered opaque geometry depth-sorts.
            spec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::Depth };
            m_ReflectionFB = Framebuffer::Create(spec);
        }
        else if (m_ReflectionFB->GetSpecification().Width != m_Width ||
                 m_ReflectionFB->GetSpecification().Height != m_Height)
        {
            m_ReflectionFB->Resize(m_Width, m_Height);
        }
    }

    void PlanarReflectionRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        // Order unconditionally — the graph topology is hashed on settings, not on
        // this pass's per-frame enable flag, so Setup may not re-run when the flag
        // flips. Declaring the dependency every time guarantees that whenever the
        // pass DOES replay, ScenePass has already batched the opaque bucket and the
        // shadow maps exist (otherwise it could run before either).
        builder.DependsOnPass("ScenePass");
        builder.DependsOnPass("ShadowPass");

        if (!m_Enabled)
            return;

        // The replayed bucket samples the shadow maps and IBL — declare the reads
        // so the graph keeps them alive and orders us correctly.
        if (blackboard.Shadows.ShadowMapCSM.IsValid())
        {
            [[maybe_unused]] const auto r = builder.Read(blackboard.Shadows.ShadowMapCSM, RGReadUsage::ShaderSample);
        }
        if (blackboard.Shadows.ShadowMapSpot.IsValid())
        {
            [[maybe_unused]] const auto r = builder.Read(blackboard.Shadows.ShadowMapSpot, RGReadUsage::ShaderSample);
        }
        for (const auto& pointHandle : blackboard.Shadows.ShadowMapPoint)
        {
            if (pointHandle.IsValid())
            {
                [[maybe_unused]] const auto r = builder.Read(pointHandle, RGReadUsage::ShaderSample);
            }
        }
        if (blackboard.IBL.PrefilterMap.IsValid())
        {
            [[maybe_unused]] const auto r = builder.Read(blackboard.IBL.PrefilterMap, RGReadUsage::ShaderSample);
        }
        if (blackboard.IBL.IrradianceMap.IsValid())
        {
            [[maybe_unused]] const auto r = builder.Read(blackboard.IBL.IrradianceMap, RGReadUsage::ShaderSample);
        }
    }

    void PlanarReflectionRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Refresh the UBO every frame so a stale enable flag can never reach the
        // water shader. Default = disabled; the enabled path overwrites it below.
        UBOData ubo;
        ubo.Params = glm::vec4(0.0f);

        const auto publishDisabled = [&]()
        {
            Renderer3D::SetPlanarReflectionTextureID(0);
            if (m_ReflectionUBO)
            {
                m_ReflectionUBO->SetData(&ubo, UBOData::GetSize());
                m_ReflectionUBO->Bind();
            }
        };

        if (!m_Enabled || !m_ScenePass)
        {
            publishDisabled();
            return;
        }

        EnsureFramebuffer();
        if (!m_ReflectionFB)
        {
            publishDisabled();
            return;
        }

        // Snapshot the real camera (CommandDispatch holds it from BeginScene; the
        // scene pass never mutates it). Restored before we return.
        const glm::mat4 realView = CommandDispatch::GetViewMatrix();
        const glm::mat4 realProj = CommandDispatch::GetProjectionMatrix();
        const glm::mat4 realVP = CommandDispatch::GetViewProjectionMatrix();
        const glm::vec3 realPos = CommandDispatch::GetViewPosition();

        const glm::vec4 plane = PlanarReflection::NormalizePlane(m_ReflectionPlane);
        const auto m = PlanarReflection::BuildReflectionMatrices(realView, realProj, realPos, plane);

        ubo.ViewProjection = m.ViewProjection;
        ubo.Params = glm::vec4(1.0f, m_Intensity, m_Distortion, 0.0f);

        auto& rendererAPI = RenderCommand::GetRendererAPI();

        // Restore the full core GL subset on exit (FBO / viewport / depth / cull /
        // front-face / program) so the second opaque pass cannot poison the
        // water / overlay / post passes that follow.
        GLStateGuard guard("PlanarReflectionRenderPass", GLStateGuard::Policy::Restore);

        // Swap the shared camera to the mirror camera. The mesh path rebinds the
        // shared CameraUBO buffer (uploaded here); terrain/voxel paths re-derive
        // from the CommandDispatch matrices — set both so every draw type in the
        // bucket sees the mirror camera and the oblique near-clip.
        CommandDispatch::SetViewMatrix(m.MirrorView);
        CommandDispatch::SetProjectionMatrix(m.ObliqueProjection);
        CommandDispatch::SetViewProjectionMatrix(m.ViewProjection);
        CommandDispatch::SetViewPosition(m.MirrorCameraPosition);
        CommandDispatch::UploadCameraUBO();
        CommandDispatch::InvalidateRenderStateCache();

        m_ReflectionFB->Bind();
        RenderCommand::SetViewport(0, 0, m_Width, m_Height);
        rendererAPI.SetDepthTest(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        rendererAPI.SetDepthMask(true);
        rendererAPI.SetBlendState(false);
        rendererAPI.SetCullFace(GL_BACK);
        rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        m_ReflectionFB->ClearAllAttachments({ 0.0f, 0.0f, 0.0f, 1.0f }, -1);

        // A reflection reverses handedness, so the geometry's front faces now wind
        // clockwise — declare CW the front winding for the replay so back-face
        // culling still removes the correct triangles.
        ::glFrontFace(GL_CW);

        // Re-establish shared scene resources the scene pass left bound (camera
        // UBO binding, shadow maps, IBL) and replay the already-batched opaque
        // bucket (skybox + meshes + terrain + voxels) into the mirror target.
        CommandDispatch::BindSceneResources();
        m_ScenePass->GetCommandBucket().Execute(rendererAPI);

        ::glFrontFace(GL_CCW);
        m_ReflectionFB->Unbind();

        // Restore the real camera for every downstream pass this frame.
        CommandDispatch::SetViewMatrix(realView);
        CommandDispatch::SetProjectionMatrix(realProj);
        CommandDispatch::SetViewProjectionMatrix(realVP);
        CommandDispatch::SetViewPosition(realPos);
        CommandDispatch::UploadCameraUBO();
        CommandDispatch::InvalidateRenderStateCache();

        Renderer3D::SetPlanarReflectionTextureID(m_ReflectionFB->GetColorAttachmentRendererID(0));
        if (m_ReflectionUBO)
        {
            m_ReflectionUBO->SetData(&ubo, UBOData::GetSize());
            m_ReflectionUBO->Bind();
        }
    }

    void PlanarReflectionRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_Width = width / kResolutionDivisor;
        m_Height = height / kResolutionDivisor;
        // The framebuffer itself is created lazily on first enabled Execute so a
        // scene that never uses planar reflection pays no VRAM.
    }

    void PlanarReflectionRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        SetupFramebuffer(width, height);
        if (m_ReflectionFB && m_Width > 0 && m_Height > 0)
            m_ReflectionFB->Resize(m_Width, m_Height);
    }

    void PlanarReflectionRenderPass::OnReset()
    {
        // Drop the texture publish so a stale reflection can't be sampled after a
        // graph reset / asset reload.
        Renderer3D::SetPlanarReflectionTextureID(0);
    }
} // namespace OloEngine
