// OLO_TEST_LAYER: plumbing
//
// Headless contract tests for the Vulkan async-compute queue (issue #808):
// the queue-family selection policy and the queue-family OWNERSHIP TRANSFER
// lowering. Both are pure functions over plain structs — no instance, no
// device — so they run in ordinary CI, which is the point: the machines that
// run CI have no discrete GPU, and the answer they exercise ("this device has
// no async compute family, keep everything on graphics") is the degrade path
// the engine must get right first.
//
// The device-gated half — a real graphics -> compute -> graphics round trip
// with the validation layers watching the transfer pairs — lives in
// VulkanAsyncComputeDeviceTest below, behind the usual SKIP ladder.
//
// Why the transfer pair is worth pinning at this layer: an ownership transfer
// that names the two families on only ONE of its halves, or that lets the
// release keep a destination scope, is accepted by NVIDIA and silently
// corrupts content on hardware that really splits the families. There is no
// pixel to look at and no error to read — so the shape of the pair is checked
// here, by construction, rather than hoped for.

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanAsyncCompute, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "Platform/Vulkan/VulkanBarrierLowering.h"
#include "Platform/Vulkan/VulkanQueueSelection.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/GraphicsContext.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TransientPool.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanStorageBuffer.h"

#include "VulkanTestSupport.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <functional>
#include <string>

#include <vector>

namespace
{
    using namespace OloEngine;
    using OloEngine::VulkanQueueSelection::AsyncComputeUnavailableReason;
    using OloEngine::VulkanQueueSelection::SelectAsyncComputeFamily;

    VkQueueFamilyProperties Family(const VkQueueFlags flags, const u32 queueCount = 1,
                                   const u32 timestampValidBits = 64)
    {
        VkQueueFamilyProperties props{};
        props.queueFlags = flags;
        props.queueCount = queueCount;
        props.timestampValidBits = timestampValidBits;
        return props;
    }

    constexpr VkQueueFlags kGraphicsFlags =
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
    constexpr VkQueueFlags kAsyncComputeFlags =
        VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
    constexpr VkQueueFlags kTransferFlags = VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;

    // The RTX 4090's real table, as reported by vulkaninfo on the machine this
    // was developed on. Family 2 is the async compute pipe.
    std::vector<VkQueueFamilyProperties> NvidiaDesktopFamilies()
    {
        return {
            Family(kGraphicsFlags, 16),
            Family(kTransferFlags, 2),
            Family(kAsyncComputeFlags, 8),
            Family(kTransferFlags | VK_QUEUE_VIDEO_DECODE_BIT_KHR, 1, 32),
            Family(kTransferFlags | VK_QUEUE_VIDEO_ENCODE_BIT_KHR, 2, 32),
        };
    }
} // namespace

// --- Queue-family selection policy -----------------------------------------

TEST(VulkanAsyncComputeSelection, PicksTheComputeOnlyFamilyOnADesktopTable)
{
    const auto families = NvidiaDesktopFamilies();
    const auto selection = SelectAsyncComputeFamily(families, 0);

    EXPECT_TRUE(selection.Found);
    EXPECT_EQ(selection.FamilyIndex, 2u);
    EXPECT_EQ(selection.TimestampValidBits, 64u);
    EXPECT_EQ(selection.Reason, AsyncComputeUnavailableReason::None);
}

TEST(VulkanAsyncComputeSelection, ASingleCombinedFamilyHasNoAsyncCompute)
{
    // The degrade path CI actually runs: one family that does everything.
    // Vulkan requires a graphics family to report COMPUTE too, so a naive
    // "any family with COMPUTE" rule would select the graphics family here
    // and buy nothing while claiming a win.
    const std::vector<VkQueueFamilyProperties> families{ Family(kGraphicsFlags, 1) };
    const auto selection = SelectAsyncComputeFamily(families, 0);

    EXPECT_FALSE(selection.Found);
    EXPECT_EQ(selection.Reason, AsyncComputeUnavailableReason::NoComputeOnlyFamily);
}

TEST(VulkanAsyncComputeSelection, NoFamiliesAtAllIsItsOwnReason)
{
    const auto selection = SelectAsyncComputeFamily({}, 0);

    EXPECT_FALSE(selection.Found);
    EXPECT_EQ(selection.Reason, AsyncComputeUnavailableReason::NoDeviceQueueFamilies);
}

TEST(VulkanAsyncComputeSelection, AComputeOnlyFamilyWithNoQueuesIsNamedSeparately)
{
    const std::vector<VkQueueFamilyProperties> families{
        Family(kGraphicsFlags, 1),
        Family(kAsyncComputeFlags, 0),
    };
    const auto selection = SelectAsyncComputeFamily(families, 0);

    EXPECT_FALSE(selection.Found);
    EXPECT_EQ(selection.Reason, AsyncComputeUnavailableReason::FamilyHasNoQueues);
}

TEST(VulkanAsyncComputeSelection, AComputeOnlyFamilyWithoutTimestampsIsNamedSeparately)
{
    const std::vector<VkQueueFamilyProperties> families{
        Family(kGraphicsFlags, 1),
        Family(kAsyncComputeFlags, 8, /*timestampValidBits*/ 0),
    };
    const auto selection = SelectAsyncComputeFamily(families, 0);

    EXPECT_FALSE(selection.Found);
    EXPECT_EQ(selection.Reason, AsyncComputeUnavailableReason::FamilyHasNoTimestamps);
}

TEST(VulkanAsyncComputeSelection, PrefersTheMostDedicatedComputeFamily)
{
    // Two compute-only families; the one carrying fewer other capabilities is
    // the more dedicated hardware pipe.
    const std::vector<VkQueueFamilyProperties> families{
        Family(kGraphicsFlags, 1),
        Family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT |
                   VK_QUEUE_VIDEO_DECODE_BIT_KHR,
               1),
        Family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1),
    };
    const auto selection = SelectAsyncComputeFamily(families, 0);

    EXPECT_TRUE(selection.Found);
    EXPECT_EQ(selection.FamilyIndex, 2u);
}

TEST(VulkanAsyncComputeSelection, TiesGoToTheLowestIndexSoCapturesAreReproducible)
{
    const std::vector<VkQueueFamilyProperties> families{
        Family(kGraphicsFlags, 1),
        Family(kAsyncComputeFlags, 4),
        Family(kAsyncComputeFlags, 4),
    };
    const auto selection = SelectAsyncComputeFamily(families, 0);

    EXPECT_TRUE(selection.Found);
    EXPECT_EQ(selection.FamilyIndex, 1u);
}

TEST(VulkanAsyncComputeSelection, TheGraphicsFamilyIsNeverACandidate)
{
    // A defensive case rather than a real one: even if the graphics family
    // somehow reported no GRAPHICS bit, the family the engine already submits
    // to cannot be its own async pipe.
    const std::vector<VkQueueFamilyProperties> families{
        Family(kAsyncComputeFlags, 8),
        Family(kGraphicsFlags, 1),
    };
    const auto selection = SelectAsyncComputeFamily(families, 0);

    EXPECT_FALSE(selection.Found);
    EXPECT_EQ(selection.Reason, AsyncComputeUnavailableReason::NoComputeOnlyFamily);
}

TEST(VulkanAsyncComputeSelection, EveryReasonHasADistinctDescription)
{
    // The log line and the MCP telemetry both render this enum; an unmapped
    // member would report "unknown" to a user trying to find out why their
    // GPU is not overlapping anything.
    const AsyncComputeUnavailableReason all[] = {
        AsyncComputeUnavailableReason::None,
        AsyncComputeUnavailableReason::NoDeviceQueueFamilies,
        AsyncComputeUnavailableReason::NoComputeOnlyFamily,
        AsyncComputeUnavailableReason::FamilyHasNoQueues,
        AsyncComputeUnavailableReason::FamilyHasNoTimestamps,
        AsyncComputeUnavailableReason::DisabledByLever,
        AsyncComputeUnavailableReason::NoDevice,
    };
    for (const auto reason : all)
        EXPECT_NE(VulkanQueueSelection::Describe(reason), "unknown");
}

// --- Queue-family ownership transfer lowering ------------------------------

namespace
{
    VkImageMemoryBarrier2 SampleImageBarrier()
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(0xA10u));
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 1;
        barrier.subresourceRange.levelCount = 2;
        barrier.subresourceRange.baseArrayLayer = 3;
        barrier.subresourceRange.layerCount = 4;
        return barrier;
    }
} // namespace

TEST(VulkanOwnershipTransfer, BothHalvesNameTheSameFamilyPair)
{
    const auto pair = VulkanBarrierLowering::SplitImageOwnershipTransfer(SampleImageBarrier(), 0, 2);

    EXPECT_EQ(pair.Release.srcQueueFamilyIndex, 0u);
    EXPECT_EQ(pair.Release.dstQueueFamilyIndex, 2u);
    EXPECT_EQ(pair.Acquire.srcQueueFamilyIndex, 0u);
    EXPECT_EQ(pair.Acquire.dstQueueFamilyIndex, 2u);
}

TEST(VulkanOwnershipTransfer, TheReleaseCarriesNoDestinationScope)
{
    // Vulkan §7.7.4: the release operation's destination scope is ignored, and
    // naming one invites the reader to believe the release makes the data
    // visible on the far queue. Only the acquire does that.
    const auto pair = VulkanBarrierLowering::SplitImageOwnershipTransfer(SampleImageBarrier(), 0, 2);

    EXPECT_EQ(pair.Release.srcStageMask, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    EXPECT_EQ(pair.Release.srcAccessMask, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    EXPECT_EQ(pair.Release.dstStageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(pair.Release.dstAccessMask, VK_ACCESS_2_NONE);
}

TEST(VulkanOwnershipTransfer, TheAcquireCarriesNoSourceScope)
{
    const auto pair = VulkanBarrierLowering::SplitImageOwnershipTransfer(SampleImageBarrier(), 0, 2);

    EXPECT_EQ(pair.Acquire.srcStageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(pair.Acquire.srcAccessMask, VK_ACCESS_2_NONE);
    EXPECT_EQ(pair.Acquire.dstStageMask, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    EXPECT_EQ(pair.Acquire.dstAccessMask, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

TEST(VulkanOwnershipTransfer, TheLayoutTransitionIsStatedIdenticallyOnBothHalves)
{
    // The transition happens ONCE, but it must be spelled the same way in both
    // halves or the pair is invalid usage — the single hardest thing to notice
    // by reading a capture, since each half looks correct alone.
    const auto source = SampleImageBarrier();
    const auto pair = VulkanBarrierLowering::SplitImageOwnershipTransfer(source, 0, 2);

    EXPECT_EQ(pair.Release.oldLayout, source.oldLayout);
    EXPECT_EQ(pair.Release.newLayout, source.newLayout);
    EXPECT_EQ(pair.Acquire.oldLayout, source.oldLayout);
    EXPECT_EQ(pair.Acquire.newLayout, source.newLayout);
}

TEST(VulkanOwnershipTransfer, TheSubresourceRangeAndImageAreIdenticalOnBothHalves)
{
    const auto source = SampleImageBarrier();
    const auto pair = VulkanBarrierLowering::SplitImageOwnershipTransfer(source, 0, 2);

    for (const auto& half : { pair.Release, pair.Acquire })
    {
        EXPECT_EQ(half.image, source.image);
        EXPECT_EQ(half.subresourceRange.aspectMask, source.subresourceRange.aspectMask);
        EXPECT_EQ(half.subresourceRange.baseMipLevel, source.subresourceRange.baseMipLevel);
        EXPECT_EQ(half.subresourceRange.levelCount, source.subresourceRange.levelCount);
        EXPECT_EQ(half.subresourceRange.baseArrayLayer, source.subresourceRange.baseArrayLayer);
        EXPECT_EQ(half.subresourceRange.layerCount, source.subresourceRange.layerCount);
    }
}

TEST(VulkanOwnershipTransfer, ReversingTheDirectionSwapsTheFamiliesAndNothingElse)
{
    const auto source = SampleImageBarrier();
    const auto forward = VulkanBarrierLowering::SplitImageOwnershipTransfer(source, 0, 2);
    const auto back = VulkanBarrierLowering::SplitImageOwnershipTransfer(source, 2, 0);

    EXPECT_EQ(back.Release.srcQueueFamilyIndex, 2u);
    EXPECT_EQ(back.Release.dstQueueFamilyIndex, 0u);
    EXPECT_EQ(back.Release.oldLayout, forward.Release.oldLayout);
    EXPECT_EQ(back.Acquire.newLayout, forward.Acquire.newLayout);
}

TEST(VulkanOwnershipTransfer, BufferHalvesSplitTheSameWay)
{
    VkBufferMemoryBarrier2 source{};
    source.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    source.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    source.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    source.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    source.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source.buffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(0xB0Fu));
    source.offset = 0;
    source.size = VK_WHOLE_SIZE;

    const auto pair = VulkanBarrierLowering::SplitBufferOwnershipTransfer(source, 2, 0);

    EXPECT_EQ(pair.Release.srcQueueFamilyIndex, 2u);
    EXPECT_EQ(pair.Release.dstQueueFamilyIndex, 0u);
    EXPECT_EQ(pair.Release.dstStageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(pair.Release.dstAccessMask, VK_ACCESS_2_NONE);
    EXPECT_EQ(pair.Acquire.srcStageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(pair.Acquire.srcAccessMask, VK_ACCESS_2_NONE);
    EXPECT_EQ(pair.Acquire.dstAccessMask, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    EXPECT_EQ(pair.Release.buffer, source.buffer);
    EXPECT_EQ(pair.Acquire.buffer, source.buffer);
    EXPECT_EQ(pair.Release.size, source.size);
    EXPECT_EQ(pair.Acquire.offset, source.offset);
}

// --- The device-gated round trip -------------------------------------------
//
// A real graphics -> compute -> graphics hand-off through the PRODUCTION
// submission path: a hidden no-API window, a real VulkanContext, and a render
// graph whose consumer is an async-compute candidate. The producer fills a
// buffer on the graphics queue; the batched consumer copies it on the compute
// queue; the copy's result is read back on the host. If the ownership transfer
// pair were wrong the copy would read undefined memory — which this hardware
// would probably still get right, so the assertion that carries the weight is
// the validation-error count, with the byte comparison as the coarse net.
//
// SKIP ladder: no loader / no ICD / no device satisfying ADR 0010 -> clean SKIP,
// so headless CI stays green. On a device with no compute-only queue family it
// asserts the DEGRADE path instead — that the batch declined, said why, and
// still produced the right bytes.

namespace
{
    using OloEngine::Tests::ProbeVulkanDeviceTestGate;
    using OloEngine::Tests::ScopedVulkanRenderCommandSelection;

    class ScopedHiddenVulkanWindow
    {
      public:
        ScopedHiddenVulkanWindow()
        {
            if (glfwInit() != GLFW_TRUE || glfwVulkanSupported() != GLFW_TRUE)
                return;
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            m_Window = glfwCreateWindow(64, 64, "OloEngine async compute test", nullptr, nullptr);
            glfwDefaultWindowHints();
        }
        ~ScopedHiddenVulkanWindow()
        {
            if (m_Window != nullptr)
                glfwDestroyWindow(m_Window);
        }
        ScopedHiddenVulkanWindow(const ScopedHiddenVulkanWindow&) = delete;
        ScopedHiddenVulkanWindow& operator=(const ScopedHiddenVulkanWindow&) = delete;
        ScopedHiddenVulkanWindow(ScopedHiddenVulkanWindow&&) = delete;
        ScopedHiddenVulkanWindow& operator=(ScopedHiddenVulkanWindow&&) = delete;
        [[nodiscard]] GLFWwindow* Get() const
        {
            return m_Window;
        }

      private:
        GLFWwindow* m_Window = nullptr;
    };

    // Minimal setup/execute node — the local stub pattern the other Vulkan
    // graph tenants use (the shared helper is file-local in each of them).
    class AsyncStubPass : public RenderGraphNode
    {
      public:
        using SetupFn = std::function<void(RGBuilder&)>;
        using ExecuteFn = std::function<void()>;

        AsyncStubPass(const std::string& name, SetupFn setup, ExecuteFn execute)
            : m_Setup(std::move(setup)), m_Execute(std::move(execute))
        {
            m_Name = name;
        }

        void Init(const FramebufferSpecification& /*spec*/) override {}
        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
        {
            RenderGraphNode::Setup(builder, blackboard);
            if (m_Setup)
                m_Setup(builder);
        }
        void Execute(RGCommandContext& /*context*/) override
        {
            if (m_Execute)
                m_Execute();
        }
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override
        {
            return nullptr;
        }

      private:
        SetupFn m_Setup;
        ExecuteFn m_Execute;
    };
} // namespace

TEST(VulkanAsyncComputeDevice, AnAsyncBatchCrossesToTheComputeQueueAndBack)
{
    const auto gate = ProbeVulkanDeviceTestGate();
    if (!gate.Available)
        GTEST_SKIP() << gate.Reason;

    ScopedHiddenVulkanWindow window;
    if (window.Get() == nullptr)
        GTEST_SKIP() << "GLFW could not create a hidden Vulkan-capable window.";

    // Split submission is the path the batch rides on; async compute is forced
    // ON so a machine whose default differs still tests the same thing.
    const bool originalSequential = Levers::RenderGraphSequential();
    const Levers::Tristate originalAsync = Levers::VulkanAsyncCompute();
    struct LeverRestore
    {
        bool Sequential;
        Levers::Tristate Async;
        ~LeverRestore()
        {
            Levers::SetRenderGraphSequential(Sequential);
            Levers::SetVulkanAsyncCompute(Async);
        }
    } leverRestore{ originalSequential, originalAsync };
    Levers::SetRenderGraphSequential(false);
    Levers::SetVulkanAsyncCompute(Levers::Tristate::On);

    ScopedVulkanRenderCommandSelection renderCommandSelection;
    {
        VulkanContext context(window.Get());
        try
        {
            context.Init();
        }
        catch (const std::exception& e)
        {
            GTEST_SKIP() << "VulkanContext bring-up refused: " << e.what();
        }
        RenderCommand::Init();
        VulkanDevice::ResetValidationErrorCount();

        auto* device = VulkanDevice::Get();
        ASSERT_NE(device, nullptr);
        const bool hasAsyncQueue = device->HasAsyncComputeQueue();

        auto source = StorageBuffer::Create(sizeof(u32), 30u, StorageBufferUsage::DynamicCopy);
        auto destination = StorageBuffer::Create(sizeof(u32), 31u, StorageBufferUsage::DynamicCopy);
        ASSERT_TRUE(source && destination);
        auto* vkSource = static_cast<VulkanStorageBuffer*>(source.Raw());
        auto* vkDestination = static_cast<VulkanStorageBuffer*>(destination.Raw());
        auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

        constexpr u32 kPattern = 0x808C0DE5u;

        // The graph resource that carries the OWNERSHIP TRANSFER has to be
        // imported BY HANDLE: a native-only import (ImportBuffer's u32 id)
        // cannot answer in the identity currency, so the planner emits no
        // per-resource barrier for it and there is nothing to split. That is a
        // pre-existing limitation of the import form, not of this change — a
        // pool texture is what a production graph actually carries.
        TransientPool pool;
        TextureSpecification colorSpec;
        colorSpec.Width = 64;
        colorSpec.Height = 64;
        colorSpec.Format = ImageFormat::RGBA16F;
        const auto colorTex = pool.AcquireTexture(colorSpec);
        ASSERT_TRUE(colorTex);
        const auto colorIdentity = colorTex->GetRHIHandle();

        RGResourceDesc texDesc;
        texDesc.Kind = RGResourceHandle::Kind::Texture2D;
        texDesc.Format = RGResourceFormat::RGBA16Float;
        texDesc.Width = 64;
        texDesc.Height = 64;

        RenderGraph graph;
        graph.SetRuntimeBarrierExecutionEnabled(true);
        auto producer = Ref<AsyncStubPass>::Create(
            "AsyncProducer",
            [&texDesc, colorIdentity](RGBuilder& builder)
            {
                auto tex = builder.ImportTextureHandle("AsyncComputeColor", colorIdentity, texDesc);
                builder.Write(tex, RGWriteUsage::ShaderImage);
            },
            [&]()
            {
                // A real graphics-queue write to both resources: the texture is
                // what the graph declares and therefore what gets the ownership
                // transfer; the buffer is the coarse "did the compute segment
                // execute after this one" check further down.
                api.ClearTextureFloat(colorIdentity, 0, { 0.25f, 0.5f, 0.75f, 1.0f });
                vkCmdFillBuffer(api.CurrentCommandBuffer(), vkSource->GetVkBuffer(), 0u, sizeof(kPattern), kPattern);
            });
        graph.AddNode(producer);

        // The consumer is what makes this an ASYNC batch rather than #807's
        // plain split submission: a Compute-work-type node that opted in.
        auto consumer = Ref<AsyncStubPass>::Create(
            "AsyncConsumer",
            [&texDesc, colorIdentity](RGBuilder& builder)
            {
                auto tex = builder.ImportTextureHandle("AsyncComputeColor", colorIdentity, texDesc);
                [[maybe_unused]] const auto sampled = builder.Read(tex, RGReadUsage::ShaderSample);
            },
            [&]()
            {
                VkBufferCopy copy{};
                copy.size = sizeof(kPattern);
                vkCmdCopyBuffer(api.CurrentCommandBuffer(), vkSource->GetVkBuffer(),
                                vkDestination->GetVkBuffer(), 1u, &copy);
            });
        consumer->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
        consumer->SetAsyncComputeCandidate(true);
        graph.AddNode(consumer);
        graph.AddExecutionDependency("AsyncProducer", "AsyncConsumer");
        graph.SetFinalPass("AsyncConsumer");
        graph.BuildFrameGraph();

        // The plan must actually contain a batch, or this tenant proves nothing
        // about the backend and would pass for the wrong reason.
        const auto plan = graph.GetSubmissionPlan();
        const auto batchBegin = std::ranges::find_if(
            plan,
            [](const RenderGraph::SubmissionCommand& command)
            { return command.CommandKind == RenderGraph::SubmissionCommand::Kind::BatchBegin; });
        ASSERT_NE(batchBegin, plan.end()) << "the graph did not schedule an async-compute batch";

        context.SetFrameRenderCallback(
            [&](const GraphicsContext::FrameRenderTarget&)
            {
                graph.Execute();
                return false; // VulkanContext owns the backbuffer clear/present
            });
        context.SwapBuffers();
        ASSERT_EQ(vkQueueWaitIdle(device->GetQueue()), VK_SUCCESS);

        const auto stats = api.GetAsyncComputeStats();
        if (hasAsyncQueue)
        {
            EXPECT_EQ(stats.BatchesOnComputeQueue, 1u) << "the batch should have crossed to the compute queue";
            EXPECT_EQ(stats.BatchesDeclined, 0u);
            EXPECT_GT(stats.OwnershipTransfers, 0u) << "crossing without an ownership transfer is the silent bug";
            EXPECT_EQ(stats.ComputeSubmits, 1u);
        }
        else
        {
            // The degrade path, and the reason it took it must be nameable —
            // never a silent no-op (no-silent-fallbacks.md).
            EXPECT_EQ(stats.BatchesOnComputeQueue, 0u);
            EXPECT_EQ(stats.BatchesDeclined, 1u);
            EXPECT_FALSE(stats.DeclineReason.empty());
            EXPECT_NE(stats.DeclineReason, "unknown");
        }

        // Either way the frame must be CORRECT: the consumer's copy ran after
        // the producer's fill and saw its bytes.
        u32 copied = 0u;
        destination->GetData(&copied, sizeof(copied));
        EXPECT_EQ(copied, kPattern);

        context.SetFrameRenderCallback({});
    }

    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
        << "queue-family ownership transfers and the cross-queue submits must be validation-clean";
}

#endif // OLO_WITH_VULKAN
