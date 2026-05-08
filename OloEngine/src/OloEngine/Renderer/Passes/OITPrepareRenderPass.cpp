#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/OITPrepareRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
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

    void OITPrepareRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::OITAccum, ResourceHandle::Kind::Texture2D);
        DeclareWrite(ResourceNames::OITRevealage, ResourceHandle::Kind::Texture2D);
    }

    void OITPrepareRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void OITPrepareRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Enabled)
            return;

        Ref<Framebuffer> sceneFramebuffer;
        Ref<Framebuffer> oitFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            if (board->SceneColor.IsValid())
            {
                if (auto resolvedSceneFramebuffer = context.ResolveFramebuffer(board->SceneColor))
                    sceneFramebuffer = resolvedSceneFramebuffer;
            }
            if (board->OITAccum.IsValid())
            {
                if (auto resolvedOITFramebuffer = context.ResolveFramebuffer(board->OITAccum))
                    oitFramebuffer = resolvedOITFramebuffer;
            }
            else if (board->OITRevealage.IsValid())
            {
                if (auto resolvedOITFramebuffer = context.ResolveFramebuffer(board->OITRevealage))
                    oitFramebuffer = resolvedOITFramebuffer;
            }
        }

        if (!oitFramebuffer)
            return;

        PrepareFramebuffer(oitFramebuffer, sceneFramebuffer);
    }

    Ref<Framebuffer> OITPrepareRenderPass::GetTarget() const
    {
        return nullptr;
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
