#pragma once

#include "OloEngine/Renderer/Framebuffer.h"

namespace OloEngine {

	class OpenGLFramebuffer : public Framebuffer
	{
	public:
		explicit OpenGLFramebuffer(FramebufferSpecification  spec);
		~OpenGLFramebuffer() override;

		void Invalidate();

		void Bind() override;
		void Unbind() override;

		void Resize(uint32_t width, uint32_t height) override;
		int ReadPixel(uint32_t attachmentIndex, int x, int y) override;

		void ClearAttachment(uint32_t attachmentIndex, int value) override;

		[[nodiscard("Store this, you probably wanted another function!")]] uint32_t GetColorAttachmentRendererID(const uint32_t index) const override { OLO_CORE_ASSERT(index < m_ColorAttachments.size()) return m_ColorAttachments[index]; }

		[[nodiscard("This returns m_Specification, you probably wanted another function!")]] const FramebufferSpecification& GetSpecification() const override { return m_Specification; }
	private:
		uint32_t m_RendererID = 0;
		FramebufferSpecification m_Specification;

		std::vector<FramebufferTextureSpecification> m_ColorAttachmentSpecifications;
		FramebufferTextureSpecification m_DepthAttachmentSpecification = FramebufferTextureFormat::None;

		std::vector<uint32_t> m_ColorAttachments;
		uint32_t m_DepthAttachment = 0;
	};

}
