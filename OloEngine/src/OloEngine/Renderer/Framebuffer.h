#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // TODO(olbu): Add more formats here and to the OpenFLFramebuffer functions
    enum class FramebufferTextureFormat
    {
        None = 0,

        // Color formats
        RGBA8,
        RGBA16F,
        RGBA32F,
        RGB16F,
        RGB32F,
        RG16F,
        RG32F,
        RED_INTEGER,

        // Depth/stencil formats
        DEPTH24STENCIL8,
        DEPTH_COMPONENT32F,

        // Defaults
        Depth = DEPTH24STENCIL8,
        ShadowDepth = DEPTH_COMPONENT32F
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

    class Framebuffer : public RefCounted
    {
      public:
        virtual ~Framebuffer() = default;

        virtual void Bind() = 0;
        virtual void Unbind() = 0;

        virtual void Resize(u32 width, u32 height) = 0;
        virtual int ReadPixel(u32 attachmentIndex, int x, int y) = 0;

        // Clear integer attachment (e.g., entity ID)
        virtual void ClearAttachment(u32 attachmentIndex, int value) = 0;
        // Clear float/color attachment with RGBA color
        virtual void ClearAttachment(u32 attachmentIndex, const glm::vec4& value) = 0;
        // Clear all attachments with appropriate types (float for color, int for integer, depth/stencil)
        virtual void ClearAllAttachments(const glm::vec4& clearColor = glm::vec4(0.0f), int entityIdClear = -1) = 0;

        [[nodiscard("Store this!")]] virtual u32 GetColorAttachmentRendererID(u32 index) const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetDepthAttachmentRendererID() const = 0;
        [[nodiscard("Store this!")]] virtual const FramebufferSpecification& GetSpecification() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetRendererID() const = 0;

        // Attach a specific layer of a texture array as the depth attachment.
        // Used by shadow mapping to render into individual cascade layers.
        virtual void AttachDepthTextureArrayLayer(u32 textureArrayRendererID, u32 layer) = 0;

        static Ref<Framebuffer> Create(const FramebufferSpecification& spec);
    };
} // namespace OloEngine
