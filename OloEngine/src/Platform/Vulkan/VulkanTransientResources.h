#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanTransientResources.h — VMA-backed resource classes for the Vulkan
// backend (#691; grown from the Phase 5 attribute-only acquire slice to the
// full Phase 8 surface).
//
// What lives here now: the texture family (VulkanTexture2D / arrays /
// cubemaps / cubemap arrays / 3D volumes) with real uploads (staged into the
// frame command buffer, one-shot outside a bracket), mip generation,
// readbacks (GetData / GetFaceData / ReadPixel on the ReadTextureSubImage
// spine), sampler-state registry metadata; VulkanFramebuffer (attachments,
// external attach, per-layer depth views, resize semantics matching the GL
// twin); the raw-handle registries backing the Create*/Delete*/Attach*
// facade family; the image-info/layout registries the barrier translator
// reads; and VulkanDeferredReclaim (generation-waited destruction). A split
// into per-class file pairs is planned follow-up — the classes are already
// independent; only the shared anonymous-namespace helpers keep them in one
// TU today.
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
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/TextureCubemapArray.h"
#include "OloEngine/Renderer/Texture3D.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
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
        // Mip-0 extent (#691 Phase 8). GetTextureDimensions is the consumer:
        // GL answers it from glGetTextureLevelParameteriv, and the handle
        // alone cannot recover an extent here. 0 = "registered before this
        // field existed / extent unknown" and the facade reports the query
        // unanswerable rather than guessing.
        u32 Width = 0;
        u32 Height = 0;
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

        // The image's OWN sampler state (#691 Phase 8) — GL keeps filter and
        // wrap on the texture object, so the "inherit" half of the sampler
        // contract (rhi-abstraction-boundary.md §4f: no stated intent means
        // the object's state, parity by construction) needs somewhere
        // API-neutral callers can't see to carry it. Registered with the
        // per-class GL defaults (§4f's table: Texture2D/attachments REPEAT,
        // arrays and cubes CLAMP_TO_EDGE, depth arrays CLAMP_TO_BORDER
        // opaque-white) and mutated by SetTextureFilter / SetTextureWrap.
        // BindTexture derives the effective VkSamplerCreateInfo from these at
        // bind time; integer formats are forced to NEAREST there, not here.
        VkFilter MinFilter = VK_FILTER_LINEAR;
        VkFilter MagFilter = VK_FILTER_LINEAR;
        VkSamplerMipmapMode MipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VkSamplerAddressMode AddressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkBorderColor BorderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
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

        // #691 Phase 8: the SetTextureFilter / SetTextureWrap facade entries
        // mutate the recorded per-image sampler state (see the sampler-state
        // fields above). No-op for an unregistered image; takes effect at the
        // next BindTexture (the sampler slot is derived at bind time).
        void SetSamplerFilter(VkImage image, VkFilter minFilter, VkFilter magFilter);
        void SetSamplerAddressMode(VkImage image, VkSamplerAddressMode mode);

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
        // Phase 7 Wave C: occlusion query pools. vkCmdResetQueryPool /
        // vkCmdBeginQuery reference the pool from in-flight command buffers,
        // and DeleteQueries is called from a frame that may still have the
        // previous one submitted — same generation discipline as pipelines.
        void Enqueue(VkQueryPool queryPool);

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
            VkImage Image = VK_NULL_HANDLE; // exactly one of Image/Buffer/Semaphore/Pipeline/View/QueryPool is set
            VkBuffer Buffer = VK_NULL_HANDLE;
            VkSemaphore Semaphore = VK_NULL_HANDLE;
            VkPipeline Pipeline = VK_NULL_HANDLE;
            VkImageView View = VK_NULL_HANDLE;
            VkQueryPool QueryPool = VK_NULL_HANDLE;
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
    // VulkanTexture2DArray — the Texture2DArray backend twin (issue #691
    // Phase 7 Wave B). First consumer: ShadowMap's CSM/atlas placeholder
    // (sampler2DArrayShadow), which VolumetricFogPass::Execute materialises
    // lazily — under --rhi=vulkan the old GL-only factory constructed an
    // OpenGLTexture2DArray whose glCreateTextures call went through a null
    // glad pointer in any GL-context-free process (an access violation, found
    // by VulkanPassSuiteTest's fog tenant in an isolated run). Registers
    // VK_IMAGE_VIEW_TYPE_2D_ARRAY so both bind paths build array views
    // (sampler2DArrayShadow needs the array dimensionality, not the 2D
    // default). Allocation/identity/lifetime are full; the upload/mip
    // virtuals are warn-once no-ops until the Wave C shadow work needs them.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    // VulkanTextureCubemap — a 6-layer 2D image with a CUBE view type.
    //
    // Brought up for #691 Phase 7's live bring-up: Renderer3D::Init reaches
    // EnvironmentMap::InitializeIBLSystem, which creates cubemaps eagerly, so
    // WITHOUT this class the factory's assert killed the editor before the
    // first frame. Scope matches VulkanTexture2DArray's: a real image with a
    // real identity (so binds, barriers and the layout tracker all work), with
    // the CPU upload / mip-generation / readback halves warn-once no-ops —
    // the IBL bake path that fills those faces is GPU-side and is Phase 8
    // work (SkyCubemapBake / IBLPrecompute still need the capture seam).
    // -------------------------------------------------------------------------
    class VulkanTextureCubemap : public TextureCubemap
    {
      public:
        explicit VulkanTextureCubemap(const CubemapSpecification& spec);
        ~VulkanTextureCubemap() override;

        [[nodiscard]] const TextureSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard]] u32 GetWidth() const override
        {
            return m_CubemapSpecification.Width;
        }
        [[nodiscard]] u32 GetHeight() const override
        {
            return m_CubemapSpecification.Height;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0; // no GL name exists; identity is the RHI handle
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const std::string& GetPath() const override
        {
            return m_Path;
        }
        [[nodiscard]] bool IsLoaded() const override
        {
            return m_Image != VK_NULL_HANDLE;
        }
        [[nodiscard]] bool HasAlphaChannel() const override
        {
            return true;
        }
        [[nodiscard]] const CubemapSpecification& GetCubemapSpecification() const override
        {
            return m_CubemapSpecification;
        }
        [[nodiscard]] u32 GetMipLevelCount() const override
        {
            return m_MipLevels;
        }
        [[nodiscard]] VkImage GetVkImage() const
        {
            return m_Image;
        }

        void Bind(u32 slot) const override;
        void SetData(void* data, u32 size) override;
        void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;
        void SetFaceData(u32 faceIndex, void* data, u32 size) override;
        bool SetFaceDataMip(u32 faceIndex, u32 mipLevel, void* data, u32 size) override;
        void GenerateMipmaps() const override;
        bool GetFaceData(u32 faceIndex, std::vector<u8>& outData, u32 mipLevel = 0) const override;
        bool GetData(std::vector<u8>& outData, u32 mipLevel = 0) const override;

      private:
        TextureSpecification m_Specification;
        CubemapSpecification m_CubemapSpecification;
        std::string m_Path;
        u32 m_MipLevels = 1;
        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        RHI::ScopedResourceHandle m_RHIHandle;
    };

    // -------------------------------------------------------------------------
    // VulkanTextureCubemapArray — a 6*Layers-layer 2D image with a CUBE_ARRAY
    // view type (#691 Phase 8).
    //
    // Brought up for the reflection-probe arrays (issue #705's radiance /
    // distance-field arrays): ReflectionProbeArray::Init creates two of these
    // eagerly, so without this class the factory's assert wedged the first
    // --rhi=vulkan editor launch of Phase 8 during init — the same
    // backend-blind-factory shape as amendment (64). Scope matches
    // VulkanTextureCubemap's: real image, real identity (binds / barriers /
    // layout tracking all work), with the CPU upload and GPU layer-copy
    // halves warn-once no-ops until the cubemap-upload work lands.
    // -------------------------------------------------------------------------
    class VulkanTextureCubemapArray : public TextureCubemapArray
    {
      public:
        explicit VulkanTextureCubemapArray(const CubemapArraySpecification& spec);
        ~VulkanTextureCubemapArray() override;

        [[nodiscard]] const TextureSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard]] u32 GetWidth() const override
        {
            return m_ArraySpecification.Resolution;
        }
        [[nodiscard]] u32 GetHeight() const override
        {
            return m_ArraySpecification.Resolution;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0; // no GL name exists; identity is the RHI handle
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const std::string& GetPath() const override
        {
            return m_Path;
        }
        [[nodiscard]] bool IsLoaded() const override
        {
            return m_Image != VK_NULL_HANDLE;
        }
        [[nodiscard]] bool HasAlphaChannel() const override
        {
            return true;
        }
        [[nodiscard]] const CubemapArraySpecification& GetArraySpecification() const override
        {
            return m_ArraySpecification;
        }
        [[nodiscard]] u32 GetMipLevelCount() const override
        {
            return m_MipLevels;
        }
        [[nodiscard]] VkImage GetVkImage() const
        {
            return m_Image;
        }

        void Bind(u32 slot) const override;
        void SetData(void* data, u32 size) override;
        void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;
        bool SetLayerMipData(u32 layer, u32 mip, const void* data, sizet sizeBytes) override;
        bool CopyLayerFromCubemap(u32 layer, const TextureCubemap& source) override;
        bool GetData(std::vector<u8>& outData, u32 mipLevel = 0) const override;

      private:
        TextureSpecification m_Specification;
        CubemapArraySpecification m_ArraySpecification;
        std::string m_Path;
        u32 m_MipLevels = 1;
        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        RHI::ScopedResourceHandle m_RHIHandle;
    };

    class VulkanTexture2DArray : public Texture2DArray
    {
      public:
        explicit VulkanTexture2DArray(const Texture2DArraySpecification& spec);
        ~VulkanTexture2DArray() override;

        [[nodiscard]] u32 GetWidth() const override
        {
            return m_Specification.Width;
        }
        [[nodiscard]] u32 GetHeight() const override
        {
            return m_Specification.Height;
        }
        [[nodiscard]] u32 GetLayers() const override
        {
            return m_Specification.Layers;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0; // no GL name exists; identity is the RHI handle
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const Texture2DArraySpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard]] VkImage GetVkImage() const
        {
            return m_Image;
        }

        void Bind(u32 slot) const override;
        void SetLayerData(u32 layer, const void* data, u32 width, u32 height) override;
        void GenerateMipmaps() override;

      private:
        Texture2DArraySpecification m_Specification;
        u32 m_MipLevels = 1;
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

        // Retire every cached per-cascade depth view built over `image`, in
        // EVERY live framebuffer. Called from VulkanDeferredReclaim's destroy
        // pass, before the image is destroyed — a VkImageView must never
        // outlive the VkImage it views. (Framebuffers self-register for this;
        // see m_DepthArrayViews for what goes wrong without it.)
        static void ReleaseCachedDepthViewsForImage(VkImage image);

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

        // #691 Phase 7 Wave C §4 (layered shadow depth): the GL twin re-points
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

        // #691 Phase 8 — the raw (object-less) framebuffer facade. A
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
        // (review finding, #691 Phase 8) — the owner re-attaches at its own
        // new size. Recomputed from the per-slot tracking below on every
        // detach, so detaching the last external attachment unblocks Resize.
        bool m_HasExternalAttachments = false;
        // Which color slots hold externally-owned attachments, plus the depth
        // twin. Needed because m_ColorAttachments mixes internal (spec-
        // created) and external slots: the flag recompute and the
        // ClearAllAttachments format decision must not confuse the two.
        std::unordered_set<u32> m_ExternalColorIndices;
        bool m_ExternalDepth = false;
        struct CachedDepthArrayView
        {
            VkImageView View = VK_NULL_HANDLE;
            VkImage SourceImage = VK_NULL_HANDLE;
        };
        std::unordered_map<u64, CachedDepthArrayView> m_DepthArrayViews;
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
        // The persistent buffer's address. Stable for the buffer's life;
        // Resize mints a new one. GPU-write participants (compute dispatch
        // root data, indirect-args resolution, copies) use THIS address —
        // their writes must land in the one buffer every later consumer
        // resolves.
        [[nodiscard]] VkDeviceAddress GetDeviceAddress() const
        {
            return m_DeviceAddress;
        }
        // The address a DRAW's root-data writer embeds (ADR 0011 §4) — the
        // storage twin of VulkanUniformBuffer::GetRootDataAddress. A CPU
        // SetData mid-frame snapshots the written range into the frame arena,
        // and draws recorded AFTER the write embed the snapshot's address
        // while earlier draws keep the one they recorded — GL's command-
        // ordered glNamedBufferSubData semantics. Without this, every draw
        // in the frame reads the LAST SetData at execute time: the exact
        // failure that emptied the auto-batched instanced draws (all batches
        // sampling the final ModelInstanceBuffer upload — #691 Phase 8).
        // Falls back to the persistent address when no snapshot is live
        // (GPU-written buffers never SetData mid-frame, so they always
        // resolve persistent).
        [[nodiscard]] VkDeviceAddress GetRootDataAddress();

      private:
        void CreateBuffer();
        void ReleaseBuffer();
        // Copies the just-written range (plus any live snapshot content it
        // does not cover) into a fresh frame-arena range and points
        // GetRootDataAddress at it. Failure (arena overflow, unreadable
        // prefix) invalidates the snapshot so draws fall back to the
        // persistent buffer — today's pre-fix semantics, never garbage.
        void PushSnapshot(const void* data, u32 size, u32 offset);
        void InvalidateSnapshot()
        {
            m_SnapshotAddress = 0;
            m_SnapshotCpu = nullptr;
            m_SnapshotBytes = 0;
        }

        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        void* m_Mapped = nullptr; ///< Non-null when VMA gave a host-visible placement.
        bool m_NeedsFlush = false;
        VkDeviceAddress m_DeviceAddress = 0;
        // Command-ordered draw-read snapshot (see GetRootDataAddress).
        // Valid only while m_SnapshotFrameGeneration matches the arena's
        // current frame — arena ranges recycle after kFramesInFlight.
        u64 m_SnapshotFrameGeneration = ~0ull;
        VkDeviceAddress m_SnapshotAddress = 0;
        void* m_SnapshotCpu = nullptr;
        u32 m_SnapshotBytes = 0;
        // Generation-checked identity for m_Buffer, kept in lockstep by
        // m_RHIHandle.Sync — same pattern as the GL twin (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        u32 m_Size = 0;
        u32 m_Binding = 0;
        StorageBufferUsage m_Usage = StorageBufferUsage::DynamicDraw;
    };

    // -------------------------------------------------------------------------
    // VulkanRawTextureRegistry — the object-less texture family behind the
    // CreateTexture2DHandle / CreateTextureCubemapHandle / DeleteTexture
    // facade entries (#691 Phase 8; the VulkanRawBufferRegistry pattern).
    //
    // GL's shape is a bare glCreateTextures name with immutable single-mip
    // storage; production consumers are pass-owned render targets
    // (FluidIntermediatesPass's splat FBO attachments), CPU-uploaded lookup
    // textures (SSAO's 4x4 rotation noise) and compute-written scratch
    // (VirtualMeshRegistry's debug planes). There is no engine-side C++
    // object at the CALL SITE to hang the VkImage off — but unlike raw
    // buffers a full backend texture class exists, so this side table owns a
    // real Ref<VulkanTexture2D>/<VulkanTextureCubemap> keyed by the object's
    // OWN identity handle (whose registry native is already the VkImage —
    // generic native resolution, image-info lookups and barrier lowering work
    // on raw textures exactly as on object-backed ones). That is the one
    // deliberate shape difference from the buffer registry: Adopt() instead
    // of CreateHandle(), because the adopted object mints the identity.
    //
    // Destroy drops the owning Ref — the texture destructor retires the
    // identity and routes the VkImage through VulkanDeferredReclaim. Safe on
    // foreign/stale handles (returns false, caller warns). ReleaseAll is the
    // device-teardown net: a raw texture still held here after its creator
    // leaked it must not outlive the allocator.
    //
    // Render-thread only, same as everything else here.
    // -------------------------------------------------------------------------
    class VulkanRawTextureRegistry
    {
      public:
        [[nodiscard]] static VulkanRawTextureRegistry& Get();

        // Adopt ownership; returns the texture's own identity handle (the
        // raw handle the facade hands out). Null texture => null handle.
        RHI::ResourceHandle Adopt(Ref<VulkanTexture2D> texture);
        RHI::ResourceHandle Adopt(Ref<VulkanTextureCubemap> cubemap);

        // Null when the handle was never adopted here, is stale, or names a
        // cubemap entry (attachment resolution wants the 2D flavour only).
        [[nodiscard]] Ref<VulkanTexture2D> Lookup2D(RHI::ResourceHandle handle) const;
        [[nodiscard]] bool Contains(RHI::ResourceHandle handle) const;

        // Drops the entry (see class comment). False when the handle was
        // never adopted here — the caller decides how loudly to say so.
        bool Destroy(RHI::ResourceHandle handle);
        void ReleaseAll();

      private:
        VulkanRawTextureRegistry() = default;

        [[nodiscard]] static u64 Key(RHI::ResourceHandle handle)
        {
            return (static_cast<u64>(handle.Generation) << 32) | handle.Index;
        }

        struct Entry
        {
            Ref<VulkanTexture2D> Texture2D; // exactly one of the two is set
            Ref<VulkanTextureCubemap> Cubemap;
        };
        std::unordered_map<u64, Entry> m_Entries;
    };

    // -------------------------------------------------------------------------
    // VulkanRawFramebufferRegistry — ownership side table for the raw
    // CreateFramebufferHandle / DeleteFramebuffer framebuffers (#691 Phase 8).
    //
    // The OBJECT resolution path needs nothing new: VulkanFramebuffer's
    // constructor registers itself in VulkanRootObjectRegistry, which is what
    // ResolveFramebufferObject (BindFramebuffer / clears / blits /
    // draw-attachment selection / the rendering scope) already reads — so a
    // raw framebuffer behaves exactly like an engine-owned one everywhere.
    // This table exists solely to OWN the Ref between CreateFramebufferHandle
    // and DeleteFramebuffer, same Adopt/Destroy/ReleaseAll contract as the
    // raw texture registry above.
    // -------------------------------------------------------------------------
    class VulkanRawFramebufferRegistry
    {
      public:
        [[nodiscard]] static VulkanRawFramebufferRegistry& Get();

        RHI::ResourceHandle Adopt(Ref<VulkanFramebuffer> framebuffer);
        [[nodiscard]] bool Contains(RHI::ResourceHandle handle) const;
        bool Destroy(RHI::ResourceHandle handle);
        void ReleaseAll();

      private:
        VulkanRawFramebufferRegistry() = default;

        [[nodiscard]] static u64 Key(RHI::ResourceHandle handle)
        {
            return (static_cast<u64>(handle.Generation) << 32) | handle.Index;
        }

        std::unordered_map<u64, Ref<VulkanFramebuffer>> m_Entries;
    };
    // Teardown forensics (#691 Phase 8): logs every VulkanTexture2D /
    // VulkanStorageBuffer object still alive, with its Debug-captured
    // creation stack — the texture/storage twin of
    // VulkanRootObjectRegistry::LogSurvivingVertexArrays. No-op in
    // non-Debug builds.
    void VulkanLogSurvivingTransients();

} // namespace OloEngine

#endif // OLO_WITH_VULKAN
