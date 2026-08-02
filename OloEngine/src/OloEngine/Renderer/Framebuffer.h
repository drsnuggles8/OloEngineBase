#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
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

        // Dynamic Resolution Scaling support.
        // When non-zero, Bind() sets glViewport to this size instead of the
        // physical framebuffer dimensions. Resize() resets this override to
        // zero (physical resize implies render viewport == physical size).
        virtual void SetRenderViewportSize(u32 width, u32 height) = 0;
        [[nodiscard]] virtual u32 GetRenderViewportWidth() const = 0;
        [[nodiscard]] virtual u32 GetRenderViewportHeight() const = 0;

        virtual int ReadPixel(u32 attachmentIndex, int x, int y) = 0;

        // Clear integer attachment (e.g., entity ID)
        virtual void ClearAttachment(u32 attachmentIndex, int value) = 0;
        // Clear float/color attachment with RGBA color
        virtual void ClearAttachment(u32 attachmentIndex, const glm::vec4& value) = 0;
        // Clear all attachments with appropriate types (float for color, int for integer, depth/stencil)
        virtual void ClearAllAttachments(const glm::vec4& clearColor = glm::vec4(0.0f), int entityIdClear = -1) = 0;

        [[nodiscard("Store this!")]] virtual u32 GetColorAttachmentRendererID(u32 index) const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetDepthAttachmentRendererID() const = 0;

        // Attachment IDENTITIES (issue #691 step 3). Separate from the
        // framebuffer's own GetRHIHandle(): the attachments are distinct GPU
        // objects that the engine samples through ResolveTexture, so "the
        // framebuffer's handle" is the wrong answer to "which texture is this?".
        //
        // These are a migration ROOT. RenderGraph::ResolveTexture returns
        // attachment ids for its framebuffer-view resources, so it cannot hand
        // out handles until these do — and almost every pass reads through
        // ResolveTexture. Ordering is forced: `native -> handle` is not
        // recoverable, so producers migrate before consumers.
        [[nodiscard("Store this!")]] virtual RHI::ResourceHandle GetColorAttachmentHandle(u32 index) const = 0;
        [[nodiscard("Store this!")]] virtual RHI::ResourceHandle GetDepthAttachmentHandle() const = 0;
        [[nodiscard("Store this!")]] virtual const FramebufferSpecification& GetSpecification() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetRendererID() const = 0;

        // Generation-checked identity, minted by RHI::ResourceRegistry
        // (issue #691 Phase 2 step 3). Sibling of GetRendererID() during the
        // migration: that one hands out the raw backend name and is deleted once
        // every caller has moved. Turning a handle back into a native object is
        // Platform/<Backend>/'s business.
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;

        // Attach a specific layer of a texture array as the depth attachment.
        // Used by shadow mapping to render into individual cascade layers.
        virtual void AttachDepthTextureArrayLayer(RHI::ResourceHandle textureArray, u32 layer) = 0;

        static Ref<Framebuffer> Create(const FramebufferSpecification& spec);
    };
} // namespace OloEngine
