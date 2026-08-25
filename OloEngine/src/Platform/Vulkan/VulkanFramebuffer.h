#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanFramebuffer.h — the Framebuffer backend twin of OpenGLFramebuffer
// (#691; split out of the single VulkanTransientResources.h):
// attachments, external attach, per-layer depth views, resize semantics
// matching the GL twin.
//
// This header exposes Vulkan types directly — it is included only by
// Platform/Vulkan siblings and by OLO_WITH_VULKAN-guarded engine factory TUs
// (the sanctioned factory-include pattern, rhi-abstraction-boundary.md).
// =============================================================================

// VulkanDevice.h provides <volk.h> and <vk_mem_alloc.h> (with the
// VMA_STATIC/DYNAMIC_VULKAN_FUNCTIONS config that must stay in sync with
// VulkanMemoryAllocator.cpp) — do NOT include either directly here, and NEVER
// <vulkan/vulkan.h> (volk owns the function pointers, ADR 0011 amendment 41a).
#include "Platform/Vulkan/VulkanDevice.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "Platform/Vulkan/VulkanTexture.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanFramebuffer — a bag of attachment images, no VkFramebuffer.
    //
    // The Vulkan backend targets dynamic rendering (no render-pass objects),
    // so there is deliberately NO VkFramebuffer object here: a framebuffer IS
    // its attachment images. Attachments are real VulkanTexture2D instances
    // (created via a FramebufferTextureFormat -> ImageFormat translation), so
    // their RHI handles and VulkanImageInfoRegistry entries come for free and
    // GetColorAttachmentHandle/GetDepthAttachmentHandle hand out genuine
    // per-attachment identities exactly like the GL twin.
    // -------------------------------------------------------------------------
    class VulkanFramebuffer : public Framebuffer
    {
      public:
        explicit VulkanFramebuffer(const FramebufferSpecification& spec);
        ~VulkanFramebuffer() override;

        // Retire every cached per-cascade depth view built over `image`, in
        // EVERY live framebuffer. Called from VulkanDeferredReclaim's destroy
        // pass, before the image is destroyed — a VkImageView must never
        // outlive the VkImage it views. (Framebuffers self-register for this;
        // see m_DepthArrayViews for what goes wrong without it.)
        static void ReleaseCachedDepthViewsForImage(VkImage image);

        // Publish / clear this framebuffer as the current render target
        // through VulkanBindingState; the dynamic-rendering scope consumes the
        // selection lazily at the next draw or clear.
        void Bind() override;
        void Unbind() override;

        void Resize(u32 width, u32 height) override;

        void SetRenderViewportSize(u32 width, u32 height) override;
        [[nodiscard]] u32 GetRenderViewportWidth() const override
        {
            return m_RenderViewportWidth;
        }
        [[nodiscard]] u32 GetRenderViewportHeight() const override
        {
            return m_RenderViewportHeight;
        }

        // Readback rides the ReadTextureSubImage spine (top-down coordinates,
        // amendment (85)); the three clears ride the facade's transfer clears
        // — integer for RED_INTEGER slots, float otherwise.
        int ReadPixel(u32 attachmentIndex, int x, int y) override;
        void ClearAttachment(u32 attachmentIndex, int value) override;
        void ClearAttachment(u32 attachmentIndex, const glm::vec4& value) override;
        void ClearAllAttachments(const glm::vec4& clearColor = glm::vec4(0.0f), int entityIdClear = -1) override;

        // Diagnostics-only fields: native GL names do not exist here.
        [[nodiscard("Store this!")]] u32 GetColorAttachmentRendererID(u32 index) const override
        {
            (void)index;
            return 0;
        }
        [[nodiscard("Store this!")]] u32 GetDepthAttachmentRendererID() const override
        {
            return 0;
        }

        [[nodiscard("Store this!")]] RHI::ResourceHandle GetColorAttachmentHandle(u32 index) const override;
        [[nodiscard("Store this!")]] RHI::ResourceHandle GetDepthAttachmentHandle() const override;

        [[nodiscard("Store this!")]] const FramebufferSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard("Store this!")]] u32 GetRendererID() const override
        {
            return 0;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }

        void AttachDepthTextureArrayLayer(RHI::ResourceHandle textureArray, u32 layer) override;

        // #691 §4 (layered shadow depth): the GL twin re-points
        // the FBO's depth attachment at ONE LAYER of a depth texture array
        // (glNamedFramebufferTextureLayer), then clears + renders a cascade
        // into it. Under dynamic rendering there is no FBO object to re-point,
        // so AttachDepthTextureArrayLayer instead SELECTS which cached
        // single-layer depth VIEW the next rendering scope opens against
        // (baseArrayLayer = the cascade, layerCount = 1). The scope-open path
        // (VulkanRendererAPI::EnsureRenderingScopeForDraw) consults this
        // override before falling back to the framebuffer's own depth
        // attachment; the layer number also rides the scope's depth barrier so
        // the layout tracker stays per-layer exact.
        struct DepthArrayLayerAttachment
        {
            VkImage Image = VK_NULL_HANDLE;
            VkImageView View = VK_NULL_HANDLE;
            VkFormat Format = VK_FORMAT_UNDEFINED;
            RHI::ResourceHandle Handle{};
            u32 Layer = 0;
            bool Active = false;
        };
        [[nodiscard]] const DepthArrayLayerAttachment& GetDepthArrayAttachment() const
        {
            return m_DepthArrayAttachment;
        }

        [[nodiscard]] Ref<VulkanTexture2D> GetColorAttachmentImage(u32 index) const;
        [[nodiscard]] u32 GetColorAttachmentCount() const
        {
            return static_cast<u32>(m_ColorAttachments.size());
        }
        [[nodiscard]] Ref<VulkanTexture2D> GetDepthAttachmentImage() const
        {
            return m_DepthAttachment;
        }

        // #691 — the raw (object-less) framebuffer facade. A
        // CreateFramebufferHandle framebuffer starts with ZERO attachments and
        // AttachFramebufferColorTexture/AttachFramebufferDepthTexture install
        // externally-owned raw-registry textures into the same
        // m_ColorAttachments/m_DepthAttachment slots the engine-owned
        // attachments use — so the rendering scope, the facade clears and the
        // blit paths see raw framebuffers with no special casing. The color
        // vector grows as needed (a gap is a null Ref — the scope's
        // VK_ATTACHMENT_UNUSED shape); a null Ref DETACHES (GL's texture-0
        // contract). The spec adopts the attached texture's extent so the
        // scope's renderArea and the DRS viewport derivation are right —
        // callers attach same-sized textures (the GL completeness rule).
        // NOTE: these framebuffers must never Resize() — CreateAttachments
        // rebuilds from the (empty) spec list and would drop every external
        // attachment. Raw callers recreate instead (immutable-storage rule).
        void AttachExternalColorTexture(u32 index, Ref<VulkanTexture2D> texture);
        // The texture must have a depth-aspect image (a D32Float/D24S8
        // facade texture); a color-format texture is refused with a warn.
        void AttachExternalDepthTexture(Ref<VulkanTexture2D> texture);

      private:
        void CreateAttachments();
        // Adopt or validate an external attachment's extent (see the
        // AttachExternal* comment): the first live attachment owns the spec
        // extent; once any other attachment is live, a mismatched extent is
        // refused (returns false) rather than silently renaming the
        // framebuffer's size under the existing attachments.
        [[nodiscard]] bool AcceptExternalExtent(const VulkanTexture2D& texture, i32 excludeColorIndex,
                                                bool excludeDepth);
        [[nodiscard]] bool HasLiveAttachmentOtherThan(i32 excludeColorIndex, bool excludeDepth) const;
        // m_HasExternalAttachments = any tracked external slot still live —
        // detaching the last one unblocks Resize again.
        void RecomputeHasExternalAttachments();

        FramebufferSpecification m_Specification;

        std::vector<FramebufferTextureSpecification> m_ColorAttachmentSpecifications;
        FramebufferTextureSpecification m_DepthAttachmentSpecification = FramebufferTextureFormat::None;

        std::vector<Ref<VulkanTexture2D>> m_ColorAttachments;
        Ref<VulkanTexture2D> m_DepthAttachment;

        // The framebuffer's OWN identity. Native = 0: under dynamic rendering
        // there is no VkFramebuffer object to name, and 0 is the honest
        // "nothing" answer for ResolveNativeForBackend. The attachments carry
        // their own (nonzero-native) handles.
        RHI::ScopedResourceHandle m_RHIHandle;

        // DRS render viewport override, same semantics as the GL twin:
        // reset to zero by Resize().
        u32 m_RenderViewportWidth = 0;
        u32 m_RenderViewportHeight = 0;

        // The selected layer (see GetDepthArrayAttachment) plus the per-layer
        // view cache that backs it, keyed by (image, layer) — a shadow pass
        // walks the same N cascades every frame, so the views are created once
        // and reused.
        //
        // The SOURCE IMAGE is stored alongside each view because these views
        // are built over an EXTERNAL array texture this framebuffer does not
        // own. Destroying them only in ~VulkanFramebuffer was wrong twice
        // over: a long-lived shadow framebuffer outlives the array across a
        // resolution change (ShadowMap::SetSettings destroys and recreates it),
        // so the views outlived their image — and once the driver recycled that
        // VkImage handle VALUE, the identical cache key handed a DEAD view to
        // vkCmdBeginRendering. ReleaseCachedDepthViewsForImage retires them at
        // the one moment that is correct: the reclaim pass, before the image
        // itself is destroyed.
        DepthArrayLayerAttachment m_DepthArrayAttachment;
        // True while AttachExternal{Color,Depth}Texture has an attachment
        // installed that this framebuffer does not own. Resize would silently
        // REPLACE the external wiring with fresh internal attachments
        // (CreateAttachments rebuilds every slot), so it refuses instead
        // (review finding, #691) — the owner re-attaches at its own
        // new size. Recomputed from the per-slot tracking below on every
        // detach, so detaching the last external attachment unblocks Resize.
        bool m_HasExternalAttachments = false;
        // Which color slots hold externally-owned attachments, plus the depth
        // twin. Needed because m_ColorAttachments mixes internal (spec-
        // created) and external slots: the flag recompute and the
        // ClearAllAttachments format decision must not confuse the two.
        std::unordered_set<u32> m_ExternalColorIndices;
        bool m_ExternalDepth = false;
        // The (image, layer) pair, kept WHOLE. It used to be packed into one
        // u64 by XORing the handle with a shifted layer, which is lossless only
        // while a VkImage's top bits are zero — true for a user-space pointer
        // handle, not guaranteed by Vulkan (a driver may hand back an index or
        // a tagged value). A collision there handed this framebuffer another
        // image's view: the wrong cascade rendered, or a view of an image that
        // was already gone. A composite key cannot collide, so no lookup needs
        // to second-guess its own hit (review finding).
        struct DepthArrayViewKey
        {
            VkImage Image = VK_NULL_HANDLE;
            u32 Layer = 0;

            [[nodiscard]] auto operator==(const DepthArrayViewKey& other) const -> bool = default;
        };
        struct DepthArrayViewKeyHash
        {
            [[nodiscard]] sizet operator()(const DepthArrayViewKey& key) const noexcept
            {
                // std::hash covers VkImage in both shapes: a pointer on 64-bit
                // builds, a u64 on 32-bit ones.
                sizet hash = std::hash<VkImage>{}(key.Image);
                hash ^= std::hash<u32>{}(key.Layer) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };
        std::unordered_map<DepthArrayViewKey, VkImageView, DepthArrayViewKeyHash> m_DepthArrayViews;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
