#pragma once

// VulkanGpuFence — RHI::GpuFence backed by a timeline VkSemaphore.
// Issue #691 Phase 6, ADR 0011 §6.
//
// The fence IS the timeline semaphore: §6's Signal(pointer, value, op) /
// Wait(pointer, value, compareOp) pair is deliberately unified with the
// primitive Vulkan already has (core 1.2; the timelineSemaphore feature is
// enabled at device creation) rather than lowered to VkEvent or a hand-rolled
// BDA atomic. Queue-attached ops are STAGED here and consumed by the next
// vkQueueSubmit2 the frame loop (or a test fixture) makes — see
// VulkanGpuFence::DrainPendingSubmitOps, which every submit site owes a call.

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RHI/RHIGpuFence.h"

#include <volk.h>

#include <vector>

namespace OloEngine
{
    class VulkanGpuFence final : public RHI::GpuFence
    {
      public:
        explicit VulkanGpuFence(u64 initialValue);
        ~VulkanGpuFence() override;

        VulkanGpuFence(const VulkanGpuFence&) = delete;
        VulkanGpuFence& operator=(const VulkanGpuFence&) = delete;
        VulkanGpuFence(VulkanGpuFence&&) = delete;
        VulkanGpuFence& operator=(VulkanGpuFence&&) = delete;

        void QueueSignal(u64 value, RHI::FenceSignalOp op) override;
        void QueueWait(u64 value, RHI::FenceCompareOp compareOp) override;
        void HostSignal(u64 value, RHI::FenceSignalOp op) override;
        [[nodiscard]] bool HostWait(u64 value, u64 timeoutNanoseconds, RHI::FenceCompareOp compareOp) override;
        [[nodiscard]] u64 CompletedValue() const override;

        [[nodiscard]] VkSemaphore GetNativeSemaphore() const
        {
            return m_Semaphore;
        }

        // Drains every staged QueueSignal/QueueWait (across ALL live fences)
        // into a submit's semaphore-info lists. Called by the submit site
        // immediately before it fills VkSubmitInfo2 — the staged ops attach to
        // exactly one submission, in staging order. Wait stages are
        // ALL_COMMANDS: a split-barrier wait guards the whole consuming
        // submission (per-stage narrowing is a Phase 7 profiling refinement).
        static void DrainPendingSubmitOps(std::vector<VkSemaphoreSubmitInfo>& outWaits,
                                          std::vector<VkSemaphoreSubmitInfo>& outSignals);

        // Test/diagnostic affordance: staged-but-undrained op count.
        [[nodiscard]] static sizet GetPendingSubmitOpCount();

      private:
        VkSemaphore m_Semaphore = VK_NULL_HANDLE;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
