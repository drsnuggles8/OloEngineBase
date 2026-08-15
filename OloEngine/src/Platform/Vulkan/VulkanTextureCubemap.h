#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanTextureCubemap.h — the VMA-backed TextureCubemap backend twin of
// OpenGLTextureCubemap (#691; split out of the single VulkanTransientResources.h
// in Phase 9).
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

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
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
        // The shared single-submit face readback behind GetFaceData and
        // GetData (#691 Phase 9, PR #794 review): copies `faceCount`
        // consecutive faces starting at `baseFace`, mip `mipLevel`, into ONE
        // readback buffer via ONE blocking one-shot submit (one mid-frame
        // flush first when a frame is recording), packed contiguously in face
        // order, then narrows widened RGB back to the caller's engine format.
        // GetData previously looped GetFaceData six times, paying the flush +
        // blocking-submit + readback-buffer create/destroy round per face.
        // `what` labels the one-shot with the public entry point's name.
        [[nodiscard]] bool ReadFaces(u32 baseFace, u32 faceCount, u32 mipLevel, std::vector<u8>& outData,
                                     const char* what) const;

        TextureSpecification m_Specification;
        CubemapSpecification m_CubemapSpecification;
        std::string m_Path;
        u32 m_MipLevels = 1;
        VkImage m_Image = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        RHI::ScopedResourceHandle m_RHIHandle;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
