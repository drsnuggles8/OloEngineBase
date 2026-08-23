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
        // Depth, not a bool: ClearTextureFloat's load-time fallback records a
        // one-shot from inside another one's record callback.
        u32 s_ImmediateExecutionDepth = 0;
    } // namespace

    VulkanImageLayoutTracker::ImmediateExecutionScope::ImmediateExecutionScope()
    {
        ++s_ImmediateExecutionDepth;
    }

    VulkanImageLayoutTracker::ImmediateExecutionScope::~ImmediateExecutionScope()
    {
        if (s_ImmediateExecutionDepth > 0u)
            --s_ImmediateExecutionDepth;
    }

    bool VulkanImageLayoutTracker::InImmediateExecutionScope()
    {
        return s_ImmediateExecutionDepth > 0u;
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
                // Work recorded inside an immediate-execution scope reaches the
                // queue as it is recorded, so it advances the executed layout
                // too (issue #800).
                if (s_ImmediateExecutionDepth > 0u && index < state.Executed.size())
                    state.Executed[index] = layout;
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
