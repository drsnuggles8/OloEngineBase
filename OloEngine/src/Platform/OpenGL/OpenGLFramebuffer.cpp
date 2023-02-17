// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLFramebuffer.h"

#include <glad/gl.h>

#include <utility>

namespace OloEngine
{

	static const uint32_t s_MaxFramebufferSize = 8192;

	namespace Utils
	{
		[[nodiscard("Store this!")]]  constexpr static GLenum TextureTarget(const bool multisampled) noexcept
		{
			return multisampled ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
		}

		static void PrepareTexture(const uint32_t id, const int samples, const GLenum format, const int width, const int height)
		{
			OLO_CORE_ASSERT((format == GL_RGBA8 || format == GL_R32I || format == GL_DEPTH24_STENCIL8), "Invalid format.");

			if (const bool multisampled = samples > 1)
			{
				glTextureStorage2DMultisample(id, samples, format, width, height, GL_FALSE);
			}
			else
			{
				glTextureStorage2D(id, 1, format, width, height);
				glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
				glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTextureParameteri(id, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
				glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}

			glGenerateTextureMipmap(id);
		}

		static void CreateTextures(const bool multisampled, const int count, uint32_t* const outID)
		{
			glCreateTextures(TextureTarget(multisampled), count, outID);
		}

		static void BindTexture(const uint32_t id)
		{
			glBindTextureUnit(0, id);
		}

		static void BindTextures(const uint32_t firstID, const uint32_t count, const GLuint* id)
		{
			glBindTextures(firstID, static_cast<int>(count), id);
		}

		static void AttachColorTexture(const uint32_t fbo, const uint32_t id, const int samples, const GLenum internalFormat, const int width, const int height, const uint32_t index)
		{
			PrepareTexture(id, samples, internalFormat, width, height);

			glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + index, id, 0);

			if (glGetError() != GL_NO_ERROR)
			{
				OLO_CORE_ERROR("Error attaching color texture!");
			}
		}

		static void AttachDepthTexture(const uint32_t fbo, const uint32_t id, const int samples, const GLenum format, const GLenum attachmentType, const int width, const int height)
		{
			PrepareTexture(id, samples, format, width, height);

			glNamedFramebufferTexture(fbo, attachmentType, id, 0);

			if (glGetError() != GL_NO_ERROR)
			{
				OLO_CORE_ERROR("Error attaching depth texture!");
			}
		}

		[[nodiscard("Store this!")]] static bool IsDepthFormat(const FramebufferTextureFormat format) noexcept
		{
			switch (format)
			{
				case FramebufferTextureFormat::DEPTH24STENCIL8:  return true;
			}
			return false;
		}

		[[nodiscard("Store this!")]] static GLenum OloFBTextureFormatToGL(const FramebufferTextureFormat format)
		{
			switch (format)
			{
				case FramebufferTextureFormat::RGBA8:       return GL_RGBA8;
				case FramebufferTextureFormat::RED_INTEGER: return GL_RED_INTEGER;
			}

			OLO_CORE_ASSERT(false);
			return 0;
		}

		[[nodiscard("Store this!")]] static GLenum OloFBColorTextureFormatToGL(const FramebufferTextureFormat format)
		{
			switch (format)
			{
				case FramebufferTextureFormat::RGBA8:       return GL_RGBA8;
				case FramebufferTextureFormat::RED_INTEGER: return GL_R32I;
			}

			OLO_CORE_ASSERT(false);
			return 0;
		}

		[[nodiscard("Store this!")]] static GLenum OloFBDepthTextureFormatToGL(const FramebufferTextureFormat format)
		{
			switch (format)
			{
				case FramebufferTextureFormat::DEPTH24STENCIL8: return GL_DEPTH24_STENCIL8;
			}

			OLO_CORE_ASSERT(false);
			return 0;
		}
	}

	OpenGLFramebuffer::OpenGLFramebuffer(FramebufferSpecification specification)
		: m_Specification(std::move(specification))
	{
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
	{
		glDeleteFramebuffers(1, &m_RendererID);
		glDeleteTextures(static_cast<GLsizei>(m_ColorAttachments.size()), m_ColorAttachments.data());
		glDeleteTextures(1, &m_DepthAttachment);
	}

	void OpenGLFramebuffer::Invalidate()
	{
		if (m_RendererID)
		{
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

			for (size_t i = 0; i < colorAttachmentSize; ++i)
			{
				Utils::BindTexture(m_ColorAttachments[i]);
				// TODO(olbu): Add more FramebufferTextureFormats in Framebuffer.h and here
				GLenum internalFormat = Utils::OloFBColorTextureFormatToGL(m_ColorAttachmentSpecifications[i].TextureFormat);
				Utils::AttachColorTexture(m_RendererID, m_ColorAttachments[i], static_cast<int>(m_Specification.Samples), internalFormat, static_cast<int>(m_Specification.Width), static_cast<int>(m_Specification.Height), static_cast<uint32_t>(i));
			}
		}

		if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
		{
			Utils::CreateTextures(multisample, 1, &m_DepthAttachment);
			Utils::BindTexture(m_DepthAttachment);

			GLenum format = Utils::OloFBDepthTextureFormatToGL(m_DepthAttachmentSpecification.TextureFormat);
			Utils::AttachDepthTexture(m_RendererID, m_DepthAttachment, static_cast<int>(m_Specification.Samples), format, GL_DEPTH_STENCIL_ATTACHMENT, static_cast<int>(m_Specification.Width), static_cast<int>(m_Specification.Height));
		}

		if (m_ColorAttachments.size() > 1)
		{
			std::vector<GLenum> colorBuffers;
			auto colorAttachmentSize = static_cast<int>(m_ColorAttachments.size());
			for (int i = 0; i < colorAttachmentSize; ++i)
			{
				colorBuffers.push_back(static_cast<uint32_t>(GL_COLOR_ATTACHMENT0 + i));
			}

			glDrawBuffers(static_cast<GLsizei>(m_ColorAttachments.size()), colorBuffers.data());
		}
		else if (m_ColorAttachments.empty())
		{
			// Only depth-pass
			glDrawBuffer(GL_NONE);
		}

		OLO_CORE_ASSERT(glCheckNamedFramebufferStatus(m_RendererID, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Framebuffer is incomplete!");

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

	void OpenGLFramebuffer::Resize(uint32_t width, uint32_t height)
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

	int OpenGLFramebuffer::ReadPixel(const uint32_t attachmentIndex, const int x, const int y)
	{
		OLO_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size());

		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
		int pixelData{};
		glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixelData);
		return pixelData;
	}

	void OpenGLFramebuffer::ClearAttachment(const uint32_t attachmentIndex, const int value)
	{
		OLO_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size());

		auto const& spec = m_ColorAttachmentSpecifications[attachmentIndex];
		glClearTexImage(m_ColorAttachments[attachmentIndex], 0, Utils::OloFBTextureFormatToGL(spec.TextureFormat), GL_INT, &value);
	}

}
