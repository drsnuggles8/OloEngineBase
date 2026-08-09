#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanGpuFence.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace OloEngine
{
    namespace
    {
        // Staged queue-attached ops, drained by the next submit. Process-wide
        // and render-thread-only, same discipline as VulkanDeferredReclaim.
        // The semaphore handle is captured raw: a fence whose destructor runs
        // with ops still staged would dangle here, so ~VulkanGpuFence purges
        // its own entries before enqueueing the semaphore for reclaim.
        struct PendingSubmitOp
        {
            VkSemaphore Semaphore = VK_NULL_HANDLE;
            u64 Value = 0;
            bool IsSignal = false;
        };
        std::vector<PendingSubmitOp>& PendingOps()
        {
            static auto* s_Ops = new std::vector<PendingSubmitOp>(); // deliberately leaked
            return *s_Ops;
        }

        VkDevice RequireDevice(const char* what)
        {
            auto* device = VulkanDevice::Get();
            OLO_CORE_ASSERT(device != nullptr, "VulkanGpuFence: no live VulkanDevice");
            if (device == nullptr)
            {
                throw std::runtime_error(std::string("VulkanGpuFence: ") + what + " requires a live VulkanDevice");
            }
            return device->GetDevice();
        }

        // The largest signal value staged (and not yet drained) for one
        // semaphore; 0 when none. The dispenser's reservation floor cannot
        // answer this — it also covers values DISPENSED but not yet
        // signalled, and rejecting those would break the ordinary
        // NextValue() → QueueSignal(same value) pattern.
        [[nodiscard]] u64 MaxPendingSignalValueFor(VkSemaphore semaphore)
        {
            u64 maxValue = 0;
            for (const PendingSubmitOp& op : PendingOps())
            {
                if (op.IsSignal && op.Semaphore == semaphore)
                {
                    maxValue = std::max(maxValue, op.Value);
                }
            }
            return maxValue;
        }

        [[nodiscard]] bool HasPendingSignalAtOrBelow(VkSemaphore semaphore, u64 value)
        {
            for (const PendingSubmitOp& op : PendingOps())
            {
                if (op.IsSignal && op.Semaphore == semaphore && op.Value <= value)
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    VulkanGpuFence::VulkanGpuFence(u64 initialValue) : RHI::GpuFence(initialValue)
    {
        const VkDevice device = RequireDevice("construction");

        VkSemaphoreTypeCreateInfo typeInfo{};
        typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeInfo.initialValue = initialValue;

        VkSemaphoreCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        createInfo.pNext = &typeInfo;

        if (vkCreateSemaphore(device, &createInfo, nullptr, &m_Semaphore) != VK_SUCCESS)
        {
            throw std::runtime_error("VulkanGpuFence: vkCreateSemaphore (timeline) failed");
        }
    }

    VulkanGpuFence::~VulkanGpuFence()
    {
        // Ops staged against this fence but never drained would dangle in the
        // pending list — purge them (their dependency is unenforceable once
        // the fence is gone; a WARN beats a use-after-free in a submit).
        auto& ops = PendingOps();
        const sizet before = ops.size();
        std::erase_if(ops, [this](const PendingSubmitOp& op)
                      { return op.Semaphore == m_Semaphore; });
        if (ops.size() != before)
        {
            OLO_CORE_WARN("VulkanGpuFence: destroyed with {} staged submit op(s) never drained",
                          before - ops.size());
        }

        // An in-flight submit may still reference the semaphore — deferred
        // destruction through the same generation queue as every other
        // backend object.
        VulkanDeferredReclaim::Get().Enqueue(m_Semaphore);
        m_Semaphore = VK_NULL_HANDLE;
    }

    void VulkanGpuFence::QueueSignal(u64 value, RHI::FenceSignalOp op)
    {
        // Timeline semaphores are monotonic regardless of the requested op —
        // Set and AtomicMax coincide as long as values increase, which the
        // NextValue() dispenser guarantees and mixed callers must respect.
        // Best-effort staging-time check: the violation would otherwise only
        // surface as a validation error (or device loss) at submit time, far
        // from the caller that staged the bad value.
        OLO_CORE_ASSERT(value > CompletedValue(),
                        "VulkanGpuFence::QueueSignal: timeline values must strictly increase");
        (void)op;
        // Ordering against SIGNALS ALREADY STAGED for this fence: two staged
        // signals drain into one submission in staging order, so a value at
        // or below an earlier staged one is guaranteed-invalid at submit
        // time. Reject it HERE, where the caller is — staging it anyway
        // would fail far away, inside the frame loop's vkQueueSubmit2.
        const u64 maxPending = MaxPendingSignalValueFor(m_Semaphore);
        if (maxPending != 0 && value <= maxPending)
        {
            // Reject (not assert): the rejection IS the contract, pinned by
            // the device-gated test — and the error names the caller's bug.
            OLO_CORE_ERROR("VulkanGpuFence::QueueSignal({}) rejected — a staged signal of {} is already pending",
                           value, maxPending);
            return;
        }
        // Reserve the value with the dispenser: a staged-but-unsubmitted
        // signal is invisible to the live counter, and NextValue() must not
        // hand out anything at or below it.
        NoteSignalValue(value);
        PendingOps().push_back({ .Semaphore = m_Semaphore, .Value = value, .IsSignal = true });
    }

    void VulkanGpuFence::QueueWait(u64 value, RHI::FenceCompareOp compareOp)
    {
        // On a monotonic counter, "== value" is satisfied exactly when the
        // counter has reached value at least once, which is what >= detects —
        // the two compare ops lower identically here.
        (void)compareOp;
        PendingOps().push_back({ .Semaphore = m_Semaphore, .Value = value, .IsSignal = false });
    }

    void VulkanGpuFence::HostSignal(u64 value, RHI::FenceSignalOp op)
    {
        (void)op;
        const VkDevice device = RequireDevice("HostSignal");
        OLO_CORE_ASSERT(value > CompletedValue(),
                        "VulkanGpuFence::HostSignal: timeline values must strictly increase");

        // A host signal executes NOW; a staged queue signal executes at the
        // next submit. Advancing the counter past a still-pending staged
        // value would make that staged signal invalid when it drains — this
        // path cannot flush the queue (no submission context), so the host
        // signal is rejected instead.
        if (HasPendingSignalAtOrBelow(m_Semaphore, value))
        {
            // Reject (not assert) — same contract shape as QueueSignal's
            // ordering check above.
            OLO_CORE_ERROR("VulkanGpuFence::HostSignal({}) rejected — it would invalidate a pending staged signal "
                           "(drain the submit first)",
                           value);
            return;
        }

        NoteSignalValue(value); // same reservation rule as QueueSignal

        VkSemaphoreSignalInfo signalInfo{};
        signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        signalInfo.semaphore = m_Semaphore;
        signalInfo.value = value;
        if (vkSignalSemaphore(device, &signalInfo) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanGpuFence: vkSignalSemaphore({}) failed", value);
        }
    }

    bool VulkanGpuFence::HostWait(u64 value, u64 timeoutNanoseconds, RHI::FenceCompareOp compareOp)
    {
        (void)compareOp; // >= is the native wait; == coincides on a monotonic counter.
        const VkDevice device = RequireDevice("HostWait");

        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &m_Semaphore;
        waitInfo.pValues = &value;

        return vkWaitSemaphores(device, &waitInfo, timeoutNanoseconds) == VK_SUCCESS;
    }

    u64 VulkanGpuFence::CompletedValue() const
    {
        const VkDevice device = RequireDevice("CompletedValue");
        u64 value = 0;
        if (vkGetSemaphoreCounterValue(device, m_Semaphore, &value) != VK_SUCCESS)
        {
            // A failed read must be DISTINCT from a valid zero counter:
            // returning 0 here would let NextValue() dispense below the real
            // timeline. UINT64_MAX is the dispenser's exhaustion/poison
            // sentinel — NextValue() saturates on it, so the fence becomes
            // unusable for further dispensing instead of silently wrong
            // (a counter read only fails on device loss, where every later
            // submit is dead anyway).
            OLO_CORE_ERROR("VulkanGpuFence: vkGetSemaphoreCounterValue failed — poisoning the fence");
            return std::numeric_limits<u64>::max();
        }
        return value;
    }

    void VulkanGpuFence::DrainPendingSubmitOps(std::vector<VkSemaphoreSubmitInfo>& outWaits,
                                               std::vector<VkSemaphoreSubmitInfo>& outSignals)
    {
        auto& ops = PendingOps();
        for (const PendingSubmitOp& op : ops)
        {
            VkSemaphoreSubmitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            info.semaphore = op.Semaphore;
            info.value = op.Value;
            info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            (op.IsSignal ? outSignals : outWaits).push_back(info);
        }
        ops.clear();
    }

    sizet VulkanGpuFence::GetPendingSubmitOpCount()
    {
        return PendingOps().size();
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
