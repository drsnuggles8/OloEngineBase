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
#include "OloEngine/Renderer/Passes/BloomRenderPass.h"
#include "OloEngine/Renderer/Passes/ChromaticAberrationRenderPass.h"
#include "OloEngine/Renderer/Passes/CloudscapeRenderPass.h"
#include "OloEngine/Renderer/Passes/ColorGradingRenderPass.h"
#include "OloEngine/Renderer/Passes/ContactShadowRenderPass.h"
#include "OloEngine/Renderer/Passes/DOFRenderPass.h"
#include "OloEngine/Renderer/Passes/DepthVelocityUpscalePass.h"
#include "OloEngine/Renderer/Passes/EASURenderPass.h"
#include "OloEngine/Renderer/Passes/FXAARenderPass.h"
#include "OloEngine/Renderer/Passes/FluidIntermediatesPass.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"
#include "OloEngine/Renderer/Passes/GTAORenderPass.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"
#include "OloEngine/Renderer/Passes/FluidCompositePass.h"
#include "OloEngine/Renderer/Passes/MotionBlurRenderPass.h"
#include "OloEngine/Renderer/Passes/OITPrepareRenderPass.h"
#include "OloEngine/Renderer/Passes/OITResolveRenderPass.h"
#include "OloEngine/Renderer/Passes/OverdrawRenderPass.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/PrecipitationRenderPass.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/Passes/SSGIRenderPass.h"
#include "OloEngine/Renderer/Passes/SSRRenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"
#include "OloEngine/Renderer/Passes/VolumetricFogPass.h"
#include "OloEngine/Renderer/Texture3D.h"
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
#include <glm/ext/matrix_transform.hpp>
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
#include <span>
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
            if (ClearTargetFirst)
            {
                // Folds into the scope's loadOp on Vulkan: color -> clear
                // color, depth -> 1.0 (the far plane the depth-seed chain's
                // fallback contract needs to displace).
                context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
                context.Clear();
            }
            if (WriteDepth)
            {
                // Author depth: the raw-NDC triangle sits at z = 0, which
                // rasterises to depth 0.0 on Vulkan (no [-1,1] remap for
                // pass-through vertices) — Always so the clear value never
                // gates the write.
                context.SetDepthTest(true);
                context.SetDepthMask(true);
                RenderCommand::SetDepthFunc(RHI::CompareOp::Always);
            }
            else
            {
                context.SetDepthTest(false);
                context.SetDepthMask(false);
            }
            context.SetBlendState(false);
            context.SetCulling(false);

            m_BlitShader->Bind();
            // External preloaded content — bound directly (upload seeded its
            // layout), the material-texture shape.
            RenderCommand::BindTexture(0, m_Pattern->GetRHIHandle());

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            context.DrawIndexed(va);
            if (WriteDepth)
            {
                // Leave the recorded state at the fixture default.
                RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
                context.SetDepthMask(false);
                context.SetDepthTest(false);
            }
            framebuffer->Unbind();
            DidDraw = true;
        }

        [[nodiscard]] Ref<Framebuffer> GetTarget() const override
        {
            return nullptr;
        }

        // The reachability cull walks DECLARED read edges backward from the
        // final pass. A tenant whose pass consumes the produced content
        // through an ATTACHMENT VIEW of the framebuffer (OITPrepare's depth
        // read) — or through no declared edge at all — leaves the producer
        // unreachable and culled. Marking it side-effecting keeps it, which
        // matches what it is: an authored-content source, not derived work.
        [[nodiscard]] bool IsSideEffecting() const override
        {
            return TreatAsSideEffecting;
        }

        // Diagnosis seam: false after Execute means the resolve guard
        // early-returned and nothing recorded.
        bool DidDraw = false;
        // Opt-in (see IsSideEffecting): defaults off so the versioned-input
        // tenants keep exercising the real reachability chain.
        bool TreatAsSideEffecting = false;
        // OITPrepare's depth-seed chain (#691 Wave C): clear the target
        // (color + depth -> 1.0) at scope open, then author depth from the
        // raw-NDC triangle (z = 0 -> Vulkan depth 0.0).
        bool ClearTargetFirst = false;
        bool WriteDepth = false;

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
                toSampled.Before = m_OutputBarrierBefore;
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
    // The LAST access the chain performs on the output attachment before the
    // harness's readback barrier. ColorAttachmentWrite for a plain
    // draw-terminated chain; a pass whose Setup EXTRACTS the output (TAA's
    // history copy) leaves it transfer-READ, and lowering the barrier's src
    // scope from the wrong access is a WRITE_AFTER_READ hazard the sync
    // validation names (the layout half is tracker-exact either way).
    RHI::Access m_OutputBarrierBefore = RHI::Access::ColorAttachmentWrite;
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

// =============================================================================
// Wave A MEDIUM tenants (#691 Phase 7) — the multi-draw / multi-resolution /
// history-carrying passes. FinalRenderPass is deliberately absent (it needs
// the swapchain import — the FrameRenderCallback live bring-up slice).
// =============================================================================

// Bloom: threshold extract -> 4 progressive downsamples -> 4 additive tent
// upsamples -> composite, across the five graph-owned BloomMips scratch
// framebuffers (AllowSamePassReadWrite — each mip is rendered then sampled
// inside one Execute). The pass mutates the shared PostProcessUBO's TexelSize
// per mip via SetData mid-pass — arena-versioned addresses on Vulkan, so each
// draw reads the range minted for it (GL command ordering with no hazard
// tracking). Contract, two chains: a single bright blob spreads into a halo
// wider than itself (core saturated, ring outside the blob lit, far corner
// near-black and darker than the ring), and an all-black input stays black
// through the whole chain (threshold-of-black = 0, additive chain of zeros).
TEST_F(VulkanPassSuite, BloomSpreadsABrightBlobIntoAHaloAndKeepsBlackBlack)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // Black field with an 8x8 white blob centred at (64, 64).
    const auto makeBlobTexture = [&](bool withBlob) -> Ref<Texture2D>
    {
        std::vector<u8> pixels(static_cast<sizet>(kSize) * kSize * 4, 0u);
        for (sizet i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255u;
        if (withBlob)
        {
            for (u32 y = 60; y < 68; ++y)
            {
                for (u32 x = 60; x < 68; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
                    pixels[i + 0] = 255u;
                    pixels[i + 1] = 255u;
                    pixels[i + 2] = 255u;
                }
            }
        }
        TextureSpecification spec;
        spec.Width = kSize;
        spec.Height = kSize;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        auto texture = Texture2D::Create(spec);
        if (texture)
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
        return texture;
    };
    auto blobInput = makeBlobTexture(true);
    ASSERT_NE(blobInput, nullptr);
    auto blackInput = makeBlobTexture(false);
    ASSERT_NE(blackInput, nullptr);

    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    // Shared PostProcessUBO@7 + the CPU mirror the pass mutates per mip.
    PostProcessUBOData uboData{};
    uboData.BloomThreshold = 0.5f;
    uboData.BloomIntensity = 1.0f;
    uboData.TexelSizeX = 1.0f / static_cast<f32>(kSize);
    uboData.TexelSizeY = 1.0f / static_cast<f32>(kSize);
    uboData.InverseScreenWidth = 1.0f / static_cast<f32>(kSize);
    uboData.InverseScreenHeight = 1.0f / static_cast<f32>(kSize);
    auto postProcessUbo = UniformBuffer::Create(sizeof(PostProcessUBOData), 7);
    postProcessUbo->SetData(&uboData, sizeof(uboData));

    const auto runChain = [&](const Ref<Texture2D>& input) -> std::vector<u8>
    {
        std::vector<u8> rendered;

        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        auto& blackboard = graph.GetBlackboard();

        RGResourceDesc fbDesc;
        fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        fbDesc.Format = RGResourceFormat::RGBA8UNorm;
        fbDesc.Width = kSize;
        fbDesc.Height = kSize;
        blackboard.Post.PostProcessColor =
            graph.DeclareTransientFramebuffer(ResourceNames::PostProcessColor, fbDesc);

        // The five bloom mips, halving from kSize/2 — the production recipe
        // (RenderPipeline's BloomMip block), pooled + RGBA16F.
        u32 mipW = kSize / 2u;
        u32 mipH = kSize / 2u;
        for (u32 i = 0; i < 5u && mipW >= 2u && mipH >= 2u; ++i)
        {
            RGResourceDesc mipDesc;
            mipDesc.Kind = RGResourceHandle::Kind::Framebuffer;
            mipDesc.Format = RGResourceFormat::RGBA16Float;
            mipDesc.Width = mipW;
            mipDesc.Height = mipH;
            mipDesc.DebugName = "BloomMip" + std::to_string(i);
            blackboard.Scratch.BloomMips[i] = graph.DeclareTransientFramebuffer(mipDesc.DebugName, mipDesc);
            mipW /= 2u;
            mipH /= 2u;
        }

        FramebufferSpecification outputSpec;
        outputSpec.Width = kSize;
        outputSpec.Height = kSize;
        outputSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outputSpec);
        if (!outputFramebuffer)
        {
            ADD_FAILURE() << "bloom output framebuffer creation failed";
            return rendered;
        }
        blackboard.Post.BloomColor =
            graph.DeclareTransientFramebuffer(ResourceNames::BloomColor, fbDesc, outputFramebuffer);

        auto bloom = Ref<BloomRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        bloom->Init(initSpec);
        bloom->SetEnabled(true);
        bloom->SetPostProcessUBO(postProcessUbo);
        bloom->SetPostProcessGPUData(&uboData); // Execute mutates TexelSize per mip through this

        auto producer = Ref<PatternProducerPass>::Create(input, blitShader);
        graph.AddNode(producer);
        graph.AddNode(bloom);
        graph.SetFinalPass("BloomPass");
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

        EXPECT_TRUE(producer->DidDraw) << "bloom: the producer pass early-returned";
        EXPECT_TRUE(bloom->GetTarget()) << "bloom: Execute early-returned (input/output/mips/shader guard)";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "bloom resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                          << "' x" << failure.Count;
        }
        {
            auto& vkApi = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
            // producer + threshold + 4 downsamples + 4 upsamples + composite.
            EXPECT_EQ(vkApi.GetPreparedDrawsThisRecording(), 11u) << "bloom mip loop draw count";
            EXPECT_EQ(vkApi.GetDroppedDrawsThisRecording(), 0u);
        }

        auto* vkOutput = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
        if (!vkOutput->GetColorAttachmentImage(0)->GetData(rendered, 0))
            ADD_FAILURE() << "bloom output readback failed";
        return rendered;
    };

    const auto redAt = [kSize](const std::vector<u8>& img, u32 x, u32 y)
    { return static_cast<int>(img[(static_cast<sizet>(y) * kSize + x) * 4]); };

    const auto bloomed = runChain(blobInput);
    ASSERT_EQ(bloomed.size(), static_cast<sizet>(kSize) * kSize * 4);
    EXPECT_GE(redAt(bloomed, 64, 64), 250) << "the blob core must stay saturated (scene + bloom)";
    const int halo = redAt(bloomed, 70, 64);
    EXPECT_GE(halo, 10) << "2 px outside the blob the additive upsample chain must have spread energy";
    const int farCorner = redAt(bloomed, 120, 120);
    EXPECT_LE(farCorner, 12) << "the far corner sits outside the tent chain's reach";
    EXPECT_GT(halo, farCorner + 5) << "the halo must decay with distance from the blob";

    const auto black = runChain(blackInput);
    ASSERT_EQ(black.size(), static_cast<sizet>(kSize) * kSize * 4);
    int maxBlack = 0;
    for (sizet i = 0; i < black.size(); i += 4)
    {
        maxBlack = std::max(maxBlack, static_cast<int>(black[i]));
        maxBlack = std::max(maxBlack, static_cast<int>(black[i + 1]));
        maxBlack = std::max(maxBlack, static_cast<int>(black[i + 2]));
    }
    EXPECT_LE(maxBlack, 2) << "a black input must pass the threshold + additive chain as black";

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
}

// =============================================================================
// SSAO: DOCUMENTED FLOOR, not a full-body tenant. The pass's Init creates its
// 4x4 RG16F rotation-noise texture through the raw-handle texture family —
// CreateTexture2DHandle + UploadTextureSubImage2D + SetTextureFilter +
// SetTextureWrap — which is ENTIRELY Phase6Stub on Vulkan (the same family
// that blocks FluidIntermediates' raw FBOs; it is NOT in this wave's
// sanctioned stub-fix list, so it is reported rather than implemented). With
// the noise handle invalid, IsReadyForExecution() is false and Execute
// early-returns before its first resolve.
//
// A second, independent gap this tenant documents (found while auditing the
// noise bind): the EXPLICIT RHI::SamplerDesc the pass passes for its
// Nearest/Repeat noise sampler is unreachable on this backend today —
//   * heap OFF (this fixture): HeapBinding::BindTextureOrOffsetImpl's
//     fallback calls plain BindTexture(slot, texture) and DROPS the
//     SamplerDesc argument entirely;
//   * heap ON: the Vulkan draw path builds every pipeline with the single
//     DefaultEmbeddedSampler (linear / clamp-to-edge —
//     VulkanPipelineBuilder::GetOrCreateGraphics is called with no
//     embeddedSampler override), so a per-binding Nearest/Repeat sampler has
//     no route into the descriptor mapping either.
// Both must land before SSAO's noise sampling is correct on Vulkan.
//
// What this floor pins: the exact 4-stub blocking set at Init (this test
// FAILS the moment someone implements the family — the prompt to promote it
// to a full 2-draw + CopyImageSubData tenant), and the clean early-out
// through the real graph (declares everything, resolves nothing, zero draws,
// zero resolve failures, zero validation errors).
// =============================================================================
TEST_F(VulkanPassSuite, SsaoPinsTheRawTextureStubFloorThroughTheGraph)
{
    constexpr u32 kSize = 128;
    constexpr u32 kHalf = kSize / 2;
    VulkanFrameArena::Get().BeginFrame(0);

    auto depthTexture = MakeSolidTexture(kSize, 128, 128, 128, 255);
    ASSERT_NE(depthTexture, nullptr);
    auto normalTexture = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(normalTexture, nullptr);
    auto aoOutput = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(aoOutput, nullptr);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto ssao = Ref<SSAORenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    ssao->Init(initSpec); // CreateNoiseTexture walks the stubbed raw-handle family

    EXPECT_EQ(api.GetPhase6StubHitCount() - stubsBefore, 4u)
        << "Init must hit exactly CreateTexture2DHandle + UploadTextureSubImage2D + SetTextureFilter + "
           "SetTextureWrap — if this shrank, the raw-texture family gained a Vulkan arm: promote this "
           "floor to the full SSAO tenant (2 half-res draws + CopyImageSubData + Nearest/Repeat noise)";
    EXPECT_FALSE(ssao->IsReadyForExecution()) << "an invalid noise handle must gate execution off";

    PostProcessSettings settings{};
    settings.SSAOEnabled = true;
    settings.ActiveAOTechnique = AOTechnique::SSAO;
    ssao->SetSettings(settings);
    SSAOUBOData ssaoData{};
    auto ssaoUbo = UniformBuffer::Create(sizeof(SSAOUBOData), 9);
    ssaoUbo->SetData(&ssaoData, sizeof(ssaoData));
    ssao->SetSSAOUBO(ssaoUbo, &ssaoData);

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
    blackboard.AO.AOBuffer =
        graph.ImportTextureHandle(ResourceNames::AOBuffer, aoOutput->GetRHIHandle(), importDesc);

    RGResourceDesc scratchDesc;
    scratchDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    scratchDesc.Format = RGResourceFormat::RG16Float;
    scratchDesc.Width = kHalf;
    scratchDesc.Height = kHalf;
    scratchDesc.DebugName = "SSAORaw";
    blackboard.Scratch.SSAORaw = graph.DeclareTransientFramebuffer("SSAORaw", scratchDesc);
    scratchDesc.DebugName = std::string(ResourceNames::SSAOBlur);
    blackboard.Scratch.SSAOBlur = graph.DeclareTransientFramebuffer(ResourceNames::SSAOBlur, scratchDesc);

    graph.AddNode(ssao);
    graph.SetFinalPass("SSAOPass");
    graph.BuildFrameGraph();

    SubmitFrame([&]()
                { graph.Execute(); });

    EXPECT_FALSE(ssao->GetTarget()) << "not-ready SSAO must reject the frame before its first resolve";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "SSAO resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                      << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 0u) << "the gated pass must record no draws";
    EXPECT_EQ(api.GetPhase6StubHitCount() - stubsBefore, 4u)
        << "Execute must early-return without touching another stubbed entry";
}

// =============================================================================
// SelectionOutline: JFA init (the suite's first INTEGER-sampler consumer —
// isampler2D over an imported R32I entity-ID stand-in, read with texelFetch,
// so the pipeline's linear embedded sampler is never used to filter) -> two
// ping-pong flood iterations across the RGBA32F JFAPing/JFAPong scratch pair
// (AllowSamePassReadWrite) -> composite. The UBO@29 step value changes per
// flood iteration mid-pass (arena-versioned SetData).
//
// Contract: a selected 32x32 id blob yields a ring of the configured colour
// straddling its boundary — pure outline 1 px outside the edge, scene
// passthrough deep inside the blob and far outside it — and with an empty
// selection the pass contributes nothing (GetTarget null, producer-only
// draw count).
// =============================================================================
TEST_F(VulkanPassSuite, SelectionOutlineRingsTheSelectedBlobAndIdlesWithoutSelection)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // Entity-ID stand-in: R32I, id 7 in [48, 80)^2, -1 elsewhere (the
    // engine's "no entity" sentinel).
    Ref<Texture2D> entityTexture;
    {
        std::vector<i32> ids(static_cast<sizet>(kSize) * kSize, -1);
        for (u32 y = 48; y < 80; ++y)
            for (u32 x = 48; x < 80; ++x)
                ids[static_cast<sizet>(y) * kSize + x] = 7;
        TextureSpecification spec;
        spec.Width = kSize;
        spec.Height = kSize;
        spec.Format = ImageFormat::R32I;
        spec.GenerateMips = false;
        entityTexture = Texture2D::Create(spec);
        ASSERT_NE(entityTexture, nullptr);
        entityTexture->SetData(ids.data(), static_cast<u32>(ids.size() * sizeof(i32)));
    }

    auto sceneInput = MakeSolidTexture(kSize, 90, 90, 90, 255);
    ASSERT_NE(sceneInput, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    const auto runChain = [&](std::span<const i32> selectedIds, u32& preparedDraws,
                              bool& targetValid) -> std::vector<u8>
    {
        std::vector<u8> rendered;

        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        auto& blackboard = graph.GetBlackboard();

        RGResourceDesc fbDesc;
        fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        fbDesc.Format = RGResourceFormat::RGBA8UNorm;
        fbDesc.Width = kSize;
        fbDesc.Height = kSize;
        blackboard.Post.PostProcessColor =
            graph.DeclareTransientFramebuffer(ResourceNames::PostProcessColor, fbDesc);

        RGResourceDesc entityDesc;
        entityDesc.Kind = RGResourceHandle::Kind::Texture2D;
        entityDesc.Format = RGResourceFormat::R32Int;
        entityDesc.Width = kSize;
        entityDesc.Height = kSize;
        blackboard.Scene.SceneEntityID =
            graph.ImportTextureHandle(ResourceNames::SceneEntityID, entityTexture->GetRHIHandle(), entityDesc);

        // JFA scratch pair — full-res RGBA32F, the production recipe.
        RGResourceDesc jfaDesc;
        jfaDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        jfaDesc.Format = RGResourceFormat::RGBA32Float;
        jfaDesc.Width = kSize;
        jfaDesc.Height = kSize;
        jfaDesc.DebugName = "JFAPing";
        blackboard.Scratch.JFAPing = graph.DeclareTransientFramebuffer("JFAPing", jfaDesc);
        jfaDesc.DebugName = "JFAPong";
        blackboard.Scratch.JFAPong = graph.DeclareTransientFramebuffer("JFAPong", jfaDesc);

        FramebufferSpecification outputSpec;
        outputSpec.Width = kSize;
        outputSpec.Height = kSize;
        outputSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outputSpec);
        if (!outputFramebuffer)
        {
            ADD_FAILURE() << "selection-outline output framebuffer creation failed";
            return rendered;
        }
        blackboard.Post.SelectionOutlineColor =
            graph.DeclareTransientFramebuffer(ResourceNames::SelectionOutlineColor, fbDesc, outputFramebuffer);

        auto outline = Ref<SelectionOutlineRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        outline->Init(initSpec);
        outline->SetEnabled(true);
        outline->SetSelectedEntityIDs(selectedIds);
        outline->SetOutlineColor(glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
        // Width 6 -> smoothstep band inner 0.012 / outer 0.024 UV (1.5-3 px at
        // 128) — inside the default 2-pass JFA's 3-texel propagation reach.
        outline->SetOutlineWidth(6);

        auto producer = Ref<PatternProducerPass>::Create(sceneInput, blitShader);
        graph.AddNode(producer);
        graph.AddNode(outline);
        graph.SetFinalPass("SelectionOutlinePass");
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

        EXPECT_TRUE(producer->DidDraw);
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "selection-outline resolve failure: pass='" << failure.PassName << "' reason='"
                          << failure.Reason << "' x" << failure.Count;
        }
        {
            auto& vkApi = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
            preparedDraws = static_cast<u32>(vkApi.GetPreparedDrawsThisRecording());
            EXPECT_EQ(vkApi.GetDroppedDrawsThisRecording(), 0u);
        }
        targetValid = outline->GetTarget() != nullptr;

        if (targetValid)
        {
            auto* vkOutput = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
            if (!vkOutput->GetColorAttachmentImage(0)->GetData(rendered, 0))
                ADD_FAILURE() << "selection-outline readback failed";
        }
        return rendered;
    };

    // --- selected blob: outline ring straddles the boundary ------------------
    const std::array<i32, 1> selected = { 7 };
    u32 draws = 0;
    bool hasTarget = false;
    const auto ringed = runChain(std::span<const i32>(selected), draws, hasTarget);
    EXPECT_TRUE(hasTarget) << "a live selection must produce the outline target";
    // producer + JFA init + 2 flood iterations + composite.
    EXPECT_EQ(draws, 5u);
    ASSERT_EQ(ringed.size(), static_cast<sizet>(kSize) * kSize * 4);
    const auto px = [&](const std::vector<u8>& img, u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        return std::array<int, 3>{ img[i], img[i + 1], img[i + 2] };
    };
    const auto ringPx = px(ringed, 64, 47); // 1 px outside the blob's top edge (dist 1 px -> alpha 1)
    EXPECT_LE(ringPx[0], 20) << "the ring must be the configured pure green";
    EXPECT_GE(ringPx[1], 235) << "the ring must be the configured pure green";
    EXPECT_LE(ringPx[2], 20) << "the ring must be the configured pure green";
    const auto innerPx = px(ringed, 64, 64); // blob centre: 16 px from every edge, far past the band
    EXPECT_GE(innerPx[0], 85);
    EXPECT_LE(innerPx[0], 95);
    EXPECT_GE(innerPx[1], 85) << "deep inside the blob the scene must pass through (no fill)";
    EXPECT_LE(innerPx[1], 95);
    const auto farPx = px(ringed, 16, 16); // far outside the JFA propagation reach
    EXPECT_GE(farPx[0], 85);
    EXPECT_LE(farPx[1], 95) << "far from the blob the scene must pass through";

    // --- empty selection: the pass idles -------------------------------------
    u32 idleDraws = 0;
    bool idleTarget = true;
    (void)runChain(std::span<const i32>{}, idleDraws, idleTarget);
    EXPECT_FALSE(idleTarget) << "SelectedCount == 0 must report no target (the pass's own contract)";
    EXPECT_EQ(idleDraws, 1u) << "only the producer may draw with an empty selection";

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
}

// =============================================================================
// Cloudscape: three draws at two resolutions — half-res raymarch into
// CloudsRaw, half-res temporal resolve into CloudsResolved (history extract
// declared on it, the TAA sink pattern at half res), full-res depth-aware
// composite — plus the pass-owned noise field: two 3D textures + a 2D
// weather map (Texture3D has no CPU upload path BY DESIGN — production
// generates the field in compute — so the stand-ins are filled with UNIFORM
// values via the facade's transfer clears, which is exactly enough for the
// density math: coverage/type come from the weather map, the base-noise
// remap yields a constant ~0.73 shape, and detail erosion is disabled via
// Field.z = 0).
//
// Contract — the documented FLOOR: executes clean (4 draws, zero stubs,
// zero validation errors), the history extract actually copied (the sink
// valid-flag flips), and clouds render SOMETHING against the sky-blue input
// — the camera looks straight up at the layer, so the composite must differ
// from the input at the zenith and vary across the frame (slant paths + the
// per-pixel IGN jitter make uniformity impossible). A stronger contract
// would need an unjittered, single-step march over the uniform medium so the
// slab path length per pixel becomes closed-form (inscatter/transmittance as
// exact functions of view angle) — that requires a jitter kill-switch the
// shader does not expose today.
// =============================================================================
TEST_F(VulkanPassSuite, CloudscapeRendersCloudsAgainstTheSkyAndExtractsHistory)
{
    constexpr u32 kSize = 128;
    constexpr u32 kHalf = (kSize + 1u) / 2u;
    VulkanFrameArena::Get().BeginFrame(0);

    auto skyInput = MakeSolidTexture(kSize, 90, 140, 220, 255);
    ASSERT_NE(skyInput, nullptr);
    auto depthTexture = MakeSolidTexture(kSize, 255, 255, 255, 255); // depth 1.0 = sky everywhere
    ASSERT_NE(depthTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    // Pass-owned noise field stand-ins. Values chosen against the
    // CloudscapeCommon density math: weather.r 0.86 passes the coverage
    // smoothstep at 1, base noise r 0.78 vs the g/b/a FBM floor 0.20 remaps
    // to a ~0.73 constant shape, zeroed detail + Field.z = 0 disables the
    // erosion term entirely.
    Texture3DSpecification noiseSpec;
    noiseSpec.Width = 8;
    noiseSpec.Height = 8;
    noiseSpec.Depth = 8;
    noiseSpec.Format = Texture3DFormat::RGBA8;
    noiseSpec.Repeat = true;
    auto baseNoise = Texture3D::Create(noiseSpec);
    ASSERT_NE(baseNoise, nullptr);
    auto detailNoise = Texture3D::Create(noiseSpec);
    ASSERT_NE(detailNoise, nullptr);
    // The weather map is the one CPU-uploadable field input, so it carries
    // the spatial structure: vertical coverage stripes alternating between
    // solidly cloudy (230 -> coverage 1 after the smoothstep) and genuinely
    // clear (30, below the 0.28 low edge -> sky gap). A UNIFORM opaque deck
    // is ambient-dominated and angle-independent — legitimately flat — so
    // the non-uniformity floor needs the field itself to vary.
    Ref<Texture2D> weatherMap;
    {
        constexpr u32 kWeatherSize = 4;
        std::vector<u8> weather(static_cast<sizet>(kWeatherSize) * kWeatherSize * 4);
        for (u32 y = 0; y < kWeatherSize; ++y)
        {
            for (u32 x = 0; x < kWeatherSize; ++x)
            {
                const u8 coverage = (x % 2u == 0u) ? 230u : 30u;
                const sizet i = (static_cast<sizet>(y) * kWeatherSize + x) * 4;
                weather[i + 0] = coverage;
                weather[i + 1] = 128u; // type blend
                weather[i + 2] = 0u;   // wetness
                weather[i + 3] = 255u;
            }
        }
        TextureSpecification spec;
        spec.Width = kWeatherSize;
        spec.Height = kWeatherSize;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        weatherMap = Texture2D::Create(spec);
        ASSERT_NE(weatherMap, nullptr);
        weatherMap->SetData(weather.data(), static_cast<u32>(weather.size()));
    }

    // Camera looking straight UP (+Y) from (4000, 0, 4000): every ray pierces
    // the [1500, 4000] m layer, and the xz footprint at cloud altitude lands
    // INSIDE the positive weather-map extent (uv ~0.2..0.8 at 1/8000 map
    // scale — the embedded sampler clamps, so a footprint straddling uv 0
    // would flatten the left half onto one texel column).
    const glm::vec3 kEye(4000.0f, 0.0f, 4000.0f);
    const glm::mat4 projection = glm::perspectiveRH_NO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    const glm::mat4 view = glm::lookAtRH(kEye, kEye + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::mat4 viewProjection = projection * view;

    UBOStructures::CameraUBO cameraData{};
    cameraData.ViewProjection = viewProjection;
    cameraData.View = view;
    cameraData.Projection = projection;
    cameraData.Position = kEye;
    cameraData.PrevViewProjection = viewProjection;
    cameraData.RenderOrigin = glm::vec3(0.0f);
    auto cameraUbo = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, UBOStructures::CameraUBO::GetSize());

    MotionBlurUBOData motionData{};
    motionData.InverseViewProjection = glm::inverse(viewProjection);
    motionData.PrevViewProjection = viewProjection;
    auto motionUbo = UniformBuffer::Create(sizeof(MotionBlurUBOData), 8);
    motionUbo->SetData(&motionData, sizeof(motionData));

    auto cloudscape = Ref<CloudscapeRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    cloudscape->Init(initSpec);
    cloudscape->SetEnabled(true);
    cloudscape->SetCameraUBO(cameraUbo);
    cloudscape->SetNoiseTextures(baseNoise->GetRHIHandle(), detailNoise->GetRHIHandle(),
                                 weatherMap->GetRHIHandle());
    cloudscape->SetHistory({}, false); // first frame: no history, Misc.x forced 0

    UBOStructures::CloudscapeUBO cloudData{};
    cloudData.Layer = glm::vec4(1500.0f, 4000.0f, 1.0f / 2500.0f, 1.0f);
    cloudData.Field = glm::vec4(0.9f, 0.5f, 0.0f, 0.0f); // dense coverage, NO detail erosion
    cloudData.Wind = glm::vec4(0.0f);
    // Map extent 8000 m: the camera's cloud-altitude footprint spans ~2.5
    // weather texels, so the stripes above land across the frame.
    cloudData.Map = glm::vec4(1.0f / 8000.0f, 32.0f, 4.0f, 0.6f);
    cloudData.Light = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f);
    cloudData.Misc = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // blend 0, enabled 1
    cloudData.SunDirection = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    cloudData.SunColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    cloudData.Ambient = glm::vec4(0.4f, 0.5f, 0.7f, 0.0f);
    cloudscape->SetUBOData(cloudData);
    cloudscape->UploadAndBindUBO(); // production uploads pre-graph (UploadExecutionState)

    // Half-res history sink target (the CloudsResolved copy destination).
    TextureSpecification historySpec;
    historySpec.Width = kHalf;
    historySpec.Height = kHalf;
    historySpec.Format = ImageFormat::RGBA16F;
    historySpec.GenerateMips = false;
    auto historyTexture = Texture2D::Create(historySpec);
    ASSERT_NE(historyTexture, nullptr);
    bool historyValid = false;

    RenderGraph graph;
    graph.SetTransientMaterializationEnabled(true);
    auto& blackboard = graph.GetBlackboard();

    RGResourceDesc fbDesc;
    fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    fbDesc.Format = RGResourceFormat::RGBA8UNorm;
    fbDesc.Width = kSize;
    fbDesc.Height = kSize;
    blackboard.Post.PostProcessColor =
        graph.DeclareTransientFramebuffer(ResourceNames::PostProcessColor, fbDesc);

    RGResourceDesc importDesc;
    importDesc.Kind = RGResourceHandle::Kind::Texture2D;
    importDesc.Format = RGResourceFormat::RGBA8UNorm;
    importDesc.Width = kSize;
    importDesc.Height = kSize;
    blackboard.Scene.SceneDepth =
        graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), importDesc);

    RGResourceDesc halfDesc;
    halfDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    halfDesc.Format = RGResourceFormat::RGBA16Float;
    halfDesc.Width = kHalf;
    halfDesc.Height = kHalf;
    halfDesc.DebugName = std::string(ResourceNames::CloudsRaw);
    blackboard.Scratch.CloudsRaw = graph.DeclareTransientFramebuffer(ResourceNames::CloudsRaw, halfDesc);
    halfDesc.DebugName = std::string(ResourceNames::CloudsResolved);
    blackboard.Scratch.CloudsResolved = graph.DeclareTransientFramebuffer(ResourceNames::CloudsResolved, halfDesc);

    graph.RegisterHistoryTextureSink(ResourceNames::CloudsHistory, historyTexture->GetRHIHandle(), kHalf, kHalf,
                                     &historyValid);

    FramebufferSpecification outputSpec;
    outputSpec.Width = kSize;
    outputSpec.Height = kSize;
    outputSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outputSpec);
    ASSERT_TRUE(outputFramebuffer);
    blackboard.Post.CloudsColor =
        graph.DeclareTransientFramebuffer(ResourceNames::CloudsColor, fbDesc, outputFramebuffer);

    auto producer = Ref<PatternProducerPass>::Create(skyInput, blitShader);
    graph.AddNode(producer);
    graph.AddNode(cloudscape);
    graph.SetFinalPass("CloudscapePass");
    graph.BuildFrameGraph();

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    SubmitFrame(
        [&]()
        {
            // Fill the noise stand-ins with their uniform values through the
            // facade's transfer clears (exact per-layout-run transitions),
            // then move each to the sampled layout its descriptor bakes.
            RenderCommand::ClearTextureFloat(baseNoise->GetRHIHandle(), 0u,
                                             glm::vec4(0.78f, 0.20f, 0.20f, 0.20f));
            RenderCommand::ClearTextureFloat(detailNoise->GetRHIHandle(), 0u, glm::vec4(0.0f));
            for (const RHI::ResourceHandle volume : { baseNoise->GetRHIHandle(), detailNoise->GetRHIHandle() })
            {
                RHI::Barrier toSampled{};
                toSampled.Resource = volume;
                toSampled.Before = RHI::Access::TransferWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            }

            graph.Execute();

            RHI::Barrier toSampled{};
            toSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(0);
            toSampled.Before = RHI::Access::ColorAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    EXPECT_TRUE(producer->DidDraw);
    EXPECT_TRUE(cloudscape->GetTarget()) << "cloudscape Execute early-returned (input/scratch/depth guard)";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "cloudscape resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                      << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 4u) << "producer + raymarch + resolve + composite";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);
    EXPECT_TRUE(historyValid) << "the CloudsResolved -> CloudsHistory extract must have copied (sink flag)";

    std::vector<u8> rendered;
    auto* vkOutput = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
    ASSERT_TRUE(vkOutput->GetColorAttachmentImage(0)->GetData(rendered, 0));
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

    // Clouds must have changed the zenith away from the sky-blue input...
    const auto px = [&](u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        return std::array<int, 3>{ rendered[i], rendered[i + 1], rendered[i + 2] };
    };
    const auto centre = px(64, 64);
    const int centreDelta =
        std::abs(centre[0] - 90) + std::abs(centre[1] - 140) + std::abs(centre[2] - 220);
    EXPECT_GE(centreDelta, 30) << "an opaque deck straight overhead must not pass the sky through";
    // ...and the frame must not be UNIFORM: the striped weather field must
    // put both cloud deck AND clear-sky gaps on screen (observed: a
    // saturated deck at ~255 with sky-blue stripes — scan the whole frame,
    // a sparse probe grid can sit entirely on the deck).
    int minR = 255;
    int maxR = 0;
    for (sizet i = 0; i < rendered.size(); i += 4)
    {
        const int r = rendered[i];
        minR = std::min(minR, r);
        maxR = std::max(maxR, r);
    }
    EXPECT_GE(maxR - minR, 30)
        << "the striped weather coverage must render cloud deck AND clear-sky gaps (uniform veil = dead "
           "density/weather path)";

    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
}

// =============================================================================
// Fog: two draws — half-res evaluation into FogHalfRes (AllowSamePassReadWrite
// write-then-sample) + full-res bilateral upsample/composite — on the
// ANALYTIC path, with the froxel fallback pinned the production way: a real
// VolumetricFogPass is attached via SetVolumetricFogPass but never added to
// the graph, so FogRenderPass::Execute sees RanThisFrame() == false and
// calls UploadDisabledUBO() (FroxelFogData@46 zeroed, enabled 0). The
// DependsOnPass("VolumetricFogPass") edge dangles harmlessly (no such node —
// tryAddDerivedDependency simply adds no edge).
//
// Contract, two chains over a mid-gray input with uniform far-plane depth:
// density 0.05 EXPONENTIAL red fog fully fogs the far field (centre pixel ~
// fogColor: factor 1 - exp(-5) = 0.993), and density 0 is an exact
// passthrough (transmittance 1, inscatter 0) — so a fog that never applies,
// or always applies, fails one side.
// =============================================================================
TEST_F(VulkanPassSuite, FogFogsTheFarFieldAnalyticallyAndPassesThroughAtZeroDensity)
{
    constexpr u32 kSize = 128;
    constexpr u32 kHalf = (kSize + 1u) / 2u;
    VulkanFrameArena::Get().BeginFrame(0);

    auto grayInput = MakeSolidTexture(kSize, 128, 128, 128, 255);
    ASSERT_NE(grayInput, nullptr);
    auto depthTexture = MakeSolidTexture(kSize, 255, 255, 255, 255); // far plane everywhere
    ASSERT_NE(depthTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    // Identity view at the origin; both fog shaders read the full 288-byte
    // CameraMatrices block, the depth reconstruction reads MotionBlurUBO@8.
    const glm::mat4 projection = glm::perspectiveRH_NO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    UBOStructures::CameraUBO cameraData{};
    cameraData.ViewProjection = projection;
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = projection;
    cameraData.Position = glm::vec3(0.0f);
    cameraData.PrevViewProjection = projection;
    cameraData.RenderOrigin = glm::vec3(0.0f);
    auto cameraUbo = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, UBOStructures::CameraUBO::GetSize());
    MotionBlurUBOData motionData{};
    motionData.InverseViewProjection = glm::inverse(projection);
    motionData.PrevViewProjection = projection;
    auto motionUbo = UniformBuffer::Create(sizeof(MotionBlurUBOData), 8);
    motionUbo->SetData(&motionData, sizeof(motionData));
    auto postProcessUbo = UniformBuffer::Create(sizeof(PostProcessUBOData), 7);
    PostProcessUBOData postData{};
    postProcessUbo->SetData(&postData, sizeof(postData));
    FogVolumesUBOData fogVolumes{}; // count 0 — no local volumes
    auto fogVolumesUbo = UniformBuffer::Create(sizeof(FogVolumesUBOData), ShaderBindingLayout::UBO_FOG_VOLUMES);
    fogVolumesUbo->SetData(&fogVolumes, sizeof(fogVolumes));
    auto fogUbo = UniformBuffer::Create(sizeof(FogUBOData), ShaderBindingLayout::UBO_FOG);

    // The froxel provider, Init'd but never graphed: RanThisFrame() stays
    // false, so the fog pass exercises the UploadDisabledUBO fallback.
    auto froxel = Ref<VolumetricFogPass>::Create();
    {
        FramebufferSpecification froxelSpec;
        froxelSpec.Width = kSize;
        froxelSpec.Height = kSize;
        froxel->Init(froxelSpec);
    }

    // The fog shader still DECLARES the froxel sampler3D@53; the volumetric
    // branch is uniform-false so it is never accessed, but the fixture keeps
    // every declared binding fed with a real (1-texel) 3D view.
    Texture3DSpecification tinySpec;
    tinySpec.Width = 1;
    tinySpec.Height = 1;
    tinySpec.Depth = 1;
    tinySpec.Format = Texture3DFormat::RGBA16F;
    auto tinyVolume = Texture3D::Create(tinySpec);
    ASSERT_NE(tinyVolume, nullptr);
    bool tinyVolumeSeeded = false;

    const auto runChain = [&](f32 density) -> std::vector<u8>
    {
        std::vector<u8> rendered;

        FogUBOData fogData{};
        fogData.ColorAndDensity = glm::vec4(1.0f, 0.0f, 0.0f, density); // red fog
        fogData.DistanceParams = glm::vec4(0.0f, 100.0f, 0.0f, 0.0f);   // heightFalloff 0
        fogData.ScatterParams = glm::vec4(0.0f);
        fogData.RayleighColorAndMaxOpacity = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        fogData.SunDirection = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
        fogData.Flags = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); // enabled, EXPONENTIAL, no scatter, no volumetric
        fogData.NoiseParams = glm::vec4(0.0f);
        fogData.VolumetricParams = glm::vec4(0.0f);
        fogUbo->SetData(&fogData, sizeof(fogData));

        auto fog = Ref<FogRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        fog->Init(initSpec);
        fog->SetEnabled(true);
        fog->SetPostProcessUBO(postProcessUbo);
        fog->SetCameraUBO(cameraUbo);
        fog->SetVolumetricFogPass(froxel);

        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        auto& blackboard = graph.GetBlackboard();

        RGResourceDesc fbDesc;
        fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        fbDesc.Format = RGResourceFormat::RGBA8UNorm;
        fbDesc.Width = kSize;
        fbDesc.Height = kSize;
        blackboard.Post.PostProcessColor =
            graph.DeclareTransientFramebuffer(ResourceNames::PostProcessColor, fbDesc);

        RGResourceDesc importDesc;
        importDesc.Kind = RGResourceHandle::Kind::Texture2D;
        importDesc.Format = RGResourceFormat::RGBA8UNorm;
        importDesc.Width = kSize;
        importDesc.Height = kSize;
        blackboard.Scene.SceneDepth =
            graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), importDesc);

        RGResourceDesc halfDesc;
        halfDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        halfDesc.Format = RGResourceFormat::RGBA16Float;
        halfDesc.Width = kHalf;
        halfDesc.Height = kHalf;
        halfDesc.DebugName = std::string(ResourceNames::FogHalfRes);
        blackboard.Scratch.FogHalfRes = graph.DeclareTransientFramebuffer(ResourceNames::FogHalfRes, halfDesc);

        FramebufferSpecification outputSpec;
        outputSpec.Width = kSize;
        outputSpec.Height = kSize;
        outputSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outputSpec);
        if (!outputFramebuffer)
        {
            ADD_FAILURE() << "fog output framebuffer creation failed";
            return rendered;
        }
        blackboard.Post.FogColor =
            graph.DeclareTransientFramebuffer(ResourceNames::FogColor, fbDesc, outputFramebuffer);

        auto producer = Ref<PatternProducerPass>::Create(grayInput, blitShader);
        graph.AddNode(producer);
        graph.AddNode(fog);
        graph.SetFinalPass("FogPass");
        graph.BuildFrameGraph();

        auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
        SubmitFrame(
            [&]()
            {
                if (!tinyVolumeSeeded)
                {
                    // One-time: give the never-sampled sampler3D@53 a real 3D
                    // view in its descriptor-baked layout.
                    RenderCommand::ClearTextureFloat(tinyVolume->GetRHIHandle(), 0u, glm::vec4(0.0f));
                    RHI::Barrier seeded{};
                    seeded.Resource = tinyVolume->GetRHIHandle();
                    seeded.Before = RHI::Access::TransferWrite;
                    seeded.After = RHI::Access::ShaderSampleRead;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &seeded, 1 });
                    tinyVolumeSeeded = true;
                }
                RenderCommand::BindTexture(ShaderBindingLayout::TEX_FROXEL_FOG, tinyVolume->GetRHIHandle());

                graph.Execute();

                RHI::Barrier toSampled{};
                toSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(0);
                toSampled.Before = RHI::Access::ColorAttachmentWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            });

        EXPECT_TRUE(producer->DidDraw);
        EXPECT_TRUE(fog->GetTarget()) << "fog Execute early-returned (input/output/halfres/depth guard)";
        EXPECT_FALSE(froxel->RanThisFrame()) << "the un-graphed froxel chain must not have dispatched";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "fog resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                          << "' x" << failure.Count;
        }
        EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 3u) << "producer + half-res fog + upsample composite";
        EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

        auto* vkOutput = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
        if (!vkOutput->GetColorAttachmentImage(0)->GetData(rendered, 0))
            ADD_FAILURE() << "fog output readback failed";
        return rendered;
    };

    // Density 0.05 over ~100 view units: factor 0.993 — red fog everywhere.
    const auto fogged = runChain(0.05f);
    ASSERT_EQ(fogged.size(), static_cast<sizet>(kSize) * kSize * 4);
    for (const auto& [x, y] : { std::pair<u32, u32>{ 64, 64 }, { 8, 8 }, { 120, 120 } })
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        EXPECT_GE(static_cast<int>(fogged[i]), 245) << "far-field pixels must carry the red fog colour at ("
                                                    << x << "," << y << ")";
        EXPECT_LE(static_cast<int>(fogged[i + 1]), 8) << "the gray scene must be almost fully extinguished";
    }

    // Density 0: transmittance 1, inscatter 0 — exact passthrough.
    const auto clear = runChain(0.0f);
    ASSERT_EQ(clear.size(), static_cast<sizet>(kSize) * kSize * 4);
    u32 maxDiff = 0;
    for (sizet i = 0; i < clear.size(); i += 4)
    {
        for (sizet c = 0; c < 3; ++c)
        {
            maxDiff = std::max(maxDiff, static_cast<u32>(std::abs(static_cast<int>(clear[i + c]) - 128)));
        }
    }
    EXPECT_LE(maxDiff, 2u) << "zero density must pass the scene through the two-draw chain untouched";

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
}

// =============================================================================
// TAA: the history pass — two frames through the RunSinglePassChain harness.
// Frame 1 declares builder.ExtractHistoryTexture(TAAHistory, output): the
// graph's FlushExtractions copies the frame's output into the fixture-owned
// history texture (RegisterHistoryTextureSink — CopyImageSubData on this
// backend) and flips the sink's valid flag; frame 1's own history bind falls
// back to the current input (the pass's documented history=current path).
// Frame 2 imports the extracted history (ImportHistoryHandle) — the copy
// left it in a TRANSFER layout, so the sample rides the backend's GL-parity
// bind-time transition seam — and the blend consumes REAL prior content.
//
// Contract on a CHECKERBOARD (not a hard-edge pattern: the variance clamp
// legitimately clips a pixel's own history at a strong edge, so "identical
// frames == identity" only holds where the neighborhood box contains the
// value — a checkerboard's 3x3 box always spans both values):
//   frame 1 (zero velocity, history == current): output == input exactly.
//   frame 2 over the INVERTED checkerboard: every pixel must land at
//   mix(current, history, feedback) = 0.1*current + 0.9*history — far from
//   the no-history result (current), so a dead import, a skipped extract, or
//   a mis-reprojection all fail by ~52 bytes.
// =============================================================================
TEST_F(VulkanPassSuite, TaaResolvesIdentityAndBlendsTheImportedHistory)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr u8 kLow = 96;
    constexpr u8 kHigh = 160;
    const auto makeChecker = [&](u32 flip) -> Ref<Texture2D>
    {
        std::vector<u8> pixels(static_cast<sizet>(kSize) * kSize * 4);
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const u8 v = (((x + y) & 1u) == flip) ? kHigh : kLow;
                const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
                pixels[i + 0] = v;
                pixels[i + 1] = v;
                pixels[i + 2] = v;
                pixels[i + 3] = 255;
            }
        }
        TextureSpecification spec;
        spec.Width = kSize;
        spec.Height = kSize;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        auto texture = Texture2D::Create(spec);
        if (texture)
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
        return texture;
    };
    auto checkerA = makeChecker(1u); // odd parity high
    ASSERT_NE(checkerA, nullptr);
    auto checkerB = makeChecker(0u); // even parity high — A inverted
    ASSERT_NE(checkerB, nullptr);

    auto depthTexture = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(depthTexture, nullptr);
    auto velocityTexture = MakeSolidTexture(kSize, 0, 0, 0, 255); // zero motion
    ASSERT_NE(velocityTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    MotionBlurUBOData motionData{}; // identity — unused on the velocity path
    auto motionUbo = UniformBuffer::Create(sizeof(MotionBlurUBOData), 8);
    motionUbo->SetData(&motionData, sizeof(motionData));

    // Fixture-owned history storage + validity, the RenderPipeline shape.
    auto historyTexture = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(historyTexture, nullptr);
    bool historyValid = false;

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
            graph.ImportTextureHandle(ResourceNames::Velocity, velocityTexture->GetRHIHandle(), auxDesc);
        graph.RegisterHistoryTextureSink(ResourceNames::TAAHistory, historyTexture->GetRHIHandle(), kSize, kSize,
                                         &historyValid);
        if (historyValid)
        {
            blackboard.Temporal.TAAHistory =
                graph.ImportHistoryHandle(ResourceNames::TAAHistory, historyTexture->GetRHIHandle());
        }
    };
    // The history extract READS the output attachment last — the readback
    // barrier's source scope must name the copy, not the raster.
    m_OutputBarrierBefore = RHI::Access::TransferRead;

    const auto runFrame = [&](const Ref<Texture2D>& input) -> std::vector<u8>
    {
        auto taa = Ref<TAARenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        taa->Init(initSpec);
        taa->SetEnabled(true);
        PostProcessSettings settings{};
        settings.TAAFeedback = 0.9f;
        settings.TAASharpness = 0.0f; // the exact-arithmetic contract wants no unsharp term
        taa->SetSettings(settings);

        auto producer = Ref<PatternProducerPass>::Create(input, blitShader);
        return RunSinglePassChain(kSize, producer, taa, "TAAPass", ResourceNames::TAAColor,
                                  [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                  { blackboard.Post.TAAColor = handle; });
    };

    const auto valueAt = [&](u32 x, u32 y, u32 flip) -> int
    { return (((x + y) & 1u) == flip) ? kHigh : kLow; };

    // Frame 1: history invalid -> the pass's history=current fallback makes
    // the resolve an exact identity; the extract fills the history sink.
    const auto frame1 = runFrame(checkerA);
    ASSERT_EQ(frame1.size(), static_cast<sizet>(kSize) * kSize * 4);
    {
        u32 maxDiff = 0;
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const int expected = valueAt(x, y, 1u);
                const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
                maxDiff = std::max(maxDiff,
                                   static_cast<u32>(std::abs(static_cast<int>(frame1[i]) - expected)));
            }
        }
        EXPECT_LE(maxDiff, 2u) << "frame 1: zero velocity + history==current must resolve to the input";
    }
    EXPECT_TRUE(historyValid) << "frame 1 must have extracted TAAColor into the history sink";

    // Frame 2 over the INVERTED checkerboard: current and history disagree at
    // every pixel, both inside the neighborhood clamp box, so the output is
    // the pure feedback blend: 0.1*current + 0.9*history.
    const auto frame2 = runFrame(checkerB);
    ASSERT_EQ(frame2.size(), static_cast<sizet>(kSize) * kSize * 4);
    {
        u32 maxDiff = 0;
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const f32 current = static_cast<f32>(valueAt(x, y, 0u));
                const f32 history = static_cast<f32>(valueAt(x, y, 1u));
                const int expected = static_cast<int>(std::lround(0.1f * current + 0.9f * history));
                const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
                maxDiff = std::max(maxDiff,
                                   static_cast<u32>(std::abs(static_cast<int>(frame2[i]) - expected)));
            }
        }
        EXPECT_LE(maxDiff, 3u) << "frame 2 must blend the IMPORTED history at the feedback weight "
                                  "(current alone here means the extract or the import never happened)";
    }
    EXPECT_TRUE(historyValid) << "frame 2 must re-extract for the frame after it";

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
    m_ExtraSetup = nullptr;
    m_OutputBarrierBefore = RHI::Access::ColorAttachmentWrite;
}

// =============================================================================
// OITResolve: the in-place read-modify-write pass — it renames SceneColor
// (WriteNewVersion) and composites INTO the framebuffer the producer just
// drew, with per-attachment blend: attachment 0 enabled via
// SetBlendStateForAttachment while the FACTORS come from the global
// SetBlendFunc(OneMinusSrcAlpha, SrcAlpha) — the exact GL glEnablei-vs-
// glBlendFunci split whose Vulkan lowering this tenant fixed (the recorded
// state previously diverted the factors to the never-written per-attachment
// array, i.e. Zero/Zero, whenever the per-attachment ENABLE was set).
//
// Contract over a blue background with an imported half-red accum and a
// split revealage: rows with revealage 0.251 must land at the EXACT
// weighted-blended composite (avg*(1-r) + bg*r), and rows with revealage 1.0
// take the shader's discard path — background untouched, byte-exact.
// =============================================================================
TEST_F(VulkanPassSuite, OitResolveCompositesAccumOverTheSceneByRevealage)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto backgroundInput = MakeSolidTexture(kSize, 0, 0, 200, 255);
    ASSERT_NE(backgroundInput, nullptr);
    // Accum: sum(Ci*ai*wi) = (0.502, 0, 0), sum(ai*wi) = 1 -> average (0.502, 0, 0).
    auto accumTexture = MakeSolidTexture(kSize, 128, 0, 0, 255);
    ASSERT_NE(accumTexture, nullptr);
    // Revealage .r: rows < 64 keep 0.251 of the background; rows >= 64 are
    // fully revealed (1.0) and must DISCARD.
    auto revealageTexture = MakeHorizontalSplitTexture(kSize, 64, { 64, 64, 64, 255 }, { 255, 255, 255, 255 });
    ASSERT_NE(revealageTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    RenderGraph graph;
    graph.SetTransientMaterializationEnabled(true);
    auto& blackboard = graph.GetBlackboard();

    RGResourceDesc fbDesc;
    fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    fbDesc.Format = RGResourceFormat::RGBA8UNorm;
    fbDesc.Width = kSize;
    fbDesc.Height = kSize;
    // The RMW target: caller-backed SceneColor — the producer writes it, the
    // resolve composites into the SAME physical through its renamed version.
    FramebufferSpecification outputSpec;
    outputSpec.Width = kSize;
    outputSpec.Height = kSize;
    outputSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(outputSpec);
    ASSERT_TRUE(sceneFramebuffer);
    blackboard.Scene.SceneColor =
        graph.DeclareTransientFramebuffer(ResourceNames::SceneColor, fbDesc, sceneFramebuffer);

    RGResourceDesc importDesc;
    importDesc.Kind = RGResourceHandle::Kind::Texture2D;
    importDesc.Format = RGResourceFormat::RGBA8UNorm;
    importDesc.Width = kSize;
    importDesc.Height = kSize;
    blackboard.OIT.OITAccum =
        graph.ImportTextureHandle(ResourceNames::OITAccum, accumTexture->GetRHIHandle(), importDesc);
    blackboard.OIT.OITRevealage =
        graph.ImportTextureHandle(ResourceNames::OITRevealage, revealageTexture->GetRHIHandle(), importDesc);

    auto resolve = Ref<OITResolveRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    resolve->Init(initSpec);
    resolve->SetEnabled(true);
    resolve->SetHasContributors(true); // the frame-gate a contributor pass normally sets

    auto producer = Ref<PatternProducerPass>::Create(
        backgroundInput, blitShader, std::string(ResourceNames::SceneColor),
        std::string(ResourceNames::SceneColorTexture),
        [](FrameBlackboard& board) -> RGFramebufferHandle&
        { return board.Scene.SceneColor; });
    graph.AddNode(producer);
    graph.AddNode(resolve);
    graph.SetFinalPass("OITResolvePass");
    graph.BuildFrameGraph();

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    SubmitFrame(
        [&]()
        {
            graph.Execute();

            RHI::Barrier toSampled{};
            toSampled.Resource = sceneFramebuffer->GetColorAttachmentHandle(0);
            toSampled.Before = RHI::Access::ColorAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    EXPECT_TRUE(producer->DidDraw);
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "OIT resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                      << "' x" << failure.Count;
    }
    // (OITResolveRenderPass never publishes m_Target — GetTarget() is not a
    // seam here; the composite READBACK is the execute proof.)
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 2u) << "producer + one resolve draw";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    std::vector<u8> rendered;
    auto* vkOutput = static_cast<VulkanFramebuffer*>(sceneFramebuffer.Raw());
    ASSERT_TRUE(vkOutput->GetColorAttachmentImage(0)->GetData(rendered, 0));
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

    const auto px = [&](u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        return std::array<int, 3>{ rendered[i], rendered[i + 1], rendered[i + 2] };
    };
    // Blended band: dst' = avg*(1 - r) + dst*r with avg = (0.502, 0, 0),
    // r = 64/255 = 0.251, dst = (0, 0, 0.784):
    //   R = 0.502 * 0.749          = 0.376 -> 96
    //   B = 0.784 * 0.251          = 0.197 -> 50
    const auto blended = px(64, 32);
    EXPECT_NEAR(blended[0], 96, 3) << "the accum's average colour must blend in by (1 - revealage)";
    EXPECT_LE(blended[1], 3);
    EXPECT_NEAR(blended[2], 50, 3) << "the background must survive by exactly revealage";
    // Discard band: revealage 1.0 -> the shader discards, background exact.
    const auto discarded = px(64, 96);
    EXPECT_EQ(discarded[0], 0) << "full revealage must take the discard path (background untouched)";
    EXPECT_EQ(discarded[1], 0);
    EXPECT_NEAR(discarded[2], 200, 1);

    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
}

// =============================================================================
// DepthVelocityUpscale: the suite's first MRT tenant — one draw with
// SetDrawBuffers({0, 1}) into a two-attachment backed framebuffer, RT0 the
// nearest-upscaled depth, RT1 the nearest-upscaled velocity. No producer:
// both inputs are imported reduced-resolution textures.
//
// ENGINE GAP (reported, not fixed here): the production declaration asks for
// an R32Float colour attachment, but FramebufferTextureFormat has NO
// single-channel float colour member at all — RenderGraph::ToFramebufferFormat
// maps RGResourceFormat::R32Float to None, so the POOLED materialization path
// silently DROPS production's RT0 (UpscaledDepthVelocity becomes a
// one-attachment FB and the RT1 attachment view dangles). This tenant backs
// the FB with RG32F for RT0 instead (float depth in .r, exact) and mirrors
// production's {R32Float, RG16Float} in the DECLARED desc, which caller
// backing renders inert.
//
// Contract: a 64x64 two-band depth/velocity pair lands in the 128x128 output
// with EXACT per-texel values and a hard band edge at the doubled row — the
// nearest tap's defining property (a bilinear upsample would invent
// intermediate depths across the seam, the exact artefact #480 exists to
// prevent).
// =============================================================================
TEST_F(VulkanPassSuite, DepthVelocityUpscaleNearestUpsamplesExactValues)
{
    constexpr u32 kSize = 128;
    constexpr u32 kReduced = kSize / 2;
    VulkanFrameArena::Get().BeginFrame(0);

    // Reduced-res stand-ins, split at reduced row 32 (output row 64).
    auto depthTexture = MakeHorizontalSplitTexture(kReduced, 32, { 200, 200, 200, 255 }, { 50, 50, 50, 255 });
    ASSERT_NE(depthTexture, nullptr);
    auto velocityTexture = MakeHorizontalSplitTexture(kReduced, 32, { 10, 240, 0, 255 }, { 80, 30, 0, 255 });
    ASSERT_NE(velocityTexture, nullptr);

    RenderGraph graph;
    graph.SetTransientMaterializationEnabled(true);
    auto& blackboard = graph.GetBlackboard();

    RGResourceDesc importDesc;
    importDesc.Kind = RGResourceHandle::Kind::Texture2D;
    importDesc.Format = RGResourceFormat::RGBA8UNorm;
    importDesc.Width = kReduced;
    importDesc.Height = kReduced;
    blackboard.Scene.SceneDepth =
        graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), importDesc);
    blackboard.GBuffer.Velocity =
        graph.ImportTextureHandle(ResourceNames::Velocity, velocityTexture->GetRHIHandle(), importDesc);

    FramebufferSpecification outputSpec;
    outputSpec.Width = kSize;
    outputSpec.Height = kSize;
    // RG32F stands in for the unrepresentable R32F (see the header comment);
    // only .r is asserted. RT1 is the production RG16F.
    outputSpec.Attachments = { FramebufferTextureFormat::RG32F, FramebufferTextureFormat::RG16F };
    Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outputSpec);
    ASSERT_TRUE(outputFramebuffer);

    RGResourceDesc dvDesc;
    dvDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    dvDesc.Width = kSize;
    dvDesc.Height = kSize;
    dvDesc.Attachments = { RGResourceFormat::R32Float, RGResourceFormat::RG16Float }; // production mirror
    dvDesc.DebugName = std::string(ResourceNames::UpscaledDepthVelocity);
    blackboard.Post.UpscaledDepthVelocity =
        graph.DeclareTransientFramebuffer(ResourceNames::UpscaledDepthVelocity, dvDesc, outputFramebuffer);

    auto upscale = Ref<DepthVelocityUpscalePass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    upscale->Init(initSpec);
    upscale->SetEnabled(true);
    upscale->SetRenderScale(0.5f);

    graph.AddNode(upscale);
    graph.SetFinalPass("DepthVelocityUpscalePass");
    graph.BuildFrameGraph();

    // Setup must have published the full-res views for the post band.
    EXPECT_TRUE(blackboard.Post.UpscaledSceneDepthTexture.IsValid());
    EXPECT_TRUE(blackboard.Post.UpscaledVelocityTexture.IsValid());

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    SubmitFrame(
        [&]()
        {
            graph.Execute();

            for (u32 attachment = 0; attachment < 2u; ++attachment)
            {
                RHI::Barrier toSampled{};
                toSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(attachment);
                toSampled.Before = RHI::Access::ColorAttachmentWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            }
        });

    EXPECT_TRUE(upscale->GetTarget()) << "DepthVelocityUpscale early-returned (input/output guard)";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "upscale resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                      << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 1u) << "one MRT2 fullscreen draw, no producer";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    auto* vkOutput = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
    std::vector<u8> depthBytes;
    ASSERT_TRUE(vkOutput->GetColorAttachmentImage(0)->GetData(depthBytes, 0));
    ASSERT_EQ(depthBytes.size(), static_cast<sizet>(kSize) * kSize * 8); // RG32F
    std::vector<u8> velocityBytes;
    ASSERT_TRUE(vkOutput->GetColorAttachmentImage(1)->GetData(velocityBytes, 0));
    ASSERT_EQ(velocityBytes.size(), static_cast<sizet>(kSize) * kSize * 4); // RG16F

    const auto depthAt = [&](u32 x, u32 y) -> f32
    {
        const auto* floats = reinterpret_cast<const f32*>(depthBytes.data());
        return floats[(static_cast<sizet>(y) * kSize + x) * 2];
    };
    const auto velocityAt = [&](u32 x, u32 y) -> glm::vec2
    {
        const auto* halves = reinterpret_cast<const u16*>(velocityBytes.data());
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 2;
        return { HalfToFloat(halves[i]), HalfToFloat(halves[i + 1]) };
    };

    constexpr f32 kNear = 200.0f / 255.0f;
    constexpr f32 kFar = 50.0f / 255.0f;
    // Exact values inside each band (UNORM8 decode and the nearest tap are
    // both exactly-rounded; RT0 is f32 end to end).
    EXPECT_NEAR(depthAt(20, 20), kNear, 1e-6f);
    EXPECT_NEAR(depthAt(100, 100), kFar, 1e-6f);
    // The band edge doubles EXACTLY: output row 63 still reads reduced row 31,
    // row 64 reads reduced row 32 — no invented intermediate value.
    EXPECT_NEAR(depthAt(64, 63), kNear, 1e-6f) << "the nearest tap must not blend across the seam";
    EXPECT_NEAR(depthAt(64, 64), kFar, 1e-6f) << "the nearest tap must not blend across the seam";
    // Velocity rides RT1 through the same tap (f16 storage tolerance).
    const glm::vec2 topVelocity = velocityAt(20, 20);
    EXPECT_NEAR(topVelocity.x, 10.0f / 255.0f, 2e-3f);
    EXPECT_NEAR(topVelocity.y, 240.0f / 255.0f, 2e-3f);
    const glm::vec2 bottomVelocity = velocityAt(100, 100);
    EXPECT_NEAR(bottomVelocity.x, 80.0f / 255.0f, 2e-3f);
    EXPECT_NEAR(bottomVelocity.y, 30.0f / 255.0f, 2e-3f);

    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
}

// =============================================================================
// SSR: the pass-owned min-HZB is (re)built by COMPUTE inside Execute
// (HZBGenerator::Generate — dispatches + its own barriers, the Wave B
// HZBParams@59 per-dispatch route), then one fullscreen draw consumes five
// samplers (scene colour, depth, G-Buffer normal + albedo, the HZB) under
// SSRParams@38.
//
// Contract — the documented FLOOR: Intensity 0 makes the final blend factor
// exactly 0, so the output must equal the input byte-for-byte WHILE the full
// machinery still runs (non-sky depth + low roughness keep every early-out
// closed: the HiZ march, the binary search and the HZB compute chain all
// execute — zero stubs / zero validation errors is the proof). The
// mirror-plane contract (a known emitter reflected onto a known receiver)
// needs oct-encoded tilted normals and a two-wall depth field whose reflected
// rays land on-screen — deferred to the wave's hardening slice; this floor
// pins the machinery and the intensity lever's OFF side.
// =============================================================================
TEST_F(VulkanPassSuite, SsrPassesThroughAtZeroIntensityWithTheHzbChainLive)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

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

    auto depthTexture = MakeSolidTexture(kSize, 128, 128, 128, 255); // mid depth — NOT sky
    ASSERT_NE(depthTexture, nullptr);
    auto normalTexture = MakeSolidTexture(kSize, 0, 0, 0, 255); // oct(0,0) -> +Z, roughness 0
    ASSERT_NE(normalTexture, nullptr);
    auto albedoTexture = MakeSolidTexture(kSize, 200, 200, 200, 0); // metallic 0
    ASSERT_NE(albedoTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

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

    auto ssr = Ref<SSRRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    ssr->Init(initSpec); // compiles PostProcess_SSR + brings up the min-HZB generator
    ssr->SetEnabled(true);

    SSRUBOData ssrData{};
    ssrData.Projection = glm::perspectiveRH_NO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    ssrData.InverseProjection = glm::inverse(ssrData.Projection);
    ssrData.View = glm::mat4(1.0f);
    ssrData.RayParams = glm::vec4(32.0f, 10.0f, 0.5f, 0.25f);
    ssrData.ShadeParams = glm::vec4(0.0f, 0.6f, 0.1f, 4.0f); // Intensity 0 — the passthrough lever
    ssrData.ScreenParams = glm::vec4(static_cast<f32>(kSize), static_cast<f32>(kSize),
                                     1.0f / static_cast<f32>(kSize), 1.0f / static_cast<f32>(kSize));
    ssrData.Flags = glm::vec4(0.0f);
    const glm::vec2 hzbUVFactor = ssr->GetHZBUVFactor();
    ssrData.HZBParams = glm::vec4(hzbUVFactor.x, hzbUVFactor.y, static_cast<f32>(ssr->GetHZBMipCount()), 1.0f);
    auto ssrUbo = UniformBuffer::Create(sizeof(SSRUBOData), 38);
    ssrUbo->SetData(&ssrData, sizeof(ssrData));
    ssr->SetSSRUBO(ssrUbo);

    auto producer = Ref<PatternProducerPass>::Create(
        patternTexture, blitShader, std::string(ResourceNames::SceneColor),
        std::string(ResourceNames::SceneColorTexture),
        [](FrameBlackboard& blackboard) -> RGFramebufferHandle&
        { return blackboard.Scene.SceneColor; });
    const auto rendered = RunSinglePassChain(kSize, producer, ssr, "SSRPass", ResourceNames::SSRColor,
                                             [](FrameBlackboard& blackboard, RGFramebufferHandle handle)
                                             { blackboard.Post.SSRColor = handle; });
    ASSERT_EQ(rendered.size(), patternRgba8.size());

    u32 maxDiff = 0;
    for (sizet i = 0; i < rendered.size(); i += 4)
    {
        for (sizet c = 0; c < 3; ++c)
        {
            maxDiff = std::max(maxDiff, static_cast<u32>(std::abs(static_cast<int>(rendered[i + c]) -
                                                                  static_cast<int>(patternRgba8[i + c]))));
        }
    }
    EXPECT_LE(maxDiff, 2u) << "Intensity 0 must pass the scene through while the march + HZB chain run";

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u)
        << "the in-Execute HZB compute chain (Resize/Generate dispatches) must not fall through to a stub";
    m_ExtraSetup = nullptr;
}

// =============================================================================
// UIComposite: ClearAllAttachments over a mixed float/int MRT (this suite's
// sibling VulkanFramebuffer::ClearAllAttachments implementation exists for
// exactly this call — per-attachment transfer clears with exact layout-run
// transitions, entity-ID attachment to -1), then the FullscreenBlit of the
// upstream scene, then the one-shot 2D overlay callback under standard alpha
// blending.
//
// Contract: the blit must land the background exactly where the callback's
// half-alpha overlay does not change it fully — out = overlay*a + bg*(1-a),
// exact arithmetic — and the R32I entity attachment must read back -1
// everywhere (the clear is real, and the narrowed SetDrawBuffers({0}) scope
// kept both blended draws off the integer attachment).
// =============================================================================
TEST_F(VulkanPassSuite, UiCompositeClearsBlitsAndBlendsTheOverlayCallback)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto backgroundInput = MakeSolidTexture(kSize, 0, 0, 200, 255);
    ASSERT_NE(backgroundInput, nullptr);
    auto overlayTexture = MakeSolidTexture(kSize, 255, 0, 0, 128); // half-alpha red
    ASSERT_NE(overlayTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    RenderGraph graph;
    graph.SetTransientMaterializationEnabled(true);
    auto& blackboard = graph.GetBlackboard();

    RGResourceDesc fbDesc;
    fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    fbDesc.Format = RGResourceFormat::RGBA8UNorm;
    fbDesc.Width = kSize;
    fbDesc.Height = kSize;
    blackboard.Post.PostProcessColor =
        graph.DeclareTransientFramebuffer(ResourceNames::PostProcessColor, fbDesc);

    // Production's UIComposite target shape: colour + entity-ID + RG16F.
    FramebufferSpecification outputSpec;
    outputSpec.Width = kSize;
    outputSpec.Height = kSize;
    outputSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER,
                               FramebufferTextureFormat::RG16F };
    Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outputSpec);
    ASSERT_TRUE(outputFramebuffer);
    RGResourceDesc uiDesc;
    uiDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    uiDesc.Width = kSize;
    uiDesc.Height = kSize;
    uiDesc.Attachments = { RGResourceFormat::RGBA8UNorm, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
    uiDesc.DebugName = std::string(ResourceNames::UIComposite);
    blackboard.Post.UIComposite =
        graph.DeclareTransientFramebuffer(ResourceNames::UIComposite, uiDesc, outputFramebuffer);

    auto uiComposite = Ref<UICompositeRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    uiComposite->Init(initSpec);
    bool callbackRan = false;
    uiComposite->SetRenderCallback(
        [&]()
        {
            // The arbitrary blended 2D overlay: a half-alpha red fullscreen
            // blit through the same facade path Renderer2D uses.
            callbackRan = true;
            blitShader->Bind();
            RenderCommand::BindTexture(0, overlayTexture->GetRHIHandle());
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
        });

    auto producer = Ref<PatternProducerPass>::Create(backgroundInput, blitShader);
    graph.AddNode(producer);
    graph.AddNode(uiComposite);
    graph.SetFinalPass("UICompositePass");
    graph.BuildFrameGraph();

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    SubmitFrame(
        [&]()
        {
            graph.Execute();

            RHI::Barrier colorToSampled{};
            colorToSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(0);
            colorToSampled.Before = RHI::Access::ColorAttachmentWrite;
            colorToSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &colorToSampled, 1 });
            // The entity attachment was only transfer-CLEARED (the narrowed
            // draw-buffer scope never contained it).
            RHI::Barrier entityToSampled{};
            entityToSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(1);
            entityToSampled.Before = RHI::Access::TransferWrite;
            entityToSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &entityToSampled, 1 });
        });

    EXPECT_TRUE(producer->DidDraw);
    EXPECT_TRUE(callbackRan) << "Execute must invoke the one-shot overlay callback";
    EXPECT_TRUE(uiComposite->GetTarget()) << "UIComposite early-returned (output resolve guard)";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "UIComposite resolve failure: pass='" << failure.PassName << "' reason='"
                      << failure.Reason << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 3u) << "producer + background blit + overlay draw";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    auto* vkOutput = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
    std::vector<u8> rendered;
    ASSERT_TRUE(vkOutput->GetColorAttachmentImage(0)->GetData(rendered, 0));
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);
    // out = overlay * a + background * (1 - a), a = 128/255:
    //   R = 1.0   * 0.502 = 0.502 -> 128
    //   B = 0.784 * 0.498 = 0.390 -> 100
    for (const auto& [x, y] : { std::pair<u32, u32>{ 16, 16 }, { 64, 64 }, { 110, 110 } })
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        EXPECT_NEAR(static_cast<int>(rendered[i]), 128, 3) << "overlay red at (" << x << "," << y << ")";
        EXPECT_LE(static_cast<int>(rendered[i + 1]), 3);
        EXPECT_NEAR(static_cast<int>(rendered[i + 2]), 100, 3) << "background blue at (" << x << "," << y << ")";
    }

    // The R32I entity attachment reads back the -1 clear everywhere probed.
    std::vector<u8> entityBytes;
    ASSERT_TRUE(vkOutput->GetColorAttachmentImage(1)->GetData(entityBytes, 0));
    ASSERT_EQ(entityBytes.size(), static_cast<sizet>(kSize) * kSize * 4);
    const auto* entityIds = reinterpret_cast<const i32*>(entityBytes.data());
    for (const auto& [x, y] : { std::pair<u32, u32>{ 0, 0 }, { 64, 64 }, { 127, 127 } })
    {
        EXPECT_EQ(entityIds[static_cast<sizet>(y) * kSize + x], -1)
            << "ClearAllAttachments must clear the integer attachment to the -1 sentinel at (" << x << "," << y
            << ")";
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u)
        << "the mixed int/float clear must ride the real ClearAllAttachments implementation";
}

// =============================================================================
// OITPrepare (#691 Wave C item 1): the clears + depth-seed pass, in two chains.
//
// Chain A (scene HAS a blit-compatible depth): the producer authors depth 0.0
// into the scene FB, OITPreparePass clears accum -> (0,0,0,0) / revealage ->
// (1,0,0,0) via ClearFramebufferColorAttachment and SEEDS the OIT depth by
// BlitFramebuffer(Depth, Nearest) — then a probe draw at NDC z = 0.5 with the
// depth test ON must be BLOCKED (0.5 < 0.0 fails Less), leaving accum at its
// clear.
//
// Chain B (scene has NO depth): the fallback ClearFramebufferDepth(1.0) runs
// instead, and the SAME probe passes (0.5 < 1.0), landing the pattern in
// accum. The pair is differential: only the seeded-vs-fallback depth content
// separates the two chains, so it pins the blit's COPIED VALUES and the
// fallback clear at once — no depth readback needed.
// =============================================================================
TEST_F(VulkanPassSuite, OitPrepareClearsTargetsAndSeedsDepthFromTheScene)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto patternTexture = MakeSolidTexture(kSize, 200, 40, 90, 255);
    ASSERT_NE(patternTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    // The probe's own geometry: a fullscreen triangle at NDC z = 0.5 (the
    // V3 20-byte {pos3, uv2} stream FullscreenBlit's pull branch reads).
    constexpr f32 kProbeZ = 0.5f;
    const f32 probeVertices[] = {
        -1.0f, -1.0f, kProbeZ, 0.0f, 0.0f,
        3.0f, -1.0f, kProbeZ, 2.0f, 0.0f,
        -1.0f, 3.0f, kProbeZ, 0.0f, 2.0f
    };
    u32 probeIndices[] = { 0u, 1u, 2u }; // IndexBuffer::Create takes a non-const u32*
    auto probeVB = VertexBuffer::Create(probeVertices, sizeof(probeVertices));
    auto probeIB = IndexBuffer::Create(probeIndices, 3);
    auto probeVA = VertexArray::Create();
    probeVA->AddVertexBuffer(probeVB);
    probeVA->SetIndexBuffer(probeIB);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u32 stubsBefore = api.GetPhase6StubHitCount();

    struct ChainResult
    {
        std::vector<u8> Accum;     // RGBA16F raw halfs
        std::vector<u8> Revealage; // RG16F raw halfs
    };

    const auto runChain = [&](bool sceneHasDepth) -> ChainResult
    {
        ChainResult result;

        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        auto& blackboard = graph.GetBlackboard();

        // Caller-backed scene FB — with or without the blit-compatible depth.
        FramebufferSpecification sceneSpec;
        sceneSpec.Width = kSize;
        sceneSpec.Height = kSize;
        if (sceneHasDepth)
            sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
        else
            sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
        EXPECT_TRUE(sceneFramebuffer);

        RGResourceDesc sceneDesc;
        sceneDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        sceneDesc.Format = RGResourceFormat::RGBA8UNorm;
        sceneDesc.Width = kSize;
        sceneDesc.Height = kSize;
        blackboard.Scene.SceneColor =
            graph.DeclareTransientFramebuffer(ResourceNames::SceneColor, sceneDesc, sceneFramebuffer);
        if (sceneHasDepth)
        {
            blackboard.Scene.SceneDepthAttachment = graph.CreateFramebufferDepthAttachmentView(
                ResourceNames::SceneDepthAttachment, blackboard.Scene.SceneColor);
        }

        // Caller-backed OIT MRT FB (production shape: RGBA16F + RG16F + D24S8).
        FramebufferSpecification oitSpec;
        oitSpec.Width = kSize;
        oitSpec.Height = kSize;
        oitSpec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::RG16F,
                                FramebufferTextureFormat::Depth };
        Ref<Framebuffer> oitFramebuffer = Framebuffer::Create(oitSpec);
        EXPECT_TRUE(oitFramebuffer);

        RGResourceDesc oitDesc;
        oitDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        oitDesc.Width = kSize;
        oitDesc.Height = kSize;
        oitDesc.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::RG16Float,
                                RGResourceFormat::Depth24Stencil8 };
        const auto oitHandle = graph.DeclareTransientFramebuffer(ResourceNames::OITBuffer, oitDesc, oitFramebuffer);
        blackboard.OIT.OITBuffer = oitHandle;
        blackboard.OIT.OITAccum = graph.CreateFramebufferAttachmentView(ResourceNames::OITAccum, oitHandle, 0u);
        blackboard.OIT.OITRevealage =
            graph.CreateFramebufferAttachmentView(ResourceNames::OITRevealage, oitHandle, 1u);
        blackboard.OIT.OITDepthAttachment =
            graph.CreateFramebufferDepthAttachmentView(ResourceNames::OITDepthAttachment, oitHandle);

        auto producer = Ref<PatternProducerPass>::Create(
            patternTexture, blitShader, std::string(ResourceNames::SceneColor),
            std::string(ResourceNames::SceneColorTexture),
            [](FrameBlackboard& board) -> RGFramebufferHandle&
            { return board.Scene.SceneColor; });
        producer->ClearTargetFirst = true;
        producer->WriteDepth = sceneHasDepth;
        // OITPrepare consumes the scene through its DEPTH VIEW (transfer
        // source), not a versioned colour read — without the side-effect
        // mark the reachability cull drops the producer.
        producer->TreatAsSideEffecting = true;

        auto prepare = Ref<OITPrepareRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        prepare->Init(initSpec);
        prepare->SetEnabled(true);
        prepare->SetHasContributors(true);

        graph.AddNode(producer);
        graph.AddNode(prepare);
        graph.SetFinalPass("OITPreparePass");
        graph.BuildFrameGraph();

        SubmitFrame(
            [&]()
            {
                graph.Execute();

                // --- probe draw (a test instrument, not a pass) ------------
                // Depth-test the seeded/fallback OIT depth at NDC z = 0.5:
                // blocked by the seed (0.0), passed by the fallback (1.0).
                oitFramebuffer->Bind();
                RenderCommand::SetViewport(0, 0, kSize, kSize);
                RenderCommand::SetDepthTest(true);
                RenderCommand::SetDepthMask(false);
                RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
                RenderCommand::SetBlendState(false);
                RenderCommand::DisableCulling();
                blitShader->Bind();
                RenderCommand::BindTexture(0, patternTexture->GetRHIHandle());
                probeVA->Bind();
                RenderCommand::DrawIndexed(probeVA);
                oitFramebuffer->Unbind();
                RenderCommand::SetDepthTest(false);
                RenderCommand::SetDepthFunc(RHI::CompareOp::Less);

                // Readback barriers: both OIT color attachments were last in
                // the probe's rendering scope (attachment writes).
                std::array<RHI::Barrier, 2> toSampled{};
                toSampled[0].Resource = oitFramebuffer->GetColorAttachmentHandle(0);
                toSampled[0].Before = RHI::Access::ColorAttachmentWrite;
                toSampled[0].After = RHI::Access::ShaderSampleRead;
                toSampled[1].Resource = oitFramebuffer->GetColorAttachmentHandle(1);
                toSampled[1].Before = RHI::Access::ColorAttachmentWrite;
                toSampled[1].After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ toSampled });
            });

        EXPECT_TRUE(producer->DidDraw) << "producer early-returned (sceneHasDepth=" << sceneHasDepth << ")";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "OITPrepare resolve failure: pass='" << failure.PassName << "' reason='"
                          << failure.Reason << "' x" << failure.Count;
        }

        auto* vkOit = static_cast<VulkanFramebuffer*>(oitFramebuffer.Raw());
        EXPECT_TRUE(vkOit->GetColorAttachmentImage(0)->GetData(result.Accum, 0));
        EXPECT_TRUE(vkOit->GetColorAttachmentImage(1)->GetData(result.Revealage, 0));
        return result;
    };

    const ChainResult seeded = runChain(true);
    const ChainResult fallback = runChain(false);

    ASSERT_EQ(seeded.Accum.size(), static_cast<sizet>(kSize) * kSize * 8);
    ASSERT_EQ(seeded.Revealage.size(), static_cast<sizet>(kSize) * kSize * 4);
    ASSERT_EQ(fallback.Accum.size(), static_cast<sizet>(kSize) * kSize * 8);

    const auto accumTexel = [&](const ChainResult& r, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(r.Accum.data());
        const sizet base = (static_cast<sizet>(y) * kSize + x) * 4;
        return std::array<f32, 4>{ HalfToFloat(halves[base]), HalfToFloat(halves[base + 1]),
                                   HalfToFloat(halves[base + 2]), HalfToFloat(halves[base + 3]) };
    };
    const auto revealageTexel = [&](const ChainResult& r, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(r.Revealage.data());
        const sizet base = (static_cast<sizet>(y) * kSize + x) * 2;
        return HalfToFloat(halves[base]);
    };

    for (const auto& [x, y] : { std::pair<u32, u32>{ 5, 5 }, { 64, 64 }, { 120, 120 } })
    {
        // Chain A: the seeded depth (0.0) blocks the z=0.5 probe — accum
        // keeps the (0,0,0,0) clear, revealage the (1,...) clear.
        const auto blocked = accumTexel(seeded, x, y);
        EXPECT_NEAR(blocked[0], 0.0f, 1e-3f) << "seeded chain accum.r at (" << x << "," << y << ")";
        EXPECT_NEAR(blocked[3], 0.0f, 1e-3f) << "seeded chain accum.a at (" << x << "," << y << ")";
        EXPECT_NEAR(revealageTexel(seeded, x, y), 1.0f, 1e-3f)
            << "revealage clear at (" << x << "," << y << ")";

        // Chain B: the fallback depth clear (1.0) admits the probe — accum
        // takes the pattern (200,40,90)/255.
        const auto passed = accumTexel(fallback, x, y);
        EXPECT_NEAR(passed[0], 200.0f / 255.0f, 0.01f) << "fallback chain accum.r at (" << x << "," << y << ")";
        EXPECT_NEAR(passed[1], 40.0f / 255.0f, 0.01f) << "fallback chain accum.g at (" << x << "," << y << ")";
        EXPECT_NEAR(passed[2], 90.0f / 255.0f, 0.01f) << "fallback chain accum.b at (" << x << "," << y << ")";
        EXPECT_NEAR(revealageTexel(fallback, x, y), 1.0f, 1e-3f)
            << "fallback revealage clear at (" << x << "," << y << ")";
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "OITPrepare must ride the real clears + depth blit, not stubs";
}

// =============================================================================
// FluidComposite (#691 Wave C item 2), two halves:
//
// (a) PASS-LEVEL FLOOR: the composite's Execute gates on its intermediates
//     partner's RanThisFrame() — and FluidIntermediatesPass::Execute cannot
//     reach `m_RanThisFrame = true` on this backend yet (its targets come
//     from the raw texture/FBO facade family, still Phase6Stub — the
//     documented Wave C raw-FBO slice). The floor pins that the composite
//     DECLARES its graph resources with a pending draw and then early-outs
//     cleanly, leaving the scene untouched — the same "documented floor"
//     shape FluidIntermediates itself pinned in Wave B.
//
// (b) SHADER-LEVEL REFRACTION PASSTHROUGH: the composite's V3 draw with the
//     REAL FluidComposite.glsl (its pull branch is this batch's sibling
//     change), driven directly with imported stand-ins: fluid depth 0 in the
//     top band (-> discard, scene EXACT) and 0.251 m in the bottom band with
//     thickness 0 (-> transmittance exp(0)=1, refraction offset 0, so the
//     copied scene passes through modulo the ~2% head-on Fresnel mix with
//     the sky-tint fallback). CopyImageSubData seeds the refraction copy —
//     the composite's production snapshot path.
// =============================================================================
TEST_F(VulkanPassSuite, FluidCompositeFloorsWithoutIntermediatesAndPassesRefractionThrough)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto patternTexture = MakeSolidTexture(kSize, 200, 40, 90, 255);
    ASSERT_NE(patternTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

    // ---- (a) the pass-level floor ------------------------------------------
    FramebufferSpecification sceneSpec;
    sceneSpec.Width = kSize;
    sceneSpec.Height = kSize;
    sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
    ASSERT_TRUE(sceneFramebuffer);

    {
        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        auto& blackboard = graph.GetBlackboard();

        RGResourceDesc sceneDesc;
        sceneDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        sceneDesc.Format = RGResourceFormat::RGBA8UNorm;
        sceneDesc.Width = kSize;
        sceneDesc.Height = kSize;
        blackboard.Scene.SceneColor =
            graph.DeclareTransientFramebuffer(ResourceNames::SceneColor, sceneDesc, sceneFramebuffer);
        blackboard.Scene.SceneColorTexture =
            graph.CreateFramebufferAttachmentView(ResourceNames::SceneColorTexture, blackboard.Scene.SceneColor, 0u);
        blackboard.Scene.SceneDepthAttachment = graph.CreateFramebufferDepthAttachmentView(
            ResourceNames::SceneDepthAttachment, blackboard.Scene.SceneColor);

        RGResourceDesc refractionDesc;
        refractionDesc.Kind = RGResourceHandle::Kind::Texture2D;
        refractionDesc.Format = RGResourceFormat::RGBA16Float;
        refractionDesc.Width = kSize;
        refractionDesc.Height = kSize;
        refractionDesc.DebugName = "FluidRefraction";
        blackboard.Scratch.FluidRefraction = graph.AllocateTransientTextureHandle("FluidRefraction", refractionDesc);

        // A pending draw makes HasPendingDraws() true, so the composite's
        // Setup declares everything — but the intermediates pass was never
        // Init'd and cannot run its body (the raw-FBO stub family), so
        // RanThisFrame() stays false and the composite's Execute early-outs.
        auto intermediates = Ref<FluidIntermediatesPass>::Create();
        FluidRenderData pendingDraw{};
        pendingDraw.ParticleUpperBound = 16u;
        intermediates->SetFrameDraws({ pendingDraw });

        auto composite = Ref<FluidCompositePass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        composite->Init(initSpec);
        composite->SetEnabled(true);
        composite->SetIntermediatesPass(intermediates);
        ASSERT_TRUE(composite->IsReadyForExecution())
            << "FluidComposite.glsl must compile through shaderc — its OLO_VULKAN pull branch";

        auto producer = Ref<PatternProducerPass>::Create(
            patternTexture, blitShader, std::string(ResourceNames::SceneColor),
            std::string(ResourceNames::SceneColorTexture),
            [](FrameBlackboard& board) -> RGFramebufferHandle&
            { return board.Scene.SceneColor; });
        // The composite reads the fixture-minted v0 attachment views, which
        // the cull does not chain to the producer's new version — keep the
        // producer via the side-effect mark (the floor's premise is that the
        // COMPOSITE early-outs, not that the scene is empty).
        producer->TreatAsSideEffecting = true;

        graph.AddNode(producer);
        graph.AddNode(composite);
        graph.SetFinalPass("FluidCompositePass");
        graph.BuildFrameGraph();

        SubmitFrame(
            [&]()
            {
                graph.Execute();

                RHI::Barrier toSampled{};
                toSampled.Resource = sceneFramebuffer->GetColorAttachmentHandle(0);
                toSampled.Before = RHI::Access::ColorAttachmentWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            });

        EXPECT_TRUE(producer->DidDraw);
        EXPECT_FALSE(intermediates->RanThisFrame())
            << "the raw-FBO stub family must still gate the intermediates body (this floor's premise)";

        std::vector<u8> rendered;
        auto* vkScene = static_cast<VulkanFramebuffer*>(sceneFramebuffer.Raw());
        ASSERT_TRUE(vkScene->GetColorAttachmentImage(0)->GetData(rendered, 0));
        const sizet centre = ((static_cast<sizet>(64) * kSize) + 64) * 4;
        EXPECT_EQ(rendered[centre + 0], 200) << "the gated composite must leave the scene untouched";
        EXPECT_EQ(rendered[centre + 1], 40);
        EXPECT_EQ(rendered[centre + 2], 90);
    }

    // ---- (b) the shader-level refraction passthrough -----------------------
    // Stand-ins: fluid depth split at row 64 (top r=0 -> discard, bottom
    // r=64/255 = 0.251 m of view depth), thickness 0 everywhere, scene depth
    // far (1.0).
    auto fluidDepthTexture = MakeHorizontalSplitTexture(kSize, 64, { 0, 0, 0, 255 }, { 64, 0, 0, 255 });
    auto fluidThicknessTexture = MakeSolidTexture(kSize, 0, 0, 0, 0);
    auto sceneDepthTexture = MakeSolidTexture(kSize, 255, 255, 255, 255);
    auto refractionTexture = MakeSolidTexture(kSize, 0, 0, 0, 0);
    ASSERT_TRUE(fluidDepthTexture && fluidThicknessTexture && sceneDepthTexture && refractionTexture);

    auto compositeShader = Shader::Create("assets/shaders/FluidComposite.glsl");
    ASSERT_TRUE(compositeShader);
    ASSERT_EQ(compositeShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    // CameraUBO: eye at origin looking down -Z (identity view), standard
    // perspective — the fragment only uses P00/P11 for view reconstruction,
    // the window-depth guard, and the velocity reprojection (xy, unused by
    // the contracts).
    const glm::mat4 projection = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = projection;
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = projection;
    cameraData.Position = glm::vec3(0.0f);
    cameraData.PrevViewProjection = projection;
    cameraData.RenderOrigin = glm::vec3(0.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    UBOStructures::FluidRenderUBO fluidUbo{};
    fluidUbo.TintRadius = glm::vec4(0.5f, 0.5f, 0.5f, 0.1f);
    fluidUbo.AbsorptionParams = glm::vec4(0.45f, 0.06f, 0.01f, 1.0f);
    fluidUbo.FoamParams = glm::vec4(3.0f, 1.0f, 0.0f, 0.0f);
    fluidUbo.SmoothParams = glm::vec4(0.0f, 0.4f, 0.1f, 100.0f);
    fluidUbo.ScreenParams = glm::vec4(static_cast<f32>(kSize), static_cast<f32>(kSize),
                                      1.0f / static_cast<f32>(kSize), 1.0f / static_cast<f32>(kSize));
    fluidUbo.Counts = glm::uvec4(16u, 7u, 0u, 0u); // z = 0: no environment map — sky-tint fallback
    auto fluidRenderUbo = UniformBuffer::Create(UBOStructures::FluidRenderUBO::GetSize(),
                                                ShaderBindingLayout::UBO_FLUID_RENDER);
    fluidRenderUbo->SetData(&fluidUbo, UBOStructures::FluidRenderUBO::GetSize());

    FramebufferSpecification outSpec;
    outSpec.Width = kSize;
    outSpec.Height = kSize;
    outSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outSpec);
    ASSERT_TRUE(outputFramebuffer);

    SubmitFrame(
        [&]()
        {
            // Seed the scene into the output, snapshot it as the refraction
            // copy (the composite's production CopyImageSubData path), then
            // run the composite draw over it.
            outputFramebuffer->Bind();
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();
            blitShader->Bind();
            RenderCommand::BindTexture(0, patternTexture->GetRHIHandle());
            const auto tri = MeshPrimitives::GetFullscreenTriangle();
            tri->Bind();
            RenderCommand::DrawIndexed(tri);

            RenderCommand::CopyImageSubData(outputFramebuffer->GetColorAttachmentHandle(0),
                                            RendererAPI::TextureTargetType::Texture2D,
                                            refractionTexture->GetRHIHandle(),
                                            RendererAPI::TextureTargetType::Texture2D, kSize, kSize);

            compositeShader->Bind();
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_FLUID_DEPTH, fluidDepthTexture->GetRHIHandle());
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_FLUID_THICKNESS,
                                       fluidThicknessTexture->GetRHIHandle());
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, sceneDepthTexture->GetRHIHandle());
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, refractionTexture->GetRHIHandle());
            tri->Bind();
            RenderCommand::DrawIndexed(tri);

            RHI::Barrier toSampled{};
            toSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(0);
            toSampled.Before = RHI::Access::ColorAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });
    // Counters reset per recording bracket — absolute counts for THIS submit.
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 2u) << "seed draw + composite draw must both prepare";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    std::vector<u8> rendered;
    auto* vkOut = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
    ASSERT_TRUE(vkOut->GetColorAttachmentImage(0)->GetData(rendered, 0));
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

    const auto px = [&](u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        return std::array<int, 3>{ rendered[i], rendered[i + 1], rendered[i + 2] };
    };

    // Top band (fluid depth 0): the discard path — scene EXACT.
    const auto discarded = px(64, 32);
    EXPECT_EQ(discarded[0], 200) << "no-fluid pixels must discard (scene untouched)";
    EXPECT_EQ(discarded[1], 40);
    EXPECT_EQ(discarded[2], 90);

    // Bottom band (uniform 0.251 m fluid, thickness 0): transmittance
    // exp(0) = 1 and a zero refraction offset pass the copied scene through;
    // the only shift is the head-on Fresnel (~0.02) mix with the sky-tint
    // fallback mix((0.35,0.45,0.60), Tint=0.5, 0.35) = (0.4025,0.4675,0.565):
    //   r = 0.98*0.7843 + 0.02*0.4025 = 0.777  -> 198
    //   g = 0.98*0.1569 + 0.02*0.4675 = 0.163  -> 42
    //   b = 0.98*0.3529 + 0.02*0.565  = 0.357  -> 91
    const auto lit = px(64, 96);
    EXPECT_NEAR(lit[0], 198, 4) << "zero-thickness fluid must pass the refraction copy through";
    EXPECT_NEAR(lit[1], 42, 4);
    EXPECT_NEAR(lit[2], 91, 4);
}

// =============================================================================
// Overdraw (#691 Wave C item 3), two halves:
//
// (a) PASS-LEVEL: the REAL OverdrawRenderPass through the graph with an
//     EMPTY SceneRenderPass bucket — the replay plumbing runs end to end
//     (accum FB creation + ClearAllAttachments, SetOverdrawActive toggles,
//     bucket execute, heat-map draw) and a zero count maps to exact black.
//     A REAL bucket replay needs scene draw packets (V1 vertex pull + the
//     CommandDispatch packet path + the overdraw shader swap + camera /
//     instance SSBO state) — that is port-order item 10's machinery, so
//     constructing one here would be disproportionate; the empty-bucket run
//     + the ramp half below pin everything this pass owns itself.
//
// (b) SHADER-LEVEL HEAT RAMP: PostProcess_OverdrawHeatmap.glsl (pull branch
//     added this batch) over an imported count texture — the ramp's blue /
//     green / red anchors at counts 2.5 / 5 / 10 out of kMaxLayers = 10.
// =============================================================================
TEST_F(VulkanPassSuite, OverdrawRunsTheEmptyReplayAndMapsCountsToHeatColours)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

    // ---- (a) the pass through the graph ------------------------------------
    FramebufferSpecification outSpec;
    outSpec.Width = kSize;
    outSpec.Height = kSize;
    outSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outSpec);
    ASSERT_TRUE(outputFramebuffer);

    SceneRenderPass emptyScenePass; // ctor is inert; its bucket stays empty

    auto overdraw = Ref<OverdrawRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    overdraw->Init(initSpec);
    ASSERT_TRUE(overdraw->IsReadyForExecution())
        << "PostProcess_OverdrawHeatmap.glsl must compile through shaderc — its OLO_VULKAN pull branch";
    overdraw->SetEnabled(true);
    overdraw->SetScenePass(&emptyScenePass);

    {
        RenderGraph graph;
        graph.SetTransientMaterializationEnabled(true);
        auto& blackboard = graph.GetBlackboard();

        RGResourceDesc fbDesc;
        fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
        fbDesc.Format = RGResourceFormat::RGBA8UNorm;
        fbDesc.Width = kSize;
        fbDesc.Height = kSize;
        blackboard.Post.OverdrawColor =
            graph.DeclareTransientFramebuffer(ResourceNames::OverdrawColor, fbDesc, outputFramebuffer);

        graph.AddNode(overdraw);
        graph.SetFinalPass("OverdrawPass");
        graph.BuildFrameGraph();

        SubmitFrame(
            [&]()
            {
                graph.Execute();

                RHI::Barrier toSampled{};
                toSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(0);
                toSampled.Before = RHI::Access::ColorAttachmentWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            });

        EXPECT_TRUE(overdraw->GetTarget()) << "the pass early-returned";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "Overdraw resolve failure: pass='" << failure.PassName << "' reason='"
                          << failure.Reason << "' x" << failure.Count;
        }
        // Counters reset per recording bracket — absolute count for this submit.
        EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 1u)
            << "exactly the heat-map draw (the replayed bucket is empty)";
        EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

        std::vector<u8> rendered;
        auto* vkOut = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
        ASSERT_TRUE(vkOut->GetColorAttachmentImage(0)->GetData(rendered, 0));
        for (const auto& [x, y] : { std::pair<u32, u32>{ 3, 3 }, { 64, 64 }, { 124, 124 } })
        {
            const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
            EXPECT_EQ(rendered[i + 0], 0) << "HeatColor(0) is black at (" << x << "," << y << ")";
            EXPECT_EQ(rendered[i + 1], 0);
            EXPECT_EQ(rendered[i + 2], 0);
            EXPECT_EQ(rendered[i + 3], 255);
        }
    }

    // ---- (b) the analytic heat ramp ----------------------------------------
    // Count stand-in: three vertical thirds at 2.5 / 5 / 10 layers. The
    // sampler reads .r; an RGBA8 import cannot hold >1, so bake the counts
    // through kMaxLayers-scaled values instead: the shader divides by 10, so
    // r = 64/255 = 0.251 (t=0.025)... — too coarse. Use an RGBA16F texture
    // written raw instead: SetData accepts the client format of the spec.
    TextureSpecification countSpec;
    countSpec.Width = kSize;
    countSpec.Height = kSize;
    countSpec.Format = ImageFormat::RGBA32F;
    countSpec.GenerateMips = false;
    auto countTexture = Texture2D::Create(countSpec);
    ASSERT_TRUE(countTexture);
    {
        std::vector<f32> counts(static_cast<sizet>(kSize) * kSize * 4, 0.0f);
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const f32 count = x < kSize / 3 ? 2.5f : (x < (2 * kSize) / 3 ? 5.0f : 10.0f);
                counts[(static_cast<sizet>(y) * kSize + x) * 4] = count;
            }
        }
        countTexture->SetData(counts.data(), static_cast<u32>(counts.size() * sizeof(f32)));
    }

    auto heatmapShader = Shader::Create("assets/shaders/PostProcess_OverdrawHeatmap.glsl");
    ASSERT_TRUE(heatmapShader);
    ASSERT_EQ(heatmapShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    FramebufferSpecification rampSpec;
    rampSpec.Width = kSize;
    rampSpec.Height = kSize;
    rampSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    Ref<Framebuffer> rampFramebuffer = Framebuffer::Create(rampSpec);
    ASSERT_TRUE(rampFramebuffer);

    SubmitFrame(
        [&]()
        {
            rampFramebuffer->Bind();
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();
            heatmapShader->Bind();
            RenderCommand::BindTexture(0, countTexture->GetRHIHandle());
            const auto tri = MeshPrimitives::GetFullscreenTriangle();
            tri->Bind();
            RenderCommand::DrawIndexed(tri);
            rampFramebuffer->Unbind();

            RHI::Barrier toSampled{};
            toSampled.Resource = rampFramebuffer->GetColorAttachmentHandle(0);
            toSampled.Before = RHI::Access::ColorAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    std::vector<u8> ramp;
    auto* vkRamp = static_cast<VulkanFramebuffer*>(rampFramebuffer.Raw());
    ASSERT_TRUE(vkRamp->GetColorAttachmentImage(0)->GetData(ramp, 0));

    const auto rampPx = [&](u32 x)
    {
        const sizet i = (static_cast<sizet>(64) * kSize + x) * 4;
        return std::array<int, 3>{ ramp[i], ramp[i + 1], ramp[i + 2] };
    };
    // count 2.5 -> t = 0.25 -> pure blue; 5 -> t = 0.5 -> pure green;
    // 10 -> t = 1.0 -> pure red (the CPU-mirrored OverdrawHeatmap ramp).
    const auto blue = rampPx(20);
    EXPECT_NEAR(blue[0], 0, 3);
    EXPECT_NEAR(blue[1], 0, 3);
    EXPECT_NEAR(blue[2], 255, 3) << "2.5 of 10 layers must be the pure-blue anchor";
    const auto green = rampPx(64);
    EXPECT_NEAR(green[0], 0, 3);
    EXPECT_NEAR(green[1], 255, 3) << "5 of 10 layers must be the pure-green anchor";
    EXPECT_NEAR(green[2], 0, 3);
    const auto red = rampPx(120);
    EXPECT_NEAR(red[0], 255, 3) << "10 of 10 layers must be the pure-red anchor";
    EXPECT_NEAR(red[1], 0, 3);
    EXPECT_NEAR(red[2], 0, 3);
}

namespace
{
    // CPU mirror of the DeferredLighting analytic contract's shader terms
    // (PBRCommon.glsl cookTorranceBRDF + calculateLightContribution +
    // calculateSimpleAmbient; DeferredLightingShared.glsl composition). Kept
    // formula-identical INCLUDING the epsilons so the expected pixel is
    // computed, not tuned.
    glm::vec3 MirrorDeferredLitPixel(const glm::vec3& albedo, const glm::vec3& N, const glm::vec3& V,
                                     const glm::vec3& L)
    {
        constexpr f32 kEpsilon = 0.0001f;
        constexpr f32 kPi = 3.14159265359f;
        const f32 roughness = 1.0f;
        const f32 metallic = 0.0f;

        const glm::vec3 H = glm::normalize(V + L);
        const f32 a = roughness * roughness;
        const f32 a2 = a * a;
        const f32 NdotH = std::max(glm::dot(N, H), 0.0f);
        f32 denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
        denom = kPi * denom * denom;
        const f32 ndf = a2 / std::max(denom, kEpsilon);

        const auto schlickGGX = [&](f32 NdotX)
        {
            const f32 r = roughness + 1.0f;
            const f32 k = (r * r) / 8.0f;
            return NdotX / std::max(NdotX * (1.0f - k) + k, kEpsilon);
        };
        const f32 NdotV = std::max(glm::dot(N, V), 0.0f);
        const f32 NdotL = std::max(glm::dot(N, L), 0.0f);
        const f32 g = schlickGGX(NdotV) * schlickGGX(NdotL);

        const glm::vec3 f0(0.04f);
        const f32 hDotV = std::max(glm::dot(H, V), 0.0f);
        const glm::vec3 fresnel = f0 + (glm::vec3(1.0f) - f0) * std::pow(1.0f - hDotV, 5.0f);

        const glm::vec3 specular = (ndf * g * fresnel) / (4.0f * NdotV * NdotL + kEpsilon);
        const glm::vec3 kD = (glm::vec3(1.0f) - fresnel) * (1.0f - metallic);
        const glm::vec3 brdf = kD * albedo * (1.0f / kPi) + specular;

        const glm::vec3 lo = brdf * NdotL;                   // white light, intensity 1, no attenuation
        const glm::vec3 ambient = glm::vec3(0.03f) * albedo; // calculateSimpleAmbient, ao = 1
        return ambient + lo;
    }
} // namespace

// =============================================================================
// DeferredLighting (#691 Wave C item 4): the G-buffer sampling tenant.
//
// Imports 5 G-buffer stand-ins under the pass's blackboard names, runs the
// UNMODIFIED pass against a caller-backed {RGBA8, R32I, D24S8} scene FB and
// a real (cleared) GBuffer object, and pins:
//   * the far band (depth 1.0): the emissive/sky passthrough EXACT;
//   * the lit band: a single pixel's full Lambert+GGX term from the known
//     G-buffer normal/albedo against one directional light, mirrored on the
//     CPU (MirrorDeferredLitPixel) — light straight down the normal;
//   * SetFramebufferDrawAttachments narrowing: the lighting draw touches
//     only RT0 while the {1}-narrowed COLOR blit lands the G-buffer's
//     entity-ID clear (42) in RT1 — the blit-remap half of the mechanism;
//   * the two shadow-sampler families bind without validation errors
//     (compare-on placeholders via NullSamplerKind::Texture2DArrayShadow,
//     compare-off raw views via CreateDepthArrayCompareOffViewHandle);
//   * the depth blit (G-buffer -> scene) rides the same BlitFramebuffer
//     path the OITPrepare tenant pinned by content.
// =============================================================================
TEST_F(VulkanPassSuite, DeferredLightingShadesAKnownGBufferAndBlitsEntityIds)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

    // The ambient path keys off PROCESS-GLOBAL state: the light-probe
    // setting AND the global IBL handles (earlier GL suites leave real
    // GL-currency IBL maps published, flipping iblAvailable on — the maps
    // then resolve to Vulkan nulls and the ambient term silently swaps from
    // simple-ambient to sampled-black, a -6/255 red shift the lit-band
    // contract caught in the full-suite run). Force the deterministic
    // simple-ambient configuration and restore on exit.
    auto& settings = Renderer3D::GetRendererSettings();
    const bool prevProbes = settings.Deferred.EnableLightProbes;
    settings.Deferred.EnableLightProbes = false;
    const RHI::ResourceHandle prevIrradiance = Renderer3D::GetGlobalIrradianceMapHandle();
    const RHI::ResourceHandle prevPrefilter = Renderer3D::GetGlobalPrefilterMapHandle();
    const RHI::ResourceHandle prevBrdfLut = Renderer3D::GetGlobalBRDFLutMapHandle();
    const RHI::ResourceHandle prevEnvironment = Renderer3D::GetGlobalEnvironmentMapHandle();
    const f32 prevIblIntensity = Renderer3D::GetGlobalIBLIntensity();
    Renderer3D::ClearGlobalIBL();

    // --- G-buffer stand-ins --------------------------------------------------
    // Albedo (0.8, 0.2, 0.2), metallic 0. Normal +Z oct-encodes to (0,0);
    // roughness 1, ao 1. Emissive: sky colour in the far band, 0 in the lit
    // band (the shader adds it). Depth: far band 1.0, lit band 128/255.
    auto albedoTexture = MakeSolidTexture(kSize, 204, 51, 51, 0);
    auto normalTexture = MakeSolidTexture(kSize, 0, 0, 255, 255);
    auto emissiveTexture = MakeHorizontalSplitTexture(kSize, 64, { 30, 60, 200, 0 }, { 0, 0, 0, 0 });
    auto velocityTexture = MakeSolidTexture(kSize, 0, 0, 0, 0);
    auto depthTexture = MakeHorizontalSplitTexture(kSize, 64, { 255, 0, 0, 255 }, { 128, 0, 0, 255 });
    ASSERT_TRUE(albedoTexture && normalTexture && emissiveTexture && velocityTexture && depthTexture);

    // --- UBOs the shader declares (bind them ALL — the DRS lesson) ----------
    // Camera: eye at +10 Z looking down -Z. World positions come from the
    // MotionBlur block's identity inverse-VP, so the lit-band pixel at the
    // band centre sits near the origin and V ~ N ~ L.
    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    cameraData.Position = glm::vec3(0.0f, 0.0f, 10.0f);
    cameraData.PrevViewProjection = glm::mat4(1.0f);
    cameraData.RenderOrigin = glm::vec3(0.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    // One directional light along -Z (L = +Z = the G-buffer normal), white,
    // intensity 1, no shadow entry (direction.w = -1).
    auto lightData = std::make_unique<ShaderBindingLayout::MultiLightUBO>();
    std::memset(lightData.get(), 0, sizeof(ShaderBindingLayout::MultiLightUBO));
    lightData->LightCount = 1;
    lightData->MaxLights = static_cast<i32>(ShaderBindingLayout::MultiLightUBO::MAX_LIGHTS);
    lightData->DirectionalLightCount = 1;
    lightData->Lights[0].Position = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // w = 0: DIRECTIONAL_LIGHT
    lightData->Lights[0].Direction = glm::vec4(0.0f, 0.0f, -1.0f, -1.0f);
    lightData->Lights[0].Color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    lightData->Lights[0].SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    auto lightsUbo = UniformBuffer::Create(ShaderBindingLayout::MultiLightUBO::GetSize(),
                                           ShaderBindingLayout::UBO_MULTI_LIGHTS);
    lightsUbo->SetData(lightData.get(), ShaderBindingLayout::MultiLightUBO::GetSize());

    // Shadows fully disabled (DirectionalShadowEnabled = 0, no atlas entries).
    auto shadowData = std::make_unique<ShaderBindingLayout::ShadowUBO>();
    std::memset(shadowData.get(), 0, sizeof(ShaderBindingLayout::ShadowUBO));
    auto shadowUbo = UniformBuffer::Create(ShaderBindingLayout::ShadowUBO::GetSize(), ShaderBindingLayout::UBO_SHADOW);
    shadowUbo->SetData(shadowData.get(), ShaderBindingLayout::ShadowUBO::GetSize());

    // MotionBlur block: identity inverse-VP -> worldPos = (uv*2-1, d*2-1).
    MotionBlurUBOData mbData{};
    mbData.InverseViewProjection = glm::mat4(1.0f);
    mbData.PrevViewProjection = glm::mat4(1.0f);
    auto mbUbo = UniformBuffer::Create(sizeof(MotionBlurUBOData), 8);
    mbUbo->SetData(&mbData, sizeof(MotionBlurUBOData));

    // Atmosphere shading (UBO 54): zeros = wetness off, cloud shadow 1.0.
    struct AtmosphereShadingZeros
    {
        glm::vec4 Params0{ 0.0f };
        glm::vec4 Params1{ 0.0f };
    } atmosphereZeros;
    auto atmosphereUbo = UniformBuffer::Create(sizeof(AtmosphereShadingZeros),
                                               ShaderBindingLayout::UBO_ATMOSPHERE_SHADING);
    atmosphereUbo->SetData(&atmosphereZeros, sizeof(atmosphereZeros));

    // Forward+ params (UBO 25): zeros = fplusActive false (the production
    // "disabled UBO" shape).
    struct ForwardPlusZeros
    {
        glm::uvec4 Params{ 0u };
        glm::vec4 Depth{ 0.0f };
    } fplusZeros;
    auto fplusUbo = UniformBuffer::Create(sizeof(ForwardPlusZeros), 25);
    fplusUbo->SetData(&fplusZeros, sizeof(fplusZeros));

    // --- the pass + its objects ---------------------------------------------
    // Real single-sample GBuffer: dimensions + the blit SOURCE. Its cleared
    // depth (1.0) and entity attachment (42) are what the two blits hand to
    // the scene FB.
    auto gbuffer = GBuffer::Create(kSize, kSize, 1);
    ASSERT_TRUE(gbuffer);
    // Copy the Ref: the accessor returns a const Ref, which const-propagates
    // to the framebuffer, and the clear is a mutating call. The clear itself
    // runs INSIDE SubmitFrame below — the facade transfer clears need the
    // recording bracket (outside it they are counted stubs and no-op).
    Ref<Framebuffer> gbufferSamplingFB = gbuffer->GetSamplingFramebuffer();

    FramebufferSpecification sceneSpec;
    sceneSpec.Width = kSize;
    sceneSpec.Height = kSize;
    sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER,
                              FramebufferTextureFormat::Depth };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
    ASSERT_TRUE(sceneFramebuffer);

    auto deferredLighting = Ref<DeferredLightingPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    deferredLighting->Init(initSpec);
    deferredLighting->SetGBuffer(gbuffer);
    deferredLighting->SetPerSampleLighting(false);

    RenderGraph graph;
    graph.SetTransientMaterializationEnabled(true);
    auto& blackboard = graph.GetBlackboard();

    RGResourceDesc sceneDesc;
    sceneDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    sceneDesc.Format = RGResourceFormat::RGBA8UNorm;
    sceneDesc.Width = kSize;
    sceneDesc.Height = kSize;
    blackboard.Scene.SceneColor =
        graph.DeclareTransientFramebuffer(ResourceNames::SceneColor, sceneDesc, sceneFramebuffer);

    RGResourceDesc importDesc;
    importDesc.Kind = RGResourceHandle::Kind::Texture2D;
    importDesc.Format = RGResourceFormat::RGBA8UNorm;
    importDesc.Width = kSize;
    importDesc.Height = kSize;
    blackboard.GBuffer.GBufferAlbedo =
        graph.ImportTextureHandle(ResourceNames::GBufferAlbedo, albedoTexture->GetRHIHandle(), importDesc);
    blackboard.GBuffer.GBufferNormal =
        graph.ImportTextureHandle(ResourceNames::GBufferNormal, normalTexture->GetRHIHandle(), importDesc);
    blackboard.GBuffer.GBufferEmissive =
        graph.ImportTextureHandle(ResourceNames::GBufferEmissive, emissiveTexture->GetRHIHandle(), importDesc);
    blackboard.GBuffer.Velocity =
        graph.ImportTextureHandle(ResourceNames::Velocity, velocityTexture->GetRHIHandle(), importDesc);
    blackboard.Scene.SceneDepth =
        graph.ImportTextureHandle(ResourceNames::SceneDepth, depthTexture->GetRHIHandle(), importDesc);

    graph.AddNode(deferredLighting);
    graph.SetFinalPass("DeferredLightingPass");
    graph.BuildFrameGraph();

    // The shadow placeholder arrays (compare-on family) + their raw aliases
    // (compare-off family, via CreateDepthArrayCompareOffViewHandle) bind
    // during Execute. Materialise them eagerly and pre-transition out of
    // UNDEFINED to the sampled layout their descriptors bake — the fog
    // tenant's placeholder recipe (their content is never read: shadows are
    // disabled in the Shadow UBO).
    const RHI::ResourceHandle csmPlaceholder = ShadowMap::GetCSMPlaceholderHandle();
    const RHI::ResourceHandle atlasPlaceholder = ShadowMap::GetAtlasPlaceholderHandle();
    const RHI::ResourceHandle csmRawAlias = ShadowMap::GetCSMRawPlaceholderHandle();
    ASSERT_TRUE(csmPlaceholder.IsValid());
    ASSERT_TRUE(atlasPlaceholder.IsValid());
    ASSERT_TRUE(csmRawAlias.IsValid())
        << "CreateDepthArrayCompareOffViewHandle must mint the raw alias on Vulkan";

    SubmitFrame(
        [&]()
        {
            // Author the G-buffer content the pass's blits copy: entity RT4
            // -> 42, depth -> 1.0 (float RTs -> 0, unread here).
            gbufferSamplingFB->ClearAllAttachments(glm::vec4(0.0f), 42);
            for (const RHI::ResourceHandle handle : { csmPlaceholder, atlasPlaceholder })
            {
                RHI::Barrier toSampled{};
                toSampled.Resource = handle;
                toSampled.Before = RHI::Access::Undefined;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            }
            graph.Execute();

            RHI::Barrier toSampled{};
            toSampled.Resource = sceneFramebuffer->GetColorAttachmentHandle(0);
            // The chain ends in the two transfer blits (depth + entity id),
            // but RT0's last access is the lighting draw's attachment write.
            toSampled.Before = RHI::Access::ColorAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });

            RHI::Barrier entityToSampled{};
            entityToSampled.Resource = sceneFramebuffer->GetColorAttachmentHandle(1);
            entityToSampled.Before = RHI::Access::TransferWrite; // the entity-ID blit
            entityToSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &entityToSampled, 1 });
        });

    EXPECT_TRUE(deferredLighting->GetTarget()) << "the pass early-returned";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "DeferredLighting resolve failure: pass='" << failure.PassName << "' reason='"
                      << failure.Reason << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    std::vector<u8> rendered;
    auto* vkScene = static_cast<VulkanFramebuffer*>(sceneFramebuffer.Raw());
    ASSERT_TRUE(vkScene->GetColorAttachmentImage(0)->GetData(rendered, 0));
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

    const auto px = [&](u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        return std::array<int, 3>{ rendered[i], rendered[i + 1], rendered[i + 2] };
    };

    // Far band (depth 1.0): the emissive passthrough, EXACT.
    const auto sky = px(64, 32);
    EXPECT_EQ(sky[0], 30) << "far-plane pixels must pass the emissive stand-in through";
    EXPECT_EQ(sky[1], 60);
    EXPECT_EQ(sky[2], 200);

    // Lit band: mirror the shader term-for-term at the probe pixel. World
    // position from the identity inverse-VP at uv=(64.5,96.5)/128, GL-shaped
    // depth 128/255; the light comes straight down the normal.
    {
        const glm::vec2 uv((64.0f + 0.5f) / kSize, (96.0f + 0.5f) / kSize);
        const f32 depth = 128.0f / 255.0f;
        const glm::vec3 worldPos(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, depth * 2.0f - 1.0f);
        const glm::vec3 albedo(204.0f / 255.0f, 51.0f / 255.0f, 51.0f / 255.0f);
        const glm::vec3 N(0.0f, 0.0f, 1.0f);
        const glm::vec3 V = glm::normalize(cameraData.Position - worldPos);
        const glm::vec3 L(0.0f, 0.0f, 1.0f);
        const glm::vec3 expected = MirrorDeferredLitPixel(albedo, N, V, L);

        const auto lit = px(64, 96);
        EXPECT_NEAR(lit[0], static_cast<int>(std::lround(std::clamp(expected.r, 0.0f, 1.0f) * 255.0f)), 4)
            << "lit-band red: ambient(0.03) + Lambert + GGX at N=L";
        EXPECT_NEAR(lit[1], static_cast<int>(std::lround(std::clamp(expected.g, 0.0f, 1.0f) * 255.0f)), 4);
        EXPECT_NEAR(lit[2], static_cast<int>(std::lround(std::clamp(expected.b, 0.0f, 1.0f) * 255.0f)), 4);
    }

    // The {1}-narrowed entity blit: RT1 carries the G-buffer's 42 clear
    // everywhere — and RT0 visibly does NOT (the narrow remapped the copy).
    std::vector<u8> entityBytes;
    ASSERT_TRUE(vkScene->GetColorAttachmentImage(1)->GetData(entityBytes, 0));
    ASSERT_EQ(entityBytes.size(), static_cast<sizet>(kSize) * kSize * 4);
    const auto* entityIds = reinterpret_cast<const i32*>(entityBytes.data());
    for (const auto& [x, y] : { std::pair<u32, u32>{ 0, 0 }, { 64, 64 }, { 127, 127 } })
    {
        EXPECT_EQ(entityIds[static_cast<sizet>(y) * kSize + x], 42)
            << "the narrowed colour blit must land the G-buffer entity clear in RT1 at (" << x << "," << y << ")";
    }

    // The pass lazily created the CSM/atlas placeholder array — a
    // process-static Ref that must die before this device tears down (the
    // fog tenant's discipline).
    vkDeviceWaitIdle(m_Device->GetDevice());
    ShadowMap::ShutdownPlaceholders();
    settings.Deferred.EnableLightProbes = prevProbes;
    Renderer3D::SetGlobalIBL(prevIrradiance, prevPrefilter, prevBrdfLut, prevEnvironment, prevIblIntensity);
}

#endif // OLO_WITH_VULKAN
