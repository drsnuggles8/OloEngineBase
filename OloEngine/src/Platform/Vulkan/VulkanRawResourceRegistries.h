#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanRawResourceRegistries.h — the raw-handle registries backing the
// Create*/Delete*/Attach* facade family: VulkanRawTextureRegistry and
// VulkanRawFramebufferRegistry (#691; split out of the single
// VulkanTransientResources.h).
// =============================================================================

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "Platform/Vulkan/VulkanTexture.h"
#include "Platform/Vulkan/VulkanTextureCubemap.h"

#include <unordered_map>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanRawTextureRegistry — the object-less texture family behind the
    // CreateTexture2DHandle / CreateTextureCubemapHandle / DeleteTexture
    // facade entries (#691; the VulkanRawBufferRegistry pattern).
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
    // CreateFramebufferHandle / DeleteFramebuffer framebuffers (#691).
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
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
