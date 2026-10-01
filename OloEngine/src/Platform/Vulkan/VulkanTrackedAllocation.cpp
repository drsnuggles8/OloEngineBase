#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTrackedAllocation.h"

#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"

#include <optional>

namespace OloEngine
{
    namespace
    {
        using ResourceType = RendererMemoryTracker::ResourceType;

        [[nodiscard]] ResourceType ClassifyBuffer(const VkBufferCreateInfo& info)
        {
            if ((info.usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR) != 0)
                return ResourceType::AccelerationStructure;
            if ((info.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0)
                return ResourceType::VertexBuffer;
            if ((info.usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0)
                return ResourceType::IndexBuffer;
            if ((info.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0 && (info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0)
                return ResourceType::UniformBuffer;
            return ResourceType::StorageBuffer;
        }

        [[nodiscard]] ResourceType ClassifyImage(const VkImageCreateInfo& info)
        {
            if ((info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0)
                return ResourceType::TextureCubemap;
            if ((info.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0)
                return ResourceType::RenderTarget;
            if (info.imageType == VK_IMAGE_TYPE_2D)
                return ResourceType::Texture2D;
            return ResourceType::Other;
        }

        // A buffer that is only ever a copy source or destination is upload/readback staging.
        [[nodiscard]] bool IsStagingBuffer(const VkBufferCreateInfo& info)
        {
            constexpr VkBufferUsageFlags kTransfer = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            return info.usage != 0 && (info.usage & ~kTransfer) == 0;
        }

        void Book(VmaAllocator allocator, VmaAllocation allocation, const VmaAllocationInfo* callerInfo,
                  ResourceType type, std::string_view name)
        {
            VmaAllocationInfo info{};
            if (callerInfo)
            {
                info = *callerInfo;
            }
            else
            {
                vmaGetAllocationInfo(allocator, allocation, &info);
            }

            RendererMemoryTracker::AllocationDesc desc;
            desc.Address = allocation;
            desc.Size = static_cast<sizet>(info.size);
            desc.Type = type;
            desc.Name = name;
            desc.IsGPU = true; // device memory, whichever heap VMA placed it in
            desc.Backend = MemoryBackend::Vulkan;
            desc.SizeSource = MemorySizeSource::Committed;
            desc.File = __FILE__;
            desc.Line = __LINE__;
            RendererMemoryTracker::GetInstance().TrackAllocation(desc);
        }

        // Opens the tracker's mutation bracket for the lifetime of a create or destroy, so
        // a report taken concurrently says Racing instead of inventing a difference.
        struct ScopedMutation
        {
            ScopedMutation()
            {
                RendererMemoryTracker::GetInstance().BeginExternalMutation();
            }
            ~ScopedMutation()
            {
                RendererMemoryTracker::GetInstance().EndExternalMutation();
            }
            ScopedMutation(const ScopedMutation&) = delete;
            ScopedMutation& operator=(const ScopedMutation&) = delete;
        };
    } // namespace

    VkResult TrackedVmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* bufferCreateInfo,
                                    const VmaAllocationCreateInfo* allocationCreateInfo, VkBuffer* buffer,
                                    VmaAllocation* allocation, VmaAllocationInfo* allocationInfo)
    {
        const ScopedMutation mutation;
        const VkResult result = vmaCreateBuffer(allocator, bufferCreateInfo, allocationCreateInfo, buffer, allocation, allocationInfo);
        if (result == VK_SUCCESS && allocation && *allocation)
        {
            // Staging made outside any owner scope is attributed as staging, not "unattributed".
            const bool unscopedStaging = IsStagingBuffer(*bufferCreateInfo) && RendererMemoryOwnerScope::Current().Owner.empty();
            std::optional<RendererMemoryOwnerScope> staging;
            if (unscopedStaging)
                staging.emplace("VulkanStaging", MemoryLifetime::Staging);
            Book(allocator, *allocation, allocationInfo, ClassifyBuffer(*bufferCreateInfo), "VMA buffer");
        }
        return result;
    }

    VkResult TrackedVmaCreateBufferWithAlignment(VmaAllocator allocator, const VkBufferCreateInfo* bufferCreateInfo,
                                                 const VmaAllocationCreateInfo* allocationCreateInfo,
                                                 VkDeviceSize minAlignment, VkBuffer* buffer,
                                                 VmaAllocation* allocation, VmaAllocationInfo* allocationInfo)
    {
        const ScopedMutation mutation;
        const VkResult result = vmaCreateBufferWithAlignment(allocator, bufferCreateInfo, allocationCreateInfo, minAlignment,
                                                             buffer, allocation, allocationInfo);
        if (result == VK_SUCCESS && allocation && *allocation)
        {
            // Staging made outside any owner scope is attributed as staging, not "unattributed".
            const bool unscopedStaging = IsStagingBuffer(*bufferCreateInfo) && RendererMemoryOwnerScope::Current().Owner.empty();
            std::optional<RendererMemoryOwnerScope> staging;
            if (unscopedStaging)
                staging.emplace("VulkanStaging", MemoryLifetime::Staging);
            Book(allocator, *allocation, allocationInfo, ClassifyBuffer(*bufferCreateInfo), "VMA buffer");
        }
        return result;
    }

    VkResult TrackedVmaCreateImage(VmaAllocator allocator, const VkImageCreateInfo* imageCreateInfo,
                                   const VmaAllocationCreateInfo* allocationCreateInfo, VkImage* image,
                                   VmaAllocation* allocation, VmaAllocationInfo* allocationInfo)
    {
        const ScopedMutation mutation;
        const VkResult result = vmaCreateImage(allocator, imageCreateInfo, allocationCreateInfo, image, allocation, allocationInfo);
        if (result == VK_SUCCESS && allocation && *allocation)
        {
            Book(allocator, *allocation, allocationInfo, ClassifyImage(*imageCreateInfo), "VMA image");
        }
        return result;
    }

    void TrackedVmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
        const ScopedMutation mutation;
        if (allocation)
        {
            OLO_TRACK_DEALLOC(allocation);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    }

    void TrackedVmaDestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation)
    {
        const ScopedMutation mutation;
        if (allocation)
        {
            OLO_TRACK_DEALLOC(allocation);
        }
        vmaDestroyImage(allocator, image, allocation);
    }

    void TrackedVmaSetAllocationName(VmaAllocator allocator, VmaAllocation allocation, const char* name)
    {
        vmaSetAllocationName(allocator, allocation, name);
        if (allocation && name)
        {
            RendererMemoryTracker::GetInstance().RenameAllocation(allocation, name);
        }
    }

    u64 RetireTrackedVmaAllocation(VmaAllocation allocation)
    {
        return allocation ? OLO_TRACK_RETIRE(allocation) : 0;
    }

    void TrackedVmaDestroyRetiredBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation, const u64 retireTicket)
    {
        const ScopedMutation mutation;
        OLO_TRACK_RELEASE_RETIRED(retireTicket);
        vmaDestroyBuffer(allocator, buffer, allocation);
    }

    void TrackedVmaDestroyRetiredImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation, const u64 retireTicket)
    {
        const ScopedMutation mutation;
        OLO_TRACK_RELEASE_RETIRED(retireTicket);
        vmaDestroyImage(allocator, image, allocation);
    }

    bool ObserveVmaMemory(VmaAllocator allocator, const bool budgetExtensionEnabled, BackendMemoryObservation& out)
    {
        out = BackendMemoryObservation{};
        out.Backend = MemoryBackend::Vulkan;
        if (allocator == VK_NULL_HANDLE)
        {
            return false;
        }

        const VkPhysicalDeviceMemoryProperties* properties = nullptr;
        vmaGetMemoryProperties(allocator, &properties);
        if (!properties)
        {
            return false;
        }

        // vmaGetHeapBudgets carries VMA's own per-heap statistics as well as the budget,
        // and is cheap (no block walk), unlike vmaCalculateStatistics.
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(allocator, budgets.data());

        out.HasAllocatorTotals = true;
        out.Residency = budgetExtensionEnabled ? MemoryResidencyStatus::OsReported : MemoryResidencyStatus::AllocatorHeuristic;
        for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap)
        {
            const VmaBudget& budget = budgets[heap];
            MemoryHeapObservation observation;
            observation.Index = heap;
            observation.DeviceLocal = (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            observation.HeapSizeBytes = properties->memoryHeaps[heap].size;
            observation.UsageBytes = budget.usage;
            observation.BudgetBytes = budget.budget;
            observation.AllocatorBlockBytes = budget.statistics.blockBytes;
            observation.AllocatorAllocationBytes = budget.statistics.allocationBytes;
            out.AllocationBytes += budget.statistics.allocationBytes;
            out.BlockBytes += budget.statistics.blockBytes;
            out.AllocationCount += budget.statistics.allocationCount;
            out.Heaps.Add(observation);
        }
        return true;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
