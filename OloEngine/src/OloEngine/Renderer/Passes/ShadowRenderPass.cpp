#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Texture2DArray.h"

namespace OloEngine
{
    ShadowRenderPass::ShadowRenderPass()
    {
        SetName("ShadowRenderPass");
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
    }

    void ShadowRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_RenderCallback || !m_ShadowMap || !m_ShadowMap->IsEnabled())
        {
            if (!m_WarnedOnce)
            {
                OLO_CORE_WARN("ShadowRenderPass::Execute skipped: callback={}, shadowMap={}, enabled={}",
                              m_RenderCallback != nullptr, m_ShadowMap != nullptr,
                              m_ShadowMap ? m_ShadowMap->IsEnabled() : false);
                m_WarnedOnce = true;
            }
            return;
        }

        if (!m_ShadowFramebuffer)
        {
            OLO_CORE_ERROR("ShadowRenderPass::Execute: Shadow framebuffer not initialized!");
            return;
        }

        const u32 resolution = m_ShadowMap->GetResolution();

        // Save current viewport and render state
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

        // Render CSM cascades
        if (const auto& csmArray = m_ShadowMap->GetCSMTextureArray(); csmArray)
        {
            if (!m_LoggedOnce)
            {
                OLO_CORE_INFO("ShadowRenderPass: Rendering {} CSM cascades, resolution={}, FBO={}, textureID={}",
                              ShadowMap::MAX_CSM_CASCADES, resolution, m_ShadowFramebuffer->GetRendererID(), csmArray->GetRendererID());
                m_LoggedOnce = true;
            }
            for (u32 cascade = 0; cascade < ShadowMap::MAX_CSM_CASCADES; ++cascade)
            {
                m_ShadowFramebuffer->AttachDepthTextureArrayLayer(csmArray->GetRendererID(), cascade);
                RenderCommand::ClearDepthOnly();

                const glm::mat4& lightVP = m_ShadowMap->GetCSMMatrix(cascade);
                m_RenderCallback(lightVP, cascade, ShadowPassType::CSM);
            }
        }

        // Render spot light shadows
        if (const auto& spotArray = m_ShadowMap->GetSpotTextureArray(); spotArray)
        {
            const auto spotCount = static_cast<u32>(m_ShadowMap->GetSpotShadowCount());
            for (u32 i = 0; i < spotCount; ++i)
            {
                m_ShadowFramebuffer->AttachDepthTextureArrayLayer(spotArray->GetRendererID(), i);
                RenderCommand::ClearDepthOnly();

                const glm::mat4& lightVP = m_ShadowMap->GetSpotMatrix(i);
                m_RenderCallback(lightVP, i, ShadowPassType::Spot);
            }
        }

        // Render point light shadow cubemaps (6 faces per light)
        if (const auto pointCount = static_cast<u32>(m_ShadowMap->GetPointShadowCount()); pointCount > 0)
        {
            for (u32 light = 0; light < pointCount; ++light)
            {
                const u32 cubemapID = m_ShadowMap->GetPointRendererID(light);
                if (cubemapID == 0)
                {
                    continue;
                }

                for (u32 face = 0; face < 6; ++face)
                {
                    // AttachDepthTextureArrayLayer uses glNamedFramebufferTextureLayer
                    // which works for cubemaps: layer 0-5 = +X,-X,+Y,-Y,+Z,-Z
                    m_ShadowFramebuffer->AttachDepthTextureArrayLayer(cubemapID, face);
                    RenderCommand::ClearDepthOnly();

                    const glm::mat4& faceVP = m_ShadowMap->GetPointFaceMatrix(light, face);
                    // Encode light index in the layer parameter so the callback
                    // can query ShadowMap::GetPointShadowParams(light) for position/far
                    m_RenderCallback(faceVP, light, ShadowPassType::Point);
                }
            }
        }

        // Restore state
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::BackCull();
        m_ShadowFramebuffer->Unbind();
        RenderCommand::SetViewport(prevViewport.x, prevViewport.y, prevViewport.width, prevViewport.height);

        // Clear callback to prevent stale captures across frames
        m_RenderCallback = nullptr;
    }

    Ref<Framebuffer> ShadowRenderPass::GetTarget() const
    {
        OLO_PROFILE_FUNCTION();
        return m_ShadowFramebuffer;
    }

    void ShadowRenderPass::SetRenderCallback(ShadowRenderCallback callback)
    {
        OLO_PROFILE_FUNCTION();
        m_RenderCallback = std::move(callback);
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
        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            Init(m_FramebufferSpec);
        }
    }
} // namespace OloEngine
