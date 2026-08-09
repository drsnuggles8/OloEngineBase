// OLO_TEST_LAYER: plumbing
//
// #691 Phase 5, the device-gated half: the render graph's execution layer on
// a REAL Vulkan device under the validation layer (debug builds also enable
// synchronization validation — the actual test of the barrier translation).
//
// What runs on-device here:
//   1. TransientPool acquisitions materialize as VMA-backed VulkanTexture2D /
//      VulkanStorageBuffer through the ordinary factories (the pool itself is
//      backend-neutral — the factory arm IS the Phase 5 seam), including LIFO
//      reuse across a simulated frame and Trim-driven deferred reclaim.
//   2. The graph's transition records, resolved to RHI::Barriers and lowered
//      to vkCmdPipelineBarrier2 batches through VulkanRendererAPI, with the
//      layout tracker supplying exact oldLayouts across two simulated frames
//      (frame 2's barriers start from tracked layouts, not UNDEFINED).
//   3. The poison instrument's clears (ClearTextureFloat / ClearBufferFloat).
//   4. CommandBucket packets dispatched through the SAME Molecular-Matters
//      dispatch table against the Vulkan backend (state packets record
//      dynamic state; draw packets hit the loud Phase 6 stubs and are
//      COUNTED, never silent).
//
// The gate: VulkanDevice::GetValidationErrorCount() must stay 0 across all
// of it. Deliberately NOT routed through RenderCommand's process-wide
// backend: swapping the global mid-suite would destroy an initialized GL
// backend under later GPU tests. The facade wiring is exercised by the GL
// suite (same neutral path); this test injects a local VulkanRendererAPI,
// which is exactly what CommandBucket::Execute(RendererAPI&) exists for.
//
// SKIP ladder mirrors VulkanBringUpTest: no loader / no instance / no device
// satisfying the ADR 0010 contract -> clean SKIP, headless CI stays green.

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanRenderGraphExecution, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TransientPool.h"
#include "OloEngine/Renderer/RHI/RHIGpuFence.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanGpuFence.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include "RenderingTestUtils.h"

#include <volk.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>

namespace
{
    using namespace OloEngine;

    // RAII flip of the process-wide API selector around the factory calls —
    // Texture2D::Create / StorageBuffer::Create switch on it. Restores
    // OpenGL so later suite tests see the default untouched. Deliberately
    // does NOT touch RenderCommand's process-wide backend (swapping it
    // mid-suite would destroy an initialized GL backend under later GPU
    // tests) — everything here injects a local VulkanRendererAPI instead.
    struct ScopedVulkanApiSelection
    {
        ScopedVulkanApiSelection()
        {
            RendererAPI::SetAPI(RendererAPI::API::Vulkan);
        }
        ~ScopedVulkanApiSelection()
        {
            RendererAPI::SetAPI(RendererAPI::API::OpenGL);
        }
    };

    // Minimal setup-lambda node (the RenderGraphTest.cpp stub pattern —
    // local because that helper is file-local there).
    class VkExecStubPass : public RenderGraphNode
    {
      public:
        using SetupFn = std::function<void(RGBuilder&)>;

        VkExecStubPass(const std::string& name, SetupFn setup)
            : m_Setup(std::move(setup))
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
        void Execute(RGCommandContext& /*context*/) override {}
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override
        {
            return nullptr;
        }

      private:
        SetupFn m_Setup;
    };
} // namespace

class VulkanRenderGraphExecution : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (volkInitialize() != VK_SUCCESS)
            GTEST_SKIP() << "No Vulkan loader on this machine.";

        // Probe with VulkanBringUpTest's EXACT ladder (bare instance, no
        // layers/extensions, enumerate, Evaluate) BEFORE constructing
        // VulkanDevice: a loader-without-ICD CI runner survives this probe
        // path provably (VulkanBringUp skips cleanly there), while the full
        // bring-up's extra loader calls SEH-faulted on the Windows ASan
        // runner. Skip decisions belong on the proven path.
        {
            VkApplicationInfo appInfo{};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pApplicationName = "OloEngine-Tests";
            appInfo.apiVersion = VulkanCapabilities::kMinApiVersion;
            VkInstanceCreateInfo instanceInfo{};
            instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            instanceInfo.pApplicationInfo = &appInfo;
            VkInstance probe = VK_NULL_HANDLE;
            if (vkCreateInstance(&instanceInfo, nullptr, &probe) != VK_SUCCESS)
                GTEST_SKIP() << "vkCreateInstance failed (driver below Vulkan 1.4?).";
            volkLoadInstance(probe);

            u32 deviceCount = 0;
            if (vkEnumeratePhysicalDevices(probe, &deviceCount, nullptr) != VK_SUCCESS)
            {
                vkDestroyInstance(probe, nullptr);
                GTEST_SKIP() << "vkEnumeratePhysicalDevices (count) failed on this machine.";
            }
            std::vector<VkPhysicalDevice> devices(deviceCount);
            if (deviceCount > 0)
            {
                const VkResult listResult = vkEnumeratePhysicalDevices(probe, &deviceCount, devices.data());
                if (listResult == VK_SUCCESS || listResult == VK_INCOMPLETE)
                {
                    // The second call rewrites deviceCount with how many it
                    // actually returned (fewer on VK_INCOMPLETE).
                    devices.resize(deviceCount);
                }
                else
                {
                    vkDestroyInstance(probe, nullptr);
                    GTEST_SKIP() << "vkEnumeratePhysicalDevices (list) failed on this machine.";
                }
            }
            const bool anySatisfies = std::ranges::any_of(
                devices,
                [](VkPhysicalDevice device)
                { return VulkanCapabilities::Evaluate(device).Satisfied; });
            vkDestroyInstance(probe, nullptr);
            if (!anySatisfies)
            {
                GTEST_SKIP() << "No device satisfies the ADR 0010 capability contract here — the gate would "
                                "refuse --rhi=vulkan.";
            }
            // The probe's volkLoadInstance left instance-scoped pointers
            // behind; restore loader-scoped ones for the real bring-up.
            if (volkInitialize() != VK_SUCCESS)
                GTEST_SKIP() << "Vulkan loader re-initialisation failed.";
        }

        m_Device = std::make_unique<VulkanDevice>();
        try
        {
            // Headless: no surface — the device pick requires graphics only.
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
        if (!m_Device)
            return;
        vkDeviceWaitIdle(m_Device->GetDevice());
        // The frame arena and resource heap are leaked process-wide
        // singletons; their buffers belong to THIS test's device and must
        // not survive it.
        VulkanFrameArena::Get().ReleaseBuffers();
        VulkanResourceHeap::Get().Release();
        VulkanDeferredReclaim::Get().FlushAll();
        if (m_Fence != VK_NULL_HANDLE)
            vkDestroyFence(m_Device->GetDevice(), m_Fence, nullptr);
        // Command buffer returns with the pool inside VulkanDevice::Shutdown.
        EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
            << "The Phase 5 bar: ZERO validation errors (sync validation included in debug builds)";
        m_Device->Shutdown();
        m_Device.reset();
    }

    // Record `work` into the command buffer through the Vulkan backend,
    // submit, and wait — one simulated frame.
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

    std::unique_ptr<VulkanDevice> m_Device;
    VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
    VkFence m_Fence = VK_NULL_HANDLE;
};

TEST_F(VulkanRenderGraphExecution, TransientPoolMaterializesVmaResourcesAndReusesThem)
{
    ScopedVulkanApiSelection vulkanSelected;

    TransientPool pool;

    TextureSpecification spec;
    spec.Width = 128;
    spec.Height = 128;
    spec.Format = ImageFormat::RGBA16F;

    const auto tex = pool.AcquireTexture(spec);
    ASSERT_TRUE(tex);
    EXPECT_TRUE(tex->GetRHIHandle().IsValid()) << "A Vulkan transient mints an identity like the GL twin";
    EXPECT_EQ(tex->GetRendererID(), 0u) << "No GL name exists under Vulkan — the diagnostics field is 0";

    const auto buffer = pool.AcquireBuffer(4096);
    ASSERT_TRUE(buffer);
    EXPECT_TRUE(buffer->GetRHIHandle().IsValid());

    // LIFO reuse: release-all then re-acquire the same descriptor must hand
    // back the SAME physical object (the pool's aliasing model is
    // backend-neutral — Phase 5 must not have disturbed it).
    const auto firstHandle = tex->GetRHIHandle();
    pool.ReleaseAll();
    const auto reacquired = pool.AcquireTexture(spec);
    ASSERT_TRUE(reacquired);
    EXPECT_EQ(reacquired->GetRHIHandle(), firstHandle) << "Same descriptor -> same pooled object (LIFO)";

    // Trim to zero: pooled objects are destroyed -> the VMA backing goes
    // through the deferred-reclaim queue, drained in TearDown after the
    // fence — destroying in-flight memory inline is the hazard the queue
    // exists for.
    pool.ReleaseAll();
    pool.Trim(0);
    pool.Clear();
}

TEST_F(VulkanRenderGraphExecution, GraphTransitionsLowerToValidationCleanBarrierBatches)
{
    ScopedVulkanApiSelection vulkanSelected;

    // Pool-acquired Vulkan resources, imported into the graph BY HANDLE —
    // the identity currency is what makes a Vulkan graph resolvable
    // (native-only imports are GL's migration tail).
    TransientPool pool;
    TextureSpecification colorSpec;
    colorSpec.Width = 64;
    colorSpec.Height = 64;
    colorSpec.Format = ImageFormat::RGBA16F;
    const auto colorTex = pool.AcquireTexture(colorSpec);
    ASSERT_TRUE(colorTex);
    const auto storageBuf = pool.AcquireBuffer(1024);
    ASSERT_TRUE(storageBuf);

    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false); // we drive the barriers ourselves below

    RGResourceDesc texDesc;
    texDesc.Kind = RGResourceHandle::Kind::Texture2D;
    texDesc.Format = RGResourceFormat::RGBA16Float;
    texDesc.Width = 64;
    texDesc.Height = 64;
    [[maybe_unused]] const auto texHandle = graph.ImportTextureHandle("VkColor", colorTex->GetRHIHandle(), texDesc);

    // Producer writes the imported texture as a storage image; a second
    // writer follows (the §1.5 WAW shape); a consumer samples it. The plan
    // therefore carries a RAW *and* a WAW transition to lower.
    const auto colorIdentity = colorTex->GetRHIHandle();
    auto producer = Ref<VkExecStubPass>::Create(
        "VkProducer",
        [&texDesc, colorIdentity](RGBuilder& builder)
        {
            auto tex = builder.ImportTextureHandle("VkColor", colorIdentity, texDesc);
            builder.Write(tex, RGWriteUsage::ShaderImage);
        });
    graph.AddNode(producer);

    auto rewriter = Ref<VkExecStubPass>::Create(
        "VkRewriter",
        [&texDesc, colorIdentity](RGBuilder& builder)
        {
            auto tex = builder.ImportTextureHandle("VkColor", colorIdentity, texDesc);
            builder.Write(tex, RGWriteUsage::ShaderImage);
        });
    graph.AddNode(rewriter);

    auto consumer = Ref<VkExecStubPass>::Create(
        "VkConsumer",
        [&texDesc, colorIdentity](RGBuilder& builder)
        {
            auto tex = builder.ImportTextureHandle("VkColor", colorIdentity, texDesc);
            [[maybe_unused]] const auto sampled = builder.Read(tex, RGReadUsage::ShaderSample);
        });
    graph.AddNode(consumer);

    graph.AddExecutionDependency("VkProducer", "VkRewriter");
    graph.AddExecutionDependency("VkRewriter", "VkConsumer");
    graph.SetFinalPass("VkConsumer");
    graph.BuildFrameGraph();
    graph.Execute();

    // EVERY MemoryBarrier command in the plan gets lowered and executed —
    // the graph plans one batch per consuming pass (the WAW before
    // VkRewriter AND the RAW before VkConsumer), and exercising only the
    // first would leave the second lowering untested on-device.
    const auto plan = graph.GetSubmissionPlan();
    struct BarrierBatch
    {
        MemoryBarrierFlags Flags = MemoryBarrierFlags::None;
        std::vector<RHI::Barrier> Resolved;
    };
    std::vector<BarrierBatch> batches;
    bool sawWaw = false;
    bool sawRaw = false;
    for (const auto& cmd : plan)
    {
        if (cmd.CommandKind != RenderGraph::SubmissionCommand::Kind::MemoryBarrier)
            continue;
        for (const auto& transition : cmd.Transitions)
        {
            sawWaw |= transition.ToAccess == RHI::Access::StorageWrite;
            sawRaw |= transition.ToAccess == RHI::Access::ShaderSampleRead;
        }
        BarrierBatch batch;
        batch.Flags = cmd.Barriers;
        batch.Resolved = graph.ResolveTransitionsToBarriers(cmd.Transitions);
        batches.push_back(std::move(batch));
    }
    ASSERT_GE(batches.size(), 2u) << "Expected a WAW batch (before VkRewriter) and a RAW batch (before VkConsumer)";
    EXPECT_TRUE(sawWaw) << "The plan must carry the write->write transition (ADR 0011 §1.5)";
    EXPECT_TRUE(sawRaw) << "The plan must carry the read-after-write transition";
    for (const auto& batch : batches)
        ASSERT_FALSE(batch.Resolved.empty()) << "A handle-imported resource must resolve to RHI::Barriers";

    VulkanRendererAPI api;
    api.Init();

    // Frame 1: first-use transitions (tracker answers UNDEFINED — discard).
    SubmitFrame(api, [&]
                {
                    for (const auto& batch : batches)
                        api.IssueBarrierBatch(batch.Flags, batch.Resolved); });

    // Frame 2: the tracker now knows the layouts — the same batches must
    // transition FROM the tracked layouts, not UNDEFINED, and still validate
    // clean (this is the state machine the phase exists to get right).
    SubmitFrame(api, [&]
                {
                    for (const auto& batch : batches)
                        api.IssueBarrierBatch(batch.Flags, batch.Resolved); });

    // A MIXED batch — one resolvable barrier plus one whose handle cannot
    // resolve — must keep the synchronisation the flags promised for the
    // unresolved remainder (the conservative global fallback) instead of
    // silently ordering only the resolved half. Validation-clean is the
    // observable contract.
    {
        std::vector<RHI::Barrier> mixed = batches.front().Resolved;
        RHI::Barrier unresolvable;
        unresolvable.Resource = RHI::ResourceHandle{}; // null handle — never resolves
        unresolvable.Before = RHI::Access::StorageWrite;
        unresolvable.After = RHI::Access::ShaderSampleRead;
        mixed.push_back(unresolvable);
        SubmitFrame(api, [&]
                    { api.IssueBarrierBatch(MemoryBarrierFlags::ShaderStorage, mixed); });
    }

    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
    pool.ReleaseAll();
    pool.Clear();
}

TEST_F(VulkanRenderGraphExecution, PoisonClearsAndBucketDispatchRunOnVulkan)
{
    ScopedVulkanApiSelection vulkanSelected;

    TransientPool pool;
    TextureSpecification spec;
    spec.Width = 32;
    spec.Height = 32;
    spec.Format = ImageFormat::RGBA8;
    const auto tex = pool.AcquireTexture(spec);
    ASSERT_TRUE(tex);
    const auto buf = pool.AcquireBuffer(256);
    ASSERT_TRUE(buf);

    VulkanRendererAPI api;
    api.Init();

    // The poison instrument's exact backend calls
    // (render-graph-transient-aliasing.md's hunt levers must stay armed on
    // the second backend).
    SubmitFrame(api, [&]
                {
                    api.ClearTextureFloat(tex->GetRHIHandle(), 0, { 1.0f, 0.0f, 1.0f, 1.0f });
                    api.ClearBufferFloat(buf->GetRHIHandle(), 1.0e9f); });

    // The same recorded CommandBucket packets the GL dispatcher executes,
    // against the Vulkan backend, through the SAME Molecular-Matters
    // dispatch table: state packets apply (dynamic state or recorded
    // pipeline-key state), draw packets would hit the LOUD Phase 6 stub
    // counter — dispatched, counted, never silent.
    //
    // Initialize is idempotent (re-assigns the same table); Shutdown is
    // deliberately NOT called — the table is process-wide, the renderer's
    // one-time init populates it for the whole suite, and tearing it down
    // here nulls the dispatch resolver under every visual test that runs
    // after this one (found as 51 suite-order-only visual failures).
    CommandDispatch::Initialize();
    {
        auto allocator = std::make_unique<CommandAllocator>();
        CommandBucket bucket;

        const auto viewport = MakeSyntheticViewportCommand(0, 0, 32, 32);
        const auto depthTest = MakeSyntheticDepthTestCommand(true);

        PacketMetadata meta0;
        meta0.m_ExecutionOrder = 0;
        meta0.m_DependsOnPrevious = true;
        PacketMetadata meta1;
        meta1.m_ExecutionOrder = 1;
        meta1.m_DependsOnPrevious = true;
        bucket.Submit(viewport, meta0, allocator.get());
        bucket.Submit(depthTest, meta1, allocator.get());
        bucket.SortCommands();

        const u64 stubsBefore = api.GetPhase6StubHitCount();
        SubmitFrame(api, [&]
                    { bucket.Execute(api); });
        EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
            << "Pure state packets must dispatch without touching a Phase 6 stub";
        bucket.Reset(*allocator);
    }

    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
    pool.ReleaseAll();
    pool.Clear();
}

// -----------------------------------------------------------------------------
// #691 Phase 6, ADR 0011 §4: the frame arena hands out persistently-mapped,
// device-addressable byte ranges — the memory every root-data struct lives in.
// This is also the test that proves bufferDeviceAddress is genuinely ENABLED
// (vkGetBufferDeviceAddress on a feature-disabled device is a validation
// error, caught by the fixture's zero-errors bar).
// -----------------------------------------------------------------------------
TEST_F(VulkanRenderGraphExecution, FrameArenaAllocatesMappedDeviceAddressableRanges)
{
    ScopedVulkanApiSelection vulkanSelected;

    auto& arena = VulkanFrameArena::Get();
    arena.BeginFrame(0);

    const auto a = arena.Allocate(64);
    ASSERT_TRUE(a.IsValid()) << "A live device must yield a real allocation";
    EXPECT_NE(a.Gpu, 0u);
    EXPECT_EQ(a.Offset % 16, 0u);

    // Writes land in mapped memory without any map/unmap ceremony.
    std::memset(a.Cpu, 0xAB, 64);

    const auto b = arena.Allocate(40, 64);
    ASSERT_TRUE(b.IsValid());
    EXPECT_EQ(b.Offset % 64, 0u);
    EXPECT_GT(b.Offset, a.Offset);
    EXPECT_EQ(b.Gpu - a.Gpu, b.Offset - a.Offset) << "GPU addresses and offsets must move in lockstep";

    // The cursor rewinds per slot, and slots are independent.
    const u64 usedSlot0 = arena.GetCurrentSlotUsedBytes();
    EXPECT_GE(usedSlot0, 104u);
    arena.BeginFrame(1);
    EXPECT_EQ(arena.GetCurrentSlotUsedBytes(), 0u);
    const auto c = arena.Allocate(16);
    ASSERT_TRUE(c.IsValid());
    EXPECT_NE(c.Gpu, a.Gpu) << "Different slots are different buffers";
    arena.BeginFrame(0);
    EXPECT_EQ(arena.GetCurrentSlotUsedBytes(), 0u) << "BeginFrame(slot) resets that slot's cursor";

    // Overflow is a counted sentinel, not UB.
    const auto tooBig = arena.Allocate(arena.GetSlotCapacityBytes() + 1);
    EXPECT_FALSE(tooBig.IsValid());
    EXPECT_GE(arena.GetOverflowCount(), 1u);

    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
}

// -----------------------------------------------------------------------------
// Headless (no device): NextValue()'s monotonic contract at the edges — it
// anchors above the LIVE counter and saturates at UINT64_MAX instead of
// wrapping to 0 (a wrap would be an invalid timeline signal much later, far
// from the caller). Uses a stub so the counter is controllable.
// -----------------------------------------------------------------------------
TEST(GpuFenceValueDispenser, AnchorsAboveTheCounterAndSaturatesInsteadOfWrapping)
{
    class StubFence final : public RHI::GpuFence
    {
      public:
        explicit StubFence(u64 initial) : RHI::GpuFence(initial) {}
        void QueueSignal(u64, RHI::FenceSignalOp) override {}
        void QueueWait(u64, RHI::FenceCompareOp) override {}
        void HostSignal(u64 value, RHI::FenceSignalOp) override
        {
            m_Counter = value;
        }
        [[nodiscard]] bool HostWait(u64, u64, RHI::FenceCompareOp) override
        {
            return true;
        }
        [[nodiscard]] u64 CompletedValue() const override
        {
            return m_Counter;
        }

        u64 m_Counter = 0;
    };

    StubFence fence(0);
    EXPECT_EQ(fence.NextValue(), 1u);
    EXPECT_EQ(fence.NextValue(), 2u);

    // A hand-signalled jump: the dispenser must climb above the live counter,
    // never below it.
    fence.m_Counter = 100;
    EXPECT_EQ(fence.NextValue(), 101u);

    // Exhaustion: saturate at UINT64_MAX, never wrap to 0.
    fence.m_Counter = std::numeric_limits<u64>::max() - 1;
    EXPECT_EQ(fence.NextValue(), std::numeric_limits<u64>::max());
    EXPECT_EQ(fence.NextValue(), std::numeric_limits<u64>::max()) << "Repeat calls stay saturated";
    fence.m_Counter = std::numeric_limits<u64>::max();
    EXPECT_EQ(fence.NextValue(), std::numeric_limits<u64>::max()) << "A maxed counter must not wrap";
}

// -----------------------------------------------------------------------------
// #691 Phase 6, ADR 0011 §6: RHI::GpuFence is a timeline semaphore — one
// monotonic counter serving the GPU queue side (staged Signal/Wait drained
// into a submit) and the CPU side (HostSignal / HostWait / CompletedValue).
// -----------------------------------------------------------------------------
TEST_F(VulkanRenderGraphExecution, GpuFenceSignalsAndWaitsAcrossHostAndQueue)
{
    ScopedVulkanApiSelection vulkanSelected;

    Ref<RHI::GpuFence> fence = RHI::GpuFence::Create(/*initialValue=*/0);
    ASSERT_TRUE(fence) << "A live VulkanDevice must yield a real GpuFence";

    // --- CPU side: signal, observe, wait -------------------------------------
    EXPECT_EQ(fence->CompletedValue(), 0u);
    fence->HostSignal(1);
    EXPECT_EQ(fence->CompletedValue(), 1u);
    EXPECT_TRUE(fence->HostWait(1, /*timeoutNanoseconds=*/0))
        << "An already-satisfied wait must return immediately";
    EXPECT_FALSE(fence->HostWait(2, /*timeoutNanoseconds=*/1'000'000))
        << "A wait on an unsignaled value must time out, not succeed";

    // --- GPU side: staged queue ops drain into a submit ----------------------
    // Producer submission signals value 2; the host observes it. This is the
    // §6 shape: Signal attaches to the producing submission, the consumer (here
    // the CPU, standing in for a later submission's QueueWait) observes the
    // counter.
    fence->QueueSignal(2);
    EXPECT_EQ(VulkanGpuFence::GetPendingSubmitOpCount(), 1u);

    std::vector<VkSemaphoreSubmitInfo> waits;
    std::vector<VkSemaphoreSubmitInfo> signals;
    VulkanGpuFence::DrainPendingSubmitOps(waits, signals);
    EXPECT_EQ(VulkanGpuFence::GetPendingSubmitOpCount(), 0u);
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_TRUE(waits.empty());

    ASSERT_EQ(vkResetCommandBuffer(m_Cmd, 0), VK_SUCCESS);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(m_Cmd, &beginInfo), VK_SUCCESS);
    ASSERT_EQ(vkEndCommandBuffer(m_Cmd), VK_SUCCESS);

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = m_Cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = static_cast<u32>(signals.size());
    submit.pSignalSemaphoreInfos = signals.data();
    ASSERT_EQ(vkResetFences(m_Device->GetDevice(), 1, &m_Fence), VK_SUCCESS);
    ASSERT_EQ(vkQueueSubmit2(m_Device->GetQueue(), 1, &submit, m_Fence), VK_SUCCESS);

    EXPECT_TRUE(fence->HostWait(2, /*timeoutNanoseconds=*/UINT64_MAX))
        << "The queue-attached signal must satisfy a host wait";
    EXPECT_EQ(fence->CompletedValue(), 2u);

    // --- Consumer submission waits on the counter ----------------------------
    fence->QueueWait(2);
    waits.clear();
    signals.clear();
    VulkanGpuFence::DrainPendingSubmitOps(waits, signals);
    ASSERT_EQ(waits.size(), 1u);
    EXPECT_TRUE(signals.empty());

    ASSERT_EQ(vkWaitForFences(m_Device->GetDevice(), 1, &m_Fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
    ASSERT_EQ(vkResetCommandBuffer(m_Cmd, 0), VK_SUCCESS);
    ASSERT_EQ(vkBeginCommandBuffer(m_Cmd, &beginInfo), VK_SUCCESS);
    ASSERT_EQ(vkEndCommandBuffer(m_Cmd), VK_SUCCESS);
    submit.waitSemaphoreInfoCount = static_cast<u32>(waits.size());
    submit.pWaitSemaphoreInfos = waits.data();
    submit.signalSemaphoreInfoCount = 0;
    submit.pSignalSemaphoreInfos = nullptr;
    ASSERT_EQ(vkResetFences(m_Device->GetDevice(), 1, &m_Fence), VK_SUCCESS);
    ASSERT_EQ(vkQueueSubmit2(m_Device->GetQueue(), 1, &submit, m_Fence), VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(m_Device->GetDevice(), 1, &m_Fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

    // The value dispenser is monotonic AND anchored above the live counter:
    // the counter reached 2 via the signals above, so the next dispensable
    // values are 3, 4 — never a value a signal already used.
    EXPECT_EQ(fence->NextValue(), 3u);
    EXPECT_EQ(fence->NextValue(), 4u);

    // Destruction goes through deferred reclaim — provoke it and drain.
    fence = nullptr;
    VulkanDeferredReclaim::Get().NotifyFrameCompleted();
    VulkanDeferredReclaim::Get().NotifyFrameCompleted();

    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
}

#endif // OLO_WITH_VULKAN
