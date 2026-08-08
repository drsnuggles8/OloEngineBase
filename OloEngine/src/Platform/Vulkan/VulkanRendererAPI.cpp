#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "Platform/Vulkan/VulkanBarrierLowering.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <bit>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Conservative both-ways masks for the debug clears and the
        // flags-only global barrier: the poison instrument and a
        // transitions-empty batch have no per-resource truth to be precise
        // with, and over-synchronisation is correct where under- is a race.
        constexpr VkPipelineStageFlags2 kAllStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        constexpr VkAccessFlags2 kAllAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

        [[nodiscard]] RHI::TextureAspect AspectFromInfo(const VulkanImageInfo& info)
        {
            if (info.HasDepth && info.HasStencil)
                return RHI::TextureAspect::DepthStencil;
            if (info.HasDepth)
                return RHI::TextureAspect::Depth;
            if (info.HasStencil)
                return RHI::TextureAspect::Stencil;
            return RHI::TextureAspect::Color;
        }
    } // namespace

    void VulkanRendererAPI::Phase6Stub(const char* entryPoint) const
    {
        ++m_Phase6StubHits;
        if (m_WarnedStubs.insert(entryPoint).second)
        {
            OLO_CORE_WARN("[RHI/Vulkan] {} is a Phase 6 concern (#691) — no-op under the Phase 5 execution layer "
                          "(further calls counted, not logged)",
                          entryPoint);
        }
    }

    void VulkanRendererAPI::CacheDeviceLimits()
    {
        if (m_LimitsCached)
            return;
        auto* device = VulkanDevice::Get();
        if (!device)
            return;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device->GetPhysicalDevice(), &props);
        m_MaxUniformBlockSize = props.limits.maxUniformBufferRange;

        const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                          props.limits.framebufferDepthSampleCounts;
        m_MaxSamples = 1;
        for (const VkSampleCountFlagBits bit : { VK_SAMPLE_COUNT_64_BIT, VK_SAMPLE_COUNT_32_BIT,
                                                 VK_SAMPLE_COUNT_16_BIT, VK_SAMPLE_COUNT_8_BIT,
                                                 VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT })
        {
            if (counts & bit)
            {
                m_MaxSamples = static_cast<u32>(bit);
                break;
            }
        }

        VkPhysicalDeviceShaderAtomicInt64Features atomics{};
        atomics.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &atomics;
        vkGetPhysicalDeviceFeatures2(device->GetPhysicalDevice(), &features2);
        m_SupportsInt64Atomics = atomics.shaderBufferInt64Atomics == VK_TRUE;

        // VUID-…-03929/-03930: a barrier stage mask may only name stages
        // whose device features are enabled.
        m_EnabledStageMask = ~VkPipelineStageFlags2{ 0 };
        if (!device->IsTessellationShaderEnabled())
        {
            m_EnabledStageMask &= ~(VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT);
        }
        if (!device->IsGeometryShaderEnabled())
            m_EnabledStageMask &= ~VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;

        m_LimitsCached = true;
    }

    void VulkanRendererAPI::Init()
    {
        OLO_PROFILE_FUNCTION();
        CacheDeviceLimits();
        OLO_CORE_INFO("[RHI/Vulkan] VulkanRendererAPI up — Phase 5 execution layer (barriers, transient clears, "
                      "dynamic state); pipeline-shaped entry points stub until Phase 6");
    }

    void VulkanRendererAPI::BeginRecording(const VkCommandBuffer cmd)
    {
        OLO_CORE_ASSERT(m_Cmd == VK_NULL_HANDLE, "BeginRecording while a recording bracket is already open");
        m_Cmd = cmd;
    }

    void VulkanRendererAPI::EndRecording()
    {
        m_Cmd = VK_NULL_HANDLE;
    }

    // --- Viewport / scissor (real dynamic state) ---------------------------

    void VulkanRendererAPI::SetViewport(const u32 x, const u32 y, const u32 width, const u32 height)
    {
        m_Viewport = { x, y, width, height };
        if (m_Cmd == VK_NULL_HANDLE)
            return;

        // Negative-height flip is a Phase 6 (pipeline/pass) concern; Phase 5
        // records the plain rect so state packets replay without error.
        VkViewport vp{};
        vp.x = static_cast<f32>(x);
        vp.y = static_cast<f32>(y);
        vp.width = static_cast<f32>(width);
        vp.height = static_cast<f32>(height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(m_Cmd, 0, 1, &vp);

        VkRect2D scissor{};
        scissor.offset = { static_cast<i32>(x), static_cast<i32>(y) };
        scissor.extent = { width, height };
        vkCmdSetScissor(m_Cmd, 0, 1, &scissor);
    }

    Viewport VulkanRendererAPI::GetViewport() const
    {
        return m_Viewport;
    }

    void VulkanRendererAPI::SetScissorBox(const i32 x, const i32 y, const u32 width, const u32 height)
    {
        if (m_Cmd == VK_NULL_HANDLE)
            return;
        VkRect2D scissor{};
        scissor.offset = { x, y };
        scissor.extent = { width, height };
        vkCmdSetScissor(m_Cmd, 0, 1, &scissor);
    }

    // --- Barriers ----------------------------------------------------------

    void VulkanRendererAPI::MemoryBarrier(MemoryBarrierFlags flags)
    {
        // The flags are the GL lowering (ADR 0011 §1.5); on Vulkan a bare
        // MemoryBarrier call has no per-resource truth, so it lowers to one
        // conservative global memory barrier.
        if (flags == MemoryBarrierFlags::None)
            return;
        if (m_Cmd == VK_NULL_HANDLE)
        {
            Phase6Stub("MemoryBarrier(outside recording bracket)");
            return;
        }

        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = kAllStages;
        barrier.srcAccessMask = kAllAccess;
        barrier.dstStageMask = kAllStages;
        barrier.dstAccessMask = kAllAccess;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(m_Cmd, &dep);
    }

    void VulkanRendererAPI::IssueBarrierBatch(const MemoryBarrierFlags flags, std::span<const RHI::Barrier> barriers)
    {
        OLO_PROFILE_FUNCTION();

        // The enabled-stage narrowing below depends on the device's feature
        // set, and this instance may have been constructed before the device
        // existed (RecreateForSelectedBackend runs pre-window, and Init() is
        // skipped while Renderer::Init is). CacheDeviceLimits early-outs once
        // cached, so this is a no-op on the steady path.
        CacheDeviceLimits();

        if (m_Cmd == VK_NULL_HANDLE)
        {
            if (flags != MemoryBarrierFlags::None || !barriers.empty())
                Phase6Stub("IssueBarrierBatch(outside recording bracket)");
            return;
        }

        std::vector<VkImageMemoryBarrier2> imageBarriers;
        std::vector<VkBufferMemoryBarrier2> bufferBarriers;
        imageBarriers.reserve(barriers.size());

        // Barriers that cannot resolve to a native object (stale/dead/foreign
        // handles, non-Vulkan images in a mixed-currency graph) must not
        // silently drop the synchronisation the batch promised for them — a
        // resolved-only batch would order SOME of the pass's inputs and race
        // the rest. They fall back to the conservative global barrier below.
        bool anyUnresolved = false;

        auto& registry = RHI::ResourceRegistry::Get();
        for (const auto& barrier : barriers)
        {
            const auto kind = registry.KindOf(barrier.Resource);
            const u64 native = registry.ResolveNativeForBackend(barrier.Resource);
            if (native == 0u)
            {
                anyUnresolved = true; // degrades to the global fallback, never to garbage
                continue;
            }

            if (kind == RHI::ResourceKind::Buffer)
            {
                auto vkBarrier = VulkanBarrierLowering::BuildBufferBarrier(barrier, reinterpret_cast<VkBuffer>(native));
                vkBarrier.srcStageMask &= m_EnabledStageMask;
                vkBarrier.dstStageMask &= m_EnabledStageMask;
                bufferBarriers.push_back(vkBarrier);
                continue;
            }
            if (kind != RHI::ResourceKind::Texture)
            {
                anyUnresolved = true;
                continue;
            }

            const auto image = reinterpret_cast<VkImage>(native);
            const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
            if (!info)
            {
                anyUnresolved = true; // not a Vulkan-backend image (mixed-currency graph)
                continue;
            }

            const auto aspect = AspectFromInfo(*info);
            m_LayoutTracker.RegisterImage(image, info->MipLevels, info->ArrayLayers, info->RegistrationId);

            // The neutral range, clamped into Vk terms once; the tracker then
            // splits it into runs of equal current layout so oldLayout is
            // EXACT per emitted barrier — one guessed layout for a mixed
            // range is precisely the class of bug sync validation exists for.
            VkImageSubresourceRange queryRange{};
            queryRange.aspectMask = VulkanBarrierLowering::AspectMaskFor(aspect);
            queryRange.baseMipLevel = std::min(barrier.Range.BaseMip, info->MipLevels - 1u);
            queryRange.levelCount = (barrier.Range.MipCount == RHI::SubresourceRange::AllRemaining)
                                        ? VK_REMAINING_MIP_LEVELS
                                        : std::min(barrier.Range.MipCount, info->MipLevels - queryRange.baseMipLevel);
            queryRange.baseArrayLayer = std::min(barrier.Range.BaseLayer, info->ArrayLayers - 1u);
            queryRange.layerCount = (barrier.Range.LayerCount == RHI::SubresourceRange::AllRemaining)
                                        ? VK_REMAINING_ARRAY_LAYERS
                                        : std::min(barrier.Range.LayerCount, info->ArrayLayers - queryRange.baseArrayLayer);

            m_LayoutTracker.ForEachLayoutRun(
                image, queryRange,
                [&](const VkImageSubresourceRange& run, const VkImageLayout trackedLayout)
                {
                    auto vkBarrier = VulkanBarrierLowering::BuildImageBarrier(barrier, image, aspect, trackedLayout,
                                                                              info->MipLevels, info->ArrayLayers);
                    vkBarrier.subresourceRange = run;
                    // The pure lowering emits the full shader-stage union;
                    // the device knows which stage features are ENABLED
                    // (VUID-…-03929/-03930). A masked-off stage can hold no
                    // work, so narrowing loses no synchronisation.
                    vkBarrier.srcStageMask &= m_EnabledStageMask;
                    vkBarrier.dstStageMask &= m_EnabledStageMask;
                    imageBarriers.push_back(vkBarrier);
                });

            m_LayoutTracker.SetLayout(image, queryRange,
                                      VulkanBarrierLowering::LayoutFor(barrier.After, aspect, barrier.ReadWhileAttached));
        }

        VkMemoryBarrier2 globalBarrier{};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;

        // The conservative global barrier fires whenever the per-resource set
        // is INCOMPLETE: a flags-only batch (no transitions at all), or a
        // MIXED batch where some operands could not resolve — the flags
        // promised synchronisation for those too, and one dependency info can
        // carry the global barrier alongside the resolved image/buffer
        // barriers (execution+memory coverage for the unresolved remainder;
        // layout transitions for them stay impossible without a native image,
        // which is exactly why a Vulkan graph must import by handle).
        const bool needsGlobalFallback =
            flags != MemoryBarrierFlags::None &&
            (anyUnresolved || (imageBarriers.empty() && bufferBarriers.empty()));

        if (!needsGlobalFallback && imageBarriers.empty() && bufferBarriers.empty())
            return;

        if (needsGlobalFallback)
        {
            globalBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            globalBarrier.srcStageMask = kAllStages;
            globalBarrier.srcAccessMask = kAllAccess;
            globalBarrier.dstStageMask = kAllStages;
            globalBarrier.dstAccessMask = kAllAccess;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &globalBarrier;
        }
        dep.imageMemoryBarrierCount = static_cast<u32>(imageBarriers.size());
        dep.pImageMemoryBarriers = imageBarriers.empty() ? nullptr : imageBarriers.data();
        dep.bufferMemoryBarrierCount = static_cast<u32>(bufferBarriers.size());
        dep.pBufferMemoryBarriers = bufferBarriers.empty() ? nullptr : bufferBarriers.data();

        vkCmdPipelineBarrier2(m_Cmd, &dep);
    }

    // --- Transient clears (the poison instrument's backend) ----------------

    void VulkanRendererAPI::ClearTextureFloat(const RHI::ResourceHandle texture, const u32 mipLevel, const glm::vec4& color)
    {
        if (m_Cmd == VK_NULL_HANDLE)
        {
            Phase6Stub("ClearTextureFloat(outside recording bracket)");
            return;
        }

        auto& registry = RHI::ResourceRegistry::Get();
        const u64 native = registry.ResolveNativeForBackend(texture);
        if (native == 0u)
            return;
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (!info)
            return;

        const auto aspect = AspectFromInfo(*info);
        m_LayoutTracker.RegisterImage(image, info->MipLevels, info->ArrayLayers, info->RegistrationId);

        VkImageSubresourceRange range{};
        range.aspectMask = VulkanBarrierLowering::AspectMaskFor(aspect);
        range.baseMipLevel = std::min(mipLevel, info->MipLevels - 1u);
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = VK_REMAINING_ARRAY_LAYERS;

        // Conservative pre-barrier into TRANSFER_DST, exact per layout run.
        std::vector<VkImageMemoryBarrier2> toTransfer;
        m_LayoutTracker.ForEachLayoutRun(
            image, range,
            [&](const VkImageSubresourceRange& run, const VkImageLayout trackedLayout)
            {
                VkImageMemoryBarrier2 b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask = (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_2_NONE : kAllStages;
                b.srcAccessMask = (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ACCESS_2_NONE : kAllAccess;
                b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                b.oldLayout = trackedLayout;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = run;
                toTransfer.push_back(b);
            });
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<u32>(toTransfer.size());
        dep.pImageMemoryBarriers = toTransfer.data();
        vkCmdPipelineBarrier2(m_Cmd, &dep);
        m_LayoutTracker.SetLayout(image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        if (aspect == RHI::TextureAspect::Color)
        {
            VkClearColorValue clear{};
            clear.float32[0] = color.r;
            clear.float32[1] = color.g;
            clear.float32[2] = color.b;
            clear.float32[3] = color.a;
            vkCmdClearColorImage(m_Cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        }
        else
        {
            VkClearDepthStencilValue clear{};
            clear.depth = color.r;
            clear.stencil = 0;
            vkCmdClearDepthStencilImage(m_Cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        }
    }

    void VulkanRendererAPI::ClearTextureUInt(const RHI::ResourceHandle texture, const u32 mipLevel, const u32 value)
    {
        if (m_Cmd == VK_NULL_HANDLE)
        {
            Phase6Stub("ClearTextureUInt(outside recording bracket)");
            return;
        }
        // Same shape as the float clear with a uint payload; share the
        // transition by delegating and then re-clearing would double-clear,
        // so the small duplication is deliberate.
        auto& registry = RHI::ResourceRegistry::Get();
        const u64 native = registry.ResolveNativeForBackend(texture);
        if (native == 0u)
            return;
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (!info || info->HasDepth || info->HasStencil)
            return; // a uint clear of a depth image has no meaning

        m_LayoutTracker.RegisterImage(image, info->MipLevels, info->ArrayLayers, info->RegistrationId);

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = std::min(mipLevel, info->MipLevels - 1u);
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = VK_REMAINING_ARRAY_LAYERS;

        std::vector<VkImageMemoryBarrier2> toTransfer;
        m_LayoutTracker.ForEachLayoutRun(
            image, range,
            [&](const VkImageSubresourceRange& run, const VkImageLayout trackedLayout)
            {
                VkImageMemoryBarrier2 b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask = (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_2_NONE : kAllStages;
                b.srcAccessMask = (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ACCESS_2_NONE : kAllAccess;
                b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                b.oldLayout = trackedLayout;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = run;
                toTransfer.push_back(b);
            });
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<u32>(toTransfer.size());
        dep.pImageMemoryBarriers = toTransfer.data();
        vkCmdPipelineBarrier2(m_Cmd, &dep);
        m_LayoutTracker.SetLayout(image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkClearColorValue clear{};
        clear.uint32[0] = value;
        clear.uint32[1] = value;
        clear.uint32[2] = value;
        clear.uint32[3] = value;
        vkCmdClearColorImage(m_Cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }

    void VulkanRendererAPI::ClearBufferFloat(const RHI::ResourceHandle buffer, const f32 value)
    {
        if (m_Cmd == VK_NULL_HANDLE)
        {
            Phase6Stub("ClearBufferFloat(outside recording bracket)");
            return;
        }
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(buffer);
        if (native == 0u)
            return;

        // Conservative bracket: the poison fill must be ordered against
        // whatever used the pooled buffer last frame and whatever reads it
        // next — this is a debug instrument, over-sync is the point.
        VkMemoryBarrier2 global{};
        global.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        global.srcStageMask = kAllStages;
        global.srcAccessMask = kAllAccess;
        global.dstStageMask = kAllStages;
        global.dstAccessMask = kAllAccess;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &global;

        vkCmdPipelineBarrier2(m_Cmd, &dep);
        vkCmdFillBuffer(m_Cmd, reinterpret_cast<VkBuffer>(native), 0, VK_WHOLE_SIZE, std::bit_cast<u32>(value));
        vkCmdPipelineBarrier2(m_Cmd, &dep);
    }

    void VulkanRendererAPI::ClearBufferUInt(const RHI::ResourceHandle buffer, const u32 value)
    {
        if (m_Cmd == VK_NULL_HANDLE)
        {
            Phase6Stub("ClearBufferUInt(outside recording bracket)");
            return;
        }
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(buffer);
        if (native == 0u)
            return;

        VkMemoryBarrier2 global{};
        global.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        global.srcStageMask = kAllStages;
        global.srcAccessMask = kAllAccess;
        global.dstStageMask = kAllStages;
        global.dstAccessMask = kAllAccess;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &global;

        vkCmdPipelineBarrier2(m_Cmd, &dep);
        vkCmdFillBuffer(m_Cmd, reinterpret_cast<VkBuffer>(native), 0, VK_WHOLE_SIZE, value);
        vkCmdPipelineBarrier2(m_Cmd, &dep);
    }

    // --- Debug labels / device queries -------------------------------------

    void VulkanRendererAPI::PushDebugGroup(u32 /*id*/, const std::string_view label)
    {
        // vkCmdBeginDebugUtilsLabelEXT is loaded only when the debug-utils
        // extension was enabled (debug builds with the validation layer) —
        // the pointer probe IS the capability check under volk.
        if (m_Cmd == VK_NULL_HANDLE || vkCmdBeginDebugUtilsLabelEXT == nullptr)
            return;
        const std::string labelStr(label);
        VkDebugUtilsLabelEXT vkLabel{};
        vkLabel.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        vkLabel.pLabelName = labelStr.c_str();
        vkCmdBeginDebugUtilsLabelEXT(m_Cmd, &vkLabel);
    }

    void VulkanRendererAPI::PopDebugGroup()
    {
        if (m_Cmd == VK_NULL_HANDLE || vkCmdEndDebugUtilsLabelEXT == nullptr)
            return;
        vkCmdEndDebugUtilsLabelEXT(m_Cmd);
    }

    void VulkanRendererAPI::WaitForDeviceIdle()
    {
        if (auto* device = VulkanDevice::Get())
            vkDeviceWaitIdle(device->GetDevice());
    }

    bool VulkanRendererAPI::IsDeviceAvailable() const
    {
        return VulkanDevice::Get() != nullptr;
    }

    u32 VulkanRendererAPI::GetMaxUniformBlockSize() const
    {
        const_cast<VulkanRendererAPI*>(this)->CacheDeviceLimits();
        return m_MaxUniformBlockSize;
    }

    bool VulkanRendererAPI::SupportsInt64ShaderAtomics() const
    {
        const_cast<VulkanRendererAPI*>(this)->CacheDeviceLimits();
        return m_SupportsInt64Atomics;
    }

    u32 VulkanRendererAPI::GetMaxFramebufferSamples() const
    {
        const_cast<VulkanRendererAPI*>(this)->CacheDeviceLimits();
        return m_MaxSamples;
    }

    u32 VulkanRendererAPI::GetMaxColorTextureSamples() const
    {
        const_cast<VulkanRendererAPI*>(this)->CacheDeviceLimits();
        return m_MaxSamples;
    }

    u32 VulkanRendererAPI::GetMaxDepthTextureSamples() const
    {
        const_cast<VulkanRendererAPI*>(this)->CacheDeviceLimits();
        return m_MaxSamples;
    }

    // --- Recorded pipeline-key state (see VulkanRecordedPipelineState) -----

    void VulkanRendererAPI::SetClearColor(const glm::vec4& color)
    {
        m_State.ClearColor = color;
    }
    void VulkanRendererAPI::SetClearDepth(const f32 depth)
    {
        m_State.ClearDepth = depth;
    }
    void VulkanRendererAPI::SetDepthTest(const bool value)
    {
        m_State.DepthTest = value;
    }
    void VulkanRendererAPI::SetDepthMask(const bool value)
    {
        m_State.DepthWrite = value;
    }
    void VulkanRendererAPI::SetDepthFunc(const RHI::CompareOp func)
    {
        m_State.DepthFunc = func;
    }
    void VulkanRendererAPI::SetBlendState(const bool value)
    {
        m_State.Blend = value;
    }
    void VulkanRendererAPI::SetBlendFunc(const RHI::BlendFactor sfactor, const RHI::BlendFactor dfactor)
    {
        m_State.BlendSrcRGB = sfactor;
        m_State.BlendDstRGB = dfactor;
        m_State.BlendSrcAlpha = sfactor;
        m_State.BlendDstAlpha = dfactor;
    }
    void VulkanRendererAPI::SetBlendFuncSeparate(const RHI::BlendFactor srcRGB, const RHI::BlendFactor dstRGB,
                                                 const RHI::BlendFactor srcAlpha, const RHI::BlendFactor dstAlpha)
    {
        m_State.BlendSrcRGB = srcRGB;
        m_State.BlendDstRGB = dstRGB;
        m_State.BlendSrcAlpha = srcAlpha;
        m_State.BlendDstAlpha = dstAlpha;
    }
    void VulkanRendererAPI::SetBlendEquation(const RHI::BlendOp mode)
    {
        m_State.BlendEquation = mode;
    }
    void VulkanRendererAPI::EnableStencilTest()
    {
        m_State.StencilTest = true;
    }
    void VulkanRendererAPI::DisableStencilTest()
    {
        m_State.StencilTest = false;
    }
    bool VulkanRendererAPI::IsStencilTestEnabled() const
    {
        return m_State.StencilTest;
    }
    void VulkanRendererAPI::SetStencilFunc(const RHI::CompareOp func, const i32 ref, const u32 mask)
    {
        m_State.StencilFunc = func;
        m_State.StencilRef = ref;
        m_State.StencilReadMask = mask;
    }
    void VulkanRendererAPI::SetStencilOp(const RHI::StencilOp sfail, const RHI::StencilOp dpfail, const RHI::StencilOp dppass)
    {
        m_State.StencilFail = sfail;
        m_State.StencilDepthFail = dpfail;
        m_State.StencilPass = dppass;
    }
    void VulkanRendererAPI::SetStencilMask(const u32 mask)
    {
        m_State.StencilWriteMask = mask;
    }
    void VulkanRendererAPI::EnableCulling()
    {
        m_State.Culling = true;
    }
    void VulkanRendererAPI::DisableCulling()
    {
        m_State.Culling = false;
    }
    void VulkanRendererAPI::FrontCull()
    {
        m_State.CullFace = RHI::CullMode::Front;
    }
    void VulkanRendererAPI::BackCull()
    {
        m_State.CullFace = RHI::CullMode::Back;
    }
    void VulkanRendererAPI::SetCullFace(const RHI::CullMode face)
    {
        m_State.CullFace = face;
    }
    void VulkanRendererAPI::SetFrontFace(const RHI::FrontFace face)
    {
        m_State.FrontFaceWinding = face;
    }
    void VulkanRendererAPI::SetPolygonMode(const RHI::PolygonMode mode)
    {
        m_State.PolygonMode = mode;
    }
    void VulkanRendererAPI::SetPolygonOffset(const f32 factor, const f32 units)
    {
        m_State.PolygonOffsetEnabled = true;
        m_State.PolygonOffsetFactor = factor;
        m_State.PolygonOffsetUnits = units;
    }
    void VulkanRendererAPI::EnableScissorTest()
    {
        m_State.ScissorTest = true;
    }
    void VulkanRendererAPI::DisableScissorTest()
    {
        m_State.ScissorTest = false;
    }
    void VulkanRendererAPI::EnableMultisampling()
    {
        m_State.Multisampling = true;
    }
    void VulkanRendererAPI::DisableMultisampling()
    {
        m_State.Multisampling = false;
    }
    void VulkanRendererAPI::SetLineWidth(const f32 width)
    {
        m_State.LineWidth = width;
    }
    void VulkanRendererAPI::SetColorMask(const bool red, const bool green, const bool blue, const bool alpha)
    {
        m_State.ColorMask[0] = red;
        m_State.ColorMask[1] = green;
        m_State.ColorMask[2] = blue;
        m_State.ColorMask[3] = alpha;
    }
    void VulkanRendererAPI::SetColorMaskForAttachment(const u32 attachment, const bool red, const bool green,
                                                      const bool blue, const bool alpha)
    {
        if (attachment >= VulkanRecordedPipelineState::kMaxAttachments)
            return;
        m_State.AttachmentColorMask[attachment] = static_cast<u8>((red ? 1u : 0u) | (green ? 2u : 0u) |
                                                                  (blue ? 4u : 0u) | (alpha ? 8u : 0u));
    }
    void VulkanRendererAPI::SetBlendStateForAttachment(const u32 attachment, const bool enabled)
    {
        if (attachment >= VulkanRecordedPipelineState::kMaxAttachments)
            return;
        m_State.AttachmentBlend[attachment] = enabled;
    }
    void VulkanRendererAPI::SetBlendFuncForAttachment(const u32 attachment, const RHI::BlendFactor src, const RHI::BlendFactor dst)
    {
        if (attachment >= VulkanRecordedPipelineState::kMaxAttachments)
            return;
        m_State.AttachmentBlendSrc[attachment] = src;
        m_State.AttachmentBlendDst[attachment] = dst;
    }
    void VulkanRendererAPI::SetPatchVertexCount(const u32 patchVertices)
    {
        m_State.PatchVertexCount = patchVertices;
    }

    // --- Phase 6 stubs (generated; every entry warns once and counts) ------

    void VulkanRendererAPI::Clear()
    {
        Phase6Stub("Clear");
    }

    void VulkanRendererAPI::ClearDepthOnly()
    {
        Phase6Stub("ClearDepthOnly");
    }

    void VulkanRendererAPI::ClearColorAndDepth()
    {
        Phase6Stub("ClearColorAndDepth");
    }

    void VulkanRendererAPI::DrawArrays(const Ref<VertexArray>& /*vertexArray*/, u32 /*vertexCount*/)
    {
        Phase6Stub("DrawArrays");
    }

    void VulkanRendererAPI::DrawIndexed(const Ref<VertexArray>& /*vertexArray*/, u32 /*indexCount*/)
    {
        Phase6Stub("DrawIndexed");
    }

    void VulkanRendererAPI::DrawIndexedInstanced(const Ref<VertexArray>& /*vertexArray*/, u32 /*indexCount*/, u32 /*instanceCount*/)
    {
        Phase6Stub("DrawIndexedInstanced");
    }

    void VulkanRendererAPI::DrawLines(const Ref<VertexArray>& /*vertexArray*/, u32 /*vertexCount*/)
    {
        Phase6Stub("DrawLines");
    }

    void VulkanRendererAPI::DrawIndexedPatches(const Ref<VertexArray>& /*vertexArray*/, u32 /*indexCount*/, u32 /*patchVertices*/)
    {
        Phase6Stub("DrawIndexedPatches");
    }

    void VulkanRendererAPI::DrawIndexedRaw(RHI::ResourceHandle /*vertexArray*/, u32 /*indexCount*/)
    {
        Phase6Stub("DrawIndexedRaw");
    }

    void VulkanRendererAPI::DrawIndexedRaw(RHI::ResourceHandle /*vertexArray*/, u32 /*indexCount*/, u32 /*baseIndex*/)
    {
        Phase6Stub("DrawIndexedRaw");
    }

    void VulkanRendererAPI::DrawIndexedInstancedRaw(RHI::ResourceHandle /*vertexArray*/, u32 /*indexCount*/, u32 /*baseIndex*/, u32 /*instanceCount*/)
    {
        Phase6Stub("DrawIndexedInstancedRaw");
    }

    void VulkanRendererAPI::DrawIndexedPatchesRaw(RHI::ResourceHandle /*vertexArray*/, u32 /*indexCount*/, u32 /*patchVertices*/)
    {
        Phase6Stub("DrawIndexedPatchesRaw");
    }

    void VulkanRendererAPI::ClearStencil()
    {
        Phase6Stub("ClearStencil");
    }

    void VulkanRendererAPI::DrawElementsIndirect(const Ref<VertexArray>& /*vertexArray*/, RHI::ResourceHandle /*indirectBuffer*/)
    {
        Phase6Stub("DrawElementsIndirect");
    }

    void VulkanRendererAPI::DrawArraysIndirect(const Ref<VertexArray>& /*vertexArray*/, RHI::ResourceHandle /*indirectBuffer*/)
    {
        Phase6Stub("DrawArraysIndirect");
    }

    void VulkanRendererAPI::DrawBoundElementsIndirect(RHI::ResourceHandle /*indirectBuffer*/)
    {
        Phase6Stub("DrawBoundElementsIndirect");
    }

    void VulkanRendererAPI::MultiDrawElementsIndirectCountRaw(RHI::ResourceHandle /*vertexArray*/, RHI::ResourceHandle /*indirectBuffer*/, u32 /*indirectOffsetBytes*/, RHI::ResourceHandle /*parameterBuffer*/, u32 /*parameterOffsetBytes*/, u32 /*maxDrawCount*/, u32 /*strideBytes*/)
    {
        Phase6Stub("MultiDrawElementsIndirectCountRaw");
    }

    void VulkanRendererAPI::DispatchCompute(u32 /*groupsX*/, u32 /*groupsY*/, u32 /*groupsZ*/)
    {
        Phase6Stub("DispatchCompute");
    }

    void VulkanRendererAPI::BindDefaultFramebuffer()
    {
        Phase6Stub("BindDefaultFramebuffer");
    }

    void VulkanRendererAPI::BlitFramebufferToDefault(RHI::ResourceHandle /*srcFramebuffer*/, u32 /*width*/, u32 /*height*/)
    {
        Phase6Stub("BlitFramebufferToDefault");
    }

    void VulkanRendererAPI::BindTexture(u32 /*slot*/, RHI::ResourceHandle /*texture*/)
    {
        Phase6Stub("BindTexture");
    }

    void VulkanRendererAPI::BindImageTexture(u32 /*unit*/, RHI::ResourceHandle /*texture*/, u32 /*mipLevel*/, bool /*layered*/, u32 /*layer*/, RHI::Access /*access*/, RHI::Format /*format*/)
    {
        Phase6Stub("BindImageTexture");
    }

    void VulkanRendererAPI::CopyImageSubData(RHI::ResourceHandle /*src*/, TextureTargetType /*srcTarget*/, RHI::ResourceHandle /*dst*/, TextureTargetType /*dstTarget*/, u32 /*width*/, u32 /*height*/)
    {
        Phase6Stub("CopyImageSubData");
    }

    void VulkanRendererAPI::CopyImageSubDataFull(RHI::ResourceHandle /*src*/, TextureTargetType /*srcTarget*/, i32 /*srcLevel*/, i32 /*srcZ*/, RHI::ResourceHandle /*dst*/, TextureTargetType /*dstTarget*/, i32 /*dstLevel*/, i32 /*dstZ*/, u32 /*width*/, u32 /*height*/)
    {
        Phase6Stub("CopyImageSubDataFull");
    }

    void VulkanRendererAPI::CopyFramebufferToTexture(RHI::ResourceHandle /*texture*/, u32 /*width*/, u32 /*height*/)
    {
        Phase6Stub("CopyFramebufferToTexture");
    }

    void VulkanRendererAPI::SetDrawBuffers(std::span<const u32> /*attachments*/)
    {
        Phase6Stub("SetDrawBuffers");
    }

    void VulkanRendererAPI::RestoreAllDrawBuffers(u32 /*colorAttachmentCount*/)
    {
        Phase6Stub("RestoreAllDrawBuffers");
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateDepthArrayCompareOffViewHandle(RHI::ResourceHandle /*srcTexture*/, u32 /*numLayers*/)
    {
        Phase6Stub("CreateDepthArrayCompareOffViewHandle");
        return {};
    }

    void VulkanRendererAPI::SetTextureFilter(RHI::ResourceHandle /*texture*/, RHI::Filter /*minFilter*/, RHI::Filter /*magFilter*/)
    {
        Phase6Stub("SetTextureFilter");
    }

    void VulkanRendererAPI::SetTextureWrap(RHI::ResourceHandle /*texture*/, RHI::AddressMode /*wrap*/)
    {
        Phase6Stub("SetTextureWrap");
    }

    void VulkanRendererAPI::UploadTextureSubImage2D(RHI::ResourceHandle /*texture*/, u32 /*width*/, u32 /*height*/, RHI::Format /*sourceFormat*/, const void* /*data*/)
    {
        Phase6Stub("UploadTextureSubImage2D");
    }

    void VulkanRendererAPI::BeginConditionalRender(RHI::ResourceHandle /*query*/)
    {
        Phase6Stub("BeginConditionalRender");
    }

    void VulkanRendererAPI::EndConditionalRender()
    {
        Phase6Stub("EndConditionalRender");
    }

    void VulkanRendererAPI::BindUniformBuffer(u32 /*bindingPoint*/, RHI::ResourceHandle /*buffer*/)
    {
        Phase6Stub("BindUniformBuffer");
    }

    void VulkanRendererAPI::BindStorageBuffer(u32 /*bindingPoint*/, RHI::ResourceHandle /*buffer*/)
    {
        Phase6Stub("BindStorageBuffer");
    }

    void VulkanRendererAPI::BindShaderProgram(RHI::ResourceHandle /*program*/)
    {
        Phase6Stub("BindShaderProgram");
    }

    void VulkanRendererAPI::BindVertexArrayRaw(RHI::ResourceHandle /*vertexArray*/)
    {
        Phase6Stub("BindVertexArrayRaw");
    }

    void VulkanRendererAPI::BindFramebuffer(RHI::ResourceHandle /*framebuffer*/)
    {
        Phase6Stub("BindFramebuffer");
    }

    void VulkanRendererAPI::DrawBoundIndexed(RHI::PrimitiveTopology /*topology*/, u32 /*indexCount*/, RHI::IndexType /*indexType*/, u32 /*baseIndex*/)
    {
        Phase6Stub("DrawBoundIndexed");
    }

    void VulkanRendererAPI::DrawBoundIndexedInstanced(RHI::PrimitiveTopology /*topology*/, u32 /*indexCount*/, RHI::IndexType /*indexType*/, u32 /*baseIndex*/, u32 /*instanceCount*/)
    {
        Phase6Stub("DrawBoundIndexedInstanced");
    }

    void VulkanRendererAPI::DrawBoundArrays(RHI::PrimitiveTopology /*topology*/, u32 /*firstVertex*/, u32 /*vertexCount*/)
    {
        Phase6Stub("DrawBoundArrays");
    }

    void VulkanRendererAPI::AttachFramebufferColorTexture(RHI::ResourceHandle /*framebuffer*/, u32 /*attachmentIndex*/, RHI::ResourceHandle /*texture*/, u32 /*mipLevel*/)
    {
        Phase6Stub("AttachFramebufferColorTexture");
    }

    void VulkanRendererAPI::AttachFramebufferDepthTexture(RHI::ResourceHandle /*framebuffer*/, RHI::ResourceHandle /*texture*/, u32 /*mipLevel*/)
    {
        Phase6Stub("AttachFramebufferDepthTexture");
    }

    bool VulkanRendererAPI::IsFramebufferComplete(RHI::ResourceHandle /*framebuffer*/)
    {
        Phase6Stub("IsFramebufferComplete");
        return false;
    }

    void VulkanRendererAPI::SetFramebufferDrawAttachments(RHI::ResourceHandle /*framebuffer*/, std::span<const u32> /*attachmentIndices*/)
    {
        Phase6Stub("SetFramebufferDrawAttachments");
    }

    void VulkanRendererAPI::RestoreAllFramebufferDrawAttachments(RHI::ResourceHandle /*framebuffer*/, u32 /*colorAttachmentCount*/)
    {
        Phase6Stub("RestoreAllFramebufferDrawAttachments");
    }

    void VulkanRendererAPI::SetFramebufferReadAttachment(RHI::ResourceHandle /*framebuffer*/, u32 /*attachmentIndex*/)
    {
        Phase6Stub("SetFramebufferReadAttachment");
    }

    void VulkanRendererAPI::ClearFramebufferColorAttachment(RHI::ResourceHandle /*framebuffer*/, u32 /*attachmentIndex*/, const glm::vec4& /*color*/)
    {
        Phase6Stub("ClearFramebufferColorAttachment");
    }

    void VulkanRendererAPI::ClearFramebufferDepth(RHI::ResourceHandle /*framebuffer*/, f32 /*depth*/)
    {
        Phase6Stub("ClearFramebufferDepth");
    }

    void VulkanRendererAPI::BlitFramebuffer(RHI::ResourceHandle /*srcFramebuffer*/, RHI::ResourceHandle /*dstFramebuffer*/, i32 /*srcX0*/, i32 /*srcY0*/, i32 /*srcX1*/, i32 /*srcY1*/, i32 /*dstX0*/, i32 /*dstY0*/, i32 /*dstX1*/, i32 /*dstY1*/, RHI::BlitAspect /*aspect*/, RHI::Filter /*filter*/)
    {
        Phase6Stub("BlitFramebuffer");
    }

    void VulkanRendererAPI::AllocateBufferStorage(RHI::ResourceHandle /*buffer*/, u64 /*sizeBytes*/, RHI::MemoryResidency /*residency*/)
    {
        Phase6Stub("AllocateBufferStorage");
    }

    void* VulkanRendererAPI::AllocatePersistentUploadStorage(RHI::ResourceHandle /*buffer*/, u64 /*sizeBytes*/)
    {
        Phase6Stub("AllocatePersistentUploadStorage");
        return nullptr;
    }

    void VulkanRendererAPI::UnmapBuffer(RHI::ResourceHandle /*buffer*/)
    {
        Phase6Stub("UnmapBuffer");
    }

    void VulkanRendererAPI::UploadBufferSubData(RHI::ResourceHandle /*buffer*/, u64 /*offsetBytes*/, u64 /*sizeBytes*/, const void* /*data*/)
    {
        Phase6Stub("UploadBufferSubData");
    }

    void VulkanRendererAPI::ReadBufferSubData(RHI::ResourceHandle /*buffer*/, u64 /*offsetBytes*/, u64 /*sizeBytes*/, void* /*dest*/)
    {
        Phase6Stub("ReadBufferSubData");
    }

    void VulkanRendererAPI::CopyBufferSubData(RHI::ResourceHandle /*srcBuffer*/, RHI::ResourceHandle /*dstBuffer*/, u64 /*srcOffsetBytes*/, u64 /*dstOffsetBytes*/, u64 /*sizeBytes*/)
    {
        Phase6Stub("CopyBufferSubData");
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateTexture2DHandle(u32 /*width*/, u32 /*height*/, RHI::Format /*internalFormat*/)
    {
        Phase6Stub("CreateTexture2DHandle");
        return {};
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateTextureCubemapHandle(u32 /*width*/, u32 /*height*/, RHI::Format /*internalFormat*/)
    {
        Phase6Stub("CreateTextureCubemapHandle");
        return {};
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateFramebufferHandle()
    {
        Phase6Stub("CreateFramebufferHandle");
        return {};
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateBufferHandle()
    {
        Phase6Stub("CreateBufferHandle");
        return {};
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateVertexArrayHandle()
    {
        Phase6Stub("CreateVertexArrayHandle");
        return {};
    }

    void VulkanRendererAPI::DeleteTexture(RHI::ResourceHandle /*texture*/)
    {
        Phase6Stub("DeleteTexture");
    }

    void VulkanRendererAPI::DeleteFramebuffer(RHI::ResourceHandle /*framebuffer*/)
    {
        Phase6Stub("DeleteFramebuffer");
    }

    void VulkanRendererAPI::DeleteBuffer(RHI::ResourceHandle /*buffer*/)
    {
        Phase6Stub("DeleteBuffer");
    }

    void VulkanRendererAPI::DeleteVertexArray(RHI::ResourceHandle /*vertexArray*/)
    {
        Phase6Stub("DeleteVertexArray");
    }

    void VulkanRendererAPI::SetVertexArrayIndexBuffer(RHI::ResourceHandle /*vertexArray*/, RHI::ResourceHandle /*indexBuffer*/)
    {
        Phase6Stub("SetVertexArrayIndexBuffer");
    }

    void VulkanRendererAPI::UploadTextureSubImage2D(RHI::ResourceHandle /*texture*/, i32 /*xOffset*/, i32 /*yOffset*/, u32 /*width*/, u32 /*height*/, RHI::Format /*sourceFormat*/, const void* /*data*/)
    {
        Phase6Stub("UploadTextureSubImage2D");
    }

    void VulkanRendererAPI::UploadTextureSubImage3D(RHI::ResourceHandle /*texture*/, i32 /*xOffset*/, i32 /*yOffset*/, i32 /*zOffset*/, u32 /*width*/, u32 /*height*/, u32 /*depth*/, RHI::Format /*sourceFormat*/, const void* /*data*/)
    {
        Phase6Stub("UploadTextureSubImage3D");
    }

    bool VulkanRendererAPI::ReadTextureImage(RHI::ResourceHandle /*texture*/, u32 /*mipLevel*/, RHI::Format /*destFormat*/, sizet /*destSizeBytes*/, void* /*dest*/)
    {
        Phase6Stub("ReadTextureImage");
        return false;
    }

    bool VulkanRendererAPI::ReadTextureSubImage(RHI::ResourceHandle /*texture*/, u32 /*mipLevel*/, i32 /*x*/, i32 /*y*/, i32 /*z*/, u32 /*width*/, u32 /*height*/, u32 /*depth*/, RHI::Format /*destFormat*/, sizet /*destSizeBytes*/, void* /*dest*/)
    {
        Phase6Stub("ReadTextureSubImage");
        return false;
    }

    void VulkanRendererAPI::GetTextureDimensions(RHI::ResourceHandle /*texture*/, u32 /*mipLevel*/, u32& /*outWidth*/, u32& /*outHeight*/)
    {
        Phase6Stub("GetTextureDimensions");
    }

    void VulkanRendererAPI::TextureBarrier()
    {
        Phase6Stub("TextureBarrier");
    }

    void VulkanRendererAPI::CreateQueries(RHI::QueryType /*type*/, std::span<RHI::ResourceHandle> /*outQueries*/)
    {
        Phase6Stub("CreateQueries");
    }

    void VulkanRendererAPI::DeleteQueries(std::span<const RHI::ResourceHandle> /*queries*/)
    {
        Phase6Stub("DeleteQueries");
    }

    void VulkanRendererAPI::BeginQuery(RHI::QueryType /*type*/, RHI::ResourceHandle /*query*/)
    {
        Phase6Stub("BeginQuery");
    }

    void VulkanRendererAPI::EndQuery(RHI::QueryType /*type*/)
    {
        Phase6Stub("EndQuery");
    }

    bool VulkanRendererAPI::IsQueryResultAvailable(RHI::ResourceHandle /*query*/)
    {
        Phase6Stub("IsQueryResultAvailable");
        return false;
    }

    u32 VulkanRendererAPI::GetQueryResultU32(RHI::ResourceHandle /*query*/)
    {
        Phase6Stub("GetQueryResultU32");
        return 0;
    }

    u64 VulkanRendererAPI::GetQueryResultU64(RHI::ResourceHandle /*query*/)
    {
        Phase6Stub("GetQueryResultU64");
        return 0;
    }

    u64 VulkanRendererAPI::CreateFence()
    {
        Phase6Stub("CreateFence");
        return 0;
    }

    RHI::FenceStatus VulkanRendererAPI::ClientWaitFence(u64 /*fence*/, u64 /*timeoutNanoseconds*/)
    {
        Phase6Stub("ClientWaitFence");
        return RHI::FenceStatus::ConditionSatisfied;
    }

    bool VulkanRendererAPI::IsFenceSignaled(u64 /*fence*/)
    {
        Phase6Stub("IsFenceSignaled");
        return false;
    }

    void VulkanRendererAPI::DestroyFence(u64 /*fence*/)
    {
        Phase6Stub("DestroyFence");
    }

    void VulkanRendererAPI::SetProgramUniformFloat(RHI::ResourceHandle /*program*/, std::string_view /*name*/, f32 /*value*/)
    {
        Phase6Stub("SetProgramUniformFloat");
    }

} // namespace OloEngine

#endif // OLO_WITH_VULKAN
