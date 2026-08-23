#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"

#if OLO_WITH_VULKAN

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        // Deliberately leaked, same rationale as this backend's other
        // process-wide registries: a tracker can be destroyed during static
        // teardown, and erasing from an already-destroyed vector is UB.
        std::vector<VulkanImageLayoutTracker*>& LiveTrackers()
        {
            static auto* s_Live = new std::vector<VulkanImageLayoutTracker*>();
            return *s_Live;
        }
    } // namespace

    VulkanImageLayoutTracker::VulkanImageLayoutTracker()
    {
        LiveTrackers().push_back(this);
    }

    VulkanImageLayoutTracker::~VulkanImageLayoutTracker()
    {
        std::erase(LiveTrackers(), this);
    }

    namespace
    {
        // One executed-layout write queued by an open immediate-execution
        // scope. RegistrationId is carried so a write cannot be applied to a
        // row that was re-registered in the meantime — the same
        // recycled-handle discipline the rest of this class uses.
        struct QueuedExecutedWrite
        {
            VulkanImageLayoutTracker* Tracker = nullptr;
            VkImage Image = VK_NULL_HANDLE;
            u64 RegistrationId = 0;
            sizet Index = 0;
            VkImageLayout Layout = VK_IMAGE_LAYOUT_UNDEFINED;
        };

        // A stack, one entry per open scope, so a nested submission's writes
        // are promoted (or dropped) independently of the outer one's.
        // Deliberately leaked, same rationale as LiveTrackers below.
        std::vector<std::vector<QueuedExecutedWrite>>& QueuedWriteStack()
        {
            static auto* s_Stack = new std::vector<std::vector<QueuedExecutedWrite>>();
            return *s_Stack;
        }
    } // namespace

    VulkanImageLayoutTracker::ImmediateExecutionScope::ImmediateExecutionScope()
    {
        QueuedWriteStack().emplace_back();
    }

    void VulkanImageLayoutTracker::ImmediateExecutionScope::MarkSubmitted()
    {
        m_Submitted = true;
    }

    VulkanImageLayoutTracker::ImmediateExecutionScope::~ImmediateExecutionScope()
    {
        auto& stack = QueuedWriteStack();
        if (stack.empty())
            return;
        const auto queued = std::move(stack.back());
        stack.pop_back();
        if (!m_Submitted)
            return; // never reached the queue — the writes were speculative

        const auto& live = LiveTrackers();
        for (const auto& write : queued)
        {
            // The tracker may have been destroyed while the scope was open
            // (a fixture teardown racing a submission).
            if (std::find(live.begin(), live.end(), write.Tracker) == live.end())
                continue;
            const auto it = write.Tracker->m_Images.find(write.Image);
            if (it == write.Tracker->m_Images.end() || it->second.RegistrationId != write.RegistrationId)
                continue; // forgotten, or a different image on a recycled handle value
            if (write.Index < it->second.Executed.size())
                it->second.Executed[write.Index] = write.Layout;
        }
    }

    bool VulkanImageLayoutTracker::InImmediateExecutionScope()
    {
        return !QueuedWriteStack().empty();
    }

    void VulkanImageLayoutTracker::ForgetImageEverywhere(const VkImage image)
    {
        for (auto* tracker : LiveTrackers())
            tracker->ForgetImage(image);
    }

    void VulkanImageLayoutTracker::RegisterImage(const VkImage image, const u32 mipCount, const u32 layerCount,
                                                 const u64 registrationId, const VkImageLayout initialLayout)
    {
        if (image == VK_NULL_HANDLE)
            return;

        const u32 mips = std::max(mipCount, 1u);
        const u32 layers = std::max(layerCount, 1u);

        if (const auto it = m_Images.find(image);
            it != m_Images.end() && it->second.MipCount == mips && it->second.LayerCount == layers &&
            it->second.RegistrationId == registrationId)
        {
            return; // idempotent re-registration of the SAME image
        }

        // New image, changed extents, or a recycled handle value carrying a
        // fresh registry stamp — all reset to the caller's initial layout
        // (UNDEFINED unless a load-time upload already placed the contents —
        // see the header note).
        ImageState state;
        state.MipCount = mips;
        state.LayerCount = layers;
        state.RegistrationId = registrationId;
        state.Layouts.assign(static_cast<sizet>(mips) * layers, initialLayout);
        // A freshly registered image has had no submitted work done to it, so
        // recorded and executed start equal (issue #800).
        state.Executed = state.Layouts;
        m_Images[image] = std::move(state);
    }

    void VulkanImageLayoutTracker::ForgetImage(const VkImage image)
    {
        m_Images.erase(image);
    }

    void VulkanImageLayoutTracker::Reset()
    {
        m_Images.clear();
    }

    VulkanImageLayoutTracker::ResolvedRange VulkanImageLayoutTracker::Resolve(const ImageState& state,
                                                                              const VkImageSubresourceRange& range)
    {
        ResolvedRange out;
        out.BaseMip = std::min(range.baseMipLevel, state.MipCount > 0u ? state.MipCount - 1u : 0u);
        out.BaseLayer = std::min(range.baseArrayLayer, state.LayerCount > 0u ? state.LayerCount - 1u : 0u);
        const u32 mipSpan = state.MipCount - out.BaseMip;
        const u32 layerSpan = state.LayerCount - out.BaseLayer;
        out.MipCount = (range.levelCount == VK_REMAINING_MIP_LEVELS) ? mipSpan : std::min(range.levelCount, mipSpan);
        out.LayerCount = (range.layerCount == VK_REMAINING_ARRAY_LAYERS) ? layerSpan : std::min(range.layerCount, layerSpan);
        return out;
    }

    void VulkanImageLayoutTracker::SetLayout(const VkImage image, const VkImageSubresourceRange& range, const VkImageLayout layout)
    {
        const auto it = m_Images.find(image);
        if (it == m_Images.end())
            return;

        auto& state = it->second;
        const auto r = Resolve(state, range);
        for (u32 layer = r.BaseLayer; layer < r.BaseLayer + r.LayerCount; ++layer)
        {
            for (u32 mip = r.BaseMip; mip < r.BaseMip + r.MipCount; ++mip)
            {
                const sizet index = static_cast<sizet>(layer) * state.MipCount + mip;
                state.Layouts[index] = layout;
                // Work recorded inside an immediate-execution scope will reach
                // the queue ahead of the frame command buffer, so it advances
                // the executed layout too (issue #800) — but only once it has
                // actually been submitted, which the scope promotes.
                if (!QueuedWriteStack().empty() && index < state.Executed.size())
                {
                    QueuedWriteStack().back().push_back(QueuedExecutedWrite{
                        .Tracker = this, .Image = image, .RegistrationId = state.RegistrationId, .Index = index, .Layout = layout });
                }
            }
        }
    }

    void VulkanImageLayoutTracker::CommitRecordedToExecuted()
    {
        for (auto& entry : m_Images)
            entry.second.Executed = entry.second.Layouts;
    }

    VkImageLayout VulkanImageLayoutTracker::CurrentExecutedLayout(const VkImage image,
                                                                  const VkImageSubresourceRange& range) const
    {
        const auto it = m_Images.find(image);
        if (it == m_Images.end())
            return VK_IMAGE_LAYOUT_UNDEFINED;

        const auto& state = it->second;
        const auto r = Resolve(state, range);
        if (r.MipCount == 0 || r.LayerCount == 0 || state.Executed.empty())
            return VK_IMAGE_LAYOUT_UNDEFINED;

        const auto first = state.Executed[static_cast<sizet>(r.BaseLayer) * state.MipCount + r.BaseMip];
        for (u32 layer = r.BaseLayer; layer < r.BaseLayer + r.LayerCount; ++layer)
        {
            for (u32 mip = r.BaseMip; mip < r.BaseMip + r.MipCount; ++mip)
            {
                if (state.Executed[static_cast<sizet>(layer) * state.MipCount + mip] != first)
                    return VK_IMAGE_LAYOUT_UNDEFINED; // mixed — no single layout to borrow
            }
        }
        return first;
    }

    void VulkanImageLayoutTracker::ForEachLayoutRun(const VkImage image,
                                                    const VkImageSubresourceRange& range,
                                                    const std::function<void(const VkImageSubresourceRange&, VkImageLayout)>& visitor) const
    {
        const auto it = m_Images.find(image);
        if (it == m_Images.end())
        {
            // Unknown image: one UNDEFINED run covering the requested range.
            visitor(range, VK_IMAGE_LAYOUT_UNDEFINED);
            return;
        }

        const auto& state = it->second;
        const auto r = Resolve(state, range);
        if (r.MipCount == 0 || r.LayerCount == 0)
            return;

        // Runs are emitted per layer as maximal contiguous mip spans of equal
        // layout. (A run could also merge across layers when every mip agrees;
        // per-layer is simpler and the barrier count difference is noise at
        // the layer counts this engine uses.)
        for (u32 layer = r.BaseLayer; layer < r.BaseLayer + r.LayerCount; ++layer)
        {
            u32 runStart = r.BaseMip;
            auto runLayout = state.Layouts[static_cast<sizet>(layer) * state.MipCount + runStart];
            for (u32 mip = r.BaseMip + 1u; mip <= r.BaseMip + r.MipCount; ++mip)
            {
                const bool atEnd = (mip == r.BaseMip + r.MipCount);
                const auto layout = atEnd ? VK_IMAGE_LAYOUT_MAX_ENUM
                                          : state.Layouts[static_cast<sizet>(layer) * state.MipCount + mip];
                if (atEnd || layout != runLayout)
                {
                    VkImageSubresourceRange run{};
                    run.aspectMask = range.aspectMask;
                    run.baseMipLevel = runStart;
                    run.levelCount = mip - runStart;
                    run.baseArrayLayer = layer;
                    run.layerCount = 1;
                    visitor(run, runLayout);
                    if (!atEnd)
                    {
                        runStart = mip;
                        runLayout = layout;
                    }
                }
            }
        }
    }

    VkImageLayout VulkanImageLayoutTracker::CurrentLayout(const VkImage image, const VkImageSubresourceRange& range) const
    {
        const auto it = m_Images.find(image);
        if (it == m_Images.end())
            return VK_IMAGE_LAYOUT_UNDEFINED;

        const auto& state = it->second;
        const auto r = Resolve(state, range);
        if (r.MipCount == 0 || r.LayerCount == 0)
            return VK_IMAGE_LAYOUT_UNDEFINED;

        const auto first = state.Layouts[static_cast<sizet>(r.BaseLayer) * state.MipCount + r.BaseMip];
        for (u32 layer = r.BaseLayer; layer < r.BaseLayer + r.LayerCount; ++layer)
        {
            for (u32 mip = r.BaseMip; mip < r.BaseMip + r.MipCount; ++mip)
            {
                if (state.Layouts[static_cast<sizet>(layer) * state.MipCount + mip] != first)
                    return VK_IMAGE_LAYOUT_UNDEFINED; // mixed — discard is the safe total answer
            }
        }
        return first;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
