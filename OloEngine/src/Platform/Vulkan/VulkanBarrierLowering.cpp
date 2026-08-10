#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanBarrierLowering.h"

#if OLO_WITH_VULKAN

namespace OloEngine::VulkanBarrierLowering
{
    namespace
    {
        [[nodiscard]] bool IsDepthLikeAspect(const RHI::TextureAspect aspect)
        {
            return aspect == RHI::TextureAspect::Depth ||
                   aspect == RHI::TextureAspect::Stencil ||
                   aspect == RHI::TextureAspect::DepthStencil;
        }

        // RenderGraph's framebuffer fan-out (BuildRHIBarriers) re-uses ONE
        // access prototype for EVERY attachment of the framebuffer — the
        // depth attachment included — and documents that "the backend
        // derives each attachment's aspect/layout from its own image
        // format". Honour that contract here: an attachment access arriving
        // with a depth-like aspect IS the depth-stencil attachment access in
        // disguise, and remapping the enum (instead of special-casing each
        // switch) keeps LowerAccess and LayoutFor consistent by
        // construction. Lowered aspect-blind, a graph WAW between two
        // passes writing the same depth-carrying framebuffer emitted
        // COLOR_ATTACHMENT_OPTIMAL on the depth image — illegal without
        // COLOR_ATTACHMENT usage (VUID-VkImageMemoryBarrier2-oldLayout-
        // 01208) — and then poisoned the layout tracker so the next
        // scope-open emitted the inverse oldLayout error (found by the
        // pass-suite ShaderDebugDraw/Particle tenants, #691 Wave C).
        [[nodiscard]] RHI::Access NormalizeAttachmentAccessForAspect(const RHI::Access access,
                                                                     const RHI::TextureAspect aspect)
        {
            if (!IsDepthLikeAspect(aspect))
                return access;
            if (access == RHI::Access::ColorAttachmentRead)
                return RHI::Access::DepthStencilAttachmentRead;
            if (access == RHI::Access::ColorAttachmentWrite)
                return RHI::Access::DepthStencilAttachmentWrite;
            return access;
        }

        // The shader-stage union one lane's shader access can touch. The
        // Graphics union includes COMPUTE deliberately — this engine's
        // Graphics-work-type passes dispatch compute mid-pass (bloom chain,
        // AO), and the lane annotation describes the PASS, not the pipe.
        [[nodiscard]] VkPipelineStageFlags2 ShaderStagesForQueue(const RHI::QueueType queue)
        {
            switch (queue)
            {
                case RHI::QueueType::Compute:
                    return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                case RHI::QueueType::Transfer:
                    // A shader access annotated onto the transfer lane cannot
                    // narrow — fall back to every shader stage.
                case RHI::QueueType::Graphics:
                    return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            }

            return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }

        constexpr VkPipelineStageFlags2 kDepthStencilTestStages =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    } // namespace

    StageAccess LowerAccess(const RHI::Access access, const RHI::QueueType queue, const RHI::TextureAspect aspect)
    {
        switch (NormalizeAttachmentAccessForAspect(access, aspect))
        {
            case RHI::Access::Undefined:
                // No prior access to make available — pairs with an UNDEFINED
                // oldLayout on the source side of a first-use transition.
                return { VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE };

            case RHI::Access::IndirectArgsRead:
                return { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT };
            case RHI::Access::IndexRead:
                return { VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT };
            case RHI::Access::VertexAttributeRead:
                return { VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT };
            case RHI::Access::UniformRead:
                return { ShaderStagesForQueue(queue), VK_ACCESS_2_UNIFORM_READ_BIT };

            case RHI::Access::ShaderSampleRead:
                return { ShaderStagesForQueue(queue), VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
            case RHI::Access::StorageRead:
                return { ShaderStagesForQueue(queue), VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
            case RHI::Access::StorageWrite:
                return { ShaderStagesForQueue(queue), VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
            case RHI::Access::StorageReadWrite:
                return { ShaderStagesForQueue(queue),
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };

            case RHI::Access::ColorAttachmentRead:
                return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT };
            case RHI::Access::ColorAttachmentWrite:
                // READ|WRITE, not WRITE alone: an attachment-write access
                // carries the loadOp/blend READ half with it — a transition
                // INTO the attachment scope must make the image visible to
                // loadOp LOAD's ATTACHMENT_READ or sync validation flags the
                // begin-rendering read (found by VulkanPassSuiteTest). As a
                // SOURCE scope the extra read bit is ignored by the
                // availability rules, so one spelling serves both directions.
                return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
            case RHI::Access::DepthStencilAttachmentRead:
                return { kDepthStencilTestStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT };
            case RHI::Access::DepthStencilAttachmentWrite:
                // READ|WRITE for the same reason as ColorAttachmentWrite: the
                // depth test's read half rides along with the write access.
                return { kDepthStencilTestStages,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
            case RHI::Access::InputAttachmentRead:
                return { VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT };

            case RHI::Access::TransferRead:
                return { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };
            case RHI::Access::TransferWrite:
                return { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };

            case RHI::Access::ClearAsLoadOp:
                // A load-op clear is an attachment write on whichever
                // attachment kind the aspect names — the one access whose
                // stage depends on the aspect.
                if (IsDepthLikeAspect(aspect))
                    return { kDepthStencilTestStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
                return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
            case RHI::Access::ClearAsTransfer:
                return { VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };

            case RHI::Access::Present:
                // Presentation is synchronised by the queue-submit semaphore,
                // not by an access mask (there is no PRESENT access bit).
                return { VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE };
        }

        return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
    }

    VkImageLayout LayoutFor(const RHI::Access access, const RHI::TextureAspect aspect, const bool readWhileAttached)
    {
        switch (NormalizeAttachmentAccessForAspect(access, aspect))
        {
            case RHI::Access::Undefined:
                return VK_IMAGE_LAYOUT_UNDEFINED;

            case RHI::Access::ShaderSampleRead:
                if (readWhileAttached)
                {
                    // The attachment half of the feedback is still live: depth
                    // keeps its read-only attachment layout (the PCSS raw-depth
                    // case); color needs GENERAL (feedback-loop layout would
                    // need VK_EXT_attachment_feedback_loop_layout).
                    return IsDepthLikeAspect(aspect) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                     : VK_IMAGE_LAYOUT_GENERAL;
                }
                return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            case RHI::Access::InputAttachmentRead:
                return readWhileAttached ? VK_IMAGE_LAYOUT_GENERAL
                                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            case RHI::Access::StorageRead:
            case RHI::Access::StorageWrite:
            case RHI::Access::StorageReadWrite:
                return VK_IMAGE_LAYOUT_GENERAL;

            case RHI::Access::ColorAttachmentRead:
            case RHI::Access::ColorAttachmentWrite:
                return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case RHI::Access::DepthStencilAttachmentWrite:
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case RHI::Access::DepthStencilAttachmentRead:
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

            case RHI::Access::TransferRead:
                return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            case RHI::Access::TransferWrite:
            case RHI::Access::ClearAsTransfer:
                return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

            case RHI::Access::ClearAsLoadOp:
                return IsDepthLikeAspect(aspect) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            case RHI::Access::Present:
                return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            // Buffer-shaped accesses have no image layout; GENERAL is the
            // total answer if one ever reaches an image barrier.
            case RHI::Access::IndirectArgsRead:
            case RHI::Access::IndexRead:
            case RHI::Access::VertexAttributeRead:
            case RHI::Access::UniformRead:
                return VK_IMAGE_LAYOUT_GENERAL;
        }

        return VK_IMAGE_LAYOUT_GENERAL;
    }

    VkImageAspectFlags AspectMaskFor(const RHI::TextureAspect aspect)
    {
        switch (aspect)
        {
            case RHI::TextureAspect::Color:
                return VK_IMAGE_ASPECT_COLOR_BIT;
            case RHI::TextureAspect::Depth:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            case RHI::TextureAspect::Stencil:
                return VK_IMAGE_ASPECT_STENCIL_BIT;
            case RHI::TextureAspect::DepthStencil:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        return VK_IMAGE_ASPECT_COLOR_BIT;
    }

    VkImageMemoryBarrier2 BuildImageBarrier(const RHI::Barrier& barrier,
                                            const VkImage image,
                                            const RHI::TextureAspect aspect,
                                            const VkImageLayout trackedOldLayout,
                                            const u32 imageMipCount,
                                            const u32 imageLayerCount)
    {
        auto src = LowerAccess(barrier.Before, barrier.SourceQueue, aspect);
        const auto dst = LowerAccess(barrier.After, barrier.DestQueue, aspect);

        // The tracker is authoritative for oldLayout. When it answers
        // UNDEFINED (first use, or a pooled transient's unknown inheritance)
        // the contents are discarded, so there is nothing to make available —
        // source masks drop to NONE regardless of what FromAccess claimed.
        if (trackedOldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            src = { VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE };
        }
        else if (barrier.Before == RHI::Access::Undefined)
        {
            // The INVERSE mismatch: the transition claims no producer, yet
            // the image is demonstrably in a defined layout — something
            // outside the graph's knowledge wrote it (the poison clear is
            // the live case: it leaves TRANSFER_DST recorded, and the
            // external first-use barrier that follows carries Undefined).
            // NONE source masks would race that writer; over-sync instead.
            src = { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
        }

        VkImageMemoryBarrier2 out{};
        out.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        out.srcStageMask = src.StageMask;
        out.srcAccessMask = src.AccessMask;
        out.dstStageMask = dst.StageMask;
        out.dstAccessMask = dst.AccessMask;
        out.oldLayout = trackedOldLayout;
        out.newLayout = LayoutFor(barrier.After, aspect, barrier.ReadWhileAttached);
        out.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out.image = image;

        // Clamp the neutral range against the image's real extents —
        // AllRemaining is a VK_REMAINING_* spelling, and a range beyond the
        // image is a validation error rather than a clamp.
        auto& range = out.subresourceRange;
        range.aspectMask = AspectMaskFor(aspect);
        range.baseMipLevel = std::min(barrier.Range.BaseMip, imageMipCount > 0u ? imageMipCount - 1u : 0u);
        // Resolved counts clamp to AT LEAST 1: a zero levelCount/layerCount is
        // a VUID violation, and the degenerate inputs that could produce one
        // (a zero-extent image record, a base at the image's last slot with a
        // zero span) should degrade to a 1-wide range, not an invalid barrier.
        range.levelCount = (barrier.Range.MipCount == RHI::SubresourceRange::AllRemaining)
                               ? VK_REMAINING_MIP_LEVELS
                               : std::max(std::min(barrier.Range.MipCount, imageMipCount - range.baseMipLevel), 1u);
        range.baseArrayLayer = std::min(barrier.Range.BaseLayer, imageLayerCount > 0u ? imageLayerCount - 1u : 0u);
        range.layerCount = (barrier.Range.LayerCount == RHI::SubresourceRange::AllRemaining)
                               ? VK_REMAINING_ARRAY_LAYERS
                               : std::max(std::min(barrier.Range.LayerCount, imageLayerCount - range.baseArrayLayer), 1u);

        return out;
    }

    VkBufferMemoryBarrier2 BuildBufferBarrier(const RHI::Barrier& barrier, const VkBuffer buffer)
    {
        const auto src = LowerAccess(barrier.Before, barrier.SourceQueue, RHI::TextureAspect::Color);
        const auto dst = LowerAccess(barrier.After, barrier.DestQueue, RHI::TextureAspect::Color);

        VkBufferMemoryBarrier2 out{};
        out.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        out.srcStageMask = src.StageMask;
        out.srcAccessMask = src.AccessMask;
        out.dstStageMask = dst.StageMask;
        out.dstAccessMask = dst.AccessMask;
        out.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out.buffer = buffer;
        out.offset = 0;
        out.size = VK_WHOLE_SIZE;
        return out;
    }
    VkFormat ToVkFormat(const RHI::Format format)
    {
        switch (format)
        {
            case RHI::Format::Unknown:
                return VK_FORMAT_UNDEFINED;
            case RHI::Format::R8UNorm:
                return VK_FORMAT_R8_UNORM;
            case RHI::Format::R8UInt:
                return VK_FORMAT_R8_UINT;
            case RHI::Format::RG8UNorm:
                return VK_FORMAT_R8G8_UNORM;
            case RHI::Format::RGB8UNorm:
                return VK_FORMAT_R8G8B8A8_UNORM; // widened, matching image allocation
            case RHI::Format::RGBA8UNorm:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case RHI::Format::RGBA8SRGB:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case RHI::Format::R16UInt:
                return VK_FORMAT_R16_UINT;
            case RHI::Format::RG16UInt:
                return VK_FORMAT_R16G16_UINT;
            case RHI::Format::RG16Float:
                return VK_FORMAT_R16G16_SFLOAT;
            case RHI::Format::RGBA16Float:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case RHI::Format::R32Float:
                return VK_FORMAT_R32_SFLOAT;
            case RHI::Format::R32Int:
                return VK_FORMAT_R32_SINT;
            case RHI::Format::R32UInt:
                return VK_FORMAT_R32_UINT;
            case RHI::Format::RG32Float:
                return VK_FORMAT_R32G32_SFLOAT;
            case RHI::Format::RGB32Float:
                return VK_FORMAT_R32G32B32A32_SFLOAT; // widened
            case RHI::Format::RGBA32Float:
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            case RHI::Format::D24UNormS8UInt:
                return VK_FORMAT_D32_SFLOAT_S8_UINT; // what the allocator actually creates
            case RHI::Format::D32Float:
                return VK_FORMAT_D32_SFLOAT;
            case RHI::Format::BC7UNorm:
                return VK_FORMAT_BC7_UNORM_BLOCK;
            case RHI::Format::BC7SRGB:
                return VK_FORMAT_BC7_SRGB_BLOCK;
            case RHI::Format::BC5UNorm:
                return VK_FORMAT_BC5_UNORM_BLOCK;
            case RHI::Format::BC6HUFloat:
                return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        }
        return VK_FORMAT_UNDEFINED;
    }

} // namespace OloEngine::VulkanBarrierLowering

#endif // OLO_WITH_VULKAN
