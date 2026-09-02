#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanSecondaryCommandPools.h"

#include "Platform/Vulkan/VulkanDevice.h"

namespace OloEngine
{
    VulkanSecondaryCommandPools& VulkanSecondaryCommandPools::Get()
    {
        static auto* s_Instance = new VulkanSecondaryCommandPools(); // deliberately leaked
        return *s_Instance;
    }

    bool VulkanSecondaryCommandPools::SyncToFrame()
    {
        const auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return false;
        }
        auto& arena = VulkanFrameArena::Get();
        const u64 generation = arena.GetFrameGeneration();
        if (generation == 0u)
        {
            // No frame owner has proven any slot retired — nothing may be
            // reset, so nothing may be recorded into either.
            return false;
        }
        if (generation == m_SyncedGeneration)
        {
            return true;
        }
        m_ActiveSlot = arena.GetCurrentSlot() % kFramesInFlight;
        for (Pool& pool : m_Pools[m_ActiveSlot])
        {
            // Only pools that handed out a buffer since their last reset: an
            // untouched pool has nothing to return to the initial state, and
            // a region of 4 items should not reset the 40 pools a bigger
            // region used two frames ago.
            if (pool.Handle != VK_NULL_HANDLE && pool.Cursor != 0u)
            {
                // The buffers stay allocated (the pool owns them); a reset
                // returns them all to the initial state in one call, which
                // is why the pool is TRANSIENT and not RESET_COMMAND_BUFFER.
                if (const VkResult result = vkResetCommandPool(device->GetDevice(), pool.Handle, 0);
                    result != VK_SUCCESS)
                {
                    OLO_CORE_ERROR("[RHI/Vulkan] vkResetCommandPool(secondary, slot {}) failed (VkResult {})",
                                   m_ActiveSlot, static_cast<int>(result));
                }
            }
            pool.Cursor = 0;
        }
        m_SyncedGeneration = generation;
        return true;
    }

    VulkanSecondaryCommandPools::Pool* VulkanSecondaryCommandPools::EnsurePool(const u32 slot, const u32 itemIndex)
    {
        if (slot >= kFramesInFlight)
        {
            return nullptr;
        }
        auto& pools = m_Pools[slot];
        if (itemIndex >= pools.size())
        {
            pools.resize(static_cast<sizet>(itemIndex) + 1u);
        }
        Pool& pool = pools[itemIndex];
        if (pool.Handle != VK_NULL_HANDLE)
        {
            return &pool;
        }
        const auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return nullptr;
        }
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // TRANSIENT: every buffer is re-recorded each frame and the whole pool
        // resets at once (SyncToFrame).
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = device->GetQueueFamily();
        if (const VkResult result = vkCreateCommandPool(device->GetDevice(), &info, nullptr, &pool.Handle);
            result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] vkCreateCommandPool(secondary, slot {}, item {}) failed (VkResult {})", slot,
                           itemIndex, static_cast<int>(result));
            pool.Handle = VK_NULL_HANDLE;
            return nullptr;
        }
        return &pool;
    }

    VkCommandBuffer VulkanSecondaryCommandPools::AcquireBegun(const u32 itemIndex)
    {
        Pool* pool = EnsurePool(m_ActiveSlot, itemIndex);
        if (pool == nullptr)
        {
            return VK_NULL_HANDLE;
        }
        const auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return VK_NULL_HANDLE;
        }
        if (pool->Cursor == pool->Buffers.size())
        {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = pool->Handle;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            allocate.commandBufferCount = 1;
            VkCommandBuffer next = VK_NULL_HANDLE;
            if (const VkResult result = vkAllocateCommandBuffers(device->GetDevice(), &allocate, &next);
                result != VK_SUCCESS)
            {
                OLO_CORE_ERROR("[RHI/Vulkan] vkAllocateCommandBuffers(secondary, item {}) failed (VkResult {})",
                               itemIndex, static_cast<int>(result));
                return VK_NULL_HANDLE;
            }
            pool->Buffers.push_back(next);
        }
        const VkCommandBuffer cmd = pool->Buffers[pool->Cursor];

        // A secondary needs an inheritance block even when it inherits
        // nothing: no render pass (it is executed outside any instance and
        // begins its own dynamic rendering), no framebuffer, no occlusion
        // query inheritance (queries are refused on worker contexts).
        VkCommandBufferInheritanceInfo inheritance{};
        inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin.pInheritanceInfo = &inheritance;
        if (const VkResult result = vkBeginCommandBuffer(cmd, &begin); result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] vkBeginCommandBuffer(secondary, item {}) failed (VkResult {})", itemIndex,
                           static_cast<int>(result));
            return VK_NULL_HANDLE;
        }
        // Counted as used even if the caller then declines the fork: a begun
        // buffer must go through the pool reset before it is handed out again.
        ++pool->Cursor;
        return cmd;
    }

    void VulkanSecondaryCommandPools::ReleaseAll()
    {
        const auto* device = VulkanDevice::Get();
        for (auto& slotPools : m_Pools)
        {
            for (Pool& pool : slotPools)
            {
                if (pool.Handle != VK_NULL_HANDLE && device != nullptr)
                {
                    // Destroying the pool frees its command buffers.
                    vkDestroyCommandPool(device->GetDevice(), pool.Handle, nullptr);
                }
            }
            slotPools.clear();
        }
        m_SyncedGeneration = 0;
        m_ActiveSlot = 0;
    }

    u32 VulkanSecondaryCommandPools::GetLivePoolCount() const
    {
        u32 count = 0;
        for (const auto& slotPools : m_Pools)
        {
            for (const Pool& pool : slotPools)
            {
                count += pool.Handle != VK_NULL_HANDLE ? 1u : 0u;
            }
        }
        return count;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
