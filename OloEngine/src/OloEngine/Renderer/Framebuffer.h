#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
	// TODO(olbu): Add more formats here and to the OpenFLFramebuffer functions
	enum class FramebufferTextureFormat
	{
		None = 0,

		// Color formats
		RGBA8,
		RED_INTEGER,

		// Depth/stencil formats
		DEPTH24STENCIL8,

		// Defaults
		Depth = DEPTH24STENCIL8
	};

	struct FramebufferTextureSpecification
	{
		FramebufferTextureSpecification() = default;
		FramebufferTextureSpecification(FramebufferTextureFormat const format)
			: TextureFormat(format) {}

		FramebufferTextureFormat TextureFormat = FramebufferTextureFormat::None;
		// TODO(olbu): filtering/wrap
	};

	struct FramebufferAttachmentSpecification
	{
		FramebufferAttachmentSpecification() = default;
		FramebufferAttachmentSpecification(std::initializer_list<FramebufferTextureSpecification> attachments)
			: Attachments(attachments) {}

		std::vector<FramebufferTextureSpecification> Attachments;
	};


	struct FramebufferSpecification
	{
		u32 Width = 0;
		u32 Height = 0;
		FramebufferAttachmentSpecification Attachments;
		u32 Samples = 1;

		bool SwapChainTarget = false;
	};

	class Framebuffer
	{
	public:
		virtual ~Framebuffer() = default;

		virtual void Bind() = 0;
		virtual void Unbind() = 0;

		virtual void Resize(u32 width, u32 height) = 0;
		virtual int ReadPixel(u32 attachmentIndex, int x, int y) = 0;

		// TODO(olbu): template this for different value types (int/float etc.)
		virtual void ClearAttachment(u32 attachmentIndex, int value) = 0;

		[[nodiscard("Store this!")]] virtual u32 GetColorAttachmentRendererID(u32 index) const = 0;

		[[nodiscard("Store this!")]] virtual const FramebufferSpecification& GetSpecification() const = 0;

		static Ref<Framebuffer> Create(const FramebufferSpecification& spec);
	};


}
