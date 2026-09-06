#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanBindingState.h"

#include "Platform/Vulkan/VulkanRecordingContext.h"

namespace OloEngine
{
    VulkanBindingState& VulkanBindingState::Global()
    {
        static auto* s_Instance = new VulkanBindingState(); // deliberately leaked
        return *s_Instance;
    }

    VulkanBindingState& VulkanBindingState::Get()
    {
        if (VulkanWorkerRecordingContext* worker = CurrentVulkanWorkerContext(); worker != nullptr)
        {
            return worker->Binding;
        }
        return Global();
    }

    void VulkanBindingState::EnsureTextureSlotsInitialised()
    {
        if (!m_TextureSlotsInitialised)
        {
            m_TextureHeapSlots.fill(kNoHeapSlot);
            m_ImageHeapSlots.fill(kNoHeapSlot);
            m_TextureSlotsInitialised = true;
        }
    }

    void VulkanBindingState::SetImageHeapSlot(u32 unit, u32 heapSlot)
    {
        EnsureTextureSlotsInitialised();
        if (unit >= kMaxTextureSlots)
        {
            OLO_CORE_WARN("VulkanBindingState: image unit {} out of range", unit);
            return;
        }
        m_ImageHeapSlots[unit] = heapSlot;
    }

    u32 VulkanBindingState::GetImageHeapSlot(u32 unit) const
    {
        if (!m_TextureSlotsInitialised || unit >= kMaxTextureSlots)
        {
            return kNoHeapSlot;
        }
        return m_ImageHeapSlots[unit];
    }

    namespace
    {
        // Render-thread scoped in practice (the recorder's prepare loop), but
        // thread_local so a buffer created on any other thread is unaffected.
        thread_local u32 t_ClaimSuppression = 0u;
    } // namespace

    VulkanBindingState::ScopedClaimSuppression::ScopedClaimSuppression()
    {
        ++t_ClaimSuppression;
    }

    VulkanBindingState::ScopedClaimSuppression::~ScopedClaimSuppression()
    {
        OLO_CORE_ASSERT(t_ClaimSuppression > 0u, "unbalanced binding-claim suppression");
        --t_ClaimSuppression;
    }

    bool VulkanBindingState::ClaimsSuppressed()
    {
        return t_ClaimSuppression != 0u;
    }

    void VulkanBindingState::SetUniformBuffer(u32 binding, VulkanUniformBuffer* buffer)
    {
        if (binding >= kMaxBufferBindings)
        {
            OLO_CORE_WARN("VulkanBindingState: UBO binding {} out of range", binding);
            return;
        }
        m_UniformBuffers[binding] = buffer;
    }

    void VulkanBindingState::SetStorageBuffer(u32 binding, VulkanStorageBuffer* buffer)
    {
        if (binding >= kMaxBufferBindings)
        {
            OLO_CORE_WARN("VulkanBindingState: SSBO binding {} out of range", binding);
            return;
        }
        m_StorageBuffers[binding] = buffer;
    }

    void VulkanBindingState::SetStorageBufferAddress(u32 binding, u64 address)
    {
        if (binding >= kMaxBufferBindings)
        {
            OLO_CORE_WARN("VulkanBindingState: SSBO binding {} out of range", binding);
            return;
        }
        m_StorageBufferAddresses[binding] = address;
    }

    u64 VulkanBindingState::GetStorageBufferAddress(u32 binding) const
    {
        return binding < kMaxBufferBindings ? m_StorageBufferAddresses[binding] : 0;
    }

    VulkanUniformBuffer* VulkanBindingState::GetUniformBuffer(u32 binding) const
    {
        return binding < kMaxBufferBindings ? m_UniformBuffers[binding] : nullptr;
    }

    VulkanStorageBuffer* VulkanBindingState::GetStorageBuffer(u32 binding) const
    {
        return binding < kMaxBufferBindings ? m_StorageBuffers[binding] : nullptr;
    }

    // GLOBAL-ONLY BY CONSTRUCTION, and that is sufficient — the reasoning is
    // not local, so it is written down here (issue #1052).
    //
    // Get() answers a WORKER's mirror while a RecordParallel item runs on that
    // thread, so clearing through Get() clears one mirror. Both callers —
    // VulkanRawBufferRegistry::Allocate's orphan path and ::Destroy — are
    // RefuseOnWorker entry points, so they always run on the render thread and
    // Get() is Global() there. A worker mirror cannot be holding a stale
    // address at that moment either: RecordParallel forks through a BLOCKING
    // ParallelFor under an m_InParallelRegion guard, so while worker mirrors
    // exist the render thread is inside that call and cannot be retiring a
    // buffer; and a worker seeds its copy FROM Global at the fork, so a clear
    // that happened before the fork is inherited.
    //
    // This is the same discipline the object-pointer twin ClearBuffer relies on
    // (see the header's dangling-pointer note). If RecordParallel ever becomes
    // non-blocking, or a resource entry point loses its RefuseOnWorker guard,
    // BOTH paths need active-mirror tracking, not just this one.
    void VulkanBindingState::ClearStorageBufferAddress(u64 address)
    {
        if (address == 0)
        {
            return;
        }
        for (auto& entry : m_StorageBufferAddresses)
        {
            if (entry == address)
            {
                entry = 0;
            }
        }
    }

    void VulkanBindingState::ClearBuffer(const void* buffer)
    {
        for (auto& entry : m_UniformBuffers)
        {
            if (entry == buffer)
            {
                entry = nullptr;
            }
        }
        for (auto& entry : m_StorageBuffers)
        {
            if (entry == buffer)
            {
                entry = nullptr;
            }
        }
    }

    void VulkanBindingState::SetTextureHeapSlot(u32 slot, u32 heapSlot)
    {
        EnsureTextureSlotsInitialised();
        if (slot >= kMaxTextureSlots)
        {
            OLO_CORE_WARN("VulkanBindingState: texture slot {} out of range", slot);
            return;
        }
        m_TextureHeapSlots[slot] = heapSlot;
    }

    u32 VulkanBindingState::GetTextureHeapSlot(u32 slot) const
    {
        if (!m_TextureSlotsInitialised || slot >= kMaxTextureSlots)
        {
            return kNoHeapSlot;
        }
        return m_TextureHeapSlots[slot];
    }

    void VulkanBindingState::SetTextureSamplerSlot(u32 slot, u32 samplerSlot)
    {
        if (slot >= kMaxTextureSlots)
        {
            // The heap-slot setter already warned for this slot.
            return;
        }
        m_TextureSamplerSlots[slot] = samplerSlot;
    }

    u32 VulkanBindingState::GetTextureSamplerSlot(u32 slot) const
    {
        if (slot >= kMaxTextureSlots)
        {
            return 0u; // VulkanSamplerHeap::DefaultSlot
        }
        return m_TextureSamplerSlots[slot];
    }

    void VulkanBindingState::SetCurrentFramebuffer(VulkanFramebuffer* framebuffer)
    {
        m_CurrentFramebuffer = framebuffer;
    }

    VulkanFramebuffer* VulkanBindingState::GetCurrentFramebuffer() const
    {
        return m_CurrentFramebuffer;
    }

    void VulkanBindingState::ClearIfCurrentFramebuffer(const VulkanFramebuffer* framebuffer)
    {
        if (m_CurrentFramebuffer == framebuffer)
        {
            m_CurrentFramebuffer = nullptr;
        }
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
