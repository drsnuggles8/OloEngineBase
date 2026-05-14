#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/OITPrepareRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        constexpr GLint kOITAccumAttachmentIndex = 0;
        constexpr GLint kOITRevealageAttachmentIndex = 1;

        [[nodiscard]] bool HasBlitCompatibleDepth(const Ref<Framebuffer>& framebuffer)
        {
            if (!framebuffer)
                return false;

            const auto& attachments = framebuffer->GetSpecification().Attachments.Attachments;
            return std::any_of(attachments.begin(), attachments.end(),
                               [](const FramebufferTextureSpecification& attachment)
                               {
                                   return attachment.TextureFormat == FramebufferTextureFormat::Depth ||
                                          attachment.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8;
                               });
        }
    } // namespace

    OITPrepareRenderPass::OITPrepareRenderPass()
    {
        SetName("OITPreparePass");
    }

    void OITPrepareRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedOITFramebuffer = {};

        if (!m_Enabled || !m_HasContributors)
            return;

        if (blackboard.Scene.SceneColor.IsValid())
            SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
        if (blackboard.OIT.OITBuffer.IsValid())
            m_SelectedOITFramebuffer = blackboard.OIT.OITBuffer;

        if (blackboard.Scene.SceneDepthAttachment.IsValid())
        {
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepthAttachment, RGReadUsage::TransferSource);
        }

        if (blackboard.OIT.OITAccum.IsValid())
            builder.Write(blackboard.OIT.OITAccum, RGWriteUsage::Clear);
        if (blackboard.OIT.OITRevealage.IsValid())
            builder.Write(blackboard.OIT.OITRevealage, RGWriteUsage::Clear);
        if (blackboard.OIT.OITDepthAttachment.IsValid())
            builder.Write(blackboard.OIT.OITDepthAttachment, RGWriteUsage::TransferDest);
    }

    void OITPrepareRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
    }

    void OITPrepareRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Enabled)
            return;

        Ref<Framebuffer> sceneFramebuffer;
        Ref<Framebuffer> oitFramebuffer;
        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFramebuffer = context.ResolveFramebuffer(sceneHandle))
                sceneFramebuffer = resolvedSceneFramebuffer;
        }
        if (m_SelectedOITFramebuffer.IsValid())
            oitFramebuffer = context.ResolveFramebuffer(m_SelectedOITFramebuffer);

        if (!oitFramebuffer)
            return;

        PrepareFramebuffer(oitFramebuffer, sceneFramebuffer);
    }

    void OITPrepareRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void OITPrepareRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void OITPrepareRenderPass::OnReset()
    {
        m_SelectedOITFramebuffer = {};
    }

    void OITPrepareRenderPass::PrepareFramebuffer(const Ref<Framebuffer>& oitFramebuffer,
                                                  const Ref<Framebuffer>& sceneFramebuffer)
    {
        if (!oitFramebuffer || oitFramebuffer->GetRendererID() == 0)
            return;

        const auto& oitSpec = oitFramebuffer->GetSpecification();
        if (oitSpec.Width == 0 || oitSpec.Height == 0)
            return;

        const auto oitFramebufferID = oitFramebuffer->GetRendererID();
        const glm::vec4 accumClear(0.0f, 0.0f, 0.0f, 0.0f);
        glClearNamedFramebufferfv(oitFramebufferID, GL_COLOR, kOITAccumAttachmentIndex, glm::value_ptr(accumClear));

        const glm::vec4 revealageClear(1.0f, 0.0f, 0.0f, 0.0f);
        glClearNamedFramebufferfv(oitFramebufferID, GL_COLOR, kOITRevealageAttachmentIndex, glm::value_ptr(revealageClear));

        bool seededFromSceneDepth = false;
        if (sceneFramebuffer && sceneFramebuffer->GetRendererID() != 0 &&
            HasBlitCompatibleDepth(sceneFramebuffer))
        {
            const auto& sceneSpec = sceneFramebuffer->GetSpecification();
            if (sceneSpec.Width == oitSpec.Width && sceneSpec.Height == oitSpec.Height)
            {
                glBlitNamedFramebuffer(sceneFramebuffer->GetRendererID(), oitFramebufferID,
                                       0, 0, static_cast<GLint>(oitSpec.Width), static_cast<GLint>(oitSpec.Height),
                                       0, 0, static_cast<GLint>(oitSpec.Width), static_cast<GLint>(oitSpec.Height),
                                       GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                seededFromSceneDepth = true;
            }
        }

        if (!seededFromSceneDepth)
        {
            const GLfloat depthClear = 1.0f;
            glClearNamedFramebufferfv(oitFramebufferID, GL_DEPTH, 0, &depthClear);
        }
    }
} // namespace OloEngine
