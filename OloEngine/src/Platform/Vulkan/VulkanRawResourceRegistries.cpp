#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanRawResourceRegistries.h"

namespace OloEngine
{
    VulkanRawTextureRegistry& VulkanRawTextureRegistry::Get()
    {
        static auto* s_Instance = new VulkanRawTextureRegistry(); // deliberately leaked
        return *s_Instance;
    }

    RHI::ResourceHandle VulkanRawTextureRegistry::Adopt(Ref<VulkanTexture2D> texture)
    {
        if (texture == nullptr)
        {
            return {};
        }
        const RHI::ResourceHandle handle = texture->GetRHIHandle();
        if (!handle.IsValid())
        {
            return {};
        }
        m_Entries[Key(handle)] = Entry{ .Texture2D = std::move(texture) };
        return handle;
    }

    RHI::ResourceHandle VulkanRawTextureRegistry::Adopt(Ref<VulkanTextureCubemap> cubemap)
    {
        if (cubemap == nullptr)
        {
            return {};
        }
        const RHI::ResourceHandle handle = cubemap->GetRHIHandle();
        if (!handle.IsValid())
        {
            return {};
        }
        m_Entries[Key(handle)] = Entry{ .Cubemap = std::move(cubemap) };
        return handle;
    }

    Ref<VulkanTexture2D> VulkanRawTextureRegistry::Lookup2D(RHI::ResourceHandle handle) const
    {
        const auto it = m_Entries.find(Key(handle));
        return it != m_Entries.end() ? it->second.Texture2D : nullptr;
    }

    bool VulkanRawTextureRegistry::Contains(RHI::ResourceHandle handle) const
    {
        return m_Entries.contains(Key(handle));
    }

    bool VulkanRawTextureRegistry::Destroy(RHI::ResourceHandle handle)
    {
        // Erasing drops the owning Ref: the texture destructor retires the
        // identity (outstanding handles go stale) and routes the VkImage
        // through VulkanDeferredReclaim — never an inline destroy.
        return m_Entries.erase(Key(handle)) != 0u;
    }

    void VulkanRawTextureRegistry::ReleaseAll()
    {
        m_Entries.clear();
    }

    VulkanRawFramebufferRegistry& VulkanRawFramebufferRegistry::Get()
    {
        static auto* s_Instance = new VulkanRawFramebufferRegistry(); // deliberately leaked
        return *s_Instance;
    }

    RHI::ResourceHandle VulkanRawFramebufferRegistry::Adopt(Ref<VulkanFramebuffer> framebuffer)
    {
        if (framebuffer == nullptr)
        {
            return {};
        }
        const RHI::ResourceHandle handle = framebuffer->GetRHIHandle();
        if (!handle.IsValid())
        {
            return {};
        }
        m_Entries[Key(handle)] = std::move(framebuffer);
        return handle;
    }

    bool VulkanRawFramebufferRegistry::Contains(RHI::ResourceHandle handle) const
    {
        return m_Entries.contains(Key(handle));
    }

    bool VulkanRawFramebufferRegistry::Destroy(RHI::ResourceHandle handle)
    {
        // The framebuffer destructor unregisters from VulkanRootObjectRegistry
        // and retires the identity; the attachment Refs it holds release with
        // it (each routing through VulkanDeferredReclaim if this was the last
        // owner).
        return m_Entries.erase(Key(handle)) != 0u;
    }

    void VulkanRawFramebufferRegistry::ReleaseAll()
    {
        m_Entries.clear();
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
