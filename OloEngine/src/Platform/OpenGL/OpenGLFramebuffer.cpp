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

		static void CreateTextures(const bool multisampled, uint32_t* const outID, const uint32_t count)
		{
			OLO_CORE_ASSERT(count > 0, "Invalid count.");
			glCreateTextures(TextureTarget(multisampled), count, outID);
		}

		static void BindTexture(const uint32_t id)
		{
			OLO_CORE_ASSERT(id > 0, "Invalid texture ID.");
			glBindTextureUnit(0, id);
		}

		static void AttachColorTexture(const uint32_t fbo, const uint32_t id, const int samples, const GLenum internalFormat, const uint32_t width, const uint32_t height, const int index)
		{
			OLO_CORE_ASSERT(fbo > 0, "Invalid framebuffer object ID.");
			OLO_CORE_ASSERT(id > 0, "Invalid texture ID.");
			OLO_CORE_ASSERT(samples > 0, "Invalid number of samples.");
			OLO_CORE_ASSERT((internalFormat == GL_RGBA8 || internalFormat == GL_RGBA16F || internalFormat == GL_RGBA32F
				|| internalFormat == GL_DEPTH24_STENCIL8 || internalFormat == GL_DEPTH_COMPONENT32F), "Invalid internal format.");
			OLO_CORE_ASSERT(width > 0, "Invalid texture width.");
			OLO_CORE_ASSERT(height > 0, "Invalid texture height.");
			OLO_CORE_ASSERT(index >= 0 && index <= 15, "Invalid color attachment index.");

			if (bool multisampled = samples > 1)
			{
				glTextureStorage2DMultisample(id, samples, internalFormat, width, height, GL_FALSE);
			}
			else
			{
				glTextureStorage2D(id, 1, internalFormat, width, height);

				glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTextureParameteri(id, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
				glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}

			glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + index, id, 0);

			if (glGetError() != GL_NO_ERROR)
			{
				OLO_CORE_ERROR("Error attaching color texture!");
			}
		}

		static void AttachDepthTexture(const uint32_t fbo, const uint32_t id, const int samples, const GLenum format, const GLenum attachmentType, const uint32_t width, const uint32_t height)
		{
			OLO_CORE_ASSERT(fbo > 0, "Invalid framebuffer object ID.");
			OLO_CORE_ASSERT(id > 0, "Invalid texture ID.");
			OLO_CORE_ASSERT(samples > 0, "Invalid number of samples.");
			OLO_CORE_ASSERT((format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH_COMPONENT32F), "Invalid format.");
			OLO_CORE_ASSERT((attachmentType == GL_DEPTH_ATTACHMENT || attachmentType == GL_STENCIL_ATTACHMENT || attachmentType == GL_DEPTH_STENCIL_ATTACHMENT), "Invalid attachment type.");
			OLO_CORE_ASSERT(width > 0, "Invalid texture width.");
			OLO_CORE_ASSERT(height > 0, "Invalid texture height.");

			if (const bool multisampled = samples > 1)
			{
				glTextureStorage2DMultisample(id, samples, format, width, height, GL_FALSE);
			}
			else
			{
				glTextureStorage2D(id, 1, format, width, height);

				glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTextureParameteri(id, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
				glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}

			glNamedFramebufferTexture(fbo, attachmentType, id, 0);
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
			Utils::CreateTextures(multisample, m_ColorAttachments.data(), static_cast<uint32_t>(m_ColorAttachments.size()));

			auto stopCondition = m_ColorAttachments.size();
			for (size_t i = 0; i < stopCondition; ++i)
			{
				Utils::BindTexture(m_ColorAttachments[i]);
				// TODO(olbu): Add more FramebufferTextureFormats in Framebuffer.h and here
				switch (m_ColorAttachmentSpecifications[i].TextureFormat)
				{
					case FramebufferTextureFormat::RGBA8:
					{
						Utils::AttachColorTexture(m_RendererID, m_ColorAttachments[i], m_Specification.Samples, GL_RGBA8, m_Specification.Width, m_Specification.Height, static_cast<int>(i));
						break;
					}
					case FramebufferTextureFormat::RED_INTEGER:
					{
						Utils::AttachColorTexture(m_RendererID, m_ColorAttachments[i], m_Specification.Samples, GL_R32I, m_Specification.Width, m_Specification.Height, static_cast<int>(i));
						break;
					}
				}
			}
		}

		if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
		{
			Utils::CreateTextures(multisample, &m_DepthAttachment, 1);
			Utils::BindTexture(m_DepthAttachment);
			switch (m_DepthAttachmentSpecification.TextureFormat)
			{
				case FramebufferTextureFormat::DEPTH24STENCIL8:
				{
					Utils::AttachDepthTexture(m_RendererID, m_DepthAttachment, m_Specification.Samples, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT, m_Specification.Width, m_Specification.Height);
					break;
				}
			}
		}

		if (m_ColorAttachments.size() > 1)
		{
			// TODO(olbu): Add color_attachment4, size() <= 5
			OLO_CORE_ASSERT(m_ColorAttachments.size() <= 4);
			const GLenum buffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
			glDrawBuffers(static_cast<GLsizei>(m_ColorAttachments.size()), buffers);
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
		glViewport(0, 0, m_Specification.Width, m_Specification.Height);
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

	// TODO(olbu): Check if we can delete those two functions
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
		glClearTexImage(m_ColorAttachments[attachmentIndex], 0,
			Utils::OloFBTextureFormatToGL(spec.TextureFormat), GL_INT, &value);
	}

}
