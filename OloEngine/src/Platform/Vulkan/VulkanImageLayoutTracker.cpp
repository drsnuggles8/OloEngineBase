#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"

#if OLO_WITH_VULKAN

#include <algorithm>
#include <cstdint>

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

        // An overlay registering an image its base already tracks with the
        // same extents and stamp is the idempotent case above, seen through
        // the read-through: copy the base's row in rather than resetting the
        // image to `initialLayout`, which would discard what the render
        // thread already recorded for it (#806).
        if (m_Base != nullptr)
        {
            if (const auto baseIt = m_Base->m_Images.find(image);
                baseIt != m_Base->m_Images.end() && baseIt->second.MipCount == mips &&
                baseIt->second.LayerCount == layers && baseIt->second.RegistrationId == registrationId)
            {
                (void)CopyRowFromBase(image);
                return;
            }
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
        if (m_Base != nullptr)
        {
            // A row the base never saw: everything is "original" and nothing
            // is written yet; the merge inserts it whole.
            state.Original = state.Layouts;
            state.Written.assign(state.Layouts.size(), 0u);
        }
        m_Images[image] = std::move(state);
    }

    VulkanImageLayoutTracker::ImageState* VulkanImageLayoutTracker::CopyRowFromBase(const VkImage image)
    {
        if (m_Base == nullptr)
            return nullptr;
        const auto baseIt = m_Base->m_Images.find(image);
        if (baseIt == m_Base->m_Images.end())
            return nullptr;
        ImageState copy;
        copy.MipCount = baseIt->second.MipCount;
        copy.LayerCount = baseIt->second.LayerCount;
        copy.RegistrationId = baseIt->second.RegistrationId;
        copy.Layouts = baseIt->second.Layouts;
        copy.Executed = baseIt->second.Executed;
        copy.Original = baseIt->second.Layouts;
        copy.Written.assign(copy.Layouts.size(), 0u);
        auto [it, inserted] = m_Images.insert_or_assign(image, std::move(copy));
        (void)inserted;
        return &it->second;
    }

    void VulkanLayoutClaimTable::Reset()
    {
        const std::scoped_lock lock(m_Mutex);
        m_Claims.clear();
    }

    u32 VulkanLayoutClaimTable::Claim(const VkImage image, const sizet subresource, const u32 item)
    {
        const std::scoped_lock lock(m_Mutex);
        const auto [it, inserted] = m_Claims.try_emplace(Key{ .Image = image, .Subresource = subresource }, item);
        return (inserted || it->second == item) ? kNone : it->second;
    }

    void VulkanImageLayoutTracker::SetReadThroughBase(const VulkanImageLayoutTracker* base,
                                                      VulkanLayoutClaimTable* claims, const u32 itemIndex)
    {
        OLO_CORE_ASSERT(base != this, "a tracker cannot overlay itself");
        m_Base = base;
        m_Claims = base != nullptr ? claims : nullptr;
        m_ItemIndex = itemIndex;
        m_Images.clear();
    }

    void VulkanImageLayoutTracker::DiscardOverlay()
    {
        m_Images.clear();
    }

    void VulkanImageLayoutTracker::MergeOverlayInto(VulkanImageLayoutTracker& base, MergeBatch& batch)
    {
        for (auto& [image, row] : m_Images)
        {
            auto baseIt = base.m_Images.find(image);
            if (baseIt == base.m_Images.end())
            {
                // Registered by this item alone: the base row starts as what
                // the item registered it with, and the loop below writes the
                // subresources the item then transitioned like any other.
                ImageState fresh;
                fresh.MipCount = row.MipCount;
                fresh.LayerCount = row.LayerCount;
                fresh.RegistrationId = row.RegistrationId;
                fresh.Layouts = row.Original;
                fresh.Executed = row.Executed;
                baseIt = base.m_Images.insert_or_assign(image, std::move(fresh)).first;
            }
            auto& target = baseIt->second;
            if (target.MipCount != row.MipCount || target.LayerCount != row.LayerCount ||
                target.RegistrationId != row.RegistrationId || target.Layouts.size() != row.Layouts.size())
            {
                // The base re-registered the image under the item's feet —
                // impossible while the base is frozen; refuse rather than
                // write past the row.
                OLO_CORE_ERROR("[RHI/Vulkan] parallel recording: layout overlay row for image {:#x} no longer "
                               "matches its base — item writes dropped",
                               reinterpret_cast<std::uintptr_t>(image));
                ++batch.Conflicts;
                continue;
            }
            // Every overlay row carries Original/Written (CopyRowFromBase and
            // the overlay arm of RegisterImage both fill them).
            OLO_CORE_ASSERT(row.Written.size() == row.Layouts.size() && row.Original.size() == row.Layouts.size(),
                            "overlay row without its Original/Written mirrors");
            auto& mark = batch.Written[image];
            if (mark.size() != row.Layouts.size())
                mark.assign(row.Layouts.size(), 0u);
            for (sizet i = 0; i < row.Layouts.size(); ++i)
            {
                if (row.Written[i] == 0u)
                    continue;
                const bool identity = row.Original[i] == row.Layouts[i];
                // Amendment (91) rule 5: overlap is legal only when every
                // writer's transition was an identity — then each item's
                // barrier named the layout the image really was in.
                if (mark[i] != 0u && (mark[i] == 2u || !identity))
                    ++batch.Conflicts;
                mark[i] = identity ? std::max<u8>(mark[i], 1u) : 2u;
                target.Layouts[i] = row.Layouts[i];
                ++batch.SubresourcesMerged;
            }
        }
        m_Images.clear();
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
        auto it = m_Images.find(image);
        if (it == m_Images.end())
        {
            // Overlay copy-on-write: the first write to an image the base
            // tracks brings its row in (#806).
            if (CopyRowFromBase(image) == nullptr)
                return;
            it = m_Images.find(image);
        }

        auto& state = it->second;
        const auto r = Resolve(state, range);
        for (u32 layer = r.BaseLayer; layer < r.BaseLayer + r.LayerCount; ++layer)
        {
            for (u32 mip = r.BaseMip; mip < r.BaseMip + r.MipCount; ++mip)
            {
                const sizet index = static_cast<sizet>(layer) * state.MipCount + mip;
                if (index < state.Written.size())
                {
                    state.Written[index] = 1u;
                    // Record-time half of amendment (91) rule 5: a non-identity
                    // transition claims the subresource for this item, and a
                    // second item's claim is reported HERE, with both indices,
                    // where the stale oldLayout is being recorded.
                    if (m_Claims != nullptr && state.Original[index] != layout)
                    {
                        if (const u32 other = m_Claims->Claim(image, index, m_ItemIndex);
                            other != VulkanLayoutClaimTable::kNone)
                        {
                            OLO_CORE_ERROR("[RHI/Vulkan] RecordParallel item {} transitions image {:#x} subresource "
                                           "{} that item {} already transitioned — its barrier names a stale "
                                           "oldLayout (amendment (91) rule 5)",
                                           m_ItemIndex, reinterpret_cast<std::uintptr_t>(image), index, other);
                            OLO_CORE_ASSERT(false, "two RecordParallel items transitioned one subresource");
                        }
                    }
                }
                state.Layouts[index] = layout;
                // Work recorded inside an immediate-execution scope will reach
                // the queue ahead of the frame command buffer, so it advances
                // the executed layout too (issue #800) — but only once it has
                // actually been submitted, which the scope promotes. An
                // overlay never sees one (one-shots are refused on worker
                // contexts), so it never queues.
                if (m_Base == nullptr && !QueuedWriteStack().empty() && index < state.Executed.size())
                {
                    QueuedWriteStack().back().push_back(QueuedExecutedWrite{
                        .Tracker = this, .Image = image, .RegistrationId = state.RegistrationId, .Index = index, .Layout = layout });
                }
            }
        }
    }

    void VulkanImageLayoutTracker::CommitRecordedToExecuted()
    {
        // An overlay's work is committed by its base's EndRecording, after
        // the merge; committing here would promote unmerged, item-local state.
        OLO_CORE_ASSERT(m_Base == nullptr, "CommitRecordedToExecuted on a layout overlay");
        if (m_Base != nullptr)
            return;
        for (auto& entry : m_Images)
            entry.second.Executed = entry.second.Layouts;
    }

    VkImageLayout VulkanImageLayoutTracker::CurrentExecutedLayout(const VkImage image,
                                                                  const VkImageSubresourceRange& range) const
    {
        const auto it = m_Images.find(image);
        if (it == m_Images.end())
            return m_Base != nullptr ? m_Base->CurrentExecutedLayout(image, range) : VK_IMAGE_LAYOUT_UNDEFINED;

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
            if (m_Base != nullptr)
            {
                m_Base->ForEachLayoutRun(image, range, visitor);
                return;
            }
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
            return m_Base != nullptr ? m_Base->CurrentLayout(image, range) : VK_IMAGE_LAYOUT_UNDEFINED;

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
