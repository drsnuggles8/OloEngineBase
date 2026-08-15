#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/PlanarReflectionRenderPass.h"

#include "OloEngine/Renderer/PlanarReflection.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

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
            // The replay re-runs the SCENE bucket's shaders, which write four
            // MRT outputs — use the scene pass's own attachment-layout
            // definition ([1] entity ID, [2] view normals, [3] velocity are
            // simply ignored here) so every replayed pipeline's fragment
            // interface matches its render targets. With only the colour
            // attachment, each replayed draw triggered a Vulkan validation
            // warning per unused output, and the replay-built PSOs could
            // never be shared with the scene pass's (#691 Phase 8).
            spec.Attachments = SceneRenderPass::SceneMRTAttachments();
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

        // The replayed bucket samples the shadow maps and IBL — declare the reads
        // UNCONDITIONALLY (not gated on m_Enabled). As above, the graph topology
        // is hashed on settings, not on this pass's per-frame enable flag, so
        // Setup may not re-run when the flag flips. Declaring the reads every
        // frame keeps those resources alive and orders us after their producers
        // whenever the pass DOES replay, even if the flip happened on a frame
        // that reused the cached graph.
        if (blackboard.Shadows.ShadowMapCSM.IsValid())
        {
            [[maybe_unused]] const auto r = builder.Read(blackboard.Shadows.ShadowMapCSM, RGReadUsage::ShaderSample);
        }
        if (blackboard.Shadows.ShadowMapAtlas.IsValid())
        {
            [[maybe_unused]] const auto r = builder.Read(blackboard.Shadows.ShadowMapAtlas, RGReadUsage::ShaderSample);
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
            Renderer3D::SetPlanarReflectionTextureID(RHI::NullResource);
            if (m_ReflectionUBO)
            {
                m_ReflectionUBO->SetData(&ubo, UBOData::GetSize());
                m_ReflectionUBO->Bind();
            }
        };

        const auto logArm = [&](u8 arm, const char* what)
        {
            if (m_LoggedExecuteArm != arm)
            {
                m_LoggedExecuteArm = arm;
                OLO_CORE_INFO("[PlanarReflection] {} (enabled={}, scenePass={}, fb={})", what, m_Enabled,
                              m_ScenePass != nullptr, m_ReflectionFB != nullptr);
            }
        };

        if (!m_Enabled || !m_ScenePass)
        {
            logArm(2, "mirror replay off");
            publishDisabled();
            return;
        }

        EnsureFramebuffer();
        if (!m_ReflectionFB)
        {
            logArm(3, "mirror replay blocked — no reflection framebuffer");
            publishDisabled();
            return;
        }
        logArm(1, "mirror replay running");

        // Snapshot the real camera (CommandDispatch holds it from BeginScene; the
        // scene pass never mutates it). Restored before we return.
        const glm::mat4 realView = CommandDispatch::GetViewMatrix();
        const glm::mat4 realProj = CommandDispatch::GetProjectionMatrix();
        const glm::mat4 realVP = CommandDispatch::GetViewProjectionMatrix();
        const glm::vec3 realPos = CommandDispatch::GetViewPosition();

        const glm::vec4 plane = PlanarReflection::NormalizePlane(m_ReflectionPlane);
        const auto m = PlanarReflection::BuildReflectionMatrices(realView, realProj, realPos, plane);

        // A8 seam, shader-reconstruction flavour (#691 Phase 7): Water.glsl
        // looks the reflection up as `(clip.xy / clip.w) * 0.5 + 0.5`, and the
        // target it samples was rendered below through UploadCameraUBO's
        // RASTERIZER flavour — so its rows are mirrored on Vulkan and the
        // lookup must mirror with them, or the water reflects the wrong half
        // of the render. Row flip only; nothing here reads depth. Identity on GL.
        ubo.ViewProjection = RHI::AdjustProjectionForShaderReconstruction(m.ViewProjection);
        ubo.Params = glm::vec4(1.0f, m_Intensity, m_Distortion, 0.0f);

        auto& rendererAPI = RenderCommand::GetRendererAPI();

        // Restore the full core GL subset on exit (FBO / viewport / depth / cull /
        // front-face / program) so the second opaque pass cannot poison the
        // water / overlay / post passes that follow.
        GLStateGuard guard("PlanarReflectionRenderPass", GLStateGuard::Policy::Restore);

        // The guard is a safety net, not the cleanup: its Restore policy logs every
        // field the pass failed to put back before rolling it back itself. This pass
        // reconfigures depth/blend/cull/polygon-mode below for the mirror replay, so
        // without an explicit restore it leaked DepthMask and DepthFunc every single
        // frame and the guard traced all of them — per-frame log spam that buries
        // real leaks. Roll back at the end and the guard stays quiet.
        //
        // Reuse the guard's own entry snapshot rather than capturing a second
        // one: the guard was constructed immediately above with nothing in
        // between, so the two are identical by construction, and GLStateSnapshot
        // ::Capture() is a long run of glGet* calls that stall the pipeline.
        const GLStateSnapshot& entryState = guard.EntryState();

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
        rendererAPI.SetDepthFunc(RHI::CompareOp::Less);
        rendererAPI.SetDepthMask(true);
        rendererAPI.SetBlendState(false);
        rendererAPI.SetCullFace(RHI::CullMode::Back);
        rendererAPI.SetPolygonMode(RHI::PolygonMode::Fill);
        m_ReflectionFB->ClearAllAttachments({ 0.0f, 0.0f, 0.0f, 1.0f }, -1);

        // A reflection reverses handedness, so the geometry's front faces now wind
        // clockwise — declare CW the front winding for the replay so back-face
        // culling still removes the correct triangles.
        RenderCommand::SetFrontFace(RHI::FrontFace::Clockwise);

        // Re-establish shared scene resources the scene pass left bound (camera
        // UBO binding, shadow maps, IBL) and replay the already-batched opaque
        // bucket (skybox + meshes + terrain + voxels) into the mirror target.
        CommandDispatch::BindSceneResources();
        m_ScenePass->GetCommandBucket().Execute(rendererAPI);

        RenderCommand::SetFrontFace(RHI::FrontFace::CounterClockwise);
        m_ReflectionFB->Unbind();

        // Put back everything the mirror replay reconfigured (depth test/func/mask,
        // blend, cull, polygon mode) so the guard's exit snapshot matches its entry
        // one. InvalidateRenderStateCache below re-syncs the cached RendererAPI
        // state with the GL state this just rolled back.
        entryState.ApplyCore();

        // Restore the real camera for every downstream pass this frame.
        CommandDispatch::SetViewMatrix(realView);
        CommandDispatch::SetProjectionMatrix(realProj);
        CommandDispatch::SetViewProjectionMatrix(realVP);
        CommandDispatch::SetViewPosition(realPos);
        CommandDispatch::UploadCameraUBO();
        CommandDispatch::InvalidateRenderStateCache();

        Renderer3D::SetPlanarReflectionTextureID(m_ReflectionFB->GetColorAttachmentHandle(0));
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
        Renderer3D::SetPlanarReflectionTextureID(RHI::NullResource);
    }
} // namespace OloEngine
