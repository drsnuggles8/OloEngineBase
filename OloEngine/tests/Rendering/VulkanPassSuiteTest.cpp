// OLO_TEST_LAYER: plumbing
// =============================================================================
// VulkanPassSuiteTest — #691 Phase 7 Stage 1.6b: REAL render passes through
// the REAL render graph on Vulkan.
//
// This is the Wave A vehicle. Where VulkanDrawPathTest proves the facade in
// isolation, this fixture runs the graph end-to-end: transient declaration,
// BuildFrameGraph (Setup + planner), Execute through RenderGraphPlanExecutor
// — pass bodies recording through the PROCESS-GLOBAL VulkanRendererAPI (the
// RGCommandContext/RenderCommand statics route there, the injected-vs-global
// trap CommandDispatch documents), planner-emitted barriers lowering through
// IssueBarrierBatch, transients materialising from the VMA pool.
//
// The first tenant is the golden-tested FXAA pass: a producer node draws the
// pilot's hard-edge pattern (via FullscreenBlit.glsl, whose OLO_VULKAN
// pulling branch is this fixture's sibling change), FXAA consumes it through
// its ordinary versioned-input scan, and the output must match the SAME
// golden PNG the Phase 6 pilot matched — the pilot's proof, re-established
// through the machinery that replaces it.
//
// Device-gated; SKIPs cleanly headless (the VulkanShaderPipelineTest ladder).
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanPassSuite, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/ChromaticAberrationRenderPass.h"
#include "OloEngine/Renderer/Passes/FXAARenderPass.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ResourceHandle.h" // ResourceNames::*
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>
#include <volk.h>
// After volk (GLFW only declares its Vulkan entry points when VK types are
// visible); used solely for the glfwGetCurrentContext teardown guard.
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace
{
    using namespace OloEngine;

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

    // Walk up from the test binary's cwd until OloEditor/ is found, then
    // enter it — shader paths are relative to it (the pilot fixture's rule).
    bool ChangeToOloEditorDir()
    {
        namespace fs = std::filesystem;
        fs::path current = fs::current_path();
        for (int i = 0; i < 6; ++i)
        {
            if (fs::exists(current / "OloEditor" / "assets" / "shaders"))
            {
                fs::current_path(current / "OloEditor");
                return true;
            }
            if (fs::exists(current / "assets" / "shaders") &&
                current.filename() == "OloEditor")
            {
                return true;
            }
            if (!current.has_parent_path() || current.parent_path() == current)
            {
                break;
            }
            current = current.parent_path();
        }
        return false;
    }

    // Solid-colour input for analytic-contract tenants (a vignette's
    // darkening, a chromatic aberration's channel split, … are all cleanest
    // to assert against a uniform field).
    Ref<Texture2D> MakeSolidTexture(u32 size, u8 r, u8 g, u8 b, u8 a)
    {
        std::vector<u8> pixels(static_cast<sizet>(size) * size * 4);
        for (sizet i = 0; i < pixels.size(); i += 4)
        {
            pixels[i + 0] = r;
            pixels[i + 1] = g;
            pixels[i + 2] = b;
            pixels[i + 3] = a;
        }
        TextureSpecification spec;
        spec.Width = size;
        spec.Height = size;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        auto texture = Texture2D::Create(spec);
        if (texture)
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
        return texture;
    }

    // A clean vertical black|white edge at pixel column `edgeX` — for
    // contracts that need a step response away from the screen centre (a
    // radial effect like chromatic aberration is zero AT the centre).
    Ref<Texture2D> MakeVerticalEdgeTexture(u32 size, u32 edgeX)
    {
        std::vector<u8> pixels(static_cast<sizet>(size) * size * 4);
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                const u8 v = x >= edgeX ? 255 : 0;
                const sizet i = (static_cast<sizet>(y) * size + x) * 4;
                pixels[i + 0] = v;
                pixels[i + 1] = v;
                pixels[i + 2] = v;
                pixels[i + 3] = 255;
            }
        }
        TextureSpecification spec;
        spec.Width = size;
        spec.Height = size;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        auto texture = Texture2D::Create(spec);
        if (texture)
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
        return texture;
    }

    // The pilot's golden input (VulkanShaderPipelineTest::MakeHardEdgePattern,
    // replicated byte-for-byte — same input, same golden).
    std::vector<f32> MakeHardEdgePattern(u32 size)
    {
        std::vector<f32> pixels(static_cast<sizet>(size) * size * 4);
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                const f32 v = (x + (y % 8)) < (size / 2 + ((y / 8) % 2) * 4) ? 0.0f : 1.0f;
                const sizet i = (static_cast<sizet>(y) * size + x) * 4;
                pixels[i + 0] = v;
                pixels[i + 1] = v;
                pixels[i + 2] = v;
                pixels[i + 3] = 1.0f;
            }
        }
        return pixels;
    }

    // A producer that draws a preloaded texture into the canonical
    // post-chain input as a NEW VERSION — the shape every upstream pass
    // uses, so FXAA's ordinary versioned-input scan finds it.
    class PatternProducerPass : public RenderGraphNode
    {
      public:
        PatternProducerPass(Ref<Texture2D> pattern, Ref<Shader> blitShader)
            : m_Pattern(std::move(pattern)), m_BlitShader(std::move(blitShader))
        {
            SetName("TestPatternProducer");
        }

        void Init(const FramebufferSpecification& /*spec*/) override
        {
        }

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
        {
            RenderGraphNode::Setup(builder, blackboard);
            if (!blackboard.Post.PostProcessColor.IsValid())
            {
                return;
            }
            constexpr std::string_view versionTag = "TestPatternProducer";
            const auto output =
                builder.WriteNewVersion(blackboard.Post.PostProcessColor, RGWriteUsage::RenderTarget, versionTag);
            SetPrimaryOutputFramebufferHandle(output);
            SetPrimaryOutputTextureHandle(builder.CreateFramebufferAttachmentView(
                std::string(ResourceNames::PostProcessColorTexture) + "@" + std::string(versionTag), output, 0u));
        }

        void Execute(RGCommandContext& context) override
        {
            auto framebuffer = context.ResolveFramebuffer(GetPrimaryOutputFramebufferHandle());
            if (!framebuffer || !m_BlitShader || !m_Pattern)
            {
                return;
            }
            framebuffer->Bind();
            const auto& spec = framebuffer->GetSpecification();
            context.SetViewport(0, 0, spec.Width, spec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetBlendState(false);
            context.SetCulling(false);

            m_BlitShader->Bind();
            // External preloaded content — bound directly (upload seeded its
            // layout), the material-texture shape.
            RenderCommand::BindTexture(0, m_Pattern->GetRHIHandle());

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            context.DrawIndexed(va);
            framebuffer->Unbind();
            DidDraw = true;
        }

        [[nodiscard]] Ref<Framebuffer> GetTarget() const override
        {
            return nullptr;
        }

        // Diagnosis seam: false after Execute means the resolve guard
        // early-returned and nothing recorded.
        bool DidDraw = false;

      private:
        Ref<Texture2D> m_Pattern;
        Ref<Shader> m_BlitShader;
    };
} // namespace

class VulkanPassSuite : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (volkInitialize() != VK_SUCCESS)
            GTEST_SKIP() << "No Vulkan loader on this machine.";

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
                    devices.resize(deviceCount);
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
                GTEST_SKIP() << "No device satisfies the ADR 0010 capability contract here.";
            if (volkInitialize() != VK_SUCCESS)
                GTEST_SKIP() << "Vulkan loader re-initialisation failed.";
        }

        if (!ChangeToOloEditorDir())
            GTEST_SKIP() << "OloEditor/ not found from the test cwd — shader paths unavailable.";

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

        // The graph's pass bodies route through the RenderCommand statics —
        // the PROCESS-GLOBAL backend must be the Vulkan one for the duration.
        // Selection stays active for the whole test (factories switch on it
        // at call time); TearDown restores both.
        m_Selection.emplace();
        RenderCommand::RecreateForSelectedBackend();

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
        // The fullscreen-triangle cache is a process STATIC now holding
        // Vulkan VMA buffers (this fixture is the first to route the real
        // MeshPrimitives triangle through the backend) — released here or
        // the allocator teardown asserts on the leak.
        MeshPrimitives::Shutdown();
        VulkanPipelineBuilder::Get().ReleaseAll();
        VulkanPipelineCache::Get().SaveAndDestroy();
        VulkanFrameArena::Get().ReleaseBuffers();
        VulkanResourceHeap::Get().Release();
        VulkanDeferredReclaim::Get().FlushAll();
        if (m_Fence != VK_NULL_HANDLE)
            vkDestroyFence(m_Device->GetDevice(), m_Fence, nullptr);
        EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
            << "Zero validation errors (sync validation included in debug builds)";
        m_Device->Shutdown();
        m_Device.reset();

        // Restore the process-global backend to the pre-test default (the
        // static-init OpenGL instance's shape): selection back to GL first,
        // then recreate against it — the amendment (34) fixture discipline.
        m_Selection.reset();
        RenderCommand::RecreateForSelectedBackend();
        // The recreated GL object is a FRESH instance whose caps were never
        // queried — without Init() its GL_MAX_DRAW_BUFFERS reads 0 and every
        // SetDrawBuffers/SetBlendStateForAttachment downstream clamps to
        // NOTHING (82 later suite tests failed exactly this way: this fixture
        // is the first to swap the PROCESS-GLOBAL api object rather than
        // inject a local one, so it is the first to need the re-init half of
        // the restore). Guarded on a live GL context: with none current the
        // caps queries would touch GL functions illegally, and no GL test can
        // run in that environment anyway.
        if (glfwGetCurrentContext() != nullptr)
        {
            RenderCommand::Init();
        }
    }

    // The Wave A tenant harness: producer(input) -> passNode -> caller-backed
    // output, through the real graph on the process-global Vulkan backend.
    // Asserts the shared execute contracts (both draws prepared, none
    // dropped, zero resolve failures, the pass's GetTarget() non-null) and
    // returns the output readback for the tenant's own pixel assertions.
    // The FXAA test predates this helper and keeps its deeper diagnostics
    // (plan dump, intermediate bisect, golden compare) inline.
    std::vector<u8> RunSinglePassChain(u32 size,
                                       const Ref<Texture2D>& inputTexture,
                                       const Ref<Shader>& blitShader,
                                       const Ref<RenderGraphNode>& passNode,
                                       const char* finalPassName,
                                       std::string_view outputResourceName,
                                       const std::function<void(FrameBlackboard&, RGFramebufferHandle)>& assignOutput)
    {
        std::vector<u8> rendered;

        RenderGraph graph;
        // Pool materialization is opt-in; production enables it in
        // Renderer3DRenderGraphSetup (the fixture-contract list in this
        // file's header).
        graph.SetTransientMaterializationEnabled(true);

        RGResourceDesc fbDesc;
        fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        fbDesc.Format = RGResourceFormat::RGBA8UNorm;
        fbDesc.Width = size;
        fbDesc.Height = size;

        auto& blackboard = graph.GetBlackboard();
        blackboard.Post.PostProcessColor =
            graph.DeclareTransientFramebuffer(ResourceNames::PostProcessColor, fbDesc);

        FramebufferSpecification outputSpec;
        outputSpec.Width = size;
        outputSpec.Height = size;
        outputSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outputSpec);
        if (!outputFramebuffer)
        {
            ADD_FAILURE() << "backed output framebuffer creation failed";
            return rendered;
        }
        assignOutput(blackboard, graph.DeclareTransientFramebuffer(outputResourceName, fbDesc, outputFramebuffer));

        auto producer = Ref<PatternProducerPass>::Create(inputTexture, blitShader);
        graph.AddNode(producer);
        graph.AddNode(passNode);
        graph.SetFinalPass(finalPassName);
        graph.BuildFrameGraph();

        SubmitFrame(
            [&]()
            {
                graph.Execute();

                auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
                RHI::Barrier toSampled{};
                toSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(0);
                toSampled.Before = RHI::Access::ColorAttachmentWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            });

        EXPECT_TRUE(producer->DidDraw) << finalPassName << ": the producer pass early-returned";
        EXPECT_TRUE(passNode->GetTarget()) << finalPassName << ": the pass early-returned (input/output/shader guard)";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << finalPassName << ": resolve failure pass='" << failure.PassName << "' reason='"
                          << failure.Reason << "' x" << failure.Count;
        }
        {
            auto& vkApi = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
            EXPECT_EQ(vkApi.GetPreparedDrawsThisRecording(), 2u) << finalPassName << ": expected exactly two draws";
            EXPECT_EQ(vkApi.GetDroppedDrawsThisRecording(), 0u) << finalPassName << ": a draw dropped silently";
        }

        auto* vkOutput = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
        if (vkOutput->GetColorAttachmentImage(0) == nullptr ||
            !vkOutput->GetColorAttachmentImage(0)->GetData(rendered, 0))
        {
            ADD_FAILURE() << finalPassName << ": output readback failed";
        }
        return rendered;
    }

    // One simulated frame through the GLOBAL backend's recording bracket.
    void SubmitFrame(const std::function<void()>& work)
    {
        auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
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
    std::optional<ScopedVulkanApiSelection> m_Selection;
    VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
    VkFence m_Fence = VK_NULL_HANDLE;
};

TEST_F(VulkanPassSuite, FxaaPassMatchesTheGoldenThroughTheRenderGraph)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // --- preloaded content ---------------------------------------------------
    const std::vector<f32> pattern = MakeHardEdgePattern(kSize);
    std::vector<u8> patternRgba8(static_cast<sizet>(kSize) * kSize * 4);
    for (sizet i = 0; i < patternRgba8.size(); ++i)
    {
        patternRgba8[i] = static_cast<u8>(std::lround(std::clamp(pattern[i], 0.0f, 1.0f) * 255.0f));
    }
    TextureSpecification patternSpec;
    patternSpec.Width = kSize;
    patternSpec.Height = kSize;
    patternSpec.Format = ImageFormat::RGBA8;
    patternSpec.GenerateMips = false;
    auto patternTexture = Texture2D::Create(patternSpec);
    ASSERT_NE(patternTexture, nullptr);
    patternTexture->SetData(patternRgba8.data(), static_cast<u32>(patternRgba8.size()));

    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
        << "FullscreenBlit.glsl must compile through shaderc(vulkan_1_4) — its OLO_VULKAN pulling branch";

    // The FXAA UBO occupies binding 7 from creation (the GL-parity ctor
    // publish); TexelSize matches the golden's 1/128 pair.
    PostProcessUBOData uboData{};
    uboData.TexelSizeX = 1.0f / static_cast<f32>(kSize);
    uboData.TexelSizeY = 1.0f / static_cast<f32>(kSize);
    uboData.InverseScreenWidth = 1.0f / static_cast<f32>(kSize);
    uboData.InverseScreenHeight = 1.0f / static_cast<f32>(kSize);
    auto postProcessUbo = UniformBuffer::Create(sizeof(PostProcessUBOData), 7);
    postProcessUbo->SetData(&uboData, sizeof(uboData));

    // FullscreenBlit's fragment stage reads DRSParams (binding 33) to clamp
    // its UV; the real pipeline always keeps this bound. The default bounds
    // (1,1) mean DRS-inactive. Without it the Vulkan root entry is a null
    // address and every sample collapses to texel (0,0).
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    // --- the graph -----------------------------------------------------------
    RenderGraph graph;
    // Pool materialization is opt-in; the production pipeline enables it in
    // Renderer3DRenderGraphSetup. Without it every pooled physical stays
    // empty and pass resolves null (reason 'framebuffer-resolve-null').
    graph.SetTransientMaterializationEnabled(true);
    RGResourceDesc fbDesc;
    fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    fbDesc.Format = RGResourceFormat::RGBA8UNorm;
    fbDesc.Width = kSize;
    fbDesc.Height = kSize;

    auto& blackboard = graph.GetBlackboard();
    // The producer's target stays POOLED — the pool-materialization path is
    // part of what this fixture exercises. The OUTPUT gets caller-supplied
    // backing so the readback owns a physical that survives the frame
    // teardown (post-Execute pool resolves are torn down by design).
    blackboard.Post.PostProcessColor =
        graph.DeclareTransientFramebuffer(ResourceNames::PostProcessColor, fbDesc);
    FramebufferSpecification outputSpec;
    outputSpec.Width = kSize;
    outputSpec.Height = kSize;
    outputSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outputSpec);
    ASSERT_TRUE(outputFramebuffer);
    blackboard.Post.FXAAColor =
        graph.DeclareTransientFramebuffer(ResourceNames::FXAAColor, fbDesc, outputFramebuffer);
    ASSERT_TRUE(blackboard.Post.PostProcessColor.IsValid());
    ASSERT_TRUE(blackboard.Post.FXAAColor.IsValid());

    auto producer = Ref<PatternProducerPass>::Create(patternTexture, blitShader);
    graph.AddNode(producer);

    auto fxaa = Ref<FXAARenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    fxaa->Init(initSpec);
    // m_Enabled defaults to false (the pipeline drives it from
    // PostProcessSettings); a disabled FXAA declares no output in Setup and
    // silently early-returns in Execute.
    fxaa->SetEnabled(true);
    graph.AddNode(fxaa);

    graph.SetFinalPass("FXAAPass");
    graph.BuildFrameGraph();

    // Setup-time seam: the versioned-input scan must have matched the
    // producer's (PostProcessColor, PostProcessColorTexture@…) pair — an
    // invalid handle here means FXAA::Execute will silently early-return.
    EXPECT_TRUE(fxaa->GetPrimaryInputTextureHandle().IsValid())
        << "FXAA's versioned-input scan found no producer texture view";
    EXPECT_TRUE(fxaa->GetPrimaryOutputFramebufferHandle().IsValid());
    // Diagnostic on failure only: the plan dump separates "planned but not
    // materialized" from "never planned" (it cracked the materialization-gate
    // bug); quiet on green runs.
    if (::testing::Test::HasFailure())
    {
        for (const auto& entry : graph.GetTransientPlan())
        {
            std::cout << "[plan] " << entry.Resource << " willAlloc=" << entry.WillAllocate << " skip='"
                      << entry.SkipReason << "' reachable=" << entry.Reachable << " first='" << entry.FirstPass
                      << "' last='" << entry.LastPass << "'\n";
        }
    }

    // --- execute through the global backend's bracket ------------------------
    // No gtest fatals inside the lambda: a fatal there skips EndRecording and
    // the failure cascades into teardown asserts that bury the real cause.
    SubmitFrame(
        [&]()
        {
            graph.Execute();

            // COLOR_ATTACHMENT after FXAA's draw; the steady-state readback
            // below wants SHADER_READ_ONLY.
            auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
            RHI::Barrier toSampled{};
            toSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(0);
            toSampled.Before = RHI::Access::ColorAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    // Execute-time seams: the producer's resolve guard, and FXAA's guard
    // chain (GetTarget() is null exactly when Execute early-returned).
    EXPECT_TRUE(producer->DidDraw) << "the producer pass early-returned (its framebuffer resolve failed)";
    EXPECT_TRUE(fxaa->GetTarget()) << "FXAARenderPass::Execute early-returned (input/output/shader guard)";
    // Zero-resolve-failures contract: the context records every execute-path
    // resolve failure with its reason — any entry is a silent early-return.
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "graph resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                      << "' x" << failure.Count;
    }
    // Draw-count contract: producer + FXAA prepare exactly one draw each and
    // nothing drops through PrepareDraw's guard chain.
    {
        auto& vkApi = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
        EXPECT_EQ(vkApi.GetPreparedDrawsThisRecording(), 2u) << "producer + FXAA must each prepare exactly one draw";
        EXPECT_EQ(vkApi.GetDroppedDrawsThisRecording(), 0u) << "a dropped draw means a silent PrepareDraw failure";
    }

    // Bisect probe: the producer's POOLED target must hold the pattern (its
    // physical Ref persists past Execute until the next registry rebuild).
    // White at (120, 8) separates "producer drew black" from "FXAA read black".
    {
        auto intermediateFramebuffer = graph.ResolveFramebuffer(blackboard.Post.PostProcessColor);
        ASSERT_TRUE(intermediateFramebuffer) << "pooled canonical PostProcessColor must resolve after Execute";
        auto* vkIntermediate = static_cast<VulkanFramebuffer*>(intermediateFramebuffer.Raw());
        std::vector<u8> mid;
        ASSERT_TRUE(vkIntermediate->GetColorAttachmentImage(0)->GetData(mid, 0));
        ASSERT_EQ(mid.size(), static_cast<sizet>(kSize) * kSize * 4);
        const sizet whiteIdx = ((static_cast<sizet>(8) * kSize) + 120) * 4;
        const sizet blackIdx = ((static_cast<sizet>(8) * kSize) + 8) * 4;
        EXPECT_GT(static_cast<int>(mid[whiteIdx]), 200)
            << "the producer's blit lost the pattern's white half — intermediate (120,8)=[" << +mid[whiteIdx] << ","
            << +mid[whiteIdx + 1] << "," << +mid[whiteIdx + 2] << "," << +mid[whiteIdx + 3] << "], (8,8)=["
            << +mid[blackIdx] << "," << +mid[blackIdx + 1] << "," << +mid[blackIdx + 2] << "," << +mid[blackIdx + 3]
            << "]";
    }

    // --- golden comparison (the pilot's exact bar) ---------------------------
    auto* vkOutput = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
    ASSERT_NE(vkOutput->GetColorAttachmentImage(0), nullptr);
    std::vector<u8> rendered;
    ASSERT_TRUE(vkOutput->GetColorAttachmentImage(0)->GetData(rendered, 0));
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

    int goldenW = 0;
    int goldenH = 0;
    int goldenChannels = 0;
    stbi_uc* golden = stbi_load("assets/tests/golden/fxaa_hard_edge.png", &goldenW, &goldenH, &goldenChannels, 4);
    ASSERT_NE(golden, nullptr) << "golden missing: assets/tests/golden/fxaa_hard_edge.png";
    ASSERT_EQ(goldenW, static_cast<int>(kSize));
    ASSERT_EQ(goldenH, static_cast<int>(kSize));

    f64 sumSquares = 0.0;
    for (sizet i = 0; i < rendered.size(); ++i)
    {
        const f64 diff = (static_cast<f64>(rendered[i]) - static_cast<f64>(golden[i])) / 255.0;
        sumSquares += diff * diff;
    }
    stbi_image_free(golden);
    const f64 rmse = std::sqrt(sumSquares / static_cast<f64>(rendered.size()));
    if (rmse >= 0.02)
    {
        // Diagnosis aid: dump what actually rendered so the failure can be
        // LOOKED at (the repo's visual-verification rule), plus probe pixels.
        stbi_write_png("assets/tests/visual/vulkan_pass_suite_fxaa_FAILED.png", static_cast<int>(kSize),
                       static_cast<int>(kSize), 4, rendered.data(), static_cast<int>(kSize) * 4);
        const auto px = [&](u32 x, u32 y)
        {
            const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
            return std::format("({},{})=[{},{},{},{}]", x, y, rendered[i], rendered[i + 1], rendered[i + 2],
                               rendered[i + 3]);
        };
        ADD_FAILURE() << "probe pixels: " << px(8, 8) << " " << px(120, 8) << " " << px(8, 120) << " "
                      << px(120, 120) << " " << px(64, 64)
                      << " — dumped assets/tests/visual/vulkan_pass_suite_fxaa_FAILED.png";
    }
    EXPECT_LT(rmse, 0.02) << "FXAA through the render graph must match the GL golden (pilot bar)";

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u) << "the graph execution must not fall through to a stub";
}

// =============================================================================
// Wave A tenants — one test per ported pass, on the RunSinglePassChain
// harness. Contract style: analytic (the pass's defining property asserted on
// pixels), not golden — self-contained, no GL-side generation step.
// =============================================================================

TEST_F(VulkanPassSuite, VignettePassDarkensCornersThroughTheRenderGraph)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto whiteInput = MakeSolidTexture(kSize, 255, 255, 255, 255);
    ASSERT_NE(whiteInput, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    // vignette = smoothstep(0.5, 0.5 - smoothness, dist * (1 + intensity)):
    // at intensity 1.0 / smoothness 0.15 the centre keeps factor 1.0 while
    // every probe corner's scaled distance sits far past the outer edge —
    // factor 0. White input therefore reads white centre, black corners.
    PostProcessUBOData uboData{};
    uboData.VignetteIntensity = 1.0f;
    uboData.VignetteSmoothness = 0.15f;
    uboData.InverseScreenWidth = 1.0f / static_cast<f32>(kSize);
    uboData.InverseScreenHeight = 1.0f / static_cast<f32>(kSize);
    auto postProcessUbo = UniformBuffer::Create(sizeof(PostProcessUBOData), 7);
    postProcessUbo->SetData(&uboData, sizeof(uboData));
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    auto vignette = Ref<VignetteRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    vignette->Init(initSpec);
    vignette->SetEnabled(true);

    const auto rendered = RunSinglePassChain(kSize, whiteInput, blitShader, vignette, "VignettePass",
                                             ResourceNames::VignetteColor,
                                             [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                             { blackboard.Post.VignetteColor = handle; });
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

    const auto luma = [&](u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        return static_cast<int>(rendered[i]);
    };
    EXPECT_GE(luma(64, 64), 250) << "the vignette factor at the centre must be ~1 (white survives)";
    for (const auto& [x, y] : { std::pair<u32, u32>{ 8, 8 }, { 120, 8 }, { 8, 120 }, { 120, 120 } })
    {
        EXPECT_LE(luma(x, y), 10) << "corner (" << x << "," << y << ") must be fully vignetted to black";
    }

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
}

TEST_F(VulkanPassSuite, ChromaticAberrationSplitsChannelsAcrossAnOffCentreEdge)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // Edge at x=96 (uv 0.75) — well off-centre, where dir is large. The
    // shader samples R at uv + dir*offset (outward), B at uv - dir*offset
    // (inward), G centred.
    auto edgeInput = MakeVerticalEdgeTexture(kSize, 96);
    ASSERT_NE(edgeInput, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    PostProcessUBOData uboData{};
    uboData.ChromaticAberrationIntensity = 0.2f;
    uboData.InverseScreenWidth = 1.0f / static_cast<f32>(kSize);
    uboData.InverseScreenHeight = 1.0f / static_cast<f32>(kSize);
    auto postProcessUbo = UniformBuffer::Create(sizeof(PostProcessUBOData), 7);
    postProcessUbo->SetData(&uboData, sizeof(uboData));
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    auto chromAb = Ref<ChromaticAberrationRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    chromAb->Init(initSpec);
    chromAb->SetEnabled(true);

    const auto rendered = RunSinglePassChain(kSize, edgeInput, blitShader, chromAb, "ChromAberrationPass",
                                             ResourceNames::ChromAbColor,
                                             [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                             { blackboard.Post.ChromAbColor = handle; });
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

    const auto px = [&](u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        return std::array<int, 3>{ rendered[i], rendered[i + 1], rendered[i + 2] };
    };

    // x=94, y=64: uv.x 0.7383, dir.x 0.2383, shift 0.0477 — R's sample lands
    // RIGHT of the edge (white), B's lands LEFT (black), G centred (black):
    // a pure red fringe. All sample points sit > 4 texels from the edge, so
    // bilinear filtering cannot soften the contract.
    const auto fringe = px(94, 64);
    EXPECT_GT(fringe[0], 200) << "R must have sampled the white side (outward shift)";
    EXPECT_LT(fringe[1], 50) << "G stays centred on the black side";
    EXPECT_LT(fringe[2], 50) << "B must have sampled the black side (inward shift)";

    // Far field: deep black (all three shifted samples stay black) and deep
    // white (all stay white) must pass through unsplit.
    const auto deepBlack = px(32, 64);
    EXPECT_LT(deepBlack[0], 10);
    EXPECT_LT(deepBlack[1], 10);
    EXPECT_LT(deepBlack[2], 10);
    const auto deepWhite = px(120, 64);
    EXPECT_GT(deepWhite[0], 245);
    EXPECT_GT(deepWhite[1], 245);
    EXPECT_GT(deepWhite[2], 245);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
}

#endif // OLO_WITH_VULKAN
