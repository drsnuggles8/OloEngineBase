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
#include "OloEngine/Renderer/Debug/GPUReadbackStatsRegistry.h"
#include "OloEngine/Renderer/GraphicsContext.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/VirtualShadowMap.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TransientPool.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanStorageBuffer.h"

#include "VulkanTestSupport.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <cstddef>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <string_view>

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
    // The log line and the MCP telemetry both render this enum. An unmapped
    // member would tell a user trying to find out why their GPU is not
    // overlapping anything only "unknown" — and two members sharing a string
    // would be just as unhelpful, so distinctness is asserted, not just
    // non-emptiness.
    const AsyncComputeUnavailableReason all[] = {
        AsyncComputeUnavailableReason::None,
        AsyncComputeUnavailableReason::NoDeviceQueueFamilies,
        AsyncComputeUnavailableReason::NoComputeOnlyFamily,
        AsyncComputeUnavailableReason::FamilyHasNoQueues,
        AsyncComputeUnavailableReason::FamilyHasNoTimestamps,
        AsyncComputeUnavailableReason::CommandPoolCreationFailed,
        AsyncComputeUnavailableReason::DisabledByLever,
        AsyncComputeUnavailableReason::NoDevice,
    };
    std::set<std::string_view> seen;
    for (const auto reason : all)
    {
        const std::string_view described = VulkanQueueSelection::Describe(reason);
        EXPECT_NE(described, "unknown");
        EXPECT_FALSE(described.empty());
        EXPECT_TRUE(seen.insert(described).second) << "two reasons share the description: " << described;
    }
    EXPECT_EQ(seen.size(), std::size(all));
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
    // Non-zero and finite on purpose: with offset 0 / VK_WHOLE_SIZE a
    // regression that dropped either field on either half would still pass.
    source.offset = 256;
    source.size = 4096;

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
    EXPECT_EQ(pair.Release.offset, source.offset);
    EXPECT_EQ(pair.Release.size, source.size);
    EXPECT_EQ(pair.Acquire.offset, source.offset);
    EXPECT_EQ(pair.Acquire.size, source.size);
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

    struct VsmMarkInputs
    {
        u32 Invalidate = 0;
        f32 CameraX = 0.0f;
    };

    void CopyVsmMarkInputs(const RGPreparedPass& mark, VkCommandBuffer cmd, VkBuffer readback, u32 frame)
    {
        const VulkanStorageBuffer* localLights = nullptr;
        VulkanUniformBuffer* globals = nullptr;
        for (const auto& use : mark.Resources)
        {
            const auto* entry = VulkanRootObjectRegistry::Get().Lookup(use.Resource);
            if (entry == nullptr || use.Write)
                continue;
            if (entry->Kind == VulkanRootObjectKind::UniformBuffer)
            {
                auto* buffer = static_cast<VulkanUniformBuffer*>(entry->Object);
                if (buffer->GetBinding() == ShaderBindingLayout::UBO_VIRTUAL_SHADOW)
                    globals = buffer;
            }
            else if (entry->Kind == VulkanRootObjectKind::StorageBuffer)
            {
                const auto* buffer = static_cast<const VulkanStorageBuffer*>(entry->Object);
                if (buffer->GetBinding() == ShaderBindingLayout::SSBO_VSM_LOCAL_LIGHTS)
                    localLights = buffer;
            }
        }
        ASSERT_NE(localLights, nullptr);
        ASSERT_NE(globals, nullptr);
        VkBuffer globalsBuffer = VK_NULL_HANDLE;
        u64 globalsOffset = 0;
        ASSERT_TRUE(VulkanFrameArena::Get().ResolveAddress(globals->GetRootDataAddress(), globalsBuffer, globalsOffset));

        // The local-light buffer's invalidation words follow its raster-mip
        // table and head table. Read this word alongside the marker's camera.
        constexpr VkDeviceSize kLayer0Invalidate = 2u * VSM::kMaxLocalLayers * sizeof(u32);
        const VkDeviceSize resultOffset = frame * sizeof(VsmMarkInputs);
        const VkBufferCopy invalidate{ kLayer0Invalidate, resultOffset + offsetof(VsmMarkInputs, Invalidate), sizeof(u32) };
        vkCmdCopyBuffer(cmd, localLights->GetVkBuffer(), readback, 1, &invalidate);
        const VkBufferCopy cameraX{ globalsOffset + offsetof(VSM::GlobalsUBO, CameraPosition),
                                    resultOffset + offsetof(VsmMarkInputs, CameraX), sizeof(f32) };
        vkCmdCopyBuffer(cmd, globalsBuffer, readback, 1, &cameraX);
    }

    void RecordVsmMarkFrame(VulkanContext& context, VirtualShadowMap& vsm, const Texture2D& depth,
                            const StorageBuffer& stats, const VirtualShadowMap::LocalLightDesc& light,
                            VkBuffer readback, u32 frame)
    {
        // Keeping the same light on frame 2 rewrites the invalidation word
        // from 1 (new layer) to 0 (retained layer), at the same buffer address.
        const glm::vec3 camera{ frame == 0u ? 1.0f : 3.0f, 0.0f, 0.0f };
        vsm.BeginLocalLights();
        ASSERT_EQ(vsm.RegisterLocalLight(light), 0);
        vsm.EndLocalLights();
        vsm.BeginFrame({ 0.0f, -1.0f, 0.0f }, camera, glm::vec3(0.0f));
        const auto depthHandle = depth.GetRHIHandle();
        auto mark = vsm.PreparePageMarking(depthHandle, depth.GetWidth(), depth.GetHeight(), glm::mat4(1.0f), camera);
        ASSERT_TRUE(mark.Record);

        auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
        if (frame == 0u)
        {
            api.ClearTextureFloat(depthHandle, 0, glm::vec4(0.5f));
            ASSERT_TRUE(context.BeginAsyncComputeSegment(0u)) << context.GetAsyncComputeDeclineReason();
            RHI::Barrier depthRead;
            depthRead.Resource = depthHandle;
            depthRead.Range = { RHI::TextureAspect::Color, 0u, 1u, 0u, 1u };
            depthRead.Before = RHI::Access::TransferWrite;
            depthRead.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &depthRead, 1u });
            stats.Bind();
            RGCommandContext commandContext;
            constexpr const char* kMarkerShader = "VSM_MarkRequiredPages";
            const auto before = api.GetComputeDispatchCensus()[kMarkerShader];
            mark.Record(commandContext);
            const auto after = api.GetComputeDispatchCensus()[kMarkerShader];
            EXPECT_EQ(after.Recorded, before.Recorded + 1u);
            EXPECT_EQ(after.Dropped, before.Dropped);
            EXPECT_EQ(after.NoShader, before.NoShader);
            EXPECT_EQ(after.NoBracket, before.NoBracket);
        }

        CopyVsmMarkInputs(mark, api.CurrentCommandBuffer(), readback, frame);
        if (frame == 0u)
        {
            // Close the segment even if the readback probe raised a failure.
            EXPECT_TRUE(context.EndAsyncComputeSegment(0u));
            mark.Publish();
        }
    }
} // namespace

TEST(VulkanAsyncComputeDevice, AnAsyncBatchCrossesToTheComputeQueueAndBack)
{
    OLO_VULKAN_DEVICE_OR_SKIP();

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

// #1341: the production shape the first test does not cover. A graphics pass
// already moved the image into its sampled layout, and the async pass's read
// is declared through a native import, so the planner issues no barrier for
// it inside the batch. The pass still samples the image on the compute family, so the bind
// must move ownership; before the fix nothing did and the image was read while
// the graphics family owned it (undefined contents on hardware that splits the
// families, invisible on NVIDIA).
TEST(VulkanAsyncComputeDevice, ABindInsideTheBatchTransfersAnImageThePlannerDidNotBarrier)
{
    OLO_VULKAN_DEVICE_OR_SKIP();

    ScopedHiddenVulkanWindow window;
    if (window.Get() == nullptr)
        GTEST_SKIP() << "GLFW could not create a hidden Vulkan-capable window.";

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
        if (!device->HasAsyncComputeQueue())
            GTEST_SKIP() << VulkanQueueSelection::Describe(device->GetAsyncComputeUnavailableReason());
        auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

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
        auto importRead = [&texDesc, colorIdentity](RGBuilder& builder)
        {
            auto tex = builder.ImportTextureHandle("AsyncBindColor", colorIdentity, texDesc);
            [[maybe_unused]] const auto sampled = builder.Read(tex, RGReadUsage::ShaderSample);
        };
        graph.AddNode(Ref<AsyncStubPass>::Create(
            "BindProducer",
            [&texDesc, colorIdentity](RGBuilder& builder)
            {
                auto tex = builder.ImportTextureHandle("AsyncBindColor", colorIdentity, texDesc);
                builder.Write(tex, RGWriteUsage::ShaderImage);
            },
            [&]()
            { api.ClearTextureFloat(colorIdentity, 0, { 0.25f, 0.5f, 0.75f, 1.0f }); }));
        // The graphics reader takes the image to its sampled layout.
        graph.AddNode(Ref<AsyncStubPass>::Create("BindGraphicsReader", importRead,
                                                 [&]()
                                                 { api.BindTexture(0u, colorIdentity); }));
        // The async pass declares its read through the native-id import, the
        // form production blackboard inputs take: it cannot answer in the
        // identity currency, so the planner resolves no per-resource barrier
        // for it and the bind below is the only place that sees the image.
        const u32 nativeId = colorTex->GetRendererID();
        auto consumer = Ref<AsyncStubPass>::Create(
            "BindAsyncConsumer",
            [&texDesc, nativeId](RGBuilder& builder)
            {
                auto tex = builder.ImportTexture("AsyncBindColorNative", nativeId, texDesc);
                [[maybe_unused]] const auto sampled = builder.Read(tex, RGReadUsage::ShaderSample);
            },
            [&]()
            { api.BindTexture(0u, colorIdentity); });
        consumer->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
        consumer->SetAsyncComputeCandidate(true);
        graph.AddNode(consumer);
        graph.AddExecutionDependency("BindProducer", "BindGraphicsReader");
        graph.AddExecutionDependency("BindGraphicsReader", "BindAsyncConsumer");
        graph.SetFinalPass("BindAsyncConsumer");
        graph.BuildFrameGraph();

        const auto plan = graph.GetSubmissionPlan();
        ASSERT_NE(std::ranges::find_if(plan, [](const RenderGraph::SubmissionCommand& command)
                                       { return command.CommandKind == RenderGraph::SubmissionCommand::Kind::BatchBegin; }),
                  plan.end())
            << "the graph did not schedule an async-compute batch";

        context.SetFrameRenderCallback(
            [&](const GraphicsContext::FrameRenderTarget&)
            {
                graph.Execute();
                return false;
            });
        context.SwapBuffers();
        ASSERT_EQ(vkQueueWaitIdle(device->GetQueue()), VK_SUCCESS);

        const auto stats = api.GetAsyncComputeStats();
        EXPECT_EQ(stats.BatchesOnComputeQueue, 1u);
        // One pair in, one pair back at the batch end.
        EXPECT_GE(stats.OwnershipTransfers, 2u)
            << "the async pass sampled an image the graphics family still owned";
        context.SetFrameRenderCallback({});
    }
    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
}

// #1341: with the lever unset, a device that HAS a compute-only family still
// keeps the batch on the graphics queue, and says why. Crossing measured no
// overlap and +2.3 ms of GPU frame time, because the graphics queue waits for
// the whole batch; the queue stays available behind OLO_VK_ASYNC_COMPUTE=1.
TEST(VulkanAsyncComputeDevice, AnUnsetLeverKeepsTheBatchOnTheGraphicsQueueAndSaysWhy)
{
    OLO_VULKAN_DEVICE_OR_SKIP();

    ScopedHiddenVulkanWindow window;
    if (window.Get() == nullptr)
        GTEST_SKIP() << "GLFW could not create a hidden Vulkan-capable window.";

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
    Levers::SetVulkanAsyncCompute(Levers::Tristate::Unset);

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
        if (!device->HasAsyncComputeQueue())
            GTEST_SKIP() << VulkanQueueSelection::Describe(device->GetAsyncComputeUnavailableReason());
        auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

        auto source = StorageBuffer::Create(sizeof(u32), 30u, StorageBufferUsage::DynamicCopy);
        auto destination = StorageBuffer::Create(sizeof(u32), 31u, StorageBufferUsage::DynamicCopy);
        ASSERT_TRUE(source && destination);
        auto* vkSource = static_cast<VulkanStorageBuffer*>(source.Raw());
        auto* vkDestination = static_cast<VulkanStorageBuffer*>(destination.Raw());
        constexpr u32 kPattern = 0x13410FFu;

        RenderGraph graph;
        graph.AddNode(Ref<AsyncStubPass>::Create("DefaultProducer", nullptr, [&]()
                                                 { vkCmdFillBuffer(api.CurrentCommandBuffer(), vkSource->GetVkBuffer(), 0u, sizeof(kPattern), kPattern); }));
        auto consumer = Ref<AsyncStubPass>::Create("DefaultConsumer", nullptr, [&]()
                                                   {
            VkBufferCopy copy{};
            copy.size = sizeof(kPattern);
            vkCmdCopyBuffer(api.CurrentCommandBuffer(), vkSource->GetVkBuffer(), vkDestination->GetVkBuffer(), 1u, &copy); });
        consumer->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
        consumer->SetAsyncComputeCandidate(true);
        graph.AddNode(consumer);
        graph.AddExecutionDependency("DefaultProducer", "DefaultConsumer");
        graph.SetFinalPass("DefaultConsumer");
        graph.BuildFrameGraph();
        const auto plan = graph.GetSubmissionPlan();
        ASSERT_NE(std::ranges::find_if(plan, [](const RenderGraph::SubmissionCommand& command)
                                       { return command.CommandKind == RenderGraph::SubmissionCommand::Kind::BatchBegin; }),
                  plan.end())
            << "the graph did not schedule an async-compute batch";

        context.SetFrameRenderCallback(
            [&](const GraphicsContext::FrameRenderTarget&)
            {
                graph.Execute();
                return false;
            });
        context.SwapBuffers();
        ASSERT_EQ(vkQueueWaitIdle(device->GetQueue()), VK_SUCCESS);

        const auto stats = api.GetAsyncComputeStats();
        EXPECT_EQ(stats.BatchesOnComputeQueue, 0u);
        EXPECT_EQ(stats.BatchesDeclined, 1u);
        EXPECT_EQ(stats.OwnershipTransfers, 0u);
        EXPECT_NE(stats.DeclineReason.find("not opted in"), std::string_view::npos) << stats.DeclineReason;
        u32 copied = 0u;
        destination->GetData(&copied, sizeof(copied));
        EXPECT_EQ(copied, kPattern) << "the graphics-queue fallback must still run the batch in order";
        context.SetFrameRenderCallback({});
    }
    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
}

// #1341: early submission cannot strand the release halves. While a batch is
// open the graphics buffer is parked, holding the releases the batch's
// transfers append; a render-graph fence segment submitted then would end the
// compute buffer and submit it to the graphics queue, and the parked releases
// would never reach a queue. The submit must be refused, and the batch must
// still close and submit cleanly afterwards.
TEST(VulkanAsyncComputeDevice, AFenceSegmentSubmitInsideAnOpenBatchIsRefused)
{
    OLO_VULKAN_DEVICE_OR_SKIP();

    ScopedHiddenVulkanWindow window;
    if (window.Get() == nullptr)
        GTEST_SKIP() << "GLFW could not create a hidden Vulkan-capable window.";
    const Levers::Tristate originalAsync = Levers::VulkanAsyncCompute();
    struct AsyncRestore
    {
        Levers::Tristate Async;
        ~AsyncRestore()
        {
            Levers::SetVulkanAsyncCompute(Async);
        }
    } asyncRestore{ originalAsync };
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
        if (!device->HasAsyncComputeQueue())
            GTEST_SKIP() << VulkanQueueSelection::Describe(device->GetAsyncComputeUnavailableReason());

        bool opened = false;
        bool fenceSubmittedInside = true;
        bool closed = false;
        bool fenceSubmittedAfter = false;
        context.SetFrameRenderCallback(
            [&](const GraphicsContext::FrameRenderTarget&)
            {
                opened = context.BeginAsyncComputeSegment(0u);
                fenceSubmittedInside = context.SubmitRenderGraphFenceSegment();
                closed = context.EndAsyncComputeSegment(0u);
                // Once the batch is closed the same submit is legal again.
                fenceSubmittedAfter = context.SubmitRenderGraphFenceSegment();
                return false;
            });
        context.SwapBuffers();
        ASSERT_EQ(vkDeviceWaitIdle(device->GetDevice()), VK_SUCCESS);
        context.SetFrameRenderCallback({});

        EXPECT_TRUE(opened) << context.GetAsyncComputeDeclineReason();
        EXPECT_FALSE(fenceSubmittedInside) << "a mid-batch fence segment would strand the parked release halves";
        EXPECT_TRUE(closed);
        EXPECT_TRUE(fenceSubmittedAfter);
    }
    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
}

namespace
{
    // The VSM shaders are runtime assets resolved against the working
    // directory, and the test binary runs from the repository root. Without
    // this, VirtualShadowMap::Init falls back to CSM and the test fails on a
    // missing file rather than on what it is about.
    class ScopedEditorWorkingDirectory
    {
      public:
        ScopedEditorWorkingDirectory()
            : m_Previous(std::filesystem::current_path())
        {
            std::error_code ec;
            std::filesystem::current_path(std::filesystem::path{ OLO_TEST_EDITOR_ROOT }, ec);
        }
        ~ScopedEditorWorkingDirectory()
        {
            std::error_code ec;
            std::filesystem::current_path(m_Previous, ec);
        }
        ScopedEditorWorkingDirectory(const ScopedEditorWorkingDirectory&) = delete;
        ScopedEditorWorkingDirectory& operator=(const ScopedEditorWorkingDirectory&) = delete;
        ScopedEditorWorkingDirectory(ScopedEditorWorkingDirectory&&) = delete;
        ScopedEditorWorkingDirectory& operator=(ScopedEditorWorkingDirectory&&) = delete;

      private:
        std::filesystem::path m_Previous;
    };
} // namespace

TEST(VulkanAsyncComputeDevice, VsmMarkInputsSurviveTheNextFrameUploads)
{
    OLO_VULKAN_DEVICE_OR_SKIP();
    const ScopedEditorWorkingDirectory editorRoot;
    // The test opens compute segments itself; crossing is opt-in (#1341).
    const Levers::Tristate originalAsync = Levers::VulkanAsyncCompute();
    struct AsyncRestore
    {
        Levers::Tristate Async;
        ~AsyncRestore()
        {
            Levers::SetVulkanAsyncCompute(Async);
        }
    } asyncRestore{ originalAsync };
    Levers::SetVulkanAsyncCompute(Levers::Tristate::On);
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    VulkanContext context(VulkanContext::OffscreenTarget{ 32u, 32u });
    try
    {
        context.Init();
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "VulkanContext bring-up refused: " << e.what();
    }
    auto* device = VulkanDevice::Get();
    ASSERT_NE(device, nullptr);
    if (!device->HasAsyncComputeQueue())
        GTEST_SKIP() << VulkanQueueSelection::Describe(device->GetAsyncComputeUnavailableReason());
    RenderCommand::Init();
    VulkanDevice::ResetValidationErrorCount();

    VirtualShadowMapSettings settings;
    settings.Enabled = true;
    settings.PhysicalResolution = 1024;
    VirtualShadowMap vsm;
    vsm.Init(settings);
    ASSERT_TRUE(vsm.AreLocalLightsActive());

    TextureSpecification depthSpec;
    depthSpec.Width = 32u;
    depthSpec.Height = 32u;
    depthSpec.Format = ImageFormat::R32F;
    const auto depth = Texture2D::Create(depthSpec);
    ASSERT_TRUE(depth);

    // Supply the renderer's disabled stats block in this standalone context.
    // The marker reads the flags even when instrumentation is off.
    constexpr u32 kStatsBytes = static_cast<u32>((4u + kGPUStatCounterSlots) * sizeof(u32));
    auto stats = StorageBuffer::Create(kStatsBytes, ShaderBindingLayout::SSBO_GPU_STATS, StorageBufferUsage::DynamicCopy);
    ASSERT_TRUE(stats);
    stats->ClearData();

    const auto readback = StorageBuffer::Create(2u * sizeof(VsmMarkInputs), StorageBuffer::kNoBinding,
                                                StorageBufferUsage::DynamicCopy);
    ASSERT_TRUE(readback);
    const VkBuffer readbackBuffer = static_cast<const VulkanStorageBuffer*>(readback.Raw())->GetVkBuffer();
    const VkDevice vkDevice = device->GetDevice();

    // A host-signalled gate ahead of the compute batch leaves the graphics
    // queue free to run. Unblock and join on every exit, including assertions.
    struct SubmissionGate
    {
        explicit SubmissionGate(VkDevice device)
            : Device(device)
        {
        }
        SubmissionGate(const SubmissionGate&) = delete;
        SubmissionGate& operator=(const SubmissionGate&) = delete;
        SubmissionGate(SubmissionGate&&) = delete;
        SubmissionGate& operator=(SubmissionGate&&) = delete;

        VkDevice Device;
        VkSemaphore Semaphore = VK_NULL_HANDLE;
        VkFence LaterGraphics = VK_NULL_HANDLE;
        bool Held = false;

        VkResult Release()
        {
            VkSemaphoreSignalInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
            signal.semaphore = Semaphore;
            signal.value = 1u;
            const VkResult result = vkSignalSemaphore(Device, &signal);
            if (result == VK_SUCCESS)
                Held = false;
            return result;
        }

        ~SubmissionGate()
        {
            if (Held)
                (void)Release();
            vkDeviceWaitIdle(Device);
            if (LaterGraphics != VK_NULL_HANDLE)
                vkDestroyFence(Device, LaterGraphics, nullptr);
            if (Semaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(Device, Semaphore, nullptr);
        }
    } gate{ vkDevice };
    VkSemaphoreTypeCreateInfo timeline{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphoreInfo.pNext = &timeline;
    ASSERT_EQ(vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &gate.Semaphore), VK_SUCCESS);
    const VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    ASSERT_EQ(vkCreateFence(vkDevice, &fenceInfo, nullptr, &gate.LaterGraphics), VK_SUCCESS);

    VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    wait.semaphore = gate.Semaphore;
    wait.value = 1u;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 hold{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    hold.waitSemaphoreInfoCount = 1;
    hold.pWaitSemaphoreInfos = &wait;
    ASSERT_EQ(vkQueueSubmit2(device->GetAsyncComputeQueue(), 1, &hold, VK_NULL_HANDLE), VK_SUCCESS);
    gate.Held = true;

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubHitsBefore = api.GetUnimplementedStubHitCount();
    VirtualShadowMap::LocalLightDesc light;
    light.LightId = 1u;
    light.Range = 8.0f;
    light.IsPoint = false;
    light.OuterCutoffDegrees = 35.0f;

    for (const u32 frame : { 0u, 1u })
    {
        context.SetFrameRenderCallback([&, frame](const auto&)
                                       {
                                           RecordVsmMarkFrame(context, vsm, *depth, *stats, light, readbackBuffer, frame);
                                           return false; });
        context.SwapBuffers();
        ASSERT_FALSE(HasFatalFailure());
        if (frame == 0u)
            ASSERT_EQ(context.GetAsyncComputeSubmitCountThisFrame(), 1u);
    }
    context.SetFrameRenderCallback({});
    const VkSubmitInfo2 observe{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    ASSERT_EQ(vkQueueSubmit2(device->GetQueue(), 1, &observe, gate.LaterGraphics), VK_SUCCESS);
    constexpr u64 kObservationNs = 1'000'000'000u;
    EXPECT_EQ(vkWaitForFences(vkDevice, 1, &gate.LaterGraphics, VK_TRUE, kObservationNs), VK_TIMEOUT)
        << "the next graphics frame ran while the old VSM mark was held";
    ASSERT_EQ(gate.Release(), VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(vkDevice, 1, &gate.LaterGraphics, VK_TRUE, 10'000'000'000u), VK_SUCCESS);

    std::array<VsmMarkInputs, 2> captured{};
    ASSERT_TRUE(readback->GetData(captured.data(), sizeof(captured)));
    EXPECT_EQ(captured[1].Invalidate, 0u);
    EXPECT_FLOAT_EQ(captured[1].CameraX, 3.0f);
    EXPECT_EQ(captured[0].Invalidate, 1u);
    EXPECT_FLOAT_EQ(captured[0].CameraX, 1.0f);
    EXPECT_EQ(api.GetUnimplementedStubHitCount(), stubHitsBefore);
    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
}

#endif // OLO_WITH_VULKAN
