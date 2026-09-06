// OLO_TEST_LAYER: plumbing
//
// Parallel command recording (issue #806, ADR 0011 amendment (92)).
//
// Two halves, the CLAUDE.md rendering-verification rule's first step for a
// backend change:
//
//  1. Pure CPU, fabricated handles (the VulkanBarrierLoweringTest shape): the
//     layout-tracker OVERLAY and its merge rule — read-through, copy-on-write,
//     item-order merge of only the written subresources, and the one loud
//     rule: two items may write one subresource only as identity transitions.
//  2. Device-gated: RecordParallel on a real device — N items into N targets
//     through one vkCmdExecuteCommands, the shared-target identity case the
//     shadow atlas depends on, the lever's inline fallback, and the frame
//     arena's lock-free claim from real threads. Validation errors == 0 is
//     asserted by the fixture's TearDown.
//
// Like VulkanBringUpTest, the whole file compiles out to one SKIP when the
// backend is off.

// The PCH first: it brings the HAL platform headers in before anything can
// pull <windows.h> (gtest does), whose Yield / MemoryBarrier macros would
// otherwise break the task-scheduler and RendererAPI declarations below.
#include "OloEnginePCH.h"

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>
#include <filesystem>

#if !OLO_WITH_VULKAN

TEST(VulkanParallelRecording, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/PreparedFullscreenPass.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/RenderGraphPlanExecutor.h"
#include "OloEngine/Renderer/RenderGraphSubmissionPlan.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Task/Scheduler.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanRecordingContext.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanSecondaryCommandPools.h"
#include "Platform/Vulkan/VulkanShader.h"

#include "VulkanTestSupport.h"

#include <volk.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace OloEngine;
    using OloEngine::Tests::ProbeVulkanDeviceTestGate;
    // The facade's own instance, not a local one: VulkanUpload::TryGetVulkanAPI()
    // resolves RenderCommand's API, so a framebuffer's depth-array selection and
    // the engine-side IsRecordingParallelItem() asserts reach the API under test.
    using OloEngine::Tests::ScopedVulkanRenderCommandSelection;

    [[nodiscard]] VkImage FakeImage(const u64 value)
    {
        // Non-dispatchable handle — a fabricated value never reaches a driver
        // in the CPU half.
        return reinterpret_cast<VkImage>(value); // NOLINT(performance-no-int-to-ptr)
    }

    [[nodiscard]] VkImageSubresourceRange Layer(const u32 layer)
    {
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = layer;
        range.layerCount = 1;
        return range;
    }

    constexpr auto kDepthAttachment = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    constexpr auto kTransferDst = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    constexpr auto kShaderRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr auto kUndefined = VK_IMAGE_LAYOUT_UNDEFINED;
} // namespace

// =============================================================================
// CPU half: the layout overlay and the merge rule
// =============================================================================

TEST(VulkanParallelRecording, OverlayReadsThroughToBaseUntilItWrites)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x10);
    base.RegisterImage(image, 1, 4, /*registrationId=*/1);
    base.SetLayout(image, Layer(1), kDepthAttachment);

    VulkanImageLayoutTracker overlay;
    overlay.SetReadThroughBase(&base);
    ASSERT_TRUE(overlay.IsOverlay());

    // Reads fall through; nothing is copied by a read.
    EXPECT_EQ(overlay.CurrentLayout(image, Layer(1)), kDepthAttachment);
    EXPECT_EQ(overlay.CurrentLayout(image, Layer(0)), kUndefined);
    EXPECT_EQ(overlay.GetOverlayRowCount(), sizet{ 0 });

    // The first write copies the row in — the whole row, so the other
    // layers keep answering what the base said.
    overlay.SetLayout(image, Layer(2), kDepthAttachment);
    EXPECT_EQ(overlay.GetOverlayRowCount(), sizet{ 1 });
    EXPECT_EQ(overlay.CurrentLayout(image, Layer(2)), kDepthAttachment);
    EXPECT_EQ(overlay.CurrentLayout(image, Layer(1)), kDepthAttachment);
    // ... and the base is untouched until the merge.
    EXPECT_EQ(base.CurrentLayout(image, Layer(2)), kUndefined);
}

TEST(VulkanParallelRecording, MergeWritesOnlyTheTouchedSubresourcesInItemOrder)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x20);
    base.RegisterImage(image, 1, 4, 1);

    VulkanImageLayoutTracker item0;
    VulkanImageLayoutTracker item1;
    item0.SetReadThroughBase(&base);
    item1.SetReadThroughBase(&base);
    item0.SetLayout(image, Layer(0), kDepthAttachment);
    item1.SetLayout(image, Layer(1), kTransferDst);

    VulkanImageLayoutTracker::MergeBatch batch;
    item0.MergeOverlayInto(base, batch);
    item1.MergeOverlayInto(base, batch);

    EXPECT_EQ(batch.Conflicts, 0u);
    EXPECT_EQ(batch.SubresourcesMerged, 2u);
    EXPECT_EQ(base.CurrentLayout(image, Layer(0)), kDepthAttachment);
    EXPECT_EQ(base.CurrentLayout(image, Layer(1)), kTransferDst);
    EXPECT_EQ(base.CurrentLayout(image, Layer(2)), kUndefined) << "an untouched subresource must not be rewritten";
    EXPECT_EQ(base.CurrentLayout(image, Layer(3)), kUndefined);
    EXPECT_EQ(item0.GetOverlayRowCount(), sizet{ 0 }) << "a merged overlay drops its rows";
    EXPECT_EQ(item1.GetOverlayRowCount(), sizet{ 0 });
}

TEST(VulkanParallelRecording, MergeReportsTwoItemsTransitioningOneSubresource)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x30);
    base.RegisterImage(image, 1, 2, 1);

    VulkanImageLayoutTracker item0;
    VulkanImageLayoutTracker item1;
    item0.SetReadThroughBase(&base);
    item1.SetReadThroughBase(&base);
    // Both items barrier layer 0 from what the pre-fork state said
    // (UNDEFINED) — the second one's oldLayout is a lie once the first has
    // executed. That is the amendment (92) rule 5 conflict.
    item0.SetLayout(image, Layer(0), kDepthAttachment);
    item1.SetLayout(image, Layer(0), kTransferDst);

    VulkanImageLayoutTracker::MergeBatch batch;
    item0.MergeOverlayInto(base, batch);
    item1.MergeOverlayInto(base, batch);

    EXPECT_EQ(batch.Conflicts, 1u);
    // Item order still decides the merged value — the later item's write is
    // what the queue will have executed last.
    EXPECT_EQ(base.CurrentLayout(image, Layer(0)), kTransferDst);
}

TEST(VulkanParallelRecording, IdentityTransitionsOnASharedSubresourceAreNotAConflict)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x40);
    base.RegisterImage(image, 1, 1, 1);
    base.SetLayout(image, Layer(0), kDepthAttachment); // the fork's pre-transition

    VulkanImageLayoutTracker item0;
    VulkanImageLayoutTracker item1;
    item0.SetReadThroughBase(&base);
    item1.SetReadThroughBase(&base);
    // The shadow atlas: every entry's scope-open records ATTACHMENT →
    // ATTACHMENT on layer 0. Each barrier's oldLayout is true regardless of
    // which item executed first.
    item0.SetLayout(image, Layer(0), kDepthAttachment);
    item1.SetLayout(image, Layer(0), kDepthAttachment);

    VulkanImageLayoutTracker::MergeBatch batch;
    item0.MergeOverlayInto(base, batch);
    item1.MergeOverlayInto(base, batch);
    EXPECT_EQ(batch.Conflicts, 0u);
    EXPECT_EQ(batch.SubresourcesMerged, 2u);
    EXPECT_EQ(base.CurrentLayout(image, Layer(0)), kDepthAttachment);
}

// An item that transitions a subresource away and back ends where it
// started, but it RECORDED a real transition. Rule 5 is stated on what each
// item recorded, so the merge counts it as a real writer, not an identity
// overlap. (The record-time claim table only reports two REAL claimants, so
// it stays silent for this pair; the merge is the backstop that sees it.)
TEST(VulkanParallelRecording, ATransitionPairThatReturnsToTheOriginalLayoutIsStillAConflict)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x45);
    base.RegisterImage(image, 1, 1, 1);
    base.SetLayout(image, Layer(0), kDepthAttachment);

    VulkanImageLayoutTracker item0;
    VulkanImageLayoutTracker item1;
    item0.SetReadThroughBase(&base);
    item1.SetReadThroughBase(&base);
    item0.SetLayout(image, Layer(0), kTransferDst);     // a real transition ...
    item0.SetLayout(image, Layer(0), kDepthAttachment); // ... back to the original
    item1.SetLayout(image, Layer(0), kDepthAttachment); // identity

    VulkanImageLayoutTracker::MergeBatch batch;
    item0.MergeOverlayInto(base, batch);
    item1.MergeOverlayInto(base, batch);
    EXPECT_EQ(batch.Conflicts, 1u) << "A -> B -> A is a real transition, not an identity write";
    EXPECT_EQ(base.CurrentLayout(image, Layer(0)), kDepthAttachment);
}

TEST(VulkanParallelRecording, AnIdentityWriteFollowedByARealTransitionIsAConflict)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x50);
    base.RegisterImage(image, 1, 1, 1);
    base.SetLayout(image, Layer(0), kDepthAttachment);

    VulkanImageLayoutTracker item0;
    VulkanImageLayoutTracker item1;
    item0.SetReadThroughBase(&base);
    item1.SetReadThroughBase(&base);
    item0.SetLayout(image, Layer(0), kDepthAttachment); // identity
    item1.SetLayout(image, Layer(0), kTransferDst);     // real — and item 0's scope stored into ATTACHMENT after it

    VulkanImageLayoutTracker::MergeBatch batch;
    item0.MergeOverlayInto(base, batch);
    item1.MergeOverlayInto(base, batch);
    EXPECT_EQ(batch.Conflicts, 1u);

    // And the mirror image: real first, identity second is a conflict too
    // (the identity's oldLayout is stale).
    VulkanImageLayoutTracker base2;
    base2.RegisterImage(image, 1, 1, 1);
    base2.SetLayout(image, Layer(0), kDepthAttachment);
    VulkanImageLayoutTracker a;
    VulkanImageLayoutTracker b;
    a.SetReadThroughBase(&base2);
    b.SetReadThroughBase(&base2);
    a.SetLayout(image, Layer(0), kTransferDst);
    b.SetLayout(image, Layer(0), kDepthAttachment);
    VulkanImageLayoutTracker::MergeBatch batch2;
    a.MergeOverlayInto(base2, batch2);
    b.MergeOverlayInto(base2, batch2);
    EXPECT_EQ(batch2.Conflicts, 1u);
}

TEST(VulkanParallelRecording, OverlayRegisterOfABaseTrackedImageKeepsTheBaseLayouts)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x60);
    base.RegisterImage(image, 1, 4, /*registrationId=*/7);
    base.SetLayout(image, Layer(1), kDepthAttachment);

    VulkanImageLayoutTracker overlay;
    overlay.SetReadThroughBase(&base);
    // Every scope-open re-registers the image (idempotently on a plain
    // tracker). On an overlay that must not reset the row to `initialLayout`.
    overlay.RegisterImage(image, 1, 4, 7, kUndefined);
    EXPECT_EQ(overlay.CurrentLayout(image, Layer(1)), kDepthAttachment);

    // Registering it did not count as writing it.
    VulkanImageLayoutTracker::MergeBatch batch;
    overlay.MergeOverlayInto(base, batch);
    EXPECT_EQ(batch.SubresourcesMerged, 0u);
    EXPECT_EQ(batch.Conflicts, 0u);
}

TEST(VulkanParallelRecording, AnImageOnlyTheOverlayRegisteredIsInsertedWholeOnMerge)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x70);

    VulkanImageLayoutTracker overlay;
    overlay.SetReadThroughBase(&base);
    overlay.RegisterImage(image, 1, 2, 3, kShaderRead);
    overlay.SetLayout(image, Layer(1), kTransferDst);

    VulkanImageLayoutTracker::MergeBatch batch;
    overlay.MergeOverlayInto(base, batch);
    EXPECT_EQ(batch.Conflicts, 0u);
    EXPECT_EQ(base.CurrentLayout(image, Layer(0)), kShaderRead) << "the registered initial layout rides along";
    EXPECT_EQ(base.CurrentLayout(image, Layer(1)), kTransferDst);
}

TEST(VulkanParallelRecording, ForEachLayoutRunFallsThroughToTheBase)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x80);
    base.RegisterImage(image, 1, 3, 1);
    base.SetLayout(image, Layer(1), kDepthAttachment);

    VulkanImageLayoutTracker overlay;
    overlay.SetReadThroughBase(&base);

    VkImageSubresourceRange all{};
    all.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    all.levelCount = 1;
    all.layerCount = VK_REMAINING_ARRAY_LAYERS;
    std::vector<std::pair<u32, VkImageLayout>> runs;
    overlay.ForEachLayoutRun(image, all,
                             [&](const VkImageSubresourceRange& run, const VkImageLayout layout)
                             { runs.emplace_back(run.baseArrayLayer, layout); });
    ASSERT_EQ(runs.size(), sizet{ 3 });
    EXPECT_EQ(runs[0].second, kUndefined);
    EXPECT_EQ(runs[1].second, kDepthAttachment);
    EXPECT_EQ(runs[2].second, kUndefined);
}

TEST(VulkanParallelRecording, DiscardedOverlayLeavesTheBaseAlone)
{
    VulkanImageLayoutTracker base;
    const VkImage image = FakeImage(0x90);
    base.RegisterImage(image, 1, 1, 1);

    VulkanImageLayoutTracker overlay;
    overlay.SetReadThroughBase(&base);
    overlay.SetLayout(image, Layer(0), kTransferDst);
    overlay.DiscardOverlay();
    EXPECT_EQ(overlay.GetOverlayRowCount(), sizet{ 0 });
    EXPECT_EQ(base.CurrentLayout(image, Layer(0)), kUndefined);
}

// No Vulkan device: these threads exercise the same claims, frozen base and
// overlay mutation used by recording workers, including in the Linux TSan job.
TEST(VulkanParallelRecording, ConcurrentClaimsAndOverlaysMergeDeterministically)
{
    constexpr u32 itemCount = 8;
    constexpr u32 imageCount = 32;
    VulkanImageLayoutTracker base;
    VulkanLayoutClaimTable claims;
    std::array<VulkanImageLayoutTracker, itemCount> overlays;
    for (u32 image = 0; image < imageCount; ++image)
        base.RegisterImage(FakeImage(0x1000u + image), 1, itemCount + 1u, 1, kDepthAttachment);
    for (u32 item = 0; item < itemCount; ++item)
        overlays[item].SetReadThroughBase(&base, &claims, item);

    std::barrier start(static_cast<std::ptrdiff_t>(itemCount));
    std::vector<std::jthread> threads;
    for (u32 item = 0; item < itemCount; ++item)
    {
        threads.emplace_back([&, item]
                             {
            start.arrive_and_wait();
            for (u32 image = 0; image < imageCount; ++image)
            {
                const auto handle = FakeImage(0x1000u + image);
                // Contend the claim map, with one owner per real transition.
                overlays[item].SetLayout(handle, Layer(item), kTransferDst);
                overlays[item].SetLayout(handle, Layer(item), kShaderRead);
                // Every worker also writes the same identity-only layer.
                overlays[item].SetLayout(handle, Layer(itemCount), kDepthAttachment);
            } });
    }
    threads.clear(); // join all workers before touching the frozen base
    VulkanImageLayoutTracker::MergeBatch batch;
    for (auto& overlay : overlays)
        overlay.MergeOverlayInto(base, batch);
    EXPECT_EQ(batch.Conflicts, 0u);
    for (u32 image = 0; image < imageCount; ++image)
    {
        for (u32 item = 0; item < itemCount; ++item)
            EXPECT_EQ(base.CurrentLayout(FakeImage(0x1000u + image), Layer(item)), kShaderRead);
        EXPECT_EQ(base.CurrentLayout(FakeImage(0x1000u + image), Layer(itemCount)), kDepthAttachment);
    }

    // Same-key contention has exactly one winner and every loser names it.
    claims.Reset();
    std::array<u32, itemCount> owners{};
    std::barrier claimStart(static_cast<std::ptrdiff_t>(itemCount));
    for (u32 item = 0; item < itemCount; ++item)
        threads.emplace_back([&, item]
                             {
            claimStart.arrive_and_wait();
            owners[item] = claims.Claim(FakeImage(0x2000), 0, item); });
    threads.clear();
    const auto winner = std::find(owners.begin(), owners.end(), VulkanLayoutClaimTable::kNone);
    ASSERT_NE(winner, owners.end());
    const auto winnerIndex = static_cast<u32>(std::distance(owners.begin(), winner));
    EXPECT_EQ(std::count(owners.begin(), owners.end(), VulkanLayoutClaimTable::kNone), 1);
    for (u32 item = 0; item < itemCount; ++item)
        if (item != winnerIndex)
            EXPECT_EQ(owners[item], winnerIndex);
}

// The facade contract every backend shares: without support the items run
// inline, in ascending order, on the calling thread. The shared GPU-test
// process runs on OpenGL here, which never overrides RecordParallel.
TEST(VulkanParallelRecording, FacadeRunsItemsInlineInOrderWithoutBackendSupport)
{
    std::vector<u32> order;
    const auto caller = std::this_thread::get_id();
    bool sameThread = true;
    RenderCommand::RecordParallel(5,
                                  [&](const u32 item)
                                  {
                                      order.push_back(item);
                                      sameThread = sameThread && std::this_thread::get_id() == caller;
                                      sameThread = sameThread && CurrentVulkanWorkerContext() == nullptr;
                                  });
    EXPECT_EQ(order, (std::vector<u32>{ 0, 1, 2, 3, 4 }));
    EXPECT_TRUE(sameThread);
}

// =============================================================================
// Device half
// =============================================================================

namespace
{
    // The same §5f-shaped pair VulkanDrawPathTest draws with: vertex pulling
    // from the reserved binding 57, a UBO tint at binding 3, a sampled
    // texture at binding 0. A fullscreen triangle covers the viewport.
    constexpr const char* kVertexSrc = R"(
#version 460 core
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    int base = gl_VertexIndex * 2;
    vec2 position = vec2(b_Vertices.v[base + 0], b_Vertices.v[base + 1]);
    v_TexCoord = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

    constexpr const char* kFragmentSrc = R"(
#version 460 core
layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

layout(binding = 0) uniform sampler2D u_Texture;
layout(std140, binding = 3) uniform TintBlock
{
    vec4 u_Tint;
};

void main()
{
    o_Color = texture(u_Texture, v_TexCoord) * u_Tint;
}
)";

    // Task workers, so the fork really runs on several threads. The
    // process-wide scheduler is started once (the FluidVisualEvidenceTest
    // shape) and never stopped — StopWorkers vs standby workers is the
    // recurring race class (notes-core-and-threading.md §6).
    void EnsureTaskWorkers()
    {
        static const bool s_Once = []
        {
            if (LowLevelTasks::FScheduler::Get().GetNumWorkers() == 0u)
            {
                LowLevelTasks::InitGameThreadId();
                Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::GameThread);
                LowLevelTasks::FScheduler::Get().StartWorkers();
            }
            return true;
        }();
        (void)s_Once;
    }

    struct TintedTarget
    {
        Ref<Framebuffer> Target;
        Ref<UniformBuffer> Tint; ///< One per item — amendment (92) rule 6.
        std::array<f32, 4> Color{};
    };
} // namespace

class VulkanParallelRecordingDevice : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto gate = ProbeVulkanDeviceTestGate();
        if (!gate.Available)
            GTEST_SKIP() << gate.Reason;

        m_Device = std::make_unique<VulkanDevice>();
        try
        {
            m_Device->Init([](VkInstance)
                           { return VK_NULL_HANDLE; });
        }
        catch (const std::exception& e)
        {
            m_Device.reset();
            GTEST_SKIP() << "Vulkan bring-up refused on a contract-satisfying machine: " << e.what();
        }

        VulkanDevice::ResetValidationErrorCount();

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_Device->GetCommandPool();
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        ASSERT_EQ(vkAllocateCommandBuffers(m_Device->GetDevice(), &allocInfo, &m_Cmd), VK_SUCCESS);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        ASSERT_EQ(vkCreateFence(m_Device->GetDevice(), &fenceInfo, nullptr, &m_Fence), VK_SUCCESS);
    }

    void TearDown() override
    {
        Levers::SetVulkanParallelRecording(Levers::Tristate::Unset);
        if (!m_Device)
            return;
        vkDeviceWaitIdle(m_Device->GetDevice());
        VulkanPipelineBuilder::Get().ReleaseAll();
        VulkanPipelineCache::Get().SaveAndDestroy();
        VulkanFrameArena::Get().ReleaseBuffers();
        VulkanSecondaryCommandPools::Get().ReleaseAll();
        VulkanResourceHeap::Get().Release();
        VulkanDeferredReclaim::Get().FlushAll();
        if (m_Fence != VK_NULL_HANDLE)
            vkDestroyFence(m_Device->GetDevice(), m_Fence, nullptr);
        EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
            << "Zero validation errors (sync validation included in debug builds)";
        m_Device->Shutdown();
        m_Device.reset();
    }

    void SubmitFrame(VulkanRendererAPI& api, const std::function<void()>& work)
    {
        ASSERT_EQ(vkResetCommandBuffer(m_Cmd, 0), VK_SUCCESS);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ASSERT_EQ(vkBeginCommandBuffer(m_Cmd, &beginInfo), VK_SUCCESS);

        api.BeginRecording(m_Cmd);
        work();
        api.EndRecording();

        ASSERT_EQ(vkEndCommandBuffer(m_Cmd), VK_SUCCESS);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &m_Cmd;
        ASSERT_EQ(vkResetFences(m_Device->GetDevice(), 1, &m_Fence), VK_SUCCESS);
        ASSERT_EQ(vkQueueSubmit(m_Device->GetQueue(), 1, &submit, m_Fence), VK_SUCCESS);
        ASSERT_EQ(vkWaitForFences(m_Device->GetDevice(), 1, &m_Fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
        VulkanDeferredReclaim::Get().NotifyFrameCompleted();
    }

    // The shared draw kit: a fullscreen triangle, a white 4x4 texel, the
    // §5f shader. Created on the render thread, outside any region.
    struct DrawKit
    {
        Ref<VertexArray> Triangle;
        Ref<Texture2D> White;
        Ref<VulkanShader> Shader;
    };
    [[nodiscard]] DrawKit MakeDrawKit()
    {
        DrawKit kit;
        const f32 vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
        auto vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
        u32 indices[] = { 0, 1, 2 };
        auto indexBuffer = IndexBuffer::Create(indices, 3);
        kit.Triangle = VertexArray::Create();
        kit.Triangle->AddVertexBuffer(vertexBuffer);
        kit.Triangle->SetIndexBuffer(indexBuffer);

        TextureSpecification texSpec;
        texSpec.Width = 4;
        texSpec.Height = 4;
        texSpec.Format = ImageFormat::RGBA8;
        texSpec.GenerateMips = false;
        kit.White = Texture2D::Create(texSpec);
        std::vector<u8> white(4 * 4 * 4, 0xFF);
        kit.White->SetData(white.data(), static_cast<u32>(white.size()));

        kit.Shader = Ref<VulkanShader>::Create("ParallelRecordingTriangle", kVertexSrc, kFragmentSrc);
        return kit;
    }

    [[nodiscard]] static TintedTarget MakeTintedTarget(const u32 size, const std::array<f32, 4>& color)
    {
        TintedTarget target;
        FramebufferSpecification fbSpec;
        fbSpec.Width = size;
        fbSpec.Height = size;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        target.Target = Framebuffer::Create(fbSpec);
        target.Tint = UniformBuffer::Create(16, 3);
        target.Tint->SetData(color.data(), sizeof(f32) * 4);
        target.Color = color;
        return target;
    }

    // Count the pixels of `target`'s attachment that are NOT `color` (8-bit,
    // exact: the tint is 0/1 per channel and the texel is white).
    [[nodiscard]] static u32 CountWrongPixels(const Ref<Framebuffer>& target, const std::array<f32, 4>& color,
                                              const u32 x0, const u32 x1, const u32 attachmentIndex = 0)
    {
        const auto* vkFramebuffer = static_cast<const VulkanFramebuffer*>(target.Raw());
        const auto attachment = vkFramebuffer->GetColorAttachmentImage(attachmentIndex);
        if (attachment == nullptr)
            return 0xFFFFFFFFu;
        std::vector<u8> pixels;
        if (!attachment->GetData(pixels, 0))
            return 0xFFFFFFFFu;
        const u32 width = target->GetSpecification().Width;
        const u32 height = target->GetSpecification().Height;
        if (pixels.size() != static_cast<sizet>(width) * height * 4u)
            return 0xFFFFFFFFu;
        u32 wrong = 0;
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = x0; x < x1; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * width + x) * 4u;
                for (u32 c = 0; c < 4; ++c)
                {
                    const u8 expected = color[c] > 0.5f ? 0xFF : 0x00;
                    if (pixels[i + c] != expected)
                    {
                        ++wrong;
                        break;
                    }
                }
            }
        }
        return wrong;
    }

    // The graph's job, done by hand: every target to COLOR_ATTACHMENT before
    // `work` writes it, and to sampled afterwards so the readback transitions
    // from the layout the tracker knows.
    void SubmitPassFrame(VulkanRendererAPI& api, std::span<const Ref<Framebuffer>> targets,
                         const std::function<void()>& work)
    {
        SubmitFrame(api,
                    [&]()
                    {
                        for (const auto& target : targets)
                        {
                            RHI::Barrier toColor{};
                            toColor.Resource = target->GetColorAttachmentHandle(0);
                            toColor.Before = RHI::Access::Undefined;
                            toColor.After = RHI::Access::ColorAttachmentWrite;
                            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toColor, 1 });
                        }
                        work();
                        for (const auto& target : targets)
                        {
                            RHI::Barrier toSampled{};
                            toSampled.Resource = target->GetColorAttachmentHandle(0);
                            toSampled.Before = RHI::Access::ColorAttachmentWrite;
                            toSampled.After = RHI::Access::ShaderSampleRead;
                            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                        }
                    });
    }

    // The unmodified GL-shaped item body: bind, clear, draw the tinted
    // triangle, unbind.
    static void DrawTintedTriangle(VulkanRendererAPI& api, const DrawKit& kit, TintedTarget& target, const u32 size)
    {
        target.Target->Bind();
        api.SetViewport(0, 0, size, size);
        api.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        api.Clear();
        kit.Shader->Bind();
        target.Tint->Bind();
        api.BindTexture(0, kit.White->GetRHIHandle());
        api.DrawIndexed(kit.Triangle, 3);
        target.Target->Unbind();
    }

    std::unique_ptr<VulkanDevice> m_Device;
    VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
    VkFence m_Fence = VK_NULL_HANDLE;
};

TEST_F(VulkanParallelRecordingDevice, GraphRecordsSharedUnboundUniformAndTimesOrderedPasses)
{
    // This test asserts on the EXACT set of pass timings it recorded, so it
    // needs a pool of its own. `Initialize` early-outs when the pool is already
    // up, so in a single-process run it would otherwise inherit the renderer's
    // pool and read that frame's ~16 accumulated timings instead of its 2
    // (issue #1074).
    //
    // Released HERE, before the Vulkan backend is selected below, because
    // `Shutdown` destroys its query objects through `RenderCommand` — the
    // renderer's queries are GL objects and must be freed on the GL backend that
    // created them, not through a Vulkan device that never owned them.
    // `Renderer3D::BeginScene` re-creates the pool on its own backend when it
    // next draws.
    GPUPassTimerPool::GetInstance().Shutdown();

    ScopedVulkanRenderCommandSelection renderCommandSelection;
    EnsureTaskWorkers();
    auto& api = renderCommandSelection.Get();
    const DrawKit kit = MakeDrawKit();
    // The production fullscreen primitive has five floats per vertex.
    std::string vertexSource(kVertexSrc);
    vertexSource.replace(vertexSource.find("gl_VertexIndex * 2"), std::string("gl_VertexIndex * 2").size(), "gl_VertexIndex * 5");
    auto shader = Ref<VulkanShader>::Create("PreparedSharedUniform", vertexSource, kFragmentSrc);
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    auto firstTarget = MakeTintedTarget(32, { 1, 0, 0, 1 });
    auto secondTarget = MakeTintedTarget(32, { 0, 0, 0, 1 });

    auto mrtSpec = secondTarget.Target->GetSpecification();
    mrtSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RGBA8 };
    secondTarget.Target = Framebuffer::Create(mrtSpec);
    std::string mrtFragment(kFragmentSrc);
    mrtFragment.insert(mrtFragment.find("layout(binding"), "layout(location = 1) out vec4 o_Second;\n");
    mrtFragment.insert(mrtFragment.find("    o_Color ="), "    o_Second = vec4(0, 1, 0, 1);\n");
    auto mrtShader = Ref<VulkanShader>::Create("PreparedMRT", vertexSource, mrtFragment);

    class FullscreenNode final : public RenderGraphNode
    {
      public:
        Ref<Framebuffer> Target;
        Ref<Shader> Program;
        Ref<UniformBuffer> Uniform;
        RHI::ResourceHandle Texture;
        std::vector<u32> DrawBuffers{ 0u };
        bool SupportsWholePassRecording() const noexcept override
        {
            return true;
        }
        RGPreparedPass PrepareParallelRecording(RGCommandContext&) override
        {
            return PrepareFullscreenPass(Target, Program, { { 0, Texture, "u_Texture", RHI::HeapSlotLifetime::Persistent } }, { Uniform }, true, DrawBuffers);
        }
        void Execute(RGCommandContext& context) override
        {
            auto prepared = PrepareParallelRecording(context);
            prepared.Record(context);
        }
    } first, second;
    first.SetName("PreparedFirst");
    second.SetName("PreparedSecond");
    first.Target = firstTarget.Target;
    second.Target = secondTarget.Target;
    for (auto* node : { &first, &second })
    {
        node->Program = shader;
        node->Uniform = firstTarget.Tint;
        node->Texture = kit.White->GetRHIHandle();
    }
    second.Program = mrtShader;
    second.DrawBuffers = { 0u, 1u };
    const std::vector<std::string> order{ first.GetName(), second.GetName() };
    const auto plan = RenderGraphSubmissionPlan::BuildPlan({ .ExecutionOrder = order, .Dependencies = {}, .PlannedBarriers = {}, .Transitions = {}, .Batches = {}, .EnableSplitBarriers = false, .GetPassWorkType = [](const std::string&)
                                                                                                                                                                                                 { return RenderGraphPassWorkType::Graphics; },
                                                             .ResolveNodePointer = [&](const std::string& name) -> RenderGraphNode*
                                                             { return name == first.GetName() ? &first : &second; } });
    ASSERT_EQ(plan.size(), 2u);
    ASSERT_EQ(plan[0].RecordingGroup, plan[1].RecordingGroup);
    ASSERT_NE(plan[0].RecordingGroup, UINT32_MAX);
    // MeshPrimitives::Shutdown stays UNCONDITIONAL despite being a process-wide
    // singleton the renderer also owns: the primitives this test builds are
    // backed by Vulkan buffers, and they must be released before this test's
    // Vulkan device tears its memory blocks down. Skipping it trades the
    // cross-test leak for a hard VMA abort — "Some allocations were not freed
    // before destruction of this memory block". The primitive cache rebuilds
    // lazily, so the later-test cost is a rebuild rather than a dead singleton.
    // Initialize unconditionally: this test asserts on the EXACT set of pass
    // timings it recorded, so it needs a pool of its own. Inheriting the
    // renderer's carries ~16 accumulated timings and the assertion reads 18
    // instead of 2.
    //
    // Shutdown likewise stays unconditional — the pool's query objects belong to
    // the Vulkan device selected here. Leaving it dead used to stop every later
    // GPU-timing test from measuring anything (issue #1074);
    // `Renderer3D::BeginScene` now re-initializes it when it next draws, on the
    // renderer's own backend rather than on this device.
    auto& timers = GPUPassTimerPool::GetInstance();
    timers.Initialize(8);
    struct Cleanup
    {
        ~Cleanup()
        {
            GPUPassTimerPool::GetInstance().Shutdown();
            MeshPrimitives::Shutdown();
        }
    } cleanup;
    std::array targets{ first.Target, second.Target };
    RGCommandContext context;
    for (u32 frame = 0; frame < 3; ++frame)
    {
        VulkanFrameArena::Get().BeginFrame(0);
        SubmitPassFrame(api, targets, [&]
                        {
            // Displace the shared red UBO with a black UBO at the SAME binding.
            // Seed-only priming cannot reach the prepared passes' shared input.
            secondTarget.Tint->Bind();
            RHI::Barrier extraTarget{};
            extraTarget.Resource = second.Target->GetColorAttachmentHandle(1);
            extraTarget.Before = RHI::Access::Undefined;
            extraTarget.After = RHI::Access::ColorAttachmentWrite;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &extraTarget, 1 });
            timers.BeginFrame();
            const auto cpu = RenderGraphPlanExecutor::ExecutePlan({
                .SubmissionPlan = plan, .Context = context, .RuntimeBarrierExecutionEnabled = false,
                .IsPassReachable = [](const std::string&) { return true; } });
            EXPECT_EQ(cpu.size(), 2u);
            timers.EndFrame();
            extraTarget.Before = RHI::Access::ColorAttachmentWrite;
            extraTarget.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &extraTarget, 1 }); });
    }
    EXPECT_EQ(CountWrongPixels(first.Target, firstTarget.Color, 0, 32), 0u);
    EXPECT_EQ(CountWrongPixels(second.Target, firstTarget.Color, 0, 32), 0u);
    EXPECT_EQ(CountWrongPixels(second.Target, { 0, 1, 0, 1 }, 0, 32, 1), 0u);
    const auto stats = api.GetParallelRecordingStats();
    ASSERT_EQ(stats.Regions, 1u);
    EXPECT_EQ(stats.SecondariesExecuted, 2u);
    EXPECT_EQ(stats.MergeConflicts, 0u);
    ASSERT_EQ(stats.RegionTimings.size(), 1u);
    EXPECT_EQ(stats.RegionTimings[0].ItemPassNames, order);
    const auto gpu = timers.GetLastPassTimingsCopy();
    ASSERT_EQ(gpu.size(), 2u);
    EXPECT_EQ(gpu[0].Name, first.GetName());
    EXPECT_EQ(gpu[1].Name, second.GetName());
}

TEST_F(VulkanParallelRecordingDevice, MeshParticleRangesMatchInlinePixels)
{
    struct RestoreDirectory
    {
        std::filesystem::path Previous = std::filesystem::current_path();
        ~RestoreDirectory()
        {
            std::filesystem::current_path(Previous);
        }
    } restoreDirectory;
    if (std::filesystem::exists("OloEditor/assets/shaders/Particle_Mesh.glsl"))
        std::filesystem::current_path("OloEditor");
    ASSERT_TRUE(std::filesystem::exists("assets/shaders/Particle_Mesh.glsl"));

    ScopedVulkanRenderCommandSelection renderCommandSelection;
    EnsureTaskWorkers();
    auto& api = renderCommandSelection.Get();
    // Init/Shutdown both stay unconditional: this builds the batch renderer's
    // buffers against the VULKAN backend selected just above, so reusing the GL
    // renderer's instance would draw Vulkan work through GL-backed buffers, and
    // skipping the Shutdown strands Vulkan allocations past device teardown
    // ("Some allocations were not freed before destruction of this memory
    // block"). Rebuilding the GL renderer's instance here would not help either:
    // VulkanPassSuiteTest's own fixture shuts ParticleBatchRenderer down again
    // a few suites later, so the durable fix for particle rendering after a
    // Vulkan excursion belongs there, not here.
    ParticleBatchRenderer::Init();
    struct Cleanup
    {
        ~Cleanup()
        {
            ParticleBatchRenderer::Shutdown();
        }
    } cleanup;
    auto cube = MeshPrimitives::CreateCube();
    ASSERT_TRUE(cube && cube->IsValid());
    auto target = MakeTintedTarget(32, { 1, 1, 1, 1 });
    FramebufferSpecification particleTarget;
    particleTarget.Width = particleTarget.Height = 32;
    particleTarget.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER,
                                   FramebufferTextureFormat::RG16F, FramebufferTextureFormat::RG16F };
    target.Target = Framebuffer::Create(particleTarget);
    std::array targets{ target.Target };
    std::array<MeshParticleInstance, 96> instances;
    for (u32 i = 0; i < instances.size(); ++i)
    {
        auto& instance = instances[i];
        instance.Model = glm::mat4(0.14f);
        instance.Model[3] = glm::vec4(-0.88f + static_cast<f32>(i % 12) * 0.16f,
                                      -0.7f + static_cast<f32>(i / 12) * 0.2f, 0.5f, 1.0f);
        instance.PrevModel = instance.Model;
        instance.Color = i < 32 ? glm::vec4(1, 0, 0, 1) : (i < 64 ? glm::vec4(0, 1, 0, 1) : glm::vec4(0, 0, 1, 1));
        instance.IDs = glm::ivec4(static_cast<i32>(i), 0, 0, 0);
    }
    std::vector<u8> reference;
    for (const auto lever : { Levers::Tristate::Off, Levers::Tristate::On })
    {
        Levers::SetVulkanParallelRecording(lever);
        VulkanFrameArena::Get().BeginFrame(0);
        SubmitPassFrame(api, targets, [&]
                        {
            target.Target->Bind();
            api.SetViewport(0, 0, 32, 32);
            api.SetDepthTest(false);
            api.SetDepthMask(false);
            api.SetBlendState(false);
            api.DisableCulling();
            api.SetClearColor({ 0, 0, 0, 1 });
            api.Clear();
            ParticleBatchRenderer::BeginBatch(Camera(glm::mat4(1.0f)), glm::mat4(1.0f));
            ParticleBatchRenderer::RenderMeshParticles(cube, instances, nullptr);
            target.Target->Unbind(); });
        const auto* framebuffer = static_cast<const VulkanFramebuffer*>(target.Target.Raw());
        std::vector<u8> pixels;
        ASSERT_TRUE(framebuffer->GetColorAttachmentImage(0)->GetData(pixels, 0));
        if (lever == Levers::Tristate::Off)
        {
            reference = pixels;
            std::array<u32, 3> colorPixels{};
            for (sizet i = 0; i < pixels.size(); i += 4)
                for (u32 channel = 0; channel < 3; ++channel)
                    colorPixels[channel] += pixels[i + channel] > 128 ? 1u : 0u;
            for (const auto count : colorPixels)
                EXPECT_GT(count, 20u) << "each range must contribute visible particles";
        }
        else
        {
            EXPECT_EQ(pixels, reference);
            const auto stats = api.GetParallelRecordingStats();
            EXPECT_EQ(stats.Regions, 1u);
            EXPECT_EQ(stats.SecondariesExecuted, 3u);
            EXPECT_EQ(stats.MergeConflicts, 0u);
        }
    }
}

// Four items, four targets, one vkCmdExecuteCommands. Each item binds its own
// framebuffer, clears, and draws with its own tint UBO (rule 6); the join
// executes them in item order; every target reads back its own colour.
TEST_F(VulkanParallelRecordingDevice, ForkRecordsEachItemIntoItsOwnTargetThroughOneExecute)
{
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    EnsureTaskWorkers();
    VulkanFrameArena::Get().BeginFrame(0);

    const DrawKit kit = MakeDrawKit();
    ASSERT_EQ(kit.Shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    constexpr u32 kItems = 4;
    const std::array<std::array<f32, 4>, kItems> colors = { { { 1.0f, 0.0f, 0.0f, 1.0f },
                                                              { 0.0f, 1.0f, 0.0f, 1.0f },
                                                              { 0.0f, 0.0f, 1.0f, 1.0f },
                                                              { 1.0f, 1.0f, 0.0f, 1.0f } } };
    std::vector<TintedTarget> targets;
    for (const auto& color : colors)
        targets.push_back(MakeTintedTarget(32, color));

    VulkanRendererAPI& api = renderCommandSelection.Get();
    std::atomic<u32> itemsRun{ 0 };
    std::atomic<u32> itemsOnWorkerContext{ 0 };
    std::mutex threadsMutex;
    std::vector<std::thread::id> threads;

    std::vector<Ref<Framebuffer>> framebuffers;
    for (const auto& target : targets)
        framebuffers.push_back(target.Target);
    SubmitPassFrame(api, framebuffers,
                    [&]()
                    {
                        ASSERT_TRUE(api.SupportsParallelRecording());
                        api.RecordParallel(kItems,
                                           [&](const u32 item)
                                           {
                                               itemsRun.fetch_add(1u, std::memory_order_relaxed);
                                               if (CurrentVulkanWorkerContext() != nullptr)
                                                   itemsOnWorkerContext.fetch_add(1u, std::memory_order_relaxed);
                                               {
                                                   const std::scoped_lock lock(threadsMutex);
                                                   threads.push_back(std::this_thread::get_id());
                                               }
                                               DrawTintedTriangle(api, kit, targets[item], 32);
                                           });
                    });

    EXPECT_EQ(itemsRun.load(), kItems);
    EXPECT_EQ(itemsOnWorkerContext.load(), kItems) << "every item must record on a worker context";
    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u) << "the draw path must not fall through to a stub";
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), kItems) << "the items' tallies are summed into the frame's";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    const auto stats = api.GetParallelRecordingStats();
    EXPECT_EQ(stats.Regions, 1u);
    EXPECT_EQ(stats.InlineRegions, 0u);
    EXPECT_EQ(stats.SecondariesExecuted, kItems);
    EXPECT_EQ(stats.MergeConflicts, 0u);
    EXPECT_GT(stats.RegionWallMs, 0.0);

    for (u32 item = 0; item < kItems; ++item)
    {
        EXPECT_EQ(CountWrongPixels(targets[item].Target, colors[item], 0, 32), 0u)
            << "target " << item << " must hold its own item's tint";
    }
    // Diagnostic, not asserted: how many distinct threads recorded. A box
    // with one core (or a scheduler that never started) records everything
    // on the caller and the result above is still required to hold.
    std::sort(threads.begin(), threads.end());
    const auto distinct = static_cast<u32>(std::unique(threads.begin(), threads.end()) - threads.begin());
    OLO_CORE_INFO("[VulkanParallelRecordingTest] {} items recorded on {} thread(s), {} worker(s) in the scheduler",
                  kItems, distinct, LowLevelTasks::FScheduler::Get().GetNumWorkers());
}

// Bucket replay must isolate both the material UBO and the model-instance
// SSBO, even when the same material index is replayed repeatedly on an item.
TEST_F(VulkanParallelRecordingDevice, BucketsReplayWithItemOwnedMaterialAndInstanceUploads)
{
    // The Shutdown below is NOT optional: this test's dispatcher state is bound
    // to buffers allocated against the Vulkan device selected here, and they
    // must be released before that device tears its memory blocks down.
    // Skipping it aborts the process on a VMA assertion ("Some allocations were
    // not freed before destruction of this memory block").
    //
    // It also drops the shared camera / material / bone UBOs that
    // `Renderer3D::Init` publishes exactly once, which used to leave every later
    // rendering test in a single-process run drawing with no camera or material
    // UBO bound (issue #1074). Nothing is restored here on purpose: republishing
    // at this point would leave GL-currency handles in the dispatcher across the
    // Vulkan suites that follow — the hazard VulkanPassSuiteTest documents.
    // `Renderer3D::BeginScene` re-homes its own buffers when it next draws.
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    EnsureTaskWorkers();
    VulkanFrameArena::Get().BeginFrame(0);
    CommandDispatch::Initialize();
    const bool ownsFrameData = !FrameDataBufferManager::IsInitialized();
    if (ownsFrameData)
        FrameDataBufferManager::Init();
    struct Restore
    {
        bool OwnsFrameData;
        ~Restore()
        {
            CommandDispatch::Shutdown();
            if (OwnsFrameData)
                FrameDataBufferManager::Shutdown();
        }
    } restore{ ownsFrameData };
    auto& frameData = FrameDataBufferManager::Get();
    frameData.Reset();

    auto kit = MakeDrawKit();
    std::string fragment = kFragmentSrc;
    fragment.replace(fragment.find("binding = 3"), std::string("binding = 3").size(), "binding = 2");
    fragment.insert(fragment.find("void main()"),
                    "layout(std430, binding = 15) readonly buffer InstanceWords { float words[]; };\n");
    const std::string output = "o_Color = texture(u_Texture, v_TexCoord) * u_Tint;";
    fragment.replace(fragment.find(output), output.size(),
                     "o_Color = abs(words[0] - u_Tint.a) < 0.25 ? vec4(texture(u_Texture, v_TexCoord).rgb * u_Tint.rgb, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);");
    kit.Shader = Ref<VulkanShader>::Create("ParallelBucketUploads", kVertexSrc, fragment);
    ASSERT_EQ(kit.Shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    auto materialBuffer = UniformBuffer::Create(ShaderBindingLayout::PBRMaterialUBO::GetSize(), ShaderBindingLayout::UBO_MATERIAL);
    auto instances = Ref<InstanceBuffer>::Create(1u);
    CommandDispatch::SetUBOReferences(nullptr, materialBuffer, nullptr, instances);

    constexpr u32 itemCount = 4;
    const std::array<std::array<f32, 4>, itemCount> colors = { { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 0, 1 } } };
    std::array<CommandBucket, itemCount> buckets;
    CommandAllocator allocator;
    std::vector<TintedTarget> targets;
    std::vector<Ref<Framebuffer>> framebuffers;
    PODRenderState state{};
    state.depthTestEnabled = false;
    state.depthWriteMask = false;
    const u16 stateIndex = frameData.AllocateRenderState(state);
    for (u32 item = 0; item < itemCount; ++item)
    {
        targets.push_back(MakeTintedTarget(32, colors[item]));
        framebuffers.push_back(targets.back().Target);
        PODMaterialData material{};
        material.enablePBR = true;
        material.shaderRendererID = kit.Shader->GetRHIHandle();
        material.albedoMapID = kit.White->GetRHIHandle();
        material.baseColorFactor = glm::vec4(colors[item][0], colors[item][1], colors[item][2], static_cast<f32>(item + 1u));
        const u16 materialIndex = frameData.AllocateMaterialData(material);
        ASSERT_NE(materialIndex, INVALID_MATERIAL_DATA_INDEX);
        DrawMeshCommand draw{};
        draw.header.type = CommandType::DrawMesh;
        draw.vertexArrayID = kit.Triangle->GetRHIHandle();
        draw.indexCount = 3;
        draw.transform = glm::mat4(1.0f);
        draw.transform[0][0] = static_cast<f32>(item + 1u);
        draw.prevTransform = draw.transform;
        draw.materialDataIndex = materialIndex;
        draw.renderStateIndex = stateIndex;
        ASSERT_NE(buckets[item].Submit(draw, {}, &allocator), nullptr);
        ASSERT_NE(buckets[item].Submit(draw, {}, &allocator), nullptr);
    }
    auto& api = renderCommandSelection.Get();
    // A newly generated cloud-shadow image has only a storage binding. Scene
    // preparation must publish its sampled binding before PBR packet workers
    // encounter it; the fork cannot discover an input that was never seeded.
    const auto cloudShadow = api.CreateTexture2DHandle(4u, 4u, RHI::Format::R32Float);
    CommandDispatch::SetCloudShadowTexture(cloudShadow);
    SubmitPassFrame(api, framebuffers, [&]
                    {
        api.BindImageTexture(0u, cloudShadow, 0u, false, 0u, RHI::Access::StorageWrite, RHI::Format::R32Float);
        CommandDispatch::BindSceneResources();
        api.RecordParallel(itemCount, [&](u32 item)
                                         {
            targets[item].Target->Bind();
            api.SetViewport(0, 0, 32, 32);
            api.SetClearColor({0, 0, 0, 1});
            api.Clear();
            // Both public entry points must be legal on an item. Timed replay
            // falls back to CPU item timing without opening worker GPU queries.
            if (item % 2u == 0u)
                buckets[item].Execute(api);
            else
                buckets[item].ExecuteWithGPUTiming(api);
            targets[item].Target->Unbind(); }); });
    EXPECT_EQ(api.GetParallelRecordingStats().SecondariesExecuted, itemCount);
    EXPECT_EQ(api.GetParallelRecordingStats().MergeConflicts, 0u);
    EXPECT_EQ(CommandDispatch::GetStatistics().DrawCalls, itemCount * 2u);
    for (u32 item = 0; item < itemCount; ++item)
        EXPECT_EQ(CountWrongPixels(targets[item].Target, colors[item], 0, 32), 0u) << item;

    // The same packets can also be partitioned within one bucket. Each range
    // changes materials repeatedly, and the final packet must win on a shared
    // target regardless of CPU completion order.
    CommandBucket combined;
    for (u32 draw = 0; draw < 128u; ++draw)
        combined.AddCommand(buckets[draw % itemCount].GetPackets()[0]);
    VulkanFrameArena::Get().BeginFrame(1);
    const std::array<Ref<Framebuffer>, 1> sharedTarget{ targets[0].Target };
    SubmitPassFrame(api, sharedTarget, [&]
                    {
        targets[0].Target->Bind();
        api.SetViewport(0, 0, 32, 32);
        api.SetClearColor({ 0, 0, 0, 1 });
        api.Clear();
        combined.ExecuteParallel(api);
        targets[0].Target->Unbind(); });
    EXPECT_EQ(combined.GetStatistics().DrawCalls, 128u);
    EXPECT_EQ(api.GetParallelRecordingStats().SecondariesExecuted, 4u);
    EXPECT_EQ(api.GetParallelRecordingStats().MergeConflicts, 0u);
    EXPECT_EQ(CountWrongPixels(targets[0].Target, colors.back(), 0, 32), 0u);
    CommandDispatch::SetCloudShadowTexture({});
    api.DeleteTexture(cloudShadow);
}

// The shadow-atlas shape: two items into ONE target, separated by viewport.
// The clear happens before the fork (it covers both items' pixels), the fork
// pre-transitions the attachment, and each item's scope-open is an identity
// transition — so no conflict, and both halves hold their own tint.
TEST_F(VulkanParallelRecordingDevice, SharedTargetItemsOpenIdentityScopesWithoutConflict)
{
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    EnsureTaskWorkers();
    VulkanFrameArena::Get().BeginFrame(0);

    const DrawKit kit = MakeDrawKit();
    ASSERT_EQ(kit.Shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    const std::array<f32, 4> left = { 1.0f, 0.0f, 0.0f, 1.0f };
    const std::array<f32, 4> right = { 0.0f, 0.0f, 1.0f, 1.0f };
    TintedTarget shared = MakeTintedTarget(32, left);
    auto rightTint = UniformBuffer::Create(16, 3);
    rightTint->SetData(right.data(), sizeof(f32) * 4);

    VulkanRendererAPI& api = renderCommandSelection.Get();
    const std::array<Ref<Framebuffer>, 1> framebuffers = { shared.Target };
    SubmitPassFrame(api, framebuffers,
                    [&]()
                    {
                        // Pre-fork: the target and the whole-target clear.
                        shared.Target->Bind();
                        // The sampled source was published before a GPU write.
                        // Both items must inherit the settled read layout. A
                        // stale sampler of the output must not undo its fork
                        // attachment transition.
                        api.BindTexture(0, kit.White->GetRHIHandle());
                        // Storage bindings can retain a slot that the heap has
                        // since recycled as sampled. Its actual descriptor kind
                        // must prevent it from hiding this read input at fork.
                        auto& bindings = VulkanBindingState::Global();
                        const u32 previousImageSlot = bindings.GetImageHeapSlot(31u);
                        bindings.SetImageHeapSlot(31u, bindings.GetTextureHeapSlot(0u));
                        api.ClearTextureFloat(kit.White->GetRHIHandle(), 0, { 1, 1, 1, 1 });
                        api.BindTexture(1, shared.Target->GetColorAttachmentHandle(0));
                        api.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
                        api.Clear();

                        api.PushDebugGroup(0, "SharedTargetTest");
                        api.RecordParallel(2,
                                           [&](const u32 item)
                                           {
                                               api.SetViewport(item * 16u, 0, 16, 32);
                                               kit.Shader->Bind();
                                               (item == 0 ? shared.Tint : rightTint)->Bind();
                                               api.BindTexture(0, kit.White->GetRHIHandle());
                                               api.DrawIndexed(kit.Triangle, 3);
                                           });
                        api.PopDebugGroup();
                        bindings.SetImageHeapSlot(31u, previousImageSlot);
                        shared.Target->Unbind();
                    });

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u);
    const auto stats = api.GetParallelRecordingStats();
    EXPECT_EQ(stats.Regions, 1u);
    EXPECT_EQ(stats.SecondariesExecuted, 2u);
    EXPECT_EQ(stats.MergeConflicts, 0u) << "identity scope-opens on a shared attachment are not a conflict";
    ASSERT_EQ(stats.RegionTimings.size(), 1u);
    EXPECT_EQ(stats.RegionTimings[0].PassName, "SharedTargetTest");
    EXPECT_TRUE(stats.RegionTimings[0].Parallel);
    EXPECT_EQ(stats.RegionTimings[0].ItemRecordMs.size(), 2u);
    EXPECT_GE(stats.RegionTimings[0].JoinWaitMs, 0.0);
    EXPECT_EQ(CountWrongPixels(shared.Target, left, 0, 16), 0u) << "left half: item 0's tint";
    EXPECT_EQ(CountWrongPixels(shared.Target, right, 16, 32), 0u) << "right half: item 1's tint";
}

// The lever is the one-thread A/B: forced off, the same call records inline
// on the render thread and the frame is unchanged.
TEST_F(VulkanParallelRecordingDevice, LeverOffRecordsInlineOnTheRenderThread)
{
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    VulkanFrameArena::Get().BeginFrame(0);
    Levers::SetVulkanParallelRecording(Levers::Tristate::Off);

    const DrawKit kit = MakeDrawKit();
    ASSERT_EQ(kit.Shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    const std::array<f32, 4> green = { 0.0f, 1.0f, 0.0f, 1.0f };
    TintedTarget target = MakeTintedTarget(16, green);

    VulkanRendererAPI& api = renderCommandSelection.Get();
    u32 itemsRun = 0;
    u32 itemsOnWorker = 0;
    const std::array<Ref<Framebuffer>, 1> framebuffers = { target.Target };
    SubmitPassFrame(api, framebuffers,
                    [&]()
                    {
                        EXPECT_FALSE(api.SupportsParallelRecording());
                        api.RecordParallel(3,
                                           [&](const u32 item)
                                           {
                                               ++itemsRun;
                                               itemsOnWorker += CurrentVulkanWorkerContext() != nullptr ? 1u : 0u;
                                               if (item == 0)
                                                   DrawTintedTriangle(api, kit, target, 16);
                                           });
                    });

    EXPECT_EQ(itemsRun, 3u);
    EXPECT_EQ(itemsOnWorker, 0u);
    const auto stats = api.GetParallelRecordingStats();
    EXPECT_EQ(stats.Regions, 0u);
    EXPECT_EQ(stats.InlineRegions, 1u);
    EXPECT_EQ(stats.SecondariesExecuted, 0u);
    EXPECT_EQ(CountWrongPixels(target.Target, green, 0, 16), 0u);
}

// The frame arena's claim is lock-free: ranges claimed from real threads at
// once never overlap, and the tallies see every claim.
TEST_F(VulkanParallelRecordingDevice, FrameArenaClaimsFromSeveralThreadsNeverOverlap)
{
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    auto& arena = VulkanFrameArena::Get();
    arena.BeginFrame(0);

    constexpr u32 kThreads = 6;
    constexpr u32 kClaimsPerThread = 512;
    struct Claim
    {
        u64 Offset;
        u64 Size;
    };
    std::array<std::vector<Claim>, kThreads> claims;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (u32 t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&arena, &claims, t]
            {
                claims[t].reserve(kClaimsPerThread);
                for (u32 i = 0; i < kClaimsPerThread; ++i)
                {
                    const u64 size = 16u + ((t * 37u + i * 11u) % 240u);
                    const auto allocation = arena.Allocate(size, 16);
                    if (!allocation.IsValid())
                        continue;
                    claims[t].push_back(Claim{ allocation.Offset, size });
                }
            });
    }
    for (auto& thread : threads)
        thread.join();

    std::vector<Claim> all;
    for (const auto& perThread : claims)
        all.insert(all.end(), perThread.begin(), perThread.end());
    ASSERT_EQ(all.size(), sizet{ kThreads * kClaimsPerThread }) << "no claim may overflow a 16 MiB slot";
    EXPECT_EQ(arena.GetAllocationCountThisFrame(), u64{ kThreads * kClaimsPerThread });
    std::sort(all.begin(), all.end(), [](const Claim& a, const Claim& b)
              { return a.Offset < b.Offset; });
    u32 overlaps = 0;
    for (sizet i = 1; i < all.size(); ++i)
    {
        if (all[i - 1].Offset + all[i - 1].Size > all[i].Offset)
            ++overlaps;
        if (all[i].Offset % 16u != 0u)
            ++overlaps;
    }
    EXPECT_EQ(overlaps, 0u);
    EXPECT_EQ(arena.GetCurrentSlotUsedBytes(), all.back().Offset + all.back().Size);
}

#endif // OLO_WITH_VULKAN
