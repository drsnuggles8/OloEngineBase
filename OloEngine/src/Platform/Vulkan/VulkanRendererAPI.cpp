#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "Platform/Vulkan/VulkanBarrierLowering.h"
#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDescriptorSlotCache.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <bit>
#include <cstring>
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

        // Three DISTINCT caps, mirroring the GL getters they implement:
        // framebuffer = the color∩depth attachment intersection (a framebuffer
        // carries both), while the per-kind texture caps come from the
        // sampled-image limits.
        const auto highestBit = [](const VkSampleCountFlags counts) -> u32
        {
            for (const VkSampleCountFlagBits bit : { VK_SAMPLE_COUNT_64_BIT, VK_SAMPLE_COUNT_32_BIT,
                                                     VK_SAMPLE_COUNT_16_BIT, VK_SAMPLE_COUNT_8_BIT,
                                                     VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT })
            {
                if (counts & bit)
                    return static_cast<u32>(bit);
            }
            return 1u;
        };
        m_MaxFramebufferSamples = highestBit(props.limits.framebufferColorSampleCounts &
                                             props.limits.framebufferDepthSampleCounts);
        m_MaxColorTextureSamples = highestBit(props.limits.sampledImageColorSampleCounts);
        m_MaxDepthTextureSamples = highestBit(props.limits.sampledImageDepthSampleCounts);

        // ENABLED on the logical device, not merely supported by the physical
        // one — a shader can only use what vkCreateDevice turned on.
        m_SupportsInt64Atomics = device->IsShaderBufferInt64AtomicsEnabled();

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
        // Per-recording caches: binds are command-buffer state.
        m_BoundIndexBuffer = VK_NULL_HANDLE;
        m_HeapBoundThisRecording = false;
        m_Scope = RenderingScope{};
        m_ScissorRectSet = false;
    }

    void VulkanRendererAPI::EndRecording()
    {
        EndRenderingScope();
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
        // Deliberately does NOT touch the scissor: glViewport never did, and
        // deriving one here would clobber an explicit SetScissorBox. A draw
        // with dynamic scissor state and no scissor set is Phase 6's pipeline
        // setup to default.
        VkViewport vp{};
        vp.x = static_cast<f32>(x);
        vp.y = static_cast<f32>(y);
        vp.width = static_cast<f32>(width);
        vp.height = static_cast<f32>(height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(m_Cmd, 0, 1, &vp);
    }

    Viewport VulkanRendererAPI::GetViewport() const
    {
        return m_Viewport;
    }

    void VulkanRendererAPI::SetScissorBox(const i32 x, const i32 y, const u32 width, const u32 height)
    {
        // Recorded; the draw front-end emits the WITH_COUNT form (the
        // pipelines declare VIEWPORT/SCISSOR_WITH_COUNT dynamic state, which
        // the plain setters cannot satisfy).
        m_ScissorRect = { { x, y }, { width, height } };
        m_ScissorRectSet = true;
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

        // Barriers are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

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

        // vkCmdPipelineBarrier2 is illegal inside a dynamic-rendering scope;
        // the next draw resumes with LOAD_OP_LOAD.
        EndRenderingScope();

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
            // Normalized extents: a registry entry with 0 mips/layers (no
            // producer writes one today, but the struct cannot forbid it)
            // would underflow every `- 1u`/`- base` below.
            const u32 mipCount = std::max(info->MipLevels, 1u);
            const u32 layerCount = std::max(info->ArrayLayers, 1u);
            // InitialLayout seeds the tracker's first sight of an uploaded
            // image (SHADER_READ_ONLY after a load-time one-shot) so its
            // first graph transition cannot legally discard the contents.
            m_LayoutTracker.RegisterImage(image, mipCount, layerCount, info->RegistrationId, info->InitialLayout);

            // The neutral range, clamped into Vk terms once; the tracker then
            // splits it into runs of equal current layout so oldLayout is
            // EXACT per emitted barrier — one guessed layout for a mixed
            // range is precisely the class of bug sync validation exists for.
            // Finite counts clamp to [1, remaining] — an explicit zero count
            // is a malformed declaration, and BuildImageBarrier already
            // treats it as 1-wide; the tracker query must agree or the two
            // silently disagree about whether a barrier exists at all.
            VkImageSubresourceRange queryRange{};
            queryRange.aspectMask = VulkanBarrierLowering::AspectMaskFor(aspect);
            queryRange.baseMipLevel = std::min(barrier.Range.BaseMip, mipCount - 1u);
            queryRange.levelCount = (barrier.Range.MipCount == RHI::SubresourceRange::AllRemaining)
                                        ? VK_REMAINING_MIP_LEVELS
                                        : std::max(std::min(barrier.Range.MipCount, mipCount - queryRange.baseMipLevel), 1u);
            queryRange.baseArrayLayer = std::min(barrier.Range.BaseLayer, layerCount - 1u);
            queryRange.layerCount = (barrier.Range.LayerCount == RHI::SubresourceRange::AllRemaining)
                                        ? VK_REMAINING_ARRAY_LAYERS
                                        : std::max(std::min(barrier.Range.LayerCount, layerCount - queryRange.baseArrayLayer), 1u);

            m_LayoutTracker.ForEachLayoutRun(
                image, queryRange,
                [&](const VkImageSubresourceRange& run, const VkImageLayout trackedLayout)
                {
                    auto vkBarrier = VulkanBarrierLowering::BuildImageBarrier(barrier, image, aspect, trackedLayout,
                                                                              mipCount, layerCount);
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
        // `anyUnresolved` forces the fallback INDEPENDENTLY of the flags:
        // today a None-flags barrier is always an external no-producer
        // transition (nothing to order), but that is an invariant of the
        // planner, not of this function — a future None-flags barrier kind
        // with a real producer must degrade to over-sync, never to a race.
        const bool needsGlobalFallback =
            anyUnresolved ||
            (flags != MemoryBarrierFlags::None && imageBarriers.empty() && bufferBarriers.empty());

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

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        auto& registry = RHI::ResourceRegistry::Get();
        const u64 native = registry.ResolveNativeForBackend(texture);
        if (native == 0u)
            return;
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (!info)
            return;

        const auto aspect = AspectFromInfo(*info);
        const u32 mipCount = std::max(info->MipLevels, 1u); // 0-extent guard, see IssueBarrierBatch
        m_LayoutTracker.RegisterImage(image, mipCount, std::max(info->ArrayLayers, 1u), info->RegistrationId);

        VkImageSubresourceRange range{};
        range.aspectMask = VulkanBarrierLowering::AspectMaskFor(aspect);
        range.baseMipLevel = std::min(mipLevel, mipCount - 1u);
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

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();
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

        const u32 mipCount = std::max(info->MipLevels, 1u); // 0-extent guard, see IssueBarrierBatch
        m_LayoutTracker.RegisterImage(image, mipCount, std::max(info->ArrayLayers, 1u), info->RegistrationId);

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = std::min(mipLevel, mipCount - 1u);
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

        // Designated init ACTIVATES the union's uint32 member — value-init
        // (`{}`) would activate float32 (the first member) and make these
        // writes inactive-member accesses.
        const VkClearColorValue clear{ .uint32 = { value, value, value, value } };
        vkCmdClearColorImage(m_Cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }

    void VulkanRendererAPI::ClearBufferFloat(const RHI::ResourceHandle buffer, const f32 value)
    {
        if (m_Cmd == VK_NULL_HANDLE)
        {
            Phase6Stub("ClearBufferFloat(outside recording bracket)");
            return;
        }

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();
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

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();
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
        return m_MaxFramebufferSamples;
    }

    u32 VulkanRendererAPI::GetMaxColorTextureSamples() const
    {
        const_cast<VulkanRendererAPI*>(this)->CacheDeviceLimits();
        return m_MaxColorTextureSamples;
    }

    u32 VulkanRendererAPI::GetMaxDepthTextureSamples() const
    {
        const_cast<VulkanRendererAPI*>(this)->CacheDeviceLimits();
        return m_MaxDepthTextureSamples;
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

    // --- Phase 7: rendering scope + draw assembly --------------------------

    namespace
    {
        [[nodiscard]] VkPrimitiveTopology ToVkTopology(const RHI::PrimitiveTopology topology)
        {
            switch (topology)
            {
                case RHI::PrimitiveTopology::TriangleList:
                    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                case RHI::PrimitiveTopology::TriangleStrip:
                    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
                case RHI::PrimitiveTopology::LineList:
                    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                case RHI::PrimitiveTopology::LineStrip:
                    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
                case RHI::PrimitiveTopology::PointList:
                    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                case RHI::PrimitiveTopology::PatchList:
                    return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
            }
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }
    } // namespace

    void VulkanRendererAPI::EndRenderingScope()
    {
        if (!m_Scope.Active)
        {
            return;
        }
        vkCmdEndRendering(m_Cmd);
        m_Scope.Active = false;
        // Pending clears survive the scope END only if never consumed; the
        // next scope begin re-reads them. Target/DrawList stay published.
    }

    bool VulkanRendererAPI::EnsureRenderingScopeForDraw()
    {
        auto* target = VulkanBindingState::Get().GetCurrentFramebuffer();
        if (target == nullptr)
        {
            static bool s_Warned = false;
            if (!s_Warned)
            {
                s_Warned = true;
                OLO_CORE_WARN("[RHI/Vulkan] draw with no framebuffer published — dropped (default-framebuffer "
                              "rendering arrives with the swapchain import)");
            }
            return false;
        }

        if (m_Scope.Active && m_Scope.Target == target)
        {
            return true;
        }
        EndRenderingScope();

        const auto& spec = target->GetSpecification();
        const u32 width = std::max(spec.Width, 1u);
        const u32 height = std::max(spec.Height, 1u);

        // The SetDrawBuffers selection maps fragment output location i onto
        // attachment DrawList[i] (GL's glDrawBuffers semantics). Identity
        // over every color attachment when no selection was recorded.
        std::array<VkRenderingAttachmentInfo, 8> colorInfos{};
        m_ScopeTargets = VulkanRenderTargetDesc{};
        m_ScopeTargets.Samples = std::max(spec.Samples, 1u);

        const bool identity = m_Scope.DrawListCount == 0;
        const u32 outputCount =
            identity ? std::min<u32>(target->GetColorAttachmentCount(), static_cast<u32>(colorInfos.size()))
                     : std::min<u32>(m_Scope.DrawListCount, static_cast<u32>(colorInfos.size()));

        for (u32 i = 0; i < outputCount; ++i)
        {
            auto& info = colorInfos[i];
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;

            const u32 attachmentIndex = identity ? i : m_Scope.DrawList[i];
            Ref<VulkanTexture2D> image =
                attachmentIndex == RHI::NoAttachment ? nullptr : target->GetColorAttachmentImage(attachmentIndex);
            if (image == nullptr)
            {
                // VK_ATTACHMENT_UNUSED shape: a null imageView slot.
                info.imageView = VK_NULL_HANDLE;
                info.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                info.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
                m_ScopeTargets.ColorFormats[i] = VK_FORMAT_UNDEFINED;
                continue;
            }

            const VkImageView view = image->GetOrCreateAttachmentView();
            if (view == VK_NULL_HANDLE)
            {
                OLO_CORE_ERROR("[RHI/Vulkan] attachment view unavailable — draw dropped");
                return false;
            }
            info.imageView = view;
            info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.loadOp = m_Scope.PendingClearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            info.clearValue.color = { { m_State.ClearColor.r, m_State.ClearColor.g, m_State.ClearColor.b,
                                        m_State.ClearColor.a } };

            const auto* imageInfo = VulkanImageInfoRegistry::Get().Lookup(image->GetVkImage());
            m_ScopeTargets.ColorFormats[i] = imageInfo != nullptr ? imageInfo->Format : VK_FORMAT_UNDEFINED;
        }
        m_ScopeTargets.ColorCount = outputCount;

        VkRenderingAttachmentInfo depthInfo{};
        Ref<VulkanTexture2D> depthImage = target->GetDepthAttachmentImage();
        if (depthImage != nullptr)
        {
            const VkImageView depthView = depthImage->GetOrCreateAttachmentView();
            if (depthView == VK_NULL_HANDLE)
            {
                OLO_CORE_ERROR("[RHI/Vulkan] depth attachment view unavailable — draw dropped");
                return false;
            }
            depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthInfo.imageView = depthView;
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = m_Scope.PendingClearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthInfo.clearValue.depthStencil = { 1.0f, 0u };

            const auto* depthImageInfo = VulkanImageInfoRegistry::Get().Lookup(depthImage->GetVkImage());
            m_ScopeTargets.DepthFormat = depthImageInfo != nullptr ? depthImageInfo->Format : VK_FORMAT_UNDEFINED;
        }

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = { { 0, 0 }, { width, height } };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = outputCount;
        rendering.pColorAttachments = outputCount > 0 ? colorInfos.data() : nullptr;
        rendering.pDepthAttachment = depthImage != nullptr ? &depthInfo : nullptr;
        // Stencil shares the depth attachment when the format carries the
        // aspect; the recorded stencil state is dynamic.
        rendering.pStencilAttachment =
            (depthImage != nullptr && m_ScopeTargets.DepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT) ? &depthInfo
                                                                                                  : nullptr;
        vkCmdBeginRendering(m_Cmd, &rendering);

        m_Scope.Active = true;
        m_Scope.Target = target;
        // The pending clears were consumed by the loadOps above.
        m_Scope.PendingClearColor = false;
        m_Scope.PendingClearDepth = false;
        return true;
    }

    bool VulkanRendererAPI::PrepareDraw(const VulkanVertexArray* vao, const VkPrimitiveTopology topology)
    {
        if (m_Cmd == VK_NULL_HANDLE)
        {
            Phase6Stub("Draw(outside recording bracket)");
            return false;
        }

        auto* shader = VulkanShader::GetCurrentlyBound();
        if (shader == nullptr || shader->GetCompilationStatus() != ShaderCompilationStatus::Ready)
        {
            static bool s_Warned = false;
            if (!s_Warned)
            {
                s_Warned = true;
                OLO_CORE_WARN("[RHI/Vulkan] draw with no ready shader bound — dropped");
            }
            return false;
        }

        if (!EnsureRenderingScopeForDraw())
        {
            return false;
        }

        const auto& layout = shader->GetRootDataLayout();
        const VkPipeline pipeline =
            VulkanPipelineBuilder::Get().GetOrCreateGraphics(*shader, layout, m_State, m_ScopeTargets);
        if (pipeline == VK_NULL_HANDLE)
        {
            return false;
        }

        vkCmdBindPipeline(m_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VulkanPipelineBuilder::FlushDynamicState(m_Cmd, m_State, m_ScopeTargets);
        // FlushDynamicState's topology is the pilot's TRIANGLE_LIST; the
        // draw knows better — later set wins.
        vkCmdSetPrimitiveTopology(m_Cmd, topology);

        // The pipelines declare VIEWPORT/SCISSOR_WITH_COUNT dynamic state —
        // the plain setters cannot satisfy it, so the draw front-end emits
        // both here: the recorded viewport (target extent when none was set)
        // and the recorded scissor box (full render area when scissor state
        // was merely disabled, which is the GL shape).
        const auto& targetSpec = m_Scope.Target->GetSpecification();
        VkViewport viewport{};
        viewport.x = static_cast<f32>(m_Viewport.x);
        viewport.y = static_cast<f32>(m_Viewport.y);
        viewport.width = static_cast<f32>(m_Viewport.width != 0 ? m_Viewport.width : std::max(targetSpec.Width, 1u));
        viewport.height =
            static_cast<f32>(m_Viewport.height != 0 ? m_Viewport.height : std::max(targetSpec.Height, 1u));
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewportWithCount(m_Cmd, 1, &viewport);

        VkRect2D scissor = m_ScissorRectSet && m_State.ScissorTest
                               ? m_ScissorRect
                               : VkRect2D{ { 0, 0 }, { std::max(targetSpec.Width, 1u), std::max(targetSpec.Height, 1u) } };
        vkCmdSetScissorWithCount(m_Cmd, 1, &scissor);

        if (!m_HeapBoundThisRecording)
        {
            VulkanResourceHeap::Get().CmdBind(m_Cmd);
            m_HeapBoundThisRecording = true;
        }

        // Root-struct assembly: one u64 device address per buffer block, one
        // u32 heap slot per sampled image, from the process-global binding
        // state (ADR 0011 §4 — this is the draw-time writer half of the
        // mapping contract; the pipeline emitted the reader half from the
        // SAME layout).
        auto& bindingState = VulkanBindingState::Get();
        m_RootScratch.assign(layout.SizeBytes, 0u);
        for (const auto& field : layout.Fields)
        {
            const auto& binding = field.Binding;
            const bool isBuffer = binding.BindingKind == VulkanShaderBinding::Kind::UniformBuffer ||
                                  binding.BindingKind == VulkanShaderBinding::Kind::StorageBuffer;
            if (isBuffer)
            {
                u64 address = 0;
                if (binding.Binding == 57u)
                {
                    // §5 vertex pulling: the reserved binding reads the draw's
                    // vertex stream.
                    const auto* pullBuffer = vao != nullptr ? vao->GetPullVertexBuffer() : nullptr;
                    address = pullBuffer != nullptr ? pullBuffer->GetDeviceAddress() : 0;
                }
                else if (auto* ubo = bindingState.GetUniformBuffer(binding.Binding);
                         ubo != nullptr && binding.BindingKind == VulkanShaderBinding::Kind::UniformBuffer)
                {
                    address = ubo->GetRootDataAddress();
                }
                else if (auto* ssbo = bindingState.GetStorageBuffer(binding.Binding); ssbo != nullptr)
                {
                    address = ssbo->GetDeviceAddress();
                }
                if (address == 0)
                {
                    // Warn once per shader+binding: an unfed block reads the
                    // null address, which the mapping treats as a zero-filled
                    // block — deterministic wrong data, never a crash the
                    // engine can't see.
                    static std::unordered_set<std::string> s_WarnedBindings;
                    if (s_WarnedBindings.insert(shader->GetName() + ":" + std::to_string(binding.Binding)).second)
                    {
                        OLO_CORE_WARN("[RHI/Vulkan] '{}' buffer binding {} has no published occupant — zero address",
                                      shader->GetName(), binding.Binding);
                    }
                }
                std::memcpy(m_RootScratch.data() + field.Offset, &address, sizeof(address));
            }
            else
            {
                u32 slot = bindingState.GetTextureHeapSlot(binding.Binding);
                if (slot == VulkanBindingState::kNoHeapSlot)
                {
                    static std::unordered_set<std::string> s_WarnedSlots;
                    if (s_WarnedSlots.insert(shader->GetName() + ":" + std::to_string(binding.Binding)).second)
                    {
                        OLO_CORE_WARN("[RHI/Vulkan] '{}' texture binding {} has no staged heap slot — slot 0",
                                      shader->GetName(), binding.Binding);
                    }
                    slot = 0;
                }
                std::memcpy(m_RootScratch.data() + field.Offset, &slot, sizeof(slot));
            }
        }

        const auto rootAllocation = VulkanFrameArena::Get().Push(m_RootScratch.data(), m_RootScratch.size(), 16);
        if (!rootAllocation.IsValid())
        {
            return false; // arena overflow — dropped draw, counted by the arena
        }

        VkPushDataInfoEXT pushInfo{};
        pushInfo.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT;
        pushInfo.offset = 0;
        VkDeviceAddress rootAddress = rootAllocation.Gpu;
        pushInfo.data = { .address = &rootAddress, .size = sizeof(rootAddress) };
        vkCmdPushDataEXT(m_Cmd, &pushInfo);
        return true;
    }

    bool VulkanRendererAPI::BindIndexBufferFor(const VulkanVertexArray* vao)
    {
        const auto* indexBuffer = vao != nullptr ? vao->GetVulkanIndexBuffer() : nullptr;
        if (indexBuffer == nullptr || indexBuffer->GetVkBuffer() == VK_NULL_HANDLE)
        {
            static bool s_Warned = false;
            if (!s_Warned)
            {
                s_Warned = true;
                OLO_CORE_WARN("[RHI/Vulkan] indexed draw without an index buffer — dropped");
            }
            return false;
        }
        if (indexBuffer->GetVkBuffer() != m_BoundIndexBuffer)
        {
            vkCmdBindIndexBuffer(m_Cmd, indexBuffer->GetVkBuffer(), 0, VK_INDEX_TYPE_UINT32);
            m_BoundIndexBuffer = indexBuffer->GetVkBuffer();
        }
        return true;
    }

    // --- Phase 6 stubs (generated; every entry warns once and counts) ------

    void VulkanRendererAPI::Clear()
    {
        if (m_Cmd == VK_NULL_HANDLE)
        {
            Phase6Stub("Clear(outside recording bracket)");
            return;
        }
        if (!m_Scope.Active)
        {
            // Folds into the next scope begin's loadOp (the GL clear-then-
            // draw shape).
            m_Scope.PendingClearColor = true;
            m_Scope.PendingClearDepth = true;
            return;
        }
        // Mid-scope clear: vkCmdClearAttachments against the live scope.
        std::array<VkClearAttachment, 9> clears{};
        u32 clearCount = 0;
        for (u32 i = 0; i < m_ScopeTargets.ColorCount; ++i)
        {
            if (m_ScopeTargets.ColorFormats[i] == VK_FORMAT_UNDEFINED)
            {
                continue;
            }
            auto& clear = clears[clearCount++];
            clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clear.colorAttachment = i;
            clear.clearValue.color = { { m_State.ClearColor.r, m_State.ClearColor.g, m_State.ClearColor.b,
                                         m_State.ClearColor.a } };
        }
        if (m_ScopeTargets.DepthFormat != VK_FORMAT_UNDEFINED)
        {
            auto& clear = clears[clearCount++];
            clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            clear.clearValue.depthStencil = { 1.0f, 0u };
        }
        if (clearCount == 0)
        {
            return;
        }
        const auto& spec = m_Scope.Target->GetSpecification();
        VkClearRect rect{};
        rect.rect = { { 0, 0 }, { std::max(spec.Width, 1u), std::max(spec.Height, 1u) } };
        rect.layerCount = 1;
        vkCmdClearAttachments(m_Cmd, clearCount, clears.data(), 1, &rect);
    }

    void VulkanRendererAPI::ClearDepthOnly()
    {
        if (m_Cmd == VK_NULL_HANDLE)
        {
            Phase6Stub("ClearDepthOnly(outside recording bracket)");
            return;
        }
        if (!m_Scope.Active)
        {
            m_Scope.PendingClearDepth = true;
            return;
        }
        if (m_ScopeTargets.DepthFormat == VK_FORMAT_UNDEFINED)
        {
            return;
        }
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil = { 1.0f, 0u };
        const auto& spec = m_Scope.Target->GetSpecification();
        VkClearRect rect{};
        rect.rect = { { 0, 0 }, { std::max(spec.Width, 1u), std::max(spec.Height, 1u) } };
        rect.layerCount = 1;
        vkCmdClearAttachments(m_Cmd, 1, &clear, 1, &rect);
    }

    void VulkanRendererAPI::ClearColorAndDepth()
    {
        Clear();
    }

    void VulkanRendererAPI::DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount)
    {
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST))
        {
            vkCmdDraw(m_Cmd, vertexCount, 1, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount)
    {
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            vkCmdDrawIndexed(m_Cmd, indexCount, 1, 0, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 instanceCount)
    {
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            vkCmdDrawIndexed(m_Cmd, indexCount, instanceCount, 0, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, u32 vertexCount)
    {
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_LINE_LIST))
        {
            vkCmdDraw(m_Cmd, vertexCount, 1, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawIndexedPatches(const Ref<VertexArray>& /*vertexArray*/, u32 /*indexCount*/, u32 /*patchVertices*/)
    {
        Phase6Stub("DrawIndexedPatches");
    }

    void VulkanRendererAPI::DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount)
    {
        DrawIndexedRaw(vertexArray, indexCount, 0u);
    }

    void VulkanRendererAPI::DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex)
    {
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(vertexArray);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::VertexArray)
        {
            Phase6Stub("DrawIndexedRaw(unresolvable vertex array)");
            return;
        }
        const auto* vao = static_cast<const VulkanVertexArray*>(entry->Object);
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            vkCmdDrawIndexed(m_Cmd, indexCount, 1, baseIndex, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawIndexedInstancedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex, u32 instanceCount)
    {
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(vertexArray);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::VertexArray)
        {
            Phase6Stub("DrawIndexedInstancedRaw(unresolvable vertex array)");
            return;
        }
        const auto* vao = static_cast<const VulkanVertexArray*>(entry->Object);
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            vkCmdDrawIndexed(m_Cmd, indexCount, instanceCount, baseIndex, 0, 0);
        }
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
        // GL semantics: unbind the named framebuffer. The swapchain-backed
        // default target arrives with the frame-loop integration; until then
        // a draw against the default framebuffer is dropped with the
        // no-target warn in EnsureRenderingScopeForDraw.
        EndRenderingScope();
        VulkanBindingState::Get().SetCurrentFramebuffer(nullptr);
    }

    void VulkanRendererAPI::BlitFramebufferToDefault(RHI::ResourceHandle /*srcFramebuffer*/, u32 /*width*/, u32 /*height*/)
    {
        Phase6Stub("BlitFramebufferToDefault");
    }

    void VulkanRendererAPI::BindTexture(u32 slot, RHI::ResourceHandle texture)
    {
        auto& bindingState = VulkanBindingState::Get();
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        if (native == 0u)
        {
            bindingState.SetTextureHeapSlot(slot, VulkanBindingState::kNoHeapSlot);
            return;
        }
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr)
        {
            bindingState.SetTextureHeapSlot(slot, VulkanBindingState::kNoHeapSlot);
            return;
        }

        // Default whole-image sampled view. Depth-stencil formats sample the
        // DEPTH aspect (GLSL sampler2D/shadow reads depth; sampling stencil
        // needs an explicit stencil view, which no current pass requests).
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = info->Format;
        view.subresourceRange.aspectMask = info->HasDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = std::max(info->MipLevels, 1u);
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = std::max(info->ArrayLayers, 1u);

        const u32 heapSlot = VulkanDescriptorSlotCache::Get().AcquireSlot(
            image, view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        bindingState.SetTextureHeapSlot(
            slot, heapSlot == VulkanResourceHeap::InvalidSlot ? VulkanBindingState::kNoHeapSlot : heapSlot);
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

    void VulkanRendererAPI::SetDrawBuffers(std::span<const u32> attachments)
    {
        // A selection change mid-scope re-shapes the attachment list: end the
        // scope so the next draw begins with the new mapping.
        const u32 count = std::min<u32>(static_cast<u32>(attachments.size()),
                                        static_cast<u32>(m_Scope.DrawList.size()));
        bool changed = count != m_Scope.DrawListCount;
        for (u32 i = 0; !changed && i < count; ++i)
        {
            changed = m_Scope.DrawList[i] != attachments[i];
        }
        if (!changed)
        {
            return;
        }
        EndRenderingScope();
        m_Scope.DrawListCount = count;
        for (u32 i = 0; i < count; ++i)
        {
            m_Scope.DrawList[i] = attachments[i];
        }
    }

    void VulkanRendererAPI::RestoreAllDrawBuffers(u32 /*colorAttachmentCount*/)
    {
        if (m_Scope.DrawListCount == 0)
        {
            return;
        }
        EndRenderingScope();
        m_Scope.DrawListCount = 0; // identity over every color attachment
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

    void VulkanRendererAPI::BindUniformBuffer(u32 bindingPoint, RHI::ResourceHandle buffer)
    {
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(buffer);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::UniformBuffer)
        {
            VulkanBindingState::Get().SetUniformBuffer(bindingPoint, nullptr);
            return;
        }
        VulkanBindingState::Get().SetUniformBuffer(bindingPoint, static_cast<VulkanUniformBuffer*>(entry->Object));
    }

    void VulkanRendererAPI::BindStorageBuffer(u32 bindingPoint, RHI::ResourceHandle buffer)
    {
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(buffer);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::StorageBuffer)
        {
            VulkanBindingState::Get().SetStorageBuffer(bindingPoint, nullptr);
            return;
        }
        VulkanBindingState::Get().SetStorageBuffer(bindingPoint, static_cast<VulkanStorageBuffer*>(entry->Object));
    }

    void VulkanRendererAPI::BindShaderProgram(RHI::ResourceHandle program)
    {
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(program);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::Shader)
        {
            Phase6Stub("BindShaderProgram(unresolvable shader)");
            return;
        }
        static_cast<VulkanShader*>(entry->Object)->Bind();
    }

    void VulkanRendererAPI::BindVertexArrayRaw(RHI::ResourceHandle vertexArray)
    {
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(vertexArray);
        m_BoundVertexArray = (entry != nullptr && entry->Kind == VulkanRootObjectKind::VertexArray)
                                 ? static_cast<VulkanVertexArray*>(entry->Object)
                                 : nullptr;
    }

    void VulkanRendererAPI::BindFramebuffer(RHI::ResourceHandle framebuffer)
    {
        // Passes bind via Framebuffer::Bind(), which publishes to
        // VulkanBindingState directly; this by-handle entry point serves the
        // POD packet path and warns until a packet producer needs it.
        (void)framebuffer;
        Phase6Stub("BindFramebuffer(by handle - packet path)");
    }

    void VulkanRendererAPI::DrawBoundIndexed(RHI::PrimitiveTopology topology, u32 indexCount, RHI::IndexType /*indexType*/, u32 baseIndex)
    {
        // The engine's index format is fixed 32-bit (IndexBuffer.h contract);
        // BindIndexBufferFor binds UINT32 accordingly.
        if (PrepareDraw(m_BoundVertexArray, ToVkTopology(topology)) && BindIndexBufferFor(m_BoundVertexArray))
        {
            vkCmdDrawIndexed(m_Cmd, indexCount, 1, baseIndex, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawBoundIndexedInstanced(RHI::PrimitiveTopology topology, u32 indexCount, RHI::IndexType /*indexType*/, u32 baseIndex, u32 instanceCount)
    {
        if (PrepareDraw(m_BoundVertexArray, ToVkTopology(topology)) && BindIndexBufferFor(m_BoundVertexArray))
        {
            vkCmdDrawIndexed(m_Cmd, indexCount, instanceCount, baseIndex, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawBoundArrays(RHI::PrimitiveTopology topology, u32 firstVertex, u32 vertexCount)
    {
        if (PrepareDraw(m_BoundVertexArray, ToVkTopology(topology)))
        {
            vkCmdDraw(m_Cmd, vertexCount, 1, firstVertex, 0);
        }
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
        // Failed, not ConditionSatisfied: a stub must not claim GPU work
        // completed (consistent with IsFenceSignaled() returning false — a
        // caller gating a readback on this would consume garbage).
        return RHI::FenceStatus::Failed;
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
