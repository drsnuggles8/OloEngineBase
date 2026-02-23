#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLFramebuffer.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
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
        // Legacy post-processing resources no longer needed — post-processing is handled by PostProcessRenderPass
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

        glDeleteFramebuffers(1, &m_RendererID);
        glDeleteTextures(static_cast<GLsizei>(m_ColorAttachments.size()), m_ColorAttachments.data());
        glDeleteTextures(1, &m_DepthAttachment);
    }

    void OpenGLFramebuffer::Invalidate()
    {
        OLO_PROFILE_FUNCTION();

        if (m_RendererID)
        { // Track GPU memory deallocation for existing framebuffer
            OLO_TRACK_DEALLOC(this);

            // Unregister from GPU Resource Inspector for existing framebuffer
            GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);

            glDeleteFramebuffers(1, &m_RendererID);
            glDeleteTextures(static_cast<GLsizei>(m_ColorAttachments.size()), m_ColorAttachments.data());
            glDeleteTextures(1, &m_DepthAttachment);

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
        glViewport(0, 0, static_cast<GLsizei>(m_Specification.Width), static_cast<GLsizei>(m_Specification.Height));
    }

    void OpenGLFramebuffer::Unbind()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

        Invalidate();
    }

    int OpenGLFramebuffer::ReadPixel(const u32 attachmentIndex, const int x, const int y)
    {
        OLO_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size());

        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_RendererID);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
        int pixelData{};
        glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixelData);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        return pixelData;
    }

    void OpenGLFramebuffer::ClearAttachment(const u32 attachmentIndex, const int value)
    {
        OLO_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size());

        auto const& spec = m_ColorAttachmentSpecifications[attachmentIndex];
        glClearTexImage(m_ColorAttachments[attachmentIndex], 0, Utils::OloFBTextureFormatToGL(spec.TextureFormat), GL_INT, &value);
    }

    void OpenGLFramebuffer::ClearAttachment(const u32 attachmentIndex, const glm::vec4& value)
    {
        OLO_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size());

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
        // Clear depth and stencil first (these work with regular glClear)
        glClearDepth(1.0);
        glClearStencil(0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

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
