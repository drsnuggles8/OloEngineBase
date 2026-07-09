#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLFramebuffer.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#include <glm/gtc/type_ptr.hpp>
#include <utility>

namespace OloEngine
{
    constexpr u32 s_MaxFramebufferSize = 8192;

    // Static member definitions for shared resources
    Ref<Shader> OpenGLFramebuffer::s_PostProcessShader = nullptr;
    std::once_flag OpenGLFramebuffer::s_InitOnceFlag;

    void OpenGLFramebuffer::InitSharedResources()
    {
        // Legacy monolithic post-processing resources no longer needed — post-processing is handled by dynamic standalone passes
    }

    void OpenGLFramebuffer::ShutdownSharedResources()
    {
        s_PostProcessShader.Reset();
    }

    OpenGLFramebuffer::OpenGLFramebuffer(FramebufferSpecification specification)
        : m_Specification(std::move(specification))
    {
        OLO_PROFILE_FUNCTION();

        for (const auto& spec : m_Specification.Attachments.Attachments)
        {
            if (!Utils::IsDepthFormat(spec.TextureFormat))
            {
                m_ColorAttachmentSpecifications.emplace_back(spec);
            }
            else
            {
                m_DepthAttachmentSpecification = spec;
            }
        }

        Invalidate();
    }
    OpenGLFramebuffer::~OpenGLFramebuffer()
    { // Track GPU memory deallocation
        OLO_TRACK_DEALLOC(this);

        // Unregister from GPU Resource Inspector
        GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);

        u32 fboId = m_RendererID;
        std::vector<u32> colorIds(m_ColorAttachments);
        u32 depthId = m_DepthAttachment;
        FrameResourceManager::Get().SubmitForDeletion([fboId, colorIds = std::move(colorIds), depthId]()
                                                      {
            glDeleteFramebuffers(1, &fboId);
            if (!colorIds.empty())
                glDeleteTextures(static_cast<GLsizei>(colorIds.size()), colorIds.data());
            glDeleteTextures(1, &depthId); });
    }

    void OpenGLFramebuffer::Invalidate()
    {
        OLO_PROFILE_FUNCTION();

        if (m_RendererID)
        { // Track GPU memory deallocation for existing framebuffer
            OLO_TRACK_DEALLOC(this);

            // Unregister from GPU Resource Inspector for existing framebuffer
            GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);

            u32 oldFboId = m_RendererID;
            std::vector<u32> oldColorIds(m_ColorAttachments);
            u32 oldDepthId = m_DepthAttachment;
            FrameResourceManager::Get().SubmitForDeletion([oldFboId, oldColorIds = std::move(oldColorIds), oldDepthId]()
                                                          {
                glDeleteFramebuffers(1, &oldFboId);
                if (!oldColorIds.empty())
                    glDeleteTextures(static_cast<GLsizei>(oldColorIds.size()), oldColorIds.data());
                glDeleteTextures(1, &oldDepthId); });

            m_ColorAttachments.clear();
            m_DepthAttachment = 0;
        }

        glCreateFramebuffers(1, &m_RendererID);
        glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);

        const bool multisample = m_Specification.Samples > 1;

        // Attachments
        if (!m_ColorAttachmentSpecifications.empty())
        {
            m_ColorAttachments.resize(m_ColorAttachmentSpecifications.size());
            auto colorAttachmentSize = m_ColorAttachments.size();
            Utils::CreateTextures(multisample, static_cast<int>(colorAttachmentSize), m_ColorAttachments.data());

            for (sizet i = 0; i < colorAttachmentSize; ++i)
            {
                Utils::BindTexture(m_ColorAttachments[i]);
                // TODO(olbu): Add more FramebufferTextureFormats in Framebuffer.h and here
                GLenum internalFormat = Utils::OloFBColorTextureFormatToGL(m_ColorAttachmentSpecifications[i].TextureFormat);
                Utils::AttachColorTexture(m_RendererID, m_ColorAttachments[i], static_cast<int>(m_Specification.Samples), internalFormat, static_cast<int>(m_Specification.Width), static_cast<int>(m_Specification.Height), static_cast<u32>(i));
            }
        }

        if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
        {
            Utils::CreateTextures(multisample, 1, &m_DepthAttachment);
            Utils::BindTexture(m_DepthAttachment);

            GLenum format = Utils::OloFBDepthTextureFormatToGL(m_DepthAttachmentSpecification.TextureFormat);
            GLenum attachmentType = (m_DepthAttachmentSpecification.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F)
                                        ? GL_DEPTH_ATTACHMENT
                                        : GL_DEPTH_STENCIL_ATTACHMENT;
            Utils::AttachDepthTexture(m_RendererID, m_DepthAttachment, static_cast<int>(m_Specification.Samples), format, attachmentType, static_cast<int>(m_Specification.Width), static_cast<int>(m_Specification.Height));
        }

        if (m_ColorAttachments.size() > 1)
        {
            std::vector<GLenum> colorBuffers;
            auto colorAttachmentSize = static_cast<int>(m_ColorAttachments.size());
            for (int i = 0; i < colorAttachmentSize; ++i)
            {
                colorBuffers.emplace_back(static_cast<u32>(GL_COLOR_ATTACHMENT0 + i));
            }

            glDrawBuffers(static_cast<GLsizei>(m_ColorAttachments.size()), colorBuffers.data());
        }
        else if (m_ColorAttachments.empty())
        {
            // Only depth-pass
            glDrawBuffer(GL_NONE);
        }
        else
        {
            // No additional handling required.
        }
        OLO_CORE_ASSERT(glCheckNamedFramebufferStatus(m_RendererID, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Framebuffer is incomplete!");

        // Calculate framebuffer memory usage
        sizet framebufferMemory = 0;

        // Color attachments
        for (sizet i = 0; i < m_ColorAttachments.size(); ++i)
        {
            // Estimate color attachment memory
            u32 bytesPerPixel = 4; // Default RGBA8
            // TODO: Could improve this by checking actual format
            framebufferMemory += static_cast<sizet>(m_Specification.Width) * m_Specification.Height * bytesPerPixel;
        }

        // Depth attachment
        if (m_DepthAttachment != 0)
        {
            u32 depthBytesPerPixel = 4; // DEPTH24_STENCIL8
            framebufferMemory += static_cast<sizet>(m_Specification.Width) * m_Specification.Height * depthBytesPerPixel;
        }
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            framebufferMemory,
                            RendererMemoryTracker::ResourceType::Framebuffer,
                            "OpenGL Framebuffer");

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterFramebuffer(m_RendererID, "OpenGL Framebuffer", "Framebuffer");

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void OpenGLFramebuffer::Bind()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);

        if (m_ColorAttachments.empty())
        {
            glDrawBuffer(GL_NONE);
        }
        else if (m_ColorAttachments.size() == 1)
        {
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
        }
        else
        {
            std::vector<GLenum> colorBuffers;
            auto colorAttachmentCount = m_ColorAttachments.size();
            colorBuffers.reserve(colorAttachmentCount);
            for (sizet i = 0; i < colorAttachmentCount; ++i)
            {
                colorBuffers.emplace_back(static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + i));
            }
            glDrawBuffers(static_cast<GLsizei>(colorBuffers.size()), colorBuffers.data());
        }

        // Use the DRS render viewport override when set; fall back to physical size.
        const auto vpW = (m_RenderViewportWidth > 0) ? m_RenderViewportWidth : m_Specification.Width;
        const auto vpH = (m_RenderViewportHeight > 0) ? m_RenderViewportHeight : m_Specification.Height;
        glViewport(0, 0, static_cast<GLsizei>(vpW), static_cast<GLsizei>(vpH));
    }

    void OpenGLFramebuffer::Unbind()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void OpenGLFramebuffer::SetRenderViewportSize(const u32 width, const u32 height)
    {
        m_RenderViewportWidth = width;
        m_RenderViewportHeight = height;
    }

    void OpenGLFramebuffer::Resize(u32 width, u32 height)
    {
        if ((0 == width) || (0 == height) || (width > s_MaxFramebufferSize) || (height > s_MaxFramebufferSize))
        {
            OLO_CORE_WARN("Attempted to resize framebuffer to {0}, {1}", width, height);
            return;
        }

        m_Specification.Width = width;
        m_Specification.Height = height;
        // Physical resize supersedes the DRS override — clear it so the new
        // physical size is used directly until SetRenderScale() re-applies.
        m_RenderViewportWidth = 0;
        m_RenderViewportHeight = 0;

        Invalidate();
    }

    int OpenGLFramebuffer::ReadPixel(const u32 attachmentIndex, const int x, const int y)
    {
        OLO_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(),
                        "ReadPixel: attachment index {} >= count {}", attachmentIndex, m_ColorAttachments.size());

        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_RendererID);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
        int pixelData{};
        glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixelData);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        return pixelData;
    }

    void OpenGLFramebuffer::ClearAttachment(const u32 attachmentIndex, const int value)
    {
        OLO_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(),
                        "ClearAttachment: attachment index {} >= count {}", attachmentIndex, m_ColorAttachments.size());

        auto const& spec = m_ColorAttachmentSpecifications[attachmentIndex];
        glClearTexImage(m_ColorAttachments[attachmentIndex], 0, Utils::OloFBTextureFormatToGL(spec.TextureFormat), GL_INT, &value);
    }

    void OpenGLFramebuffer::ClearAttachment(const u32 attachmentIndex, const glm::vec4& value)
    {
        OLO_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(),
                        "ClearAttachment: attachment index {} >= count {}", attachmentIndex, m_ColorAttachments.size());

        // See ClearAllAttachments(): a stale bound program would be
        // revalidated by the driver during this framebuffer clear.
        Utils::GLClearProgramGuard programGuard;

        // Use glClearBufferfv to clear a specific color buffer
        glClearBufferfv(GL_COLOR, static_cast<GLint>(attachmentIndex), glm::value_ptr(value));
    }

    void OpenGLFramebuffer::AttachDepthTextureArrayLayer(u32 textureArrayRendererID, u32 layer)
    {
        OLO_PROFILE_FUNCTION();

        const GLenum attachmentType = (m_DepthAttachmentSpecification.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F || m_DepthAttachmentSpecification.TextureFormat == FramebufferTextureFormat::ShadowDepth)
                                          ? GL_DEPTH_ATTACHMENT
                                          : GL_DEPTH_STENCIL_ATTACHMENT;

        glNamedFramebufferTextureLayer(
            m_RendererID,
            attachmentType,
            textureArrayRendererID,
            0, // mip level
            static_cast<GLint>(layer));

        OLO_CORE_ASSERT(
            glCheckNamedFramebufferStatus(m_RendererID, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
            "Framebuffer incomplete after attaching depth texture array layer {}", layer);
    }

    void OpenGLFramebuffer::ClearAllAttachments(const glm::vec4& clearColor, int entityIdClear)
    {
        // A program left bound by the previous pass would be revalidated
        // against this framebuffer by the driver during the clears below
        // (NVIDIA id 131218 vertex-shader recompile) — unbind it for the clears.
        Utils::GLClearProgramGuard programGuard;

        // Only clear depth/stencil if this FBO actually has a depth attachment
        if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
        {
            GLbitfield clearBits = GL_DEPTH_BUFFER_BIT;
            GLint previousStencilWriteMask = 0;
            bool restoreStencilWriteMask = false;
            if (m_DepthAttachmentSpecification.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8)
            {
                clearBits |= GL_STENCIL_BUFFER_BIT;
                glClearStencil(0);

                glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
                if (previousStencilWriteMask == 0)
                {
                    // Clearing stencil with write-mask 0x00 is a no-op and emits
                    // GL debug warning 131076. Temporarily enable stencil writes.
                    glStencilMask(0xFF);
                    restoreStencilWriteMask = true;
                }
            }
            glClearDepth(1.0);
            glClear(clearBits);

            if (restoreStencilWriteMask)
            {
                glStencilMask(static_cast<GLuint>(previousStencilWriteMask));
            }
        }

        // Clear each color attachment based on its type
        for (sizet i = 0; i < m_ColorAttachmentSpecifications.size(); ++i)
        {
            auto const& spec = m_ColorAttachmentSpecifications[i];
            if (spec.TextureFormat == FramebufferTextureFormat::RED_INTEGER)
            {
                // Clear integer attachments with glClearBufferiv
                GLint clearValue = entityIdClear;
                glClearBufferiv(GL_COLOR, static_cast<GLint>(i), &clearValue);
            }
            else
            {
                // Clear float attachments with glClearBufferfv
                glClearBufferfv(GL_COLOR, static_cast<GLint>(i), glm::value_ptr(clearColor));
            }
        }
    }

} // namespace OloEngine
