#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanFramebuffer.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <algorithm>
#include <unordered_set>

namespace OloEngine
{
    namespace
    {
        // FramebufferTextureFormat -> ImageFormat, so VulkanFramebuffer can
        // build its attachments as real VulkanTexture2D instances (handles +
        // VulkanImageInfoRegistry entries come for free). Widening choices:
        //  - RGB16F -> RGBA16F: ImageFormat has no RGB16F member, and the
        //    Vulkan map would widen a 3-channel format anyway (see
        //    ImageFormatToVkFormat).
        //  - DEPTH_COMPONENT32F -> DEPTH24STENCIL8: ImageFormat has no
        //    stencil-free depth member; the Vulkan map allocates
        //    D32_SFLOAT_S8_UINT for it, so a shadow depth attachment keeps its
        //    32-bit float depth aspect — the surplus stencil aspect is unused.
        // (Depth/ShadowDepth are enumerator aliases of the two depth members,
        // so this switch covers every distinct value.)
        [[nodiscard]] ImageFormat FramebufferFormatToImageFormat(FramebufferTextureFormat format)
        {
            switch (format)
            {
                case FramebufferTextureFormat::None:
                    break;
                case FramebufferTextureFormat::RGBA8:
                    return ImageFormat::RGBA8;
                case FramebufferTextureFormat::RGBA16F:
                    return ImageFormat::RGBA16F;
                case FramebufferTextureFormat::RGBA32F:
                    return ImageFormat::RGBA32F;
                case FramebufferTextureFormat::RGB16F:
                    return ImageFormat::RGBA16F;
                case FramebufferTextureFormat::RGB32F:
                    return ImageFormat::RGB32F;
                case FramebufferTextureFormat::RG16F:
                    return ImageFormat::RG16F;
                case FramebufferTextureFormat::RG32F:
                    return ImageFormat::RG32F;
                case FramebufferTextureFormat::RED_INTEGER:
                    return ImageFormat::R32I;
                case FramebufferTextureFormat::DEPTH24STENCIL8:
                    return ImageFormat::DEPTH24STENCIL8;
                case FramebufferTextureFormat::DEPTH_COMPONENT32F:
                    return ImageFormat::DEPTH24STENCIL8;
            }

            OLO_CORE_ASSERT(false, "FramebufferFormatToImageFormat: unknown FramebufferTextureFormat {}",
                            static_cast<u32>(format));
            return ImageFormat::None;
        }

        [[nodiscard]] bool IsDepthFramebufferFormat(FramebufferTextureFormat format)
        {
            return format == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                   format == FramebufferTextureFormat::DEPTH_COMPONENT32F;
        }

        // Every live VulkanFramebuffer, so the reclaim pass can reach the
        // per-cascade depth views they cache over EXTERNAL array images.
        // Deliberately leaked, same rationale as this backend's other
        // process-wide registries (a framebuffer released during static
        // teardown must still find a live set).
        std::unordered_set<VulkanFramebuffer*>& LiveFramebuffers()
        {
            static auto* s_Live = new std::unordered_set<VulkanFramebuffer*>();
            return *s_Live;
        }
    } // namespace

    void VulkanFramebuffer::ReleaseCachedDepthViewsForImage(const VkImage image)
    {
        if (image == VK_NULL_HANDLE)
            return;
        auto* device = VulkanDevice::Get();
        for (auto* framebuffer : LiveFramebuffers())
        {
            auto& cache = framebuffer->m_DepthArrayViews;
            for (auto it = cache.begin(); it != cache.end();)
            {
                if (it->second.SourceImage != image)
                {
                    ++it;
                    continue;
                }
                // Destroyed INLINE, not enqueued. This runs from
                // VulkanDeferredReclaim::DestroyEntry, which is itself inside
                // an erase_if over the queue's own entry vector — enqueueing
                // here would reallocate the container being iterated. Inline is
                // also simply correct: the queue has already waited
                // kFramesInFlight generations past this image's last use, so no
                // in-flight frame can still reference a view of it.
                if (it->second.View != VK_NULL_HANDLE && device != nullptr)
                    vkDestroyImageView(device->GetDevice(), it->second.View, nullptr);
                // The current selection may name the view being retired; drop
                // it so a later scope cannot attach a destroyed view.
                if (framebuffer->m_DepthArrayAttachment.View == it->second.View)
                    framebuffer->m_DepthArrayAttachment = DepthArrayLayerAttachment{};
                it = cache.erase(it);
            }
        }
    }

    VulkanFramebuffer::VulkanFramebuffer(const FramebufferSpecification& spec)
        : m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();
        LiveFramebuffers().insert(this);
        OLO_CORE_ASSERT(VulkanDevice::Get() != nullptr, "VulkanFramebuffer requires a live VulkanDevice");

        for (const auto& attachmentSpec : m_Specification.Attachments.Attachments)
        {
            if (IsDepthFramebufferFormat(attachmentSpec.TextureFormat))
            {
                m_DepthAttachmentSpecification = attachmentSpec;
            }
            else if (attachmentSpec.TextureFormat != FramebufferTextureFormat::None)
            {
                m_ColorAttachmentSpecifications.push_back(attachmentSpec);
            }
        }

        CreateAttachments();

        // The framebuffer's own identity: native = 0 because under dynamic
        // rendering no VkFramebuffer object exists to name (render passes are
        // Phase 6). The attachments carry their own nonzero-native handles.
        m_RHIHandle.Adopt(RHI::ResourceKind::Framebuffer, 0u, RHI::Backend::Vulkan);
        // Raw-handle framebuffer ops (ClearFramebuffer* / BlitFramebuffer /
        // the per-FB draw-attachment selection, #691 Phase 7 Wave C) receive
        // only this handle and need the OBJECT back — the native is 0, so the
        // root-object side table is the resolve path, exactly as for VAOs.
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::Framebuffer, this);
    }

    VulkanFramebuffer::~VulkanFramebuffer()
    {
        // API-side state must not outlive the object: end a scope targeting
        // this framebuffer and drop any pending lazy clear naming it (a later
        // materialization would dereference the freed object). Live-object
        // probe, the ClearData rule.
        if (auto* vk = TryGetVulkanAPI(); vk != nullptr)
        {
            vk->NotifyFramebufferDestroyed(this, m_RHIHandle.Get());
        }
        VulkanBindingState::Get().ClearIfCurrentFramebuffer(this);
        VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
        // The per-cascade depth views (AttachDepthTextureArrayLayer) are owned
        // here, not by the array texture — they outlive no frame of their own,
        // so they retire on the deferred queue like every other view.
        for (const auto& [key, cached] : m_DepthArrayViews)
        {
            if (cached.View != VK_NULL_HANDLE)
                VulkanDeferredReclaim::Get().Enqueue(cached.View);
        }
        m_DepthArrayViews.clear();
        LiveFramebuffers().erase(this);
        m_DepthArrayAttachment = DepthArrayLayerAttachment{};
        // Retire the framebuffer identity; the attachment Refs release next
        // and each texture enqueues its image on VulkanDeferredReclaim.
        m_RHIHandle.Reset();
    }

    void VulkanFramebuffer::CreateAttachments()
    {
        // Replacing the Refs drops the old attachments — their destructors
        // route the images through VulkanDeferredReclaim, never an inline
        // destroy.
        m_ColorAttachments.clear();
        m_DepthAttachment = nullptr;

        // A 0-sized spec is legal in the engine (framebuffers are routinely
        // resized before first use); Vulkan refuses a zero extent, so the
        // attachment textures clamp to 1x1 until Resize provides real
        // dimensions. m_Specification keeps the authored values.
        const u32 width = std::max(m_Specification.Width, 1u);
        const u32 height = std::max(m_Specification.Height, 1u);

        const auto makeAttachmentSpec = [&](FramebufferTextureFormat format)
        {
            TextureSpecification texSpec;
            texSpec.Width = width;
            texSpec.Height = height;
            texSpec.Format = FramebufferFormatToImageFormat(format);
            texSpec.GenerateMips = false;
            texSpec.MipLevels = 1u;
            texSpec.Samples = std::max(m_Specification.Samples, 1u);
            return texSpec;
        };

        // GL's PrepareTexture (OpenGLUtilities.cpp) stamps every framebuffer
        // attachment CLAMP_TO_EDGE + LINEAR — different from a plain
        // OpenGLTexture2D's REPEAT — and the inherit sampler path (#691
        // Phase 8) reproduces whatever the creator stamped. Without this, a
        // post-process read past uv 1.0 wraps to the far side of the frame
        // (the chromatic-aberration tenant's white edge sampled the black
        // left border the moment inherit landed).
        const auto stampAttachmentSamplerState = [](const Ref<VulkanTexture2D>& attachment)
        {
            auto& registry = VulkanImageInfoRegistry::Get();
            registry.SetSamplerFilter(attachment->GetVkImage(), VK_FILTER_LINEAR, VK_FILTER_LINEAR);
            registry.SetSamplerAddressMode(attachment->GetVkImage(), VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        };

        m_ColorAttachments.reserve(m_ColorAttachmentSpecifications.size());
        for (const auto& attachmentSpec : m_ColorAttachmentSpecifications)
        {
            m_ColorAttachments.push_back(Ref<VulkanTexture2D>::Create(makeAttachmentSpec(attachmentSpec.TextureFormat)));
            stampAttachmentSamplerState(m_ColorAttachments.back());
        }

        if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
        {
            m_DepthAttachment = Ref<VulkanTexture2D>::Create(makeAttachmentSpec(m_DepthAttachmentSpecification.TextureFormat));
            stampAttachmentSamplerState(m_DepthAttachment);
        }
    }

    void VulkanFramebuffer::Bind()
    {
        // Publish as the current render target. The dynamic-rendering scope
        // (VulkanRendererAPI) consumes this LAZILY at the next draw/clear —
        // nothing is recorded here, matching how GL passes freely interleave
        // binds with state calls.
        VulkanBindingState::Get().SetCurrentFramebuffer(this);
    }

    void VulkanFramebuffer::Unbind()
    {
        auto& state = VulkanBindingState::Get();
        if (state.GetCurrentFramebuffer() == this)
        {
            state.SetCurrentFramebuffer(nullptr);
        }
    }

    void VulkanFramebuffer::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0u || height == 0u)
        {
            OLO_CORE_WARN("VulkanFramebuffer::Resize: ignoring zero extent {}x{}", width, height);
            return;
        }
        if (m_HasExternalAttachments)
        {
            // CreateAttachments would replace the externally-owned wiring
            // with fresh internal textures — the owner must re-attach at its
            // own new size instead.
            OLO_CORE_WARN("VulkanFramebuffer::Resize: framebuffer holds EXTERNAL attachments — refusing the "
                          "rebuild; re-attach at the new size from the owner");
            return;
        }

        m_Specification.Width = width;
        m_Specification.Height = height;

        // Physical resize implies render viewport == physical size (same
        // contract as the GL twin).
        m_RenderViewportWidth = 0u;
        m_RenderViewportHeight = 0u;

        // The attachments genuinely become NEW objects with NEW handles —
        // anything still holding the old ones has to see them go stale (same
        // contract as the GL twin). The old images auto-enqueue on
        // VulkanDeferredReclaim via their destructors.
        CreateAttachments();
    }

    void VulkanFramebuffer::SetRenderViewportSize(u32 width, u32 height)
    {
        m_RenderViewportWidth = width;
        m_RenderViewportHeight = height;
    }

    int VulkanFramebuffer::ReadPixel(u32 attachmentIndex, int x, int y)
    {
        // glReadPixels(GL_RED_INTEGER, GL_INT) of one texel, on the
        // ReadTextureSubImage spine (#691 Phase 8b; coordinate contract
        // re-derived in Phase 9, ADR 0011 amendment (85)): every Vulkan
        // off-screen target is TOP-DOWN, so the caller hands TOP-DOWN
        // coordinates (mouse/viewport y, unconverted) and they address texel
        // rows verbatim — the GL arm is the one that converts, at the caller
        // (EditorLayer keys its mouse-origin flip on
        // ImGuiLayer::RenderTargetRowsAreBottomUp). The two sites move
        // together or picking silently selects the vertically mirrored
        // entity.
        if (attachmentIndex >= m_ColorAttachments.size() || m_ColorAttachments[attachmentIndex] == nullptr)
        {
            return -1;
        }
        const u32 height = m_Specification.Height;
        if (x < 0 || y < 0 || static_cast<u32>(x) >= m_Specification.Width || static_cast<u32>(y) >= height)
        {
            return -1;
        }
        int value = -1;
        if (!RenderCommand::GetRendererAPI().ReadTextureSubImage(
                m_ColorAttachments[attachmentIndex]->GetRHIHandle(), 0, x, y, 0, 1u, 1u, 1u,
                RHI::Format::R32Int, sizeof(value), &value))
        {
            return -1; // the entity-picking "nothing here" value
        }
        return value;
    }

    void VulkanFramebuffer::ClearAttachment(u32 attachmentIndex, int value)
    {
        // Single-attachment integer clear (the entity-ID -1 wipe) — the
        // per-attachment slice of ClearAllAttachments, riding the same facade
        // transfer clear (#691 Phase 8: last ClearAttachment stub retired).
        if (attachmentIndex >= m_ColorAttachments.size() || m_ColorAttachments[attachmentIndex] == nullptr)
        {
            return;
        }
        RenderCommand::ClearTextureUInt(m_ColorAttachments[attachmentIndex]->GetRHIHandle(), 0u,
                                        static_cast<u32>(value));
    }

    void VulkanFramebuffer::ClearAttachment(u32 attachmentIndex, const glm::vec4& value)
    {
        // Single-attachment float clear — same shape as the int form above.
        if (attachmentIndex >= m_ColorAttachments.size() || m_ColorAttachments[attachmentIndex] == nullptr)
        {
            return;
        }
        RenderCommand::ClearTextureFloat(m_ColorAttachments[attachmentIndex]->GetRHIHandle(), 0u, value);
    }

    void VulkanFramebuffer::ClearAllAttachments(const glm::vec4& clearColor, int entityIdClear)
    {
        // GL-parity semantics (OpenGLFramebuffer::ClearAllAttachments): every
        // float colour attachment clears to `clearColor`, every RED_INTEGER
        // attachment to `entityIdClear`, the depth attachment to 1.0 (stencil
        // 0). Routed through the facade's transfer clears — each resolves the
        // attachment via the registries, ends the rendering scope first, and
        // issues exact per-layout-run transitions through the layout tracker
        // (the ClearTextureFloat/UInt shape), so the tracker stays true for
        // whatever samples or renders these attachments next (#691 Phase 7
        // Wave A — UICompositePass's mixed int/float clear is the first
        // caller on this backend).
        for (sizet i = 0; i < m_ColorAttachments.size(); ++i)
        {
            if (!m_ColorAttachments[i])
                continue;
            const RHI::ResourceHandle attachment = m_ColorAttachments[i]->GetRHIHandle();
            // Spec-created slots carry their format in the spec list, but an
            // EXTERNAL slot may sit past it (raw framebuffers have an empty
            // spec) or shadow a spec entry with a different format — decide
            // int-vs-float from the attached texture itself there, because a
            // float clear on an integer image is a validation error (review
            // finding, #691 Phase 8).
            const bool isExternal = m_ExternalColorIndices.contains(static_cast<u32>(i));
            const bool isInteger =
                (!isExternal && i < m_ColorAttachmentSpecifications.size())
                    ? m_ColorAttachmentSpecifications[i].TextureFormat == FramebufferTextureFormat::RED_INTEGER
                    : IsIntegerFormat(m_ColorAttachments[i]->GetSpecification().Format);
            if (isInteger)
            {
                // vkCmdClearColorImage on an SINT image reads the int32 union
                // lanes; the uint clear writes the same bit pattern, so the
                // cast is bit-exact for the -1 sentinel and every other id.
                RenderCommand::ClearTextureUInt(attachment, 0u, static_cast<u32>(entityIdClear));
            }
            else
            {
                RenderCommand::ClearTextureFloat(attachment, 0u, clearColor);
            }
        }

        if (m_DepthAttachment)
        {
            // ClearTextureFloat's depth path clears depth = color.r, stencil 0
            // — the GL twin's glClearDepth(1.0) / glClearStencil(0) pair.
            RenderCommand::ClearTextureFloat(m_DepthAttachment->GetRHIHandle(), 0u,
                                             glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        }
    }

    RHI::ResourceHandle VulkanFramebuffer::GetColorAttachmentHandle(u32 index) const
    {
        OLO_CORE_ASSERT(index < m_ColorAttachments.size());
        if (index >= m_ColorAttachments.size())
        {
            return {};
        }
        // A null slot is legal on raw framebuffers (a detached or never-
        // attached index below a higher attached one) — "no identity", not a
        // crash.
        if (m_ColorAttachments[index] == nullptr)
        {
            return {};
        }
        return m_ColorAttachments[index]->GetRHIHandle();
    }

    RHI::ResourceHandle VulkanFramebuffer::GetDepthAttachmentHandle() const
    {
        return m_DepthAttachment ? m_DepthAttachment->GetRHIHandle() : RHI::ResourceHandle{};
    }

    void VulkanFramebuffer::AttachDepthTextureArrayLayer(RHI::ResourceHandle textureArray, u32 layer)
    {
        OLO_PROFILE_FUNCTION();

        // See the DepthArrayLayerAttachment comment in the header: this does
        // not RE-POINT anything (there is no VkFramebuffer under dynamic
        // rendering) — it selects the single-layer depth view the NEXT
        // rendering scope opens against. The scope currently open on this
        // framebuffer still holds the PREVIOUS layer's view; ending it is the
        // backend's business, not this setter's, and VulkanRendererAPI does it
        // by comparing the live scope's recorded selection against this one
        // (ScopeMatchesCurrentTarget) — the same shape as the "the bound
        // framebuffer changed" guard in Clear()/ClearDepthOnly(). Without that
        // comparison every cascade would render into cascade 0's view, which
        // is exactly the failure the layered-depth tenant pins.
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: no live device");
            return;
        }

        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(textureArray);
        if (native == 0)
        {
            OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: unresolvable texture-array handle");
            m_DepthArrayAttachment = DepthArrayLayerAttachment{};
            return;
        }
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr)
        {
            OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: image not in the info registry");
            m_DepthArrayAttachment = DepthArrayLayerAttachment{};
            return;
        }
        if (layer >= std::max(info->ArrayLayers, 1u))
        {
            OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: layer {} >= arrayLayers {}", layer,
                           info->ArrayLayers);
            m_DepthArrayAttachment = DepthArrayLayerAttachment{};
            return;
        }

        const u64 cacheKey = VkHandleToU64(image) ^ (static_cast<u64>(layer + 1u) << 48u);
        VkImageView view = VK_NULL_HANDLE;
        if (const auto it = m_DepthArrayViews.find(cacheKey); it != m_DepthArrayViews.end())
        {
            view = it->second.View;
        }
        else
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image;
            // A single-layer view of an array image: 2D, not 2D_ARRAY — a
            // depth attachment renders into exactly one layer and the
            // rendering info's layerCount stays 1 (no multiview here).
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = info->Format;
            viewInfo.subresourceRange.aspectMask =
                info->HasDepth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (info->HasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u))
                               : VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = layer;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device->GetDevice(), &viewInfo, nullptr, &view) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: view creation failed (layer {})",
                               layer);
                m_DepthArrayAttachment = DepthArrayLayerAttachment{};
                return;
            }
            m_DepthArrayViews.emplace(cacheKey, CachedDepthArrayView{ .View = view, .SourceImage = image });
        }

        m_DepthArrayAttachment.Image = image;
        m_DepthArrayAttachment.View = view;
        m_DepthArrayAttachment.Format = info->Format;
        m_DepthArrayAttachment.Handle = textureArray;
        m_DepthArrayAttachment.Layer = layer;
        m_DepthArrayAttachment.Active = true;
    }

    Ref<VulkanTexture2D> VulkanFramebuffer::GetColorAttachmentImage(u32 index) const
    {
        OLO_CORE_ASSERT(index < m_ColorAttachments.size());
        if (index >= m_ColorAttachments.size())
        {
            return nullptr;
        }
        return m_ColorAttachments[index];
    }

    bool VulkanFramebuffer::HasLiveAttachmentOtherThan(i32 excludeColorIndex, bool excludeDepth) const
    {
        for (sizet i = 0; i < m_ColorAttachments.size(); ++i)
        {
            if (static_cast<i32>(i) != excludeColorIndex && m_ColorAttachments[i] != nullptr)
            {
                return true;
            }
        }
        return !excludeDepth && m_DepthAttachment != nullptr;
    }

    void VulkanFramebuffer::RecomputeHasExternalAttachments()
    {
        m_HasExternalAttachments = m_ExternalDepth || !m_ExternalColorIndices.empty();
    }

    bool VulkanFramebuffer::AcceptExternalExtent(const VulkanTexture2D& texture, i32 excludeColorIndex,
                                                 bool excludeDepth)
    {
        // Raw callers attach same-sized textures (the GL completeness rule).
        // The first live attachment owns the spec extent — a raw framebuffer
        // is born 0x0, and after a detach-all the owner may legitimately
        // re-attach at a new size. Once any OTHER attachment is live, a
        // mismatched extent is a caller bug: refuse it rather than silently
        // renaming the framebuffer's size under the existing attachments,
        // which would skew the scope's renderArea for all of them (review
        // finding, #691 Phase 8).
        const u32 width = texture.GetWidth();
        const u32 height = texture.GetHeight();
        if (m_Specification.Width != 0u && m_Specification.Height != 0u &&
            (m_Specification.Width != width || m_Specification.Height != height) &&
            HasLiveAttachmentOtherThan(excludeColorIndex, excludeDepth))
        {
            OLO_CORE_WARN("[RHI/Vulkan] AttachExternal*: {}x{} texture does not match the framebuffer's "
                          "{}x{} live attachments — attach refused",
                          width, height, m_Specification.Width, m_Specification.Height);
            return false;
        }
        m_Specification.Width = width;
        m_Specification.Height = height;
        return true;
    }

    void VulkanFramebuffer::AttachExternalColorTexture(u32 index, Ref<VulkanTexture2D> texture)
    {
        if (texture == nullptr)
        {
            // Detach (GL's texture-0 form). An index that was never attached
            // needs no slot minted for it.
            if (index < m_ColorAttachments.size())
            {
                m_ColorAttachments[index] = nullptr;
            }
            m_ExternalColorIndices.erase(index);
            RecomputeHasExternalAttachments();
            return;
        }
        // The rendering scope opens color attachments COLOR_OPTIMAL with a
        // color-aspect view — a depth-format image there is a validation
        // error, so refuse it here where the mistake is nameable (the mirror
        // of AttachExternalDepthTexture's no-depth-aspect refusal).
        if (const auto* info = VulkanImageInfoRegistry::Get().Lookup(texture->GetVkImage());
            info != nullptr && info->HasDepth)
        {
            OLO_CORE_WARN("[RHI/Vulkan] AttachExternalColorTexture: texture has a depth aspect — attach refused");
            return;
        }
        if (!AcceptExternalExtent(*texture, static_cast<i32>(index), /*excludeDepth*/ false))
        {
            return;
        }
        if (index >= m_ColorAttachments.size())
        {
            // Growth leaves null gaps below `index` — the scope's
            // VK_ATTACHMENT_UNUSED shape, and IsFramebufferComplete only
            // requires the NON-null attachments to be live.
            m_ColorAttachments.resize(static_cast<sizet>(index) + 1u);
        }
        m_ColorAttachments[index] = std::move(texture);
        m_ExternalColorIndices.insert(index);
        m_HasExternalAttachments = true;
    }

    void VulkanFramebuffer::AttachExternalDepthTexture(Ref<VulkanTexture2D> texture)
    {
        if (texture == nullptr)
        {
            m_DepthAttachment = nullptr;
            m_ExternalDepth = false;
            RecomputeHasExternalAttachments();
            return;
        }
        // The rendering scope opens this attachment DEPTH_STENCIL_OPTIMAL
        // with a depth-aspect view — a color-format image there is a
        // validation error, so refuse it here where the mistake is nameable.
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(texture->GetVkImage());
        if (info == nullptr || !info->HasDepth)
        {
            OLO_CORE_WARN("[RHI/Vulkan] AttachExternalDepthTexture: texture has no depth aspect — attach refused");
            return;
        }
        if (!AcceptExternalExtent(*texture, /*excludeColorIndex*/ -1, /*excludeDepth*/ true))
        {
            return;
        }
        m_DepthAttachment = std::move(texture);
        m_ExternalDepth = true;
        m_HasExternalAttachments = true;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
