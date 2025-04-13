#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLFramebuffer.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "OloEngine/Renderer/Shader.h"

#include <utility>

namespace OloEngine
{
	constexpr u32 s_MaxFramebufferSize = 8192;

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
		InitPostProcessing();
	}

	OpenGLFramebuffer::~OpenGLFramebuffer()
	{
		glDeleteFramebuffers(1, &m_RendererID);
		glDeleteTextures(static_cast<GLsizei>(m_ColorAttachments.size()), m_ColorAttachments.data());
		glDeleteTextures(1, &m_DepthAttachment);

		// Cleanup post-processing resources
		glDeleteVertexArrays(1, &m_PostProcessVAO);
		glDeleteBuffers(1, &m_PostProcessVBO);
	}

	void OpenGLFramebuffer::InitPostProcessing()
	{
		// Create and setup VAO/VBO for post-processing quad
		f32 quadVertices[] = {
			// positions        // texture coords
			-1.0f,  1.0f, 0.0f, 0.0f, 1.0f,
			-1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
			 1.0f,  1.0f, 0.0f, 1.0f, 1.0f,
			 1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
		};

		glCreateVertexArrays(1, &m_PostProcessVAO);
		glCreateBuffers(1, &m_PostProcessVBO);

		glBindVertexArray(m_PostProcessVAO);
		glBindBuffer(GL_ARRAY_BUFFER, m_PostProcessVBO);
		glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(f32), (void*)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(f32), (void*)(3 * sizeof(f32)));

		glBindVertexArray(0);

		// Load post-processing shader
		m_PostProcessShader = Shader::Create("assets/shaders/PostProcess.glsl");
	}

	void OpenGLFramebuffer::ApplyPostProcessing()
	{
		if (m_Specification.PostProcess == PostProcessEffect::None)
		{
			// Bind default framebuffer
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDisable(GL_DEPTH_TEST);

			// Clear the default framebuffer
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);

			// Bind our shader and texture
			m_PostProcessShader->Bind();
			glBindTextureUnit(0, m_ColorAttachments[0]);

			// Render quad
			glBindVertexArray(m_PostProcessVAO);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glBindVertexArray(0);

			// Cleanup
			m_PostProcessShader->Unbind();
			glEnable(GL_DEPTH_TEST);
		}
		// Add more post-processing effects here later
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
			Utils::AttachDepthTexture(m_RendererID, m_DepthAttachment, static_cast<int>(m_Specification.Samples), format, GL_DEPTH_STENCIL_ATTACHMENT, static_cast<int>(m_Specification.Width), static_cast<int>(m_Specification.Height));
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

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	void OpenGLFramebuffer::Bind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);
		glViewport(0, 0, static_cast<GLsizei>(m_Specification.Width), static_cast<GLsizei>(m_Specification.Height));

		bool isIntegerFramebuffer = false;
		for (const auto& spec : m_ColorAttachmentSpecifications)
		{
			if (spec.TextureFormat == FramebufferTextureFormat::RED_INTEGER)
			{
				isIntegerFramebuffer = true;
				break;
			}
		}

		if (isIntegerFramebuffer)
		{
			glDisable(GL_BLEND);
			glDisable(GL_DITHER);
		}
		else
		{
			glEnable(GL_BLEND);
			glEnable(GL_DITHER);
		}
	}

	void OpenGLFramebuffer::Unbind()
	{
		ApplyPostProcessing();
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

		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
		int pixelData{};
		glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixelData);
		return pixelData;
	}

	void OpenGLFramebuffer::ClearAttachment(const u32 attachmentIndex, const int value)
	{
		OLO_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size());

		auto const& spec = m_ColorAttachmentSpecifications[attachmentIndex];
		glClearTexImage(m_ColorAttachments[attachmentIndex], 0, Utils::OloFBTextureFormatToGL(spec.TextureFormat), GL_INT, &value);
	}

}
