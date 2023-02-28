#pragma once

#include "OloEngine/Renderer/Framebuffer.h"

namespace OloEngine
{
	class OpenGLFramebuffer : public Framebuffer
	{
	public:
		explicit OpenGLFramebuffer(FramebufferSpecification spec);
		~OpenGLFramebuffer() override;

		void Invalidate();

		void Bind() override;
		void Unbind() override;

		// TODO(olbu): Add BindColorAttachment, BindDepthAttachment

		void Resize(u32 width, u32 height) override;
		int ReadPixel(u32 attachmentIndex, int x, int y) override;

		void ClearAttachment(u32 attachmentIndex, int value) override;

		[[nodiscard("Store this!")]] u32 GetColorAttachmentRendererID(const u32 index) const override { OLO_CORE_ASSERT(index < m_ColorAttachments.size()); return m_ColorAttachments[index]; }
		// TODO(olbu): Add GetDepthAttachmentRendererID
		[[nodiscard("Store this!")]] const FramebufferSpecification& GetSpecification() const override { return m_Specification; }
	private:
		u32 m_RendererID = 0;
		FramebufferSpecification m_Specification;

		std::vector<FramebufferTextureSpecification> m_ColorAttachmentSpecifications;
		FramebufferTextureSpecification m_DepthAttachmentSpecification = FramebufferTextureFormat::None;

		std::vector<u32> m_ColorAttachments;
		u32 m_DepthAttachment = 0;
	};
}
