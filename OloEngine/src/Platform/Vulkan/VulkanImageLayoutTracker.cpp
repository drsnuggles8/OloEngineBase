#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"

#if OLO_WITH_VULKAN

#include <algorithm>

namespace OloEngine
{
    void VulkanImageLayoutTracker::RegisterImage(const VkImage image, const u32 mipCount, const u32 layerCount, const u64 registrationId)
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
        // fresh registry stamp — all reset to UNDEFINED.
        ImageState state;
        state.MipCount = mips;
        state.LayerCount = layers;
        state.RegistrationId = registrationId;
        state.Layouts.assign(static_cast<sizet>(mips) * layers, VK_IMAGE_LAYOUT_UNDEFINED);
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
                state.Layouts[static_cast<sizet>(layer) * state.MipCount + mip] = layout;
        }
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
