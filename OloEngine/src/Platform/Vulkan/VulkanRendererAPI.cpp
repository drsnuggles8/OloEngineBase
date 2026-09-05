#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "Platform/Vulkan/VulkanBarrierLowering.h"
#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanDescriptorHeapBackend.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDescriptorSlotCache.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanGpuFence.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanSecondaryCommandPools.h"
#include "Platform/Vulkan/VulkanWarnOnce.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Task/ParallelFor.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanComputeShader.h"
#include "Platform/Vulkan/VulkanSamplerHeap.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanStorageBuffer.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <exception>
#include <chrono>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
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

        // Integer formats force NEAREST sampling (#691): GL makes an
        // integer texture with a LINEAR filter incomplete, and incomplete
        // samples as zero — §4f's mandatory row, mirrored at the sampler-heap
        // seam. The set is the integer formats the engine can actually mint.
        [[nodiscard]] bool IsIntegerVkFormat(const VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_R8_UINT:
                case VK_FORMAT_R8_SINT:
                case VK_FORMAT_R16_UINT:
                case VK_FORMAT_R16_SINT:
                case VK_FORMAT_R32_UINT:
                case VK_FORMAT_R32_SINT:
                case VK_FORMAT_R8G8_UINT:
                case VK_FORMAT_R16G16_UINT:
                case VK_FORMAT_R32G32_UINT:
                case VK_FORMAT_R8G8B8A8_UINT:
                case VK_FORMAT_R16G16B16A16_UINT:
                case VK_FORMAT_R32G32B32A32_UINT:
                    return true;
                default:
                    return false;
            }
        }

        // Client-data byte size per texel for the upload facade — the size of
        // what the CALLER hands over, before any conversion to the image's
        // texel format (#691). Compressed formats have no per-texel
        // size and never travel this path.
        [[nodiscard]] u32 ClientBytesPerPixel(const RHI::Format format)
        {
            switch (format)
            {
                case RHI::Format::R8UNorm:
                case RHI::Format::R8UInt:
                    return 1u;
                case RHI::Format::RG8UNorm:
                case RHI::Format::R16UInt:
                    return 2u;
                case RHI::Format::RGB8UNorm:
                    return 3u;
                case RHI::Format::RGBA8UNorm:
                case RHI::Format::RGBA8SRGB:
                case RHI::Format::RG16UInt:
                case RHI::Format::RG16Float:
                case RHI::Format::R32Float:
                case RHI::Format::R32Int:
                case RHI::Format::R32UInt:
                case RHI::Format::D24UNormS8UInt:
                case RHI::Format::D32Float:
                    return 4u;
                case RHI::Format::RGBA16Float:
                case RHI::Format::RG32Float:
                    return 8u;
                case RHI::Format::RGB32Float:
                    return 12u;
                case RHI::Format::RGBA32Float:
                case RHI::Format::RGBA32UInt:
                    return 16u;
                default:
                    return 0u;
            }
        }

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

    void VulkanRendererAPI::UnimplementedStub(const char* entryPoint, StubKind kind) const
    {
        const std::scoped_lock lock(m_StubMutex);
        ++m_UnimplementedStubHits;
        ++m_StubHitsByKind[static_cast<sizet>(kind)];
        if (m_WarnedStubs.insert(entryPoint).second)
        {
            const char* why = kind == StubKind::PreconditionFailure ? "inputs did not resolve"
                              : kind == StubKind::OutsideRecording  ? "called outside a recording bracket"
                                                                    : "no Vulkan lowering yet (#691)";
            OLO_CORE_WARN("[RHI/Vulkan] {} — {}; no-op (further calls counted, not logged)", entryPoint, why);
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
        // Nanoseconds per timestamp tick — the query readback's tick→ns
        // scaling factor (facade contract: timestamps read back in ns).
        m_TimestampPeriodNs = static_cast<f64>(props.limits.timestampPeriod);

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
        // Same VUID family for the mesh-pipeline stages (issue #813): no
        // barrier names them today (per-resource barriers use precise
        // classic-stage or catch-all bits), but a mask that claims to be
        // "every enabled stage" must stay true when one does.
        if (!device->IsMeshShaderEnabled())
        {
            m_EnabledStageMask &= ~(VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT |
                                    VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
        }
        // Same VUID family again for the ray-tracing stages (issue #978).
        // Unlike the mesh bits, barriers DO name these — the AS build->read
        // hazard is exactly a VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD
        // source scope — so on a device without the extensions this strip is
        // what keeps a lowered barrier legal rather than a validation error.
        if (!device->IsRayQueryEnabled())
        {
            m_EnabledStageMask &= ~VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        }
        // The ACCELERATION_STRUCTURE_COPY stage is never in the mask at all:
        // it belongs to VK_KHR_ray_tracing_maintenance1, which is not enabled,
        // so it would be illegal even WITH ray query on. The lowering does not
        // emit it (see VulkanBarrierLowering's AccelerationStructureBuild arm).
        m_EnabledStageMask &= ~VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR;
        if (!device->IsRayTracingPipelineEnabled())
        {
            m_EnabledStageMask &= ~VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        }

        m_LimitsCached = true;
    }

    void VulkanRendererAPI::Init()
    {
        OLO_PROFILE_FUNCTION();
        CacheDeviceLimits();
        OLO_CORE_INFO("[RHI/Vulkan] VulkanRendererAPI up — execution layer (barriers, transient clears, "
                      "dynamic state); pipeline-shaped entry points still stubbed");
    }

    void VulkanRendererAPI::BeginRecording(const VkCommandBuffer cmd)
    {
        OLO_CORE_ASSERT(!OnWorkerContext() && !m_InParallelRegion, "BeginRecording from a RecordParallel item");
        auto& ctx = Ctx();
        OLO_CORE_ASSERT(ctx.Cmd == VK_NULL_HANDLE, "BeginRecording while a recording bracket is already open");
        // Everything that is command-buffer state, in one list
        // (VulkanRecordingContext::ResetForCommandBuffer) — an item context
        // starts from the same list at every fork.
        ctx.ResetForCommandBuffer(cmd);
        // The parallel recorder's tallies are per recording (#806); they stay
        // readable until the next bracket opens.
        m_ParallelStats = {};
        m_BackbufferWritten = false;
    }

    void VulkanRendererAPI::EndRecording()
    {
        OLO_CORE_ASSERT(!OnWorkerContext() && !m_InParallelRegion, "EndRecording from a RecordParallel item");
        auto& ctx = Ctx();
        if (ctx.Query.Pool != VK_NULL_HANDLE)
        {
            // An unbalanced vkCmdBeginQuery makes the whole command buffer
            // invalid at vkEndCommandBuffer — close it loudly instead.
            EndRenderingScope();
            if (ctx.Query.IsElapsed)
            {
                // An elapsed bracket is a TIMESTAMP pair, not a query span:
                // vkCmdEndQuery on a timestamp pool is invalid usage
                // (VUID-vkCmdEndQuery-queryPool-01041), so close it the way
                // EndQuery does — stamp the end slot and mark the entry
                // readable, or the pair would block forever under WAIT.
                OLO_CORE_WARN("[RHI/Vulkan] recording ended with an elapsed-time bracket still open — closing it");
                vkCmdWriteTimestamp2(ctx.Cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ctx.Query.Pool,
                                     ctx.Query.Index + 1u);
                if (auto* entry = VulkanQueryRegistry::Get().Lookup(ctx.Query.Handle); entry != nullptr)
                {
                    entry->Recorded = true;
                }
            }
            else
            {
                OLO_CORE_WARN("[RHI/Vulkan] recording ended with an occlusion query still active — auto-ending it");
                vkCmdEndQuery(ctx.Cmd, ctx.Query.Pool, ctx.Query.Index);
            }
            ctx.Query = {};
        }
        EndRenderingScope();
        // An unconsumed pending clear still HAPPENS (GL cleared eagerly at
        // the request): a shadow-atlas entry culled to zero draws must not
        // lose its clear, nor may the clear leak into the next recording's
        // first scope as a spurious CLEAR loadOp (#691).
        MaterializePendingClear();
        // Everything this command buffer recorded — MaterializePendingClear's
        // late transitions included — is about to be submitted (every opener
        // submits: the frame loop, the device-gated test harnesses), so it
        // becomes the images' EXECUTED layout. Until this point a one-shot,
        // which reaches the queue ahead of this buffer, must not believe those
        // transitions have happened; that is issue #800.
        ctx.Tracker.CommitRecordedToExecuted();
        ctx.Cmd = VK_NULL_HANDLE;
        // A backbuffer publication is scoped to ONE recording by construction:
        // the next frame acquires a different image (see FrameBackbuffer).
        m_Backbuffer = FrameBackbuffer{};
        m_BackbufferWritten = false;
    }

    VkCommandBuffer VulkanRendererAPI::SuspendRecordingForFlush()
    {
        // A secondary cannot be submitted on its own, and the primary cannot
        // be split while items still record into it (#806, amendment (92)
        // rule 7): refuse, and the caller takes its previous-frame arm.
        if (RefuseOnWorker("SuspendRecordingForFlush") || m_InParallelRegion)
        {
            return VK_NULL_HANDLE;
        }
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            return VK_NULL_HANDLE;
        }
        if (ctx.Query.Pool != VK_NULL_HANDLE)
        {
            // A query span cannot cross command buffers — refuse rather than
            // corrupt the recording. (An elapsed-time TIMESTAMP pair could in
            // principle be split across buffers, but its two stamps would then
            // measure across a submit boundary, which is not what the caller
            // asked for; refusing keeps one meaning for both kinds.)
            static std::atomic<bool> s_Warned{ false };
            if (!s_Warned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] mid-frame flush refused: an occlusion query is open");
            }
            return VK_NULL_HANDLE;
        }
        EndRenderingScope();
        // Clear() is lazy until a rendering scope opens. A dependency signal
        // must nevertheless sit after every producer command, including a
        // clear-only pass, so materialize it into the segment being detached
        // instead of carrying it into the consumer command buffer.
        MaterializePendingClear();
        const VkCommandBuffer cmd = ctx.Cmd;
        ctx.Cmd = VK_NULL_HANDLE;
        return cmd;
    }

    void VulkanRendererAPI::MarkSuspendedRecordingSubmitted()
    {
        auto& ctx = Ctx();
        ctx.Tracker.CommitRecordedToExecuted();
    }

    void VulkanRendererAPI::ResumeRecordingAfterFlush(const VkCommandBuffer cmd)
    {
        auto& ctx = Ctx();
        OLO_CORE_ASSERT(ctx.Cmd == VK_NULL_HANDLE, "ResumeRecordingAfterFlush while a recording bracket is open");
        ctx.Cmd = cmd;
        // Only the per-COMMAND-BUFFER caches reset (the new buffer holds no
        // binds); PrepareDraw re-binds pipeline/viewport/scissor per draw, and
        // frame-scoped state — the backbuffer publication, the pending clear,
        // framebuffer selections, draw counters — deliberately survives.
        ctx.ForgetCommandBufferBinds();
        ctx.ScissorRectSet = false;
        ctx.ScissorRectSet = false;
    }

    bool VulkanRendererAPI::RecordStagedImageUpload(VkImage image, u32 mip, u32 baseLayer, u32 width, u32 height,
                                                    const void* data, u64 sizeBytes)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE || image == VK_NULL_HANDLE || data == nullptr || sizeBytes == 0u)
        {
            return false;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return false;
        }

        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = sizeBytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] RecordStagedImageUpload: staging allocation failed ({} bytes)", sizeBytes);
            return false;
        }
        std::memcpy(stagingOut.pMappedData, data, sizeBytes);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, sizeBytes);

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, mip, 1u, baseLayer, 1u };
        std::vector<VkImageMemoryBarrier2> toTransfer;
        StageTransferTransition(image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                toTransfer);
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<u32>(toTransfer.size());
        dep.pImageMemoryBarriers = toTransfer.data();
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, baseLayer, 1u };
        region.imageExtent = { width, height, 1u };
        vkCmdCopyBufferToImage(ctx.Cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);

        // Left in TRANSFER_DST with the tracker in agreement; the bind-time
        // visibility seam transitions to SHADER_READ_ONLY at the next sample.
        // The frame consumes the copy at submit — the staging buffer takes
        // deferred reclaim, never an inline destroy.
        VulkanDeferredReclaim::Get().Enqueue(staging, stagingAllocation);
        return true;
    }

    // --- Viewport / scissor (real dynamic state) ---------------------------

    void VulkanRendererAPI::SetViewport(const u32 x, const u32 y, const u32 width, const u32 height)
    {
        auto& ctx = Ctx();
        ctx.RecordedViewport = { x, y, width, height };
        if (ctx.Cmd == VK_NULL_HANDLE)
            return;

        // Negative-height flip is a pipeline/pass concern; this layer
        // records the plain rect so state packets replay without error.
        // Deliberately does NOT touch the scissor: glViewport never did, and
        // deriving one here would clobber an explicit SetScissorBox. A draw
        // with dynamic scissor state and no scissor set is the pipeline
        // setup to default.
        VkViewport vp{};
        vp.x = static_cast<f32>(x);
        vp.y = static_cast<f32>(y);
        vp.width = static_cast<f32>(width);
        vp.height = static_cast<f32>(height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(ctx.Cmd, 0, 1, &vp);
    }

    Viewport VulkanRendererAPI::GetViewport() const
    {
        auto& ctx = Ctx();
        return ctx.RecordedViewport;
    }

    void VulkanRendererAPI::SetScissorBox(const i32 x, const i32 y, const u32 width, const u32 height)
    {
        auto& ctx = Ctx();
        // Recorded; the draw front-end emits the WITH_COUNT form (the
        // pipelines declare VIEWPORT/SCISSOR_WITH_COUNT dynamic state, which
        // the plain setters cannot satisfy).
        ctx.ScissorRect = { { x, y }, { width, height } };
        ctx.ScissorRectSet = true;
    }

    // --- Barriers ----------------------------------------------------------

    void VulkanRendererAPI::MemoryBarrier(MemoryBarrierFlags flags)
    {
        auto& ctx = Ctx();
        // The flags are the GL lowering (ADR 0011 §1.5); on Vulkan a bare
        // MemoryBarrier call has no per-resource truth, so it lowers to one
        // conservative global memory barrier.
        if (flags == MemoryBarrierFlags::None)
            return;
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("MemoryBarrier(outside recording bracket)", StubKind::OutsideRecording);
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
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);
    }

    void VulkanRendererAPI::IssueBarrierBatch(const MemoryBarrierFlags flags, std::span<const RHI::Barrier> barriers)
    {
        auto& ctx = Ctx();
        OLO_PROFILE_FUNCTION();

        // The enabled-stage narrowing below depends on the device's feature
        // set, and this instance may have been constructed before the device
        // existed (RecreateForSelectedBackend runs pre-window, and Init() is
        // skipped while Renderer::Init is). CacheDeviceLimits early-outs once
        // cached, so this is a no-op on the steady path.
        CacheDeviceLimits();

        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            if (flags != MemoryBarrierFlags::None || !barriers.empty())
                UnimplementedStub("IssueBarrierBatch(outside recording bracket)", StubKind::OutsideRecording);
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
            ctx.Tracker.RegisterImage(image, mipCount, layerCount, info->RegistrationId, info->InitialLayout);

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

            ctx.Tracker.ForEachLayoutRun(
                image, queryRange,
                [&](const VkImageSubresourceRange& run, const VkImageLayout trackedLayout)
                {
                    auto vkBarrier = VulkanBarrierLowering::BuildImageBarrier(barrier, image, aspect, trackedLayout,
                                                                              mipCount, layerCount);
                    vkBarrier.subresourceRange = run;
                    // THE TRACKED LAYOUT IS EVIDENCE ABOUT THE LAST WRITER,
                    // not just about oldLayout. A subresource sitting in a
                    // TRANSFER layout was put there by a transfer command
                    // (ClearTextureFloat's vkCmdClearColorImage,
                    // CopyImageSubData's vkCmdCopyImage) — operations the
                    // render GRAPH never declared, so the planner's `Before`
                    // access names whatever the graph last knew
                    // (ColorAttachmentWrite) and the emitted source scope
                    // misses the transfer entirely. Sync validation calls
                    // that a WRITE_AFTER_WRITE / WRITE_AFTER_READ hazard on
                    // the layout transition, and it is right. Widening the
                    // source scope with the transfer stage+access whenever
                    // the tracker says "transfer layout" costs one extra
                    // stage bit on a barrier that is already transitioning
                    // and cannot over-synchronise anything real.
                    // (ALL_TRANSFER covers CLEAR, COPY, BLIT and RESOLVE.)
                    if (trackedLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
                        trackedLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                    {
                        vkBarrier.srcStageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                        vkBarrier.srcAccessMask |= (trackedLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
                                                       ? VK_ACCESS_2_TRANSFER_WRITE_BIT
                                                       : VK_ACCESS_2_TRANSFER_READ_BIT;
                    }
                    // THE EXACT MIRROR OF THE CASE ABOVE, and the same
                    // argument. A subresource sitting in an ATTACHMENT layout
                    // was left there by a render pass instance, and at
                    // vkCmdEndRendering that instance's storeOp performs a
                    // COLOR_ATTACHMENT_WRITE / DEPTH_STENCIL_ATTACHMENT_WRITE.
                    //
                    // Crucially, the storeOp writes EVERY attachment in the
                    // instance — including ones nothing was drawn to. A
                    // narrowed SetDrawBuffers scope steers fragment outputs;
                    // it does not remove an attachment from the render pass,
                    // so "we only transfer-cleared this one" is true about the
                    // draws and false about the store. A caller that declares
                    // `Before` from what it believes it did (TransferWrite
                    // after a clear) then names a source scope that misses the
                    // store, and sync validation calls the layout transition a
                    // WRITE_AFTER_WRITE hazard against it. It is right.
                    //
                    // Same cost argument as the transfer widening: one extra
                    // stage bit on a barrier that is already transitioning, on
                    // work that has demonstrably already happened, so it
                    // cannot over-synchronise anything real.
                    else if (trackedLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                    {
                        vkBarrier.srcStageMask |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                        vkBarrier.srcAccessMask |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                    }
                    else if (trackedLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
                    {
                        vkBarrier.srcStageMask |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                        vkBarrier.srcAccessMask |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    }
                    // The pure lowering emits the full shader-stage union;
                    // the device knows which stage features are ENABLED
                    // (VUID-…-03929/-03930). A masked-off stage can hold no
                    // work, so narrowing loses no synchronisation.
                    vkBarrier.srcStageMask &= m_EnabledStageMask;
                    vkBarrier.dstStageMask &= m_EnabledStageMask;
                    imageBarriers.push_back(vkBarrier);
                });

            ctx.Tracker.SetLayout(image, queryRange,
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

        vkCmdPipelineBarrier2(ctx.Cmd, &dep);
    }

    bool VulkanRendererAPI::WriteBufferDeviceAddress(const RHI::ResourceHandle destination, const u32 destinationOffset,
                                                     const RHI::ResourceHandle source)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE || (destinationOffset % sizeof(u32)) != 0u ||
            RHI::ResourceRegistry::Get().KindOf(destination) != RHI::ResourceKind::Buffer ||
            RHI::ResourceRegistry::Get().KindOf(source) != RHI::ResourceKind::Buffer)
        {
            return false;
        }

        const auto* destinationEntry = VulkanRootObjectRegistry::Get().Lookup(destination);
        if (destinationEntry == nullptr || destinationEntry->Kind != VulkanRootObjectKind::StorageBuffer ||
            destinationEntry->Object == nullptr)
            return false;
        const auto* destinationBuffer = static_cast<const VulkanStorageBuffer*>(destinationEntry->Object);
        if (destinationBuffer->GetSize() < sizeof(VkDeviceAddress) ||
            destinationOffset > destinationBuffer->GetSize() - sizeof(VkDeviceAddress))
            return false;

        const u64 destinationNative = RHI::ResourceRegistry::Get().ResolveNativeForBackend(destination);
        const u64 sourceNative = RHI::ResourceRegistry::Get().ResolveNativeForBackend(source);
        auto* device = VulkanDevice::Get();
        if (destinationNative == 0u || sourceNative == 0u || device == nullptr)
            return false;

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = reinterpret_cast<VkBuffer>(sourceNative);
        const VkDeviceAddress sourceAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);
        if (sourceAddress == 0u)
            return false;

        // vkCmdUpdateBuffer is a GPU transfer, not a CPU root-struct write.
        // The cull compute consumes this one address as input and writes the
        // root ABI itself before its indirect draw is recorded.
        EndRenderingScope();
        vkCmdUpdateBuffer(ctx.Cmd, reinterpret_cast<VkBuffer>(destinationNative), destinationOffset,
                          sizeof(sourceAddress), &sourceAddress);
        return true;
    }

    bool VulkanRendererAPI::QueryGpuDrivenRootDataLayout(const RHI::ResourceHandle shaderHandle,
                                                         const u32 storageBinding,
                                                         GpuDrivenRootDataLayout& outLayout)
    {
        outLayout = {};
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(shaderHandle);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::Shader || entry->Object == nullptr)
            return false;

        auto* shader = static_cast<VulkanShader*>(entry->Object);
        const auto& layout = shader->GetRootDataLayout();
        const auto field = std::ranges::find_if(
            layout.Fields,
            [storageBinding](const VulkanRootDataLayout::Field& candidate)
            {
                return candidate.Binding.Set == 0u && candidate.Binding.Binding == storageBinding &&
                       candidate.Binding.BindingKind == VulkanShaderBinding::Kind::StorageBuffer;
            });
        if (field == layout.Fields.end())
            return false;

        const GpuDrivenRootDataLayout described{
            .SizeBytes = layout.SizeBytes,
            .GpuWrittenFieldOffsetBytes = field->Offset,
        };
        if (!described.IsValid())
            return false;

        outLayout = described;
        return true;
    }

    bool VulkanRendererAPI::SetNextDrawRootData(const RHI::ResourceHandle rootData,
                                                const u32 gpuWrittenStorageBinding,
                                                const u32 expectedFieldOffsetBytes)
    {
        auto& ctx = Ctx();
        // Failed selection must not leak an earlier unconsumed hand-off into
        // the next draw. A successful call below republishes the address.
        ctx.NextDrawRootDataAddress = 0u;
        if (ctx.Cmd == VK_NULL_HANDLE)
            return false;

        const auto* rootEntry = VulkanRootObjectRegistry::Get().Lookup(rootData);
        if (rootEntry == nullptr || rootEntry->Kind != VulkanRootObjectKind::StorageBuffer || rootEntry->Object == nullptr)
            return false;
        auto* rootBuffer = static_cast<VulkanStorageBuffer*>(rootEntry->Object);

        VulkanShader* shader = VulkanShader::GetCurrentlyBound();
        if (shader == nullptr)
            return false;
        const auto& layout = shader->GetRootDataLayout();
        const auto gpuField = std::ranges::find_if(
            layout.Fields,
            [gpuWrittenStorageBinding](const VulkanRootDataLayout::Field& candidate)
            {
                return candidate.Binding.Set == 0u && candidate.Binding.Binding == gpuWrittenStorageBinding &&
                       candidate.Binding.BindingKind == VulkanShaderBinding::Kind::StorageBuffer;
            });
        constexpr u32 kMaxUpdateBytes = 65536u;
        if (gpuField == layout.Fields.end() || gpuField->Offset != expectedFieldOffsetBytes ||
            layout.SizeBytes == 0u || layout.SizeBytes > rootBuffer->GetSize() || layout.SizeBytes > kMaxUpdateBytes)
        {
            return false;
        }

        const VkDeviceAddress rootAddress = rootBuffer->GetDeviceAddress();
        if (rootAddress == 0u)
            return false;

        // The cull compute already wrote this field. Complete every other
        // field from the binding state that belongs to THIS sorted draw, but
        // never overwrite the GPU-owned address. Recording vkCmdUpdateBuffer
        // keeps the completed struct in GPU command order; no GPU result is
        // read back and no CPU-built replacement is substituted for the
        // compute-written field.
        AssembleRootData(layout, shader->GetName().c_str(), ctx.BoundVertexArray,
                         /*commandOrderedBufferReads=*/true);

        const u32 gpuFieldEnd = gpuField->Offset + static_cast<u32>(sizeof(VkDeviceAddress));
        const VkBuffer native = rootBuffer->GetVkBuffer();
        EndRenderingScope();
        const auto updateRange = [&](const u32 offset, const u32 size)
        {
            if (size == 0u)
                return;
            vkCmdUpdateBuffer(ctx.Cmd, native, offset, size, ctx.RootScratch.data() + offset);
        };
        if (gpuField->Offset > 0u)
            updateRange(0u, gpuField->Offset);
        if (gpuFieldEnd < layout.SizeBytes)
            updateRange(gpuFieldEnd, layout.SizeBytes - gpuFieldEnd);

        // Both the compute-written address and the transfer-written static
        // fields must be visible when descriptor mapping dereferences the root
        // struct. This resource-local barrier is intentionally after the last
        // update and immediately before the draw that consumes it.
        VkBufferMemoryBarrier2 rootBarrier{};
        rootBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        rootBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        rootBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        rootBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        rootBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        rootBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rootBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rootBarrier.buffer = native;
        rootBarrier.offset = 0u;
        rootBarrier.size = layout.SizeBytes;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = 1u;
        dependency.pBufferMemoryBarriers = &rootBarrier;
        vkCmdPipelineBarrier2(ctx.Cmd, &dependency);

        ctx.NextDrawRootDataAddress = rootAddress;
        return true;
    }

    bool VulkanRendererAPI::SupportsRenderGraphFenceSubmission() const
    {
        auto& ctx = Ctx();
        // A bare VulkanRendererAPI in a device-gated fixture has a command
        // buffer but no swapchain/frame owner. It must not stage a fence pair
        // into that fixture's single submit: producer signal and consumer wait
        // would then be the invalid same-submit shape this API exists to avoid.
        const VulkanContext* context = VulkanContext::Get();
        return ctx.Cmd != VK_NULL_HANDLE && context != nullptr && context->CanSubmitRenderGraphFenceSegments();
    }

    bool VulkanRendererAPI::SubmitRenderGraphFenceSegment()
    {
        if (VulkanContext* context = VulkanContext::Get(); context != nullptr)
            return context->SubmitRenderGraphFenceSegment();
        return false;
    }

    // --- Transient clears (the poison instrument's backend) ----------------

    void VulkanRendererAPI::ClearTextureFloat(const RHI::ResourceHandle texture, const u32 mipLevel, const glm::vec4& color)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            // GL's glClearTexImage works at ANY time; load-time callers
            // (DDGIProbeUpdatePass's 1x1 placeholder/black-cubemap Init
            // clears) depend on that. Re-enter through a one-shot submit so
            // the clear LANDS instead of leaving the texture undefined —
            // the layout tracker updates inside stay correct for the frame
            // recordings that follow.
            VulkanOneShot::Submit("ClearTextureFloat(load-time one-shot)",
                                  [&](const VkCommandBuffer cmd)
                                  {
                                      ctx.Cmd = cmd;
                                      ClearTextureFloat(texture, mipLevel, color);
                                      ctx.Cmd = VK_NULL_HANDLE;
                                  });
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
        ctx.Tracker.RegisterImage(image, mipCount, std::max(info->ArrayLayers, 1u), info->RegistrationId);

        VkImageSubresourceRange range{};
        range.aspectMask = VulkanBarrierLowering::AspectMaskFor(aspect);
        range.baseMipLevel = std::min(mipLevel, mipCount - 1u);
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = VK_REMAINING_ARRAY_LAYERS;

        // Conservative pre-barrier into TRANSFER_DST, exact per layout run.
        std::vector<VkImageMemoryBarrier2> toTransfer;
        ctx.Tracker.ForEachLayoutRun(
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
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);
        ctx.Tracker.SetLayout(image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        if (aspect == RHI::TextureAspect::Color)
        {
            VkClearColorValue clear{};
            clear.float32[0] = color.r;
            clear.float32[1] = color.g;
            clear.float32[2] = color.b;
            clear.float32[3] = color.a;
            vkCmdClearColorImage(ctx.Cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        }
        else
        {
            VkClearDepthStencilValue clear{};
            clear.depth = color.r;
            clear.stencil = 0;
            vkCmdClearDepthStencilImage(ctx.Cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        }
    }

    void VulkanRendererAPI::ClearTextureUInt(const RHI::ResourceHandle texture, const u32 mipLevel, const u32 value)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            // Same load-time one-shot fallback as ClearTextureFloat above.
            VulkanOneShot::Submit("ClearTextureUInt(load-time one-shot)",
                                  [&](const VkCommandBuffer cmd)
                                  {
                                      ctx.Cmd = cmd;
                                      ClearTextureUInt(texture, mipLevel, value);
                                      ctx.Cmd = VK_NULL_HANDLE;
                                  });
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
        ctx.Tracker.RegisterImage(image, mipCount, std::max(info->ArrayLayers, 1u), info->RegistrationId);

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = std::min(mipLevel, mipCount - 1u);
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = VK_REMAINING_ARRAY_LAYERS;

        std::vector<VkImageMemoryBarrier2> toTransfer;
        ctx.Tracker.ForEachLayoutRun(
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
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);
        ctx.Tracker.SetLayout(image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // Designated init ACTIVATES the union's uint32 member — value-init
        // (`{}`) would activate float32 (the first member) and make these
        // writes inactive-member accesses.
        const VkClearColorValue clear{ .uint32 = { value, value, value, value } };
        vkCmdClearColorImage(ctx.Cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }

    void VulkanRendererAPI::ClearBufferFloat(const RHI::ResourceHandle buffer, const f32 value)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("ClearBufferFloat(outside recording bracket)", StubKind::OutsideRecording);
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

        vkCmdPipelineBarrier2(ctx.Cmd, &dep);
        vkCmdFillBuffer(ctx.Cmd, reinterpret_cast<VkBuffer>(native), 0, VK_WHOLE_SIZE, std::bit_cast<u32>(value));
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);
    }

    void VulkanRendererAPI::ClearBufferUInt(const RHI::ResourceHandle buffer, const u32 value, const u64 offset,
                                            const u64 size)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("ClearBufferUInt(outside recording bracket)", StubKind::OutsideRecording);
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

        vkCmdPipelineBarrier2(ctx.Cmd, &dep);
        // ~0ull is VK_WHOLE_SIZE, so the default covers the whole buffer.
        vkCmdFillBuffer(ctx.Cmd, reinterpret_cast<VkBuffer>(native), offset, size == ~0ull ? VK_WHOLE_SIZE : size, value);
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);
    }

    // --- Debug labels / device queries -------------------------------------

    void VulkanRendererAPI::PushDebugGroup(u32 /*id*/, const std::string_view label)
    {
        auto& ctx = Ctx();
        // vkCmdBeginDebugUtilsLabelEXT is loaded only when the debug-utils
        // extension was enabled (debug builds with the validation layer) —
        // the pointer probe IS the capability check under volk.
        if (ctx.Cmd == VK_NULL_HANDLE || vkCmdBeginDebugUtilsLabelEXT == nullptr)
            return;
        const std::string labelStr(label);
        VkDebugUtilsLabelEXT vkLabel{};
        vkLabel.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        vkLabel.pLabelName = labelStr.c_str();
        vkCmdBeginDebugUtilsLabelEXT(ctx.Cmd, &vkLabel);
    }

    void VulkanRendererAPI::PopDebugGroup()
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE || vkCmdEndDebugUtilsLabelEXT == nullptr)
            return;
        vkCmdEndDebugUtilsLabelEXT(ctx.Cmd);
    }

    void VulkanRendererAPI::WaitForDeviceIdle()
    {
        if (RefuseOnWorker("WaitForDeviceIdle"))
        {
            return;
        }
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

    bool VulkanRendererAPI::SupportsMeshShaders() const
    {
        // ENABLED on the logical device, not merely supported by the physical
        // one (the SupportsInt64ShaderAtomics rule): VulkanDevice turned
        // VK_EXT_mesh_shader's taskShader+meshShader on only when the driver
        // had both, and DrawMeshTasks refuses without them (issue #813).
        const auto* device = VulkanDevice::Get();
        return device != nullptr && device->IsMeshShaderEnabled();
    }

    RayTracing::Capabilities VulkanRendererAPI::GetRayTracingCapabilities() const
    {
        // Same enabled-not-merely-supported rule, and the device is the single
        // owner of the answer: it is the only place that saw the extension
        // list, the feature probe, the vkCreateDevice result and the volk
        // pointers, which are four independent ways this can be unavailable.
        RayTracing::Capabilities capabilities{};
        const auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            capabilities.Reason = RayTracing::UnsupportedReason::NoDevice;
            return capabilities;
        }
        capabilities.Supported = device->IsRayQueryEnabled();
        capabilities.RayTracingPipeline = device->IsRayTracingPipelineEnabled();
        capabilities.Reason = device->GetRayTracingUnsupportedReason();
        capabilities.Properties = device->GetRayTracingProperties();
        return capabilities;
    }

    VkCommandBuffer VulkanRendererAPI::BeginAccelerationStructureRecording()
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            return VK_NULL_HANDLE;
        }
        // vkCmdBuildAccelerationStructuresKHR, vkCmdCopyAccelerationStructureKHR,
        // vkCmdResetQueryPool and vkCmdPipelineBarrier2 are all illegal inside
        // a render-pass instance, and this backend's rendering scope is opened
        // lazily — so closing it here is not defensive, it is the precondition.
        EndRenderingScope();
        return ctx.Cmd;
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
        auto& ctx = Ctx();
        ctx.State.ClearColor = color;
    }
    void VulkanRendererAPI::SetClearDepth(const f32 depth)
    {
        auto& ctx = Ctx();
        ctx.State.ClearDepth = depth;
    }
    void VulkanRendererAPI::SetDepthTest(const bool value)
    {
        auto& ctx = Ctx();
        ctx.State.DepthTest = value;
    }
    void VulkanRendererAPI::SetDepthMask(const bool value)
    {
        auto& ctx = Ctx();
        ctx.State.DepthWrite = value;
    }
    void VulkanRendererAPI::SetDepthFunc(const RHI::CompareOp func)
    {
        auto& ctx = Ctx();
        ctx.State.DepthFunc = func;
    }
    void VulkanRendererAPI::SetBlendState(const bool value)
    {
        auto& ctx = Ctx();
        // Deliberately does NOT touch AttachmentBlend, unlike SetColorMask
        // below. A per-attachment blend opinion OUTRANKS this flag in both
        // directions and is withdrawn only by ResetBlendStateForAttachment —
        // see the rule at the AttachmentBlend declaration. The GL backend
        // re-asserts its standing opinions after glEnable/glDisable(GL_BLEND)
        // so that both backends implement that one rule.
        ctx.State.Blend = value;
    }
    void VulkanRendererAPI::SetBlendFunc(const RHI::BlendFactor sfactor, const RHI::BlendFactor dfactor)
    {
        auto& ctx = Ctx();
        ctx.State.BlendSrcRGB = sfactor;
        ctx.State.BlendDstRGB = dfactor;
        ctx.State.BlendSrcAlpha = sfactor;
        ctx.State.BlendDstAlpha = dfactor;
        // glBlendFunc overwrites EVERY buffer's func in GL — the recorded
        // per-attachment funcs must stop diverting (see the struct comment).
        for (auto& funcOpinion : ctx.State.AttachmentBlendFunc)
            funcOpinion = VulkanRecordedPipelineState::AttachmentBlendFuncOpinion::Inherit;
    }
    void VulkanRendererAPI::SetBlendFuncSeparate(const RHI::BlendFactor srcRGB, const RHI::BlendFactor dstRGB,
                                                 const RHI::BlendFactor srcAlpha, const RHI::BlendFactor dstAlpha)
    {
        auto& ctx = Ctx();
        ctx.State.BlendSrcRGB = srcRGB;
        ctx.State.BlendDstRGB = dstRGB;
        ctx.State.BlendSrcAlpha = srcAlpha;
        ctx.State.BlendDstAlpha = dstAlpha;
        // Same glBlendFuncSeparate global-overwrite semantics as SetBlendFunc.
        for (auto& funcOpinion : ctx.State.AttachmentBlendFunc)
            funcOpinion = VulkanRecordedPipelineState::AttachmentBlendFuncOpinion::Inherit;
    }
    void VulkanRendererAPI::SetBlendEquation(const RHI::BlendOp mode)
    {
        auto& ctx = Ctx();
        ctx.State.BlendEquation = mode;
    }
    void VulkanRendererAPI::EnableStencilTest()
    {
        auto& ctx = Ctx();
        ctx.State.StencilTest = true;
    }
    void VulkanRendererAPI::DisableStencilTest()
    {
        auto& ctx = Ctx();
        ctx.State.StencilTest = false;
    }
    bool VulkanRendererAPI::IsStencilTestEnabled() const
    {
        auto& ctx = Ctx();
        return ctx.State.StencilTest;
    }
    void VulkanRendererAPI::SetStencilFunc(const RHI::CompareOp func, const i32 ref, const u32 mask)
    {
        auto& ctx = Ctx();
        ctx.State.StencilFunc = func;
        ctx.State.StencilRef = ref;
        ctx.State.StencilReadMask = mask;
    }
    void VulkanRendererAPI::SetStencilOp(const RHI::StencilOp sfail, const RHI::StencilOp dpfail, const RHI::StencilOp dppass)
    {
        auto& ctx = Ctx();
        ctx.State.StencilFail = sfail;
        ctx.State.StencilDepthFail = dpfail;
        ctx.State.StencilPass = dppass;
    }
    void VulkanRendererAPI::SetStencilMask(const u32 mask)
    {
        auto& ctx = Ctx();
        ctx.State.StencilWriteMask = mask;
    }
    void VulkanRendererAPI::EnableCulling()
    {
        auto& ctx = Ctx();
        ctx.State.Culling = true;
    }
    void VulkanRendererAPI::DisableCulling()
    {
        auto& ctx = Ctx();
        ctx.State.Culling = false;
    }
    void VulkanRendererAPI::FrontCull()
    {
        auto& ctx = Ctx();
        ctx.State.CullFace = RHI::CullMode::Front;
    }
    void VulkanRendererAPI::BackCull()
    {
        auto& ctx = Ctx();
        ctx.State.CullFace = RHI::CullMode::Back;
    }
    void VulkanRendererAPI::SetCullFace(const RHI::CullMode face)
    {
        auto& ctx = Ctx();
        ctx.State.CullFace = face;
    }
    void VulkanRendererAPI::SetFrontFace(const RHI::FrontFace face)
    {
        auto& ctx = Ctx();
        ctx.State.FrontFaceWinding = face;
    }
    void VulkanRendererAPI::SetPolygonMode(const RHI::PolygonMode mode)
    {
        auto& ctx = Ctx();
        ctx.State.PolygonMode = mode;
    }
    void VulkanRendererAPI::SetPolygonOffset(const f32 factor, const f32 units)
    {
        auto& ctx = Ctx();
        ctx.State.PolygonOffsetEnabled = true;
        ctx.State.PolygonOffsetFactor = factor;
        ctx.State.PolygonOffsetUnits = units;
    }
    void VulkanRendererAPI::EnableScissorTest()
    {
        auto& ctx = Ctx();
        ctx.State.ScissorTest = true;
    }
    void VulkanRendererAPI::DisableScissorTest()
    {
        auto& ctx = Ctx();
        ctx.State.ScissorTest = false;
    }
    void VulkanRendererAPI::EnableMultisampling()
    {
        auto& ctx = Ctx();
        ctx.State.Multisampling = true;
    }
    void VulkanRendererAPI::DisableMultisampling()
    {
        auto& ctx = Ctx();
        ctx.State.Multisampling = false;
    }
    void VulkanRendererAPI::SetLineWidth(const f32 width)
    {
        auto& ctx = Ctx();
        ctx.State.LineWidth = width;
    }
    void VulkanRendererAPI::SetColorMask(const bool red, const bool green, const bool blue, const bool alpha)
    {
        auto& ctx = Ctx();
        ctx.State.ColorMask[0] = red;
        ctx.State.ColorMask[1] = green;
        ctx.State.ColorMask[2] = blue;
        ctx.State.ColorMask[3] = alpha;
        // glColorMask writes EVERY draw buffer's mask, so it also CLEARS any
        // divergence a previous glColorMaski installed — the same global-
        // overwrite rule SetBlendFunc already honours, and the rule
        // CommandDispatch::ApplyRenderState depends on: that function only ever
        // DISABLES attachments (`if (!(colorAttachmentWriteMask & bit))`) and
        // relies on this call to have re-enabled them first. Without the fill a
        // narrowed attachment stayed masked for the rest of the PROCESS — one
        // Renderer3D::DrawLine (colorAttachmentWriteMask = 0x01) killed every
        // G-Buffer attachment above 0 from that frame on, which is issue #823.
        for (u32 attachment = 0; attachment < VulkanRecordedPipelineState::kMaxAttachments; ++attachment)
            SetColorMaskForAttachment(attachment, red, green, blue, alpha);
    }
    void VulkanRendererAPI::SetColorMaskForAttachment(const u32 attachment, const bool red, const bool green,
                                                      const bool blue, const bool alpha)
    {
        auto& ctx = Ctx();
        if (attachment >= VulkanRecordedPipelineState::kMaxAttachments)
            return;
        ctx.State.AttachmentColorMask[attachment] = static_cast<u8>((red ? 1u : 0u) | (green ? 2u : 0u) |
                                                                    (blue ? 4u : 0u) | (alpha ? 8u : 0u));
    }
    void VulkanRendererAPI::SetBlendStateForAttachment(const u32 attachment, const bool enabled)
    {
        auto& ctx = Ctx();
        if (attachment >= VulkanRecordedPipelineState::kMaxAttachments)
            return;
        // States an opinion that outranks the global flag until it is
        // withdrawn. `false` is ForceOff, NOT "back to normal" — a pass
        // restoring its state wants ResetBlendStateForAttachment (issue #896).
        ctx.State.AttachmentBlend[attachment] = enabled
                                                    ? VulkanRecordedPipelineState::AttachmentBlendOpinion::ForceOn
                                                    : VulkanRecordedPipelineState::AttachmentBlendOpinion::ForceOff;
    }
    void VulkanRendererAPI::ResetBlendStateForAttachment(const u32 attachment)
    {
        auto& ctx = Ctx();
        if (attachment >= VulkanRecordedPipelineState::kMaxAttachments)
            return;
        ctx.State.AttachmentBlend[attachment] = VulkanRecordedPipelineState::AttachmentBlendOpinion::Inherit;
    }
    void VulkanRendererAPI::SetBlendFuncForAttachment(const u32 attachment, const RHI::BlendFactor src, const RHI::BlendFactor dst)
    {
        auto& ctx = Ctx();
        if (attachment >= VulkanRecordedPipelineState::kMaxAttachments)
            return;
        ctx.State.AttachmentBlendSrc[attachment] = src;
        ctx.State.AttachmentBlendDst[attachment] = dst;
        // glBlendFunci semantics — diverted until the global setter withdraws it.
        ctx.State.AttachmentBlendFunc[attachment] = VulkanRecordedPipelineState::AttachmentBlendFuncOpinion::Diverted;
    }
    void VulkanRendererAPI::SetPatchVertexCount(const u32 patchVertices)
    {
        auto& ctx = Ctx();
        ctx.State.PatchVertexCount = patchVertices;
    }

    // Rendering scope + draw assembly --------------------------

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
        auto& ctx = Ctx();
        if (!ctx.Scope.Active)
        {
            return;
        }
        vkCmdEndRendering(ctx.Cmd);
        ctx.Scope.Active = false;
        ctx.Scope.DepthArrayView = VK_NULL_HANDLE;
        ctx.Scope.TargetIsBackbuffer = false;
        ctx.Scope.BackbufferView = VK_NULL_HANDLE;
        // Pending clears survive the scope END only if never consumed; the
        // next scope begin re-reads them. Target/DrawList stay published.
    }

    // Conservative source access for a scope-open attachment transition. The
    // graph's planner orders passes but never lowers a FRAMEBUFFER-kind write
    // to an image barrier — the attachment-layout transition is the draw
    // front-end's job. The true prior access isn't known here, so it is
    // guessed from the tracker's current layout: exact for UNDEFINED (first
    // use) and attachment-to-attachment; conservative otherwise.
    static RHI::Access AccessGuessForLayout(const VkImageLayout layout)
    {
        switch (layout)
        {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                return RHI::Access::Undefined;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return RHI::Access::ColorAttachmentWrite;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return RHI::Access::DepthStencilAttachmentWrite;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                return RHI::Access::DepthStencilAttachmentRead;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return RHI::Access::ShaderSampleRead;
            case VK_IMAGE_LAYOUT_GENERAL:
                return RHI::Access::StorageReadWrite;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return RHI::Access::TransferWrite;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return RHI::Access::TransferRead;
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                // The presentation engine's read is ordered by the acquire
                // SEMAPHORE, not by an access mask — so the src scope here is
                // deliberately empty (the lowering maps Present to
                // STAGE_NONE/ACCESS_NONE). Naming a stage instead would be a
                // lie the sync validation has every right to disbelieve.
                return RHI::Access::Present;
            default:
                return RHI::Access::StorageReadWrite;
        }
    }

    bool VulkanRendererAPI::ShouldTargetBackbuffer() const
    {
        // The swapchain image is the render thread's (amendment (92) rule 7):
        // an item with nothing bound has no target, and its draw is dropped
        // with the existing warn-once rather than painting the backbuffer
        // from a secondary.
        return !OnWorkerContext() && VulkanBindingState::Get().GetCurrentFramebuffer() == nullptr &&
               m_Backbuffer.IsValid();
    }

    VkExtent2D VulkanRendererAPI::ScopeExtent() const
    {
        auto& ctx = Ctx();
        if (!ctx.Scope.Active)
        {
            return { 0u, 0u };
        }
        if (ctx.Scope.TargetIsBackbuffer)
        {
            return { std::max(m_Backbuffer.Width, 1u), std::max(m_Backbuffer.Height, 1u) };
        }
        const auto& spec = ctx.Scope.Target->GetSpecification();
        return { std::max(spec.Width, 1u), std::max(spec.Height, 1u) };
    }

    bool VulkanRendererAPI::ScopeMatchesCurrentTarget() const
    {
        auto& ctx = Ctx();
        if (!ctx.Scope.Active)
        {
            return false;
        }
        auto* target = VulkanBindingState::Get().GetCurrentFramebuffer();
        // The published backbuffer is a target in its own right (see
        // FrameBackbuffer): "nothing bound" means the DEFAULT framebuffer
        // while a publication is live, so a backbuffer scope must not be
        // confused with the no-target scope that predates the import.
        if (ctx.Scope.TargetIsBackbuffer || ShouldTargetBackbuffer())
        {
            return ctx.Scope.TargetIsBackbuffer && ShouldTargetBackbuffer() &&
                   ctx.Scope.BackbufferView == m_Backbuffer.View;
        }
        if (ctx.Scope.Target != target)
        {
            return false;
        }
        if (target == nullptr)
        {
            return true;
        }
        // #691 §4: a layered shadow pass renders every cascade through
        // ONE framebuffer object, calling AttachDepthTextureArrayLayer between
        // them. Comparing only the framebuffer POINTER would keep the scope
        // open across that change and paint every cascade into cascade 0's
        // view — silently, with no validation error (the view is legal, just
        // stale). The selection is part of the scope's identity.
        const auto depthArray = GetFramebufferDepthArraySelection(target->GetRHIHandle());
        const VkImageView selected = depthArray.Active ? depthArray.View : VK_NULL_HANDLE;
        return ctx.Scope.DepthArrayView == selected;
    }

    bool VulkanRendererAPI::PendingClearMatchesCurrentTarget() const
    {
        auto& ctx = Ctx();
        if (!ctx.Pending.Any())
        {
            return false;
        }
        auto* target = VulkanBindingState::Get().GetCurrentFramebuffer();
        // Same identity rules as ScopeMatchesCurrentTarget: the backbuffer
        // publication and the depth-array layer selection are both part of
        // "which target asked".
        if (ctx.Pending.TargetIsBackbuffer || ShouldTargetBackbuffer())
        {
            return ctx.Pending.TargetIsBackbuffer && ShouldTargetBackbuffer() &&
                   ctx.Pending.BackbufferView == m_Backbuffer.View;
        }
        if (ctx.Pending.Target != target)
        {
            return false;
        }
        if (target == nullptr)
        {
            return true;
        }
        const auto depthArray = GetFramebufferDepthArraySelection(target->GetRHIHandle());
        const VkImageView selected = depthArray.Active ? depthArray.View : VK_NULL_HANDLE;
        return ctx.Pending.DepthArrayView == selected;
    }

    void VulkanRendererAPI::RecordPendingClear(const bool color, const bool depth)
    {
        auto& ctx = Ctx();
        // A pending clear that belongs to a DIFFERENT target must happen
        // before this one is recorded — GL executed it eagerly at its own
        // glClear.
        if (ctx.Pending.Any() && !PendingClearMatchesCurrentTarget())
        {
            MaterializePendingClear();
        }

        auto* target = VulkanBindingState::Get().GetCurrentFramebuffer();
        ctx.Pending.Color = ctx.Pending.Color || color;
        ctx.Pending.Depth = ctx.Pending.Depth || depth;
        ctx.Pending.Target = target;
        ctx.Pending.TargetIsBackbuffer = ShouldTargetBackbuffer();
        ctx.Pending.BackbufferView = ctx.Pending.TargetIsBackbuffer ? m_Backbuffer.View : VK_NULL_HANDLE;
        ctx.Pending.DepthArrayView = VK_NULL_HANDLE;
        ctx.Pending.DepthArrayImage = VK_NULL_HANDLE;
        ctx.Pending.DepthArrayLayer = 0u;
        ctx.Pending.DepthArrayHandle = {};
        if (target != nullptr)
        {
            if (const auto depthArray = GetFramebufferDepthArraySelection(target->GetRHIHandle()); depthArray.Active)
            {
                ctx.Pending.DepthArrayView = depthArray.View;
                ctx.Pending.DepthArrayImage = depthArray.Image;
                ctx.Pending.DepthArrayLayer = depthArray.Layer;
                ctx.Pending.DepthArrayHandle = depthArray.Handle;
            }
        }
        // Values are captured NOW (GL uses the state at glClear time; the
        // scope-open may be many state changes later).
        if (color)
        {
            ctx.Pending.ClearColor = ctx.State.ClearColor;
        }
        if (depth)
        {
            ctx.Pending.ClearDepth = ctx.State.ClearDepth;
        }
    }

    void VulkanRendererAPI::MaterializePendingClear()
    {
        auto& ctx = Ctx();
        if (!ctx.Pending.Any() || ctx.Cmd == VK_NULL_HANDLE)
        {
            ctx.Pending = PendingClear{};
            return;
        }
        // Take a copy and drop the record FIRST: ClearTextureFloat below ends
        // the scope and must not observe (or re-enter) the pending state.
        const PendingClear pending = ctx.Pending;
        ctx.Pending = PendingClear{};

        if (pending.TargetIsBackbuffer)
        {
            if (pending.Color && m_Backbuffer.IsValid() && pending.BackbufferView == m_Backbuffer.View)
            {
                ClearTextureFloat(m_Backbuffer.Handle, 0u, pending.ClearColor);
                // The screen now holds defined content — the present fallback
                // must not treat the frame as untouched.
                m_BackbufferWritten = true;
            }
            return;
        }

        auto* fb = pending.Target;
        if (fb == nullptr)
        {
            return;
        }

        if (pending.Color)
        {
            // GL's glClear writes the DRAW-BUFFER selection (identity when
            // none is stored), same mapping the scope-open uses.
            const FramebufferAttachmentSelection* selection = FindSelection(fb->GetRHIHandle());
            const bool identity = selection == nullptr || selection->DrawListCount == 0;
            const u32 count = identity ? fb->GetColorAttachmentCount() : selection->DrawListCount;
            for (u32 i = 0; i < count; ++i)
            {
                const u32 attachment = identity ? i : selection->DrawList[i];
                if (attachment == RHI::NoAttachment || attachment >= fb->GetColorAttachmentCount())
                {
                    continue;
                }
                ClearTextureFloat(fb->GetColorAttachmentHandle(attachment), 0u, pending.ClearColor);
            }
        }
        if (pending.Depth)
        {
            if (pending.DepthArrayImage != VK_NULL_HANDLE)
            {
                // Only the layer that was attached at request time — GL
                // cleared exactly that layer. ClearTextureFloat spans every
                // layer, so this arm transitions and clears the single run
                // inline (the ClearTextureFloat shape, CLEAR-stage scope).
                EndRenderingScope();
                if (const auto* info = VulkanImageInfoRegistry::Get().Lookup(pending.DepthArrayImage);
                    info != nullptr)
                {
                    ctx.Tracker.RegisterImage(pending.DepthArrayImage, std::max(info->MipLevels, 1u),
                                              std::max(info->ArrayLayers, 1u), info->RegistrationId,
                                              info->InitialLayout);
                    VkImageSubresourceRange range{};
                    range.aspectMask = VulkanBarrierLowering::AspectMaskFor(AspectFromInfo(*info));
                    range.baseMipLevel = 0u;
                    range.levelCount = 1u;
                    range.baseArrayLayer = pending.DepthArrayLayer;
                    range.layerCount = 1u;
                    std::vector<VkImageMemoryBarrier2> toTransfer;
                    ctx.Tracker.ForEachLayoutRun(
                        pending.DepthArrayImage, range,
                        [&](const VkImageSubresourceRange& run, const VkImageLayout trackedLayout)
                        {
                            VkImageMemoryBarrier2 b{};
                            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                            b.srcStageMask = (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                                                 ? VK_PIPELINE_STAGE_2_NONE
                                                 : kAllStages;
                            b.srcAccessMask =
                                (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ACCESS_2_NONE : kAllAccess;
                            b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                            b.oldLayout = trackedLayout;
                            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            b.image = pending.DepthArrayImage;
                            b.subresourceRange = run;
                            toTransfer.push_back(b);
                        });
                    VkDependencyInfo dep{};
                    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                    dep.imageMemoryBarrierCount = static_cast<u32>(toTransfer.size());
                    dep.pImageMemoryBarriers = toTransfer.data();
                    vkCmdPipelineBarrier2(ctx.Cmd, &dep);
                    ctx.Tracker.SetLayout(pending.DepthArrayImage, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                    VkClearDepthStencilValue clear{};
                    clear.depth = pending.ClearDepth;
                    clear.stencil = 0u;
                    vkCmdClearDepthStencilImage(ctx.Cmd, pending.DepthArrayImage,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
                }
            }
            else if (const RHI::ResourceHandle depthHandle = fb->GetDepthAttachmentHandle(); depthHandle.IsValid())
            {
                // ClearTextureFloat's depth path clears depth = color.r.
                ClearTextureFloat(depthHandle, 0u, glm::vec4(pending.ClearDepth, 0.0f, 0.0f, 0.0f));
            }
        }
    }

    bool VulkanRendererAPI::EnsureRenderingScopeForDraw()
    {
        auto& ctx = Ctx();
        auto* target = VulkanBindingState::Get().GetCurrentFramebuffer();
        const bool backbuffer = ShouldTargetBackbuffer();
        if (target == nullptr && !backbuffer)
        {
            static std::atomic<bool> s_Warned{ false };
            if (!s_Warned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] draw with no framebuffer published and no backbuffer for this frame "
                              "— dropped");
            }
            return false;
        }

        if (ScopeMatchesCurrentTarget())
        {
            return true;
        }
        EndRenderingScope();

        // A pending clear for a DIFFERENT target must not fold into THIS
        // scope's loadOp (the `Bind(A); Clear(); Bind(B); Draw()` bug —
        // an earlier pass cleared B and left A untouched). Materialize it against its
        // own target first; the folds below then apply only when the pending
        // requester IS this scope's target (#691).
        if (ctx.Pending.Any() && !PendingClearMatchesCurrentTarget())
        {
            MaterializePendingClear();
        }
        const bool foldColor = ctx.Pending.Color && PendingClearMatchesCurrentTarget();
        const bool foldDepth = ctx.Pending.Depth && PendingClearMatchesCurrentTarget();

        if (backbuffer)
        {
            // The swapchain image: one color attachment, no depth, no
            // draw-buffer selection (GL's default framebuffer offers exactly
            // GL_BACK). Everything else — the pre-scope layout transition,
            // the pending-clear fold into loadOp, the PSO's target
            // description — is the framebuffer path's, verbatim.
            ctx.ScopeTargets = VulkanRenderTargetDesc{};
            ctx.ScopeTargets.Samples = 1u;
            ctx.ScopeTargets.ColorCount = 1u;
            ctx.ScopeTargets.ColorFormats[0] = m_Backbuffer.Format;

            RHI::Barrier toAttachment{};
            toAttachment.Resource = m_Backbuffer.Handle;
            toAttachment.Range.BaseMip = 0u;
            toAttachment.Range.MipCount = 1u;
            toAttachment.Range.BaseLayer = 0u;
            toAttachment.Range.LayerCount = 1u;
            const VkImageSubresourceRange probe{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
            toAttachment.Before = AccessGuessForLayout(ctx.Tracker.CurrentLayout(m_Backbuffer.Image, probe));
            toAttachment.After = RHI::Access::ColorAttachmentWrite;
            IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ &toAttachment, 1 });

            VkRenderingAttachmentInfo colorInfo{};
            colorInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorInfo.imageView = m_Backbuffer.View;
            colorInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorInfo.loadOp = foldColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            colorInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            // The values captured at the Clear() request, not the live state.
            colorInfo.clearValue.color = { { ctx.Pending.ClearColor.r, ctx.Pending.ClearColor.g,
                                             ctx.Pending.ClearColor.b, ctx.Pending.ClearColor.a } };

            VkRenderingInfo backbufferRendering{};
            backbufferRendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            backbufferRendering.renderArea = { { 0, 0 },
                                               { std::max(m_Backbuffer.Width, 1u),
                                                 std::max(m_Backbuffer.Height, 1u) } };
            backbufferRendering.layerCount = 1;
            backbufferRendering.colorAttachmentCount = 1;
            backbufferRendering.pColorAttachments = &colorInfo;
            vkCmdBeginRendering(ctx.Cmd, &backbufferRendering);

            ctx.Scope.Active = true;
            ctx.Scope.Target = nullptr;
            ctx.Scope.TargetIsBackbuffer = true;
            ctx.Scope.BackbufferView = m_Backbuffer.View;
            ctx.Scope.DepthArrayView = VK_NULL_HANDLE;
            // Consumed by the loadOp above (a pending depth against the
            // depthless default framebuffer is GL's silent no-op).
            ctx.Pending = PendingClear{};
            m_BackbufferWritten = true;
            return true;
        }

        const auto& spec = target->GetSpecification();
        const u32 width = std::max(spec.Width, 1u);
        const u32 height = std::max(spec.Height, 1u);

        // The SetDrawBuffers selection maps fragment output location i onto
        // attachment DrawList[i] (GL's glDrawBuffers semantics). Identity
        // over every color attachment when no selection was recorded.
        std::array<VkRenderingAttachmentInfo, 8> colorInfos{};
        ctx.ScopeTargets = VulkanRenderTargetDesc{};
        ctx.ScopeTargets.Samples = std::max(spec.Samples, 1u);

        // Attachment transitions collected while building the infos below and
        // recorded through IssueBarrierBatch BEFORE vkCmdBeginRendering: the
        // rendering infos declare (COLOR/DEPTH)_ATTACHMENT_OPTIMAL, and
        // nothing else transitions the images there — the graph's planner
        // cannot see framebuffer attachments (an FB-kind write never lowers
        // to an image barrier), so without this every first use renders into
        // an UNDEFINED-layout image and later sample barriers lower with an
        // empty source scope (both are validation errors; found by
        // VulkanPassSuiteTest's first full-graph frame).
        std::vector<RHI::Barrier> scopeBarriers;

        // Per-framebuffer persistent selection (GL's glNamedFramebufferDrawBuffers
        // model — both the bound and the raw-handle setter write the same map).
        const FramebufferAttachmentSelection* selection = FindSelection(target->GetRHIHandle());
        const bool identity = selection == nullptr || selection->DrawListCount == 0;
        const u32 outputCount =
            identity ? std::min<u32>(target->GetColorAttachmentCount(), static_cast<u32>(colorInfos.size()))
                     : std::min<u32>(selection->DrawListCount, static_cast<u32>(colorInfos.size()));

        for (u32 i = 0; i < outputCount; ++i)
        {
            auto& info = colorInfos[i];
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;

            const u32 attachmentIndex = identity ? i : selection->DrawList[i];
            Ref<VulkanTexture2D> image =
                (attachmentIndex == RHI::NoAttachment || attachmentIndex >= target->GetColorAttachmentCount())
                    ? nullptr
                    : target->GetColorAttachmentImage(attachmentIndex);
            if (image == nullptr)
            {
                // VK_ATTACHMENT_UNUSED shape: a null imageView slot.
                info.imageView = VK_NULL_HANDLE;
                info.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                info.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
                ctx.ScopeTargets.ColorFormats[i] = VK_FORMAT_UNDEFINED;
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
            info.loadOp = foldColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            // The values captured at the Clear() request, not the live state.
            info.clearValue.color = { { ctx.Pending.ClearColor.r, ctx.Pending.ClearColor.g,
                                        ctx.Pending.ClearColor.b, ctx.Pending.ClearColor.a } };

            const auto* imageInfo = VulkanImageInfoRegistry::Get().Lookup(image->GetVkImage());
            ctx.ScopeTargets.ColorFormats[i] = imageInfo != nullptr ? imageInfo->Format : VK_FORMAT_UNDEFINED;

            RHI::Barrier toAttachment{};
            toAttachment.Resource = image->GetRHIHandle();
            toAttachment.Range.BaseMip = 0u;
            toAttachment.Range.MipCount = 1u;
            toAttachment.Range.BaseLayer = 0u;
            toAttachment.Range.LayerCount = 1u;
            const VkImageSubresourceRange probe{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
            toAttachment.Before = AccessGuessForLayout(ctx.Tracker.CurrentLayout(image->GetVkImage(), probe));
            toAttachment.After = RHI::Access::ColorAttachmentWrite;
            scopeBarriers.push_back(toAttachment);
        }
        ctx.ScopeTargets.ColorCount = outputCount;

        // §4 layered shadow depth: a selected texture-array LAYER wins over the
        // framebuffer's own depth attachment (the GL twin's
        // glNamedFramebufferTextureLayer re-point). Everything else about the
        // depth attachment — layout, loadOp folding, the pre-scope transition —
        // is identical; only the view, the format source and the barrier's
        // layer differ.
        const auto depthArray = GetFramebufferDepthArraySelection(target->GetRHIHandle());
        VkRenderingAttachmentInfo depthInfo{};
        Ref<VulkanTexture2D> depthImage = depthArray.Active ? nullptr : target->GetDepthAttachmentImage();
        const bool hasDepth = depthArray.Active || depthImage != nullptr;
        if (hasDepth)
        {
            const VkImageView depthView =
                depthArray.Active ? depthArray.View : depthImage->GetOrCreateAttachmentView();
            if (depthView == VK_NULL_HANDLE)
            {
                OLO_CORE_ERROR("[RHI/Vulkan] depth attachment view unavailable — draw dropped");
                return false;
            }
            depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthInfo.imageView = depthView;
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = foldDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthInfo.clearValue.depthStencil = { ctx.Pending.ClearDepth, 0u };

            const VkImage depthVkImage = depthArray.Active ? depthArray.Image : depthImage->GetVkImage();
            const auto* depthImageInfo = VulkanImageInfoRegistry::Get().Lookup(depthVkImage);
            ctx.ScopeTargets.DepthFormat = depthImageInfo != nullptr ? depthImageInfo->Format : VK_FORMAT_UNDEFINED;

            const u32 depthLayer = depthArray.Active ? depthArray.Layer : 0u;
            RHI::Barrier toDepth{};
            toDepth.Resource = depthArray.Active ? depthArray.Handle : depthImage->GetRHIHandle();
            toDepth.Range.BaseMip = 0u;
            toDepth.Range.MipCount = 1u;
            toDepth.Range.BaseLayer = depthLayer;
            toDepth.Range.LayerCount = 1u;
            // The tracker resolves runs by (mip, layer) only; the aspect mask
            // merely echoes into the emitted runs, so DEPTH is safe for the
            // query even on a combined depth-stencil format.
            const VkImageSubresourceRange depthProbe{ VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, depthLayer, 1u };
            toDepth.Before = AccessGuessForLayout(ctx.Tracker.CurrentLayout(depthVkImage, depthProbe));
            toDepth.After = RHI::Access::DepthStencilAttachmentWrite;
            scopeBarriers.push_back(toDepth);
        }

        // A rendering scope cannot contain vkCmdPipelineBarrier2 — record the
        // collected transitions first (no scope is active here; the batch
        // also keeps the layout tracker's state true, which is what gives the
        // NEXT barrier on these images its correct source scope).
        if (!scopeBarriers.empty())
            IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ scopeBarriers });

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = { { 0, 0 }, { width, height } };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = outputCount;
        rendering.pColorAttachments = outputCount > 0 ? colorInfos.data() : nullptr;
        rendering.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
        // Stencil shares the depth attachment when the format carries the
        // aspect; the recorded stencil state is dynamic.
        rendering.pStencilAttachment =
            (hasDepth && ctx.ScopeTargets.DepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT) ? &depthInfo : nullptr;
        vkCmdBeginRendering(ctx.Cmd, &rendering);

        ctx.Scope.Active = true;
        ctx.Scope.Target = target;
        ctx.Scope.TargetIsBackbuffer = false;
        ctx.Scope.BackbufferView = VK_NULL_HANDLE;
        ctx.Scope.DepthArrayView = depthArray.Active ? depthArray.View : VK_NULL_HANDLE;
        // The pending clear was consumed by the loadOps above (it matched
        // this target by construction — a mismatch was materialized before
        // the scope opened).
        ctx.Pending = PendingClear{};
        return true;
    }

    bool VulkanRendererAPI::PrepareDraw(const VulkanVertexArray* vao, const VkPrimitiveTopology topology)
    {
        auto& ctx = Ctx();
        if (!PrepareDrawCommon(vao, /*meshPipeline=*/false))
        {
            return false;
        }
        // FlushDynamicState's topology is the pilot's TRIANGLE_LIST; the
        // draw knows better — later set wins. Lives in this wrapper rather
        // than the common part because a mesh pipeline has no input assembly
        // and setting the topology against one is invalid (issue #813).
        vkCmdSetPrimitiveTopology(ctx.Cmd, topology);
        return true;
    }

    bool VulkanRendererAPI::PrepareDrawCommon(const VulkanVertexArray* vao, const bool meshPipeline)
    {
        auto& ctx = Ctx();
        // This is deliberately consumed by the next *attempted* draw, even
        // when the attempt later drops. Letting it leak into an unrelated draw
        // after a missing shader/pipeline failure would be worse than dropping
        // the producer's work (and would violate the one-draw hand-off).
        const VkDeviceAddress gpuWrittenRootData = ctx.NextDrawRootDataAddress;
        ctx.NextDrawRootDataAddress = 0;

        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("Draw(outside recording bracket)", StubKind::OutsideRecording);
            return false;
        }

        // Host-side conditional rendering (see BeginConditionalRender): the
        // predicate said "fully occluded last frame", so this draw is skipped
        // by request — counted apart from the failure drops.
        if (ctx.ConditionalRenderSkip)
        {
            ++ctx.ConditionallySkippedDraws;
            return false;
        }

        auto* shader = VulkanShader::GetCurrentlyBound();
        if (shader == nullptr || shader->GetCompilationStatus() != ShaderCompilationStatus::Ready)
        {
            static std::atomic<bool> s_Warned{ false };
            if (!s_Warned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] draw with no ready shader bound — dropped");
            }
            ++ctx.DroppedDraws;
            return false;
        }

        // The pipeline KIND is the bound shader's stage set (issue #813): a
        // task/mesh shader under a classic vkCmdDraw*, or a vertex shader
        // under vkCmdDrawMeshTasksEXT, is invalid usage either way
        // (VUID-vkCmdDraw-stage-06481 / VUID-vkCmdDrawMeshTasksEXT-stage-
        // 06480), so the entry point and the shader must agree — drop loudly
        // on a mismatch instead of recording an invalid draw.
        if (shader->HasMeshStage() != meshPipeline)
        {
            // Warn-once bool, not a per-shader set: a stage/entry-point
            // mismatch is a programming error fixed at the first occurrence,
            // and a name-keyed set grows across hot-reload churn for no
            // diagnostic gain (the established warn-once shape in this file).
            static std::atomic<bool> s_WarnedStageMismatch{ false };
            if (!s_WarnedStageMismatch.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_ERROR("[RHI/Vulkan] '{}' {} — draw dropped", shader->GetName(),
                               meshPipeline ? "has no mesh stage but was dispatched via DrawMeshTasks"
                                            : "is a task/mesh shader but was dispatched via a classic draw");
            }
            ++ctx.DroppedDraws;
            return false;
        }

        if (!EnsureRenderingScopeForDraw())
        {
            ++ctx.DroppedDraws;
            return false;
        }

        const auto& layout = shader->GetRootDataLayout();
        const VkPipeline pipeline =
            VulkanPipelineBuilder::Get().GetOrCreateGraphics(*shader, layout, ctx.State, ctx.ScopeTargets);
        if (pipeline == VK_NULL_HANDLE)
        {
            // This was the ONE fully silent drop in the chain — a PSO
            // creation failure must name its shader.
            static VulkanWarnOnceSet s_WarnedPipelines; // items may fail concurrently (#806)
            if (s_WarnedPipelines.Insert(shader->GetName()))
            {
                OLO_CORE_ERROR("[RHI/Vulkan] graphics pipeline creation failed for '{}' — draw dropped",
                               shader->GetName());
            }
            ++ctx.DroppedDraws;
            return false;
        }

        vkCmdBindPipeline(ctx.Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VulkanPipelineBuilder::FlushDynamicState(ctx.Cmd, ctx.State, ctx.ScopeTargets);

        // The pipelines declare VIEWPORT/SCISSOR_WITH_COUNT dynamic state —
        // the plain setters cannot satisfy it, so the draw front-end emits
        // both here: the recorded viewport (target extent when none was set)
        // and the recorded scissor box (full render area when scissor state
        // was merely disabled, which is the GL shape).
        // ScopeExtent covers both target kinds (a backbuffer scope has no
        // VulkanFramebuffer to ask for a specification).
        const VkExtent2D targetExtent = ScopeExtent();
        VkViewport viewport{};
        viewport.x = static_cast<f32>(ctx.RecordedViewport.x);
        viewport.y = static_cast<f32>(ctx.RecordedViewport.y);
        viewport.width = static_cast<f32>(ctx.RecordedViewport.width != 0 ? ctx.RecordedViewport.width : targetExtent.width);
        viewport.height = static_cast<f32>(ctx.RecordedViewport.height != 0 ? ctx.RecordedViewport.height : targetExtent.height);
        if (ctx.Scope.TargetIsBackbuffer)
        {
            // THE ACQUIRED IMAGE'S EXTENT IS THE AUTHORITY, not the pass's
            // recorded viewport. FinalRenderPass sizes its viewport from the
            // GRAPH's framebuffer spec, which the editor resizes to its
            // VIEWPORT PANEL — smaller than the window. On GL the default
            // framebuffer keeps whatever the rest of the window already held
            // (and ImGui paints over it anyway); a swapchain image is
            // UNDEFINED outside what this frame writes, so honouring a
            // smaller viewport presented the frame in a corner with garbage
            // bands to the right and below it. The present blit covers the
            // window, full stop.
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<f32>(targetExtent.width);
            viewport.height = static_cast<f32>(targetExtent.height);

            // NO Y FLIP — DELIBERATELY (#691, ADR 0011 amendment (85);
            // supersedes the negative-height mirror that lived here).
            // Every off-screen target is authored TOP-DOWN under Vulkan: the
            // (59) projection seam's clip-y negation lands a view's top row at
            // memory row 0, NDC-passthrough fullscreen hops preserve memory
            // order, and the live inventory measured every archetype
            // (geometry, post hop, compute imageStore, depth, shadow) agreeing.
            // The swapchain also displays row 0 at the top, so the present
            // blit must PRESERVE row order; the old mirror was written
            // against a chain believed to be GL-ordered, and nothing displayed
            // the graph chain through this path since (the editor composites
            // via ImGui; the runtime was Vulkan-gated until rollout) — the
            // (67) rule that orientation is a window-only proof, in action.
            // Pinned by the FinalPassBlits... tenant, which asserts the
            // presented rows EQUAL the chain rows.
        }
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewportWithCount(ctx.Cmd, 1, &viewport);

        // Same authority rule for the scissor: a pass-recorded box sized to
        // the graph's targets would re-introduce the bands the viewport
        // override above removes.
        VkRect2D scissor = (ctx.ScissorRectSet && ctx.State.ScissorTest && !ctx.Scope.TargetIsBackbuffer)
                               ? ctx.ScissorRect
                               : VkRect2D{ { 0, 0 }, targetExtent };
        vkCmdSetScissorWithCount(ctx.Cmd, 1, &scissor);

        if (!ctx.HeapBoundThisRecording)
        {
            // Binds BOTH heaps (the sampler heap cascades inside CmdBind).
            VulkanResourceHeap::Get().CmdBind(ctx.Cmd);
            ctx.HeapBoundThisRecording = true;
        }

        const bool assembled = gpuWrittenRootData != 0u
                                   ? PushRootDataAddress(gpuWrittenRootData)
                                   : AssembleAndPushRootData(layout, shader->GetName().c_str(), vao,
                                                             /*commandOrderedBufferReads=*/true);
        if (assembled)
        {
            ++ctx.PreparedDraws;
            if (gpuWrittenRootData != 0u)
                ++ctx.GpuWrittenRootDraws;
        }
        else
            ++ctx.DroppedDraws;
        return assembled;
    }

    namespace
    {
        // View type for the unfed-binding null-texture fallback (#691)
        // — the sampled-image twin of the frame arena's null buffer
        // block. The null image itself is owned by
        // VulkanDescriptorHeapBackend (reclaimed with the heap), because a
        // leaked VMA image outliving the allocator trips VMA's
        // "allocations not freed" assert at device teardown.
        [[nodiscard]] VkImageViewType ToNullViewType(const VulkanShaderBinding::TexDim dim)
        {
            switch (dim)
            {
                case VulkanShaderBinding::TexDim::Tex2DArray:
                    return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                case VulkanShaderBinding::TexDim::TexCube:
                    return VK_IMAGE_VIEW_TYPE_CUBE;
                case VulkanShaderBinding::TexDim::TexCubeArray:
                    return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                case VulkanShaderBinding::TexDim::Tex3D:
                    return VK_IMAGE_VIEW_TYPE_3D;
                case VulkanShaderBinding::TexDim::Tex2D:
                default:
                    return VK_IMAGE_VIEW_TYPE_2D;
            }
        }
    } // namespace

    void VulkanRendererAPI::AssembleRootData(const VulkanRootDataLayout& layout, const char* shaderName,
                                             const VulkanVertexArray* vao, const bool commandOrderedBufferReads)
    {
        auto& ctx = Ctx();
        // Root-struct assembly: one u64 device address per buffer block, one
        // u32 heap slot per image, from the process-global binding state
        // (ADR 0011 §4 — this is the writer half of the mapping contract;
        // the pipeline emitted the reader half from the SAME layout).
        auto& bindingState = VulkanBindingState::Get();
        ctx.RootScratch.assign(layout.SizeBytes, 0u);
        for (const auto& field : layout.Fields)
        {
            const auto& binding = field.Binding;
            const bool isBuffer = binding.BindingKind == VulkanShaderBinding::Kind::UniformBuffer ||
                                  binding.BindingKind == VulkanShaderBinding::Kind::StorageBuffer;
            if (isBuffer)
            {
                // §5 vertex pulling: the reserved pull PAIR reads the draw's
                // VAO streams (A2/A3, see ShaderBindingLayout's SSBO section):
                // SSBO 57 "OloVertexPull" = stream 0, SSBO 63 "OloBonePull" =
                // stream 1 (the bone-influence VB). KIND-guarded to the SSBO
                // namespace: UBO_DEBUG_DRAW is also 57, and a UBO at these
                // numbers must keep resolving through the published binding
                // state (GL's disjoint namespaces, amendment (29)).
                const bool isStorage = binding.BindingKind == VulkanShaderBinding::Kind::StorageBuffer;
                sizet pullStream = ~sizet{ 0 };
                if (isStorage && binding.Binding == ShaderBindingLayout::SSBO_VERTEX_PULL)
                    pullStream = 0;
                else if (isStorage && binding.Binding == ShaderBindingLayout::SSBO_BONE_PULL)
                    pullStream = 1;

                u64 address = 0;
                if (pullStream != ~sizet{ 0 })
                {
                    // A VAO with fewer streams than the shader pulls (a
                    // skinned shader on a static mesh) resolves to the zero
                    // address: deterministic zeros + the warn-once below.
                    const auto* pullBuffer = vao != nullptr ? vao->GetPullVertexBuffer(pullStream) : nullptr;
                    address = pullBuffer != nullptr ? pullBuffer->GetDeviceAddress() : 0;
                }
                else if (auto* ubo = bindingState.GetUniformBuffer(binding.Binding);
                         ubo != nullptr && binding.BindingKind == VulkanShaderBinding::Kind::UniformBuffer)
                {
                    address = ubo->GetRootDataAddress();
                }
                else if (auto* ssbo = bindingState.GetStorageBuffer(binding.Binding);
                         ssbo != nullptr && isStorage)
                {
                    // KIND-guarded for the same reason as the UBO arm above:
                    // the two bind-point namespaces overlap by design, so an
                    // unfed UBO block must resolve to the zero address (warned
                    // below) rather than to whatever SSBO shares its number.
                    //
                    // Draws take the command-ordered snapshot seam (a mid-
                    // frame SetData hands later draws a NEW arena range —
                    // GL's glNamedBufferSubData ordering; the batched-
                    // instance archetype, #691). Compute keeps the
                    // persistent address: its SSBOs are GPU-write
                    // participants (cull survivors, indirect seeds) whose
                    // writes must land where the indirect/copy consumers
                    // resolve, and the culler's slot pool already gives each
                    // dispatch fresh buffers.
                    address = commandOrderedBufferReads ? ssbo->GetRootDataAddress() : ssbo->GetDeviceAddress();
                }
                else if (isStorage)
                {
                    // A raw buffer staged by BindStorageBuffer (issue #1052).
                    // No snapshot seam applies: a raw buffer has no per-frame
                    // arena copy, so draws and dispatches both read the one
                    // persistent allocation — the pre-snapshot GL behaviour,
                    // and what its GPU-writing tenants (indirect args, the
                    // virtual-geometry index arena) require anyway.
                    address = bindingState.GetStorageBufferAddress(binding.Binding);
                }
                if (address == 0)
                {
                    // Warn once per shader+binding, then substitute the
                    // arena's persistent zero-filled block. Address 0 is NOT
                    // safe to hand to the shader: dereferencing the null
                    // device address is a GPU page fault that escalates to
                    // VK_ERROR_DEVICE_LOST (#691 — the IBL-bake UBO
                    // scope bug lost the whole device this way). The null
                    // block makes an unfed binding read deterministic zeros,
                    // matching what this comment always promised.
                    static VulkanWarnOnceSet s_WarnedBindings; // items may fail concurrently (#806)
                    if (s_WarnedBindings.Insert(std::string(shaderName) + ":" + std::to_string(binding.Binding)))
                    {
                        // An unfed UBO reads defined zeros — wrong, survivable.
                        // An unfed SSBO is a small block a shader INDEXES, and
                        // the index usually comes from a DIFFERENT buffer that
                        // was fed correctly, so the read lands outside it and
                        // loses the device. That asymmetry is the whole of
                        // #1052, so the two cases do not get the same voice.
                        if (isStorage)
                        {
                            OLO_CORE_ERROR("[RHI/Vulkan] '{}' STORAGE binding {} has no published occupant — "
                                           "substituting the null block, which a shader that indexes this buffer "
                                           "will read past (issue #1052). Bind it, or stop declaring it.",
                                           shaderName, binding.Binding);
                        }
                        else
                        {
                            OLO_CORE_WARN("[RHI/Vulkan] '{}' buffer binding {} has no published occupant — null block",
                                          shaderName, binding.Binding);
                        }
                    }
                    // Counted separately from the stub tally so existing
                    // "must not fall through to a stub" assertions keep meaning
                    // exactly what they meant, and a tenant can assert on THIS
                    // deliberately.
                    if (isStorage)
                    {
                        m_UnfedStorageBindings.fetch_add(1, std::memory_order_relaxed);
                    }
                    address = VulkanFrameArena::Get().GetNullBlockAddress();
                }
                std::memcpy(ctx.RootScratch.data() + field.Offset, &address, sizeof(address));
            }
            else
            {
                // amendment (29): image units and texture slots are DISJOINT
                // namespaces — source by the binding's KIND.
                const bool storageImage = binding.BindingKind == VulkanShaderBinding::Kind::StorageImage;
                u32 slot = storageImage ? bindingState.GetImageHeapSlot(binding.Binding)
                                        : bindingState.GetTextureHeapSlot(binding.Binding);
                if (slot == VulkanBindingState::kNoHeapSlot)
                {
                    // Sampled bindings resolve to the zero-filled null
                    // texture of the declaration's dimensionality — GL's
                    // "unbound sampler reads black" (#691; slot 0
                    // leaked the first-registered texture into every unfed
                    // sampler). Storage images keep the slot-0 fallback: a
                    // write target has no safe neutral image, and no
                    // production shader dispatches with an unfed image unit.
                    u32 nullSlot = VulkanResourceHeap::InvalidSlot;
                    // Multisampled declarations (sampler2DMS) have no null MS
                    // image to fall back to, and a single-sample null under an
                    // MS sampler is the wrong-view-type bug the typed fallback
                    // exists to prevent — leave the slot invalid so the
                    // warn-once below names the shader and binding instead.
                    const bool multisampled = binding.ImageDim == VulkanShaderBinding::TexDim::Tex2DMS ||
                                              binding.ImageDim == VulkanShaderBinding::TexDim::Tex2DMSArray;
                    if (!storageImage && !multisampled)
                    {
                        nullSlot =
                            VulkanDescriptorHeapBackend::Get().GetNullSampledHeapSlot(ToNullViewType(binding.ImageDim));
                    }
                    static VulkanWarnOnceSet s_WarnedSlots; // items may fail concurrently (#806)
                    if (s_WarnedSlots.Insert(std::string(shaderName) + ":" + std::to_string(binding.Binding)))
                    {
                        // The sampled fallback is SILENT: reading black from an
                        // unfed optional sampler is the GL contract (GL warns
                        // about nothing here), and every editor scene has a few
                        // by design (u_Use*Map-gated material maps, optional
                        // water inputs) — logging it is pure noise. The
                        // storage-image arm stays a WARN — slot 0 as a WRITE
                        // target is never by design.
                        if (nullSlot == VulkanResourceHeap::InvalidSlot)
                        {
                            OLO_CORE_WARN("[RHI/Vulkan] '{}' {} binding {} has no staged heap slot — slot 0",
                                          shaderName, storageImage ? "image" : "texture", binding.Binding);
                        }
                    }
                    slot = nullSlot != VulkanResourceHeap::InvalidSlot ? nullSlot : 0u;
                }
                std::memcpy(ctx.RootScratch.data() + field.Offset, &slot, sizeof(slot));
                if (!storageImage)
                {
                    // The sampler half (#691): the SAMPLER-heap slot
                    // BindTexture staged beside the texture slot. Zero (the
                    // default linear/clamp sampler) when nothing was staged.
                    const u32 samplerSlot = bindingState.GetTextureSamplerSlot(binding.Binding);
                    std::memcpy(ctx.RootScratch.data() + field.Offset + VulkanRootDataLayout::kSamplerIndexOffset,
                                &samplerSlot, sizeof(samplerSlot));
                }
            }
        }
    }

    bool VulkanRendererAPI::AssembleAndPushRootData(const VulkanRootDataLayout& layout, const char* shaderName,
                                                    const VulkanVertexArray* vao, const bool commandOrderedBufferReads)
    {
        auto& ctx = Ctx();
        AssembleRootData(layout, shaderName, vao, commandOrderedBufferReads);
        const auto rootAllocation = VulkanFrameArena::Get().Push(ctx.RootScratch.data(), ctx.RootScratch.size(), 16);
        if (!rootAllocation.IsValid())
        {
            return false; // arena overflow — dropped work, counted by the arena
        }

        return PushRootDataAddress(rootAllocation.Gpu);
    }

    bool VulkanRendererAPI::PushRootDataAddress(const VkDeviceAddress rootAddress)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE || rootAddress == 0u)
            return false;

        VkPushDataInfoEXT pushInfo{};
        pushInfo.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT;
        pushInfo.offset = 0;
        pushInfo.data = { .address = &rootAddress, .size = sizeof(rootAddress) };
        vkCmdPushDataEXT(ctx.Cmd, &pushInfo);
        return true;
    }

    VulkanRendererAPI::ResolvedIndexBuffer VulkanRendererAPI::ResolveIndexBufferFor(const VulkanVertexArray* vao)
    {
        if (vao == nullptr)
        {
            return {};
        }
        // The object-backed occupant wins, the way BindStorageBuffer's does:
        // a VAO built through VertexArray::SetIndexBuffer owns a Ref, and that
        // is the stronger claim on "this VAO's element buffer".
        if (const auto* indexBuffer = vao->GetVulkanIndexBuffer();
            indexBuffer != nullptr && indexBuffer->GetVkBuffer() != VK_NULL_HANDLE)
        {
            // The engine's index format is fixed 32-bit (IndexBuffer.h), so the
            // byte extent is the element count times four.
            return { indexBuffer->GetVkBuffer(), static_cast<VkDeviceSize>(indexBuffer->GetCount()) * sizeof(u32),
                     indexBuffer->GetCount() };
        }
        // The raw element buffer (SetVertexArrayIndexBuffer, #1052). Resolved
        // here rather than cached on the VAO so a re-allocate under the same
        // identity — VirtualMeshRegistry re-sizes its index arena when the page
        // budget changes — is picked up without a re-set.
        if (const auto* raw = VulkanRawBufferRegistry::Get().Lookup(vao->GetRawIndexBuffer());
            raw != nullptr && raw->Buffer != VK_NULL_HANDLE)
        {
            return { raw->Buffer, static_cast<VkDeviceSize>(raw->Size), static_cast<u32>(raw->Size / sizeof(u32)) };
        }
        return {};
    }

    bool VulkanRendererAPI::BindIndexBufferFor(const VulkanVertexArray* vao)
    {
        auto& ctx = Ctx();
        const ResolvedIndexBuffer indexBuffer = ResolveIndexBufferFor(vao);
        if (indexBuffer.Buffer == VK_NULL_HANDLE)
        {
            static std::atomic<bool> s_Warned{ false };
            if (!s_Warned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] indexed draw without an index buffer — dropped");
            }
            return false;
        }
        if (indexBuffer.Buffer != ctx.BoundIndexBuffer || indexBuffer.SizeBytes != ctx.BoundIndexBufferSize)
        {
            // #809 (maintenance5, core in 1.4): bind the buffer's REAL byte
            // size instead of the implicit whole-buffer bind. The facade's
            // DrawIndexed(va, 0) "whole buffer" sentinel (amendment (9b)) is
            // resolved to indexBuffer->GetCount() at the draw sites; stating
            // the same extent on the BIND turns a draw whose count overruns
            // the buffer into a validation error naming the bind, instead of
            // an out-of-bounds index fetch that renders garbage. The engine's
            // index format is fixed 32-bit (IndexBuffer.h), so the byte size
            // is the count times four. ResolveIndexBufferFor computes it for
            // both occupant kinds.
            //
            // The cache key is the buffer AND the extent: VulkanIndexBuffer's
            // count is fixed at construction, but a RAW arena is re-allocated
            // at a new size under the same identity when the page budget
            // changes, and VMA is free to return the same VkBuffer for the
            // replacement — so the handle alone cannot distinguish the two.
            const auto* device = VulkanDevice::Get();
            if (device != nullptr && device->IsMaintenance5Enabled())
            {
                vkCmdBindIndexBuffer2(ctx.Cmd, indexBuffer.Buffer, 0, indexBuffer.SizeBytes, VK_INDEX_TYPE_UINT32);
            }
            else
            {
                vkCmdBindIndexBuffer(ctx.Cmd, indexBuffer.Buffer, 0, VK_INDEX_TYPE_UINT32);
            }
            ctx.BoundIndexBuffer = indexBuffer.Buffer;
            ctx.BoundIndexBufferSize = indexBuffer.SizeBytes;
        }
        return true;
    }

    // --- Unimplemented stubs (generated; every entry warns once and counts) ---

    void VulkanRendererAPI::Clear()
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("Clear(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        // GL clears the BOUND framebuffer — but the rendering scope switches
        // lazily at the next draw, so after a mid-pass Bind() the live scope
        // still holds the PREVIOUS target. Clearing "the live scope" then
        // wipes the output the pass just drew (the bloom mip ladder, JFA
        // ping-pong, fog/cloud half-res chains all Bind(); Clear(); draw;
        // — found by #691: every intra-pass consumer sampled
        // its producer's CLEAR). A scope on a stale target ends here and the
        // clear folds into the NEW target's first-draw loadOp.
        if (ctx.Scope.Active && !ScopeMatchesCurrentTarget())
        {
            EndRenderingScope();
        }
        if (!ctx.Scope.Active)
        {
            // Folds into the next scope begin's loadOp (the GL clear-then-
            // draw shape) — recorded against the BOUND target with the LIVE
            // clear values (#691, see PendingClear).
            RecordPendingClear(true, true);
            return;
        }
        // Mid-scope clear: vkCmdClearAttachments against the live scope.
        std::array<VkClearAttachment, 9> clears{};
        u32 clearCount = 0;
        for (u32 i = 0; i < ctx.ScopeTargets.ColorCount; ++i)
        {
            if (ctx.ScopeTargets.ColorFormats[i] == VK_FORMAT_UNDEFINED)
            {
                continue;
            }
            auto& clear = clears[clearCount++];
            clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clear.colorAttachment = i;
            clear.clearValue.color = { { ctx.State.ClearColor.r, ctx.State.ClearColor.g, ctx.State.ClearColor.b,
                                         ctx.State.ClearColor.a } };
        }
        if (ctx.ScopeTargets.DepthFormat != VK_FORMAT_UNDEFINED)
        {
            auto& clear = clears[clearCount++];
            clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            clear.clearValue.depthStencil = { ctx.State.ClearDepth, 0u };
        }
        if (clearCount == 0)
        {
            return;
        }
        VkClearRect rect{};
        rect.rect = { { 0, 0 }, ScopeExtent() };
        rect.layerCount = 1;
        vkCmdClearAttachments(ctx.Cmd, clearCount, clears.data(), 1, &rect);
    }

    void VulkanRendererAPI::ClearDepthOnly()
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("ClearDepthOnly(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        // Same stale-scope guard as Clear(): GL clears the BOUND framebuffer,
        // never the lazily-switched previous scope target.
        if (ctx.Scope.Active && !ScopeMatchesCurrentTarget())
        {
            EndRenderingScope();
        }
        if (!ctx.Scope.Active)
        {
            RecordPendingClear(false, true);
            return;
        }
        if (ctx.ScopeTargets.DepthFormat == VK_FORMAT_UNDEFINED)
        {
            return;
        }
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil = { ctx.State.ClearDepth, 0u };
        VkClearRect rect{};
        rect.rect = { { 0, 0 }, ScopeExtent() };
        rect.layerCount = 1;
        vkCmdClearAttachments(ctx.Cmd, 1, &clear, 1, &rect);
    }

    void VulkanRendererAPI::ClearColorAndDepth()
    {
        Clear();
    }

    void VulkanRendererAPI::DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount)
    {
        auto& ctx = Ctx();
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST))
        {
            vkCmdDraw(ctx.Cmd, vertexCount, 1, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount)
    {
        auto& ctx = Ctx();
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            // GL-facade contract: indexCount 0 means "the whole index buffer"
            // (the GL twin derives identically). Passed through raw it is a
            // LEGAL zero-index draw that renders nothing — every
            // context.DrawIndexed(va) pass-body call uses the no-count form,
            // so the whole pass suite silently drew nothing (found by
            // VulkanPassSuiteTest's first full-graph frame).
            const u32 count = indexCount != 0 ? indexCount : ResolveIndexBufferFor(vao).Count;
            vkCmdDrawIndexed(ctx.Cmd, count, 1, 0, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 instanceCount)
    {
        auto& ctx = Ctx();
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            // Same 0 = whole-index-buffer facade contract as DrawIndexed.
            const u32 count = indexCount != 0 ? indexCount : ResolveIndexBufferFor(vao).Count;
            vkCmdDrawIndexed(ctx.Cmd, count, instanceCount, 0, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, u32 vertexCount)
    {
        auto& ctx = Ctx();
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_LINE_LIST))
        {
            vkCmdDraw(ctx.Cmd, vertexCount, 1, 0, 0);
        }
    }

    // A10 (#691): tessellated draws. GL's glPatchParameteri +
    // GL_PATCHES becomes a PATCH_LIST topology plus a baked
    // VkPipelineTessellationStateCreateInfo::patchControlPoints — the patch
    // size is a PSO axis on the ADR 0010 floor (extendedDynamicState2-
    // PatchControlPoints is not required), so the count is recorded through
    // the same SetPatchVertexCount state the GL twin sets and the PSO key
    // picks it up. Callers that pass an explicit count (the packet-decoded
    // Raw form) record it here first, exactly as glPatchParameteri would.
    void VulkanRendererAPI::DrawIndexedPatches(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 patchVertices)
    {
        auto& ctx = Ctx();
        if (patchVertices != 0)
            ctx.State.PatchVertexCount = patchVertices;
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) && BindIndexBufferFor(vao))
        {
            // Same 0 = whole-index-buffer facade contract as DrawIndexed.
            const u32 count = indexCount != 0 ? indexCount : ResolveIndexBufferFor(vao).Count;
            vkCmdDrawIndexed(ctx.Cmd, count, 1, 0, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount)
    {
        DrawIndexedRaw(vertexArray, indexCount, 0u);
    }

    void VulkanRendererAPI::DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex)
    {
        auto& ctx = Ctx();
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(vertexArray);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::VertexArray)
        {
            UnimplementedStub("DrawIndexedRaw(unresolvable vertex array)", StubKind::PreconditionFailure);
            return;
        }
        const auto* vao = static_cast<const VulkanVertexArray*>(entry->Object);
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            vkCmdDrawIndexed(ctx.Cmd, indexCount, 1, baseIndex, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawIndexedInstancedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex, u32 instanceCount)
    {
        auto& ctx = Ctx();
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(vertexArray);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::VertexArray)
        {
            UnimplementedStub("DrawIndexedInstancedRaw(unresolvable vertex array)", StubKind::PreconditionFailure);
            return;
        }
        const auto* vao = static_cast<const VulkanVertexArray*>(entry->Object);
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            vkCmdDrawIndexed(ctx.Cmd, indexCount, instanceCount, baseIndex, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawIndexedPatchesRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 patchVertices)
    {
        auto& ctx = Ctx();
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(vertexArray);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::VertexArray)
        {
            UnimplementedStub("DrawIndexedPatchesRaw(unresolvable vertex array)", StubKind::PreconditionFailure);
            return;
        }
        if (patchVertices != 0)
            ctx.State.PatchVertexCount = patchVertices;
        const auto* vao = static_cast<const VulkanVertexArray*>(entry->Object);
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) && BindIndexBufferFor(vao))
        {
            vkCmdDrawIndexed(ctx.Cmd, indexCount, 1, 0, 0, 0);
        }
    }

    void VulkanRendererAPI::ClearStencil()
    {
        UnimplementedStub("ClearStencil");
    }

    // --- Indirect draws (#691) ------------------------------
    //
    // GL's Draw*Indirect commands read the same POD layouts Vulkan defines
    // (DrawArraysIndirectCommand == VkDrawIndirectCommand,
    // DrawElementsIndirectCommand == VkDrawIndexedIndirectCommand, field for
    // field), so the buffers pass through untranslated. The producers are
    // shader atomics or CPU SetData; the facade's MemoryBarrier(Command)
    // lowers to the conservative global barrier, whose ALL_COMMANDS /
    // MEMORY_READ scopes cover DRAW_INDIRECT + INDIRECT_COMMAND_READ.

    VkBuffer VulkanRendererAPI::ResolveIndirectBuffer(const RHI::ResourceHandle indirectBuffer, const char* entryPoint) const
    {
        // Every Vulkan-backend buffer registers its VkBuffer as the identity's
        // native (VulkanStorageBuffer / VulkanVertexBuffer / the raw-buffer
        // registry all Sync it), so generic resolution covers them all.
        if (RHI::ResourceRegistry::Get().KindOf(indirectBuffer) != RHI::ResourceKind::Buffer)
        {
            UnimplementedStub(entryPoint);
            return VK_NULL_HANDLE;
        }
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(indirectBuffer);
        if (native == 0u)
        {
            UnimplementedStub(entryPoint);
            return VK_NULL_HANDLE;
        }
        return reinterpret_cast<VkBuffer>(native);
    }

    void VulkanRendererAPI::DrawElementsIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer)
    {
        auto& ctx = Ctx();
        const VkBuffer indirect = ResolveIndirectBuffer(indirectBuffer, "DrawElementsIndirect(unresolvable indirect buffer)");
        if (indirect == VK_NULL_HANDLE)
            return;
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            vkCmdDrawIndexedIndirect(ctx.Cmd, indirect, 0, 1, 0);
        }
    }

    void VulkanRendererAPI::DrawArraysIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer)
    {
        auto& ctx = Ctx();
        const VkBuffer indirect = ResolveIndirectBuffer(indirectBuffer, "DrawArraysIndirect(unresolvable indirect buffer)");
        if (indirect == VK_NULL_HANDLE)
            return;
        const auto* vao = static_cast<const VulkanVertexArray*>(vertexArray.Raw());
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST))
        {
            vkCmdDrawIndirect(ctx.Cmd, indirect, 0, 1, 0);
        }
    }

    void VulkanRendererAPI::DrawBoundElementsIndirect(RHI::ResourceHandle indirectBuffer,
                                                      RHI::PrimitiveTopology topology)
    {
        auto& ctx = Ctx();
        const VkBuffer indirect = ResolveIndirectBuffer(indirectBuffer, "DrawBoundElementsIndirect(unresolvable indirect buffer)");
        if (indirect == VK_NULL_HANDLE)
            return;
        if (PrepareDraw(ctx.BoundVertexArray, ToVkTopology(topology)) && BindIndexBufferFor(ctx.BoundVertexArray))
        {
            vkCmdDrawIndexedIndirect(ctx.Cmd, indirect, 0, 1, 0);
        }
    }

    void VulkanRendererAPI::MultiDrawElementsIndirectCountRaw(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indirectBuffer, u32 indirectOffsetBytes, RHI::ResourceHandle parameterBuffer, u32 parameterOffsetBytes, u32 maxDrawCount, u32 strideBytes)
    {
        auto& ctx = Ctx();
        if (maxDrawCount == 0)
            return; // GL twin's early-out: nothing to draw
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(vertexArray);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::VertexArray)
        {
            UnimplementedStub("MultiDrawElementsIndirectCountRaw(unresolvable vertex array)", StubKind::PreconditionFailure);
            return;
        }
        const VkBuffer indirect = ResolveIndirectBuffer(indirectBuffer, "MultiDrawElementsIndirectCountRaw(unresolvable indirect buffer)");
        const VkBuffer parameter = ResolveIndirectBuffer(parameterBuffer, "MultiDrawElementsIndirectCountRaw(unresolvable parameter buffer)");
        if (indirect == VK_NULL_HANDLE || parameter == VK_NULL_HANDLE)
            return;

        // vkCmdDrawIndexedIndirectCount is core 1.2 but feature-gated
        // (drawIndirectCount), and maxDrawCount > 1 additionally needs
        // multiDrawIndirect — both enabled-when-supported at device init.
        // Universal on the ADR 0010 desktop floor; a device without them
        // drops the draw LOUDLY rather than faking it with a CPU loop over a
        // count the CPU cannot read.
        const auto* device = VulkanDevice::Get();
        if (device == nullptr || !device->IsDrawIndirectCountEnabled() ||
            (maxDrawCount > 1 && !device->IsMultiDrawIndirectEnabled()))
        {
            static std::atomic<bool> s_Warned{ false };
            if (!s_Warned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_ERROR("[RHI/Vulkan] MultiDrawElementsIndirectCountRaw needs drawIndirectCount"
                               "/multiDrawIndirect, which this device did not enable — draw dropped");
            }
            ++ctx.DroppedDraws;
            return;
        }

        const auto* vao = static_cast<const VulkanVertexArray*>(entry->Object);
        if (PrepareDraw(vao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) && BindIndexBufferFor(vao))
        {
            vkCmdDrawIndexedIndirectCount(ctx.Cmd, indirect, indirectOffsetBytes, parameter, parameterOffsetBytes,
                                          maxDrawCount, strideBytes);
        }
    }

    void VulkanRendererAPI::DrawMeshTasks(u32 groupsX, u32 groupsY, u32 groupsZ)
    {
        auto& ctx = Ctx();
        // Facade contract: a zero group count in any dimension is a legal
        // no-op — before every guard, so an empty launch is neither a drop
        // nor a warn (the DispatchComputeIndirect "no survivors cost
        // nothing" shape).
        if (groupsX == 0 || groupsY == 0 || groupsZ == 0)
            return;

        // vkCmdDrawMeshTasksEXT is a VK_EXT_mesh_shader entry point — volk
        // leaves the pointer null when the device did not enable the
        // extension, so this gate is a null-call guard as much as a
        // capability check. SupportsMeshShaders() exists so callers route
        // away BEFORE reaching here; a call that lands anyway drops loudly
        // (the MultiDrawElementsIndirectCountRaw shape).
        if (const auto* device = VulkanDevice::Get(); device == nullptr || !device->IsMeshShaderEnabled())
        {
            static std::atomic<bool> s_Warned{ false };
            if (!s_Warned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_ERROR("[RHI/Vulkan] DrawMeshTasks needs VK_EXT_mesh_shader (task+mesh), which this "
                               "device did not enable — the capability gate should have routed this away; "
                               "draw dropped");
            }
            ++ctx.DroppedDraws;
            return;
        }

        // The common front-end minus the two input-assembly dynamic states
        // (topology, primitive restart) — both invalid against a mesh
        // pipeline. vao == nullptr: a mesh pipeline pulls nothing through
        // the vertex-pull pair, so bindings 57/63 resolve to the arena null
        // block, exactly as compute dispatches do.
        if (PrepareDrawCommon(nullptr, /*meshPipeline=*/true))
        {
            vkCmdDrawMeshTasksEXT(ctx.Cmd, groupsX, groupsY, groupsZ);
        }
    }

    void VulkanRendererAPI::DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("DispatchCompute(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        auto* shader = VulkanComputeShader::GetCurrentlyBound();
        if (shader == nullptr || !shader->IsValid())
        {
            static std::atomic<bool> s_Warned{ false };
            if (!s_Warned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] dispatch with no valid compute shader bound - dropped");
            }
            return;
        }

        // Dispatches are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        const auto& layout = shader->GetRootDataLayout();
        const VkPipeline pipeline =
            VulkanPipelineBuilder::Get().GetOrCreateCompute(shader->GetPipelineIndexKey(), shader->GetModule(), layout);
        if (pipeline == VK_NULL_HANDLE)
        {
            return;
        }

        vkCmdBindPipeline(ctx.Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        if (!ctx.HeapBoundThisRecording)
        {
            // Binds BOTH heaps (the sampler heap cascades inside CmdBind).
            VulkanResourceHeap::Get().CmdBind(ctx.Cmd);
            ctx.HeapBoundThisRecording = true;
        }
        if (!AssembleAndPushRootData(layout, shader->GetName().c_str(), nullptr,
                                     /*commandOrderedBufferReads=*/false))
        {
            return;
        }
        vkCmdDispatch(ctx.Cmd, std::max(groupsX, 1u), std::max(groupsY, 1u), std::max(groupsZ, 1u));
    }

    void VulkanRendererAPI::DispatchComputeIndirect(RHI::ResourceHandle argsBuffer, u32 offsetBytes)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("DispatchComputeIndirect(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        auto* shader = VulkanComputeShader::GetCurrentlyBound();
        if (shader == nullptr || !shader->IsValid())
        {
            static std::atomic<bool> s_Warned{ false };
            if (!s_Warned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] indirect dispatch with no valid compute shader bound - dropped");
            }
            return;
        }

        // Same buffer-resolution path the indirect DRAWS use. It resolves the
        // handle and validates its KIND — it does NOT verify the usage bit.
        // VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT comes from
        // VulkanStorageBuffer::CreateBuffer setting it on every storage buffer,
        // which is what makes an SSBO legal as a dispatch-argument source.
        const VkBuffer args = ResolveIndirectBuffer(argsBuffer, "DispatchComputeIndirect(unresolvable args buffer)");
        if (args == VK_NULL_HANDLE)
            return;

        // Dispatches are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        const auto& layout = shader->GetRootDataLayout();
        const VkPipeline pipeline =
            VulkanPipelineBuilder::Get().GetOrCreateCompute(shader->GetPipelineIndexKey(), shader->GetModule(), layout);
        if (pipeline == VK_NULL_HANDLE)
        {
            return;
        }

        vkCmdBindPipeline(ctx.Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        if (!ctx.HeapBoundThisRecording)
        {
            VulkanResourceHeap::Get().CmdBind(ctx.Cmd);
            ctx.HeapBoundThisRecording = true;
        }
        if (!AssembleAndPushRootData(layout, shader->GetName().c_str(), nullptr,
                                     /*commandOrderedBufferReads=*/false))
        {
            return;
        }
        vkCmdDispatchIndirect(ctx.Cmd, args, static_cast<VkDeviceSize>(offsetBytes));
    }

    void VulkanRendererAPI::SetFrameBackbuffer(const RHI::ResourceHandle handle, const VkImageView view,
                                               const u32 width, const u32 height)
    {
        m_Backbuffer = FrameBackbuffer{};
        m_BackbufferWritten = false;
        if (!handle.IsValid() || view == VK_NULL_HANDLE)
        {
            return;
        }
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(handle);
        if (native == 0u)
        {
            OLO_CORE_WARN("[RHI/Vulkan] backbuffer handle does not resolve on this backend — the frame will "
                          "fall back to the clear path");
            return;
        }
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr)
        {
            OLO_CORE_WARN("[RHI/Vulkan] backbuffer image is not registered (format/aspect unknown) — the frame "
                          "will fall back to the clear path");
            return;
        }
        m_Backbuffer.Handle = handle;
        m_Backbuffer.Image = image;
        m_Backbuffer.View = view;
        m_Backbuffer.Format = info->Format;
        m_Backbuffer.Width = width;
        m_Backbuffer.Height = height;
    }

    void VulkanRendererAPI::ClearFrameBackbuffer()
    {
        auto& ctx = Ctx();
        // A scope still open against the retiring publication must not survive
        // into the next frame's (different) image.
        if (ctx.Scope.Active && ctx.Scope.TargetIsBackbuffer)
        {
            EndRenderingScope();
        }
        m_Backbuffer = FrameBackbuffer{};
        m_BackbufferWritten = false;
    }

    bool VulkanRendererAPI::FinalizeBackbufferForPresent(const bool frameRendered)
    {
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE || !m_Backbuffer.IsValid())
        {
            return false;
        }

        // The clear-only-pass gap: with a pending clear and no draw behind it
        // the lazy scope never opened, so nothing has been written. Open the
        // scope (which consumes the pending clear as loadOp CLEAR) and close
        // it immediately — an empty CLEAR/STORE rendering instance is exactly
        // GL's eager glClear. Only a pending clear that TARGETS the
        // backbuffer qualifies (#691): the target-blind flag
        // let a leftover FBO-pass clear land on the screen here. A pending
        // clear for another target is materialized by EndRecording.
        if (frameRendered && !m_BackbufferWritten && ctx.Pending.Color && ctx.Pending.TargetIsBackbuffer &&
            ctx.Pending.BackbufferView == m_Backbuffer.View)
        {
            VulkanBindingState::Get().SetCurrentFramebuffer(nullptr);
            if (EnsureRenderingScopeForDraw())
            {
                EndRenderingScope();
            }
        }
        EndRenderingScope();

        if (!m_BackbufferWritten)
        {
            return false;
        }

        RHI::Barrier toPresent{};
        toPresent.Resource = m_Backbuffer.Handle;
        toPresent.Range.BaseMip = 0u;
        toPresent.Range.MipCount = 1u;
        toPresent.Range.BaseLayer = 0u;
        toPresent.Range.LayerCount = 1u;
        toPresent.Before = RHI::Access::ColorAttachmentWrite;
        toPresent.After = RHI::Access::Present;
        IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ &toPresent, 1 });
        return true;
    }

    void VulkanRendererAPI::BindDefaultFramebuffer()
    {
        // GL semantics: unbind the named framebuffer, i.e. select framebuffer
        // 0. On this backend "framebuffer 0" is the swapchain image the frame
        // recorder published for THIS recording (SetFrameBackbuffer) — so
        // this is a target CHANGE, not a target loss, and the lazy scope
        // re-opens against the backbuffer at the next draw. With nothing
        // published (headless recordings) it stays the old no-target state.
        VulkanBindingState::Get().SetCurrentFramebuffer(nullptr);
        if (!ScopeMatchesCurrentTarget())
        {
            EndRenderingScope();
        }
    }

    void VulkanRendererAPI::BlitFramebufferToDefault(RHI::ResourceHandle /*srcFramebuffer*/, u32 /*width*/, u32 /*height*/)
    {
        UnimplementedStub("BlitFramebufferToDefault");
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

        // GL-parity mid-pass visibility (#691): GL makes a
        // just-rendered (or just-copied) image visible to a later texture()
        // with NO application barrier, so pass bodies sample an attachment
        // they drew two draws ago without saying anything — the bloom mip
        // ladder, the JFA ping-pong, fog's and cloudscape's half-res
        // write-then-sample all do exactly this inside ONE Execute, where the
        // graph's per-pass planner barriers cannot reach. Without this seam
        // the sample raced the raster and read the PRE-draw content (the
        // clear), while the baked SHADER_READ_ONLY descriptor lied about the
        // image's actual ATTACHMENT/TRANSFER layout. Transition exactly the
        // layout runs a previous command produced, at bind time, scope-ended
        // first (the ClearTextureFloat shape).
        //
        // GENERAL is included. It used to be skipped, on the reasoning that a
        // compute store-then-sample chain keeps its image in GENERAL and
        // moving it here "would break the write half" — but that left the
        // baked SHADER_READ_ONLY descriptor disagreeing with the image's real
        // layout on exactly those chains (HZB mip ladder, fog scatter ->
        // integrate, snow, cloud shadows, the ocean FFT), which is invalid
        // usage even though the RAW hazard is covered by the explicit
        // MemoryBarrier those paths issue. It is safe to move now because
        // BindImageTexture transitions BACK to GENERAL when the image is next
        // bound for storage — the write half is no longer stranded.
        {
            VkImageSubresourceRange whole{};
            // A BARRIER on a combined depth-stencil image must name BOTH
            // aspects while separateDepthStencilLayouts is off
            // (VUID-VkImageMemoryBarrier2-image-03320) — naming only DEPTH
            // leaves the stencil half in whatever layout the producer left it
            // (TRANSFER_DST after a depth clear), and the draw that samples the
            // image then fails VUID-vkCmdDraw-None-09600 on the stencil
            // subresource. The DESCRIPTOR view below is the opposite rule: a
            // sampled view may name exactly one aspect, so it stays DEPTH.
            // (First hit by the decal tenant — the first pass on Vulkan to
            // sample a depth attachment; #691.)
            whole.aspectMask = VulkanBarrierLowering::AspectMaskFor(AspectFromInfo(*info));
            whole.baseMipLevel = 0;
            whole.levelCount = VK_REMAINING_MIP_LEVELS;
            whole.baseArrayLayer = 0;
            whole.layerCount = VK_REMAINING_ARRAY_LAYERS;
            EnsureImageLayoutForDescriptor(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, whole);
        }

        // Default whole-image sampled view. Depth-stencil formats sample the
        // DEPTH aspect (GLSL sampler2D/shadow reads depth; sampling stencil
        // needs an explicit stencil view, which no current pass requests).
        // The registry carries the dimensionality (a 3D volume samples as
        // sampler3D — hardcoding 2D here built an invalid view for it).
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image;
        view.viewType = info->ViewType;
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

        // The sampler half (#691): "no stated intent means the
        // texture object's own state" (rhi-abstraction-boundary.md §4f —
        // parity by construction with GL's glBindTextureUnit, which samples
        // with the object's parameters). Derived per bind from the recorded
        // per-image state; the sampler heap dedups, so steady state is a hash
        // lookup.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = info->MagFilter;
        samplerInfo.minFilter = info->MinFilter;
        samplerInfo.mipmapMode = info->MipmapMode;
        samplerInfo.addressModeU = info->AddressMode;
        samplerInfo.addressModeV = info->AddressMode;
        samplerInfo.addressModeW = info->AddressMode;
        samplerInfo.borderColor = info->BorderColor;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        // §4f's mandatory row: GL makes an integer texture with a LINEAR
        // filter INCOMPLETE (samples zero, texelFetch included on Mesa) —
        // NEAREST is forced, not defaulted, so the heap cannot reintroduce
        // the vanished-glyphs bug for RG16UI/R16UI/R32I textures.
        if (IsIntegerVkFormat(info->Format))
        {
            samplerInfo.magFilter = VK_FILTER_NEAREST;
            samplerInfo.minFilter = VK_FILTER_NEAREST;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        }
        bindingState.SetTextureSamplerSlot(slot, VulkanSamplerHeap::Get().GetOrCreateSlot(samplerInfo));
    }

    void VulkanRendererAPI::BindTexture(u32 slot, RHI::ResourceHandle texture, const RHI::SamplerDesc& sampler)
    {
        // Image half + the inherit sampler staging.
        BindTexture(slot, texture);

        // A default-constructed desc IS a request to inherit (RHIResources.h)
        // — the two-arg path above already staged the object's state.
        if (sampler.Source != RHI::SamplerSource::Explicit)
        {
            return;
        }
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        if (native == 0u)
        {
            return; // the two-arg path already cleared the slot
        }
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(reinterpret_cast<VkImage>(native));
        if (info == nullptr)
        {
            return;
        }

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = sampler.MagFilter == RHI::Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        samplerInfo.minFilter = sampler.MinFilter == RHI::Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        samplerInfo.mipmapMode =
            sampler.LinearMipFilter ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        const auto toAddressMode = [](const RHI::AddressMode mode)
        {
            switch (mode)
            {
                case RHI::AddressMode::Repeat:
                    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case RHI::AddressMode::MirroredRepeat:
                    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case RHI::AddressMode::ClampToEdge:
                    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                case RHI::AddressMode::ClampToBorder:
                    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            }
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };
        samplerInfo.addressModeU = toAddressMode(sampler.AddressU);
        samplerInfo.addressModeV = toAddressMode(sampler.AddressV);
        samplerInfo.addressModeW = toAddressMode(sampler.AddressW);
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        // Compare::Never means "comparison disabled", not "compare with NEVER"
        // — the SamplerDesc contract. This is what makes sampler2DArrayShadow
        // reads legal on this backend: the ShadowDepthSampler desc carries the
        // compare op the GL texture object used to hold.
        if (sampler.Compare != RHI::CompareOp::Never)
        {
            samplerInfo.compareEnable = VK_TRUE;
            switch (sampler.Compare)
            {
                case RHI::CompareOp::Never: // unreachable — guarded above
                    break;
                case RHI::CompareOp::Less:
                    samplerInfo.compareOp = VK_COMPARE_OP_LESS;
                    break;
                case RHI::CompareOp::Equal:
                    samplerInfo.compareOp = VK_COMPARE_OP_EQUAL;
                    break;
                case RHI::CompareOp::LessOrEqual:
                    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                    break;
                case RHI::CompareOp::Greater:
                    samplerInfo.compareOp = VK_COMPARE_OP_GREATER;
                    break;
                case RHI::CompareOp::NotEqual:
                    samplerInfo.compareOp = VK_COMPARE_OP_NOT_EQUAL;
                    break;
                case RHI::CompareOp::GreaterOrEqual:
                    samplerInfo.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
                    break;
                case RHI::CompareOp::Always:
                    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
                    break;
            }
        }
        if (sampler.MaxAnisotropy > 1.0f)
        {
            samplerInfo.anisotropyEnable = VK_TRUE;
            samplerInfo.maxAnisotropy = sampler.MaxAnisotropy;
        }
        switch (sampler.Border)
        {
            case RHI::BorderColor::TransparentBlack:
                samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                break;
            case RHI::BorderColor::OpaqueBlack:
                samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
                break;
            case RHI::BorderColor::OpaqueWhite:
                samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                break;
        }
        // The integer-format NEAREST rule outranks even an explicit desc —
        // GL answered a LINEAR filter on an integer texture with
        // incompleteness, never with linear filtering.
        if (IsIntegerVkFormat(info->Format))
        {
            samplerInfo.magFilter = VK_FILTER_NEAREST;
            samplerInfo.minFilter = VK_FILTER_NEAREST;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        }
        VulkanBindingState::Get().SetTextureSamplerSlot(slot, VulkanSamplerHeap::Get().GetOrCreateSlot(samplerInfo));
    }

    void VulkanRendererAPI::BindImageTexture(u32 unit, RHI::ResourceHandle texture, u32 mipLevel, bool layered, u32 layer, RHI::Access /*access*/, RHI::Format format)
    {
        // `access` is Vulkan-irrelevant here (it lives in shader qualifiers
        // and barriers - the ViewDesc::StorageAccess note); the FORMAT is the
        // descriptor-typed half a storage binding cannot do without.
        auto& bindingState = VulkanBindingState::Get();
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        if (native == 0u)
        {
            bindingState.SetImageHeapSlot(unit, VulkanBindingState::kNoHeapSlot);
            return;
        }
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr)
        {
            bindingState.SetImageHeapSlot(unit, VulkanBindingState::kNoHeapSlot);
            return;
        }

        VkFormat viewFormat = VulkanBarrierLowering::ToVkFormat(format);
        if (viewFormat == VK_FORMAT_UNDEFINED)
        {
            viewFormat = info->Format;
        }

        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = image;
        // A 3D volume (image3D) binds as a whole-volume 3D view — GL's
        // "layered" flag is how image3D binds arrive; layer slicing of 3D
        // images (2D-slice views) has no current consumer.
        view.viewType = info->ViewType == VK_IMAGE_VIEW_TYPE_3D
                            ? VK_IMAGE_VIEW_TYPE_3D
                            : ((layered && info->ArrayLayers > 1u) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                   : VK_IMAGE_VIEW_TYPE_2D);
        view.format = viewFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = std::min(mipLevel, std::max(info->MipLevels, 1u) - 1u);
        view.subresourceRange.levelCount = 1u;
        view.subresourceRange.baseArrayLayer =
            layered ? 0u : std::min(layer, std::max(info->ArrayLayers, 1u) - 1u);
        view.subresourceRange.layerCount = layered ? std::max(info->ArrayLayers, 1u) : 1u;

        // Storage accesses live in GENERAL (the barrier lowering's rule), and
        // the descriptor below bakes GENERAL — so the image has to ACTUALLY BE
        // in GENERAL. Nothing else puts it there: only the render graph's
        // planner barriers reach VulkanBarrierLowering::LayoutFor, so a
        // pass-owned or system-owned storage image (the froxel scatter volume,
        // cloud noise, the ocean FFT ping-pongs, the snow clipmaps, VG's
        // raster targets) reached its first imageStore in whatever layout
        // vkCreateImage left it — UNDEFINED. That is both a descriptor/layout
        // mismatch (VUID-vkCmdDispatch-None-09600) and undefined contents plus
        // uninitialised compression metadata on IHVs that use it, so a store
        // followed by a sample could return garbage rather than the value
        // stored. NVIDIA masks it; it is the AMD conformance runner that would
        // not. Validation cannot see it either — the descriptors live in the
        // resource heap and the layer cannot tell which slot a shader indexes.
        //
        // Same tracker-driven seam BindTexture uses, scoped to the SUBRESOURCE
        // this bind exposes (one mip, one layer or all): transition every run
        // that is not already GENERAL, including UNDEFINED runs, which cost
        // nothing because they discard contents that are undefined anyway.
        {
            VkImageSubresourceRange bound{};
            bound.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; // storage images are colour-aspect
            bound.baseMipLevel = view.subresourceRange.baseMipLevel;
            bound.levelCount = 1u;
            bound.baseArrayLayer = view.subresourceRange.baseArrayLayer;
            bound.layerCount = view.subresourceRange.layerCount;
            EnsureImageLayoutForDescriptor(image, VK_IMAGE_LAYOUT_GENERAL, bound);
        }

        const u32 heapSlot = VulkanDescriptorSlotCache::Get().AcquireSlot(
            image, view, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_LAYOUT_GENERAL);
        bindingState.SetImageHeapSlot(
            unit, heapSlot == VulkanResourceHeap::InvalidSlot ? VulkanBindingState::kNoHeapSlot : heapSlot);
    }

    void VulkanRendererAPI::EnsureImageLayoutForDescriptor(VkImage image, VkImageLayout target,
                                                           const VkImageSubresourceRange& range)
    {
        auto& ctx = Ctx();
        // The bind-time layout seam behind BOTH descriptor routes — see the
        // header note. GL-parity mid-pass visibility (#691): GL
        // makes a just-rendered (or just-copied) image visible to a later
        // texture() with NO application barrier, so pass bodies sample an
        // attachment they drew two draws ago without saying anything — the
        // bloom mip ladder, the JFA ping-pong, fog's and cloudscape's half-res
        // write-then-sample all do exactly this inside ONE Execute, where the
        // graph's per-pass planner barriers cannot reach. Without this seam
        // the sample raced the raster and read the PRE-draw content (the
        // clear), while the baked descriptor lied about the image's actual
        // layout. Transition exactly the layout runs a previous command
        // produced, at bind/publish time, scope-ended first (the
        // ClearTextureFloat shape).
        //
        // For a SAMPLED target, GENERAL is included. It used to be skipped, on
        // the reasoning that a compute store-then-sample chain keeps its image
        // in GENERAL and moving it here "would break the write half" — but
        // that left the baked SHADER_READ_ONLY descriptor disagreeing with the
        // image's real layout on exactly those chains (HZB mip ladder, fog
        // scatter -> integrate, snow, cloud shadows, the ocean FFT), which is
        // invalid usage even though the RAW hazard is covered by the explicit
        // MemoryBarrier those paths issue. Safe because a storage publish
        // transitions BACK to GENERAL — the write half is never stranded.
        // UNDEFINED is included for both targets: a never-written subresource
        // (a shadow cascade with no casters, a mid-frame-recreated resize
        // target) would otherwise stay UNDEFINED while the descriptor claims
        // otherwise; transitioning it is free — it discards contents that are
        // undefined anyway.
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            return;
        }
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr)
        {
            return;
        }
        ctx.Tracker.RegisterImage(image, std::max(info->MipLevels, 1u), std::max(info->ArrayLayers, 1u),
                                  info->RegistrationId, info->InitialLayout);

        std::vector<VkImageMemoryBarrier2> barriers;
        ctx.Tracker.ForEachLayoutRun(
            image, range,
            [&](const VkImageSubresourceRange& run, const VkImageLayout trackedLayout)
            {
                if (trackedLayout == target)
                    return; // already where this descriptor kind needs it
                if (target == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                {
                    // The sampled filter: only runs a prior command produced
                    // (or UNDEFINED, per above) move; anything else is a
                    // layout some OTHER machinery owns right now.
                    const bool producedByPriorCommand =
                        trackedLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
                        trackedLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
                        trackedLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
                        trackedLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
                        trackedLayout == VK_IMAGE_LAYOUT_GENERAL ||
                        trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED;
                    if (!producedByPriorCommand)
                        return;
                }

                VkImageMemoryBarrier2 b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                // Conservative all-stage scopes: the producer may be raster, a
                // transfer, a prior dispatch, or the harness — over-sync beats
                // a silent race on a correctness seam.
                b.srcStageMask = kAllStages;
                b.srcAccessMask = kAllAccess;
                b.dstStageMask = kAllStages;
                b.dstAccessMask = kAllAccess;
                b.oldLayout = trackedLayout;
                b.newLayout = target;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = run;
                barriers.push_back(b);
            });
        if (!barriers.empty())
        {
            // vkCmdPipelineBarrier2 is illegal inside a rendering scope;
            // ending here also closes the scope that RENDERED the runs being
            // transitioned (the JFA/bloom shape: the source's scope is still
            // open when the next iteration binds it).
            EndRenderingScope();
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = static_cast<u32>(barriers.size());
            dep.pImageMemoryBarriers = barriers.data();
            vkCmdPipelineBarrier2(ctx.Cmd, &dep);
            for (const auto& b : barriers)
            {
                ctx.Tracker.SetLayout(image, b.subresourceRange, target);
            }
        }
    }

    void VulkanRendererAPI::StageTransferTransition(VkImage image, const VkImageSubresourceRange& range,
                                                    VkImageLayout newLayout, VkAccessFlags2 dstAccess,
                                                    std::vector<VkImageMemoryBarrier2>& out)
    {
        auto& ctx = Ctx();
        // The CopyImageSubData / ClearTextureFloat discipline, shared by the
        // blit path: register the image with the tracker (idempotent), emit
        // one exact-oldLayout barrier per layout run — a same-layout run still
        // emits, so the producer->transfer execution+memory dependency never
        // silently disappears — and advance the tracker.
        if (const auto* info = VulkanImageInfoRegistry::Get().Lookup(image); info != nullptr)
        {
            ctx.Tracker.RegisterImage(image, std::max(info->MipLevels, 1u), std::max(info->ArrayLayers, 1u),
                                      info->RegistrationId, info->InitialLayout);
        }
        ctx.Tracker.ForEachLayoutRun(
            image, range,
            [&](const VkImageSubresourceRange& run, const VkImageLayout trackedLayout)
            {
                VkImageMemoryBarrier2 b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask = (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_2_NONE : kAllStages;
                b.srcAccessMask = (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ACCESS_2_NONE : kAllAccess;
                b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                b.dstAccessMask = dstAccess;
                b.oldLayout = trackedLayout;
                b.newLayout = newLayout;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = run;
                out.push_back(b);
            });
        ctx.Tracker.SetLayout(image, range, newLayout);
    }

    void VulkanRendererAPI::CopyImageSubData(RHI::ResourceHandle src, TextureTargetType /*srcTarget*/, RHI::ResourceHandle dst, TextureTargetType /*dstTarget*/, u32 width, u32 height)
    {
        auto& ctx = Ctx();
        // The facade contract is glCopyImageSubData's no-offset form: mip 0,
        // origin 0, one layer, width x height texels (GTAO's final AO copy,
        // SSAO's blur copy). Lowered as vkCmdCopyImage with exact-oldLayout
        // transitions into the transfer layouts, per layout run — the
        // ClearTextureFloat shape (#691).
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("CopyImageSubData(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        if (width == 0u || height == 0u || !src.IsValid() || !dst.IsValid())
            return;

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        auto& registry = RHI::ResourceRegistry::Get();
        const u64 srcNative = registry.ResolveNativeForBackend(src);
        const u64 dstNative = registry.ResolveNativeForBackend(dst);
        if (srcNative == 0u || dstNative == 0u)
            return;
        const auto srcImage = reinterpret_cast<VkImage>(srcNative);
        const auto dstImage = reinterpret_cast<VkImage>(dstNative);
        const auto* srcInfo = VulkanImageInfoRegistry::Get().Lookup(srcImage);
        const auto* dstInfo = VulkanImageInfoRegistry::Get().Lookup(dstImage);
        if (srcInfo == nullptr || dstInfo == nullptr)
            return;

        const VkImageAspectFlags srcAspect = VulkanBarrierLowering::AspectMaskFor(AspectFromInfo(*srcInfo));
        const VkImageAspectFlags dstAspect = VulkanBarrierLowering::AspectMaskFor(AspectFromInfo(*dstInfo));
        ctx.Tracker.RegisterImage(srcImage, std::max(srcInfo->MipLevels, 1u), std::max(srcInfo->ArrayLayers, 1u),
                                  srcInfo->RegistrationId, srcInfo->InitialLayout);
        ctx.Tracker.RegisterImage(dstImage, std::max(dstInfo->MipLevels, 1u), std::max(dstInfo->ArrayLayers, 1u),
                                  dstInfo->RegistrationId, dstInfo->InitialLayout);

        VkImageSubresourceRange srcRange{ srcAspect, 0u, 1u, 0u, 1u };
        VkImageSubresourceRange dstRange{ dstAspect, 0u, 1u, 0u, 1u };

        // Both transitions ride one dependency info. A run whose tracked layout
        // already equals the transfer layout still emits (same-layout barriers
        // are legal) so the execution+memory dependency between the producer
        // and this copy never silently disappears.
        std::vector<VkImageMemoryBarrier2> toTransfer;
        const auto stageTransition = [&](const VkImage image, const VkImageSubresourceRange& range,
                                         const VkImageLayout newLayout, const VkAccessFlags2 dstAccess)
        {
            ctx.Tracker.ForEachLayoutRun(
                image, range,
                [&](const VkImageSubresourceRange& run, const VkImageLayout trackedLayout)
                {
                    VkImageMemoryBarrier2 b{};
                    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    b.srcStageMask = (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_2_NONE : kAllStages;
                    b.srcAccessMask = (trackedLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_ACCESS_2_NONE : kAllAccess;
                    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    b.dstAccessMask = dstAccess;
                    b.oldLayout = trackedLayout;
                    b.newLayout = newLayout;
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = image;
                    b.subresourceRange = run;
                    toTransfer.push_back(b);
                });
            ctx.Tracker.SetLayout(image, range, newLayout);
        };
        stageTransition(srcImage, srcRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        stageTransition(dstImage, dstRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<u32>(toTransfer.size());
        dep.pImageMemoryBarriers = toTransfer.data();
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);

        VkImageCopy region{};
        region.srcSubresource = { srcAspect, 0u, 0u, 1u };
        region.dstSubresource = { dstAspect, 0u, 0u, 1u };
        region.extent = { width, height, 1u };
        vkCmdCopyImage(ctx.Cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
    }

    void VulkanRendererAPI::CopyImageSubDataFull(RHI::ResourceHandle src, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ, RHI::ResourceHandle dst, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ, u32 width, u32 height)
    {
        // glCopyImageSubData's addressed form (#691): level = mip and
        // z = array layer — for a cubemap target GL's z IS the face index,
        // which is exactly a Vulkan array layer on the CUBE_COMPATIBLE image.
        // This is the IBL/sky bake's face write (render to a 2D framebuffer,
        // copy into cube face i at mip m), so implementing it is what turns
        // the flat grey sky into a skybox. The body lives in the offset-taking
        // sibling below; this form is its (0, 0) special case.
        CopyImageSubDataRegion(src, srcTarget, srcLevel, 0, 0, srcZ, dst, dstTarget, dstLevel, 0, 0, dstZ,
                               width, height);
    }

    void VulkanRendererAPI::CopyImageSubDataRegion(RHI::ResourceHandle src, TextureTargetType /*srcTarget*/, i32 srcLevel, i32 srcX, i32 srcY, i32 srcZ, RHI::ResourceHandle dst, TextureTargetType /*dstTarget*/, i32 dstLevel, i32 dstX, i32 dstY, i32 dstZ, u32 width, u32 height)
    {
        auto& ctx = Ctx();
        // Offsets pass through UNSCALED (the facade's block-copy contract,
        // RendererAPI.h): for a mixed compressed/uncompressed pair Vulkan takes
        // the same source-texel extent and dest-texel offsets as GL. Same
        // transition discipline as the no-offset sibling above.
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("CopyImageSubDataRegion(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        if (width == 0u || height == 0u || !src.IsValid() || !dst.IsValid() || srcLevel < 0 || dstLevel < 0 ||
            srcX < 0 || srcY < 0 || srcZ < 0 || dstX < 0 || dstY < 0 || dstZ < 0)
        {
            return;
        }

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        auto& registry = RHI::ResourceRegistry::Get();
        const u64 srcNative = registry.ResolveNativeForBackend(src);
        const u64 dstNative = registry.ResolveNativeForBackend(dst);
        if (srcNative == 0u || dstNative == 0u)
        {
            return;
        }
        const auto srcImage = reinterpret_cast<VkImage>(srcNative);
        const auto dstImage = reinterpret_cast<VkImage>(dstNative);
        const auto* srcInfo = VulkanImageInfoRegistry::Get().Lookup(srcImage);
        const auto* dstInfo = VulkanImageInfoRegistry::Get().Lookup(dstImage);
        if (srcInfo == nullptr || dstInfo == nullptr)
        {
            return;
        }
        // ArrayLayers carries the DEPTH for a 3D image (see VulkanImageInfo),
        // so the same bound covers a slice index and a layer index.
        if (static_cast<u32>(srcLevel) >= std::max(srcInfo->MipLevels, 1u) ||
            static_cast<u32>(dstLevel) >= std::max(dstInfo->MipLevels, 1u) ||
            static_cast<u32>(srcZ) >= std::max(srcInfo->ArrayLayers, 1u) ||
            static_cast<u32>(dstZ) >= std::max(dstInfo->ArrayLayers, 1u))
        {
            OLO_CORE_WARN("[RHI/Vulkan] CopyImageSubDataRegion: subresource out of range (src mip {}/{} layer {}/{}, "
                          "dst mip {}/{} layer {}/{}) — copy skipped",
                          srcLevel, srcInfo->MipLevels, srcZ, srcInfo->ArrayLayers, dstLevel, dstInfo->MipLevels,
                          dstZ, dstInfo->ArrayLayers);
            return;
        }

        const VkImageAspectFlags srcAspect = VulkanBarrierLowering::AspectMaskFor(AspectFromInfo(*srcInfo));
        const VkImageAspectFlags dstAspect = VulkanBarrierLowering::AspectMaskFor(AspectFromInfo(*dstInfo));
        ctx.Tracker.RegisterImage(srcImage, std::max(srcInfo->MipLevels, 1u), std::max(srcInfo->ArrayLayers, 1u),
                                  srcInfo->RegistrationId, srcInfo->InitialLayout);
        ctx.Tracker.RegisterImage(dstImage, std::max(dstInfo->MipLevels, 1u), std::max(dstInfo->ArrayLayers, 1u),
                                  dstInfo->RegistrationId, dstInfo->InitialLayout);

        // Same 3D-vs-array split as the copy region below: a 3D image has one
        // array layer, so its barrier range must name layer 0 whatever slice
        // the copy addresses.
        const VkImageSubresourceRange srcRange{
            srcAspect, static_cast<u32>(srcLevel), 1u,
            srcInfo->ViewType == VK_IMAGE_VIEW_TYPE_3D ? 0u : static_cast<u32>(srcZ), 1u
        };
        const VkImageSubresourceRange dstRange{
            dstAspect, static_cast<u32>(dstLevel), 1u,
            dstInfo->ViewType == VK_IMAGE_VIEW_TYPE_3D ? 0u : static_cast<u32>(dstZ), 1u
        };

        std::vector<VkImageMemoryBarrier2> toTransfer;
        StageTransferTransition(srcImage, srcRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_ACCESS_2_TRANSFER_READ_BIT, toTransfer);
        StageTransferTransition(dstImage, dstRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, toTransfer);
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<u32>(toTransfer.size());
        dep.pImageMemoryBarriers = toTransfer.data();
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);

        // z means different things per dimensionality, and Vulkan checks it:
        // for a 3D image baseArrayLayer MUST be 0 and the slice is the copy's
        // z OFFSET, while for a 2D array it is the layer and the offset must be
        // 0. GL's glCopyImageSubData takes one z for both and sorts it out
        // itself, which is why this only bites here. Getting it wrong is a
        // validation error and a copy of the wrong subresource, not a wrong
        // picture — the same split ReadTextureSubImage already makes.
        const bool srcIsVolume = srcInfo->ViewType == VK_IMAGE_VIEW_TYPE_3D;
        const bool dstIsVolume = dstInfo->ViewType == VK_IMAGE_VIEW_TYPE_3D;

        VkImageCopy region{};
        region.srcSubresource = { srcAspect, static_cast<u32>(srcLevel),
                                  srcIsVolume ? 0u : static_cast<u32>(srcZ), 1u };
        region.dstSubresource = { dstAspect, static_cast<u32>(dstLevel),
                                  dstIsVolume ? 0u : static_cast<u32>(dstZ), 1u };
        region.srcOffset = { srcX, srcY, srcIsVolume ? srcZ : 0 };
        region.dstOffset = { dstX, dstY, dstIsVolume ? dstZ : 0 };
        region.extent = { width, height, 1u };
        vkCmdCopyImage(ctx.Cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
    }

    void VulkanRendererAPI::CopyFramebufferToTexture(RHI::ResourceHandle /*texture*/, u32 /*width*/, u32 /*height*/)
    {
        UnimplementedStub("CopyFramebufferToTexture");
    }

    VulkanRendererAPI::FramebufferAttachmentSelection* VulkanRendererAPI::FindSelection(RHI::ResourceHandle framebuffer)
    {
        auto& ctx = Ctx();
        if (!framebuffer.IsValid())
        {
            return nullptr;
        }
        const auto it = ctx.Selections.find(SelectionKey(framebuffer));
        return it != ctx.Selections.end() ? &it->second : nullptr;
    }

    void VulkanRendererAPI::EndScopeIfTargets(RHI::ResourceHandle framebuffer)
    {
        auto& ctx = Ctx();
        if (ctx.Scope.Active && ctx.Scope.Target != nullptr && ctx.Scope.Target->GetRHIHandle() == framebuffer)
        {
            EndRenderingScope();
        }
    }

    void VulkanRendererAPI::SetDrawBuffers(std::span<const u32> attachments)
    {
        auto* target = VulkanBindingState::Get().GetCurrentFramebuffer();
        if (target == nullptr)
        {
            // GL's bound form against the default framebuffer — nothing to
            // select until the swapchain import exists.
            return;
        }
        SetFramebufferDrawAttachments(target->GetRHIHandle(), attachments);
    }

    void VulkanRendererAPI::RestoreAllDrawBuffers(u32 colorAttachmentCount)
    {
        auto* target = VulkanBindingState::Get().GetCurrentFramebuffer();
        if (target == nullptr)
        {
            return;
        }
        RestoreAllFramebufferDrawAttachments(target->GetRHIHandle(), colorAttachmentCount);
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateDepthArrayCompareOffViewHandle(RHI::ResourceHandle srcTexture, u32 /*numLayers*/)
    {
        if (RefuseOnWorker("CreateDepthArrayCompareOffViewHandle"))
        {
            return {};
        }
        // GL builds a second texture VIEW whose sampler state has depth
        // comparison OFF, so PCSS blocker searches can read raw occluder
        // depth. On this backend the equivalent needs no new view OBJECT:
        // BindTexture mints the (image, view-desc, SAMPLED, layout) descriptor
        // through VulkanDescriptorSlotCache, and the embedded sampler the
        // pipeline pairs with it is the compare-DISABLED default — i.e. a
        // plain alias handle to the SAME VkImage already samples raw depth.
        // (The compare-ON family is the special one here, and that is the
        // per-binding embedded-sampler work the shadow path owns.)
        // The alias is REGISTERED as a texture in its own right — mirroring
        // the GL twin's "a separate name with its own lifetime" contract — and
        // cached per image so repeated calls (ShadowMap init + placeholder
        // statics) return one identity. Lifetime: the alias is never retired;
        // its owners (ShadowMap's arrays + process-lifetime placeholders) own
        // the source image for at least as long, and the slot cache frees
        // descriptors by VkImage, so a destroyed source leaves only an inert
        // registry row. `numLayers` is baked into the GL view; here the bind
        // path derives layer count from the image registry, so it is unused.
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(srcTexture);
        if (native == 0u)
        {
            return RHI::NullResource;
        }
        if (const auto it = m_CompareOffViewHandles.find(native); it != m_CompareOffViewHandles.end())
        {
            return it->second;
        }
        const RHI::ResourceHandle alias =
            RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Texture, native, RHI::Backend::Vulkan);
        m_CompareOffViewHandles.emplace(native, alias);
        return alias;
    }

    void VulkanRendererAPI::SetTextureFilter(RHI::ResourceHandle texture, RHI::Filter minFilter, RHI::Filter magFilter)
    {
        // #691: GL keeps filter/wrap on the texture OBJECT; here the
        // object's state lives in the image-info registry, and BindTexture
        // derives the sampler-heap slot from it at bind time — so a mutation
        // takes effect on the next bind, exactly like glTextureParameteri.
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        if (native == 0u)
        {
            UnimplementedStub("SetTextureFilter(unresolved texture)", StubKind::PreconditionFailure);
            return;
        }
        VulkanImageInfoRegistry::Get().SetSamplerFilter(
            reinterpret_cast<VkImage>(native), minFilter == RHI::Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
            magFilter == RHI::Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }

    void VulkanRendererAPI::SetTextureWrap(RHI::ResourceHandle texture, RHI::AddressMode wrap)
    {
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        if (native == 0u)
        {
            UnimplementedStub("SetTextureWrap(unresolved texture)", StubKind::PreconditionFailure);
            return;
        }
        VkSamplerAddressMode mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        switch (wrap)
        {
            case RHI::AddressMode::Repeat:
                mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                break;
            case RHI::AddressMode::MirroredRepeat:
                mode = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                break;
            case RHI::AddressMode::ClampToEdge:
                mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                break;
            case RHI::AddressMode::ClampToBorder:
                mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                break;
        }
        VulkanImageInfoRegistry::Get().SetSamplerAddressMode(reinterpret_cast<VkImage>(native), mode);
    }

    void VulkanRendererAPI::UploadTextureSubImage2D(RHI::ResourceHandle texture, u32 width, u32 height, RHI::Format sourceFormat, const void* data)
    {
        if (RefuseOnWorker("UploadTextureSubImage2D"))
        {
            return;
        }
        // Whole-image form — the offset overload with a zero origin (#691;
        // the shared implementation lives there).
        UploadTextureSubImage2D(texture, 0, 0, width, height, sourceFormat, data);
    }

    // Conditional rendering — the HOST-side gate (ADR item A6, second half;
    // the shape §1.6a of the phase plan pinned as "conditional render -> CPU
    // skip"). VK_EXT_conditional_rendering exists and this device may well
    // expose it, but its predicate lives in a BUFFER, not in the query object:
    // using it would mean a scratch device buffer, a vkCmdCopyQueryPoolResults
    // with WAIT (a GPU-side stall that hangs outright on a never-written slot),
    // and an object whose lifetime outlives the API instance that made it.
    // The engine's ONE caller (CommandDispatch::DrawMesh) passes
    // OcclusionQueryPool::GetQueryHandle — the PREVIOUS frame's query, whose
    // result is already resolved host-side — so the host read is exact for the
    // production usage and costs nothing. The GL fail-safe is preserved: an
    // unavailable result renders (never culls).
    void VulkanRendererAPI::BeginConditionalRender(RHI::ResourceHandle query)
    {
        if (RefuseOnWorker("BeginConditionalRender"))
        {
            return;
        }
        auto& ctx = Ctx();
        u64 samplesPassed = 0;
        const bool available = ReadQueryResult(query, false, samplesPassed);
        ctx.ConditionalRenderSkip = available && samplesPassed == 0u;
    }

    void VulkanRendererAPI::EndConditionalRender()
    {
        auto& ctx = Ctx();
        ctx.ConditionalRenderSkip = false;
    }

    void VulkanRendererAPI::BindUniformBuffer(u32 bindingPoint, RHI::ResourceHandle buffer)
    {
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(buffer);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::UniformBuffer)
        {
            VulkanBindingState::Get().SetUniformBuffer(bindingPoint, nullptr);
            // An UNBIND is routine; a handle that resolves nowhere is not. Same
            // rule as BindStorageBuffer below (issue #1052): a bind that cannot
            // do what it was asked says so, rather than leaving the point empty
            // and letting the publication site substitute a block.
            if (buffer.IsValid())
            {
                UnimplementedStub("BindUniformBuffer(unresolvable buffer)", StubKind::PreconditionFailure);
            }
            return;
        }
        VulkanBindingState::Get().SetUniformBuffer(bindingPoint, static_cast<VulkanUniformBuffer*>(entry->Object));
    }

    void VulkanRendererAPI::BindStorageBuffer(u32 bindingPoint, RHI::ResourceHandle buffer)
    {
        auto& bindingState = VulkanBindingState::Get();
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(buffer);
        if (entry != nullptr && entry->Kind == VulkanRootObjectKind::StorageBuffer)
        {
            bindingState.SetStorageBuffer(bindingPoint, static_cast<VulkanStorageBuffer*>(entry->Object));
            bindingState.SetStorageBufferAddress(bindingPoint, 0);
            return;
        }

        // A RAW buffer (CreateBufferHandle) is a legitimate SSBO occupant, not
        // an error — GL's "a buffer is a buffer" reaches here whenever one
        // object serves two roles. Virtual geometry's index arena is exactly
        // that: an element buffer AND SSBO_VIRTUAL_INDICES, so it lives in
        // VulkanRawBufferRegistry and never in the root-object registry.
        //
        // This arm used to fall through to SetStorageBuffer(nullptr) SILENTLY.
        // The binding then took the frame arena's null block, and because that
        // block is small while the shaders index it with real cluster offsets
        // (localIndices[cluster.IndexBase + ...]), every virtual-geometry scene
        // read far past it and lost the device — VK_ERROR_DEVICE_LOST out of
        // vkQueueSubmit2, reported as a READ of an invalid address. The null
        // block defends against dereferencing address 0; it cannot defend
        // against an out-of-range read from a legitimately mapped substitute.
        // Issue #1052.
        bindingState.SetStorageBuffer(bindingPoint, nullptr);
        if (auto* raw = VulkanRawBufferRegistry::Get().Lookup(buffer);
            raw != nullptr && raw->DeviceAddress != 0)
        {
            bindingState.SetStorageBufferAddress(bindingPoint, raw->DeviceAddress);
            return;
        }
        bindingState.SetStorageBufferAddress(bindingPoint, 0);

        // An UNBIND (the null handle) is routine — passes clear their bind
        // points. A handle that resolves nowhere is not: it is the silent
        // no-occupant path above, and it must say so rather than let the
        // publication site substitute a block the shader will read past.
        if (buffer.IsValid())
        {
            UnimplementedStub("BindStorageBuffer(unresolvable buffer)", StubKind::PreconditionFailure);
        }
    }

    void VulkanRendererAPI::BindShaderProgram(RHI::ResourceHandle program)
    {
        // The null handle is glUseProgram(0): several passes unbind their
        // program at exit (ShaderDebugDraw / ForwardOverlay state hygiene) —
        // an unbind, not an unresolvable bind (#691).
        if (!program.IsValid())
        {
            if (auto* bound = VulkanShader::GetCurrentlyBound(); bound != nullptr)
                bound->Unbind();
            return;
        }
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(program);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::Shader)
        {
            UnimplementedStub("BindShaderProgram(unresolvable shader)", StubKind::PreconditionFailure);
            return;
        }
        static_cast<VulkanShader*>(entry->Object)->Bind();
    }

    void VulkanRendererAPI::BindVertexArrayRaw(RHI::ResourceHandle vertexArray)
    {
        auto& ctx = Ctx();
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(vertexArray);
        ctx.BoundVertexArray = (entry != nullptr && entry->Kind == VulkanRootObjectKind::VertexArray)
                                   ? static_cast<VulkanVertexArray*>(entry->Object)
                                   : nullptr;
    }

    // Defined below (after the draw entries); declared here because the
    // by-handle bind family needs it first.
    static VulkanFramebuffer* ResolveFramebufferObject(RHI::ResourceHandle framebuffer);

    void VulkanRendererAPI::BindFramebuffer(RHI::ResourceHandle framebuffer)
    {
        // #691: raw framebuffers (CreateFramebufferHandle) reach the
        // backend ONLY through this by-handle entry — there is no engine
        // Framebuffer object to call Bind() on — so it resolves through the
        // root-object side table and publishes exactly like the object form.
        // The null handle is glBindFramebuffer(0): publish "no target" (the
        // default framebuffer arrives with the swapchain import).
        if (!framebuffer.IsValid())
        {
            VulkanBindingState::Get().SetCurrentFramebuffer(nullptr);
            return;
        }
        auto* fb = ResolveFramebufferObject(framebuffer);
        if (fb == nullptr)
        {
            UnimplementedStub("BindFramebuffer(unresolvable framebuffer)", StubKind::PreconditionFailure);
            return;
        }
        fb->Bind();
    }

    void VulkanRendererAPI::DrawBoundIndexed(RHI::PrimitiveTopology topology, u32 indexCount, RHI::IndexType /*indexType*/, u32 baseIndex)
    {
        auto& ctx = Ctx();
        // The engine's index format is fixed 32-bit (IndexBuffer.h contract);
        // BindIndexBufferFor binds UINT32 accordingly.
        if (PrepareDraw(ctx.BoundVertexArray, ToVkTopology(topology)) && BindIndexBufferFor(ctx.BoundVertexArray))
        {
            vkCmdDrawIndexed(ctx.Cmd, indexCount, 1, baseIndex, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawBoundIndexedInstanced(RHI::PrimitiveTopology topology, u32 indexCount, RHI::IndexType /*indexType*/, u32 baseIndex, u32 instanceCount)
    {
        auto& ctx = Ctx();
        if (PrepareDraw(ctx.BoundVertexArray, ToVkTopology(topology)) && BindIndexBufferFor(ctx.BoundVertexArray))
        {
            vkCmdDrawIndexed(ctx.Cmd, indexCount, instanceCount, baseIndex, 0, 0);
        }
    }

    void VulkanRendererAPI::DrawBoundArrays(RHI::PrimitiveTopology topology, u32 firstVertex, u32 vertexCount)
    {
        auto& ctx = Ctx();
        if (PrepareDraw(ctx.BoundVertexArray, ToVkTopology(topology)))
        {
            vkCmdDraw(ctx.Cmd, vertexCount, 1, firstVertex, 0);
        }
    }

    namespace
    {
        // Resolve an attach operand to the backend texture object. Only
        // raw-registry textures are attachable: an object-owned attachment
        // already lives inside its framebuffer, and every production caller
        // (FluidIntermediatesPass) attaches raw-family textures — so a
        // foreign handle here is a caller bug worth one loud line, not a
        // second resolution path.
        [[nodiscard]] Ref<VulkanTexture2D> ResolveRawAttachTexture(RHI::ResourceHandle texture, const char* entryPoint)
        {
            Ref<VulkanTexture2D> resolved = VulkanRawTextureRegistry::Get().Lookup2D(texture);
            if (resolved == nullptr)
            {
                static std::atomic<bool> s_WarnedForeign{ false };
                if (!s_WarnedForeign.exchange(true, std::memory_order_relaxed))
                {
                    OLO_CORE_WARN("[RHI/Vulkan] {}: only raw-registry textures (CreateTexture2DHandle) can be "
                                  "attached — attach skipped (warn-once)",
                                  entryPoint);
                }
            }
            return resolved;
        }

        // No caller passes mip > 0 today (verified across the engine); the
        // attachment-view plumbing is mip-0 only, so say so once instead of
        // silently rendering into the wrong mip.
        [[nodiscard]] bool WarnUnsupportedAttachMip(u32 mipLevel, const char* entryPoint)
        {
            if (mipLevel == 0u)
            {
                return false;
            }
            static std::atomic<bool> s_WarnedMip{ false };
            if (!s_WarnedMip.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] {}: mip {} attachment is not lowered yet — attach skipped (warn-once)",
                              entryPoint, mipLevel);
            }
            return true;
        }
    } // namespace

    void VulkanRendererAPI::AttachFramebufferColorTexture(RHI::ResourceHandle framebuffer, u32 attachmentIndex, RHI::ResourceHandle texture, u32 mipLevel)
    {
        if (RefuseOnWorker("AttachFramebufferColorTexture"))
        {
            return;
        }
        auto* fb = ResolveFramebufferObject(framebuffer);
        if (fb == nullptr)
        {
            UnimplementedStub("AttachFramebufferColorTexture(unresolved framebuffer)", StubKind::PreconditionFailure);
            return;
        }
        if (WarnUnsupportedAttachMip(mipLevel, "AttachFramebufferColorTexture"))
        {
            return;
        }
        // RHI::NullResource detaches — the GL native-0 contract.
        if (!texture.IsValid())
        {
            fb->AttachExternalColorTexture(attachmentIndex, nullptr);
            return;
        }
        Ref<VulkanTexture2D> resolved = ResolveRawAttachTexture(texture, "AttachFramebufferColorTexture");
        if (resolved == nullptr)
        {
            return;
        }
        fb->AttachExternalColorTexture(attachmentIndex, std::move(resolved));
    }

    void VulkanRendererAPI::AttachFramebufferDepthTexture(RHI::ResourceHandle framebuffer, RHI::ResourceHandle texture, u32 mipLevel)
    {
        if (RefuseOnWorker("AttachFramebufferDepthTexture"))
        {
            return;
        }
        auto* fb = ResolveFramebufferObject(framebuffer);
        if (fb == nullptr)
        {
            UnimplementedStub("AttachFramebufferDepthTexture(unresolved framebuffer)", StubKind::PreconditionFailure);
            return;
        }
        if (WarnUnsupportedAttachMip(mipLevel, "AttachFramebufferDepthTexture"))
        {
            return;
        }
        if (!texture.IsValid())
        {
            fb->AttachExternalDepthTexture(nullptr);
            return;
        }
        Ref<VulkanTexture2D> resolved = ResolveRawAttachTexture(texture, "AttachFramebufferDepthTexture");
        if (resolved == nullptr)
        {
            return;
        }
        fb->AttachExternalDepthTexture(std::move(resolved));
    }

    bool VulkanRendererAPI::IsFramebufferComplete(RHI::ResourceHandle framebuffer)
    {
        // The GL twin answers "no" for native 0 (a stale handle would
        // otherwise report the always-complete default framebuffer); the
        // equivalent here is an unresolvable handle.
        auto* fb = ResolveFramebufferObject(framebuffer);
        if (fb == nullptr)
        {
            return false;
        }
        // No VkFramebuffer object exists to ask, so completeness is the
        // attachment invariant the rendering scope needs: at least one
        // attachment, and every NON-null attachment backed by a live VkImage
        // (null color slots are the scope's VK_ATTACHMENT_UNUSED shape).
        u32 attachmentCount = 0;
        for (u32 i = 0; i < fb->GetColorAttachmentCount(); ++i)
        {
            const Ref<VulkanTexture2D> image = fb->GetColorAttachmentImage(i);
            if (image == nullptr)
            {
                continue;
            }
            if (image->GetVkImage() == VK_NULL_HANDLE)
            {
                return false;
            }
            ++attachmentCount;
        }
        if (const Ref<VulkanTexture2D> depth = fb->GetDepthAttachmentImage(); depth != nullptr)
        {
            if (depth->GetVkImage() == VK_NULL_HANDLE)
            {
                return false;
            }
            ++attachmentCount;
        }
        return attachmentCount >= 1u;
    }

    // Resolve a framebuffer HANDLE to its backend object through the
    // root-object side table (the FB's registry native is 0 — no
    // VkFramebuffer exists under dynamic rendering). Null on a stale or
    // foreign handle; callers warn-once rather than crash.
    static VulkanFramebuffer* ResolveFramebufferObject(RHI::ResourceHandle framebuffer)
    {
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(framebuffer);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::Framebuffer)
        {
            return nullptr;
        }
        return static_cast<VulkanFramebuffer*>(entry->Object);
    }

    void VulkanRendererAPI::SetFramebufferDrawAttachments(RHI::ResourceHandle framebuffer, std::span<const u32> attachmentIndices)
    {
        auto& ctx = Ctx();
        if (!framebuffer.IsValid())
        {
            return;
        }
        auto& selection = ctx.Selections[SelectionKey(framebuffer)];
        const u32 count = std::min<u32>(static_cast<u32>(attachmentIndices.size()),
                                        static_cast<u32>(selection.DrawList.size()));
        bool changed = count != selection.DrawListCount;
        for (u32 i = 0; !changed && i < count; ++i)
        {
            changed = selection.DrawList[i] != attachmentIndices[i];
        }
        if (!changed)
        {
            return;
        }
        selection.DrawListCount = count;
        for (u32 i = 0; i < count; ++i)
        {
            selection.DrawList[i] = attachmentIndices[i];
        }
        // A selection change re-shapes the live scope's attachment array: end
        // it so the next draw re-opens with the new mapping (only when the
        // scope actually targets this framebuffer — the raw form legally
        // mutates an unbound FB's state, GL's named-object semantics).
        EndScopeIfTargets(framebuffer);
    }

    void VulkanRendererAPI::RestoreAllFramebufferDrawAttachments(RHI::ResourceHandle framebuffer, u32 /*colorAttachmentCount*/)
    {
        // Identity over every color attachment IS this backend's stored
        // default, so the restore just clears the override; the count the GL
        // twin needs (to spell the identity list out) is derived from the
        // target at scope-open time here.
        auto* selection = FindSelection(framebuffer);
        if (selection == nullptr || selection->DrawListCount == 0)
        {
            return;
        }
        selection->DrawListCount = 0;
        EndScopeIfTargets(framebuffer);
    }

    void VulkanRendererAPI::SetFramebufferReadAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex)
    {
        auto& ctx = Ctx();
        if (!framebuffer.IsValid())
        {
            return;
        }
        // Read selection only feeds blits — no scope interaction.
        ctx.Selections[SelectionKey(framebuffer)].ReadAttachment = attachmentIndex;
    }

    void VulkanRendererAPI::ClearFramebufferColorAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex, const glm::vec4& color)
    {
        auto* fb = ResolveFramebufferObject(framebuffer);
        if (fb == nullptr)
        {
            UnimplementedStub("ClearFramebufferColorAttachment(unresolved framebuffer)", StubKind::PreconditionFailure);
            return;
        }
        // GL contract: the index is a DRAW BUFFER index, remapped through the
        // FB's draw-buffer selection (identity when none is stored).
        u32 effective = attachmentIndex;
        if (const auto* selection = FindSelection(framebuffer);
            selection != nullptr && selection->DrawListCount != 0)
        {
            if (attachmentIndex >= selection->DrawListCount)
            {
                return; // past the selected list — GL clears nothing
            }
            effective = selection->DrawList[attachmentIndex];
        }
        if (effective == RHI::NoAttachment || effective >= fb->GetColorAttachmentCount())
        {
            return;
        }
        // The transfer-clear facade owns scope-end + exact per-layout-run
        // transitions (the ClearTextureFloat shape) — same route
        // VulkanFramebuffer::ClearAllAttachments takes.
        ClearTextureFloat(fb->GetColorAttachmentHandle(effective), 0u, color);
    }

    void VulkanRendererAPI::ClearFramebufferDepth(RHI::ResourceHandle framebuffer, f32 depth)
    {
        auto* fb = ResolveFramebufferObject(framebuffer);
        if (fb == nullptr)
        {
            UnimplementedStub("ClearFramebufferDepth(unresolved framebuffer)", StubKind::PreconditionFailure);
            return;
        }
        const RHI::ResourceHandle depthAttachment = fb->GetDepthAttachmentHandle();
        if (!depthAttachment.IsValid())
        {
            return; // no depth attachment — GL is a silent no-op too
        }
        // ClearTextureFloat's depth path clears depth = color.r (stencil 0).
        ClearTextureFloat(depthAttachment, 0u, glm::vec4(depth, 0.0f, 0.0f, 0.0f));
    }

    void VulkanRendererAPI::BlitFramebuffer(RHI::ResourceHandle srcFramebuffer, RHI::ResourceHandle dstFramebuffer, i32 srcX0, i32 srcY0, i32 srcX1, i32 srcY1, i32 dstX0, i32 dstY0, i32 dstX1, i32 dstY1, RHI::BlitAspect aspect, RHI::Filter /*filter*/)
    {
        auto& ctx = Ctx();
        // The engine's production blits are 1:1 full-surface copies (depth
        // seeds, entity-ID handoffs, G-buffer resolves), lowered here as
        // vkCmdCopyImage with the CopyImageSubData transition discipline. A
        // scaling blit would need vkCmdBlitImage + per-aspect filter rules —
        // no current caller scales, so that arm is a loud warn-once (report,
        // don't guess) rather than silent wrong output. The filter argument
        // is meaningless for a 1:1 copy (Nearest semantics by construction).
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("BlitFramebuffer(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        auto* src = ResolveFramebufferObject(srcFramebuffer);
        auto* dst = ResolveFramebufferObject(dstFramebuffer);
        if (src == nullptr || dst == nullptr)
        {
            // RHI::NullResource spells "the default framebuffer" on GL — that
            // arm arrives with the swapchain import.
            UnimplementedStub("BlitFramebuffer(unresolved framebuffer)", StubKind::PreconditionFailure);
            return;
        }
        const i32 width = srcX1 - srcX0;
        const i32 height = srcY1 - srcY0;
        if (width <= 0 || height <= 0)
        {
            return;
        }
        if (srcX0 != 0 || srcY0 != 0 || dstX0 != 0 || dstY0 != 0 || (dstX1 - dstX0) != width ||
            (dstY1 - dstY0) != height)
        {
            static std::atomic<bool> s_WarnedScaled{ false };
            if (!s_WarnedScaled.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] BlitFramebuffer with offset/scaling rects is not lowered yet "
                              "(src {}x{} at {},{} -> dst {}x{} at {},{}) — blit skipped",
                              width, height, srcX0, srcY0, dstX1 - dstX0, dstY1 - dstY0, dstX0, dstY0);
            }
            return;
        }

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        const auto copyOne = [&](const Ref<VulkanTexture2D>& srcImage, const Ref<VulkanTexture2D>& dstImage)
        {
            if (srcImage == nullptr || dstImage == nullptr)
            {
                return;
            }
            const VkImage srcVk = srcImage->GetVkImage();
            const VkImage dstVk = dstImage->GetVkImage();
            // The aspect comes from the IMAGE, not from the caller's
            // BlitAspect: a combined depth/stencil format must name BOTH
            // aspects in a layout-transition barrier (VUID 03320 — no
            // separateDepthStencilLayouts on the floor), and a matched-format
            // copy legally moves both (the unused stencil rides along, which
            // is also what GL's depth blit leaves behaviourally). The blit
            // pairs are format-matched by the caller contract, so one aspect
            // set serves src and dst alike.
            const auto* srcInfo = VulkanImageInfoRegistry::Get().Lookup(srcVk);
            const auto* dstInfo = VulkanImageInfoRegistry::Get().Lookup(dstVk);
            if (srcInfo == nullptr || dstInfo == nullptr)
            {
                return;
            }
            const VkImageAspectFlags aspectMask = VulkanBarrierLowering::AspectMaskFor(AspectFromInfo(*srcInfo));
            std::vector<VkImageMemoryBarrier2> toTransfer;
            const VkImageSubresourceRange srcRange{ aspectMask, 0u, 1u, 0u, 1u };
            const VkImageSubresourceRange dstRange{ aspectMask, 0u, 1u, 0u, 1u };
            StageTransferTransition(srcVk, srcRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_ACCESS_2_TRANSFER_READ_BIT, toTransfer);
            StageTransferTransition(dstVk, dstRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT, toTransfer);
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = static_cast<u32>(toTransfer.size());
            dep.pImageMemoryBarriers = toTransfer.data();
            vkCmdPipelineBarrier2(ctx.Cmd, &dep);

            VkImageCopy region{};
            region.srcSubresource = { aspectMask, 0u, 0u, 1u };
            region.dstSubresource = { aspectMask, 0u, 0u, 1u };
            region.extent = { static_cast<u32>(width), static_cast<u32>(height), 1u };
            vkCmdCopyImage(ctx.Cmd, srcVk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstVk,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
        };

        if (aspect == RHI::BlitAspect::Depth || aspect == RHI::BlitAspect::DepthStencil ||
            aspect == RHI::BlitAspect::Stencil)
        {
            // The BlitAspect selects the ATTACHMENT PAIR; the copied aspect
            // set comes from the images themselves (see copyOne).
            copyOne(src->GetDepthAttachmentImage(), dst->GetDepthAttachmentImage());
            return;
        }

        // Color: GL copies FROM the src's READ attachment TO every attachment
        // in the dst's draw-buffer selection (identity = all attachments).
        u32 readAttachment = 0;
        if (const auto* srcSelection = FindSelection(srcFramebuffer); srcSelection != nullptr)
        {
            readAttachment = srcSelection->ReadAttachment;
        }
        if (readAttachment >= src->GetColorAttachmentCount())
        {
            return;
        }
        const Ref<VulkanTexture2D> srcImage = src->GetColorAttachmentImage(readAttachment);

        const auto* dstSelection = FindSelection(dstFramebuffer);
        if (dstSelection != nullptr && dstSelection->DrawListCount != 0)
        {
            for (u32 i = 0; i < dstSelection->DrawListCount; ++i)
            {
                const u32 attachment = dstSelection->DrawList[i];
                if (attachment == RHI::NoAttachment || attachment >= dst->GetColorAttachmentCount())
                {
                    continue;
                }
                copyOne(srcImage, dst->GetColorAttachmentImage(attachment));
            }
            return;
        }
        for (u32 i = 0; i < dst->GetColorAttachmentCount(); ++i)
        {
            copyOne(srcImage, dst->GetColorAttachmentImage(i));
        }
    }

    void VulkanRendererAPI::AllocateBufferStorage(RHI::ResourceHandle buffer, u64 sizeBytes, RHI::MemoryResidency residency)
    {
        if (RefuseOnWorker("AllocateBufferStorage"))
        {
            return;
        }
        // Raw-handle family only (CreateBufferHandle mints these). An
        // object-backed buffer sizes itself; GL's glNamedBufferData shape has
        // no other Vulkan tenant.
        VulkanRawBufferRegistry::Get().Allocate(buffer, sizeBytes, residency);
    }

    void* VulkanRendererAPI::AllocatePersistentUploadStorage(RHI::ResourceHandle buffer, u64 sizeBytes)
    {
        if (RefuseOnWorker("AllocatePersistentUploadStorage"))
        {
            return nullptr;
        }
        // #1052. GL's shape is glNamedBufferStorage(WRITE|PERSISTENT|COHERENT)
        // followed by glMapNamedBufferRange over the whole range. VMA's twin is
        // a HOST_VISIBLE|HOST_COHERENT placement created MAPPED: the pointer is
        // the allocation's own persistent mapping and stays valid for the
        // buffer's life, so there is no map/unmap pair here at all (see
        // UnmapBuffer).
        //
        // Leaving this a no-op is what emptied virtual geometry's arenas on
        // Vulkan: GPUCircularBuffer::Create takes the returned nullptr as
        // "mapping failed" and degrades to direct uploads, and the direct
        // upload — UploadBufferSubData — was a no-op stub too, so every page
        // load wrote nothing to either arena and no raster arm had geometry to
        // draw.
        if (RHI::ResourceRegistry::Get().KindOf(buffer) != RHI::ResourceKind::Buffer)
        {
            UnimplementedStub("AllocatePersistentUploadStorage(not a buffer handle)", StubKind::PreconditionFailure);
            return nullptr;
        }
        auto& registry = VulkanRawBufferRegistry::Get();
        // A Buffer-kind handle this registry never minted would no-op inside
        // Allocate and then look identical to a failed mapping below, so the
        // caller's "mapping failed" branch would swallow a caller bug — the
        // exact #1052 shape. Refuse it here, counted.
        if (registry.Lookup(buffer) == nullptr)
        {
            UnimplementedStub("AllocatePersistentUploadStorage(not a raw-registry buffer)",
                              StubKind::PreconditionFailure);
            return nullptr;
        }
        registry.Allocate(buffer, sizeBytes, RHI::MemoryResidency::HostToDevice, /*requireHostCoherentMap=*/true);
        const auto* entry = registry.Lookup(buffer);
        if (entry == nullptr || entry->Mapped == nullptr)
        {
            // GL returns nullptr when the map fails and every caller carries
            // that branch, so this is the contract rather than a fallback — but
            // it is a real capability gap on a desktop device, so say so.
            OLO_CORE_ERROR("[RHI/Vulkan] AllocatePersistentUploadStorage({} bytes): no host-coherent mappable "
                           "placement — the caller's direct-upload path takes over",
                           sizeBytes);
            return nullptr;
        }
        return entry->Mapped;
    }

    void VulkanRendererAPI::UnmapBuffer(RHI::ResourceHandle buffer)
    {
        if (RefuseOnWorker("UnmapBuffer"))
        {
            return;
        }
        // #1052. Complete, and deliberately does nothing to the allocation.
        // GL needs this call because glMapNamedBufferRange hands out a mapping
        // the buffer object outlives; AllocatePersistentUploadStorage's VMA
        // placement is MAPPED for its whole life instead, so the mapping dies
        // with the storage in Destroy / a re-Allocate and there is nothing to
        // release here. Kind-guarded like its siblings so a handle from the
        // wrong family is still refused loudly rather than ignored.
        if (RHI::ResourceRegistry::Get().KindOf(buffer) != RHI::ResourceKind::Buffer ||
            VulkanRawBufferRegistry::Get().Lookup(buffer) == nullptr)
        {
            UnimplementedStub("UnmapBuffer(not a raw-registry buffer)", StubKind::PreconditionFailure);
        }
    }

    void VulkanRendererAPI::UploadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, const void* data)
    {
        if (RefuseOnWorker("UploadBufferSubData"))
        {
            return;
        }
        if (sizeBytes == 0u || data == nullptr)
        {
            return; // glNamedBufferSubData's degenerate call, which GL ignores
        }
        // #1052. GL's glNamedBufferSubData is ordered against everything the
        // frame already issued, so this stages into a transfer-source buffer
        // and records vkCmdCopyBuffer on the FRAME command buffer — the
        // UploadTextureSubImage2D shape, for the same reason. Resolution is
        // generic (the identity registry, like CopyBufferSubData) because both
        // families reach here: VirtualMeshRegistry's ring fallback writes the
        // RAW index arena and the OBJECT-backed vertex StorageBuffer through
        // this one entry point.
        auto& ctx = Ctx();
        auto& registry = RHI::ResourceRegistry::Get();
        const u64 native =
            registry.KindOf(buffer) == RHI::ResourceKind::Buffer ? registry.ResolveNativeForBackend(buffer) : 0u;
        if (native == 0u)
        {
            UnimplementedStub("UploadBufferSubData(unresolvable buffer)", StubKind::PreconditionFailure);
            return;
        }
        const auto dst = reinterpret_cast<VkBuffer>(native);

        // Range-check against the destination the way ReadBufferSubData does.
        // vkCmdCopyBuffer past the end is a VUID violation (and undefined
        // behaviour on a device without robustness); a counted refusal names
        // the caller instead. Only the raw family can be measured — an
        // object-backed buffer's size lives on its C++ object, and the
        // registry does not carry one.
        if (const auto* rawDst = VulkanRawBufferRegistry::Get().Lookup(buffer);
            rawDst != nullptr && offsetBytes + sizeBytes > rawDst->Size)
        {
            OLO_CORE_WARN("[RHI/Vulkan] UploadBufferSubData: out-of-range write ({}+{} of {}) — dropped", offsetBytes,
                          sizeBytes, rawDst->Size);
            UnimplementedStub("UploadBufferSubData(out-of-range)", StubKind::PreconditionFailure);
            return;
        }

        // A VulkanStorageBuffer serves DRAWS from a frame-arena SNAPSHOT when a
        // mid-frame SetData pushed one (GetRootDataAddress). This write goes to
        // the persistent buffer, so a live snapshot would shadow it and the
        // upload would land nowhere a shader looks — drop it, which returns the
        // buffer to resolving persistent.
        if (const auto* rootEntry = VulkanRootObjectRegistry::Get().Lookup(buffer);
            rootEntry != nullptr && rootEntry->Kind == VulkanRootObjectKind::StorageBuffer)
        {
            static_cast<VulkanStorageBuffer*>(rootEntry->Object)->InvalidateSnapshotForExternalWrite();
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] UploadBufferSubData with no live VulkanDevice — ignored");
            return;
        }

        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            // No recording bracket (load-time seeding, headless fixtures): the
            // blocking one-shot, which owns its own staging and the
            // availability barrier every later submission needs.
            VulkanOneShot::UploadToBuffer(dst, offsetBytes, data, sizeBytes, "VulkanRendererAPI::UploadBufferSubData");
            return;
        }

        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = sizeBytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] UploadBufferSubData: staging allocation failed ({} bytes)", sizeBytes);
            return;
        }
        std::memcpy(stagingOut.pMappedData, data, sizeBytes);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, sizeBytes);

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        // No per-buffer state tracking exists (ADR 0011 §1.5's GL-shaped
        // barrier model), so bracket conservatively — the CopyBufferSubData
        // spelling, minus the host-read half this direction never needs.
        const auto globalBarrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
        {
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = srcStage & m_EnabledStageMask;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage & m_EnabledStageMask;
            barrier.dstAccessMask = dstAccess;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(ctx.Cmd, &dep);
        };
        globalBarrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = offsetBytes;
        region.size = sizeBytes;
        vkCmdCopyBuffer(ctx.Cmd, staging, dst, 1, &region);

        globalBarrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);

        // The copy is consumed when the FRAME submits, so the staging buffer
        // must outlive this call (the UploadTextureSubImage2D discipline).
        VulkanDeferredReclaim::Get().Enqueue(staging, stagingAllocation);
    }

    void VulkanRendererAPI::ReadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, void* dest)
    {
        if (RefuseOnWorker("ReadBufferSubData"))
        {
            return;
        }
        if (dest == nullptr || sizeBytes == 0)
            return;

        // The production consumer is the readback-ring shape (A7): a
        // DeviceToHost raw buffer written by a PRIOR frame's CopyBufferSubData
        // whose submission has already fence-waited — the fence signal makes
        // device writes available, host-coherent memory makes them visible,
        // so the read is a plain memcpy off the persistent mapping.
        if (auto* raw = VulkanRawBufferRegistry::Get().Lookup(buffer); raw != nullptr)
        {
            if (raw->Mapped == nullptr || offsetBytes + sizeBytes > raw->Size)
            {
                OLO_CORE_WARN("[RHI/Vulkan] ReadBufferSubData: unmapped raw buffer or out-of-range read "
                              "({}+{} of {}) — zero-filled",
                              offsetBytes, sizeBytes, raw->Size);
                std::memset(dest, 0, sizeBytes);
                return;
            }
            if (!raw->Coherent)
            {
                vmaInvalidateAllocation(VulkanDevice::Get()->GetAllocator(), raw->Allocation, offsetBytes, sizeBytes);
            }
            std::memcpy(dest, static_cast<const u8*>(raw->Mapped) + offsetBytes, sizeBytes);
            return;
        }

        // Object-backed fallback: a StorageBuffer identity read through the
        // facade (GL's glGetNamedBufferSubData is buffer-generic). GetData
        // runs a blocking one-shot copy with its own availability barrier.
        if (const auto* entry = VulkanRootObjectRegistry::Get().Lookup(buffer);
            entry != nullptr && entry->Kind == VulkanRootObjectKind::StorageBuffer)
        {
            static_cast<VulkanStorageBuffer*>(entry->Object)
                ->GetData(dest, static_cast<u32>(sizeBytes), static_cast<u32>(offsetBytes));
            return;
        }

        UnimplementedStub("ReadBufferSubData(unresolvable buffer)", StubKind::PreconditionFailure);
        std::memset(dest, 0, sizeBytes);
    }

    void VulkanRendererAPI::CopyBufferSubData(RHI::ResourceHandle srcBuffer, RHI::ResourceHandle dstBuffer, u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes)
    {
        auto& ctx = Ctx();
        if (sizeBytes == 0)
            return;
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("CopyBufferSubData(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }

        auto& registry = RHI::ResourceRegistry::Get();
        const u64 srcNative = registry.KindOf(srcBuffer) == RHI::ResourceKind::Buffer
                                  ? registry.ResolveNativeForBackend(srcBuffer)
                                  : 0u;
        const u64 dstNative = registry.KindOf(dstBuffer) == RHI::ResourceKind::Buffer
                                  ? registry.ResolveNativeForBackend(dstBuffer)
                                  : 0u;
        if (srcNative == 0u || dstNative == 0u)
        {
            UnimplementedStub("CopyBufferSubData(unresolvable buffer)", StubKind::PreconditionFailure);
            return;
        }
        const auto src = reinterpret_cast<VkBuffer>(srcNative);
        const auto dst = reinterpret_cast<VkBuffer>(dstNative);

        // The copy lands in the destination's PERSISTENT buffer, so a live
        // frame-arena snapshot staged by an earlier SetData would shadow it for
        // every later draw (issue #1052 — this is the path
        // VirtualMeshRegistry's upload ring takes for the vertex arena once the
        // ring maps, so it is not hypothetical). Drop it; the buffer then
        // resolves persistent, which is where the copy went.
        if (const auto* dstRoot = VulkanRootObjectRegistry::Get().Lookup(dstBuffer);
            dstRoot != nullptr && dstRoot->Kind == VulkanRootObjectKind::StorageBuffer)
        {
            static_cast<VulkanStorageBuffer*>(dstRoot->Object)->InvalidateSnapshotForExternalWrite();
        }

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        // No per-buffer state tracking exists (ADR 0011 §1.5's GL-shaped
        // barrier model), so bracket the copy conservatively: make every
        // prior write available to the copy, then the copy's write available
        // to every later consumer INCLUDING the host (the readback ring's
        // ReadBufferSubData memcpy next frame — fence-wait handles ordering,
        // the HOST access flag makes the availability explicit for sync
        // validation).
        const auto globalBarrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
        {
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = srcStage & m_EnabledStageMask;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage & m_EnabledStageMask;
            barrier.dstAccessMask = dstAccess;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(ctx.Cmd, &dep);
        };
        globalBarrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferCopy region{};
        region.srcOffset = srcOffsetBytes;
        region.dstOffset = dstOffsetBytes;
        region.size = sizeBytes;
        vkCmdCopyBuffer(ctx.Cmd, src, dst, 1, &region);

        globalBarrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
    }

    // =========================================================================
    // The raw (object-less) texture / framebuffer facade family (#691).
    //
    // GL's shape is a bare glCreateTextures name with immutable single-mip
    // storage (OpenGLRendererAPI::CreateTexture2D) and a bare
    // glCreateFramebuffers name attachments are hung on later. Here each raw
    // handle is backed by a REAL backend object (VulkanTexture2D /
    // VulkanTextureCubemap / VulkanFramebuffer) owned by a side registry —
    // so identity, image-info metadata, barrier lowering, binds, clears and
    // the rendering scope all work on raw resources exactly as on
    // object-backed ones, and only OWNERSHIP is facade-managed.
    // =========================================================================

    namespace
    {
        struct RawTextureFormat
        {
            ImageFormat Format = ImageFormat::None;
            bool SRGB = false;
        };

        // RHI::Format -> engine ImageFormat for the raw facade family. An
        // explicit switch — ImageFormat's integer values are persisted, so no
        // static_cast bridge may ever exist (RHITypes.h's own rule). Widening
        // choices mirror ImageFormatToVkFormat's documented ones (3-channel
        // formats widen to RGBA); D32Float maps to the engine's one depth
        // member, which allocates D32_SFLOAT_S8_UINT — the depth aspect and
        // 32-bit float precision the caller asked for, with a surplus stencil
        // aspect nothing reads (the FramebufferFormatToImageFormat
        // DEPTH_COMPONENT32F precedent). Formats with no ImageFormat member
        // (R32UInt — VirtualMeshRegistry's overdraw counter) return None and
        // the create warns + returns the null handle rather than lying.
        [[nodiscard]] RawTextureFormat RawFormatToImageFormat(RHI::Format format)
        {
            switch (format)
            {
                case RHI::Format::Unknown:
                    break;
                case RHI::Format::R8UNorm:
                    return { ImageFormat::R8, false };
                case RHI::Format::R8UInt:
                    return { ImageFormat::R8UI, false };
                case RHI::Format::RG8UNorm:
                    return { ImageFormat::RG8, false };
                case RHI::Format::RGB8UNorm:
                    return { ImageFormat::RGB8, false };
                case RHI::Format::RGBA8UNorm:
                    return { ImageFormat::RGBA8, false };
                case RHI::Format::RGBA8SRGB:
                    return { ImageFormat::RGBA8, true };
                case RHI::Format::R16UInt:
                    return { ImageFormat::R16UI, false };
                case RHI::Format::RG16UInt:
                    return { ImageFormat::RG16UI, false };
                case RHI::Format::RG16Float:
                    return { ImageFormat::RG16F, false };
                case RHI::Format::RGBA16Float:
                    return { ImageFormat::RGBA16F, false };
                case RHI::Format::R32Float:
                    return { ImageFormat::R32F, false };
                case RHI::Format::R32Int:
                    return { ImageFormat::R32I, false };
                case RHI::Format::R32UInt:
                    return { ImageFormat::R32UI, false };
                case RHI::Format::RG32Float:
                    return { ImageFormat::RG32F, false };
                case RHI::Format::RGB32Float:
                    return { ImageFormat::RGB32F, false };
                case RHI::Format::RGBA32Float:
                    return { ImageFormat::RGBA32F, false };
                case RHI::Format::D24UNormS8UInt:
                    return { ImageFormat::DEPTH24STENCIL8, false };
                case RHI::Format::D32Float:
                    return { ImageFormat::DEPTH24STENCIL8, false };
                case RHI::Format::BC5UNorm:
                    return { ImageFormat::BC5, false };
                case RHI::Format::BC6HUFloat:
                    return { ImageFormat::BC6H, false };
                case RHI::Format::BC7UNorm:
                    return { ImageFormat::BC7, false };
                case RHI::Format::BC7SRGB:
                    return { ImageFormat::BC7, true };
                case RHI::Format::RGBA32UInt:
                    // No ImageFormat member yet (the R32UInt/#702 precedent):
                    // fall through to None, so the create warns and returns
                    // the null handle rather than lying.
                    break;
            }
            return {};
        }

        // Ownership side table for raw CreateVertexArrayHandle aggregates. A
        // VulkanVertexArray is a pure CPU aggregate (no GPU object) that
        // registers itself in VulkanRootObjectRegistry, so this map only OWNS
        // the Ref between create and delete — small enough to stay
        // file-local. Process-wide, deliberately leaked, render-thread only
        // (the raw-registry contract).
        [[nodiscard]] std::unordered_map<u64, Ref<VulkanVertexArray>>& RawVertexArrays()
        {
            static auto* s_Instance = new std::unordered_map<u64, Ref<VulkanVertexArray>>();
            return *s_Instance;
        }

        [[nodiscard]] u64 RawObjectKey(RHI::ResourceHandle handle)
        {
            return (static_cast<u64>(handle.Generation) << 32) | handle.Index;
        }
    } // namespace

    RHI::ResourceHandle VulkanRendererAPI::CreateTexture2DHandle(u32 width, u32 height, RHI::Format internalFormat)
    {
        if (RefuseOnWorker("CreateTexture2DHandle"))
        {
            return {};
        }
        const RawTextureFormat mapped = RawFormatToImageFormat(internalFormat);
        if (mapped.Format == ImageFormat::None)
        {
            OLO_CORE_WARN("[RHI/Vulkan] CreateTexture2DHandle: RHI::Format {} has no ImageFormat mapping — "
                          "returning the null handle",
                          static_cast<u32>(internalFormat));
            return {};
        }
        // GL parity (OpenGLRendererAPI::CreateTexture2D): immutable SINGLE-MIP
        // storage — no mip chain, no generation.
        TextureSpecification spec;
        spec.Width = std::max(width, 1u);
        spec.Height = std::max(height, 1u);
        spec.Format = mapped.Format;
        spec.SRGB = mapped.SRGB;
        spec.GenerateMips = false;
        spec.MipLevels = 1u;
        try
        {
            Ref<VulkanTexture2D> texture = Ref<VulkanTexture2D>::Create(spec);
            if (texture == nullptr || texture->GetVkImage() == VK_NULL_HANDLE)
            {
                return {};
            }
            return VulkanRawTextureRegistry::Get().Adopt(std::move(texture));
        }
        catch (const std::exception& e)
        {
            // GL's create cannot fail at the call site; the closest honest
            // answer is the null handle every caller already guards on.
            OLO_CORE_ERROR("[RHI/Vulkan] CreateTexture2DHandle({}x{}) failed: {}", width, height, e.what());
            return {};
        }
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateTextureCubemapHandle(u32 width, u32 height, RHI::Format internalFormat)
    {
        if (RefuseOnWorker("CreateTextureCubemapHandle"))
        {
            return {};
        }
        const RawTextureFormat mapped = RawFormatToImageFormat(internalFormat);
        if (mapped.Format == ImageFormat::None)
        {
            OLO_CORE_WARN("[RHI/Vulkan] CreateTextureCubemapHandle: RHI::Format {} has no ImageFormat mapping — "
                          "returning the null handle",
                          static_cast<u32>(internalFormat));
            return {};
        }
        // Same GL parity as the 2D form: immutable single-mip storage.
        // (CubemapSpecification has no SRGB member; the sRGB raw-cubemap case
        // has no caller — DDGI's black cubemap is RGBA16F.)
        CubemapSpecification spec;
        spec.Width = std::max(width, 1u);
        spec.Height = std::max(height, 1u);
        spec.Format = mapped.Format;
        spec.GenerateMips = false;
        spec.MipLevels = 1u;
        try
        {
            Ref<VulkanTextureCubemap> cubemap = Ref<VulkanTextureCubemap>::Create(spec);
            if (cubemap == nullptr || cubemap->GetVkImage() == VK_NULL_HANDLE)
            {
                return {};
            }
            return VulkanRawTextureRegistry::Get().Adopt(std::move(cubemap));
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] CreateTextureCubemapHandle({}x{}) failed: {}", width, height, e.what());
            return {};
        }
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateFramebufferHandle()
    {
        if (RefuseOnWorker("CreateFramebufferHandle"))
        {
            return {};
        }
        // A real VulkanFramebuffer with an EMPTY spec: zero attachments until
        // AttachFramebufferColor/DepthTexture installs the raw textures, and
        // a 0x0 extent the first attach adopts. The constructor registers it
        // in VulkanRootObjectRegistry, which is what makes BindFramebuffer,
        // SetFramebufferDrawAttachments, the facade clears, blits and the
        // rendering scope work UNCHANGED for raw framebuffers.
        try
        {
            Ref<VulkanFramebuffer> framebuffer = Ref<VulkanFramebuffer>::Create(FramebufferSpecification{});
            return VulkanRawFramebufferRegistry::Get().Adopt(std::move(framebuffer));
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] CreateFramebufferHandle failed: {}", e.what());
            return {};
        }
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateBufferHandle()
    {
        if (RefuseOnWorker("CreateBufferHandle"))
        {
            return {};
        }
        return VulkanRawBufferRegistry::Get().CreateHandle();
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateVertexArrayHandle()
    {
        if (RefuseOnWorker("CreateVertexArrayHandle"))
        {
            return {};
        }
        // A real (empty) CPU-side aggregate: the identity mint is what the
        // callers need (VirtualMeshRegistry holds the handle; the native-
        // identity test round-trips it), and BindVertexArrayRaw resolves it
        // through the root-object registry like any VAO. A draw through an
        // empty aggregate fails loudly in PrepareDraw (no pull stream) —
        // honest, since the facade has no SetVertexArrayIndexBuffer lowering
        // yet.
        Ref<VulkanVertexArray> vertexArray = Ref<VulkanVertexArray>::Create();
        const RHI::ResourceHandle handle = vertexArray->GetRHIHandle();
        if (!handle.IsValid())
        {
            return {};
        }
        RawVertexArrays()[RawObjectKey(handle)] = std::move(vertexArray);
        return handle;
    }

    void VulkanRendererAPI::DeleteTexture(RHI::ResourceHandle texture)
    {
        if (RefuseOnWorker("DeleteTexture"))
        {
            return;
        }
        // Kind-guarded like the GL twin — a live handle of the wrong family
        // names someone else's resource.
        if (RHI::ResourceRegistry::Get().KindOf(texture) != RHI::ResourceKind::Texture)
        {
            return;
        }
        if (VulkanRawTextureRegistry::Get().Destroy(texture))
        {
            return;
        }
        // Snapshot clones (CreateMatchingTextureHandle, #810) live in their own
        // owner table — they are a bare {VkImage, VmaAllocation}, not a
        // VulkanTexture2D.
        if (VulkanRawImageRegistry::Get().Destroy(texture))
        {
            return;
        }
        // Compare-off view alias (CreateDepthArrayCompareOffViewHandle):
        // ShadowMap retires these through this entry on GL, where the view
        // is a real second texture. Here the alias is a cached second
        // handle to a SOURCE-owned image — honor the deletion by retiring
        // the alias identity and dropping the cache row, so the next
        // Create call after a shadow-map re-init mints a fresh alias
        // instead of resolving a retired one. No native object dies (the
        // source owns the image), so this is the complete lowering.
        for (auto it = m_CompareOffViewHandles.begin(); it != m_CompareOffViewHandles.end(); ++it)
        {
            if (it->second == texture)
            {
                RHI::ResourceRegistry::Get().Unregister(it->second);
                m_CompareOffViewHandles.erase(it);
                return;
            }
        }

        // Genuinely foreign: an object-owned texture reached the raw-delete
        // entry. Its C++ object owns the destruction; deleting here would
        // double-free. Warn — unlike the alias case this is a caller bug.
        static std::atomic<bool> s_WarnedForeign{ false };
        if (!s_WarnedForeign.exchange(true, std::memory_order_relaxed))
        {
            OLO_CORE_WARN("[RHI/Vulkan] DeleteTexture on an object-owned texture handle — the owner destroys the "
                          "image; ignored (warn-once)");
        }
    }

    void VulkanRendererAPI::NotifyFramebufferDestroyed(const VulkanFramebuffer* framebuffer,
                                                       RHI::ResourceHandle handle)
    {
        auto& ctx = Ctx();
        EndScopeIfTargets(handle);
        // The depth-array selection names a view the dying framebuffer owns
        // (#806): drop it from every context so no later scope can attach it.
        if (handle.IsValid())
        {
            const u64 key = SelectionKey(handle);
            m_Main.Selections.erase(key);
            for (const auto& item : m_Items)
            {
                item->Selections.erase(key);
            }
        }
        if (ctx.Pending.Target == framebuffer)
        {
            // Materializing later would dereference the freed object; the
            // clear dies with its target, exactly as GL FBO state does.
            ctx.Pending = PendingClear{};
        }
    }

    void VulkanRendererAPI::DeleteFramebuffer(RHI::ResourceHandle framebuffer)
    {
        if (RefuseOnWorker("DeleteFramebuffer"))
        {
            return;
        }
        auto& ctx = Ctx();
        if (RHI::ResourceRegistry::Get().KindOf(framebuffer) != RHI::ResourceKind::Framebuffer)
        {
            return;
        }
        // A scope still targeting this framebuffer must not outlive it (no-op
        // outside a recording bracket); the stored draw-buffer selection dies
        // with the object like GL's FBO state does.
        EndScopeIfTargets(framebuffer);
        ctx.Selections.erase(SelectionKey(framebuffer));
        if (VulkanRawFramebufferRegistry::Get().Destroy(framebuffer))
        {
            return;
        }
        static std::atomic<bool> s_WarnedForeign{ false };
        if (!s_WarnedForeign.exchange(true, std::memory_order_relaxed))
        {
            OLO_CORE_WARN("[RHI/Vulkan] DeleteFramebuffer on a non-raw-registry handle (object-owned framebuffer) "
                          "— no-op (warn-once)");
        }
    }

    void VulkanRendererAPI::DeleteBuffer(RHI::ResourceHandle buffer)
    {
        if (RefuseOnWorker("DeleteBuffer"))
        {
            return;
        }
        // Raw-handle family only: object-backed buffers die with their C++
        // object. Kind-guarded like the GL twin — a live handle of the wrong
        // family names someone else's resource.
        if (RHI::ResourceRegistry::Get().KindOf(buffer) != RHI::ResourceKind::Buffer)
            return;
        if (VulkanRawBufferRegistry::Get().Lookup(buffer) == nullptr)
        {
            UnimplementedStub("DeleteBuffer(not a raw-registry buffer)", StubKind::PreconditionFailure);
            return;
        }
        VulkanRawBufferRegistry::Get().Destroy(buffer);
    }

    void VulkanRendererAPI::DeleteVertexArray(RHI::ResourceHandle vertexArray)
    {
        if (RefuseOnWorker("DeleteVertexArray"))
        {
            return;
        }
        auto& ctx = Ctx();
        // Raw-handle family only (CreateVertexArrayHandle mints these);
        // object-backed VAOs die with their C++ object. Kind-guarded like
        // DeleteBuffer/DeleteTexture. Also drop the bound-VAO cache if it
        // points at the dying aggregate (GL unbinds a deleted bound VAO).
        if (RHI::ResourceRegistry::Get().KindOf(vertexArray) != RHI::ResourceKind::VertexArray)
        {
            return;
        }
        auto& rawVaos = RawVertexArrays();
        const auto it = rawVaos.find(RawObjectKey(vertexArray));
        if (it == rawVaos.end())
        {
            UnimplementedStub("DeleteVertexArray(not a raw-registry vertex array)", StubKind::PreconditionFailure);
            return;
        }
        if (ctx.BoundVertexArray == it->second.Raw())
        {
            ctx.BoundVertexArray = nullptr;
        }
        rawVaos.erase(it);
    }

    void VulkanRendererAPI::SetVertexArrayIndexBuffer(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indexBuffer)
    {
        if (RefuseOnWorker("SetVertexArrayIndexBuffer"))
        {
            return;
        }
        // #1052 — and the design question this issue was really about.
        //
        // ADR 0011 §5 decides that vertex input layout is "not baked at all":
        // vertex shaders fetch through a device-address pointer instead of
        // VkPipelineVertexInputStateCreateInfo. That reads like it removes this
        // entry point along with the rest of vertex-input state. It does not.
        // The index buffer is not part of VkGraphicsPipelineCreateInfo in the
        // first place — it is bound with vkCmdBindIndexBuffer, plain command
        // state, contributing zero PSO permutation axes — so §5's "remove the
        // axis entirely" cannot apply to it, and no ADR contract changes here.
        // BindIndexBufferFor has been doing exactly this for object-backed VAOs
        // since #691; all that was missing is the RAW aggregate's half.
        //
        // (The alternative — feeding the hardware MDI arm from the same index
        // SSBO the software raster reads — was rejected:
        // vkCmdDrawIndexedIndirectCount REQUIRES a bound index buffer, so it
        // would mean rewriting VirtualMeshRegistry's DrawElementsIndirectCommand
        // records into a non-indexed encoding and forking VG's GL and Vulkan
        // draw paths, which is what the raw facade exists to avoid.)
        const auto* entry = VulkanRootObjectRegistry::Get().Lookup(vertexArray);
        if (entry == nullptr || entry->Kind != VulkanRootObjectKind::VertexArray)
        {
            UnimplementedStub("SetVertexArrayIndexBuffer(unresolvable vertex array)", StubKind::PreconditionFailure);
            return;
        }
        auto* vao = static_cast<VulkanVertexArray*>(entry->Object);
        if (!indexBuffer.IsValid())
        {
            vao->SetRawIndexBuffer({}); // glVertexArrayElementBuffer(vao, 0) detaches
            return;
        }
        // Raw-handle family only: an object-backed index buffer reaches a VAO
        // through VertexArray::SetIndexBuffer, which owns a Ref. Accepting one
        // here would record a handle nothing keeps alive.
        if (RHI::ResourceRegistry::Get().KindOf(indexBuffer) != RHI::ResourceKind::Buffer ||
            VulkanRawBufferRegistry::Get().Lookup(indexBuffer) == nullptr)
        {
            UnimplementedStub("SetVertexArrayIndexBuffer(not a raw-registry buffer)", StubKind::PreconditionFailure);
            return;
        }
        vao->SetRawIndexBuffer(indexBuffer);
    }

    void VulkanRendererAPI::UploadTextureSubImage2D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset, u32 width, u32 height, RHI::Format sourceFormat, const void* data)
    {
        if (RefuseOnWorker("UploadTextureSubImage2D"))
        {
            return;
        }
        auto& ctx = Ctx();
        // #691: the frame-command-buffer staged upload. GL's
        // glTextureSubImage2D is ordered against everything the frame already
        // issued; recording the copy HERE (not a one-shot, which submits
        // BEFORE the still-recording frame) preserves that ordering for
        // mid-frame callers (terrain sculpt/paint region flushes, the DDGI
        // probe-data tile writes).
        // Dual-routed like the 1c family: with NO recording (pass Init at
        // load time, headless fixtures) the one-shot arm below is both safe
        // and required — the ColorGrading identity LUT was the first caller
        // to hit the bracket-free gap.
        if (data == nullptr || width == 0u || height == 0u)
        {
            return;
        }
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        if (native == 0u)
        {
            UnimplementedStub("UploadTextureSubImage2D(unresolved texture)", StubKind::PreconditionFailure);
            return;
        }
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr)
        {
            UnimplementedStub("UploadTextureSubImage2D(unregistered image)", StubKind::PreconditionFailure);
            return;
        }

        // GL's pixel-transfer converts client data into the internal format
        // driver-side; vkCmdCopyBufferToImage is a raw texel copy, so the CPU
        // converts. Identity plus the pairs the engine actually uploads.
        std::vector<u8> converted;
        const void* uploadData = data;
        u64 uploadSize = 0;
        const u64 texelCount = static_cast<u64>(width) * height;
        const VkFormat srcAsVk = VulkanBarrierLowering::ToVkFormat(sourceFormat);
        if (srcAsVk == info->Format)
        {
            const u32 bpp = ClientBytesPerPixel(sourceFormat);
            if (bpp == 0u)
            {
                return;
            }
            uploadSize = texelCount * bpp;
        }
        else if (sourceFormat == RHI::Format::RG32Float && info->Format == VK_FORMAT_R16G16_SFLOAT)
        {
            // SSAO's noise import shape.
            converted.resize(texelCount * sizeof(u32));
            const auto* src = static_cast<const f32*>(data);
            auto* dst = reinterpret_cast<u32*>(converted.data());
            for (u64 i = 0; i < texelCount; ++i)
            {
                dst[i] = glm::packHalf2x16(glm::vec2(src[i * 2u], src[i * 2u + 1u]));
            }
            uploadData = converted.data();
            uploadSize = converted.size();
        }
        else if (sourceFormat == RHI::Format::RGBA32Float && info->Format == VK_FORMAT_R16G16B16A16_SFLOAT)
        {
            // DDGI's probe-data tile shape.
            converted.resize(texelCount * 2u * sizeof(u32));
            const auto* src = static_cast<const f32*>(data);
            auto* dst = reinterpret_cast<u32*>(converted.data());
            for (u64 i = 0; i < texelCount; ++i)
            {
                dst[i * 2u] = glm::packHalf2x16(glm::vec2(src[i * 4u], src[i * 4u + 1u]));
                dst[i * 2u + 1u] = glm::packHalf2x16(glm::vec2(src[i * 4u + 2u], src[i * 4u + 3u]));
            }
            uploadData = converted.data();
            uploadSize = converted.size();
        }
        else if (sourceFormat == RHI::Format::RGB8UNorm &&
                 (info->Format == VK_FORMAT_R8G8B8A8_UNORM || info->Format == VK_FORMAT_R8G8B8A8_SRGB))
        {
            // stb-style tightly-packed RGB widened to the RGBA image.
            converted.resize(texelCount * 4u);
            const auto* src = static_cast<const u8*>(data);
            for (u64 i = 0; i < texelCount; ++i)
            {
                converted[i * 4u] = src[i * 3u];
                converted[i * 4u + 1u] = src[i * 3u + 1u];
                converted[i * 4u + 2u] = src[i * 3u + 2u];
                converted[i * 4u + 3u] = 0xFFu;
            }
            uploadData = converted.data();
            uploadSize = converted.size();
        }
        else if (sourceFormat == RHI::Format::RGB32Float && info->Format == VK_FORMAT_R32G32B32A32_SFLOAT)
        {
            // RGB32F textures store as RGBA32F on this backend (the
            // ExpandRgbToRgba widening) — same widening for client data.
            converted.resize(texelCount * 4u * sizeof(f32));
            const auto* src = static_cast<const f32*>(data);
            auto* dst = reinterpret_cast<f32*>(converted.data());
            for (u64 i = 0; i < texelCount; ++i)
            {
                dst[i * 4u] = src[i * 3u];
                dst[i * 4u + 1u] = src[i * 3u + 1u];
                dst[i * 4u + 2u] = src[i * 3u + 2u];
                dst[i * 4u + 3u] = 1.0f;
            }
            uploadData = converted.data();
            uploadSize = converted.size();
        }
        else
        {
            static std::unordered_set<u32> s_WarnedPairs;
            if (s_WarnedPairs.insert((static_cast<u32>(sourceFormat) << 16) | static_cast<u32>(info->Format)).second)
            {
                OLO_CORE_WARN("[RHI/Vulkan] UploadTextureSubImage2D: no conversion from source format {} to image "
                              "format {} — upload skipped",
                              static_cast<u32>(sourceFormat), static_cast<u32>(info->Format));
            }
            return;
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return;
        }
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = uploadSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] UploadTextureSubImage2D: staging allocation failed ({} bytes)", uploadSize);
            return;
        }
        std::memcpy(stagingOut.pMappedData, uploadData, uploadSize);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, uploadSize);

        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        region.imageOffset = { xOffset, yOffset, 0 };
        region.imageExtent = { width, height, 1u };

        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            // No recording: the blocking one-shot (the SetFaceDataMip shape).
            // Whole-image transitions keep the tracked layout uniform, and
            // the image settles into SHADER_READ_ONLY for the bind paths.
            const VkImageLayout priorLayout = info->InitialLayout;
            const u32 mipCount = std::max(info->MipLevels, 1u);
            const u32 layerCount = std::max(info->ArrayLayers, 1u);
            const bool ok = VulkanOneShot::Submit(
                "VulkanRendererAPI::UploadTextureSubImage2D",
                [&](VkCommandBuffer cmd)
                {
                    VkImageMemoryBarrier2 toDst{};
                    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    toDst.srcStageMask =
                        priorLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : kAllStages;
                    toDst.srcAccessMask =
                        priorLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : kAllAccess;
                    toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    toDst.oldLayout = priorLayout;
                    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toDst.image = image;
                    toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, mipCount, 0u, layerCount };
                    VkDependencyInfo oneShotDep{};
                    oneShotDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                    oneShotDep.imageMemoryBarrierCount = 1u;
                    oneShotDep.pImageMemoryBarriers = &toDst;
                    vkCmdPipelineBarrier2(cmd, &oneShotDep);

                    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);

                    VkImageMemoryBarrier2 toRead = toDst;
                    toRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    toRead.dstStageMask = kAllStages;
                    toRead.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
                    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    oneShotDep.pImageMemoryBarriers = &toRead;
                    vkCmdPipelineBarrier2(cmd, &oneShotDep);
                });
            vmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);
            if (ok)
            {
                VulkanImageInfoRegistry::Get().SetInitialLayout(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            return;
        }

        // Transfer commands are illegal inside a dynamic-rendering scope.
        EndRenderingScope();

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0u;
        range.levelCount = 1u;
        range.baseArrayLayer = 0u;
        range.layerCount = 1u;
        std::vector<VkImageMemoryBarrier2> toTransfer;
        StageTransferTransition(image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                toTransfer);
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<u32>(toTransfer.size());
        dep.pImageMemoryBarriers = toTransfer.data();
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);

        vkCmdCopyBufferToImage(ctx.Cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);

        // Left in TRANSFER_DST with the tracker in agreement — the bind-time
        // visibility seam transitions produced runs to SHADER_READ_ONLY at
        // the next sample (the ClearTextureFloat discipline).

        // The copy is consumed when the FRAME submits — the staging buffer
        // must outlive it, so it takes the deferred-reclaim queue, not an
        // inline destroy.
        VulkanDeferredReclaim::Get().Enqueue(staging, stagingAllocation);
    }

    void VulkanRendererAPI::UploadTextureSubImage3D(RHI::ResourceHandle /*texture*/, i32 /*xOffset*/, i32 /*yOffset*/, i32 /*zOffset*/, u32 /*width*/, u32 /*height*/, u32 /*depth*/, RHI::Format /*sourceFormat*/, const void* /*data*/)
    {
        if (RefuseOnWorker("UploadTextureSubImage3D"))
        {
            return;
        }
        // Deliberately not lowered (#691): the engine's ONE caller is
        // OceanFFTGpu's twiddle-index seed, and the ocean FFT chain is not a
        // Vulkan tenant yet. When it becomes one, follow the
        // UploadTextureSubImage2D staged pattern with depth as
        // imageExtent.depth. Still counted as a stub hit so the pass-suite
        // instruments see the fall-through if a Vulkan caller ever appears.
        UnimplementedStub("UploadTextureSubImage3D(no Vulkan-reachable caller — OceanFFTGpu only)");
    }

    namespace
    {
        // Native texel width for the formats the readback path can decode.
        // 0 = undecodable (block-compressed, packed depth-stencil, exotic).
        [[nodiscard]] u32 ReadbackTexelBytes(const VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_R8_UNORM:
                case VK_FORMAT_R8_UINT:
                    return 1u;
                case VK_FORMAT_R8G8_UNORM:
                case VK_FORMAT_R16_SFLOAT:
                    return 2u;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB:
                case VK_FORMAT_R16G16_SFLOAT:
                case VK_FORMAT_R32_SFLOAT:
                case VK_FORMAT_R32_SINT:
                case VK_FORMAT_R32_UINT:
                case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                case VK_FORMAT_D32_SFLOAT:
                case VK_FORMAT_D32_SFLOAT_S8_UINT: // depth aspect copies as 4-byte floats
                case VK_FORMAT_D24_UNORM_S8_UINT:  // depth aspect: one D24 per 32-bit word
                    return 4u;
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                case VK_FORMAT_R32G32_SFLOAT:
                    return 8u;
                case VK_FORMAT_R32G32B32A32_SFLOAT:
                    return 16u;
                default:
                    return 0u;
            }
        }

        // Decode one native texel to float components. sRGB stays raw
        // (glGetTextureImage's behaviour — no linearization on readback).
        [[nodiscard]] bool DecodeReadbackTexel(const VkFormat format, const u8* texel, glm::vec4& out)
        {
            const auto unorm = [](const u8 v)
            { return static_cast<f32>(v) / 255.0f; };
            switch (format)
            {
                case VK_FORMAT_R8_UNORM:
                    out = { unorm(texel[0]), 0.0f, 0.0f, 1.0f };
                    return true;
                case VK_FORMAT_R8G8_UNORM:
                    out = { unorm(texel[0]), unorm(texel[1]), 0.0f, 1.0f };
                    return true;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                    out = { unorm(texel[0]), unorm(texel[1]), unorm(texel[2]), unorm(texel[3]) };
                    return true;
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB:
                    out = { unorm(texel[2]), unorm(texel[1]), unorm(texel[0]), unorm(texel[3]) };
                    return true;
                case VK_FORMAT_R16_SFLOAT:
                {
                    u16 bits;
                    std::memcpy(&bits, texel, sizeof(bits));
                    out = { glm::unpackHalf2x16(bits).x, 0.0f, 0.0f, 1.0f };
                    return true;
                }
                case VK_FORMAT_R16G16_SFLOAT:
                {
                    u32 bits;
                    std::memcpy(&bits, texel, sizeof(bits));
                    const glm::vec2 rg = glm::unpackHalf2x16(bits);
                    out = { rg.x, rg.y, 0.0f, 1.0f };
                    return true;
                }
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                {
                    u32 lo;
                    u32 hi;
                    std::memcpy(&lo, texel, sizeof(lo));
                    std::memcpy(&hi, texel + 4, sizeof(hi));
                    const glm::vec2 rg = glm::unpackHalf2x16(lo);
                    const glm::vec2 ba = glm::unpackHalf2x16(hi);
                    out = { rg.x, rg.y, ba.x, ba.y };
                    return true;
                }
                case VK_FORMAT_R32_SFLOAT:
                case VK_FORMAT_D32_SFLOAT:
                case VK_FORMAT_D32_SFLOAT_S8_UINT:
                    std::memcpy(&out.x, texel, sizeof(f32));
                    out.y = 0.0f;
                    out.z = 0.0f;
                    out.w = 1.0f;
                    return true;
                case VK_FORMAT_R32G32_SFLOAT:
                    std::memcpy(&out.x, texel, 2 * sizeof(f32));
                    out.z = 0.0f;
                    out.w = 1.0f;
                    return true;
                case VK_FORMAT_R32G32B32A32_SFLOAT:
                    std::memcpy(&out.x, texel, 4 * sizeof(f32));
                    return true;
                case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                {
                    u32 bits;
                    std::memcpy(&bits, texel, sizeof(bits));
                    out = { glm::unpackF2x11_1x10(bits), 1.0f };
                    return true;
                }
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                {
                    u32 bits;
                    std::memcpy(&bits, texel, sizeof(bits));
                    out = glm::unpackUnorm3x10_1x2(bits);
                    return true;
                }
                case VK_FORMAT_D24_UNORM_S8_UINT:
                {
                    // Depth-aspect copy layout: D24 in the low bits of each
                    // 32-bit word, upper byte undefined (Vulkan spec 20.4).
                    u32 bits;
                    std::memcpy(&bits, texel, sizeof(bits));
                    out = { static_cast<f32>(bits & 0xFFFFFFu) / 16777215.0f, 0.0f, 0.0f, 1.0f };
                    return true;
                }
                default:
                    return false;
            }
        }

        // Encode float components into the caller's destination format.
        [[nodiscard]] bool EncodeReadbackTexel(const RHI::Format format, const glm::vec4& texel, u8* dest)
        {
            const auto toU8 = [](const f32 v)
            { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            switch (format)
            {
                case RHI::Format::R32Float:
                // A depth readback names D32Float as its destination on both
                // backends — GL because only a depth dest maps to
                // GL_DEPTH_COMPONENT, and here because the identity path only
                // fires when the image really is VK_FORMAT_D32_SFLOAT (this
                // hardware backs the graph's Depth24Stencil8 with
                // D32_SFLOAT_S8_UINT, so the decode path has to serve it).
                // The depth plane already decoded to texel.x, so this is the
                // same single float R32Float writes.
                case RHI::Format::D32Float:
                    std::memcpy(dest, &texel.x, sizeof(f32));
                    return true;
                case RHI::Format::RG32Float:
                    std::memcpy(dest, &texel.x, 2 * sizeof(f32));
                    return true;
                case RHI::Format::RGB32Float:
                    std::memcpy(dest, &texel.x, 3 * sizeof(f32));
                    return true;
                case RHI::Format::RGBA32Float:
                    std::memcpy(dest, &texel.x, 4 * sizeof(f32));
                    return true;
                case RHI::Format::RGB8UNorm:
                    dest[0] = toU8(texel.x);
                    dest[1] = toU8(texel.y);
                    dest[2] = toU8(texel.z);
                    return true;
                case RHI::Format::RGBA8UNorm:
                    dest[0] = toU8(texel.x);
                    dest[1] = toU8(texel.y);
                    dest[2] = toU8(texel.z);
                    dest[3] = toU8(texel.w);
                    return true;
                default:
                    return false;
            }
        }
    } // namespace

    bool VulkanRendererAPI::ReadTextureImage(RHI::ResourceHandle texture, u32 mipLevel, RHI::Format destFormat, sizet destSizeBytes, void* dest)
    {
        if (RefuseOnWorker("ReadTextureImage"))
        {
            return false;
        }
        // GL's glGetTextureImage: whole level, dimensions answered by the
        // object. The image-info registry carries the mip-0 extent; a
        // pre-extent registration (Width == 0) cannot size the level, so the
        // read reports failure rather than guessing (the callers all handle
        // false).
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        const auto* info = native != 0u ? VulkanImageInfoRegistry::Get().Lookup(reinterpret_cast<VkImage>(native))
                                        : nullptr;
        if (info == nullptr || info->Width == 0u || info->Height == 0u)
        {
            UnimplementedStub("ReadTextureImage(unresolved/extent-less image)", StubKind::PreconditionFailure);
            return false;
        }
        const u32 mipW = std::max(info->Width >> mipLevel, 1u);
        const u32 mipH = std::max(info->Height >> mipLevel, 1u);
        return ReadTextureSubImage(texture, mipLevel, 0, 0, 0, mipW, mipH, std::max(info->ArrayLayers, 1u),
                                   destFormat, destSizeBytes, dest);
    }

    bool VulkanRendererAPI::ReadTextureSubImage(RHI::ResourceHandle texture, u32 mipLevel, i32 x, i32 y, i32 z, u32 width, u32 height, u32 depth, RHI::Format destFormat, sizet destSizeBytes, void* dest)
    {
        if (RefuseOnWorker("ReadTextureSubImage"))
        {
            return false;
        }
        auto& ctx = Ctx();
        // glGetTextureSubImage, lowered as a blocking one-shot
        // copy-image-to-buffer plus a CPU-side format conversion (GL's
        // pixel-transfer conversion happens driver-side; vkCmdCopyImageToBuffer
        // is a raw texel copy). This is the MCP diagnostics readback spine:
        // olo_screenshot / olo_render_capture_target, ThumbnailCapture, the
        // probe bakers and entity picking all funnel through here (#691
        // Phase 8b).
        if (dest == nullptr || width == 0u || height == 0u || depth == 0u)
        {
            return false;
        }
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        if (native == 0u)
        {
            UnimplementedStub("ReadTextureSubImage(unresolved texture)", StubKind::PreconditionFailure);
            return false;
        }
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr || mipLevel >= std::max(info->MipLevels, 1u))
        {
            UnimplementedStub("ReadTextureSubImage(unregistered image / bad mip)", StubKind::PreconditionFailure);
            return false;
        }

        const u32 destBpp = ClientBytesPerPixel(destFormat);
        const u64 texelCount = static_cast<u64>(width) * height * depth;
        if (destBpp == 0u || destSizeBytes < texelCount * destBpp)
        {
            OLO_CORE_WARN("[RHI/Vulkan] ReadTextureSubImage: destination too small or format unsized "
                          "({} bytes for {} texels of format {})",
                          destSizeBytes, texelCount, static_cast<u32>(destFormat));
            return false;
        }
        const u32 nativeBpp = ReadbackTexelBytes(info->Format);
        if (nativeBpp == 0u)
        {
            OLO_CORE_WARN("[RHI/Vulkan] ReadTextureSubImage: undecodable native format {} — readback refused",
                          static_cast<u32>(info->Format));
            return false;
        }

        // Mid-frame read: pending writes to this image may still sit in the
        // recording frame command buffer — the StorageBuffer::GetData rule.
        // Flush and wait so the read sees THIS frame's writes.
        //
        // A REFUSED flush (the backbuffer was already written this frame) is
        // not a "read anyway" licence: the one-shot below would execute
        // BEFORE the still-recording frame command buffer (amendment 72) and
        // its layout transitions would contradict the layouts that buffer was
        // recorded against — validation reported exactly that
        // ("expects SHADER_READ_ONLY_OPTIMAL — instead, current layout is
        // UNDEFINED") and the frame came back with only the skybox drawn.
        // So a refused flush switches the read into BORROW mode: transition
        // from the tracked layout, copy, transition straight BACK, and leave
        // the layout tracker untouched. The frame CB then executes against
        // exactly the state it was recorded for, and the caller gets the
        // PREVIOUS frame's contents — which is precisely what GL's
        // double-buffered PBO entity-pick read returns too.
        bool borrowLayout = false;
        if (ctx.Cmd != VK_NULL_HANDLE)
        {
            auto* context = VulkanContext::Get();
            if (context == nullptr || !context->FlushFrameRecordingAndWait())
            {
                borrowLayout = true;
                // TRACE: borrow mode IS the designed path for a post-blit
                // read (the viewport entity pick) — previous-frame contents
                // are exactly what GL's double-buffered PBO pick returns.
                static bool s_Traced = false;
                if (!s_Traced)
                {
                    s_Traced = true;
                    OLO_CORE_TRACE("[Vulkan] mid-frame ReadTextureSubImage in borrow mode — previous-frame "
                                   "contents, GL's double-buffered pick semantics (trace-once)");
                }
            }
        }

        // z picks the array layer for layered images and the depth slice for
        // volumes (glGetTextureSubImage semantics). Multi-layer reads pack
        // layer-major, matching GL.
        const bool isVolume = info->ViewType == VK_IMAGE_VIEW_TYPE_3D;
        const u32 baseLayer = isVolume ? 0u : static_cast<u32>(std::max(z, 0));
        const u32 layerCount = isVolume ? 1u : depth;
        const i32 zOffset = isVolume ? z : 0;
        const u32 depthExtent = isVolume ? depth : 1u;
        if (!isVolume && baseLayer + layerCount > std::max(info->ArrayLayers, 1u))
        {
            return false;
        }
        const VkImageAspectFlags aspect = info->HasDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        if (info->HasDepth && info->Format != VK_FORMAT_D32_SFLOAT &&
            info->Format != VK_FORMAT_D32_SFLOAT_S8_UINT && info->Format != VK_FORMAT_D24_UNORM_S8_UINT)
        {
            OLO_CORE_WARN("[RHI/Vulkan] ReadTextureSubImage: unreadable depth format {}",
                          static_cast<u32>(info->Format));
            return false;
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return false;
        }
        const u64 stagedSize = texelCount * nativeBpp;
        VkBufferCreateInfo readbackInfo{};
        readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        readbackInfo.size = stagedSize;
        readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo readbackAlloc{};
        readbackAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        readbackAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo readbackOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &readbackInfo, &readbackAlloc, &readback, &readbackAllocation,
                            &readbackOut) != VK_SUCCESS)
        {
            return false;
        }

        // The BARRIER range must name every aspect a combined depth/stencil
        // image has (VUID-VkImageMemoryBarrier2-image-03320 — without
        // separateDepthStencilLayouts both transition together); only the
        // COPY below selects the depth plane alone.
        const VkImageAspectFlags barrierAspect =
            info->HasStencil ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : aspect;
        const VkImageSubresourceRange range{ barrierAspect, mipLevel, 1u, baseLayer, layerCount };

        // Borrow mode needs ONE layout to hand back. A mixed range has no
        // single answer, and transitioning from UNDEFINED discards the very
        // content the read wants — refuse instead of returning garbage.
        VkImageLayout borrowedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (borrowLayout)
        {
            // The EXECUTED layout, not the recorded one: this one-shot is
            // submitted immediately and therefore runs BEFORE the frame command
            // buffer whose transitions the recorded layout already reflects
            // (issue #800). On a steady frame the two are equal; on the frame
            // an attachment is (re)created — a window resize — the recorded
            // layout already reads SHADER_READ_ONLY while the image has never
            // been touched by submitted work and is still UNDEFINED. Barriering
            // from the recorded layout there is what produced one
            // VUID-vkCmdDraw-None-09600 per resize.
            borrowedLayout = ctx.Tracker.CurrentExecutedLayout(image, range);
            if (borrowedLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            {
                vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
                return false;
            }
        }

        const bool ok = VulkanOneShot::Submit(
            "VulkanRendererAPI::ReadTextureSubImage",
            [&](VkCommandBuffer cmd)
            {
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                std::vector<VkImageMemoryBarrier2> toSrc;
                VkImageMemoryBarrier2 borrowIn{};
                if (borrowLayout)
                {
                    // Tracker untouched: this transition is undone below.
                    borrowIn.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    borrowIn.srcStageMask = kAllStages;
                    borrowIn.srcAccessMask = kAllAccess;
                    borrowIn.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    borrowIn.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                    borrowIn.oldLayout = borrowedLayout;
                    borrowIn.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    borrowIn.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    borrowIn.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    borrowIn.image = image;
                    borrowIn.subresourceRange = range;
                    dep.imageMemoryBarrierCount = 1u;
                    dep.pImageMemoryBarriers = &borrowIn;
                }
                else
                {
                    // Exact-oldLayout transitions per tracked layout run (the
                    // CopyImageSubData discipline), so attachments read
                    // correctly whatever layout the last pass left them in.
                    StageTransferTransition(image, range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            VK_ACCESS_2_TRANSFER_READ_BIT, toSrc);
                    dep.imageMemoryBarrierCount = static_cast<u32>(toSrc.size());
                    dep.pImageMemoryBarriers = toSrc.data();
                }
                vkCmdPipelineBarrier2(cmd, &dep);

                VkBufferImageCopy region{};
                region.imageSubresource = { aspect, mipLevel, baseLayer, layerCount };
                region.imageOffset = { x, y, zOffset };
                region.imageExtent = { width, height, depthExtent };
                vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);

                // Borrow mode hands the layout straight back (and leaves the
                // tracker alone); otherwise settle into the sampled steady
                // state and record it, so the next graph execution
                // transitions from truth.
                const VkImageLayout settleLayout =
                    borrowLayout ? borrowedLayout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                VkImageMemoryBarrier2 toRead{};
                toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                toRead.dstStageMask = kAllStages;
                toRead.dstAccessMask = borrowLayout ? kAllAccess : VK_ACCESS_2_MEMORY_READ_BIT;
                toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toRead.newLayout = settleLayout;
                toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toRead.image = image;
                toRead.subresourceRange = range;
                dep.imageMemoryBarrierCount = 1u;
                dep.pImageMemoryBarriers = &toRead;
                vkCmdPipelineBarrier2(cmd, &dep);
                if (!borrowLayout)
                {
                    ctx.Tracker.SetLayout(image, range, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            });

        bool converted = false;
        if (ok)
        {
            vmaInvalidateAllocation(device->GetAllocator(), readbackAllocation, 0, stagedSize);
            const auto* staged = static_cast<const u8*>(readbackOut.pMappedData);
            if (info->Format == VulkanBarrierLowering::ToVkFormat(destFormat))
            {
                // Identity (covers the integer formats — entity picking's
                // R32I read — which the decode path deliberately excludes).
                std::memcpy(dest, staged, texelCount * destBpp);
                converted = true;
            }
            else
            {
                auto* destBytes = static_cast<u8*>(dest);
                converted = true;
                for (u64 i = 0; i < texelCount; ++i)
                {
                    glm::vec4 texel{};
                    if (!DecodeReadbackTexel(info->Format, staged + i * nativeBpp, texel) ||
                        !EncodeReadbackTexel(destFormat, texel, destBytes + i * destBpp))
                    {
                        OLO_CORE_WARN("[RHI/Vulkan] ReadTextureSubImage: no conversion {} -> {}",
                                      static_cast<u32>(info->Format), static_cast<u32>(destFormat));
                        converted = false;
                        break;
                    }
                }
            }
        }
        vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
        return ok && converted;
    }

    void VulkanRendererAPI::GetTextureDimensions(RHI::ResourceHandle texture, u32 mipLevel, u32& outWidth, u32& outHeight)
    {
        // GL answers from glGetTextureLevelParameteriv; here the image-info
        // registry carries the mip-0 extent (#691). GL's contract for
        // a missing level is 0, so the outputs are zeroed on every failure
        // path — callers already treat 0 as "no answer".
        outWidth = 0u;
        outHeight = 0u;
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        if (native == 0u)
        {
            UnimplementedStub("GetTextureDimensions(unresolved texture)", StubKind::PreconditionFailure);
            return;
        }
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(reinterpret_cast<VkImage>(native));
        if (info == nullptr || info->Width == 0u)
        {
            UnimplementedStub("GetTextureDimensions(image without a registered extent)");
            return;
        }
        if (mipLevel >= info->MipLevels)
        {
            return; // past the chain — GL reports 0
        }
        outWidth = std::max(info->Width >> mipLevel, 1u);
        outHeight = std::max(info->Height >> mipLevel, 1u);
    }

    bool VulkanRendererAPI::QueryTextureFormat(RHI::ResourceHandle texture, u32 mipLevel,
                                               RHI::TextureFormatInfo& out)
    {
        // Keyed on the VULKAN format from the image-info registry, never on the
        // render graph's format label — ADR 0011 amendment (79): the backend
        // satisfies the graph's Depth24Stencil8 with D32_SFLOAT_S8_UINT here,
        // so a table keyed on the label would decode the wrong number of bytes.
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(texture);
        if (native == 0u)
        {
            return false;
        }
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(reinterpret_cast<VkImage>(native));
        if (info == nullptr || info->Width == 0u || mipLevel >= std::max(info->MipLevels, 1u))
        {
            return false;
        }

        RHI::TextureFormatInfo described;
        described.Native = static_cast<u64>(info->Format);
        const u32 mipLevels = std::max(info->MipLevels, 1u);
        const u32 arrayLayers = std::max(info->ArrayLayers, 1u);
        RHI::TextureShape shape = RHI::TextureShape::Unknown;
        switch (info->ViewType)
        {
            case VK_IMAGE_VIEW_TYPE_2D:
                // Multisample is a SHAPE here exactly as it is on GL, so a
                // consumer that must refuse one (the snapshot clone) sees it
                // through the same field on both backends.
                shape = info->Samples > 1u ? RHI::TextureShape::Texture2DMultisample
                                           : RHI::TextureShape::Texture2D;
                break;
            case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
                shape = RHI::TextureShape::Texture2DArray;
                break;
            case VK_IMAGE_VIEW_TYPE_CUBE:
                shape = RHI::TextureShape::TextureCube;
                break;
            case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
                shape = RHI::TextureShape::TextureCubeArray;
                break;
            case VK_IMAGE_VIEW_TYPE_3D:
                shape = RHI::TextureShape::Texture3D;
                break;
            default:
                break;
        }

        // Tokens match the OpenGL arm's spelling for the same format, which is
        // what makes a cross-backend A/B readable without a translation table.
        switch (info->Format)
        {
            case VK_FORMAT_R8G8B8A8_UNORM:
                described = { RHI::Format::RGBA8UNorm, described.Native, "RGBA8", 4, false, false, false };
                break;
            case VK_FORMAT_R8G8B8A8_SRGB:
                described = { RHI::Format::RGBA8SRGB, described.Native, "RGBA8_SRGB", 4, false, false, false };
                break;
            case VK_FORMAT_B8G8R8A8_UNORM:
                described = { RHI::Format::Unknown, described.Native, "BGRA8", 4, false, false, false };
                break;
            case VK_FORMAT_B8G8R8A8_SRGB:
                described = { RHI::Format::Unknown, described.Native, "BGRA8_SRGB", 4, false, false, false };
                break;
            case VK_FORMAT_R8G8_UNORM:
                described = { RHI::Format::RG8UNorm, described.Native, "RG8", 2, false, false, false };
                break;
            case VK_FORMAT_R8_UNORM:
                described = { RHI::Format::R8UNorm, described.Native, "R8", 1, false, false, false };
                break;
            case VK_FORMAT_R8_UINT:
                described = { RHI::Format::R8UInt, described.Native, "R8UI", 1, true, false, false };
                break;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                described = { RHI::Format::RGBA16Float, described.Native, "RGBA16F", 4, false, false, true };
                break;
            case VK_FORMAT_R32G32B32A32_SFLOAT:
                described = { RHI::Format::RGBA32Float, described.Native, "RGBA32F", 4, false, false, true };
                break;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                described = { RHI::Format::Unknown, described.Native, "R11F_G11F_B10F", 3, false, false, true };
                break;
            case VK_FORMAT_R16G16_SFLOAT:
                described = { RHI::Format::RG16Float, described.Native, "RG16F", 2, false, false, true };
                break;
            case VK_FORMAT_R32G32_SFLOAT:
                described = { RHI::Format::RG32Float, described.Native, "RG32F", 2, false, false, true };
                break;
            case VK_FORMAT_R16_SFLOAT:
                described = { RHI::Format::Unknown, described.Native, "R16F", 1, false, false, true };
                break;
            case VK_FORMAT_R32_SFLOAT:
                described = { RHI::Format::R32Float, described.Native, "R32F", 1, false, false, true };
                break;
            case VK_FORMAT_R32_SINT:
                described = { RHI::Format::R32Int, described.Native, "R32I", 1, true, false, false };
                break;
            case VK_FORMAT_R32_UINT:
                described = { RHI::Format::R32UInt, described.Native, "R32UI", 1, true, false, false };
                break;
            case VK_FORMAT_D16_UNORM:
                described = { RHI::Format::Unknown, described.Native, "D16", 1, false, true, false };
                break;
            case VK_FORMAT_D32_SFLOAT:
                described = { RHI::Format::D32Float, described.Native, "D32F", 1, false, true, true };
                break;
            case VK_FORMAT_D24_UNORM_S8_UINT:
                described = { RHI::Format::D24UNormS8UInt, described.Native, "D24S8", 1, false, true, false };
                break;
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                described = { RHI::Format::Unknown, described.Native, "D32FS8", 1, false, true, true };
                break;
            default:
                return false;
        }

        described.MipLevels = mipLevels;
        described.ArrayLayers = arrayLayers;
        described.Shape = shape;
        out = described;
        return true;
    }

    RHI::ResourceHandle VulkanRendererAPI::CreateMatchingTextureHandle(RHI::ResourceHandle source)
    {
        if (RefuseOnWorker("CreateMatchingTextureHandle"))
        {
            return {};
        }
        // Reproduce the SOURCE's own VkImageCreateInfo — no neutral format
        // vocabulary in the middle (RendererAPI.h explains why). A render-graph
        // target can hold a VkFormat the engine's ImageFormat enum cannot name,
        // and vkCmdCopyImage requires format compatibility, so a translated
        // near-miss would produce a garbage clone rather than an error.
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(source);
        if (native == 0u)
        {
            UnimplementedStub("CreateMatchingTextureHandle(unresolved source)", StubKind::PreconditionFailure);
            return {};
        }
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(reinterpret_cast<VkImage>(native));
        if (info == nullptr || info->Width == 0u || info->Height == 0u)
        {
            UnimplementedStub("CreateMatchingTextureHandle(source without a registered extent)");
            return {};
        }
        if (info->Samples > 1u)
        {
            // The facade contract (RendererAPI.h) promises the null handle for
            // a multisampled source, and the GL arm already refuses one. A
            // copy destination must match VkImageCreateInfo::samples, so
            // cloning this as single-sample would produce an image
            // vkCmdCopyImage rejects — an empty clone the tools would then
            // report as the pass's output.
            OLO_CORE_WARN("[RHI/Vulkan] CreateMatchingTextureHandle: source is multisampled ({} samples) — refused",
                          info->Samples);
            return {};
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return {};
        }

        const bool isVolume = info->ViewType == VK_IMAGE_VIEW_TYPE_3D;
        const bool isCube = info->ViewType == VK_IMAGE_VIEW_TYPE_CUBE ||
                            info->ViewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        const u32 mipLevels = std::max(info->MipLevels, 1u);
        const u32 arrayLayers = std::max(info->ArrayLayers, 1u);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = isVolume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        imageInfo.format = info->Format;
        imageInfo.extent = { info->Width, info->Height, isVolume ? arrayLayers : 1u };
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = isVolume ? 1u : arrayLayers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_DST for the mid-frame copy in, TRANSFER_SRC for the readback
        // out — and SAMPLED, which is NOT about sampling.
        //
        // Usage flags have to satisfy the contract of every API the image is
        // handed to, not just this code's intent for it. ReadTextureSubImage
        // settles what it reads into SHADER_READ_ONLY_OPTIMAL, and that layout
        // is only legal on an image created with SAMPLED (or INPUT_ATTACHMENT)
        // — VUID-VkImageMemoryBarrier2-oldLayout-01211. Omitting it produced
        // five validation errors per capture on the first live Vulkan run,
        // with correct pixels and green tests either way, which is exactly the
        // class of defect only the validation layer reports.
        //
        // Still no STORAGE/ATTACHMENT: nothing writes a clone through a
        // descriptor, so it stays clear of the sRGB and multisample
        // storage-image restrictions the texture classes reason about.
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (isCube)
        {
            imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

        VkImage clone = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        if (vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &clone, &allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_WARN("[RHI/Vulkan] CreateMatchingTextureHandle: vmaCreateImage failed ({}x{} format {})",
                          info->Width, info->Height, static_cast<u32>(info->Format));
            return {};
        }
        vmaSetAllocationName(device->GetAllocator(), allocation, "SnapshotClone");

        // Register the clone exactly like an object-backed texture: the barrier
        // translator derives aspect masks from this, the layout tracker seeds
        // from it, and ReadTextureSubImage refuses anything it cannot find here.
        VulkanImageInfoRegistry::Get().Register(clone, VulkanImageInfo{
                                                           .Format = info->Format,
                                                           .Width = info->Width,
                                                           .Height = info->Height,
                                                           .MipLevels = mipLevels,
                                                           .ArrayLayers = arrayLayers,
                                                           .HasDepth = info->HasDepth,
                                                           .HasStencil = info->HasStencil,
                                                           .ViewType = info->ViewType,
                                                       });

        const RHI::ResourceHandle handle = VulkanRawImageRegistry::Get().Adopt(clone, allocation);
        if (!handle.IsValid())
        {
            VulkanDeferredReclaim::Get().Enqueue(clone, allocation);
        }
        return handle;
    }

    void VulkanRendererAPI::TextureBarrier()
    {
        auto& ctx = Ctx();
        // glTextureBarrier orders framebuffer writes against texture fetches
        // OF THE SAME TEXTURE (the VirtualGeometryPass phase-2 shape: draw
        // depth, then sample it for the Hi-Z rebuild). The call carries no
        // resource identity, so it lowers exactly like the flags-only
        // MemoryBarrier: end the rendering scope (attachment writes retire
        // with it) + one conservative global barrier. Layout transitions stay
        // the callers' IssueBarrierBatch/tracker business — GL's glTextureBarrier
        // has no layout concept either, so no caller expects one here.
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("TextureBarrier(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
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
        vkCmdPipelineBarrier2(ctx.Cmd, &dep);
    }

    // =========================================================================
    // Occlusion queries (#691, ADR item A6)
    //
    // GL hands out N independent query names; Vulkan has one VkQueryPool with N
    // slots, so VulkanQueryRegistry (declared in the header) makes ONE pool per
    // CreateQueries call and each handle names a (pool, slot) pair.
    //
    // Three Vulkan-only disciplines the GL twin has no equivalent of:
    //
    //  1. RESET. A slot must be reset before it is written, and
    //     vkCmdResetQueryPool is illegal inside a render pass instance — so
    //     BeginQuery ends the lazy rendering scope first. The reset is per-slot
    //     and immediately before the write, which is what keeps the PREVIOUS
    //     frame's results readable in the double-buffered pool (a blanket
    //     reset-at-BeginRecording would wipe the half OcclusionQueryPool::
    //     BeginFrame is about to read).
    //  2. SCOPE SPAN. vkCmdEndQuery must sit in the same render pass instance
    //     as its vkCmdBeginQuery. Both therefore run with the scope ENDED: the
    //     proxy draws in between open (and close) their own instance, which is
    //     legal precisely because the query began outside one. The re-open uses
    //     LOAD_OP_LOAD, so splitting the instance is a cost, not a content
    //     change — the same trade every barrier/copy/dispatch already makes.
    //  3. READ SAFETY. vkGetQueryPoolResults under WAIT on a slot that was
    //     reset but never written blocks forever, and reading a never-reset
    //     slot is undefined. `Entry::Recorded` gates both: nothing is read
    //     until a Begin/End pair has actually reached a command buffer.
    // =========================================================================

    VulkanQueryRegistry& VulkanQueryRegistry::Get()
    {
        // Deliberately leaked, like the other process-wide side tables.
        static auto* instance = new VulkanQueryRegistry();
        return *instance;
    }

    void VulkanQueryRegistry::CreatePool(RHI::QueryType type, std::span<RHI::ResourceHandle> outQueries)
    {
        std::ranges::fill(outQueries, RHI::NullResource);
        if (outQueries.empty())
        {
            return;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr || device->GetDevice() == VK_NULL_HANDLE)
        {
            OLO_CORE_WARN("[RHI/Vulkan] CreateQueries with no live device — {} queries left null", outQueries.size());
            return;
        }

        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        // Slot layout per query KIND (#691): occlusion and plain
        // timestamps are one slot per handle; a TimeElapsed handle owns a PAIR
        // of timestamp slots (begin at 2i, end at 2i+1) because Vulkan has no
        // native elapsed query — Begin/EndQuery bracket it with two
        // vkCmdWriteTimestamp2 stamps and readback subtracts.
        const bool elapsedPair = type == RHI::QueryType::TimeElapsed;
        info.queryType = type == RHI::QueryType::OcclusionAnySamples ? VK_QUERY_TYPE_OCCLUSION
                                                                     : VK_QUERY_TYPE_TIMESTAMP;
        const u32 slotsPerQuery = elapsedPair ? 2u : 1u;
        info.queryCount = static_cast<u32>(outQueries.size()) * slotsPerQuery;
        VkQueryPool pool = VK_NULL_HANDLE;
        if (vkCreateQueryPool(device->GetDevice(), &info, nullptr, &pool) != VK_SUCCESS || pool == VK_NULL_HANDLE)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] vkCreateQueryPool failed for {} queries", outQueries.size());
            return;
        }

        auto& registry = RHI::ResourceRegistry::Get();
        u32 live = 0;
        for (sizet i = 0; i < outQueries.size(); ++i)
        {
            // The registry native is the POOL, so a generic native resolve of a
            // query handle names the object it belongs to rather than garbage.
            const RHI::ResourceHandle handle =
                registry.Register(RHI::ResourceKind::Query, std::bit_cast<u64>(pool), RHI::Backend::Vulkan);
            if (!handle.IsValid())
            {
                OLO_CORE_ERROR("[RHI/Vulkan] resource registry full — query {} left null", i);
                continue;
            }
            outQueries[i] = handle;
            m_Entries.emplace(Key(handle), Entry{ .Pool = pool,
                                                  .Index = static_cast<u32>(i) * slotsPerQuery,
                                                  .Recorded = false,
                                                  .Type = type });
            ++live;
        }
        if (live == 0)
        {
            vkDestroyQueryPool(device->GetDevice(), pool, nullptr);
            return;
        }
        m_Pools.push_back(Pool{ .Handle = pool, .Count = info.queryCount, .LiveQueries = live });
    }

    VulkanQueryRegistry::Entry* VulkanQueryRegistry::Lookup(RHI::ResourceHandle handle)
    {
        if (!handle.IsValid())
        {
            return nullptr;
        }
        const auto it = m_Entries.find(Key(handle));
        return it == m_Entries.end() ? nullptr : &it->second;
    }

    void VulkanQueryRegistry::Destroy(std::span<const RHI::ResourceHandle> queries)
    {
        auto& registry = RHI::ResourceRegistry::Get();
        for (const RHI::ResourceHandle handle : queries)
        {
            const auto it = m_Entries.find(Key(handle));
            if (it == m_Entries.end())
            {
                continue; // foreign / already-retired handle — the GL twin no-ops too
            }
            const VkQueryPool pool = it->second.Pool;
            m_Entries.erase(it);
            registry.Unregister(handle);

            const auto poolIt = std::ranges::find_if(m_Pools, [pool](const Pool& p)
                                                     { return p.Handle == pool; });
            if (poolIt == m_Pools.end())
            {
                continue;
            }
            if (--poolIt->LiveQueries == 0)
            {
                // In-flight command buffers may still name the pool
                // (vkCmdResetQueryPool / vkCmdBeginQuery) — same generation
                // discipline as pipelines and attachment views.
                VulkanDeferredReclaim::Get().Enqueue(pool);
                m_Pools.erase(poolIt);
            }
        }
    }

    void VulkanQueryRegistry::ReleaseAll()
    {
        auto& registry = RHI::ResourceRegistry::Get();
        for (const auto& [key, entry] : m_Entries)
        {
            // Rebuild the handle from the map key (generation << 32 | index).
            RHI::ResourceHandle handle{};
            handle.Index = static_cast<u32>(key & 0xFFFFFFFFu);
            handle.Generation = static_cast<u32>(key >> 32);
            registry.Unregister(handle);
        }
        m_Entries.clear();
        auto* device = VulkanDevice::Get();
        for (const Pool& pool : m_Pools)
        {
            if (device != nullptr && device->GetDevice() != VK_NULL_HANDLE)
            {
                vkDestroyQueryPool(device->GetDevice(), pool.Handle, nullptr);
            }
        }
        m_Pools.clear();
    }

    void VulkanRendererAPI::CreateQueries(RHI::QueryType type, std::span<RHI::ResourceHandle> outQueries)
    {
        if (RefuseOnWorker("CreateQueries"))
        {
            return;
        }
        VulkanQueryRegistry::Get().CreatePool(type, outQueries);
    }

    void VulkanRendererAPI::DeleteQueries(std::span<const RHI::ResourceHandle> queries)
    {
        if (RefuseOnWorker("DeleteQueries"))
        {
            return;
        }
        VulkanQueryRegistry::Get().Destroy(queries);
    }

    void VulkanRendererAPI::BeginQuery(RHI::QueryType /*type*/, RHI::ResourceHandle query)
    {
        if (RefuseOnWorker("BeginQuery"))
        {
            return;
        }
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("BeginQuery(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        auto* entry = VulkanQueryRegistry::Get().Lookup(query);
        if (entry == nullptr)
        {
            UnimplementedStub("BeginQuery(unresolved query)", StubKind::PreconditionFailure);
            return;
        }
        if (ctx.Query.Pool != VK_NULL_HANDLE)
        {
            // GL keeps one active query per target and errors on nesting; do
            // the same loudly rather than record an illegal command buffer.
            OLO_CORE_WARN("[RHI/Vulkan] BeginQuery while another query is active — ignored");
            return;
        }
        if (entry->Type == RHI::QueryType::Timestamp)
        {
            OLO_CORE_WARN("[RHI/Vulkan] BeginQuery on a Timestamp query — timestamps are stamped via "
                          "WriteTimestamp, never bracketed; ignored");
            return;
        }
        // Discipline 1 + 2 (see the block comment above): both the reset and
        // the begin/stamp must sit outside a render pass instance.
        EndRenderingScope();
        if (entry->Type == RHI::QueryType::TimeElapsed)
        {
            // The elapsed pair: reset both slots, stamp the begin timestamp
            // now; EndQuery stamps Index+1 and only then marks Recorded —
            // reading a pair whose end was never stamped would block forever
            // under WAIT.
            vkCmdResetQueryPool(ctx.Cmd, entry->Pool, entry->Index, 2u);
            vkCmdWriteTimestamp2(ctx.Cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, entry->Pool, entry->Index);
            ctx.Query = { .Pool = entry->Pool, .Index = entry->Index, .IsElapsed = true, .Handle = query };
            return;
        }
        vkCmdResetQueryPool(ctx.Cmd, entry->Pool, entry->Index, 1u);
        vkCmdBeginQuery(ctx.Cmd, entry->Pool, entry->Index, 0u);
        ctx.Query = { .Pool = entry->Pool, .Index = entry->Index, .IsElapsed = false, .Handle = query };
        entry->Recorded = true;
    }

    void VulkanRendererAPI::EndQuery(RHI::QueryType /*type*/)
    {
        if (RefuseOnWorker("EndQuery"))
        {
            return;
        }
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("EndQuery(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        if (ctx.Query.Pool == VK_NULL_HANDLE)
        {
            // GL's glEndQuery with nothing active is an error, not a crash.
            return;
        }
        EndRenderingScope();
        if (ctx.Query.IsElapsed)
        {
            vkCmdWriteTimestamp2(ctx.Cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ctx.Query.Pool,
                                 ctx.Query.Index + 1u);
            // Re-look-up rather than caching the Entry* across the bracket —
            // the registry map can rehash between Begin and End.
            if (auto* entry = VulkanQueryRegistry::Get().Lookup(ctx.Query.Handle); entry != nullptr)
            {
                entry->Recorded = true;
            }
            ctx.Query = {};
            return;
        }
        vkCmdEndQuery(ctx.Cmd, ctx.Query.Pool, ctx.Query.Index);
        ctx.Query = {};
    }

    void VulkanRendererAPI::WriteTimestamp(RHI::ResourceHandle query)
    {
        if (RefuseOnWorker("WriteTimestamp"))
        {
            return;
        }
        auto& ctx = Ctx();
        if (ctx.Cmd == VK_NULL_HANDLE)
        {
            UnimplementedStub("WriteTimestamp(outside recording bracket)", StubKind::OutsideRecording);
            return;
        }
        auto* entry = VulkanQueryRegistry::Get().Lookup(query);
        if (entry == nullptr)
        {
            UnimplementedStub("WriteTimestamp(unresolved query)", StubKind::PreconditionFailure);
            return;
        }
        if (entry->Type != RHI::QueryType::Timestamp)
        {
            OLO_CORE_WARN("[RHI/Vulkan] WriteTimestamp on a non-Timestamp query — ignored");
            return;
        }
        // Reset + stamp, both outside a render pass instance. The slot is
        // rewritten every ring cycle by its pool owner (GPUPassTimerPool's
        // 4-slot ring); an unread result that old was already dropped by the
        // owner's own staleness handling, same overwrite semantics as GL.
        EndRenderingScope();
        vkCmdResetQueryPool(ctx.Cmd, entry->Pool, entry->Index, 1u);
        vkCmdWriteTimestamp2(ctx.Cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, entry->Pool, entry->Index);
        entry->Recorded = true;
    }

    bool VulkanRendererAPI::ReadQueryResult(RHI::ResourceHandle query, bool wait, u64& outValue) const
    {
        outValue = 0;
        auto* entry = VulkanQueryRegistry::Get().Lookup(query);
        auto* device = VulkanDevice::Get();
        if (entry == nullptr || !entry->Recorded || device == nullptr || device->GetDevice() == VK_NULL_HANDLE)
        {
            // A stale handle (or one never written) has no result and never
            // will. Reporting "available" would make the caller read a zero and
            // treat it as a real answer — for occlusion that reads as "fully
            // occluded", which silently deletes geometry (the GL twin's note).
            return false;
        }

        // Per-kind readback (#691): occlusion reads its single slot
        // raw; Timestamp reads one timestamp slot and scales ticks →
        // nanoseconds; TimeElapsed reads its PAIR (availability keyed on the
        // END slot — the last one stamped, so its availability implies the
        // begin's) and returns the scaled difference. The nanosecond contract
        // is the facade's (RendererAPI::WriteTimestamp): GL timestamps are
        // nanoseconds natively, so the pools' subtraction math stays
        // backend-blind.
        const bool isElapsed = entry->Type == RHI::QueryType::TimeElapsed;
        const u32 slotCount = isElapsed ? 2u : 1u;

        const auto finish = [&](const u64* slots) -> bool
        {
            switch (entry->Type)
            {
                case RHI::QueryType::OcclusionAnySamples:
                    outValue = slots[0];
                    return true;
                case RHI::QueryType::Timestamp:
                    outValue = static_cast<u64>(static_cast<f64>(slots[0]) * m_TimestampPeriodNs);
                    return true;
                case RHI::QueryType::TimeElapsed:
                {
                    const u64 ticks = slots[1] > slots[0] ? slots[1] - slots[0] : 0u;
                    outValue = static_cast<u64>(static_cast<f64>(ticks) * m_TimestampPeriodNs);
                    return true;
                }
            }
            return false;
        };

        // Availability first, never blocking: this is both the answer for
        // IsQueryResultAvailable and the guard that keeps the WAIT read below
        // off a slot whose submission has not been made yet. Layout: pairs of
        // (value, availability) per slot.
        std::array<u64, 4> probe{ 0u, 0u, 0u, 0u };
        const VkResult status = vkGetQueryPoolResults(
            device->GetDevice(), entry->Pool, entry->Index, slotCount, sizeof(u64) * 2u * slotCount, probe.data(),
            sizeof(u64) * 2u, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (status == VK_SUCCESS && probe[(slotCount - 1u) * 2u + 1u] != 0u)
        {
            const std::array<u64, 2> values{ probe[0], probe[2] };
            return finish(values.data());
        }
        if (!wait)
        {
            return false;
        }
        std::array<u64, 2> values{ 0u, 0u };
        if (vkGetQueryPoolResults(device->GetDevice(), entry->Pool, entry->Index, slotCount, sizeof(u64) * slotCount,
                                  values.data(), sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        {
            return false;
        }
        return finish(values.data());
    }

    bool VulkanRendererAPI::IsQueryResultAvailable(RHI::ResourceHandle query)
    {
        // Timestamp scaling reads m_TimestampPeriodNs, which Init() caches —
        // and a query can be read on an instance whose Init was skipped (the
        // headless/test shapes, and the pre-window RecreateForSelectedBackend
        // instance). CacheDeviceLimits early-outs once cached, so this is the
        // same no-op-on-the-steady-path guard IssueBarrierBatch uses. Without
        // it the period stays at its 1.0 default and every timestamp reads as
        // ticks — invisible on NVIDIA (period IS 1.0 ns there) and wrong
        // everywhere else, which is the worst possible way to be wrong.
        CacheDeviceLimits();
        u64 ignored = 0;
        return ReadQueryResult(query, false, ignored);
    }

    u32 VulkanRendererAPI::GetQueryResultU32(RHI::ResourceHandle query)
    {
        CacheDeviceLimits(); // the timestamp period — see IsQueryResultAvailable
        u64 value = 0;
        // GL's glGetQueryObjectuiv(GL_QUERY_RESULT) blocks until the result is
        // there; the WAIT read is the parity (Recorded gates the deadlock).
        (void)ReadQueryResult(query, true, value);
        return static_cast<u32>(std::min<u64>(value, std::numeric_limits<u32>::max()));
    }

    u64 VulkanRendererAPI::GetQueryResultU64(RHI::ResourceHandle query)
    {
        CacheDeviceLimits(); // the timestamp period — see IsQueryResultAvailable
        u64 value = 0;
        (void)ReadQueryResult(query, true, value);
        return value;
    }

    // --- Fences (the u64-token facade) -------------------------------------
    //
    // GL's glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE) is "a token that becomes
    // signalled once everything submitted so far has completed"; the ONE
    // production caller (FrameResourceManager::EndFrame, per frame) uses it to
    // know when a frame's transient allocations are safe to recycle. Leaving
    // it stubbed made EndFrame fail EVERY frame in a live session.
    //
    // The timeline semaphore already models exactly that, so the token IS a
    // VulkanGpuFence with a staged queue-signal of 1: the next vkQueueSubmit2
    // — the frame's own submit, since EndFrame runs inside the frame-render
    // callback — drains it (DrainPendingSubmitOps) and signals the timeline
    // when that submission completes. Ownership is raw because the facade's
    // currency is a u64 the caller stores and later destroys; DestroyFence is
    // the matching delete.
    u64 VulkanRendererAPI::CreateFence()
    {
        if (RefuseOnWorker("CreateFence"))
        {
            return 0u;
        }
        auto* fence = new VulkanGpuFence(0u);
        if (fence->GetNativeSemaphore() == VK_NULL_HANDLE)
        {
            delete fence;
            return 0u;
        }
        fence->QueueSignal(1u, RHI::FenceSignalOp::Set);
        return reinterpret_cast<u64>(fence);
    }

    RHI::FenceStatus VulkanRendererAPI::ClientWaitFence(const u64 fence, const u64 timeoutNanoseconds)
    {
        if (fence == 0u)
        {
            return RHI::FenceStatus::Failed;
        }
        auto* gpuFence = reinterpret_cast<VulkanGpuFence*>(fence);
        if (gpuFence->CompletedValue() >= 1u)
        {
            return RHI::FenceStatus::AlreadySignaled;
        }
        return gpuFence->HostWait(1u, timeoutNanoseconds, RHI::FenceCompareOp::GreaterEqual)
                   ? RHI::FenceStatus::ConditionSatisfied
                   : RHI::FenceStatus::TimeoutExpired;
    }

    bool VulkanRendererAPI::IsFenceSignaled(const u64 fence)
    {
        if (fence == 0u)
        {
            return false;
        }
        return reinterpret_cast<VulkanGpuFence*>(fence)->CompletedValue() >= 1u;
    }

    void VulkanRendererAPI::DestroyFence(const u64 fence)
    {
        if (RefuseOnWorker("DestroyFence"))
        {
            return;
        }
        if (fence == 0u)
        {
            return;
        }
        delete reinterpret_cast<VulkanGpuFence*>(fence);
    }

    void VulkanRendererAPI::SetProgramUniformFloat(RHI::ResourceHandle /*program*/, std::string_view /*name*/, f32 /*value*/)
    {
        UnimplementedStub("SetProgramUniformFloat");
    }

    // =====================================================================
    // Parallel command recording (issue #806, ADR 0011 amendment (92))
    // =====================================================================

    bool VulkanRendererAPI::RefuseOnWorker(const char* entryPoint) const
    {
        if (!OnWorkerContext())
        {
            return false;
        }
        // Debug: loud at the call site. Otherwise: the warn-once stub path,
        // so the refusal is counted and named, never silent.
        OLO_CORE_ASSERT(false, "{} called from a RecordParallel item (amendment (92) rule 7)", entryPoint);
        UnimplementedStub(entryPoint, StubKind::PreconditionFailure);
        return true;
    }

    void VulkanRendererAPI::SetFramebufferDepthArraySelection(
        const RHI::ResourceHandle framebuffer,
        const VulkanRecordingContext::FramebufferAttachmentSelection::DepthArrayLayer& selection)
    {
        if (!framebuffer.IsValid())
        {
            return;
        }
        auto& ctx = Ctx();
        // The next scope must open against the new view: a live scope on
        // this framebuffer holds the PREVIOUS layer's (ScopeMatchesCurrentTarget
        // compares selections, so the draw front-end would end it anyway —
        // ending it here keeps a clear that follows on the correct target).
        if (ctx.Scope.Active && ctx.Scope.Target != nullptr && ctx.Scope.Target->GetRHIHandle() == framebuffer &&
            ctx.Scope.DepthArrayView != (selection.Active ? selection.View : VK_NULL_HANDLE))
        {
            EndRenderingScope();
        }
        ctx.Selections[SelectionKey(framebuffer)].DepthArray = selection;
    }

    VulkanRecordingContext::FramebufferAttachmentSelection::DepthArrayLayer
    VulkanRendererAPI::GetFramebufferDepthArraySelection(const RHI::ResourceHandle framebuffer) const
    {
        if (!framebuffer.IsValid())
        {
            return {};
        }
        const auto& ctx = Ctx();
        const auto it = ctx.Selections.find(SelectionKey(framebuffer));
        return it != ctx.Selections.end() ? it->second.DepthArray
                                          : VulkanRecordingContext::FramebufferAttachmentSelection::DepthArrayLayer{};
    }

    void VulkanRendererAPI::ForgetDepthArrayView(const VkImageView view)
    {
        if (view == VK_NULL_HANDLE)
        {
            return;
        }
        const auto forget = [view](VulkanRecordingContext& context)
        {
            for (auto& [key, selection] : context.Selections)
            {
                if (selection.DepthArray.View == view)
                {
                    selection.DepthArray = {};
                }
            }
        };
        forget(m_Main);
        for (const auto& item : m_Items)
        {
            forget(*item);
        }
    }

    bool VulkanRendererAPI::SupportsParallelRecording() const
    {
        if (OnWorkerContext() || m_InParallelRegion)
        {
            return false; // no nesting
        }
        if (m_Main.Cmd == VK_NULL_HANDLE || VulkanDevice::Get() == nullptr)
        {
            return false;
        }
        if (Levers::VulkanParallelRecording() == Levers::Tristate::Off)
        {
            return false;
        }
        // A query span on the primary cannot contain vkCmdExecuteCommands
        // without occlusion-query inheritance, and a host-side conditional
        // predicate is per context; both are rare enough to run inline.
        if (m_Main.Query.Pool != VK_NULL_HANDLE || m_Main.ConditionalRenderSkip)
        {
            return false;
        }
        // The pools reset on the frame arena's clock (rule 9): no frame, no fork.
        return VulkanFrameArena::Get().GetFrameGeneration() != 0u;
    }

    void VulkanRendererAPI::TransitionBoundTargetAttachmentsForFork()
    {
        auto& ctx = Ctx();
        auto* target = VulkanBindingState::Get().GetCurrentFramebuffer();
        if (target == nullptr || ctx.Cmd == VK_NULL_HANDLE)
        {
            return; // nothing bound (the backbuffer is refused on items anyway)
        }
        // The same barriers EnsureRenderingScopeForDraw would collect for
        // this target, recorded on the primary WITHOUT opening a scope, so
        // every item's own scope-open on the same target is an identity
        // transition (amendment (92) rule 5).
        std::vector<RHI::Barrier>& barriers = m_ForkBarriers;
        barriers.clear();
        const FramebufferAttachmentSelection* selection = FindSelection(target->GetRHIHandle());
        const bool identity = selection == nullptr || selection->DrawListCount == 0;
        const u32 outputCount = identity ? std::min<u32>(target->GetColorAttachmentCount(), 8u)
                                         : std::min<u32>(selection->DrawListCount, 8u);
        for (u32 i = 0; i < outputCount; ++i)
        {
            const u32 attachmentIndex = identity ? i : selection->DrawList[i];
            if (attachmentIndex == RHI::NoAttachment || attachmentIndex >= target->GetColorAttachmentCount())
            {
                continue;
            }
            const Ref<VulkanTexture2D> image = target->GetColorAttachmentImage(attachmentIndex);
            if (image == nullptr)
            {
                continue;
            }
            RHI::Barrier toAttachment{};
            toAttachment.Resource = image->GetRHIHandle();
            toAttachment.Range.BaseMip = 0u;
            toAttachment.Range.MipCount = 1u;
            toAttachment.Range.BaseLayer = 0u;
            toAttachment.Range.LayerCount = 1u;
            const VkImageSubresourceRange probe{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
            toAttachment.Before = AccessGuessForLayout(ctx.Tracker.CurrentLayout(image->GetVkImage(), probe));
            toAttachment.After = RHI::Access::ColorAttachmentWrite;
            barriers.push_back(toAttachment);
        }
        const auto depthArray = GetFramebufferDepthArraySelection(target->GetRHIHandle());
        const Ref<VulkanTexture2D> depthImage = depthArray.Active ? nullptr : target->GetDepthAttachmentImage();
        if (depthArray.Active || depthImage != nullptr)
        {
            const VkImage depthVkImage = depthArray.Active ? depthArray.Image : depthImage->GetVkImage();
            const u32 depthLayer = depthArray.Active ? depthArray.Layer : 0u;
            RHI::Barrier toDepth{};
            toDepth.Resource = depthArray.Active ? depthArray.Handle : depthImage->GetRHIHandle();
            toDepth.Range.BaseMip = 0u;
            toDepth.Range.MipCount = 1u;
            toDepth.Range.BaseLayer = depthLayer;
            toDepth.Range.LayerCount = 1u;
            const VkImageSubresourceRange depthProbe{ VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, depthLayer, 1u };
            toDepth.Before = AccessGuessForLayout(ctx.Tracker.CurrentLayout(depthVkImage, depthProbe));
            toDepth.After = RHI::Access::DepthStencilAttachmentWrite;
            barriers.push_back(toDepth);
        }
        if (!barriers.empty())
        {
            IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ barriers });
        }
    }

    void VulkanRendererAPI::RecordParallelItem(VulkanRecordingContext& item,
                                               const std::function<void(u32 item)>& body,
                                               std::exception_ptr& firstFailure, std::mutex& failureMutex)
    {
        const auto start = std::chrono::steady_clock::now();
        {
            // Everything the body records now resolves to `item`.
            const ScopedVulkanWorkerContext scope(&item);
            try
            {
                body(item.ItemIndex);
            }
            catch (...)
            {
                const std::scoped_lock lock(failureMutex);
                if (!firstFailure)
                {
                    firstFailure = std::current_exception();
                }
            }
            // Close the item the way EndRecording closes a frame: the scope,
            // then a clear nothing consumed.
            EndRenderingScope();
            MaterializePendingClear();
        }
        if (const VkResult result = vkEndCommandBuffer(item.Cmd); result != VK_SUCCESS)
        {
            // The commands are in an unusable buffer; the item's work is lost
            // for this frame and the log says so. (A driver-level failure —
            // there is no correct way to re-record half an item.)
            OLO_CORE_ERROR("[RHI/Vulkan] vkEndCommandBuffer(secondary, item {}) failed (VkResult {}) — item dropped",
                           item.ItemIndex, static_cast<int>(result));
            item.Cmd = VK_NULL_HANDLE;
            return;
        }
        item.Recorded = true;
        item.RecordMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void VulkanRendererAPI::RecordParallel(const u32 itemCount, const std::function<void(u32 item)>& body)
    {
        OLO_PROFILE_FUNCTION();
        if (itemCount == 0u)
        {
            return;
        }

        // --- decide: fork, or the ONE inline path -----------------------------
        // One item has nothing to overlap with; the predicate covers the
        // lever, the device, the frame clock and nesting; and every item's
        // secondary is acquired HERE, on the render thread, so a pool that
        // cannot hand one out declines the whole region before anything is
        // seeded — there is exactly one inline semantics (ascending order,
        // pre-fork state) for the lever, the predicate and a failure alike.
        bool fork = itemCount >= 2u && SupportsParallelRecording() && VulkanSecondaryCommandPools::Get().SyncToFrame();
        if (fork)
        {
            while (m_Items.size() < itemCount)
            {
                m_Items.push_back(CreateScope<VulkanRecordingContext>());
            }
            for (u32 index = 0; index < itemCount; ++index)
            {
                const VkCommandBuffer cmd = VulkanSecondaryCommandPools::Get().AcquireBegun(index);
                if (cmd == VK_NULL_HANDLE)
                {
                    // Already-begun buffers of earlier items are simply not
                    // executed; the pool reset returns them on the next sync.
                    OLO_CORE_WARN("[RHI/Vulkan] RecordParallel: no secondary for item {} of {} — region recorded inline",
                                  index, itemCount);
                    fork = false;
                    break;
                }
                m_Items[index]->ResetForCommandBuffer(cmd);
            }
        }
        if (!fork)
        {
            ++m_ParallelStats.InlineRegions;
            for (u32 item = 0; item < itemCount; ++item)
            {
                body(item);
            }
            return;
        }

        const auto wallStart = std::chrono::steady_clock::now();
        auto& main = m_Main;
        const u64 regionId = ++m_RegionSerial;

        // --- fork: settle the primary, seed the items ----------------------
        // Items open their own scopes, so the primary's must be closed; the
        // pending clear the pass requested before the fork must land BEFORE
        // the items' work (sequential semantics); and the bound target's
        // attachments go to their attachment layouts now so the items'
        // scope-opens are identity transitions (rule 5).
        EndRenderingScope();
        MaterializePendingClear();
        TransitionBoundTargetAttachmentsForFork();
        m_LayoutClaims.Reset();
        // Two lazy paths every item's draw may take are primed here so no
        // item ever creates or pushes on a shared object: the null sampled
        // slots (a miss submits a one-shot, refused on an item) and the
        // arena push of every UBO bound at the fork (a shared read-only UBO
        // would otherwise be pushed by N items at once).
        VulkanDescriptorHeapBackend::Get().WarmNullSampledSlots();
        for (u32 binding = 0; binding < VulkanBindingState::kMaxBufferBindings; ++binding)
        {
            if (VulkanUniformBuffer* ubo = VulkanBindingState::Global().GetUniformBuffer(binding); ubo != nullptr)
            {
                (void)ubo->GetRootDataAddress();
            }
        }

        for (u32 index = 0; index < itemCount; ++index)
        {
            VulkanRecordingContext& item = *m_Items[index];
            item.ItemIndex = index;
            item.RegionId = regionId;
            // Rule 4: an item sees what iteration 0 of the sequential loop saw.
            item.State = main.State;
            item.RecordedViewport = main.RecordedViewport;
            item.ScissorRect = main.ScissorRect;
            item.ScissorRectSet = main.ScissorRectSet;
            item.Selections = main.Selections;
            item.BoundVertexArray = main.BoundVertexArray;
            item.Binding = VulkanBindingState::Global();
            item.CurrentShader = VulkanShader::GetCurrentlyBound();
            item.CurrentComputeShader = VulkanComputeShader::GetCurrentlyBound();
            item.Tracker.SetReadThroughBase(&main.Tracker, &m_LayoutClaims, index);
            item.RootScratch.clear();
        }

        // --- record ---------------------------------------------------------
        std::exception_ptr firstFailure;
        std::mutex failureMutex;
        {
            // RAII: an exception escaping ParallelFor itself must not leave the
            // region flag stuck (every later fork and mid-frame flush would
            // refuse for the rest of the process).
            const TGuardValue<bool> regionGuard(m_InParallelRegion, true);
            ParallelFor(
                "VulkanRendererAPI::RecordParallel", static_cast<i32>(itemCount), /*MinBatchSize=*/1,
                [&](const i32 itemIndex)
                { RecordParallelItem(*m_Items[static_cast<sizet>(itemIndex)], body, firstFailure, failureMutex); },
                EParallelForFlags::None);
        }

        // --- join: merge layouts in item order, execute in item order --------
        VulkanImageLayoutTracker::MergeBatch batch;
        std::vector<VkCommandBuffer>& secondaries = m_JoinSecondaries;
        secondaries.clear();
        const VulkanRecordingContext* lastRecorded = nullptr;
        f64 workerMs = 0.0;
        u64 arenaAllocations = 0;
        for (u32 index = 0; index < itemCount; ++index)
        {
            VulkanRecordingContext& item = *m_Items[index];
            arenaAllocations += item.ArenaAllocations;
            if (!item.Recorded)
            {
                item.Tracker.DiscardOverlay();
                continue;
            }
            item.Tracker.MergeOverlayInto(main.Tracker, batch);
            secondaries.push_back(item.Cmd);
            main.PreparedDraws += item.PreparedDraws;
            main.DroppedDraws += item.DroppedDraws;
            main.GpuWrittenRootDraws += item.GpuWrittenRootDraws;
            main.ConditionallySkippedDraws += item.ConditionallySkippedDraws;
            workerMs += item.RecordMs;
            lastRecorded = &item;
        }
        if (!secondaries.empty())
        {
            vkCmdExecuteCommands(main.Cmd, static_cast<u32>(secondaries.size()), secondaries.data());
        }
        // Command-buffer state is undefined on the primary after
        // vkCmdExecuteCommands: the per-command-buffer caches must not claim
        // otherwise (the ResumeRecordingAfterFlush shape).
        main.ForgetCommandBufferBinds();
        // GL's sticky framebuffer state after a loop is the LAST iteration's.
        if (lastRecorded != nullptr)
        {
            main.Selections = lastRecorded->Selections;
        }
        for (u32 index = 0; index < itemCount; ++index)
        {
            m_Items[index]->Tracker.SetReadThroughBase(nullptr);
        }
        VulkanFrameArena::Get().AddWorkerAllocations(arenaAllocations);

        ++m_ParallelStats.Regions;
        m_ParallelStats.SecondariesExecuted += static_cast<u32>(secondaries.size());
        m_ParallelStats.MergeConflicts += batch.Conflicts;
        m_ParallelStats.WorkerRecordMs += workerMs;
        m_ParallelStats.RegionWallMs +=
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - wallStart).count();
        if (batch.Conflicts != 0u)
        {
            // Not warn-once: a conflict is a pass bug, and its frequency is
            // part of the diagnosis. The record-time claim already named the
            // two items (VulkanImageLayoutTracker::SetLayout); this is the
            // backstop count. The secondaries still execute: a stale
            // oldLayout on one barrier is a validation error and a possible
            // hazard, dropping the region's work is a certainly broken frame.
            OLO_CORE_ERROR("[RHI/Vulkan] RecordParallel: {} subresource(s) written by more than one item with at least "
                           "one real transition among the writes — a barrier may have named a stale oldLayout "
                           "(amendment (92) rule 5)",
                           batch.Conflicts);
        }
        if (firstFailure)
        {
            std::rethrow_exception(firstFailure);
        }
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
