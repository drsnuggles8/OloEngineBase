#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanTransientResources.h — #691 Phase 5: VMA-backed resource classes for
// the TransientPool's attribute-only acquire path.
//
// TransientPool::AcquireTexture / AcquireFramebuffer / AcquireBuffer build GPU
// objects from a bare specification — no pixel upload, no bind, no sampling.
// That is exactly the slice these classes implement fully: allocation,
// identity (RHI::ResourceRegistry handles), lifetime (deferred reclaim), and
// the metadata the barrier translator needs (VulkanImageInfoRegistry). Every
// upload/bind/readback-shaped virtual is a warn-once no-op — Phase 6 territory
// (it needs the transfer/PSO paths) — never an assert, because the pool's
// acquire path must stay alive under --rhi=vulkan.
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
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Texture3D.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanImageInfoRegistry — backend-internal VkImage metadata side table.
    //
    // Maps a live VkImage to the creation attributes a barrier emitter cannot
    // recover from the handle alone. CONSUMER: the orchestrator's
    // VulkanRendererAPI reads this to derive VkImageAspectFlags for barriers
    // (color vs depth|stencil) and to seed its image-layout tracking; keep the
    // API to Register/Lookup/Unregister — it is a lookup table, not a manager.
    //
    // Thread-safety: NONE, deliberately. Registration happens in resource
    // constructors and unregistration inside VulkanDeferredReclaim's destroy
    // pass — all render-thread work, matching the rest of the Vulkan backend.
    // -------------------------------------------------------------------------
    struct VulkanImageInfo
    {
        VkFormat Format = VK_FORMAT_UNDEFINED;
        u32 MipLevels = 1;
        u32 ArrayLayers = 1;
        bool HasDepth = false;
        bool HasStencil = false;
        // The default whole-image sampled-view dimensionality. 2D for every
        // texture/attachment; 3D volumes (Texture3D — froxel fog, noise
        // fields) register VK_IMAGE_VIEW_TYPE_3D so the bind paths build the
        // right view instead of hardcoding 2D (issue #691 Phase 7 Wave B).
        VkImageViewType ViewType = VK_IMAGE_VIEW_TYPE_2D;

        // Stamped by Register() from a process-wide monotonic counter. A
        // destroyed VkImage's handle VALUE can be recycled by the driver for
        // a later image with identical extents; layout trackers key on the
        // handle and would otherwise inherit the dead image's layouts (a
        // wrong oldLayout, i.e. a validation error at best). Comparing this
        // stamp is how a tracker detects "same handle value, different
        // image" — the same recycled-name lesson as RHI handle generations.
        u64 RegistrationId = 0;

        // The layout the image sits in BEFORE the graph's layout tracker
        // first sees it. UNDEFINED for attachment/storage images (first use
        // discards); SHADER_READ_ONLY_OPTIMAL for content uploaded by a
        // load-time one-shot (#691 Phase 7) — without this, the tracker's
        // first barrier would use oldLayout = UNDEFINED and LEGALLY discard
        // the uploaded pixels. Updated via SetInitialLayout, which does NOT
        // bump RegistrationId (the image is the same image).
        VkImageLayout InitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    class VulkanImageInfoRegistry
    {
      public:
        // Process-wide instance. Deliberately leaked (never destroyed), same
        // rationale as RHI::ResourceRegistry: a Ref<Texture> released during
        // static destruction must still find a live registry.
        [[nodiscard]] static VulkanImageInfoRegistry& Get();

        void Register(VkImage image, const VulkanImageInfo& info);
        // Returns nullptr when the image was never registered (or already
        // unregistered). The pointer is invalidated by the next Register or
        // Unregister — copy out, don't hold.
        [[nodiscard]] const VulkanImageInfo* Lookup(VkImage image) const;
        void Unregister(VkImage image);

        // Record the layout an upload left the image in (see
        // VulkanImageInfo::InitialLayout). No-op for an unregistered image.
        // Only affects trackers that have NOT yet registered the image —
        // a tracker already following it keeps its own (fresher) state.
        void SetInitialLayout(VkImage image, VkImageLayout layout);

      private:
        VulkanImageInfoRegistry() = default;

        std::unordered_map<VkImage, VulkanImageInfo> m_Infos;
    };

    // -------------------------------------------------------------------------
    // VulkanDeferredReclaim — deferred-destroy queue for VMA allocations.
    //
    // WHY: TransientPool::Clear()/Trim() destroy pooled GPU objects while prior
    // frames may still be executing on the GPU. On GL the driver refcounts the
    // object behind the name, so an in-flight delete is safe; on Vulkan,
    // destroying a resource a submitted command buffer still references is
    // undefined behavior. So no Vulkan resource class ever calls
    // vmaDestroyImage/vmaDestroyBuffer inline — destruction enqueues here and
    // the actual destroy happens once the GPU is provably past the frame that
    // could have referenced the object.
    //
    // The wait is counted in GENERATIONS, not clocks: the frame loop calls
    // NotifyFrameCompleted() once per completed frame, and an entry is
    // destroyed once >= kFramesInFlight (2, matching VulkanContext's
    // frames-in-flight shape) notifications have passed since it was enqueued.
    // FlushAll() is the shutdown/device-idle path: the CALLER guarantees
    // vkDeviceWaitIdle has already been done, and everything is destroyed
    // immediately.
    //
    // Thread-safety: NONE, deliberately — render thread only, like the rest of
    // the backend.
    // -------------------------------------------------------------------------
    class VulkanDeferredReclaim
    {
      public:
        // Process-wide instance, deliberately leaked (see
        // VulkanImageInfoRegistry::Get for the rationale).
        [[nodiscard]] static VulkanDeferredReclaim& Get();

        void Enqueue(VkImage image, VmaAllocation allocation);
        void Enqueue(VkBuffer buffer, VmaAllocation allocation);
        // Phase 6: non-VMA device objects share the same generation discipline.
        // A semaphore may be referenced by an in-flight submit's wait/signal
        // list; a pipeline by an in-flight command buffer (ADR 0011 §3(d) —
        // hot-reload destruction is deferred, never inline).
        void Enqueue(VkSemaphore semaphore);
        void Enqueue(VkPipeline pipeline);
        // Phase 7: attachment views (vkCmdBeginRendering references them from
        // in-flight command buffers exactly like pipelines).
        void Enqueue(VkImageView view);

        // Called by the frame loop once per completed frame. Destroys every
        // entry enqueued >= 2 notifications ago; also unregisters images from
        // VulkanImageInfoRegistry at actual-destroy time.
        void NotifyFrameCompleted();

        // Destroy everything immediately. Caller guarantees device idle
        // (vkDeviceWaitIdle already done — shutdown, swapchain teardown).
        void FlushAll();

        // Diagnostic/test affordance.
        [[nodiscard]] sizet GetPendingCount() const
        {
            return m_Entries.size();
        }

      private:
        VulkanDeferredReclaim() = default;

        struct Entry
        {
            VkImage Image = VK_NULL_HANDLE; // exactly one of Image/Buffer/Semaphore/Pipeline/View is set
            VkBuffer Buffer = VK_NULL_HANDLE;
            VkSemaphore Semaphore = VK_NULL_HANDLE;
            VkPipeline Pipeline = VK_NULL_HANDLE;
            VkImageView View = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE; // set only for Image/Buffer entries
            u64 EnqueuedAtGeneration = 0;
        };

        // Destroys one entry through the live device's allocator. When the
        // device is already gone (shutdown teardown races) the entry is
        // dropped with a warn log — leaking at process exit beats calling
        // into a destroyed allocator.
        static void DestroyEntry(const Entry& entry);

        // Matches VulkanContextData::kFramesInFlight.
        static constexpr u64 kFramesInFlight = 2;

        std::vector<Entry> m_Entries;
        u64 m_Generation = 0;
    };

    // -------------------------------------------------------------------------
    // VulkanTexture2D — attribute-only VMA image for the TransientPool.
    //
    // Fully implemented: allocation, identity, metadata, Resize. Upload /
    // bind / readback virtuals are warn-once no-ops until Phase 6.
    // -------------------------------------------------------------------------
    class VulkanTexture2D : public Texture2D
    {
      public:
        explicit VulkanTexture2D(const TextureSpecification& specification);
        // File load (#691 Phase 7): stbi with the SAME thread-local vertical
        // flip the GL twin uses — asset bytes must be identical across
        // backends, since UV sampling is convention-free.
        VulkanTexture2D(const std::string& path, bool srgb);
        ~VulkanTexture2D() override;

        const TextureSpecification& GetSpecification() const override
        {
            return m_Specification;
        }

        [[nodiscard("Store this!")]] u32 GetWidth() const override
        {
            return m_Width;
        }
        [[nodiscard("Store this!")]] u32 GetHeight() const override
        {
            return m_Height;
        }
        // Diagnostics-only field: a native GL name does not exist here.
        [[nodiscard("Store this!")]] u32 GetRendererID() const override
        {
            return 0;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard("Store this!")]] const std::string& GetPath() const override
        {
            return m_Path;
        }

        // Real upload/readback paths (#691 Phase 7): one-shot staged copies,
        // mip generation via a blit chain, final layout SHADER_READ_ONLY
        // recorded through VulkanImageInfoRegistry::SetInitialLayout.
        void SetData(void* data, u32 size) override;
        void SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, u32 dataSize) override;
        void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;
        // Bind is meaningless on this backend (heap-bindless): warn-once no-op.
        void Bind(u32 slot) const override;
        bool GetData(std::vector<u8>& outData, u32 mipLevel = 0) const override;

        [[nodiscard("Store this!")]] bool IsLoaded() const override
        {
            return m_IsLoaded;
        }

        [[nodiscard("Use for transparency")]] bool HasAlphaChannel() const override
        {
            return m_Specification.Format == ImageFormat::RGBA8 ||
                   m_Specification.Format == ImageFormat::RGBA16F ||
                   m_Specification.Format == ImageFormat::RGBA32F ||
                   m_Specification.Format == ImageFormat::BC7;
        }

        [[nodiscard("Store this!")]] u32 GetMipLevelCount() const override
        {
            return m_MipLevels;
        }

        // Recreates the VMA image at the new size (old image goes through
        // VulkanDeferredReclaim). Identity is PRESERVED via m_RHIHandle.Sync,
        // matching the GL twin's recreate-in-place semantics.
        void Resize(u32 width, u32 height) override;

        [[nodiscard]] VkImage GetVkImage() const
        {
            return m_Image;
        }

        // Lazily-created whole-image VkImageView for dynamic-rendering
        // attachment use (the ONE place view objects still exist on this
        // backend — sampled use goes through descriptor-heap view
        // DESCRIPTIONS). Cached; released with the image (Resize mints a new
        // one). VK_NULL_HANDLE on failure.
        [[nodiscard]] VkImageView GetOrCreateAttachmentView();

      private:
        void CreateImage();
        void ReleaseImage();
        // Full-image base-level upload + optional blit-chain mip generation,
        // leaving every mip in SHADER_READ_ONLY_OPTIMAL. `data` is tightly
        // packed rows in the spec's format.
        bool UploadPixels(const void* data, u64 sizeBytes);

        TextureSpecification m_Specification;
        std::string m_Path; // set by the file ctor; empty for transient/spec textures
        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_MipLevels = 1;
        bool m_IsLoaded = false;

        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        VkImageView m_AttachmentView = VK_NULL_HANDLE; ///< See GetOrCreateAttachmentView.
        // Generation-checked identity for m_Image, kept in lockstep by
        // m_RHIHandle.Sync at every site that assigns the native handle —
        // same pattern as the GL twin (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
    };

    // -------------------------------------------------------------------------
    // VulkanTexture3D — the Texture3D backend twin (issue #691 Phase 7
    // Wave B: froxel-fog volumes, 3D noise fields). Sampled (sampler3D) +
    // storage (image3D) usage in one image; registers
    // VK_IMAGE_VIEW_TYPE_3D in VulkanImageInfoRegistry so both bind paths
    // build 3D views instead of the 2D default.
    // -------------------------------------------------------------------------
    class VulkanTexture3D : public Texture3D
    {
      public:
        explicit VulkanTexture3D(const Texture3DSpecification& spec);
        ~VulkanTexture3D() override;

        [[nodiscard]] u32 GetWidth() const override
        {
            return m_Specification.Width;
        }
        [[nodiscard]] u32 GetHeight() const override
        {
            return m_Specification.Height;
        }
        [[nodiscard]] u32 GetDepth() const override
        {
            return m_Specification.Depth;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0; // no GL name exists; identity is the RHI handle
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const Texture3DSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard]] VkImage GetVkImage() const
        {
            return m_Image;
        }

        void Bind(u32 slot) const override;

      private:
        Texture3DSpecification m_Specification;
        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        RHI::ScopedResourceHandle m_RHIHandle;
    };

    // -------------------------------------------------------------------------
    // VulkanFramebuffer — a bag of attachment images, no VkFramebuffer.
    //
    // The Vulkan backend targets dynamic rendering (render passes are Phase 6),
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

        // Will become meaningful when the orchestrator's VulkanRendererAPI
        // current-render-target state lands — warn-once no-ops until then.
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

        // Readback / clear-shaped — Phase 6 concerns, warn-once no-ops.
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

        [[nodiscard]] Ref<VulkanTexture2D> GetColorAttachmentImage(u32 index) const;
        [[nodiscard]] u32 GetColorAttachmentCount() const
        {
            return static_cast<u32>(m_ColorAttachments.size());
        }
        [[nodiscard]] Ref<VulkanTexture2D> GetDepthAttachmentImage() const
        {
            return m_DepthAttachment;
        }

      private:
        void CreateAttachments();

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
    };

    // -------------------------------------------------------------------------
    // VulkanStorageBuffer — attribute-only VMA buffer for the TransientPool.
    // -------------------------------------------------------------------------
    class VulkanStorageBuffer : public StorageBuffer
    {
      public:
        VulkanStorageBuffer(u32 size, u32 binding, StorageBufferUsage usage = StorageBufferUsage::DynamicDraw);
        ~VulkanStorageBuffer() override;

        // Bind is meaningless on this backend (buffers travel as device
        // addresses in root data): silent no-ops.
        void Bind() const override;
        void Unbind() const override;
        // Real paths (#691 Phase 7): mapped write-through (BAR/UMA) or a
        // staged one-shot copy; readback via a one-shot copy to host memory.
        void SetData(const void* data, u32 size, u32 offset = 0) override;
        void GetData(void* outData, u32 size, u32 offset = 0) const override;
        void ClearData() override;

        // Recreates the VMA buffer at the new size (old buffer goes through
        // VulkanDeferredReclaim). Identity preserved via m_RHIHandle.Sync.
        void Resize(u32 newSize) override;

        // Diagnostics-only field: a native GL name does not exist here.
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] u32 GetSize() const override
        {
            return m_Size;
        }
        [[nodiscard]] u32 GetBinding() const override
        {
            return m_Binding;
        }

        [[nodiscard]] VkBuffer GetVkBuffer() const
        {
            return m_Buffer;
        }
        // The address the root-data writer embeds (ADR 0011 §4). Stable for
        // the buffer's life; Resize mints a new one.
        [[nodiscard]] VkDeviceAddress GetDeviceAddress() const
        {
            return m_DeviceAddress;
        }

      private:
        void CreateBuffer();
        void ReleaseBuffer();

        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        void* m_Mapped = nullptr; ///< Non-null when VMA gave a host-visible placement.
        bool m_NeedsFlush = false;
        VkDeviceAddress m_DeviceAddress = 0;
        // Generation-checked identity for m_Buffer, kept in lockstep by
        // m_RHIHandle.Sync — same pattern as the GL twin (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        u32 m_Size = 0;
        u32 m_Binding = 0;
        StorageBufferUsage m_Usage = StorageBufferUsage::DynamicDraw;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
