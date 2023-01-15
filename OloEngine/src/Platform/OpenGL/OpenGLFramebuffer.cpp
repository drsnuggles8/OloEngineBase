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
			return multisampled ? GL_TEXTURE_2D_MULTISAMPLE_ARRAY : GL_TEXTURE_2D_ARRAY;
		}

		static void CreateTexturesArray(const bool multisampled, const uint32_t count, uint32_t* const outID)
		{
			glCreateTextures(TextureTarget(multisampled), count, outID);
		}

		static void PrepareTexture(const uint32_t id, const int samples, const GLenum format, const uint32_t width, const uint32_t height, const uint32_t layer)
		{
			OLO_CORE_TRACE("Preparing texture with id: {0}, samples: {1}, format: {2}, width: {3}, height: {4}, number of layers: {5}", id, samples, format, width, height, layer);
			OLO_CORE_ASSERT((format == GL_RGBA8 || format == GL_R32I || format == GL_DEPTH24_STENCIL8), "Invalid format.");

			if (const bool multisampled = samples > 1)
			{
				glTextureStorage3DMultisample(id, samples, format, width, height, layer, GL_FALSE);
			}
			else
			{
				glTextureStorage3D(id, 1, format, width, height, layer);
				glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTextureParameteri(id, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
				glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}
		}

		static void BindTexture(const uint32_t id)
		{
			glBindTextureUnit(0, id);
		}

		static void AttachColorTexture(const uint32_t fbo, const uint32_t id, const int samples, const GLenum internalFormat, const uint32_t width, const uint32_t height, const int index, const int layer)
		{
			PrepareTexture(id, samples, internalFormat, width, height, layer);

			glNamedFramebufferTextureLayer(fbo, GL_COLOR_ATTACHMENT0 + index, id, 0, layer);

			if (glGetError() != GL_NO_ERROR)
			{
				OLO_CORE_ERROR("Error attaching color texture!");
			}

		}

		static void AttachDepthTexture(const uint32_t fbo, const uint32_t id, const int samples, const GLenum format, const GLenum attachmentType, const uint32_t width, const uint32_t height, const int layer)
		{
			PrepareTexture(id, samples, format, width, height, layer);

			glNamedFramebufferTextureLayer(fbo, attachmentType, id, 0, layer);

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

		void PrintAttachmentParameters(uint32_t framebuffer, GLenum attachment)
		{
			GLint objectType;
			GLint objectName;
			GLint level;
			GLint layer;
			GLint redSize;
			GLint greenSize;
			GLint blueSize;
			GLint alphaSize;
			GLint depthSize;
			GLint stencilSize;
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objectName);
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &layer);
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &redSize);
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &greenSize);
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &blueSize);
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alphaSize);
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &depthSize);
			glGetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &stencilSize);
			OLO_CORE_TRACE("Attachment parameters for framebuffer {0} and attachment {1}", framebuffer, attachment);
			OLO_CORE_TRACE("Object type: {0}", objectType); 
			OLO_CORE_TRACE("Object name: {0}", objectName); 
			OLO_CORE_TRACE("Level: {0}", level);
			OLO_CORE_TRACE("Layer: {0}", layer);
			OLO_CORE_TRACE("Red Size: {0}", redSize);
			OLO_CORE_TRACE("Green Size: {0}", greenSize);
			OLO_CORE_TRACE("Blue Size: {0}", blueSize);
			OLO_CORE_TRACE("Alpha Size: {0}", alphaSize);
			OLO_CORE_TRACE("Depth size: {0}", depthSize);
			OLO_CORE_TRACE("Stencil size: {0}", stencilSize);
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
			auto colorAttachmentSize = static_cast<int>(m_ColorAttachments.size());
			OLO_CORE_TRACE("Creating {0} color texture arrays!", colorAttachmentSize);
			Utils::CreateTexturesArray(multisample, static_cast<uint32_t>(colorAttachmentSize), m_ColorAttachments.data());

			for (int i = 0; i < colorAttachmentSize; ++i)
			{
				Utils::BindTexture(m_ColorAttachments[i]);
				// TODO(olbu): Add more FramebufferTextureFormats in Framebuffer.h and here
				GLenum internalFormat = Utils::OloFBColorTextureFormatToGL(m_ColorAttachmentSpecifications[i].TextureFormat);
				Utils::AttachColorTexture(m_RendererID, m_ColorAttachments[i], m_Specification.Samples, internalFormat, m_Specification.Width, m_Specification.Height, i, 1);

				GLenum status = glCheckNamedFramebufferStatus(m_RendererID, GL_FRAMEBUFFER);
				if (status != GL_FRAMEBUFFER_COMPLETE)
				{
					OLO_CORE_ERROR("Framebuffer is incomplete. Status: {0}", status);
				}
			}
		}

		if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
		{
			Utils::CreateTexturesArray(multisample, 1, &m_DepthAttachment);
			Utils::BindTexture(m_DepthAttachment);

			GLenum format = Utils::OloFBDepthTextureFormatToGL(m_DepthAttachmentSpecification.TextureFormat);
			Utils::AttachDepthTexture(m_RendererID, m_DepthAttachment, m_Specification.Samples, format, GL_DEPTH_STENCIL_ATTACHMENT, m_Specification.Width, m_Specification.Height, 1);
		}

		if (m_ColorAttachments.size() > 1)
		{
			std::vector<GLenum> colorBuffers;
			for (int i = 0; i < m_ColorAttachments.size(); i++)
			{
				colorBuffers.push_back(GL_COLOR_ATTACHMENT0 + i);
			}

			glDrawBuffers(static_cast<GLsizei>(m_ColorAttachments.size()), colorBuffers.data());
		}
		else if (m_ColorAttachments.empty())
		{
			// Only depth-pass
			glDrawBuffer(GL_NONE);
		}

		// Check for attachments
		Utils::PrintAttachmentParameters(m_RendererID, GL_DEPTH_ATTACHMENT);
		Utils::PrintAttachmentParameters(m_RendererID, GL_COLOR_ATTACHMENT0);
		Utils::PrintAttachmentParameters(m_RendererID, GL_COLOR_ATTACHMENT1);
		Utils::PrintAttachmentParameters(m_RendererID, GL_COLOR_ATTACHMENT2);
		Utils::PrintAttachmentParameters(m_RendererID, GL_COLOR_ATTACHMENT3);

		// Check framebuffer completeness
		auto test = GL_TEXTURE;
		GLenum status = glCheckNamedFramebufferStatus(m_RendererID, GL_FRAMEBUFFER); // 36054 => incomplete attachment somewhere
		OLO_CORE_TRACE("Framebuffer status: {0}", status);
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
