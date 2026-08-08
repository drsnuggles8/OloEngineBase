// OLO_TEST_LAYER: plumbing
//
// Headless contract tests for the Vulkan barrier lowering (issue #691
// Phase 5, ADR 0011 §1.5) — the CPU layer that pins the formula before any
// device exists (CLAUDE.md rendering-verification rule, step 1).
//
// Everything here is pure: RHI::Access → (stage2, access2, layout) tables,
// the (Access, aspect, read-while-attached) layout-resolution exceptions,
// full VkImageMemoryBarrier2 assembly, and the image-layout tracker's
// per-subresource run splitting. No instance, no device — runs in plain CI.
// The device-gated half (real vkCmdPipelineBarrier2 under the validation
// layer) lives in VulkanRenderGraphExecutionTest.
//
// Like VulkanBringUpTest, the whole file compiles out to one SKIP when the
// backend is off.

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanBarrierLowering, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "Platform/Vulkan/VulkanBarrierLowering.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"

#include <vector>

namespace
{
    using namespace OloEngine;
    namespace VBL = OloEngine::VulkanBarrierLowering;

    constexpr auto kGfx = RHI::QueueType::Graphics;
    constexpr auto kCompute = RHI::QueueType::Compute;
    constexpr auto kColor = RHI::TextureAspect::Color;
    constexpr auto kDepth = RHI::TextureAspect::Depth;
    constexpr auto kDepthStencil = RHI::TextureAspect::DepthStencil;

    [[nodiscard]] VkImage FakeImage(const u64 value)
    {
        // Non-dispatchable handle — a fabricated value is fine for the pure
        // CPU tracker; it never reaches a driver.
        return reinterpret_cast<VkImage>(value); // NOLINT(performance-no-int-to-ptr)
    }
} // namespace

// ---------------------------------------------------------------------------
// LowerAccess: the stage/access table
// ---------------------------------------------------------------------------

TEST(VulkanBarrierLowering, UndefinedLowersToNoStageNoAccess)
{
    const auto sa = VBL::LowerAccess(RHI::Access::Undefined, kGfx, kColor);
    EXPECT_EQ(sa.StageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(sa.AccessMask, VK_ACCESS_2_NONE);
}

TEST(VulkanBarrierLowering, AttachmentAccessesLowerToAttachmentStages)
{
    const auto colorWrite = VBL::LowerAccess(RHI::Access::ColorAttachmentWrite, kGfx, kColor);
    EXPECT_EQ(colorWrite.StageMask, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    EXPECT_EQ(colorWrite.AccessMask, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const auto depthWrite = VBL::LowerAccess(RHI::Access::DepthStencilAttachmentWrite, kGfx, kDepthStencil);
    EXPECT_EQ(depthWrite.StageMask,
              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    EXPECT_EQ(depthWrite.AccessMask, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

TEST(VulkanBarrierLowering, ComputeLaneNarrowsShaderStagesToComputeOnly)
{
    const auto compute = VBL::LowerAccess(RHI::Access::StorageWrite, kCompute, kColor);
    EXPECT_EQ(compute.StageMask, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    EXPECT_EQ(compute.AccessMask, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // Graphics-lane shader accesses include COMPUTE deliberately — this
    // engine's Graphics-work-type passes dispatch compute mid-pass, so the
    // union must cover it (under-inclusion is a race).
    const auto gfx = VBL::LowerAccess(RHI::Access::ShaderSampleRead, kGfx, kColor);
    EXPECT_NE(gfx.StageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0u);
    EXPECT_NE(gfx.StageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0u);
    EXPECT_EQ(gfx.AccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

TEST(VulkanBarrierLowering, ClearAccessesSplitByKind)
{
    // ADR 0011 §1.5's Clear split, lowered: load-op clear is an attachment
    // write (stage depends on aspect); explicit clear is a transfer write on
    // the CLEAR stage.
    const auto loadOpColor = VBL::LowerAccess(RHI::Access::ClearAsLoadOp, kGfx, kColor);
    EXPECT_EQ(loadOpColor.StageMask, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    EXPECT_EQ(loadOpColor.AccessMask, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const auto loadOpDepth = VBL::LowerAccess(RHI::Access::ClearAsLoadOp, kGfx, kDepth);
    EXPECT_EQ(loadOpDepth.AccessMask, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    const auto transferClear = VBL::LowerAccess(RHI::Access::ClearAsTransfer, kGfx, kColor);
    EXPECT_EQ(transferClear.StageMask, VK_PIPELINE_STAGE_2_CLEAR_BIT);
    EXPECT_EQ(transferClear.AccessMask, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

// ---------------------------------------------------------------------------
// LayoutFor: the (Access, aspect, read-while-attached) resolution
// ---------------------------------------------------------------------------

TEST(VulkanBarrierLowering, CommonLayoutsFollowAccess)
{
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::Undefined, kColor, false), VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::ShaderSampleRead, kColor, false), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::ColorAttachmentWrite, kColor, false), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::DepthStencilAttachmentWrite, kDepthStencil, false),
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::TransferRead, kColor, false), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::TransferWrite, kColor, false), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::Present, kColor, false), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

TEST(VulkanBarrierLowering, StorageAccessesLowerToGeneralNotReadOnly)
{
    // THE §1.5 poster child: before the unification, a storage-image WAW
    // transition record claimed ShaderSample and a naïve backend would have
    // picked SHADER_READ_ONLY_OPTIMAL with a read-only access mask. The
    // unified enum makes the write representable; this pins its lowering.
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::StorageWrite, kColor, false), VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::StorageRead, kColor, false), VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::StorageReadWrite, kColor, false), VK_IMAGE_LAYOUT_GENERAL);
}

TEST(VulkanBarrierLowering, ReadWhileAttachedDepthKeepsReadOnlyAttachmentLayout)
{
    // The PCSS raw-depth case (ADR 0011 §1.5's read-while-attached input):
    // sampling depth that is simultaneously an attachment must land in
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL, never plain SHADER_READ_ONLY_OPTIMAL.
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::ShaderSampleRead, kDepth, true),
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::ShaderSampleRead, kDepthStencil, true),
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    // Color feedback needs GENERAL (feedback-loop layout would need
    // VK_EXT_attachment_feedback_loop_layout, outside the ADR 0010 contract).
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::ShaderSampleRead, kColor, true), VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(VBL::LayoutFor(RHI::Access::InputAttachmentRead, kColor, true), VK_IMAGE_LAYOUT_GENERAL);
}

// ---------------------------------------------------------------------------
// BuildImageBarrier: full assembly
// ---------------------------------------------------------------------------

TEST(VulkanBarrierLowering, ImageBarrierAssemblesBothSidesAndLayouts)
{
    RHI::Barrier b;
    b.Before = RHI::Access::ColorAttachmentWrite;
    b.After = RHI::Access::ShaderSampleRead;
    b.Range = {}; // full

    const auto image = FakeImage(0x10);
    const auto out = VBL::BuildImageBarrier(b, image, kColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 4, 1);

    EXPECT_EQ(out.sType, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    EXPECT_EQ(out.image, image);
    EXPECT_EQ(out.srcStageMask, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    EXPECT_EQ(out.srcAccessMask, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    EXPECT_EQ(out.oldLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(out.newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(out.srcQueueFamilyIndex, static_cast<u32>(VK_QUEUE_FAMILY_IGNORED));
    EXPECT_EQ(out.dstQueueFamilyIndex, static_cast<u32>(VK_QUEUE_FAMILY_IGNORED));
    EXPECT_EQ(out.subresourceRange.aspectMask, static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT));
    EXPECT_EQ(out.subresourceRange.levelCount, static_cast<u32>(VK_REMAINING_MIP_LEVELS));
}

TEST(VulkanBarrierLowering, TrackedUndefinedOldLayoutForcesSourceMasksToNone)
{
    // A pooled transient's first-use / unknown-inheritance case: the tracker
    // answers UNDEFINED, the contents are discarded, and whatever FromAccess
    // claimed must NOT leak into the source masks (there is nothing to make
    // available from a discarded image).
    RHI::Barrier b;
    b.Before = RHI::Access::ColorAttachmentWrite; // claims a producer...
    b.After = RHI::Access::ShaderSampleRead;

    const auto out = VBL::BuildImageBarrier(b, FakeImage(0x11), kColor, VK_IMAGE_LAYOUT_UNDEFINED, 1, 1);
    EXPECT_EQ(out.oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(out.srcStageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(out.srcAccessMask, VK_ACCESS_2_NONE);
}

TEST(VulkanBarrierLowering, UndefinedBeforeWithDefinedTrackedLayoutOverSyncs)
{
    // The inverse mismatch: the transition claims no producer (Undefined) but
    // the tracker says the image IS in a defined layout — an out-of-graph
    // writer (the poison clear leaves TRANSFER_DST) produced it. NONE source
    // masks would race that writer; the lowering must go conservative.
    RHI::Barrier b;
    b.Before = RHI::Access::Undefined;
    b.After = RHI::Access::ShaderSampleRead;

    const auto out = VBL::BuildImageBarrier(b, FakeImage(0x13), kColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 1);
    EXPECT_EQ(out.oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(out.srcStageMask, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    EXPECT_NE(out.srcAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT, 0u);
}

TEST(VulkanBarrierLowering, StorageWawBarrierGetsGeneralAndWriteAccess)
{
    // End-to-end §1.5 pin at the assembly level: a WAW transition
    // (StorageWrite -> StorageWrite) must produce GENERAL + a WRITE access on
    // the destination side.
    RHI::Barrier b;
    b.Before = RHI::Access::StorageWrite;
    b.After = RHI::Access::StorageWrite;

    const auto out = VBL::BuildImageBarrier(b, FakeImage(0x12), kColor, VK_IMAGE_LAYOUT_GENERAL, 1, 1);
    EXPECT_EQ(out.newLayout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_NE(out.dstAccessMask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0u);
    EXPECT_EQ(out.dstAccessMask & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0u)
        << "A WAW consumer must not be lowered as a sampled read (the pre-Phase-5 fallback)";
}

TEST(VulkanBarrierLowering, BufferBarrierCoversWholeBufferWithBothSides)
{
    RHI::Barrier b;
    b.Before = RHI::Access::StorageWrite;
    b.After = RHI::Access::IndirectArgsRead;

    const auto buffer = reinterpret_cast<VkBuffer>(static_cast<u64>(0x20)); // NOLINT(performance-no-int-to-ptr)
    const auto out = VBL::BuildBufferBarrier(b, buffer);
    EXPECT_EQ(out.sType, VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
    EXPECT_EQ(out.buffer, buffer);
    EXPECT_EQ(out.size, VK_WHOLE_SIZE);
    EXPECT_EQ(out.dstStageMask, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
    EXPECT_EQ(out.dstAccessMask, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}

// ---------------------------------------------------------------------------
// VulkanImageLayoutTracker
// ---------------------------------------------------------------------------

TEST(VulkanImageLayoutTracker, UnknownImageAnswersUndefined)
{
    const VulkanImageLayoutTracker tracker;
    VkImageSubresourceRange full{ VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    EXPECT_EQ(tracker.CurrentLayout(FakeImage(0x30), full), VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST(VulkanImageLayoutTracker, SetAndQueryRoundTrip)
{
    VulkanImageLayoutTracker tracker;
    const auto image = FakeImage(0x31);
    tracker.RegisterImage(image, 4, 1);

    VkImageSubresourceRange full{ VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    EXPECT_EQ(tracker.CurrentLayout(image, full), VK_IMAGE_LAYOUT_UNDEFINED);

    tracker.SetLayout(image, full, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(tracker.CurrentLayout(image, full), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

TEST(VulkanImageLayoutTracker, MixedRangeSplitsIntoExactRuns)
{
    // The HZB shape: mip 0 written as an attachment, mips 1..N as storage.
    // A whole-image query must yield one run per layout region with exact
    // sub-ranges — one guessed oldLayout for a mixed range is the validation
    // error class this tracker exists to prevent.
    VulkanImageLayoutTracker tracker;
    const auto image = FakeImage(0x32);
    tracker.RegisterImage(image, 4, 1);

    VkImageSubresourceRange mip0{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange rest{ VK_IMAGE_ASPECT_COLOR_BIT, 1, 3, 0, 1 };
    tracker.SetLayout(image, mip0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    tracker.SetLayout(image, rest, VK_IMAGE_LAYOUT_GENERAL);

    VkImageSubresourceRange full{ VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    std::vector<std::pair<VkImageSubresourceRange, VkImageLayout>> runs;
    tracker.ForEachLayoutRun(image, full,
                             [&runs](const VkImageSubresourceRange& r, const VkImageLayout l)
                             { runs.emplace_back(r, l); });

    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].first.baseMipLevel, 0u);
    EXPECT_EQ(runs[0].first.levelCount, 1u);
    EXPECT_EQ(runs[0].second, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(runs[1].first.baseMipLevel, 1u);
    EXPECT_EQ(runs[1].first.levelCount, 3u);
    EXPECT_EQ(runs[1].second, VK_IMAGE_LAYOUT_GENERAL);

    // The convenience whole-range query cannot name one layout for a mixed
    // range — UNDEFINED (discard) is its safe total answer.
    EXPECT_EQ(tracker.CurrentLayout(image, full), VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST(VulkanImageLayoutTracker, ForgetDropsStateAndReRegistrationResets)
{
    VulkanImageLayoutTracker tracker;
    const auto image = FakeImage(0x33);
    tracker.RegisterImage(image, 2, 1);
    VkImageSubresourceRange full{ VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    tracker.SetLayout(image, full, VK_IMAGE_LAYOUT_GENERAL);

    // Destroy + handle-value reuse: the recycled value must not inherit the
    // dead image's layouts.
    tracker.ForgetImage(image);
    EXPECT_EQ(tracker.CurrentLayout(image, full), VK_IMAGE_LAYOUT_UNDEFINED);

    // Re-registration with different extents resets too (different
    // allocation reusing the handle value).
    tracker.RegisterImage(image, 2, 1);
    tracker.SetLayout(image, full, VK_IMAGE_LAYOUT_GENERAL);
    tracker.RegisterImage(image, 6, 1);
    EXPECT_EQ(tracker.CurrentLayout(image, full), VK_IMAGE_LAYOUT_UNDEFINED);
}

#endif // OLO_WITH_VULKAN
