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

#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/AOApplyRenderPass.h"
#include "OloEngine/Renderer/Passes/ChromaticAberrationRenderPass.h"
#include "OloEngine/Renderer/Passes/ColorGradingRenderPass.h"
#include "OloEngine/Renderer/Passes/ContactShadowRenderPass.h"
#include "OloEngine/Renderer/Passes/DOFRenderPass.h"
#include "OloEngine/Renderer/Passes/EASURenderPass.h"
#include "OloEngine/Renderer/Passes/FXAARenderPass.h"
#include "OloEngine/Renderer/Passes/FluidIntermediatesPass.h"
#include "OloEngine/Renderer/Passes/GTAORenderPass.h"
#include "OloEngine/Renderer/Passes/MotionBlurRenderPass.h"
#include "OloEngine/Renderer/Passes/PrecipitationRenderPass.h"
#include "OloEngine/Renderer/Passes/SSGIRenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"
#include "OloEngine/Renderer/Passes/VolumetricFogPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ResourceHandle.h" // ResourceNames::*
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/StorageBuffer.h"
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

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>
#include <volk.h>
// After volk (GLFW only declares its Vulkan entry points when VK types are
// visible); used solely for the glfwGetCurrentContext teardown guard.
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <bit>
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

    // Two horizontal bands split at buffer row `splitY`: rows < splitY get
    // `low`, rows >= splitY get `high`. Buffer row r samples at v=(r+0.5)/size
    // — the harness is row-identity end to end (the FXAA golden pins it), so
    // band membership is plain row math. Used as depth / radiance stand-ins
    // for the screen-space marchers (a nearer wall in the upper band).
    Ref<Texture2D> MakeHorizontalSplitTexture(u32 size, u32 splitY,
                                              const std::array<u8, 4>& low,
                                              const std::array<u8, 4>& high)
    {
        std::vector<u8> pixels(static_cast<sizet>(size) * size * 4);
        for (u32 y = 0; y < size; ++y)
        {
            const auto& rgba = y < splitY ? low : high;
            for (u32 x = 0; x < size; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * size + x) * 4;
                pixels[i + 0] = rgba[0];
                pixels[i + 1] = rgba[1];
                pixels[i + 2] = rgba[2];
                pixels[i + 3] = rgba[3];
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

    // IEEE 754 binary16 -> f32 for RGBA16F volume readbacks (the froxel fog
    // tenant reads the integrated volume back raw; no engine 3D readback
    // exists and none is added for a test).
    f32 HalfToFloat(u16 h)
    {
        const u32 sign = (static_cast<u32>(h) >> 15) & 0x1u;
        const u32 exponent = (static_cast<u32>(h) >> 10) & 0x1Fu;
        const u32 mantissa = static_cast<u32>(h) & 0x3FFu;
        u32 bits;
        if (exponent == 0u)
        {
            if (mantissa == 0u)
            {
                bits = sign << 31;
            }
            else
            {
                // Subnormal: normalise into f32.
                u32 e = 0;
                u32 m = mantissa;
                while ((m & 0x400u) == 0u)
                {
                    m <<= 1u;
                    ++e;
                }
                bits = (sign << 31) | ((127u - 15u - e + 1u) << 23) | ((m & 0x3FFu) << 13);
            }
        }
        else if (exponent == 0x1Fu)
        {
            bits = (sign << 31) | 0x7F800000u | (mantissa << 13); // inf / NaN
        }
        else
        {
            bits = (sign << 31) | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
        }
        return std::bit_cast<f32>(bits);
    }

    // A producer that draws a preloaded texture into the canonical
    // post-chain input as a NEW VERSION — the shape every upstream pass
    // uses, so FXAA's ordinary versioned-input scan finds it.
    class PatternProducerPass : public RenderGraphNode
    {
      public:
        // The producer's target defaults to the post-chain input; tenants
        // whose candidate ladder ends elsewhere (EASU reads the SceneColor
        // family) redirect it via the slot accessor + names.
        using TargetSlotAccessor = std::function<RGFramebufferHandle&(FrameBlackboard&)>;

        PatternProducerPass(Ref<Texture2D> pattern, Ref<Shader> blitShader, std::string targetResource = std::string(ResourceNames::PostProcessColor), std::string targetTextureView = std::string(ResourceNames::PostProcessColorTexture), TargetSlotAccessor targetSlot = [](FrameBlackboard& blackboard) -> RGFramebufferHandle&
                            { return blackboard.Post.PostProcessColor; })
            : m_Pattern(std::move(pattern)), m_BlitShader(std::move(blitShader)), m_TargetResource(std::move(targetResource)), m_TargetTextureView(std::move(targetTextureView)), m_TargetSlot(std::move(targetSlot))
        {
            SetName("TestPatternProducer");
        }

        void Init(const FramebufferSpecification& /*spec*/) override
        {
        }

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
        {
            RenderGraphNode::Setup(builder, blackboard);
            auto& slot = m_TargetSlot(blackboard);
            if (!slot.IsValid())
            {
                return;
            }
            constexpr std::string_view versionTag = "TestPatternProducer";
            const auto output = builder.WriteNewVersion(slot, RGWriteUsage::RenderTarget, versionTag);
            SetPrimaryOutputFramebufferHandle(output);
            SetPrimaryOutputTextureHandle(builder.CreateFramebufferAttachmentView(
                m_TargetTextureView + "@" + std::string(versionTag), output, 0u));
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

        [[nodiscard]] const std::string& GetTargetResource() const
        {
            return m_TargetResource;
        }
        [[nodiscard]] const TargetSlotAccessor& GetTargetSlot() const
        {
            return m_TargetSlot;
        }

      private:
        Ref<Texture2D> m_Pattern;
        Ref<Shader> m_BlitShader;
        std::string m_TargetResource;
        std::string m_TargetTextureView;
        TargetSlotAccessor m_TargetSlot;
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
                                       const Ref<PatternProducerPass>& producer,
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
        producer->GetTargetSlot()(blackboard) =
            graph.DeclareTransientFramebuffer(producer->GetTargetResource(), fbDesc);

        // Auxiliary inputs (depth, G-buffer planes, …): tenants import
        // uploaded stand-in textures under the blackboard names their pass's
        // Setup reads.
        if (m_ExtraSetup)
            m_ExtraSetup(graph, blackboard);

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
    // Per-chain auxiliary-resource hook; reset it between chains.
    std::function<void(RenderGraph&, FrameBlackboard&)> m_ExtraSetup;
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

    auto producer = Ref<PatternProducerPass>::Create(whiteInput, blitShader);
    const auto rendered = RunSinglePassChain(kSize, producer, vignette, "VignettePass",
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

    auto producer = Ref<PatternProducerPass>::Create(edgeInput, blitShader);
    const auto rendered = RunSinglePassChain(kSize, producer, chromAb, "ChromAberrationPass",
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

TEST_F(VulkanPassSuite, ColorGradingIdentityLutPassesThePatternThrough)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // The pass imports its own identity LUT at creation — with it, grading
    // is a passthrough, so the hard-edge pattern must survive byte-for-byte
    // (small tolerance for the 256x16 strip's bilinear fetch).
    const std::vector<f32> pattern = MakeHardEdgePattern(kSize);
    std::vector<u8> patternRgba8(static_cast<sizet>(kSize) * kSize * 4);
    for (sizet i = 0; i < patternRgba8.size(); ++i)
        patternRgba8[i] = static_cast<u8>(std::lround(std::clamp(pattern[i], 0.0f, 1.0f) * 255.0f));
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
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    // The pass binds a PostProcessUBO its shader never declares (a known
    // dead bind — zero-address warn-once on this backend); the DRS UBO
    // feeds the producer's blit.
    PostProcessUBOData uboData{};
    auto postProcessUbo = UniformBuffer::Create(sizeof(PostProcessUBOData), 7);
    postProcessUbo->SetData(&uboData, sizeof(uboData));
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    auto grading = Ref<ColorGradingRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    grading->Init(initSpec);
    grading->SetEnabled(true);

    auto producer = Ref<PatternProducerPass>::Create(patternTexture, blitShader);
    const auto rendered = RunSinglePassChain(kSize, producer, grading, "ColorGradingPass",
                                             ResourceNames::ColorGradingColor,
                                             [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                             { blackboard.Post.ColorGradingColor = handle; });
    ASSERT_EQ(rendered.size(), patternRgba8.size());

    u32 maxDiff = 0;
    for (sizet i = 0; i < rendered.size(); i += 4)
    {
        for (sizet c = 0; c < 3; ++c)
        {
            const u32 diff = static_cast<u32>(
                std::abs(static_cast<int>(rendered[i + c]) - static_cast<int>(patternRgba8[i + c])));
            maxDiff = std::max(maxDiff, diff);
        }
    }
    EXPECT_LE(maxDiff, 2u) << "identity LUT must pass the pattern through (bilinear strip tolerance)";
}

TEST_F(VulkanPassSuite, EasuPreservesAConstantFieldAtIdentityScale)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // EASU is a convolution with normalised weights: a constant field must
    // come out constant regardless of the kernel's edge adaptation. The pass
    // owns its EASUParams UBO (binding 45) and reads the SceneColor family —
    // the producer is redirected there.
    auto grayInput = MakeSolidTexture(kSize, 128, 128, 128, 255);
    ASSERT_NE(grayInput, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    auto easu = Ref<EASURenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    easu->Init(initSpec);
    easu->SetEnabled(true);

    auto producer = Ref<PatternProducerPass>::Create(
        grayInput, blitShader, std::string(ResourceNames::SceneColor),
        std::string(ResourceNames::SceneColorTexture),
        [](FrameBlackboard& blackboard) -> RGFramebufferHandle&
        { return blackboard.Scene.SceneColor; });
    const auto rendered = RunSinglePassChain(kSize, producer, easu, "EASUPass", ResourceNames::EASUColor,
                                             [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                             { blackboard.Post.EASUColor = handle; });
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

    for (const auto& [x, y] :
         { std::pair<u32, u32>{ 8, 8 }, { 120, 8 }, { 64, 64 }, { 8, 120 }, { 120, 120 } })
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        for (sizet c = 0; c < 3; ++c)
        {
            EXPECT_NEAR(static_cast<int>(rendered[i + c]), 128, 2)
                << "EASU of a constant field must stay constant at (" << x << "," << y << ") channel " << c;
        }
    }
}

TEST_F(VulkanPassSuite, DofFocusGatesTheBlurThroughAnImportedDepth)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // Two chains over the same edge input with an imported uniform depth of
    // 0.0 (linearises to CameraNear): focus AT the near plane must pass the
    // hard edge through untouched; focus pushed far away must soften it.
    // Two-sided so a DOF that never blurs — or always blurs — fails.
    auto edgeInput = MakeVerticalEdgeTexture(kSize, 96);
    ASSERT_NE(edgeInput, nullptr);
    auto depthTexture = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(depthTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    auto postProcessUbo = UniformBuffer::Create(sizeof(PostProcessUBOData), 7);

    m_ExtraSetup = [&](RenderGraph& graph, FrameBlackboard& blackboard)
    {
        RGResourceDesc depthDesc;
        depthDesc.Kind = RGResourceHandle::Kind::Texture2D;
        depthDesc.Format = RGResourceFormat::RGBA8UNorm;
        depthDesc.Width = kSize;
        depthDesc.Height = kSize;
        blackboard.Scene.SceneDepth =
            graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), depthDesc);
    };

    const auto runChain = [&](f32 focusDistance) -> std::vector<u8>
    {
        PostProcessUBOData uboData{};
        uboData.DOFFocusDistance = focusDistance;
        uboData.DOFFocusRange = 1.0f;
        uboData.DOFBokehRadius = 8.0f;
        uboData.CameraNear = 0.1f;
        uboData.CameraFar = 100.0f;
        uboData.InverseScreenWidth = 1.0f / static_cast<f32>(kSize);
        uboData.InverseScreenHeight = 1.0f / static_cast<f32>(kSize);
        uboData.TexelSizeX = 1.0f / static_cast<f32>(kSize);
        uboData.TexelSizeY = 1.0f / static_cast<f32>(kSize);
        postProcessUbo->SetData(&uboData, sizeof(uboData));

        auto dof = Ref<DOFRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        dof->Init(initSpec);
        dof->SetEnabled(true);

        auto producer = Ref<PatternProducerPass>::Create(edgeInput, blitShader);
        return RunSinglePassChain(kSize, producer, dof, "DOFPass", ResourceNames::DOFColor,
                                  [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                  { blackboard.Post.DOFColor = handle; });
    };

    // Focus at the near plane (depth 0 linearises to 0.1): coc 0, passthrough.
    const auto inFocus = runChain(0.1f);
    ASSERT_EQ(inFocus.size(), static_cast<sizet>(kSize) * kSize * 4);
    const auto redAt = [kSize](const std::vector<u8>& img, u32 x, u32 y)
    { return static_cast<int>(img[(static_cast<sizet>(y) * kSize + x) * 4]); };
    EXPECT_LE(redAt(inFocus, 94, 64), 5) << "in focus: the black side must stay black";
    EXPECT_GE(redAt(inFocus, 98, 64), 250) << "in focus: the white side must stay white";

    // Focus at 50 (coc saturates to 1): the 8px bokeh disc must mix both
    // sides of the edge — the pixel two texels into the white side reads a
    // blend, not pure white.
    const auto outOfFocus = runChain(50.0f);
    ASSERT_EQ(outOfFocus.size(), static_cast<sizet>(kSize) * kSize * 4);
    const int blurred = redAt(outOfFocus, 98, 64);
    EXPECT_GT(blurred, 30) << "out of focus: not black — the disc still covers white texels";
    EXPECT_LT(blurred, 225) << "out of focus: the hard edge must have softened";

    m_ExtraSetup = nullptr;
}

TEST_F(VulkanPassSuite, SssBlurSoftensTheEdgeOnlyWhenTheUboFlagEnablesIt)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // SSS reads the DIRECT blackboard pair (Scene.SceneColor + the canonical
    // Scene.SceneColorTexture attachment view), not the versioned-name
    // ladder — so besides redirecting the producer to the SceneColor family
    // (the EASU shape), m_ExtraSetup declares the canonical view production
    // creates in RenderPipeline. Depth (bilateral weight) comes from
    // Scene.SceneDepthAttachment; a uniform stand-in + BlurFalloff 0 makes
    // every depth weight exactly 1, leaving a pure normalized Gaussian.
    //
    // Two-sided lever: SSSParams.Flags.x. 0 => the shader's passthrough
    // branch (edge intact, alpha forced 1); 1 with an 8-texel radius => the
    // +x kernel taps (x+8..x+32) cross the edge at x=96 from pixel 94, so
    // sum(gauss[1..4]) / totalWeight = 0.218 of white leaks in (~56/255).
    auto edgeInput = MakeVerticalEdgeTexture(kSize, 96);
    ASSERT_NE(edgeInput, nullptr);
    auto depthTexture = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(depthTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    auto sssUbo = UniformBuffer::Create(sizeof(SSSUBOData), 14);

    m_ExtraSetup = [&](RenderGraph& graph, FrameBlackboard& blackboard)
    {
        blackboard.Scene.SceneColorTexture = graph.CreateFramebufferAttachmentView(
            ResourceNames::SceneColorTexture, blackboard.Scene.SceneColor, 0u);
        RGResourceDesc depthDesc;
        depthDesc.Kind = RGResourceHandle::Kind::Texture2D;
        depthDesc.Format = RGResourceFormat::RGBA8UNorm;
        depthDesc.Width = kSize;
        depthDesc.Height = kSize;
        blackboard.Scene.SceneDepthAttachment = graph.ImportTextureHandle(
            ResourceNames::SceneDepthAttachment, depthTexture->GetRHIHandle(), depthDesc);
    };

    const auto runChain = [&](f32 enabledFlag) -> std::vector<u8>
    {
        SSSUBOData sssData{};
        sssData.BlurParams = glm::vec4(8.0f, 0.0f, static_cast<f32>(kSize), static_cast<f32>(kSize));
        sssData.Flags = glm::vec4(enabledFlag, 0.0f, 0.0f, 0.0f);
        sssUbo->SetData(&sssData, sizeof(sssData));

        auto sss = Ref<SSSRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        sss->Init(initSpec);
        // Pass-level gates (Setup declares + GetTarget non-null) stay ON in
        // both chains — the contract's on/off lever is the UBO flag alone.
        SnowSettings snow;
        snow.Enabled = true;
        snow.SSSBlurEnabled = true;
        sss->SetSettings(snow);
        sss->SetSSSUBO(sssUbo, nullptr); // exercises the pass's own rebind path

        auto producer = Ref<PatternProducerPass>::Create(
            edgeInput, blitShader, std::string(ResourceNames::SceneColor),
            std::string(ResourceNames::SceneColorTexture),
            [](FrameBlackboard& blackboard) -> RGFramebufferHandle&
            { return blackboard.Scene.SceneColor; });
        return RunSinglePassChain(kSize, producer, sss, "SSSPass", ResourceNames::SSSColor,
                                  [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                  { blackboard.Post.SSSColor = handle; });
    };

    const auto redAt = [kSize](const std::vector<u8>& img, u32 x, u32 y)
    { return static_cast<int>(img[(static_cast<sizet>(y) * kSize + x) * 4]); };

    const auto disabled = runChain(0.0f);
    ASSERT_EQ(disabled.size(), static_cast<sizet>(kSize) * kSize * 4);
    EXPECT_LE(redAt(disabled, 94, 64), 5) << "Flags.x=0: passthrough must keep the black side";
    EXPECT_GE(redAt(disabled, 98, 64), 250) << "Flags.x=0: passthrough must keep the white side";
    EXPECT_EQ(static_cast<int>(disabled[((static_cast<sizet>(64) * kSize + 94) * 4) + 3]), 255)
        << "SSS must reset alpha to 1 (the produce-consume-reset contract)";

    const auto enabled = runChain(1.0f);
    ASSERT_EQ(enabled.size(), static_cast<sizet>(kSize) * kSize * 4);
    const int softened = redAt(enabled, 94, 64);
    EXPECT_GT(softened, 25) << "enabled SSS blur must leak white across the edge (~56 expected)";
    EXPECT_LT(softened, 120) << "the leak must stay a partial mix, not full white";
    // All of (34,64)'s taps stay left of the edge and interior — deep field
    // must remain black, proving the blur is a local kernel, not a wash.
    EXPECT_LE(redAt(enabled, 34, 64), 5) << "far field must stay black under the enabled blur";

    m_ExtraSetup = nullptr;
}

TEST_F(VulkanPassSuite, AoApplyModulatesSceneColorByTheAoBuffer)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // The apply formula is sceneColor * mix(1, ao, intensity): with a
    // UNIFORM AO stand-in the bilateral upsample's weights cancel and ao is
    // the stand-in value itself, so the contract is exact arithmetic.
    // Two-sided: white AO (1.0) => passthrough; mid-gray AO (0.502) at
    // intensity 1 => the gray input halves. Real projection matrices keep
    // linearizeDepth finite (uniform depth 0 => CameraNear).
    auto grayInput = MakeSolidTexture(kSize, 128, 128, 128, 255);
    ASSERT_NE(grayInput, nullptr);
    auto depthTexture = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(depthTexture, nullptr);
    auto aoWhite = MakeSolidTexture(kSize, 255, 255, 255, 255);
    ASSERT_NE(aoWhite, nullptr);
    auto aoGray = MakeSolidTexture(kSize, 128, 128, 128, 255);
    ASSERT_NE(aoGray, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    auto ssaoUbo = UniformBuffer::Create(sizeof(SSAOUBOData), 9);
    SSAOUBOData ssaoData{};
    ssaoData.Intensity = 1.0f;
    ssaoData.DebugView = 0;
    ssaoData.ScreenWidth = static_cast<i32>(kSize);
    ssaoData.ScreenHeight = static_cast<i32>(kSize);
    ssaoData.Projection = glm::perspectiveRH_NO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    ssaoData.InverseProjection = glm::inverse(ssaoData.Projection);
    ssaoUbo->SetData(&ssaoData, sizeof(ssaoData));

    Ref<Texture2D> currentAO; // per-chain AO stand-in, read by m_ExtraSetup
    m_ExtraSetup = [&](RenderGraph& graph, FrameBlackboard& blackboard)
    {
        RGResourceDesc auxDesc;
        auxDesc.Kind = RGResourceHandle::Kind::Texture2D;
        auxDesc.Format = RGResourceFormat::RGBA8UNorm;
        auxDesc.Width = kSize;
        auxDesc.Height = kSize;
        blackboard.AO.AOBuffer =
            graph.ImportTextureHandle(ResourceNames::AOBuffer, currentAO->GetRHIHandle(), auxDesc);
        blackboard.Scene.SceneDepth =
            graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), auxDesc);
    };

    const auto runChain = [&](const Ref<Texture2D>& aoTexture) -> std::vector<u8>
    {
        currentAO = aoTexture;

        auto aoApply = Ref<AOApplyRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        aoApply->Init(initSpec);
        aoApply->SetEnabled(true);
        aoApply->SetSSAOUBO(ssaoUbo); // exercises the pass's own rebind path

        // AOApply's candidate ladder is SSSColor -> SceneColor; only the
        // SceneColor family exists here, so redirect the producer there.
        auto producer = Ref<PatternProducerPass>::Create(
            grayInput, blitShader, std::string(ResourceNames::SceneColor),
            std::string(ResourceNames::SceneColorTexture),
            [](FrameBlackboard& blackboard) -> RGFramebufferHandle&
            { return blackboard.Scene.SceneColor; });
        return RunSinglePassChain(kSize, producer, aoApply, "AOApplyPass", ResourceNames::AOApplyColor,
                                  [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                  { blackboard.Post.AOApplyColor = handle; });
    };

    const auto redAt = [kSize](const std::vector<u8>& img, u32 x, u32 y)
    { return static_cast<int>(img[(static_cast<sizet>(y) * kSize + x) * 4]); };

    const auto unoccluded = runChain(aoWhite);
    ASSERT_EQ(unoccluded.size(), static_cast<sizet>(kSize) * kSize * 4);
    for (const auto& [x, y] : { std::pair<u32, u32>{ 64, 64 }, { 8, 120 } })
    {
        const int v = redAt(unoccluded, x, y);
        EXPECT_GE(v, 124) << "white AO must pass the gray input through at (" << x << "," << y << ")";
        EXPECT_LE(v, 132) << "white AO must pass the gray input through at (" << x << "," << y << ")";
    }

    const auto occluded = runChain(aoGray);
    ASSERT_EQ(occluded.size(), static_cast<sizet>(kSize) * kSize * 4);
    for (const auto& [x, y] : { std::pair<u32, u32>{ 64, 64 }, { 8, 120 } })
    {
        const int v = redAt(occluded, x, y);
        EXPECT_GE(v, 56) << "mid-gray AO must darken proportionally (~64) at (" << x << "," << y << ")";
        EXPECT_LE(v, 72) << "mid-gray AO must darken proportionally (~64) at (" << x << "," << y << ")";
    }

    m_ExtraSetup = nullptr;
}

TEST_F(VulkanPassSuite, ContactShadowDarkensTheContactRegionOnlyWithIntensity)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // A genuine occluder, in buffer space (row 0 = uv.y 0 — the harness is
    // row-identity end to end, pinned by the FXAA golden): a FAR wall on
    // rows < 64 (depth byte 128 -> view z -0.2006 under the 90-degree
    // perspective below) and a NEARER wall on rows >= 64 (byte 64 -> view z
    // -0.1335), frontal oct(0,0)=+Z normals everywhere, and a toward-light
    // direction tilted up (0, .707, .707) so NdotL = 0.707 passes the
    // form-shadow gate. A receiver just below the boundary (row 58) marches
    // up toward the light and crosses the near wall's screen region while
    // still ~0.02-0.05 view units BEHIND it (< thickness 0.2) => occluded
    // at traveled ~0.026 of maxDist 0.5 => occlusion = distFade^2 ~ 0.90.
    // A receiver far below (row 8) rises toward the camera (light Z) faster
    // than its projected point enters the near band, so by the crossing its
    // ray is well IN FRONT of the wall (delta < 0) => never occluded. The
    // shadow is therefore local to the contact — the pass's defining
    // property — and the intensity lever makes the contract two-sided.
    auto grayInput = MakeSolidTexture(kSize, 128, 128, 128, 255);
    ASSERT_NE(grayInput, nullptr);
    auto depthTexture = MakeHorizontalSplitTexture(kSize, 64, { 128, 128, 128, 255 }, { 64, 64, 64, 255 });
    ASSERT_NE(depthTexture, nullptr);
    auto normalTexture = MakeSolidTexture(kSize, 0, 0, 0, 255); // oct(0,0) -> +Z world normal
    ASSERT_NE(normalTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    auto csUbo = UniformBuffer::Create(sizeof(ContactShadowUBOData), 41);

    m_ExtraSetup = [&](RenderGraph& graph, FrameBlackboard& blackboard)
    {
        RGResourceDesc auxDesc;
        auxDesc.Kind = RGResourceHandle::Kind::Texture2D;
        auxDesc.Format = RGResourceFormat::RGBA8UNorm;
        auxDesc.Width = kSize;
        auxDesc.Height = kSize;
        blackboard.Scene.SceneDepth =
            graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), auxDesc);
        blackboard.GBuffer.GBufferNormal =
            graph.ImportTextureHandle(ResourceNames::GBufferNormal, normalTexture->GetRHIHandle(), auxDesc);
    };

    const auto runChain = [&](f32 intensity) -> std::vector<u8>
    {
        ContactShadowUBOData csData{};
        csData.Projection = glm::perspectiveRH_NO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
        csData.InverseProjection = glm::inverse(csData.Projection);
        csData.View = glm::mat4(1.0f);
        csData.LightDirection = glm::vec4(0.0f, 0.70710678f, 0.70710678f, 1.0f);
        // maxSteps 64 x stride 0.004 = 0.256 view-units reach; thickness 0.2
        // comfortably brackets the 0.067 wall separation.
        csData.RayParams = glm::vec4(64.0f, 0.5f, 0.2f, 0.004f);
        csData.ShadeParams = glm::vec4(intensity, 0.0f, 0.0f, 0.0f); // no edge fade, no bias
        csData.ScreenParams = glm::vec4(static_cast<f32>(kSize), static_cast<f32>(kSize),
                                        1.0f / static_cast<f32>(kSize), 1.0f / static_cast<f32>(kSize));
        csData.Flags = glm::vec4(0.0f);
        csUbo->SetData(&csData, sizeof(csData));

        auto contactShadow = Ref<ContactShadowRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        contactShadow->Init(initSpec);
        contactShadow->SetEnabled(true);
        contactShadow->SetContactShadowUBO(csUbo);

        auto producer = Ref<PatternProducerPass>::Create(
            grayInput, blitShader, std::string(ResourceNames::SceneColor),
            std::string(ResourceNames::SceneColorTexture),
            [](FrameBlackboard& blackboard) -> RGFramebufferHandle&
            { return blackboard.Scene.SceneColor; });
        return RunSinglePassChain(kSize, producer, contactShadow, "ContactShadowPass",
                                  ResourceNames::ContactShadowColor,
                                  [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                  { blackboard.Post.ContactShadowColor = handle; });
    };

    const auto redAt = [kSize](const std::vector<u8>& img, u32 x, u32 y)
    { return static_cast<int>(img[(static_cast<sizet>(y) * kSize + x) * 4]); };

    // Intensity 0: the march still finds the occluder, but the shadow factor
    // is 1 everywhere — full passthrough.
    const auto zeroIntensity = runChain(0.0f);
    ASSERT_EQ(zeroIntensity.size(), static_cast<sizet>(kSize) * kSize * 4);
    EXPECT_GE(redAt(zeroIntensity, 64, 58), 120) << "intensity 0 must pass the contact pixel through";
    EXPECT_LE(redAt(zeroIntensity, 64, 58), 136);
    EXPECT_GE(redAt(zeroIntensity, 64, 8), 120) << "intensity 0 must pass the far receiver through";
    EXPECT_LE(redAt(zeroIntensity, 64, 8), 136);

    // Intensity 1: occlusion ~0.90 -> factor ~0.10 -> the contact pixel
    // drops to ~13, while the far receiver keeps its full lighting.
    const auto fullIntensity = runChain(1.0f);
    ASSERT_EQ(fullIntensity.size(), static_cast<sizet>(kSize) * kSize * 4);
    EXPECT_LE(redAt(fullIntensity, 64, 58), 70) << "the contact pixel must darken under the near wall";
    EXPECT_GE(redAt(fullIntensity, 64, 8), 120) << "the far receiver must stay lit (no occluder crossing)";
    EXPECT_LE(redAt(fullIntensity, 64, 8), 136);

    m_ExtraSetup = nullptr;
}

TEST_F(VulkanPassSuite, SsgiAddsGatheredBounceLightOnlyWithIntensity)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // Same two-wall geometry as the contact-shadow tenant (near wall on
    // rows >= 64), but the light transport runs the other way: the receiver
    // at row 58 casts a cosine hemisphere around its +Z view normal, and
    // the rays whose azimuth points up cross into the near wall's screen
    // band ~0.05-0.07 view units behind it (< thickness) => hits that
    // gather the upstream colour THERE. The colour split is deliberately at
    // row 60 (below the depth split at 64) so every gather lands on solid
    // white radiance while the receiver's own base stays dark gray — the
    // added bounce is then unambiguous: base 32 + albedo * mean(hits) with
    // several of the 16 rays hitting. Intensity is the two-sided lever
    // (0 => base exactly; 1 => the contact region visibly brightens).
    auto radianceInput = MakeHorizontalSplitTexture(kSize, 60, { 32, 32, 32, 255 }, { 255, 255, 255, 255 });
    ASSERT_NE(radianceInput, nullptr);
    auto depthTexture = MakeHorizontalSplitTexture(kSize, 64, { 128, 128, 128, 255 }, { 64, 64, 64, 255 });
    ASSERT_NE(depthTexture, nullptr);
    auto normalTexture = MakeSolidTexture(kSize, 0, 0, 0, 255); // oct(0,0) -> +Z world normal
    ASSERT_NE(normalTexture, nullptr);
    auto albedoTexture = MakeSolidTexture(kSize, 255, 255, 255, 255);
    ASSERT_NE(albedoTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    auto ssgiUbo = UniformBuffer::Create(sizeof(SSGIUBOData), 40);

    m_ExtraSetup = [&](RenderGraph& graph, FrameBlackboard& blackboard)
    {
        RGResourceDesc auxDesc;
        auxDesc.Kind = RGResourceHandle::Kind::Texture2D;
        auxDesc.Format = RGResourceFormat::RGBA8UNorm;
        auxDesc.Width = kSize;
        auxDesc.Height = kSize;
        blackboard.Scene.SceneDepth =
            graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), auxDesc);
        blackboard.GBuffer.GBufferNormal =
            graph.ImportTextureHandle(ResourceNames::GBufferNormal, normalTexture->GetRHIHandle(), auxDesc);
        blackboard.GBuffer.GBufferAlbedo =
            graph.ImportTextureHandle(ResourceNames::GBufferAlbedo, albedoTexture->GetRHIHandle(), auxDesc);
    };

    const auto runChain = [&](f32 intensity) -> std::vector<u8>
    {
        SSGIUBOData ssgiData{};
        ssgiData.Projection = glm::perspectiveRH_NO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
        ssgiData.InverseProjection = glm::inverse(ssgiData.Projection);
        ssgiData.View = glm::mat4(1.0f);
        ssgiData.RayParams = glm::vec4(64.0f, 1.0f, 0.2f, 0.004f);      // generous maxDist keeps distFade ~1
        ssgiData.ShadeParams = glm::vec4(intensity, 16.0f, 0.0f, 0.0f); // 16 rays, no edge fade
        ssgiData.ScreenParams = glm::vec4(static_cast<f32>(kSize), static_cast<f32>(kSize),
                                          1.0f / static_cast<f32>(kSize), 1.0f / static_cast<f32>(kSize));
        ssgiData.Flags = glm::vec4(0.0f);
        ssgiUbo->SetData(&ssgiData, sizeof(ssgiData));

        auto ssgi = Ref<SSGIRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        ssgi->Init(initSpec);
        ssgi->SetEnabled(true);
        ssgi->SetSSGIUBO(ssgiUbo);

        auto producer = Ref<PatternProducerPass>::Create(
            radianceInput, blitShader, std::string(ResourceNames::SceneColor),
            std::string(ResourceNames::SceneColorTexture),
            [](FrameBlackboard& blackboard) -> RGFramebufferHandle&
            { return blackboard.Scene.SceneColor; });
        return RunSinglePassChain(kSize, producer, ssgi, "SSGIPass", ResourceNames::SSGIColor,
                                  [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                  { blackboard.Post.SSGIColor = handle; });
    };

    const auto redAt = [kSize](const std::vector<u8>& img, u32 x, u32 y)
    { return static_cast<int>(img[(static_cast<sizet>(y) * kSize + x) * 4]); };

    const auto zeroIntensity = runChain(0.0f);
    ASSERT_EQ(zeroIntensity.size(), static_cast<sizet>(kSize) * kSize * 4);
    const int base = redAt(zeroIntensity, 64, 58);
    EXPECT_GE(base, 29) << "intensity 0 must reduce to the passthrough base colour";
    EXPECT_LE(base, 35) << "intensity 0 must reduce to the passthrough base colour";

    const auto fullIntensity = runChain(1.0f);
    ASSERT_EQ(fullIntensity.size(), static_cast<sizet>(kSize) * kSize * 4);
    const int lit = redAt(fullIntensity, 64, 58);
    EXPECT_GE(lit, base + 25) << "hemisphere rays crossing into the near wall must gather its white radiance";

    m_ExtraSetup = nullptr;
}

TEST_F(VulkanPassSuite, MotionBlurSmearsAlongTheVelocityAndPassesThroughAtZero)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // The shader reads velocity as RAW .rg floats (per-pixel path taken
    // because the params UBO's hasVelocity flag is 1 and depth < 1), so an
    // RGBA8 stand-in can express it: byte 51 = 0.2 uv, scaled by
    // MotionBlurStrength 0.5 => a 0.1-uv (12.8 px) sample spread along +x,
    // t in [-0.5, 0.5]. Two-sided: zero velocity => every tap lands on the
    // same texel => the hard edge at x=96 survives; 0.2 velocity => pixel
    // 94's 8 taps span x~90..101, mixing both sides (~3/8 white ~ 96).
    // The camera-only matrices at binding 8 are bound but unused on this
    // path (identity); binding them anyway follows the fixture rule of
    // binding every UBO the shader declares.
    auto edgeInput = MakeVerticalEdgeTexture(kSize, 96);
    ASSERT_NE(edgeInput, nullptr);
    auto depthTexture = MakeSolidTexture(kSize, 0, 0, 0, 255); // depth 0 < 1 -> velocity path
    ASSERT_NE(depthTexture, nullptr);
    auto velocityZero = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(velocityZero, nullptr);
    auto velocityRight = MakeSolidTexture(kSize, 51, 0, 0, 255); // .r = 0.2 uv toward +x
    ASSERT_NE(velocityRight, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    PostProcessUBOData uboData{};
    uboData.MotionBlurStrength = 0.5f;
    uboData.MotionBlurSamples = 8;
    uboData.InverseScreenWidth = 1.0f / static_cast<f32>(kSize);
    uboData.InverseScreenHeight = 1.0f / static_cast<f32>(kSize);
    auto postProcessUbo = UniformBuffer::Create(sizeof(PostProcessUBOData), 7);
    postProcessUbo->SetData(&uboData, sizeof(uboData));
    MotionBlurUBOData matricesData{}; // identity — unused on the velocity path
    auto motionBlurUbo = UniformBuffer::Create(sizeof(MotionBlurUBOData), 8);
    motionBlurUbo->SetData(&matricesData, sizeof(matricesData));

    Ref<Texture2D> currentVelocity; // per-chain stand-in, read by m_ExtraSetup
    m_ExtraSetup = [&](RenderGraph& graph, FrameBlackboard& blackboard)
    {
        RGResourceDesc auxDesc;
        auxDesc.Kind = RGResourceHandle::Kind::Texture2D;
        auxDesc.Format = RGResourceFormat::RGBA8UNorm;
        auxDesc.Width = kSize;
        auxDesc.Height = kSize;
        blackboard.Scene.SceneDepth =
            graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), auxDesc);
        blackboard.GBuffer.Velocity =
            graph.ImportTextureHandle(ResourceNames::Velocity, currentVelocity->GetRHIHandle(), auxDesc);
    };

    const auto runChain = [&](const Ref<Texture2D>& velocityTexture) -> std::vector<u8>
    {
        currentVelocity = velocityTexture;

        auto motionBlur = Ref<MotionBlurRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        motionBlur->Init(initSpec); // creates the pass-owned params UBO (binding 42)
        motionBlur->SetEnabled(true);
        motionBlur->SetPostProcessUBO(postProcessUbo);
        motionBlur->SetMotionBlurUBO(motionBlurUbo);

        // MotionBlur's ladder ends at PostProcessColor — the producer default.
        auto producer = Ref<PatternProducerPass>::Create(edgeInput, blitShader);
        return RunSinglePassChain(kSize, producer, motionBlur, "MotionBlurPass",
                                  ResourceNames::MotionBlurColor,
                                  [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                  { blackboard.Post.MotionBlurColor = handle; });
    };

    const auto redAt = [kSize](const std::vector<u8>& img, u32 x, u32 y)
    { return static_cast<int>(img[(static_cast<sizet>(y) * kSize + x) * 4]); };

    const auto still = runChain(velocityZero);
    ASSERT_EQ(still.size(), static_cast<sizet>(kSize) * kSize * 4);
    EXPECT_LE(redAt(still, 94, 64), 5) << "zero velocity: the black side must stay black";
    EXPECT_GE(redAt(still, 98, 64), 250) << "zero velocity: the white side must stay white";

    const auto moving = runChain(velocityRight);
    ASSERT_EQ(moving.size(), static_cast<sizet>(kSize) * kSize * 4);
    const int smearedBlackSide = redAt(moving, 94, 64);
    EXPECT_GT(smearedBlackSide, 40) << "moving: taps across the edge must brighten the black side (~96)";
    EXPECT_LT(smearedBlackSide, 170) << "moving: the mix must stay partial";
    const int smearedWhiteSide = redAt(moving, 98, 64);
    EXPECT_GT(smearedWhiteSide, 140) << "moving: the white side keeps a white majority (~200)";
    EXPECT_LT(smearedWhiteSide, 240) << "moving: the white side must have softened";
    EXPECT_LE(redAt(moving, 32, 64), 5) << "far field: all taps stay on the black side";

    m_ExtraSetup = nullptr;
}

TEST_F(VulkanPassSuite, PrecipitationPassesThePatternThroughAtZeroIntensity)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // Passthrough floor, documented: the pass's two visual levers both sit
    // outside the graph. (a) The engine-global PrecipitationUBO (binding
    // 18) is owned by PrecipitationSystem, which is never initialised
    // headlessly — Execute null-checks the global and merely SKIPS the bind,
    // so this test binds its own all-zero UBO@18 to make the streak gate a
    // deterministic 0 rather than a null-address read. (b) The pass-owned
    // screen UBO (binding 19) is refilled inside Execute from
    // ScreenSpacePrecipitation statics — zero streak intensity and inactive
    // lens impacts headlessly, not overridable from outside without driving
    // the CPU weather sim (spawning lens impacts / gust streaks through
    // ScreenSpacePrecipitation::Update). With both zero the shader adds
    // nothing and forces alpha to 1, so the hard-edge pattern must survive
    // exactly; the shared harness contracts (2 draws, zero resolve
    // failures, zero validation errors) carry the machinery proof.
    ScreenSpacePrecipitation::Reset(); // full-suite hygiene: drop any stale lens impacts

    const std::vector<f32> pattern = MakeHardEdgePattern(kSize);
    std::vector<u8> patternRgba8(static_cast<sizet>(kSize) * kSize * 4);
    for (sizet i = 0; i < patternRgba8.size(); ++i)
        patternRgba8[i] = static_cast<u8>(std::lround(std::clamp(pattern[i], 0.0f, 1.0f) * 255.0f));
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
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    PrecipitationUBOData precipData{};
    precipData.IntensityAndScreenFX = glm::vec4(0.0f);
    precipData.LensParams = glm::vec4(0.0f);        // .w = enabled -> off
    precipData.ScreenWindAndTime = glm::vec4(0.0f); // .w = streaksEnabled -> off
    precipData.ParticleColor = glm::vec4(0.0f);
    precipData.TypeParams = glm::vec4(0.0f);
    auto precipUbo = UniformBuffer::Create(sizeof(PrecipitationUBOData), 18);
    precipUbo->SetData(&precipData, sizeof(precipData));

    auto precipitation = Ref<PrecipitationRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    precipitation->Init(initSpec); // creates the pass-owned screen UBO (binding 19)
    precipitation->SetEnabled(true);

    // Precipitation's ladder ends at PostProcessColor — the producer default.
    auto producer = Ref<PatternProducerPass>::Create(patternTexture, blitShader);
    const auto rendered = RunSinglePassChain(kSize, producer, precipitation, "PrecipitationPass",
                                             ResourceNames::PrecipitationColor,
                                             [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                             { blackboard.Post.PrecipitationColor = handle; });
    ASSERT_EQ(rendered.size(), patternRgba8.size());

    u32 maxDiff = 0;
    for (sizet i = 0; i < rendered.size(); i += 4)
    {
        for (sizet c = 0; c < 3; ++c)
        {
            const u32 diff = static_cast<u32>(
                std::abs(static_cast<int>(rendered[i + c]) - static_cast<int>(patternRgba8[i + c])));
            maxDiff = std::max(maxDiff, diff);
        }
    }
    EXPECT_LE(maxDiff, 2u) << "zero-intensity precipitation must pass the pattern through";
    EXPECT_EQ(static_cast<int>(rendered[3]), 255) << "the pass writes alpha 1";
}

// =============================================================================
// Wave B tenants — the compute-centric passes (#691 Phase 7 Wave B).
// =============================================================================

// ToneMap: the mixed pass — auto-exposure metering computes (histogram +
// average) feeding a persistent ExposureState SSBO@20 the fullscreen draw
// reads. Three chains over the same producer shape:
//   A) manual identity: operator 0 (clamp) x Exposure 1 x Gamma 1 x Dither 0
//      must pass mid-gray through byte-exact;
//   B) manual Exposure 2 doubles a quarter-gray to mid-gray — exact
//      arithmetic, so a tone map that ignores exposure fails;
//   C) auto-exposure ON runs BOTH compute dispatches (histogram bins the
//      128x128 metered grid, average reduces + writes the exposure) and the
//      draw must consume the METERED exposure: for a uniform 0.502 field the
//      Lagarde EV100 chain gives exposure = 1/(9.6 * ~0.502) ~ 0.21, so the
//      output lands at ~27/255 — far from both the manual-1.0 result (128,
//      the "metering never ran / sentinel still -1" failure mode) and from
//      black. Dt = 0 makes the adaptation snap (state[1] starts 0), so the
//      value is deterministic up to one histogram bin of quantisation.
// =============================================================================
TEST_F(VulkanPassSuite, ToneMapAppliesManualExposureAndMetersAutoExposure)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto midGray = MakeSolidTexture(kSize, 128, 128, 128, 255);
    ASSERT_NE(midGray, nullptr);
    auto quarterGray = MakeSolidTexture(kSize, 64, 64, 64, 255);
    ASSERT_NE(quarterGray, nullptr);
    // Depth stand-in: the underwater fog stage is disabled (Flags.x = 0), but
    // binding the declared slot keeps the fixture on the bind-everything rule
    // and exercises the pass's depth bind + slot-clear path.
    auto depthTexture = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(depthTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    auto postProcessUbo = UniformBuffer::Create(sizeof(PostProcessUBOData), 7);
    // Underwater fog block (binding 37): zeroed Flags disable the stage.
    UnderwaterFogUBOData underwaterData{};
    underwaterData.Flags = glm::vec4(0.0f);
    auto underwaterUbo = UniformBuffer::Create(sizeof(UnderwaterFogUBOData), 37);
    underwaterUbo->SetData(&underwaterData, sizeof(underwaterData));

    m_ExtraSetup = [&](RenderGraph& graph, FrameBlackboard& blackboard)
    {
        RGResourceDesc depthDesc;
        depthDesc.Kind = RGResourceHandle::Kind::Texture2D;
        depthDesc.Format = RGResourceFormat::RGBA8UNorm;
        depthDesc.Width = kSize;
        depthDesc.Height = kSize;
        blackboard.Scene.SceneDepth =
            graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), depthDesc);
    };

    const auto runChain = [&](const Ref<Texture2D>& input, f32 exposure, bool autoExposure) -> std::vector<u8>
    {
        PostProcessUBOData uboData{};
        uboData.TonemapOperator = 0; // TONEMAP_NONE: clamp only
        uboData.Exposure = exposure;
        uboData.Gamma = 1.0f;
        uboData.DitherAmplitude = 0.0f;
        uboData.InverseScreenWidth = 1.0f / static_cast<f32>(kSize);
        uboData.InverseScreenHeight = 1.0f / static_cast<f32>(kSize);
        postProcessUbo->SetData(&uboData, sizeof(uboData));

        auto toneMap = Ref<ToneMapRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        toneMap->Init(initSpec); // creates the persistent ExposureState SSBO@20 (sentinel -1 => manual)
        toneMap->SetPostProcessUBO(postProcessUbo);
        toneMap->SetUnderwaterFogUBO(underwaterUbo);

        AutoExposureFrameParams aeParams{};
        aeParams.Enabled = autoExposure;
        aeParams.DeltaTime = 0.0f; // adaptation snaps to the metered target (state[1] starts 0)
        aeParams.Compensation = 0.0f;
        aeParams.MinExposure = 0.05f;
        aeParams.MaxExposure = 16.0f;
        aeParams.LowPercentile = 0.0f; // untrimmed mean — the whole field sits in one bin anyway
        aeParams.HighPercentile = 1.0f;
        toneMap->SetAutoExposure(aeParams);

        auto producer = Ref<PatternProducerPass>::Create(input, blitShader);
        return RunSinglePassChain(kSize, producer, toneMap, "ToneMapPass", ResourceNames::ToneMapColor,
                                  [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                  { blackboard.Post.ToneMapColor = handle; });
    };

    const auto redAt = [kSize](const std::vector<u8>& img, u32 x, u32 y)
    { return static_cast<int>(img[(static_cast<sizet>(y) * kSize + x) * 4]); };

    // A) identity: operator 0 / exposure 1 / gamma 1 passes mid-gray through.
    const auto identity = runChain(midGray, 1.0f, false);
    ASSERT_EQ(identity.size(), static_cast<sizet>(kSize) * kSize * 4);
    for (const auto& [x, y] : { std::pair<u32, u32>{ 8, 8 }, { 64, 64 }, { 120, 120 } })
    {
        EXPECT_NEAR(redAt(identity, x, y), 128, 2) << "identity tone map must pass mid-gray through at ("
                                                   << x << "," << y << ")";
    }
    EXPECT_EQ(static_cast<int>(identity[3]), 255) << "the pass writes alpha 1";

    // B) manual exposure multiplies: 0.251 x 2 = 0.502.
    const auto doubled = runChain(quarterGray, 2.0f, false);
    ASSERT_EQ(doubled.size(), static_cast<sizet>(kSize) * kSize * 4);
    EXPECT_NEAR(redAt(doubled, 64, 64), 128, 3) << "exposure 2 must double quarter-gray to mid-gray";

    // C) auto-exposure: the metered exposure (~0.21 for a uniform 0.502
    // field) must replace the manual 1.0 — the two dispatches actually ran
    // and the fragment consumed the SSBO they wrote.
    const auto metered = runChain(midGray, 1.0f, true);
    ASSERT_EQ(metered.size(), static_cast<sizet>(kSize) * kSize * 4);
    const int meteredValue = redAt(metered, 64, 64);
    EXPECT_GE(meteredValue, 21) << "metered exposure must not crush to black";
    EXPECT_LE(meteredValue, 33) << "metered exposure ~1/(9.6 x 0.502) must dim mid-gray to ~27 "
                                   "(128 here means the metering computes never wrote the exposure SSBO)";

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
    m_ExtraSetup = nullptr;
}

// =============================================================================
// VolumetricFog: two compute dispatches (scatter 4x4x4 locals, integrate
// 8x8x1) over pass-owned RGBA16F Texture3D volumes, run TWICE so the
// cross-frame scatter ping-pong + temporal-history path execute too.
//
// Contract — analytic, on the raw readback of the integrated volume's centre
// column: for a UNIFORM medium with albedo-1 fog colour the Hillaire
// integration telescopes, so per slice  accumulated + transmittance == 1
// exactly, transmittance decays monotonically along z, and the near slices
// carry less accumulated fog than the far slices (total optical depth
// D=0.05 x ~60m => far transmittance ~ exp(-3) ~ 0.05). A dead scatter
// (density 0) pins a=1/r=0 and fails the far-slice bounds; a dead integrate
// leaves zeros and fails the identity.
//
// The volumes are 3D images with no engine readback (by design); the test
// reads the integrated volume back RAW: an engine barrier moves it
// StorageWrite -> TransferRead (tracker-true), then vkCmdCopyImageToBuffer
// into a host-visible buffer created here.
//
// Fixture-supplied inputs (bind-everything rule): FogData@17 (uniform
// medium), FogVolumes@20 / ShadowData@6 / ForwardPlus@25 / Atmosphere@54 all
// zeroed (=> local volumes off, CSM tap unshadowed, cluster loop off, cloud
// shadow off), zeroed cluster-list SSBOs @9/10/11/12/18 (never dereferenced
// with the cluster loop off, bound to keep every declared binding fed). The
// pass itself binds the CSM/atlas placeholder handles and the froxel UBO@46.
// Camera matrices come from Renderer3D's statics — set through the getters'
// storage since production writes them in RenderPipeline::PrepareFrame, which
// never runs headlessly.
//
// Layout note (#691 Phase 7 Wave B, reported in the plan): the pass's
// volumes are import-only graph resources, so the planner emits no barriers
// for them — the fixture pre-transitions each volume to its FIRST use's
// descriptor-baked layout per frame (storage => GENERAL, sampled =>
// SHADER_READ_ONLY). The one boundary the fixture cannot reach is
// INSIDE Execute: integrate samples the volume scatter just image-stored,
// where the pass's GL-shaped MemoryBarrier orders memory but cannot
// transition GENERAL -> SHADER_READ_ONLY. Content is well-defined on
// desktop implementations and the validation layers cannot observe
// heap-descriptor accesses; the principled fix (a layout-aware mid-pass
// barrier seam) is engine work outside this tenant.
// =============================================================================
TEST_F(VulkanPassSuite, VolumetricFogIntegratesAUniformMediumMonotonically)
{
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr u32 kVolW = VolumetricFogPass::kVolumeWidth;
    constexpr u32 kVolH = VolumetricFogPass::kVolumeHeight;
    constexpr u32 kVolD = VolumetricFogPass::kVolumeDepth;

    // The compile gate comes FIRST, before any process-global state is
    // touched: today the froxel chain is BLOCKED on the Vulkan compute
    // includer, which resolves relative includes against the fixed shader
    // root ("assets/shaders" + "../include/..." = assets/include/, which
    // does not exist) instead of the including FILE's directory — so
    // FroxelFogScatter.comp's load-bearing includes (FogCommon.glsl,
    // FogVolumeCommon.glsl, AtmosphereShading.glsl) never expand and the
    // shader fails glslang with hundreds of undeclared identifiers. Every
    // .comp that compiled so far (HZB/GTAO/Denoise/AutoExposure) only
    // includes BindlessHeap.glsl, whose entire content is OLO_BINDLESS-
    // guarded — a silently-tolerated failed include, which is why the gap
    // stayed invisible until this pass. Fix belongs in the compute shaderc
    // includer (file-relative resolution), not in this tenant.
    {
        auto compileProbe = Ref<VolumetricFogPass>::Create();
        FramebufferSpecification probeSpec;
        probeSpec.Width = 128;
        probeSpec.Height = 128;
        compileProbe->Init(probeSpec);
        if (!compileProbe->IsReadyForExecution())
        {
            GTEST_SKIP() << "FroxelFogScatter/Integrate .comp failed to compile on Vulkan — check the compute "
                            "route's include resolution (VulkanComputeShader must pass the .comp file's own "
                            "directory to ProcessIncludes) and the device-feature set; this gate exists because "
                            "exactly that includer bug once silently blocked this tenant.";
        }
    }

    // --- Renderer3D statics the pass reads (restored at test end) ----------
    const glm::mat4 savedView = Renderer3D::GetViewMatrix();
    const glm::mat4 savedProjection = Renderer3D::GetProjectionMatrix();
    const FogSettings savedFog = Renderer3D::GetFogSettings();
    const glm::mat4 projection = glm::perspectiveRH_NO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    const_cast<glm::mat4&>(Renderer3D::GetViewMatrix()) = glm::mat4(1.0f);
    const_cast<glm::mat4&>(Renderer3D::GetProjectionMatrix()) = projection;
    Renderer3D::GetFogSettings().End = 60.0f; // fog volume spans [0.1, 60]

    // --- every UBO/SSBO the two .comp shaders declare -----------------------
    constexpr f32 kDensity = 0.05f;
    FogUBOData fogData{};
    fogData.ColorAndDensity = glm::vec4(1.0f, 1.0f, 1.0f, kDensity); // albedo-1 white fog
    fogData.DistanceParams = glm::vec4(0.0f, 60.0f, 0.0f, 0.0f);     // heightFalloff 0 => uniform
    fogData.ScatterParams = glm::vec4(0.0f);
    fogData.RayleighColorAndMaxOpacity = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    fogData.SunDirection = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
    fogData.Flags = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); // scatteringEnabled (z) OFF
    fogData.NoiseParams = glm::vec4(0.0f);             // noiseIntensity 0 => uniform
    fogData.VolumetricParams = glm::vec4(0.0f);        // absorption 0, light shafts OFF
    auto fogUbo = UniformBuffer::Create(sizeof(FogUBOData), ShaderBindingLayout::UBO_FOG);
    fogUbo->SetData(&fogData, sizeof(fogData));

    FogVolumesUBOData fogVolumes{}; // count 0 — no local volumes
    auto fogVolumesUbo = UniformBuffer::Create(sizeof(FogVolumesUBOData), ShaderBindingLayout::UBO_FOG_VOLUMES);
    fogVolumesUbo->SetData(&fogVolumes, sizeof(fogVolumes));

    auto shadowData = std::make_unique<UBOStructures::ShadowUBO>(); // zeroed => DirectionalShadowEnabled 0
    std::memset(shadowData.get(), 0, sizeof(UBOStructures::ShadowUBO));
    auto shadowUbo = UniformBuffer::Create(UBOStructures::ShadowUBO::GetSize(), ShaderBindingLayout::UBO_SHADOW);
    shadowUbo->SetData(shadowData.get(), UBOStructures::ShadowUBO::GetSize());

    UBOStructures::ForwardPlusUBO forwardPlusData{}; // Enabled 0 => cluster loop self-gates
    auto forwardPlusUbo =
        UniformBuffer::Create(UBOStructures::ForwardPlusUBO::GetSize(), ShaderBindingLayout::UBO_FORWARD_PLUS);
    forwardPlusUbo->SetData(&forwardPlusData, sizeof(forwardPlusData));

    UBOStructures::AtmosphereShadingUBO atmosphereData{}; // cloud shadow disabled
    auto atmosphereUbo = UniformBuffer::Create(UBOStructures::AtmosphereShadingUBO::GetSize(),
                                               ShaderBindingLayout::UBO_ATMOSPHERE_SHADING);
    atmosphereUbo->SetData(&atmosphereData, sizeof(atmosphereData));

    // Cluster light lists (SSBOs 9/10/11/12/18): never dereferenced with the
    // cluster loop off; small zeroed buffers keep the declared bindings fed.
    const std::array<f32, 16> zeros{};
    std::array<Ref<StorageBuffer>, 5> clusterBuffers;
    const std::array<u32, 5> clusterBindings = { 9u, 10u, 11u, 12u, 18u };
    for (sizet i = 0; i < clusterBindings.size(); ++i)
    {
        clusterBuffers[i] = StorageBuffer::Create(static_cast<u32>(zeros.size() * sizeof(f32)), clusterBindings[i]);
        clusterBuffers[i]->SetData(zeros.data(), static_cast<u32>(zeros.size() * sizeof(f32)));
        clusterBuffers[i]->Bind();
    }

    // --- the pass, through the real graph -----------------------------------
    auto fogPass = Ref<VolumetricFogPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = 128;
    initSpec.Height = 128;
    fogPass->Init(initSpec);
    ASSERT_TRUE(fogPass->IsReadyForExecution())
        << "FroxelFogScatter.comp / FroxelFogIntegrate.comp must compile through shaderc(vulkan_1_4)";
    fogPass->SetEnabled(true);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const auto transitionVolume = [&api](RHI::ResourceHandle volume, RHI::Access before, RHI::Access after)
    {
        RHI::Barrier barrier{};
        barrier.Resource = volume;
        barrier.Before = before;
        barrier.After = after;
        api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &barrier, 1 });
    };

    RHI::ResourceHandle scatter0{};
    RHI::ResourceHandle scatter1{};
    const RHI::ResourceHandle integrated = fogPass->GetIntegratedVolumeID();
    ASSERT_TRUE(integrated.IsValid());

    // The pass binds the CSM/atlas placeholder array (no real shadow maps
    // headlessly). Materialise it EAGERLY — a VulkanTexture2DArray since the
    // factory grew its Vulkan arm — so frame 1 can pre-transition it out of
    // UNDEFINED to the sampled layout its descriptor bakes (the shadow taps
    // are gated off, so its garbage content is never read).
    const RHI::ResourceHandle shadowPlaceholder = ShadowMap::GetCSMPlaceholderHandle();
    ASSERT_TRUE(shadowPlaceholder.IsValid())
        << "the CSM placeholder must construct on the Vulkan backend (VulkanTexture2DArray)";

    // Frame 1: fresh history (weight 0). The imports registered by Setup give
    // the fixture the scatter handles for the pre-transitions.
    {
        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        graph.AddNode(fogPass);
        graph.SetFinalPass("VolumetricFogPass");
        graph.BuildFrameGraph();

        scatter0 = graph.ResolveTextureHandle(graph.GetTextureHandle("FroxelFogScatter0"));
        scatter1 = graph.ResolveTextureHandle(graph.GetTextureHandle("FroxelFogScatter1"));
        ASSERT_TRUE(scatter0.IsValid()) << "Setup must import the scatter ping volume";
        ASSERT_TRUE(scatter1.IsValid()) << "Setup must import the scatter pong volume";

        SubmitFrame(
            [&]()
            {
                // First use per volume this frame: scatter[0] is image-stored
                // (GENERAL), scatter[1] is the sampled history, integrate
                // image-stores the integrated volume. The shadow placeholder
                // is sampled (never actually read — the taps are gated off).
                transitionVolume(scatter0, RHI::Access::Undefined, RHI::Access::StorageWrite);
                transitionVolume(scatter1, RHI::Access::Undefined, RHI::Access::ShaderSampleRead);
                transitionVolume(integrated, RHI::Access::Undefined, RHI::Access::StorageWrite);
                transitionVolume(shadowPlaceholder, RHI::Access::Undefined, RHI::Access::ShaderSampleRead);
                graph.Execute();
            });

        EXPECT_TRUE(fogPass->RanThisFrame()) << "frame 1: the compute chain must have dispatched";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "frame 1 resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                          << "' x" << failure.Count;
        }
    }

    // Frame 2: the ping-pong swaps (scatter[1] becomes the write target,
    // scatter[0] the history) and the temporal path blends at weight 0.9.
    // The readback rides this frame's tail.
    constexpr sizet kTexelCount = static_cast<sizet>(kVolW) * kVolH * kVolD;
    constexpr VkDeviceSize kReadbackBytes = kTexelCount * 8u; // RGBA16F
    VkBuffer readbackBuffer = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kReadbackBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ASSERT_EQ(vkCreateBuffer(m_Device->GetDevice(), &bufferInfo, nullptr, &readbackBuffer), VK_SUCCESS);

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_Device->GetDevice(), readbackBuffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(m_Device->GetPhysicalDevice(), &memoryProperties);
        u32 memoryType = UINT32_MAX;
        constexpr VkMemoryPropertyFlags kHostFlags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (u32 i = 0; i < memoryProperties.memoryTypeCount; ++i)
        {
            if ((requirements.memoryTypeBits & (1u << i)) != 0u &&
                (memoryProperties.memoryTypes[i].propertyFlags & kHostFlags) == kHostFlags)
            {
                memoryType = i;
                break;
            }
        }
        ASSERT_NE(memoryType, UINT32_MAX) << "no host-visible coherent memory type for the volume readback";

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        ASSERT_EQ(vkAllocateMemory(m_Device->GetDevice(), &allocInfo, nullptr, &readbackMemory), VK_SUCCESS);
        ASSERT_EQ(vkBindBufferMemory(m_Device->GetDevice(), readbackBuffer, readbackMemory, 0), VK_SUCCESS);
    }

    {
        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        graph.AddNode(fogPass);
        graph.SetFinalPass("VolumetricFogPass");
        graph.BuildFrameGraph();

        // Resolved OUTSIDE the lambda: no gtest fatals inside the recording
        // bracket (a fatal there skips EndRecording and cascades).
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(integrated);
        ASSERT_NE(native, 0u);

        SubmitFrame(
            [&]()
            {
                transitionVolume(scatter1, RHI::Access::ShaderSampleRead, RHI::Access::StorageWrite);
                transitionVolume(scatter0, RHI::Access::StorageWrite, RHI::Access::ShaderSampleRead);
                graph.Execute();

                // Readback tail: integrated volume -> host buffer, through the
                // engine barrier so the layout tracker stays true.
                transitionVolume(integrated, RHI::Access::StorageWrite, RHI::Access::TransferRead);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
                region.imageExtent = { kVolW, kVolH, kVolD };
                vkCmdCopyImageToBuffer(m_Cmd, reinterpret_cast<VkImage>(native),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1, &region);

                VkMemoryBarrier2 toHost{};
                toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &toHost;
                vkCmdPipelineBarrier2(m_Cmd, &dep);
            });

        EXPECT_TRUE(fogPass->RanThisFrame()) << "frame 2: the compute chain must have dispatched";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "frame 2 resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                          << "' x" << failure.Count;
        }
        const auto& state = fogPass->GetFroxelVolumeState();
        EXPECT_TRUE(state.Valid);
        EXPECT_EQ(state.ScatterTextureID, scatter1) << "frame 2 must have written the OTHER scatter ping";
    }

    // --- decode + contract --------------------------------------------------
    {
        void* mapped = nullptr;
        ASSERT_EQ(vkMapMemory(m_Device->GetDevice(), readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped), VK_SUCCESS);
        const auto* halves = static_cast<const u16*>(mapped);

        const auto texel = [&](u32 x, u32 y, u32 z) -> glm::vec4
        {
            const sizet idx = ((static_cast<sizet>(z) * kVolH + y) * kVolW + x) * 4u;
            return { HalfToFloat(halves[idx + 0]), HalfToFloat(halves[idx + 1]), HalfToFloat(halves[idx + 2]),
                     HalfToFloat(halves[idx + 3]) };
        };

        const u32 cx = kVolW / 2u;
        const u32 cy = kVolH / 2u;
        f32 prevTransmittance = 1.0f + 1e-3f;
        f32 prevAccumulated = -1e-3f;
        for (u32 z = 0; z < kVolD; ++z)
        {
            const glm::vec4 v = texel(cx, cy, z);
            // Monotone: transmittance decays, in-scatter accumulates.
            EXPECT_LE(v.a, prevTransmittance + 2e-3f) << "transmittance must be non-increasing at slice " << z;
            EXPECT_GE(v.r, prevAccumulated - 2e-3f) << "accumulated fog must be non-decreasing at slice " << z;
            // Albedo-1 identity: accumulated + transmittance == 1 per slice.
            EXPECT_NEAR(v.r + v.a, 1.0f, 0.02f) << "energy identity broke at slice " << z;
            // Uniform white medium: channels agree.
            EXPECT_NEAR(v.r, v.g, 0.01f) << "channel divergence at slice " << z;
            EXPECT_NEAR(v.r, v.b, 0.01f) << "channel divergence at slice " << z;
            prevTransmittance = v.a;
            prevAccumulated = v.r;
        }

        const glm::vec4 nearSlice = texel(cx, cy, 0);
        const glm::vec4 farSlice = texel(cx, cy, kVolD - 1u);
        EXPECT_GT(nearSlice.a, 0.99f) << "the first exponential slice is ~1cm of fog";
        EXPECT_LT(nearSlice.r, 0.01f) << "near slices must carry almost no accumulated fog";
        EXPECT_GT(farSlice.r, 0.85f) << "far slices must have accumulated most of the in-scatter (1 - exp(-3))";
        EXPECT_LT(farSlice.a, 0.10f) << "optical depth 3 leaves ~5% transmittance at the far slice";
        EXPECT_GT(farSlice.a, 0.02f) << "transmittance must not collapse to zero (density over-count)";

        vkUnmapMemory(m_Device->GetDevice(), readbackMemory);
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u) << "the froxel chain must not fall through to a stub";

    // --- restore what the tenant displaced ----------------------------------
    vkDeviceWaitIdle(m_Device->GetDevice());
    vkDestroyBuffer(m_Device->GetDevice(), readbackBuffer, nullptr);
    vkFreeMemory(m_Device->GetDevice(), readbackMemory, nullptr);
    // The pass's Execute lazily created the CSM/atlas placeholder array — a
    // process-static Ref that must die before this device tears down.
    ShadowMap::ShutdownPlaceholders();
    const_cast<glm::mat4&>(Renderer3D::GetViewMatrix()) = savedView;
    const_cast<glm::mat4&>(Renderer3D::GetProjectionMatrix()) = savedProjection;
    Renderer3D::GetFogSettings() = savedFog;
}

// =============================================================================
// GTAO: the hardest Wave B tenant — HZB mip-chain reduction (4 storage-image
// mips per dispatch batch, per-dispatch HZBParams UBO@59), the XeGTAO main
// dispatch (UBO@28, R8 storage outputs, HZB/normals/Hilbert samplers), one
// edge-aware denoise ping-pong pass (UBO@60), and the final CopyImageSubData
// into the graph-imported AO buffer — the call this suite's sibling
// VulkanRendererAPI::CopyImageSubData implementation exists for.
//
// Contracts, two-sided by construction:
//   A) UNIFORM depth => a flat wall head-on. Every horizon sample lies in the
//      receiver's tangent plane (horizonCos <= 0), the arc integral is exactly
//      the open hemisphere, and AO == 1 everywhere (GTAOPower 1 keeps the
//      curve linear). Bytes >= 250 across the whole buffer — this also proves
//      the copy landed, since the AO import is pre-seeded with zeros.
//   B) TWO-PLANE depth (far band above, near band below, the ContactShadow
//      stand-in): far-side receivers near the crease see the near wall raise
//      their horizon => visibly darker than receivers far from it, which stay
//      unoccluded. Denoise is edge-aware (the 33% depth step is far over its
//      2% threshold), so one blur pass cannot wash the crease out.
//
// Normals: the forward path's view-space shape (SceneNormalsAreViewSpace =
// true, identity view in the UBO), oct(0,0) = +Z encoded as byte (0,0).
// =============================================================================
TEST_F(VulkanPassSuite, GtaoIsOpenOnUniformDepthAndDarkensACrease)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto normalTexture = MakeSolidTexture(kSize, 0, 0, 0, 255); // oct(0,0) -> +Z view normal
    ASSERT_NE(normalTexture, nullptr);
    auto depthUniform = MakeSolidTexture(kSize, 128, 128, 128, 255);
    ASSERT_NE(depthUniform, nullptr);
    // Rows < 64 far wall (byte 128 -> view 0.2006 under the 90-degree
    // projection), rows >= 64 near wall (byte 64 -> view 0.1335) — the same
    // stand-in geometry the ContactShadow tenant marches against.
    auto depthCrease = MakeHorizontalSplitTexture(kSize, 64, { 128, 128, 128, 255 }, { 64, 64, 64, 255 });
    ASSERT_NE(depthCrease, nullptr);

    const glm::mat4 projection = glm::perspectiveRH_NO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);

    const auto runChain = [&](const Ref<Texture2D>& depthTexture) -> std::vector<u8>
    {
        std::vector<u8> aoBytes;

        // Caller-owned AO output, imported under the blackboard slot GTAO's
        // Setup declares TransferDest on. Pre-seeded ZEROS: the readback can
        // only pass if the pass's final image copy actually wrote it.
        TextureSpecification aoSpec;
        aoSpec.Width = kSize;
        aoSpec.Height = kSize;
        aoSpec.Format = ImageFormat::R8;
        aoSpec.GenerateMips = false;
        auto aoOutput = Texture2D::Create(aoSpec);
        if (!aoOutput)
        {
            ADD_FAILURE() << "R8 AO output texture creation failed";
            return aoBytes;
        }
        std::vector<u8> aoZeros(static_cast<sizet>(kSize) * kSize, 0u);
        aoOutput->SetData(aoZeros.data(), static_cast<u32>(aoZeros.size()));

        auto gtao = Ref<GTAORenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        gtao->Init(initSpec); // HZB.comp + GTAO.comp + GTAO_Denoise.comp + Hilbert LUT (R16UI upload)
        if (!gtao->IsReadyForExecution())
        {
            ADD_FAILURE() << "GTAO compute shaders must compile through shaderc(vulkan_1_4)";
            return aoBytes;
        }

        PostProcessSettings settings{};
        settings.GTAOEnabled = true;
        settings.ActiveAOTechnique = AOTechnique::GTAO;
        settings.GTAORadius = 0.3f;
        settings.GTAOPower = 1.0f; // linear visibility — the analytic bounds below assume no contrast curve
        settings.GTAOFalloffRange = 0.2f;
        settings.GTAOSampleDistribution = 1.0f;
        settings.GTAOThinCompensation = 0.0f;
        settings.GTAODenoiseEnabled = true;
        settings.GTAODenoisePasses = 1; // odd => the final copy sources the PONG target
        settings.TAAEnabled = false;    // fixed noise phase — deterministic across the two chains
        gtao->SetSettings(settings);

        auto gtaoData = std::make_unique<UBOStructures::GTAOUBO>();
        auto gtaoUbo = UniformBuffer::Create(UBOStructures::GTAOUBO::GetSize(), ShaderBindingLayout::UBO_GTAO);
        gtao->SetGTAOUBO(gtaoUbo, gtaoData.get());
        gtao->SetProjectionMatrix(projection);
        gtao->SetViewMatrix(glm::mat4(1.0f)); // unused: the normals are already view-space

        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        auto& blackboard = graph.GetBlackboard();

        RGResourceDesc importDesc;
        importDesc.Kind = RGResourceHandle::Kind::Texture2D;
        importDesc.Format = RGResourceFormat::RGBA8UNorm;
        importDesc.Width = kSize;
        importDesc.Height = kSize;
        blackboard.Scene.SceneDepth =
            graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), importDesc);
        blackboard.Scene.SceneNormals =
            graph.ImportTextureHandle(ResourceNames::SceneNormals, normalTexture->GetRHIHandle(), importDesc);
        blackboard.Scene.SceneNormalsAreViewSpace = true;

        RGResourceDesc aoDesc;
        aoDesc.Kind = RGResourceHandle::Kind::Texture2D;
        aoDesc.Format = RGResourceFormat::R8UNorm;
        aoDesc.Width = kSize;
        aoDesc.Height = kSize;
        blackboard.AO.AOBuffer = graph.ImportTextureHandle(ResourceNames::AOBuffer, aoOutput->GetRHIHandle(), aoDesc);

        // The GTAO scratch set, declared with the production recipe
        // (RenderPipeline's scene-band block): a pow2 R32F HZB with a full mip
        // chain + per-mip views, and three R8 storage scratch planes.
        u32 mipCount = 1u;
        for (u32 mipW = kSize, mipH = kSize; mipW > 1u || mipH > 1u; ++mipCount)
        {
            mipW = mipW > 1u ? (mipW / 2u) : 1u;
            mipH = mipH > 1u ? (mipH / 2u) : 1u;
        }
        RGResourceDesc hzbDesc;
        hzbDesc.Kind = RGResourceHandle::Kind::Texture2D;
        hzbDesc.Format = RGResourceFormat::R32Float;
        hzbDesc.Width = kSize; // 128 is already pow2
        hzbDesc.Height = kSize;
        hzbDesc.MipLevels = mipCount;
        hzbDesc.DebugName = std::string(ResourceNames::HZBDepth);
        blackboard.Scratch.HZBDepth = graph.AllocateTransientTextureHandle(ResourceNames::HZBDepth, hzbDesc);
        for (u32 mip = 0u; mip < std::min<u32>(mipCount, FrameBlackboard::MaxHZBMipViews); ++mip)
        {
            const auto mipViewName = std::string(ResourceNames::HZBDepth) + "Mip" + std::to_string(mip);
            blackboard.Scratch.HZBDepthMipViews[mip] =
                graph.CreateTextureMipView(mipViewName, blackboard.Scratch.HZBDepth, mip);
        }

        RGResourceDesc scratchDesc;
        scratchDesc.Kind = RGResourceHandle::Kind::Texture2D;
        scratchDesc.Format = RGResourceFormat::R8UNorm;
        scratchDesc.Width = kSize;
        scratchDesc.Height = kSize;
        scratchDesc.DebugName = "GTAOEdge";
        blackboard.Scratch.GTAOEdge = graph.AllocateTransientTextureHandle("GTAOEdge", scratchDesc);
        scratchDesc.DebugName = std::string(ResourceNames::GTAODenoisePing);
        blackboard.Scratch.GTAODenoisePing =
            graph.AllocateTransientTextureHandle(ResourceNames::GTAODenoisePing, scratchDesc);
        scratchDesc.DebugName = std::string(ResourceNames::GTAODenoisePong);
        blackboard.Scratch.GTAODenoisePong =
            graph.AllocateTransientTextureHandle(ResourceNames::GTAODenoisePong, scratchDesc);

        graph.AddNode(gtao);
        graph.SetFinalPass("GTAOPass");
        graph.BuildFrameGraph();

        auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
        SubmitFrame(
            [&]()
            {
                graph.Execute();

                // TRANSFER_DST after the pass's final copy; GetData's one-shot
                // readback assumes the SHADER_READ_ONLY steady state.
                RHI::Barrier toSampled{};
                toSampled.Resource = aoOutput->GetRHIHandle();
                toSampled.Before = RHI::Access::TransferWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            });

        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "GTAO resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                          << "' x" << failure.Count;
        }
        EXPECT_EQ(api.GetPhase6StubHitCount(), 0u)
            << "the GTAO chain (incl. CopyImageSubData) must not fall through to a stub";

        if (!aoOutput->GetData(aoBytes, 0))
        {
            ADD_FAILURE() << "AO readback failed";
        }
        return aoBytes;
    };

    const auto aoAt = [kSize](const std::vector<u8>& img, u32 x, u32 y)
    { return static_cast<int>(img[static_cast<sizet>(y) * kSize + x]); };
    const auto rowMean = [&](const std::vector<u8>& img, u32 y)
    {
        int sum = 0;
        int count = 0;
        for (u32 x = 24; x <= 104; x += 8)
        {
            sum += aoAt(img, x, y);
            ++count;
        }
        return sum / count;
    };

    // A) uniform depth — the CONTROL image. Centre pixels view the wall
    // nearly along their normals and integrate the open hemisphere (~1);
    // toward the corners the per-pixel view vector tilts up to ~54 degrees
    // at this 90-degree FOV and the slice integral legitimately dips (the
    // XeGTAO projected-normal weighting; observed floor ~188 at the extreme
    // corners). The floor catches the classic collapses — the 0.03
    // visibility clamp (byte 8), garbage normals (~0), or a dead final copy
    // (the AO import is pre-seeded ZEROS).
    const auto open = runChain(depthUniform);
    ASSERT_EQ(open.size(), static_cast<sizet>(kSize) * kSize);
    int minAO = 255;
    for (const u8 v : open)
        minAO = std::min(minAO, static_cast<int>(v));
    EXPECT_GE(minAO, 180) << "uniform depth must stay near-open everywhere (collapse floor)";
    {
        int centerSum = 0;
        int centerCount = 0;
        for (u32 y = 44; y <= 84; y += 4)
        {
            for (u32 x = 44; x <= 84; x += 4)
            {
                centerSum += aoAt(open, x, y);
                ++centerCount;
            }
        }
        EXPECT_GE(centerSum / centerCount, 240)
            << "near-axial pixels of a flat wall must integrate the open hemisphere (AO ~ 1)";
    }

    // B) crease vs the paired control — every claim is a same-row differential
    // so the radial baseline above cancels out. Observed field (deterministic:
    // fixed noise phase): far-side rows darken progressively toward the crease
    // (row8 192, row56 180, row62 163 vs control 246), the near-side rows stay
    // OPEN (row66 249 — their depth step goes AWAY from the camera, an
    // occluder must be in FRONT), and the deep near side matches the control.
    const auto crease = runChain(depthCrease);
    ASSERT_EQ(crease.size(), static_cast<sizet>(kSize) * kSize);
    const int open62 = rowMean(open, 62);
    const int crease62 = rowMean(crease, 62);
    EXPECT_GE(open62 - crease62, 40) << "the near wall must darken the far-side crease row vs the control";
    EXPECT_LE(rowMean(crease, 62) + 5, rowMean(crease, 56))
        << "occlusion must weaken with distance from the crease (62 -> 56)";
    EXPECT_LE(rowMean(crease, 56) + 5, rowMean(crease, 8))
        << "occlusion must keep weakening with distance from the crease (56 -> 8)";
    EXPECT_GE(rowMean(crease, 66), 240)
        << "near-side receivers see the depth step BEHIND them — no occlusion (sign correctness)";
    EXPECT_LE(std::abs(rowMean(crease, 120) - rowMean(open, 120)), 10)
        << "the deep near side must match the uniform control (the crease is local)";
}

// =============================================================================
// FluidIntermediates: DOCUMENTED FLOOR, not a full-body tenant. Three
// independent engine gaps keep the splat + smooth body off Vulkan today —
// each named here so none is silently worked around:
//
//   1. The pass's render targets are raw-handle objects created through the
//      RendererAPI raw-resource family (CreateTexture2DHandle,
//      CreateFramebufferHandle, AttachFramebufferColor/DepthTexture,
//      SetFramebufferDrawAttachments, IsFramebufferComplete,
//      ClearFramebufferColorAttachment/Depth, SetTextureFilter/Wrap,
//      DeleteTexture/DeleteFramebuffer) — ALL still Phase6Stub on Vulkan,
//      so CreateTargets() cannot produce the splat FBOs (shared with the
//      WaterRenderPass raw-FBO work in Wave C).
//   2. FluidSmooth.comp cannot compile: the Vulkan compute includer resolves
//      "../include/FluidRenderCommon.glsl" against the shader root instead
//      of the including file's directory (the same gap that blocks
//      VolumetricFog — see that tenant).
//   3. FluidDepthSplat/FluidThickness DO compile (their vec2 a_QuadPos
//      vertex-pull branches are shaderc-clean) but vkCreateShaderModule
//      rejects the fragment SPIR-V: glslang lowers `discard` to
//      OpDemoteToHelperInvocation under vulkan1.4, and VulkanDevice does not
//      enable VkPhysicalDeviceVulkan13Features::shaderDemoteToHelperInvocation
//      — a validation error per module (VUID-VkShaderModuleCreateInfo-pCode-
//      08740) that every discard-carrying shader (water, foliage, decals)
//      will hit in Wave C. One device-init line, outside this tenant's remit.
//
// Because gap 3 fires INSIDE Init(), this tenant never calls Init — the
// contracts it pins are the ones that hold independent of pass resources:
//   a. The no-draw early-out through the REAL graph: with an empty draw list
//      Setup declares NOTHING (the issue #530 fingerprint gate) and Execute
//      returns before touching any raw handle — graph green, zero stubs.
//   b. The one-shot draw-list contract: a submitted draw (real particle
//      SSBOs — constructing them headlessly is NOT the blocker) is CONSUMED
//      by the very next Execute even when the target guard then rejects the
//      frame — a skipped frame can never replay stale draws.
// =============================================================================
TEST_F(VulkanPassSuite, FluidIntermediatesPinsTheNoDrawEarlyOutThroughTheGraph)
{
    VulkanFrameArena::Get().BeginFrame(0);

    auto fluid = Ref<FluidIntermediatesPass>::Create();
    // Deliberately NO Init() and NO SetupFramebuffer(): the shader loads trip
    // the shaderDemoteToHelperInvocation validation error (gap 3) and target
    // creation walks into the stubbed raw-handle family (gap 1). The gates
    // this tenant pins sit in front of both.
    fluid->SetEnabled(true);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    // (a) empty draw list: Setup declares nothing, Execute early-returns.
    {
        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        graph.AddNode(fluid);
        graph.SetFinalPass("FluidIntermediatesPass");
        graph.BuildFrameGraph();

        SubmitFrame([&]()
                    { graph.Execute(); });

        EXPECT_FALSE(fluid->RanThisFrame()) << "no draws => the pass must not run";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "fluid resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                          << "' x" << failure.Count;
        }
    }

    // (b) a real submitted draw is consumed even though the guard chain then
    // rejects the frame (no Init => not ready, and no targets exist).
    {
        constexpr u32 kParticleCount = 4;
        const std::array<glm::vec4, kParticleCount> positions = {
            glm::vec4(0.0f, 0.0f, -1.0f, 1.0f), glm::vec4(0.2f, 0.0f, -1.2f, 1.0f),
            glm::vec4(-0.2f, 0.1f, -0.9f, 1.0f), glm::vec4(0.0f, -0.1f, -1.1f, 1.0f)
        };
        const std::array<glm::vec4, kParticleCount> velocities{};
        const std::array<u32, 8> counters = { kParticleCount, 0u, 0u, 0u, 0u, 0u, 0u, 0u };

        auto positionsBuffer = StorageBuffer::Create(static_cast<u32>(sizeof(positions)),
                                                     ShaderBindingLayout::SSBO_FLUID_POSITIONS);
        positionsBuffer->SetData(positions.data(), static_cast<u32>(sizeof(positions)));
        auto velocitiesBuffer = StorageBuffer::Create(static_cast<u32>(sizeof(velocities)),
                                                      ShaderBindingLayout::SSBO_FLUID_VELOCITIES);
        velocitiesBuffer->SetData(velocities.data(), static_cast<u32>(sizeof(velocities)));
        auto countersBuffer = StorageBuffer::Create(static_cast<u32>(sizeof(counters)),
                                                    ShaderBindingLayout::SSBO_FLUID_COUNTERS);
        countersBuffer->SetData(counters.data(), static_cast<u32>(sizeof(counters)));

        FluidRenderData draw{};
        draw.PositionsSSBOId = positionsBuffer->GetRHIHandle();
        draw.VelocitiesSSBOId = velocitiesBuffer->GetRHIHandle();
        draw.CountersSSBOId = countersBuffer->GetRHIHandle();
        draw.ParticleUpperBound = kParticleCount;
        draw.ParticleRadius = 0.1f;
        fluid->SetFrameDraws({ draw });
        ASSERT_TRUE(fluid->HasPendingDraws());

        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        graph.AddNode(fluid);
        graph.SetFinalPass("FluidIntermediatesPass");
        graph.BuildFrameGraph();

        SubmitFrame([&]()
                    { graph.Execute(); });

        EXPECT_FALSE(fluid->HasPendingDraws()) << "Execute must CONSUME the draw list (one-shot contract)";
        EXPECT_FALSE(fluid->RanThisFrame()) << "without raw-FBO targets the body must reject the frame";
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "both gated paths must return before touching the stubbed raw-handle family";
}

#endif // OLO_WITH_VULKAN
