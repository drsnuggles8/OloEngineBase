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

#if !OLO_WITH_VULKAN

TEST(VulkanParallelRecording, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
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
// item recorded — the claim table claims the subresource at the first A -> B —
// so the merge counter must agree: a real writer, not an identity overlap.
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
                                              const u32 x0, const u32 x1)
    {
        const auto* vkFramebuffer = static_cast<const VulkanFramebuffer*>(target.Raw());
        const auto attachment = vkFramebuffer->GetColorAttachmentImage(0);
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
                        api.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
                        api.Clear();

                        api.RecordParallel(2,
                                           [&](const u32 item)
                                           {
                                               api.SetViewport(item * 16u, 0, 16, 32);
                                               kit.Shader->Bind();
                                               (item == 0 ? shared.Tint : rightTint)->Bind();
                                               api.BindTexture(0, kit.White->GetRHIHandle());
                                               api.DrawIndexed(kit.Triangle, 3);
                                           });
                        shared.Target->Unbind();
                    });

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u);
    const auto stats = api.GetParallelRecordingStats();
    EXPECT_EQ(stats.Regions, 1u);
    EXPECT_EQ(stats.SecondariesExecuted, 2u);
    EXPECT_EQ(stats.MergeConflicts, 0u) << "identity scope-opens on a shared attachment are not a conflict";
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
