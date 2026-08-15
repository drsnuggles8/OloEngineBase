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

#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDraw.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"
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
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Passes/FluidIntermediatesPass.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"
#include "OloEngine/Renderer/Passes/GTAORenderPass.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Passes/DeferredOpaqueDecalPass.h"
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"
#include "OloEngine/Renderer/Passes/FluidCompositePass.h"
#include "OloEngine/Renderer/Passes/ForwardOverlayRenderPass.h"
#include "OloEngine/Renderer/Passes/MotionBlurRenderPass.h"
#include "OloEngine/Renderer/Passes/ParticleRenderPass.h"
#include "OloEngine/Renderer/Passes/ShaderDebugDrawPass.h"
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
#include "OloEngine/Renderer/PlanarReflection.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
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
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanOneShot.h"
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
#include <fstream>
#include <string>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <utility>
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
        // Wave C batch 2 failure-path net: a tenant that re-homed the
        // ShaderDebugDraw / particle-batch statics onto this device normally
        // shuts them down itself, but an ASSERT exit skips that — and a
        // Vulkan-currency static surviving past device teardown asserts in
        // the allocator (the MeshPrimitives lesson). Scope: when this test
        // flagged a re-home, or when no production GL renderer exists (an
        // isolated run — the statics can only be this suite's). NEVER
        // unconditionally mid-suite: that would strip the GL renderer's
        // live statics from tests that never touched them.
        if (m_ReinitShaderDebugDrawOnTearDown || !Renderer3D::IsInitialized())
        {
            if (ShaderDebugDraw::IsInitialised())
                ShaderDebugDraw::Shutdown();
        }
        if (m_ReinitParticleBatchRendererOnTearDown || !Renderer3D::IsInitialized())
        {
            ParticleBatchRenderer::Shutdown(); // safe on empty statics
        }
        // The fullscreen-triangle cache is a process STATIC now holding
        // Vulkan VMA buffers (this fixture is the first to route the real
        // MeshPrimitives triangle through the backend) — released here or
        // the allocator teardown asserts on the leak.
        MeshPrimitives::Shutdown();
        VulkanPipelineBuilder::Get().ReleaseAll();
        VulkanPipelineCache::Get().SaveAndDestroy();
        VulkanFrameArena::Get().ReleaseBuffers();
        VulkanResourceHeap::Get().Release();
        // #691 Phase 8: raw facade resources (CreateTexture2DHandle /
        // CreateFramebufferHandle) are OWNED by process-wide side registries.
        // Tenants normally retire them through DeleteTexture/DeleteFramebuffer
        // in their pass destructors, but an ASSERT exit skips that — and a
        // registry entry surviving past device teardown is a leaked VMA image
        // the allocator asserts on. Release BEFORE FlushAll so the dropped
        // Refs' reclaim entries are destroyed on the still-live device.
        VulkanRawTextureRegistry::Get().ReleaseAll();
        VulkanRawFramebufferRegistry::Get().ReleaseAll();
        VulkanDeferredReclaim::Get().FlushAll();
        if (m_Fence != VK_NULL_HANDLE)
            vkDestroyFence(m_Device->GetDevice(), m_Fence, nullptr);
        m_Device->Shutdown();
        // AFTER Shutdown, deliberately: vkDestroyDevice is when the object
        // tracker reports leaked handles, and the next SetUp resets the
        // counter — so reading it before teardown could never see the very
        // leak reports several tenants say they rely on (the occlusion-query
        // pool retirement note, for one).
        EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
            << "Zero validation errors (sync validation included in debug builds; "
               "device-teardown object-leak reports land here too)";
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
            // Wave C batch 2: tenants that re-home process-wide renderer
            // statics onto the Vulkan device (the ShaderDebugDraw channels,
            // the particle batch renderer) tear the GL-currency versions down
            // first and release their Vulkan versions before device teardown.
            // When the production GL renderer is still up (mid-suite runs —
            // Renderer3D::IsInitialized() survives across fixtures), re-init
            // them on the restored GL backend, or every later GL test that
            // relies on Renderer3D::Init's statics finds them missing (the
            // amendment (34) contamination class; ShaderDebugDrawVisualTest
            // and the particle visual tests run AFTER this suite).
            if (m_ReinitShaderDebugDrawOnTearDown && Renderer3D::IsInitialized())
                ShaderDebugDraw::Init();
            if (m_ReinitParticleBatchRendererOnTearDown && Renderer3D::IsInitialized())
                ParticleBatchRenderer::Init();
        }
        m_ReinitShaderDebugDrawOnTearDown = false;
        m_ReinitParticleBatchRendererOnTearDown = false;
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

        // ADR amendment (57) lists zero-stub among the instruments every wave
        // fixture carries, but this shared harness was omitting it — so nine
        // tenants could have had a facade entry point fall through to
        // Phase6Stub while still producing their pixels by other means. A
        // DELTA, not an absolute: the counter is cumulative across a fixture's
        // tenants (and historically the SSAO stub-floor tenant deliberately
        // accumulated hits before its Phase 8 promotion).
        const u32 stubsBefore =
            static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI()).GetPhase6StubHitCount();

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
            EXPECT_EQ(vkApi.GetPhase6StubHitCount(), stubsBefore)
                << finalPassName << ": the chain fell through to a Phase 6 stub";
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
    // Wave C batch 2 restore flags — see TearDown. A tenant that shuts down a
    // production (GL-currency) renderer static to re-home it on Vulkan sets
    // its flag IMMEDIATELY (before its own Init), so the restore happens even
    // when the tenant fails mid-way.
    bool m_ReinitShaderDebugDrawOnTearDown = false;
    bool m_ReinitParticleBatchRendererOnTearDown = false;
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
    // One 33-tile LUT step is 255/32 ≈ 8: the identity round trip through an
    // 8-bit strip loses up to a step to the intra-tile bilinear + inter-tile
    // z-mix quantization. The old floor of 2 was calibrated while the Vulkan
    // LUT upload was a stub — the pass detected the null LUT and PASSED THE
    // PATTERN THROUGH untouched, so the tolerance measured the disabled path,
    // not LUT sampling (#691 Phase 8: the raw-texture family made the LUT
    // real, and the measured max error is exactly one LUT quantum). Follow-up
    // recorded in the PR: capture the same tenant scene through the GL pass
    // and pin both backends to a shared bound.
    EXPECT_LE(maxDiff, 9u) << "identity LUT must stay within one 33-tile LUT quantum (255/32) of the pattern";
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
    // touched. It is a REGRESSION GUARD, not a live blocker: the froxel chain
    // compiles today. It exists because writing this tenant is what found the
    // Vulkan compute includer resolving relative includes against the fixed
    // shader root ("assets/shaders" + "../include/..." = assets/include/,
    // which does not exist) instead of the including FILE's directory — so
    // FroxelFogScatter.comp's load-bearing includes (FogCommon.glsl,
    // FogVolumeCommon.glsl, AtmosphereShading.glsl) never expanded and the
    // shader failed glslang with hundreds of undeclared identifiers. That is
    // fixed in VulkanComputeShader; every .comp that compiled BEFORE the fix
    // (HZB/GTAO/Denoise/AutoExposure) only included BindlessHeap.glsl, whose
    // entire content is OLO_BINDLESS-guarded — a silently-tolerated failed
    // include, which is why the gap stayed invisible until this pass. If this
    // ever skips again, the includer regressed.
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
                // DELIBERATELY no first-use transitions here. This tenant used
                // to hand-transition all four volumes out of UNDEFINED, which
                // silently supplied what production omitted: BindImageTexture
                // baked GENERAL into its descriptor without ever transitioning
                // the image, so every pass-owned storage volume reached its
                // first imageStore still in UNDEFINED. Because the test did
                // the backend's job, it could not fail. The backend now does
                // its own bind-time transitions, so leaving these out is what
                // makes the tenant exercise the production path — a regression
                // there resurfaces as the validation errors TearDown asserts on.
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
// FluidIntermediates: raw-target bring-up + the graph-level gating contracts.
// The three engine gaps the floor predecessor of this tenant documented are
// all CLOSED now:
//
//   1. (CLOSED in this batch — #691 Phase 8) The raw-handle resource family
//      (CreateTexture2DHandle, CreateFramebufferHandle,
//      AttachFramebufferColor/DepthTexture, SetFramebufferDrawAttachments,
//      IsFramebufferComplete, SetTextureFilter/Wrap,
//      DeleteTexture/DeleteFramebuffer) has a real Vulkan arm: raw handles
//      are backed by real VulkanTexture2D/VulkanFramebuffer objects owned by
//      the VulkanRaw{Texture,Framebuffer}Registry side tables, so
//      CreateTargets() builds the splat FBOs for real below.
//   2. (CLOSED earlier) FluidSmooth.comp could not compile: the Vulkan
//      compute includer resolved "../include/FluidRenderCommon.glsl" against
//      the shader root instead of the including file's directory. Fixed in
//      VulkanComputeShader; see the VolumetricFog tenant's compile gate.
//   3. (CLOSED earlier) vkCreateShaderModule rejected the
//      FluidDepthSplat/FluidThickness fragment SPIR-V, because glslang lowers
//      `discard` to OpDemoteToHelperInvocation under vulkan1.4 and the device
//      did not enable shaderDemoteToHelperInvocation (VUID-
//      VkShaderModuleCreateInfo-pCode-08740). VulkanDevice now enables it.
//
// What this tenant pins:
//   a. Init() + SetupFramebuffer() walk the raw family for REAL with zero
//      stub hits: 4 raw textures (R32F pair, RG16F thickness, the D32Float
//      splat-z that must come out with a depth aspect), 2 raw FBOs, attach +
//      draw-attachment selection + the IsFramebufferComplete gate. Live
//      target ids afterwards ARE the completeness signal — CreateTargets
//      releases everything when the gate says no — and IsReadyForExecution()
//      confirms the shader/UBO/VAO half.
//   b. The no-draw early-out through the REAL graph: with an empty draw list
//      Setup declares NOTHING (the issue #530 fingerprint gate) and Execute
//      returns without running — graph green, zero stubs.
//   c. The one-shot draw-list contract: a submitted draw (real particle
//      SSBOs) is CONSUMED by the very next Execute even when the guard chain
//      then rejects the frame — here the missing scene-depth attachment (the
//      blackboard carries none), which gates AFTER the target guards but
//      BEFORE the splat body records anything — a skipped frame can never
//      replay stale draws.
// The full splat + smooth EXECUTE body (two instanced draw loops + the
// compute ping-pong into the raw targets, driven by a scene-depth import) is
// the remaining promotion for this tenant.
// =============================================================================
TEST_F(VulkanPassSuite, FluidIntermediatesBuildsRawTargetsAndPinsTheNoDrawEarlyOutThroughTheGraph)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto fluid = Ref<FluidIntermediatesPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    fluid->Init(initSpec);                 // splat/thickness shaders + FluidSmooth.comp + UBO + splat VAO
    fluid->SetupFramebuffer(kSize, kSize); // CreateTargets: the raw texture/FBO family, for real (gap 1 closed)
    fluid->SetEnabled(true);

    ASSERT_TRUE(fluid->IsReadyForExecution())
        << "fluid shaders/UBO/VAO must come up on Vulkan (gaps 2+3 stay closed)";
    EXPECT_TRUE(fluid->GetSmoothedDepthTextureID().IsValid())
        << "raw R32F depth target must survive the IsFramebufferComplete gate (raw family bring-up)";
    EXPECT_TRUE(fluid->GetThicknessTextureID().IsValid())
        << "raw RG16F thickness target must survive the IsFramebufferComplete gate (raw family bring-up)";
    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "raw target creation (textures, FBOs, attach, filter/wrap, completeness) must not fall through "
           "to a Phase 6 stub";

    // (b) empty draw list: Setup declares nothing, Execute early-returns.
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

    // (c) a real submitted draw is consumed even though the guard chain then
    // rejects the frame: targets and readiness pass now, so the gate that
    // fires is the unresolved scene-depth attachment — still before any
    // splat-body recording.
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
        EXPECT_FALSE(fluid->RanThisFrame()) << "without a scene-depth attachment the body must reject the frame";
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "the raw-family bring-up and both gated graph paths must record zero stub hits";
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
// SSAO: full tenant (#691 Phase 8 — promoted from the raw-texture stub floor
// the moment the raw-handle family gained its Vulkan arm, exactly as the
// floor's embedded instruction demanded). Init creates the 4x4 RG16F
// rotation-noise texture through that family for real — CreateTexture2DHandle
// + UploadTextureSubImage2D + SetTextureFilter + SetTextureWrap — inside a
// recording bracket, because the sub-image upload is the frame-command-buffer
// staged path (GL's command-ordered semantics; outside a bracket it
// warn-drops). The Nearest/Repeat the pass sets lands in the image-info
// registry, which IS what the heap-off BindTexture derives the bind-time
// sampler from — while the EXPLICIT RHI::SamplerDesc argument the pass also
// passes remains dropped on this route (heap OFF: BindTextureOrOffsetImpl's
// fallback discards it; heap ON: pipelines carry the single
// DefaultEmbeddedSampler). That per-binding embedded-sampler gap is
// unchanged by this promotion; here the two sources happen to agree.
//
// Chain contract (uniform inputs => analytic AO): depth is a constant ~0.502
// plane and the solid-zero normals octDecode to the +Z view normal, so every
// in-bounds spiral tap reconstructs to the SAME view depth as its center —
// toSample ⟂ normal, ndotv = 0, zero occlusion — and the raw pass must write
// AO = 1.0, which the bilateral blur of a constant field preserves. The
// half-res blur result is then CopyImageSubData'd into the imported RG16F AO
// output (format-matched to the blur attachment), whose pre-seeded ZEROS are
// what prove the copy landed. Instruments: exactly the 2 half-res draws,
// none dropped, zero resolve failures, zero stub hits end to end, zero
// validation errors in teardown.
// =============================================================================
TEST_F(VulkanPassSuite, SsaoRunsBothHalfResDrawsAndCopiesUnoccludedAOThroughTheGraph)
{
    constexpr u32 kSize = 128;
    constexpr u32 kHalf = kSize / 2;
    VulkanFrameArena::Get().BeginFrame(0);

    auto depthTexture = MakeSolidTexture(kSize, 128, 128, 128, 255);
    ASSERT_NE(depthTexture, nullptr);
    auto normalTexture = MakeSolidTexture(kSize, 0, 0, 0, 255);
    ASSERT_NE(normalTexture, nullptr);

    // Half-res RG16F AO output, zero-seeded: half-res because the pass copies
    // a kHalf x kHalf region (its production AOBuffer contract), RG16F
    // because vkCmdCopyImage needs size-compatible texels with the blur
    // attachment, and zeros so the readback proves the copy replaced them.
    Ref<Texture2D> aoOutput;
    {
        TextureSpecification aoSpec;
        aoSpec.Width = kHalf;
        aoSpec.Height = kHalf;
        aoSpec.Format = ImageFormat::RG16F;
        aoSpec.GenerateMips = false;
        aoOutput = Texture2D::Create(aoSpec);
        ASSERT_NE(aoOutput, nullptr);
        // Facade client contract for the 16F formats: f32 PER CHANNEL (the
        // GL driver converts; this backend converts CPU-side) — 8 bytes per
        // RG16F texel, not the native 4. Zeros are zeros in either width.
        std::vector<u8> zeros(static_cast<sizet>(kHalf) * kHalf * 2u * sizeof(f32), 0u);
        aoOutput->SetData(zeros.data(), static_cast<u32>(zeros.size()));
    }

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto ssao = Ref<SSAORenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    // Init inside a recording bracket — see the tenant header.
    SubmitFrame([&]()
                { ssao->Init(initSpec); });

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "the noise-import calls (CreateTexture2DHandle + UploadTextureSubImage2D + "
           "SetTextureFilter/Wrap) must not fall through to a stub anymore";
    ASSERT_TRUE(ssao->IsReadyForExecution())
        << "a valid raw noise handle + compiled SSAO shaders must gate execution ON";

    PostProcessSettings settings{};
    settings.SSAOEnabled = true;
    settings.ActiveAOTechnique = AOTechnique::SSAO;
    ssao->SetSettings(settings);
    SSAOUBOData ssaoData{};
    // A real projection pair: the shader reconstructs view positions through
    // u_InverseProjection and projects the AO radius through u_Projection.
    // (The analytic contract — zero occlusion on a constant-depth plane —
    // holds for any invertible projection; a genuine perspective keeps the
    // tenant on the production math.)
    ssaoData.Projection = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 1000.0f);
    ssaoData.InverseProjection = glm::inverse(ssaoData.Projection);
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

    RGResourceDesc aoDesc;
    aoDesc.Kind = RGResourceHandle::Kind::Texture2D;
    aoDesc.Format = RGResourceFormat::RG16Float;
    aoDesc.Width = kHalf;
    aoDesc.Height = kHalf;
    blackboard.AO.AOBuffer = graph.ImportTextureHandle(ResourceNames::AOBuffer, aoOutput->GetRHIHandle(), aoDesc);

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

    SubmitFrame(
        [&]()
        {
            graph.Execute();

            // TRANSFER_DST after the pass's final CopyImageSubData; GetData's
            // one-shot readback assumes the SHADER_READ_ONLY steady state
            // (the GTAO tenant's pattern).
            RHI::Barrier toSampled{};
            toSampled.Resource = aoOutput->GetRHIHandle();
            toSampled.Before = RHI::Access::TransferWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    EXPECT_TRUE(ssao->GetTarget()) << "ready SSAO must run through to the blur target";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "SSAO resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                      << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 2u)
        << "raw SSAO + bilateral blur — exactly the two half-res draws";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u) << "a draw dropped silently";
    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "the whole chain (noise bind + 2 draws + CopyImageSubData) must record without a stub";

    std::vector<u8> aoBytes;
    ASSERT_TRUE(aoOutput->GetData(aoBytes, 0)) << "AO readback failed";
    ASSERT_EQ(aoBytes.size(), static_cast<sizet>(kHalf) * kHalf * 4u); // RG16F: 4 bytes/texel
    const auto aoAt = [&](u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kHalf + x) * 4u;
        const u16 half = static_cast<u16>(aoBytes[i]) | static_cast<u16>(static_cast<u16>(aoBytes[i + 1u]) << 8u);
        return HalfToFloat(half);
    };
    // Uniform depth + camera-facing normals => zero occlusion: the copy must
    // have replaced the pre-seeded zeros with AO = 1.0 (half-exact). Sampled
    // on a grid, not just the centre — a blur cannot manufacture occlusion
    // out of a constant field, so any dip is a real chain break.
    for (u32 y = 8; y < kHalf; y += 16)
    {
        for (u32 x = 8; x < kHalf; x += 16)
        {
            const f32 ao = aoAt(x, y);
            EXPECT_GE(ao, 0.9f) << "unoccluded flat field must stay open at (" << x << "," << y << ")";
            EXPECT_LE(ao, 1.01f) << "AO is a [0,1] visibility at (" << x << "," << y << ")";
        }
    }
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
// The caller backing mirrors production's {R32Float, RG16Float} exactly. It
// used to stand RT0 up as RG32F because FramebufferTextureFormat had no
// single-channel float colour member and ToFramebufferFormat answered None for
// RGResourceFormat::R32Float — which silently dropped production's RT0 on the
// POOLED path (issue #772, fixed: the enum now carries R32F and both backends
// translate it). RT0 is a true R32F here so the tenant rides the same
// attachment format production does.
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
    // The production pair, verbatim: R32F depth + RG16F velocity (#772).
    outputSpec.Attachments = { FramebufferTextureFormat::R32F, FramebufferTextureFormat::RG16F };
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
    ASSERT_EQ(depthBytes.size(), static_cast<sizet>(kSize) * kSize * 4); // R32F
    std::vector<u8> velocityBytes;
    ASSERT_TRUE(vkOutput->GetColorAttachmentImage(1)->GetData(velocityBytes, 0));
    ASSERT_EQ(velocityBytes.size(), static_cast<sizet>(kSize) * kSize * 4); // RG16F

    const auto depthAt = [&](u32 x, u32 y) -> f32
    {
        const auto* floats = reinterpret_cast<const f32*>(depthBytes.data());
        return floats[static_cast<sizet>(y) * kSize + x];
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

// =============================================================================
// VirtualGeometry (#691 Wave C item 5): the MDI-count indirect-draw entry.
//
// The FULL VirtualGeometryPass is disproportionate headlessly: its cull/raster
// compute shaders drive ~15 bare uniforms through ComputeShader::Set* calls
// (no-ops on the SPIR-V route — they need the Wave B "bare uniforms -> UBO"
// migration first), VirtualMeshRegistry seeding needs cooked VirtualMeshAssets
// (the VirtualMeshBuilder cook, not reachable from a plain run), and the
// material loop rides CommandDispatch::UploadMaterialForDirectDraw (port-order
// item 10's machinery). So per the survey's fallback this tenant pins the
// INDIRECT-DRAW ENTRY itself — with the REAL VirtualMeshGBuffer.glsl and a
// hand-authored cluster set, which is strictly stronger than a V1 stand-in:
//
//   * vkCmdDrawIndexedIndirectCount from a hand-built 2-command buffer
//     (32-byte stride pass-through: command 1 reads wrong bytes if the stride
//     is dropped) + a VirtualDrawArgs count buffer;
//   * THE COUNT BUFFER DRIVES: chain 2 rewrites DrawCount 2 -> 1 with
//     maxDrawCount still 2, and the second cluster must vanish;
//   * per-command FirstIndex/BaseVertex land each cluster on its pooled slot
//     (the registry's segmentation contract);
//   * the SPIR-V DrawParameters capability: the shader requires
//     GL_ARB_shader_draw_parameters (gl_BaseInstanceARB -> v_DbgSlot), so
//     shaderc must emit the capability and the device must enable
//     shaderDrawParameters — module/pipeline creation fails validation
//     otherwise, and this suite's zero-error gate catches it;
//   * TextureBarrier() — the pass's phase-2 "draw depth, then sample it"
//     fence — lowers to scope-end + a conservative global barrier.
//
// The shader's debug SSBOs (33/38) and UBO 50 are bound with zeroed stand-ins
// (mode 0 = off); its VirtualDebugViz storage images (image units 0/1) stay
// unbound — never accessed under mode 0, the Wave B "content-safe, heap
// descriptors invisible to validation" precedent (no R32UI ImageFormat exists
// to stage a real one).
// =============================================================================
TEST_F(VulkanPassSuite, VirtualGeometryMdiCountDrawsHandAuthoredClusters)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    // --- the real MDI shader (DrawParameters capability gate) ---------------
    auto vgShader = Shader::Create("assets/shaders/VirtualMeshGBuffer.glsl");
    ASSERT_TRUE(vgShader);
    ASSERT_EQ(vgShader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
        << "VirtualMeshGBuffer.glsl must compile through shaderc (GL_ARB_shader_draw_parameters)";

    // --- hand-authored cluster set ------------------------------------------
    // Two 4-vertex quad clusters in the pooled-array shape: cluster 0 on the
    // left (NDC x -0.9..-0.1), cluster 1 on the right (0.1..0.9), both
    // y -0.4..0.4 at z 0.5. Identity camera => authored NDC.
    std::array<VirtualGpuVertex, 8> vertices{};
    const auto quad = [&](sizet base, f32 x0, f32 x1)
    {
        vertices[base + 0].PositionU = { x0, -0.4f, 0.5f, 0.0f };
        vertices[base + 1].PositionU = { x1, -0.4f, 0.5f, 1.0f };
        vertices[base + 2].PositionU = { x1, 0.4f, 0.5f, 1.0f };
        vertices[base + 3].PositionU = { x0, 0.4f, 0.5f, 0.0f };
        for (sizet i = 0; i < 4; ++i)
            vertices[base + i].NormalV = { 0.0f, 0.0f, 1.0f, 0.0f };
    };
    quad(0, -0.9f, -0.1f);
    quad(4, 0.1f, 0.9f);
    auto vertexSSBO = StorageBuffer::Create(static_cast<u32>(vertices.size() * sizeof(VirtualGpuVertex)),
                                            ShaderBindingLayout::SSBO_VIRTUAL_VERTICES);
    vertexSSBO->SetData(vertices.data(), static_cast<u32>(vertices.size() * sizeof(VirtualGpuVertex)));
    vertexSSBO->Bind();

    VirtualInstanceGpuRecord instance{};
    instance.EntityID = 7;
    auto instanceSSBO = StorageBuffer::Create(sizeof(VirtualInstanceGpuRecord),
                                              ShaderBindingLayout::SSBO_VIRTUAL_INSTANCES);
    instanceSSBO->SetData(&instance, sizeof(instance));
    instanceSSBO->Bind();

    // Debug stand-ins (read only under debug mode != 0, but the root layout
    // wants live addresses): one zeroed cluster record + two visible records.
    VirtualClusterGpuRecord zeroCluster{};
    auto dbgClusterSSBO = StorageBuffer::Create(sizeof(VirtualClusterGpuRecord),
                                                ShaderBindingLayout::SSBO_VIRTUAL_CLUSTERS);
    dbgClusterSSBO->SetData(&zeroCluster, sizeof(zeroCluster));
    dbgClusterSSBO->Bind();
    std::array<VirtualVisibleCluster, 2> zeroVisible{};
    auto dbgVisibleSSBO = StorageBuffer::Create(sizeof(zeroVisible), ShaderBindingLayout::SSBO_VIRTUAL_VISIBLE);
    dbgVisibleSSBO->SetData(zeroVisible.data(), sizeof(zeroVisible));
    dbgVisibleSSBO->Bind();

    // Pooled cluster-local index buffer: each cluster's 6 indices are LOCAL
    // (0..3); the command's BaseVertex lands them on the pooled slot.
    u32 indices[] = { 0u, 1u, 2u, 2u, 3u, 0u, 0u, 1u, 2u, 2u, 3u, 0u };
    auto indexBuffer = IndexBuffer::Create(indices, 12);
    auto vao = VertexArray::Create();
    vao->SetIndexBuffer(indexBuffer);

    // The registry's 32-byte command records (DrawElementsIndirectCommand +
    // 12 pad bytes — VkDrawIndexedIndirectCommand is the same 5-field prefix).
    struct MdiCommand
    {
        u32 IndexCount = 0;
        u32 InstanceCount = 0;
        u32 FirstIndex = 0;
        i32 BaseVertex = 0;
        u32 BaseInstance = 0;
        u32 _Pad[3] = { 0, 0, 0 };
    };
    static_assert(sizeof(MdiCommand) == 32, "the registry's MDI stride is 32 bytes");
    std::array<MdiCommand, 2> commands{};
    commands[0] = { 6u, 1u, 0u, 0, 0u, {} };
    commands[1] = { 6u, 1u, 6u, 4, 1u, {} }; // BaseInstance 1 -> v_DbgSlot record 1
    auto commandSSBO = StorageBuffer::Create(sizeof(commands), ShaderBindingLayout::SSBO_VIRTUAL_DRAW_COMMANDS);
    commandSSBO->SetData(commands.data(), sizeof(commands));
    commandSSBO->Bind();

    VirtualDrawArgs args{};
    args.DrawCount = 2;
    auto argsSSBO = StorageBuffer::Create(sizeof(VirtualDrawArgs), ShaderBindingLayout::SSBO_VIRTUAL_DRAW_ARGS);
    argsSSBO->SetData(&args, sizeof(args));
    argsSSBO->Bind();

    // --- every UBO the shader declares (the DRS lesson) ---------------------
    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    cameraData.Position = glm::vec3(0.0f, 0.0f, 1.0f);
    cameraData.PrevViewProjection = glm::mat4(1.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    MotionBlurUBOData mbData{};
    mbData.InverseViewProjection = glm::mat4(1.0f);
    mbData.PrevViewProjection = glm::mat4(1.0f);
    auto mbUbo = UniformBuffer::Create(sizeof(MotionBlurUBOData), 8);
    mbUbo->SetData(&mbData, sizeof(mbData));

    ShaderBindingLayout::PBRMaterialUBO material{};
    material.BaseColorFactor = glm::vec4(0.8f, 0.2f, 0.1f, 1.0f);
    material.EmissiveFactor = glm::vec4(0.0f);
    material.MetallicFactor = 0.0f;
    material.RoughnessFactor = 1.0f;
    material.NormalScale = 1.0f;
    material.OcclusionStrength = 1.0f;
    material.UseAlbedoMap = 0;
    material.UseNormalMap = 0;
    material.UseMetallicRoughnessMap = 0;
    material.UseAOMap = 0;
    material.UseEmissiveMap = 0;
    material.ApplyGammaCorrection = 0;
    material.EnableLightProbes = 0;
    auto materialUbo = UniformBuffer::Create(sizeof(ShaderBindingLayout::PBRMaterialUBO),
                                             ShaderBindingLayout::UBO_MATERIAL);
    materialUbo->SetData(&material, sizeof(material));

    const u32 drawInfo[4] = { 0u, 0u, 0u, 0u }; // instance 0, segment base 0
    auto drawInfoUbo = UniformBuffer::Create(16, ShaderBindingLayout::UBO_VIRTUAL_DRAW);
    drawInfoUbo->SetData(drawInfo, sizeof(drawInfo));

    const u32 debugInfo[4] = { 0u, 0u, 0u, 0u }; // debug mode OFF
    auto debugInfoUbo = UniformBuffer::Create(16, ShaderBindingLayout::UBO_VIRTUAL_DEBUG);
    debugInfoUbo->SetData(debugInfo, sizeof(debugInfo));

    auto whiteTexture = MakeSolidTexture(4, 255, 255, 255, 255);
    ASSERT_TRUE(whiteTexture);

    // --- G-Buffer-shaped MRT target -----------------------------------------
    FramebufferSpecification gbufferSpec;
    gbufferSpec.Width = kSize;
    gbufferSpec.Height = kSize;
    gbufferSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RGBA16F,
                                FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::RG16F,
                                FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::Depth };
    Ref<Framebuffer> gbufferFB = Framebuffer::Create(gbufferSpec);
    ASSERT_TRUE(gbufferFB);

    const auto runChain = [&](u32 gpuDrawCount)
    {
        args.DrawCount = gpuDrawCount;
        argsSSBO->SetData(&args, sizeof(args));

        SubmitFrame(
            [&]()
            {
                gbufferFB->Bind();
                RenderCommand::SetViewport(0, 0, kSize, kSize);
                RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
                RenderCommand::Clear(); // folds into the first draw's loadOp (+ depth -> 1.0)
                RenderCommand::SetDepthTest(true);
                RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
                RenderCommand::SetDepthMask(true);
                RenderCommand::SetBlendState(false);
                RenderCommand::DisableCulling();

                vgShader->Bind();
                for (const u32 slot : { 0u, 1u, 2u, 4u, 5u })
                    RenderCommand::BindTexture(slot, whiteTexture->GetRHIHandle());

                // maxDrawCount stays 2 in BOTH chains: the count BUFFER must
                // drive (chain 2's contract).
                RenderCommand::MultiDrawElementsIndirectCountRaw(
                    vao->GetRHIHandle(), commandSSBO->GetRHIHandle(), 0u,
                    argsSSBO->GetRHIHandle(), 0u, 2u, 32u);

                // The pass's phase-2 fence: framebuffer writes -> texture
                // fetches. Scope-end + conservative global barrier.
                RenderCommand::TextureBarrier();

                for (const u32 attachment : { 0u, 4u })
                {
                    RHI::Barrier toSampled{};
                    toSampled.Resource = gbufferFB->GetColorAttachmentHandle(attachment);
                    toSampled.Before = RHI::Access::ColorAttachmentWrite;
                    toSampled.After = RHI::Access::ShaderSampleRead;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                }
            });

        EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 1u) << "the one MDI-count draw";
        EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);
    };

    auto* vkGbuffer = static_cast<VulkanFramebuffer*>(gbufferFB.Raw());
    const auto albedoAt = [&](const std::vector<u8>& rgba, u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        return std::array<int, 3>{ rgba[i], rgba[i + 1], rgba[i + 2] };
    };

    // --- chain 1: count = 2 -> both clusters land ---------------------------
    runChain(2u);
    {
        std::vector<u8> albedo;
        std::vector<u8> entityBytes;
        ASSERT_TRUE(vkGbuffer->GetColorAttachmentImage(0)->GetData(albedo, 0));
        ASSERT_TRUE(vkGbuffer->GetColorAttachmentImage(4)->GetData(entityBytes, 0));
        const auto* entities = reinterpret_cast<const i32*>(entityBytes.data());

        const auto left = albedoAt(albedo, 32, 64);
        EXPECT_NEAR(left[0], 204, 3) << "cluster 0 albedo (command 0: FirstIndex 0, BaseVertex 0)";
        EXPECT_NEAR(left[1], 51, 3);
        EXPECT_NEAR(left[2], 26, 3);
        const auto right = albedoAt(albedo, 96, 64);
        EXPECT_NEAR(right[0], 204, 3) << "cluster 1 albedo (command 1 via the 32-byte stride)";
        EXPECT_NEAR(right[1], 51, 3);
        EXPECT_NEAR(right[2], 26, 3);
        const auto background = albedoAt(albedo, 64, 5);
        EXPECT_EQ(background[0], 0) << "uncovered pixels keep the clear";

        EXPECT_EQ(entities[static_cast<sizet>(64) * kSize + 32], 7) << "cluster 0 entity id";
        EXPECT_EQ(entities[static_cast<sizet>(64) * kSize + 96], 7) << "cluster 1 entity id";
        EXPECT_EQ(entities[static_cast<sizet>(5) * kSize + 64], 0) << "uncovered entity id keeps the clear";
    }

    // --- chain 2: count = 1 (maxDrawCount still 2) -> cluster 1 vanishes ----
    runChain(1u);
    {
        std::vector<u8> albedo;
        ASSERT_TRUE(vkGbuffer->GetColorAttachmentImage(0)->GetData(albedo, 0));
        const auto left = albedoAt(albedo, 32, 64);
        EXPECT_NEAR(left[0], 204, 3) << "cluster 0 must survive the count cut";
        const auto right = albedoAt(albedo, 96, 64);
        EXPECT_EQ(right[0], 0) << "the COUNT BUFFER (not maxDrawCount) must bound the draw";
        EXPECT_EQ(right[1], 0);
        EXPECT_EQ(right[2], 0);
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "MDI-count + TextureBarrier must ride real implementations, not stubs";
}

namespace
{
    // Directional hue test (the GL ShaderDebugDrawVisualTest's IsHue, floors
    // kept): per-channel thresholds conflate hues sharing a dominant channel,
    // so compare DIRECTION in RGB space instead.
    [[nodiscard]] bool PixelCarriesHue(const u8* px, const glm::vec3& hue)
    {
        const glm::vec3 pixel(static_cast<f32>(px[0]), static_cast<f32>(px[1]), static_cast<f32>(px[2]));
        const f32 brightest = std::max({ pixel.r, pixel.g, pixel.b });
        const f32 darkest = std::min({ pixel.r, pixel.g, pixel.b });
        if (brightest < 60.0f || (brightest - darkest) < 40.0f)
            return false;
        const f32 length = glm::length(pixel);
        if (length < 1e-3f)
            return false;
        return glm::dot(pixel / length, glm::normalize(hue)) > 0.96f;
    }

    [[nodiscard]] u32 CountHuePixels(const std::vector<u8>& rgba, u32 size, const glm::vec3& hue)
    {
        u32 count = 0;
        for (sizet i = 0; i + 3 < rgba.size(); i += 4)
        {
            if (PixelCarriesHue(&rgba[i], hue))
                ++count;
        }
        (void)size;
        return count;
    }
} // namespace

// =============================================================================
// ShaderDebugDraw (#691 Wave C item 6): the GPU-pushable debug channels'
// DRAW side + the stats readback ring, through the REAL pass in the graph.
//
// One line + one AABB go through the CPU appenders into the SAME channel
// SSBOs the GLSL push helpers append to; BeginFrame uploads them; the pass
// then issues one DrawArraysIndirect PER CHANNEL (7 draws — the channel's
// own 32-byte header IS the DrawArraysIndirectCommand), narrowed to colour 0
// with depth-test-no-write against the producer's scene. Pinned:
//   * vkCmdDrawIndirect from a resolved StorageBuffer handle (the channels
//     now carry INDIRECT_BUFFER usage);
//   * the primitives REACH pixels (directional hue counts — red line,
//     yellow AABB — over the black producer pattern);
//   * the readback ring: StageStatsForReadback mints DeviceToHost staging
//     buffers (CreateBufferHandle + AllocateBufferStorage) and records
//     vkCmdCopyBuffer header copies (CopyBufferSubData) INSIDE the pass;
//     next frame's ReadbackStats reads the persistent mappings
//     (ReadBufferSubData) — the A7 frames-in-flight discipline (this frame
//     copies, the NEXT frame reads after the fence);
//   * the empty channels draw too (instanceCount 0 costs nothing): 8
//     prepared draws (producer + 7), zero dropped, ZERO stub hits across
//     the whole tenant — the ring and the indirect draw must be real.
// =============================================================================
TEST_F(VulkanPassSuite, ShaderDebugDrawIndirectDrawsChannelsAndReadsBackStats)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

    // Mid-suite the channels already exist with GL currency (Renderer3D::Init
    // owns them): tear that version down and flag TearDown to re-init it on
    // the restored GL backend — the amendment (34) two-way discipline. (The
    // GL objects' dtors are direct GL calls, which is exactly why this only
    // happens when the production renderer is up: with it up, glad is loaded
    // and a context exists.) The stub baseline is taken AFTER this teardown:
    // its DeleteBuffer calls route GL-currency staging handles into the
    // Vulkan raw-registry guard, which counts them as stubs by design.
    if (ShaderDebugDraw::IsInitialised())
    {
        m_ReinitShaderDebugDrawOnTearDown = true;
        ShaderDebugDraw::Shutdown();
    }
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto patternTexture = MakeSolidTexture(kSize, 0, 0, 0, 255); // black: hue floors never match it
    ASSERT_NE(patternTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    // Channels + params UBO come up on the Vulkan backend (device-lifetime
    // statics — Shutdown() below releases them before device teardown).
    ShaderDebugDraw::Init();
    ShaderDebugDraw::SetEnabled(true);
    ShaderDebugDraw::SetLineWidth(4.0f);

    // World == NDC (identity VP). z inside [0, 1] — Vulkan clips z outside.
    ShaderDebugDraw::DrawLine(glm::vec3(-0.8f, 0.0f, 0.5f), glm::vec3(0.8f, 0.0f, 0.5f),
                              glm::vec3(1.0f, 0.0f, 0.0f));
    ShaderDebugDraw::DrawAABB(glm::vec3(-0.5f, -0.5f, 0.1f), glm::vec3(0.5f, 0.5f, 0.9f),
                              glm::vec3(1.0f, 1.0f, 0.0f));
    ShaderDebugDraw::BeginFrame(); // uploads the CPU pushes into the channels

    auto debugPass = Ref<ShaderDebugDrawPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    debugPass->Init(initSpec);
    ASSERT_TRUE(debugPass->IsReadyForExecution())
        << "DebugDrawPrimitives.glsl must compile through shaderc (V12: no vertex buffer)";
    debugPass->SetEnabled(true);
    debugPass->SetCameraState(glm::mat4(1.0f), glm::mat4(1.0f));

    // Scene FB with depth: the pass depth-TESTS (LessOrEqual, no write)
    // against the producer's cleared 1.0.
    FramebufferSpecification sceneSpec;
    sceneSpec.Width = kSize;
    sceneSpec.Height = kSize;
    sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
    ASSERT_TRUE(sceneFramebuffer);

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

    auto producer = Ref<PatternProducerPass>::Create(
        patternTexture, blitShader, std::string(ResourceNames::SceneColor),
        std::string(ResourceNames::SceneColorTexture),
        [](FrameBlackboard& board) -> RGFramebufferHandle&
        { return board.Scene.SceneColor; });
    producer->ClearTargetFirst = true; // depth -> 1.0 at scope open

    graph.AddNode(producer);
    graph.AddNode(debugPass);
    graph.SetFinalPass("ShaderDebugDrawPass");
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
    EXPECT_TRUE(debugPass->GetTarget()) << "the pass early-returned";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "ShaderDebugDraw resolve failure: pass='" << failure.PassName << "' reason='"
                      << failure.Reason << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 8u)
        << "producer + one indirect draw per channel (empty channels draw with instanceCount 0)";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    // --- pixels: the primitives must reach the viewport ---------------------
    std::vector<u8> rendered;
    auto* vkScene = static_cast<VulkanFramebuffer*>(sceneFramebuffer.Raw());
    ASSERT_TRUE(vkScene->GetColorAttachmentImage(0)->GetData(rendered, 0));
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

    const u32 redCount = CountHuePixels(rendered, kSize, glm::vec3(1.0f, 0.0f, 0.0f));
    const u32 yellowCount = CountHuePixels(rendered, kSize, glm::vec3(1.0f, 1.0f, 0.0f));
    // The line spans ~102 px at width 4 (~400 px); the AABB's on-screen edges
    // cover well over 500. Floors sit far below so anti-aliasing/overlap
    // slack cannot flake, far above noise (the pattern is black).
    EXPECT_GE(redCount, 100u) << "the pushed line must reach the viewport";
    EXPECT_GE(yellowCount, 150u) << "the pushed AABB must reach the viewport";

    // --- the stats ring: copied THIS frame, read NEXT frame -----------------
    // The pass staged the seven 32-byte header copies into DeviceToHost
    // buffers during Execute; the submit fence has signalled, so BeginFrame's
    // ReadbackStats memcpy off the persistent mappings sees them.
    ShaderDebugDraw::BeginFrame();
    const auto& stats = ShaderDebugDraw::GetStats();
    EXPECT_TRUE(stats.StatsValid);
    const auto channelIndex = [](ShaderDebugDrawPrimitive primitive)
    { return static_cast<sizet>(std::to_underlying(primitive)); };
    const auto& lineStats = stats.Channels[channelIndex(ShaderDebugDrawPrimitive::Line)];
    EXPECT_EQ(lineStats.Drawn, 1u) << "the line channel header's InstanceCount round-tripped the ring";
    EXPECT_EQ(lineStats.Requested, 1u);
    EXPECT_EQ(lineStats.CpuPushes, 1u);
    const auto& aabbStats = stats.Channels[channelIndex(ShaderDebugDrawPrimitive::AABB)];
    EXPECT_EQ(aabbStats.Drawn, 1u);
    EXPECT_EQ(aabbStats.Requested, 1u);
    for (const auto primitive : { ShaderDebugDrawPrimitive::Circle, ShaderDebugDrawPrimitive::Rectangle,
                                  ShaderDebugDrawPrimitive::Box, ShaderDebugDrawPrimitive::Cone,
                                  ShaderDebugDrawPrimitive::Sphere })
    {
        EXPECT_EQ(stats.Channels[channelIndex(primitive)].Drawn, 0u)
            << "untouched channel " << static_cast<int>(std::to_underlying(primitive)) << " must read back empty";
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "DrawArraysIndirect + the CreateBufferHandle/AllocateBufferStorage/CopyBufferSubData/"
           "ReadBufferSubData ring must be real, not stubs";

    // Device-lifetime statics die before the fixture tears the device down
    // (the fog tenant's placeholder discipline). Shutdown() also routes the
    // staging buffers through DeleteBuffer -> deferred reclaim.
    vkDeviceWaitIdle(m_Device->GetDevice());
    ShaderDebugDraw::Shutdown();
}

// =============================================================================
// Foliage (#691 Wave C item 7, first half): the V8 TWO-STREAM instance pull.
//
// FoliageRenderer::Render itself cannot run headless — it dereferences
// Renderer3D statics that only Renderer3D::Init creates (GetFoliageUBO /
// GetModelInstanceBuffer are null Refs here), so this tenant drives the EXACT
// draw shape the renderer emits, byte-for-byte: the same VAO construction
// (BuildQuadGeometry's 20-byte quad stream 0 + UploadInstances'
// AddInstanceBuffer 48-byte stream 1), the REAL Foliage_Instance.glsl, and
// DrawIndexedInstanced(vao, 6, 3). On Vulkan the two streams ride the
// reserved pull pair — stream 0 on SSBO 57 by gl_VertexIndex, stream 1 on
// SSBO 63 by gl_InstanceIndex (A3's "bones are just the first tenant") — so
// three instances at known positions/tints pin per-vertex AND per-instance
// lanes at once: any stride/offset slip moves or recolours a card.
//
// Deterministic shading: zero-intensity light (lightDir must still be
// non-degenerate — normalize(vec3(0)) is NaN), wind UBO zeroed (windEnabled
// false + legacy strength 0), distance fade far -> every card is exactly
// ambient = 0.3 * tint * white.
//
// The other three V8 consumers (Foliage_Depth / _Instance_GBuffer /
// _Impostor) carry byte-identical pull branches; their PSO-level exercise
// needs depth-only / G-Buffer / impostor-atlas scaffolding better paid for
// by items 10-11, so this tenant pins them at compile level (shaderc Ready).
// =============================================================================
TEST_F(VulkanPassSuite, FoliageInstancePullDrawsThreeTintedCards)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto foliageShader = Shader::Create("assets/shaders/Foliage_Instance.glsl");
    ASSERT_TRUE(foliageShader);
    ASSERT_EQ(foliageShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    for (const char* sibling : { "assets/shaders/Foliage_Depth.glsl", "assets/shaders/Foliage_Instance_GBuffer.glsl",
                                 "assets/shaders/Foliage_Impostor.glsl" })
    {
        auto shader = Shader::Create(sibling);
        ASSERT_TRUE(shader);
        EXPECT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
            << sibling << " must compile through shaderc (V8 pull branch)";
    }

    // --- the FoliageRenderer VAO shape, verbatim ----------------------------
    const f32 quadVertices[] = {
        -0.5f,
        0.0f,
        0.0f,
        0.0f,
        0.0f, // bottom-left
        0.5f,
        0.0f,
        0.0f,
        1.0f,
        0.0f, // bottom-right
        0.5f,
        1.0f,
        0.0f,
        1.0f,
        1.0f, // top-right
        -0.5f,
        1.0f,
        0.0f,
        0.0f,
        1.0f, // top-left
    };
    u32 quadIndices[] = { 0u, 1u, 2u, 2u, 3u, 0u };
    auto vao = VertexArray::Create();
    auto quadVB = VertexBuffer::Create(quadVertices, sizeof(quadVertices));
    quadVB->SetLayout({ { ShaderDataType::Float3, "a_Position" }, { ShaderDataType::Float2, "a_TexCoord" } });
    vao->AddVertexBuffer(quadVB);
    vao->SetIndexBuffer(IndexBuffer::Create(quadIndices, 6));

    // Three instances: red / green / blue cards at x -0.6 / 0 / +0.6, scale
    // 0.5, height 1, fade 1, cutoff 0.5. Identity VP + identity u_Model =>
    // card i spans x = pos.x +- 0.25, y = pos.y .. pos.y + 0.5 in NDC.
    struct FoliageInstance
    {
        glm::vec4 PositionScale;
        glm::vec4 RotationHeight;
        glm::vec4 ColorAlpha;
    };
    static_assert(sizeof(FoliageInstance) == 48, "V8 instance stride is 48 bytes");
    const std::array<FoliageInstance, 3> instances{
        FoliageInstance{ { -0.6f, -0.25f, 0.5f, 0.5f }, { 0.0f, 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.5f } },
        FoliageInstance{ { 0.0f, -0.25f, 0.5f, 0.5f }, { 0.0f, 1.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.5f } },
        FoliageInstance{ { 0.6f, -0.25f, 0.5f, 0.5f }, { 0.0f, 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.5f } },
    };
    auto instanceVB = VertexBuffer::Create(static_cast<u32>(sizeof(instances)));
    instanceVB->SetLayout({ { ShaderDataType::Float4, "a_PositionScale" },
                            { ShaderDataType::Float4, "a_RotationHeight" },
                            { ShaderDataType::Float4, "a_ColorAlpha" } });
    instanceVB->SetData({ instances.data(), static_cast<u32>(sizeof(instances)) });
    vao->AddInstanceBuffer(instanceVB);

    // --- every binding the shader declares ----------------------------------
    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    cameraData.Position = glm::vec3(0.0f);
    cameraData.PrevViewProjection = glm::mat4(1.0f);
    cameraData.RenderOrigin = glm::vec3(0.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    // The foliage fragment's own five-vec4 light block (binding 5): zero
    // intensity, but a NON-degenerate direction — normalize(-direction) of a
    // zero vector is NaN and would poison litColor through 0 * NaN.
    struct FoliageLightBlock
    {
        i32 NumLights = 0;
        i32 _pad0 = 0;
        i32 _pad1 = 0;
        i32 _pad2 = 0;
        glm::vec4 Position{ 0.0f };
        glm::vec4 Direction{ 0.0f, -1.0f, 0.0f, 0.0f };
        glm::vec4 ColorIntensity{ 1.0f, 1.0f, 1.0f, 0.0f }; // w = intensity 0
        glm::vec4 Params{ 0.0f };
        glm::vec4 Params2{ 0.0f };
    } lightBlock;
    auto lightUbo = UniformBuffer::Create(sizeof(FoliageLightBlock), 5);
    lightUbo->SetData(&lightBlock, sizeof(lightBlock));

    ShaderBindingLayout::FoliageUBO foliageData{};
    foliageData.Time = 0.0f;
    foliageData.WindStrength = 0.0f; // legacy sine path contributes zero displacement
    foliageData.WindSpeed = 0.0f;
    foliageData.ViewDistance = 100.0f; // cards sit ~0.7 from the origin camera
    foliageData.FadeStart = 50.0f;     // => fadeFactor exactly 1
    foliageData.AlphaCutoff = 0.5f;
    foliageData.BaseColor = glm::vec4(0.0f);
    auto foliageUbo = UniformBuffer::Create(ShaderBindingLayout::FoliageUBO::GetSize(),
                                            ShaderBindingLayout::UBO_FOLIAGE);
    foliageUbo->SetData(&foliageData, ShaderBindingLayout::FoliageUBO::GetSize());

    // WindSampling's UBO (uniform binding 15 — disjoint from the SSBO 15
    // below, amendment (29)): all zeros => windEnabled() false.
    const std::array<glm::vec4, 4> windZeros{};
    auto windUbo = UniformBuffer::Create(sizeof(windZeros), 15);
    windUbo->SetData(windZeros.data(), sizeof(windZeros));

    // InstanceBlock_Vertex's SSBO 15: the shader indexes it by
    // gl_InstanceIndex (the production path uploads ONE entry and relies on
    // instance 0 — the impostor-card doc's known quirk), so give it three
    // identity entries to keep every read in bounds and deterministic.
    const std::array<InstanceData, 3> modelInstances{};
    auto modelSSBO = StorageBuffer::Create(sizeof(modelInstances), 15);
    modelSSBO->SetData(modelInstances.data(), sizeof(modelInstances));
    modelSSBO->Bind();

    auto whiteTexture = MakeSolidTexture(4, 255, 255, 255, 255);
    ASSERT_TRUE(whiteTexture);
    // WindSampling's sampler3D u_WindField (binding 29): never sampled with
    // the zeroed UBO, but stage a REAL 3D descriptor so the baked slot's
    // dimensionality matches the declaration.
    Texture3DSpecification windFieldSpec;
    windFieldSpec.Width = 2;
    windFieldSpec.Height = 2;
    windFieldSpec.Depth = 2;
    windFieldSpec.Format = Texture3DFormat::RGBA8;
    auto windField = Texture3D::Create(windFieldSpec);
    ASSERT_TRUE(windField);

    FramebufferSpecification sceneSpec;
    sceneSpec.Width = kSize;
    sceneSpec.Height = kSize;
    sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
    ASSERT_TRUE(sceneFramebuffer);

    SubmitFrame(
        [&]()
        {
            sceneFramebuffer->Bind();
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();
            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
            RenderCommand::SetDepthMask(true);
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();

            foliageShader->Bind();
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_DIFFUSE, whiteTexture->GetRHIHandle());
            RenderCommand::BindTexture(29, windField->GetRHIHandle());

            vao->Bind();
            RenderCommand::DrawIndexedInstanced(vao, 6, 3);

            RHI::Barrier toSampled{};
            toSampled.Resource = sceneFramebuffer->GetColorAttachmentHandle(0);
            toSampled.Before = RHI::Access::ColorAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 1u);
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

    // ambient = 0.3 * tint * white => 76 on the tinted channel. Cards span
    // y NDC -0.25..0.25 (row 64 covered under either y orientation); centres
    // x = 25 / 64 / 103.
    const auto red = px(25, 64);
    EXPECT_NEAR(red[0], 76, 3) << "instance 0 tint (stream-1 lane 8..10 at instance stride 12 floats)";
    EXPECT_NEAR(red[1], 0, 3);
    EXPECT_NEAR(red[2], 0, 3);
    const auto green = px(64, 64);
    EXPECT_NEAR(green[0], 0, 3);
    EXPECT_NEAR(green[1], 76, 3) << "instance 1 tint";
    EXPECT_NEAR(green[2], 0, 3);
    const auto blue = px(103, 64);
    EXPECT_NEAR(blue[0], 0, 3);
    EXPECT_NEAR(blue[1], 0, 3);
    EXPECT_NEAR(blue[2], 76, 3) << "instance 2 tint";
    const auto background = px(5, 120);
    EXPECT_EQ(background[0], 0) << "uncovered pixels keep the clear";
    EXPECT_EQ(background[1], 0);
    EXPECT_EQ(background[2], 0);

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore);
}

// =============================================================================
// ForwardOverlay (#691 Wave C item 7, second half): the empty-bucket floor.
//
// The pass is pure bucket replay: its Setup declares nothing when the bucket
// is empty (so the node is unreachable and culled) and its Execute would
// re-dispatch recorded CommandPackets — port-order item 10's machinery
// (CommandDispatch's per-packet draw path, the V1 mesh workhorse, material
// upload). A populated replay is deferred there; what the pass OWNS in the
// graph today is exactly this floor: registered with an empty bucket in
// Deferred mode it must declare nothing, execute nothing, and leave the
// producer's scene untouched — no stubs, no resolve failures. Its narrowing/
// restore half (SetFramebufferDrawAttachments {0,1,2} + RestoreAll) is the
// same mechanism the DeferredLighting and ShaderDebugDraw tenants already
// pin with live draws.
// =============================================================================
TEST_F(VulkanPassSuite, ForwardOverlayEmptyBucketFloorLeavesTheSceneUntouched)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    // Setup gates on the Deferred path (Forward routes overlay draws through
    // SceneRenderPass) — force it and restore.
    auto& settings = Renderer3D::GetRendererSettings();
    const RenderingPath prevPath = settings.Path;
    settings.Path = RenderingPath::Deferred;

    auto patternTexture = MakeSolidTexture(kSize, 200, 40, 90, 255);
    ASSERT_NE(patternTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    FramebufferSpecification sceneSpec;
    sceneSpec.Width = kSize;
    sceneSpec.Height = kSize;
    sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
    ASSERT_TRUE(sceneFramebuffer);

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

    auto producer = Ref<PatternProducerPass>::Create(
        patternTexture, blitShader, std::string(ResourceNames::SceneColor),
        std::string(ResourceNames::SceneColorTexture),
        [](FrameBlackboard& board) -> RGFramebufferHandle&
        { return board.Scene.SceneColor; });

    auto overlay = Ref<ForwardOverlayRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    overlay->Init(initSpec);
    // No commands are submitted to the pass's bucket: the floor's premise.

    graph.AddNode(producer);
    graph.AddNode(overlay);
    // The producer is the final pass: the undeclared overlay node has no
    // edges to reach it by.
    graph.SetFinalPass("TestPatternProducer");
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
    EXPECT_FALSE(overlay->GetTarget()) << "an empty-bucket overlay must not resolve a target";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "ForwardOverlay resolve failure: pass='" << failure.PassName << "' reason='"
                      << failure.Reason << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 1u) << "exactly the producer's draw";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    std::vector<u8> rendered;
    auto* vkScene = static_cast<VulkanFramebuffer*>(sceneFramebuffer.Raw());
    ASSERT_TRUE(vkScene->GetColorAttachmentImage(0)->GetData(rendered, 0));
    const sizet centre = ((static_cast<sizet>(64) * kSize) + 64) * 4;
    EXPECT_EQ(rendered[centre + 0], 200) << "the floored overlay must leave the scene untouched";
    EXPECT_EQ(rendered[centre + 1], 40);
    EXPECT_EQ(rendered[centre + 2], 90);

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore);
    settings.Path = prevPath;
}

// =============================================================================
// Particle (#691 Wave C item 8), three chains:
//
// (a) CPU CLASSIC through the REAL ParticleRenderPass in the graph: the pass
//     binds the scene FB, sets the LEqual/no-write depth + per-attachment
//     alpha blend state, and runs the render callback — which drives the REAL
//     ParticleBatchRenderer statics end to end (BeginBatch camera, Submit x2,
//     SubmitTrailQuad, EndBatch -> Flush + FlushTrails). On Vulkan that is
//     the V5+V6 two-stream pull (quad @57 by gl_VertexIndex, 96-byte
//     ParticleInstance @63 by gl_InstanceIndex, EntityID int-lane bitcast)
//     for the billboards and the V7 40-byte pull for the trail. Two
//     billboards + one trail quad at known positions/colours probe all
//     three streams' lane math.
//
// (b) WB-OIT: the same batch through OITPreparePass (clears + fallback depth
//     seed — the item-1 tenant's machinery) + ParticleRenderPass in OIT mode:
//     per-attachment split blend (accum additive, revealage
//     Zero/OneMinusSrcColor). Covered pixels accumulate weight > 0 and
//     multiply revealage to alpha = 0.5; uncovered keep the {0 / 1} clears.
//
// (c) GPU INDIRECT: Particle_Billboard_GPU (V5 pull + particle SSBOs 0/1/14)
//     through DrawElementsIndirect — vkCmdDrawIndexedIndirect off a
//     hand-seeded {6, 2, 0, 0, 0} indirect StorageBuffer, the exact
//     RenderGPUBillboards shape. The full GPUParticleSystem sim is
//     DISPROPORTIONATE here, deliberately: its emit/simulate/compact .comp
//     chain drives bare uniforms through the no-op SPIR-V Set* route (the
//     Wave B "bare uniforms -> UBO" migration owes it a dedicated slice), so
//     the hand-seeded buffer pins the indirect-draw entry itself and the
//     sim stays documented debt.
// =============================================================================
TEST_F(VulkanPassSuite, ParticleBillboardsTrailsOitAndGpuIndirectDraw)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto patternTexture = MakeSolidTexture(kSize, 16, 16, 16, 255);
    ASSERT_NE(patternTexture, nullptr);
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    // Mid-suite the batch renderer's statics are GL currency (Renderer3D::Init
    // owns them): tear that version down first and flag TearDown to re-init
    // it on the restored GL backend — the amendment (34) two-way discipline
    // (a later GL particle test writing through a shut-down InstancePtr is an
    // AV, not a failure message).
    if (Renderer3D::IsInitialized())
    {
        m_ReinitParticleBatchRendererOnTearDown = true;
        ParticleBatchRenderer::Shutdown();
    }
    // The real batch-renderer statics (shaders, quad/instance/trail VBs,
    // camera + params UBOs, white texture) come up on the Vulkan backend.
    ParticleBatchRenderer::Init();

    const Camera identityCamera(glm::mat4(1.0f));
    const auto submitTestParticles = [&](bool withTrail)
    {
        // Identity projection x identity transform => world == NDC;
        // CameraRight/Up from the transform columns = +X/+Y.
        ParticleBatchRenderer::BeginBatch(identityCamera, glm::mat4(1.0f));
        ParticleBatchRenderer::SetTexture(nullptr); // white fallback
        ParticleBatchRenderer::Submit(glm::vec3(-0.5f, 0.0f, 0.0f), 0.5f, 0.0f,
                                      glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), 11,
                                      glm::vec3(-0.5f, 0.0f, 0.0f), 0.5f, 0.0f);
        ParticleBatchRenderer::Submit(glm::vec3(0.5f, 0.0f, 0.0f), 0.5f, 0.0f,
                                      glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), 12,
                                      glm::vec3(0.5f, 0.0f, 0.0f), 0.5f, 0.0f);
        if (withTrail)
        {
            const std::array<glm::vec3, 4> corners{ glm::vec3(-0.2f, -0.15f, 0.0f), glm::vec3(0.2f, -0.15f, 0.0f),
                                                    glm::vec3(0.2f, 0.15f, 0.0f), glm::vec3(-0.2f, 0.15f, 0.0f) };
            const std::array<glm::vec4, 4> colors{ glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f),
                                                   glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f) };
            const std::array<glm::vec2, 4> uvs{ glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f),
                                                glm::vec2(0.0f, 1.0f) };
            ParticleBatchRenderer::SetTrailTexture(nullptr);
            ParticleBatchRenderer::SubmitTrailQuad(corners, colors, uvs, 13);
        }
        ParticleBatchRenderer::EndBatch();
    };

    const auto px = [&](const std::vector<u8>& rgba, u32 x, u32 y)
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        return std::array<int, 3>{ rgba[i], rgba[i + 1], rgba[i + 2] };
    };

    // ---- (a) classic alpha-blended path through the pass -------------------
    {
        FramebufferSpecification sceneSpec;
        sceneSpec.Width = kSize;
        sceneSpec.Height = kSize;
        sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
        Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
        ASSERT_TRUE(sceneFramebuffer);

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

        auto producer = Ref<PatternProducerPass>::Create(
            patternTexture, blitShader, std::string(ResourceNames::SceneColor),
            std::string(ResourceNames::SceneColorTexture),
            [](FrameBlackboard& board) -> RGFramebufferHandle&
            { return board.Scene.SceneColor; });
        producer->ClearTargetFirst = true; // depth -> 1.0: the LEqual test admits z=0 particles

        auto particlePass = Ref<ParticleRenderPass>::Create();
        FramebufferSpecification initSpec;
        initSpec.Width = kSize;
        initSpec.Height = kSize;
        particlePass->Init(initSpec);
        particlePass->SetRenderCallback([&]()
                                        { submitTestParticles(true); });

        graph.AddNode(producer);
        graph.AddNode(particlePass);
        graph.SetFinalPass("ParticleRenderPass");
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
        EXPECT_TRUE(particlePass->GetTarget()) << "the pass early-returned";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "Particle resolve failure: pass='" << failure.PassName << "' reason='"
                          << failure.Reason << "' x" << failure.Count;
        }
        EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 3u)
            << "producer + billboard flush + trail flush";
        EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

        std::vector<u8> rendered;
        auto* vkScene = static_cast<VulkanFramebuffer*>(sceneFramebuffer.Raw());
        ASSERT_TRUE(vkScene->GetColorAttachmentImage(0)->GetData(rendered, 0));
        ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);

        // Billboards: centre +-0.5 NDC, half-size 0.25 => px 16..48 / 80..112,
        // rows 48..80 (y-symmetric — orientation-proof). Trail: x -0.2..0.2.
        const auto red = px(rendered, 32, 64);
        EXPECT_EQ(red[0], 255) << "billboard 0 colour (instance lane 4..7 at stride 24 floats)";
        EXPECT_EQ(red[1], 0);
        EXPECT_EQ(red[2], 0);
        const auto green = px(rendered, 96, 64);
        EXPECT_EQ(green[0], 0);
        EXPECT_EQ(green[1], 255) << "billboard 1 colour";
        EXPECT_EQ(green[2], 0);
        const auto blue = px(rendered, 64, 64);
        EXPECT_EQ(blue[0], 0);
        EXPECT_EQ(blue[1], 0);
        EXPECT_EQ(blue[2], 255) << "trail quad colour (V7 lane 3..6 at stride 10 floats)";
        const auto background = px(rendered, 5, 5);
        EXPECT_EQ(background[0], 16) << "uncovered pixels keep the producer pattern";
    }

    // ---- (b) WB-OIT per-attachment blend variant ---------------------------
    {
        FramebufferSpecification sceneSpec;
        sceneSpec.Width = kSize;
        sceneSpec.Height = kSize;
        sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8 }; // no depth: OITPrepare's fallback clear
        Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
        ASSERT_TRUE(sceneFramebuffer);

        FramebufferSpecification oitSpec;
        oitSpec.Width = kSize;
        oitSpec.Height = kSize;
        oitSpec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::RG16F,
                                FramebufferTextureFormat::Depth };
        Ref<Framebuffer> oitFramebuffer = Framebuffer::Create(oitSpec);
        ASSERT_TRUE(oitFramebuffer);

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
        producer->TreatAsSideEffecting = true; // the OITPrepare tenant's reachability rule

        auto prepare = Ref<OITPrepareRenderPass>::Create();
        FramebufferSpecification prepareSpec;
        prepareSpec.Width = kSize;
        prepareSpec.Height = kSize;
        prepare->Init(prepareSpec);
        prepare->SetEnabled(true);
        prepare->SetHasContributors(true);

        auto particlePass = Ref<ParticleRenderPass>::Create();
        particlePass->Init(prepareSpec);
        particlePass->SetOITEnabled(true);
        particlePass->SetRenderCallback([&]()
                                        { submitTestParticles(false); });

        graph.AddNode(producer);
        graph.AddNode(prepare);
        graph.AddNode(particlePass);
        graph.SetFinalPass("ParticleRenderPass");
        graph.BuildFrameGraph();

        SubmitFrame(
            [&]()
            {
                graph.Execute();

                std::array<RHI::Barrier, 2> toSampled{};
                toSampled[0].Resource = oitFramebuffer->GetColorAttachmentHandle(0);
                toSampled[0].Before = RHI::Access::ColorAttachmentWrite;
                toSampled[0].After = RHI::Access::ShaderSampleRead;
                toSampled[1].Resource = oitFramebuffer->GetColorAttachmentHandle(1);
                toSampled[1].Before = RHI::Access::ColorAttachmentWrite;
                toSampled[1].After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ toSampled });
            });

        EXPECT_TRUE(particlePass->GetTarget()) << "the OIT-mode pass early-returned";
        for (const auto& failure : graph.GetResolveFailures())
        {
            ADD_FAILURE() << "Particle OIT resolve failure: pass='" << failure.PassName << "' reason='"
                          << failure.Reason << "' x" << failure.Count;
        }
        EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

        std::vector<u8> accumBytes;
        std::vector<u8> revealageBytes;
        auto* vkOit = static_cast<VulkanFramebuffer*>(oitFramebuffer.Raw());
        ASSERT_TRUE(vkOit->GetColorAttachmentImage(0)->GetData(accumBytes, 0));
        ASSERT_TRUE(vkOit->GetColorAttachmentImage(1)->GetData(revealageBytes, 0));
        ASSERT_EQ(accumBytes.size(), static_cast<sizet>(kSize) * kSize * 8);
        ASSERT_EQ(revealageBytes.size(), static_cast<sizet>(kSize) * kSize * 4);

        const auto accumAt = [&](u32 x, u32 y)
        {
            const auto* halves = reinterpret_cast<const u16*>(accumBytes.data());
            const sizet base = (static_cast<sizet>(y) * kSize + x) * 4;
            return std::array<f32, 4>{ HalfToFloat(halves[base]), HalfToFloat(halves[base + 1]),
                                       HalfToFloat(halves[base + 2]), HalfToFloat(halves[base + 3]) };
        };
        const auto revealageAt = [&](u32 x, u32 y)
        {
            const auto* halves = reinterpret_cast<const u16*>(revealageBytes.data());
            return HalfToFloat(halves[(static_cast<sizet>(y) * kSize + x) * 2]);
        };

        // Alpha-1 particles: revealage dst *= (1 - 1) => exactly 0 where
        // covered; accum takes colour * weight (weight's magnitude is the
        // OITCommon curve — assert sign/field, not the constant).
        const auto redAccum = accumAt(32, 64);
        EXPECT_GT(redAccum[0], 0.5f) << "covered accum must accumulate red * weight";
        EXPECT_NEAR(redAccum[1], 0.0f, 1e-3f);
        EXPECT_GT(redAccum[3], 0.5f) << "covered accum alpha must accumulate alpha * weight";
        EXPECT_NEAR(revealageAt(32, 64), 0.0f, 1e-3f) << "alpha-1 coverage zeroes revealage";
        const auto greenAccum = accumAt(96, 64);
        EXPECT_GT(greenAccum[1], 0.5f) << "covered accum must accumulate green * weight";
        EXPECT_NEAR(greenAccum[0], 0.0f, 1e-3f);

        EXPECT_NEAR(accumAt(64, 8)[3], 0.0f, 1e-3f) << "uncovered accum keeps the (0,0,0,0) clear";
        EXPECT_NEAR(revealageAt(64, 8), 1.0f, 1e-3f) << "uncovered revealage keeps the 1.0 clear";
    }

    // ---- (c) GPU-driven indirect draw --------------------------------------
    {
        auto gpuShader = Shader::Create("assets/shaders/Particle_Billboard_GPU.glsl");
        ASSERT_TRUE(gpuShader);
        ASSERT_EQ(gpuShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

        // The RenderGPUBillboards VAO shape: unit quad + index buffer, no
        // instance stream (particle data rides the SSBOs).
        const f32 gpuQuad[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        u32 gpuIndices[] = { 0u, 1u, 2u, 2u, 3u, 0u };
        auto gpuVao = VertexArray::Create();
        auto gpuQuadVB = VertexBuffer::Create(gpuQuad, sizeof(gpuQuad));
        gpuQuadVB->SetLayout({ { ShaderDataType::Float2, "a_QuadPos" } });
        gpuVao->AddVertexBuffer(gpuQuadVB);
        gpuVao->SetIndexBuffer(IndexBuffer::Create(gpuIndices, 6));

        struct GpuParticle
        {
            glm::vec4 PositionLifetime;
            glm::vec4 VelocityMaxLifetime;
            glm::vec4 Color;
            glm::vec4 InitialColor;
            glm::vec4 InitialVelocitySize;
            glm::vec4 Misc; // x = initial size, y = rotation, z = alive, w = entityID
        };
        std::array<GpuParticle, 2> particles{};
        particles[0].PositionLifetime = { -0.5f, 0.0f, 0.0f, 1.0f };
        particles[0].InitialVelocitySize = { 0.0f, 0.0f, 0.0f, 0.5f };
        particles[0].Color = { 1.0f, 0.0f, 0.0f, 1.0f };
        particles[0].Misc = { 0.5f, 0.0f, 1.0f, 21.0f };
        particles[1] = particles[0];
        particles[1].PositionLifetime.x = 0.5f;
        particles[1].Color = { 0.0f, 1.0f, 0.0f, 1.0f };
        particles[1].Misc.w = 22.0f;
        auto particleSSBO = StorageBuffer::Create(sizeof(particles), 0);
        particleSSBO->SetData(particles.data(), sizeof(particles));
        particleSSBO->Bind();

        const u32 aliveIndices[2] = { 0u, 1u };
        auto aliveSSBO = StorageBuffer::Create(sizeof(aliveIndices), 1);
        aliveSSBO->SetData(aliveIndices, sizeof(aliveIndices));
        aliveSSBO->Bind();

        struct PrevParticle
        {
            glm::vec4 Position;
            glm::vec4 RotationSize;
        };
        std::array<PrevParticle, 2> prevData{};
        prevData[0].Position = particles[0].PositionLifetime;
        prevData[0].RotationSize = { 0.0f, 0.5f, 0.0f, 0.0f };
        prevData[1].Position = particles[1].PositionLifetime;
        prevData[1].RotationSize = { 0.0f, 0.5f, 0.0f, 0.0f };
        auto prevSSBO = StorageBuffer::Create(sizeof(prevData), 14);
        prevSSBO->SetData(prevData.data(), sizeof(prevData));
        prevSSBO->Bind();

        // Hand-seeded DrawElementsIndirectCommand == VkDrawIndexedIndirectCommand.
        const u32 indirectArgs[8] = { 6u, 2u, 0u, 0u, 0u, 0u, 0u, 0u };
        auto indirectSSBO = StorageBuffer::Create(sizeof(indirectArgs), 30);
        indirectSSBO->SetData(indirectArgs, sizeof(indirectArgs));

        // Camera + particle params: the GPU shader shares the classic camera
        // block layout (engine CameraUBO prefix) and ParticleParams@2.
        ShaderBindingLayout::CameraUBO cameraData{};
        cameraData.ViewProjection = glm::mat4(1.0f);
        cameraData.View = glm::mat4(1.0f);
        cameraData.Projection = glm::mat4(1.0f);
        cameraData.PrevViewProjection = glm::mat4(1.0f);
        auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(),
                                               ShaderBindingLayout::UBO_CAMERA);
        cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

        struct ParticleParamsBlock
        {
            glm::vec3 CameraRight{ 1.0f, 0.0f, 0.0f };
            f32 _pad0 = 0.0f;
            glm::vec3 CameraUp{ 0.0f, 1.0f, 0.0f };
            i32 HasTexture = 0;
            i32 SoftParticlesEnabled = 0;
            f32 SoftParticleDistance = 1.0f;
            f32 NearClip = 0.1f;
            f32 FarClip = 1000.0f;
            glm::vec2 ViewportSize{ static_cast<f32>(kSize), static_cast<f32>(kSize) };
            f32 _pad1[2]{};
        } particleParams;
        auto paramsUbo = UniformBuffer::Create(sizeof(ParticleParamsBlock), 2);
        paramsUbo->SetData(&particleParams, sizeof(particleParams));

        auto whiteTexture = MakeSolidTexture(4, 255, 255, 255, 255);
        ASSERT_TRUE(whiteTexture);

        FramebufferSpecification outSpec;
        outSpec.Width = kSize;
        outSpec.Height = kSize;
        outSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
        Ref<Framebuffer> outputFramebuffer = Framebuffer::Create(outSpec);
        ASSERT_TRUE(outputFramebuffer);

        SubmitFrame(
            [&]()
            {
                outputFramebuffer->Bind();
                RenderCommand::SetViewport(0, 0, kSize, kSize);
                RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
                RenderCommand::Clear();
                RenderCommand::SetDepthTest(false);
                RenderCommand::SetDepthMask(false);
                RenderCommand::SetBlendState(false);
                RenderCommand::DisableCulling();

                gpuShader->Bind();
                RenderCommand::BindTexture(0, whiteTexture->GetRHIHandle());
                RenderCommand::BindTexture(1, whiteTexture->GetRHIHandle());
                gpuVao->Bind();
                RenderCommand::DrawElementsIndirect(gpuVao, indirectSSBO->GetRHIHandle());

                RHI::Barrier toSampled{};
                toSampled.Resource = outputFramebuffer->GetColorAttachmentHandle(0);
                toSampled.Before = RHI::Access::ColorAttachmentWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            });

        EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 1u) << "the one indirect draw";
        EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

        std::vector<u8> rendered;
        auto* vkOut = static_cast<VulkanFramebuffer*>(outputFramebuffer.Raw());
        ASSERT_TRUE(vkOut->GetColorAttachmentImage(0)->GetData(rendered, 0));

        const auto red = px(rendered, 32, 64);
        EXPECT_EQ(red[0], 255) << "GPU particle 0 (SSBO-sourced) through vkCmdDrawIndexedIndirect";
        EXPECT_EQ(red[1], 0);
        const auto green = px(rendered, 96, 64);
        EXPECT_EQ(green[1], 255) << "GPU particle 1: the indirect instanceCount drives both instances";
        EXPECT_EQ(green[0], 0);
        const auto background = px(rendered, 64, 5);
        EXPECT_EQ(background[0], 0) << "uncovered pixels keep the clear";
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "DrawElementsIndirect + the pull streams must be real, not stubs";

    // Batch-renderer statics hold Vulkan buffers/shaders — release before the
    // fixture tears the device down.
    vkDeviceWaitIdle(m_Device->GetDevice());
    ParticleBatchRenderer::Shutdown();
}

namespace
{
    // -------------------------------------------------------------------------
    // Decal scaffolding (#691 Wave C item 9).
    //
    // The packets a decal pass drains are ordinary CommandPackets, but
    // CommandDispatch::DrawDecal reaches for Renderer3D's OWN per-frame objects
    // (GetDecalUBO, the SSBO-15 InstanceBuffer) — and mid-suite those exist and
    // are GL-currency, so the dispatch would bind foreign handles over this
    // fixture's Vulkan ones and there is no setter to re-home them. The pass
    // under test is ExecuteOnGBuffer, whose whole content is the ATTACHMENT-MAP
    // MATRIX (draw-buffer selection + per-attachment colour mask + additive
    // blend on RT2); the dispatch is scaffolding. So the packets carry a
    // FIXTURE dispatch function — same binds, same draw, this fixture's Vulkan
    // buffers — and the pass body runs completely unmodified.
    //
    // CommandDispatchFn is a plain function pointer (no capture), so the
    // scaffolding's state hangs off a file-scope pointer, exactly as
    // CommandDispatch's own s_Data does.
    // -------------------------------------------------------------------------
    struct DecalDispatchScaffold
    {
        Ref<Shader> AlbedoShader;
        Ref<Shader> NormalShader;
        Ref<Shader> RmaShader;
        Ref<Shader> EmissiveShader;
        Ref<StorageBuffer> InstanceSSBO;
        Ref<UniformBuffer> DecalUbo;
        Ref<VertexArray> ProxyVao;
        u32 ProxyIndexCount = 0;
        u32 Draws = 0;

        [[nodiscard]] Ref<Shader> ShaderFor(DrawDecalCommand::DecalMode mode) const
        {
            switch (mode)
            {
                case DrawDecalCommand::DecalMode::Normal:
                    return NormalShader;
                case DrawDecalCommand::DecalMode::RMA:
                    return RmaShader;
                case DrawDecalCommand::DecalMode::Emissive:
                    return EmissiveShader;
                case DrawDecalCommand::DecalMode::Albedo:
                default:
                    return AlbedoShader;
            }
        }
    };
    DecalDispatchScaffold* g_DecalScaffold = nullptr;

    void FixtureDecalDispatch(const void* data, RendererAPI& api)
    {
        const auto* cmd = static_cast<const DrawDecalCommand*>(data);
        if (cmd == nullptr || g_DecalScaffold == nullptr)
        {
            return;
        }
        auto& scaffold = *g_DecalScaffold;
        auto shader = scaffold.ShaderFor(cmd->mode);
        if (!shader || !scaffold.ProxyVao)
        {
            return;
        }
        shader->Bind();

        // SSBO 15 — InstanceBlock_Vertex's u_Model (CommandDispatch's
        // UploadModelInstance, minus the render-origin shift: this fixture
        // renders at the origin).
        InstanceData instance{};
        instance.Transform = cmd->decalTransform;
        instance.Normal = glm::transpose(glm::inverse(cmd->decalTransform));
        instance.PrevTransform = cmd->decalTransform;
        instance.EntityID = cmd->entityID;
        scaffold.InstanceSSBO->SetData(&instance, sizeof(instance));
        scaffold.InstanceSSBO->Bind();

        // UBO 21 — DecalParams (the projection + colour the fragment reads).
        ShaderBindingLayout::DecalUBO decalData{};
        decalData.InverseDecalTransform = cmd->inverseDecalTransform;
        decalData.InverseViewProjection = cmd->inverseViewProjection;
        decalData.DecalColor = cmd->decalColor;
        decalData.DecalParams = cmd->decalParams;
        scaffold.DecalUbo->SetData(&decalData, ShaderBindingLayout::DecalUBO::GetSize());
        api.BindUniformBuffer(ShaderBindingLayout::UBO_DECAL, scaffold.DecalUbo->GetRHIHandle());

        if (cmd->albedoTextureID.IsValid())
            api.BindTexture(ShaderBindingLayout::TEX_USER_0, cmd->albedoTextureID);
        if (cmd->normalTextureID.IsValid())
            api.BindTexture(ShaderBindingLayout::TEX_USER_1, cmd->normalTextureID);
        if (cmd->rmaTextureID.IsValid())
            api.BindTexture(ShaderBindingLayout::TEX_USER_2, cmd->rmaTextureID);

        // Renderer3D::DrawDecal's PODRenderState, verbatim except culling:
        // the proxy here is a single quad (see the tenant header), so there is
        // no back face to cull and no winding to depend on.
        api.SetDepthTest(true);
        api.SetDepthFunc(RHI::CompareOp::LessOrEqual);
        api.SetDepthMask(false);
        const bool blendForThisMode = cmd->mode == DrawDecalCommand::DecalMode::Albedo;
        api.SetBlendState(blendForThisMode);
        if (blendForThisMode)
        {
            // ONLY when blending is on — ApplyPODRenderState guards it the same
            // way, and that guard is load-bearing: a GLOBAL SetBlendFunc
            // overwrites every draw buffer's factors (glBlendFunc semantics,
            // which the recorded state mirrors via AttachmentBlendFuncSet), so
            // an unconditional call here would silently replace the One/One
            // the Emissive mode installed on RT2 with SrcAlpha/OneMinusSrcAlpha
            // — and the emissive shader writes alpha 0, so the decal would
            // blend to exactly the destination and vanish.
            api.SetBlendFunc(RHI::BlendFactor::SrcAlpha, RHI::BlendFactor::OneMinusSrcAlpha);
        }
        api.DisableCulling();

        api.BindVertexArrayRaw(scaffold.ProxyVao->GetRHIHandle());
        api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, cmd->indexCount, RHI::IndexType::UInt32, 0u);
        ++scaffold.Draws;
    }

    // A quad in the ENGINE Vertex layout (V1: 32 B {vec3 position, vec3 normal,
    // vec2 uv} => the 8-float pull stride) at a fixed NDC z. The decal tenant
    // wants one WIDER than the projected decal box on purpose — the box
    // discard is what has to carve the footprint out, so a proxy that already
    // matched it would prove nothing; the occlusion tenant wants offset ones.
    Ref<VertexArray> MakeV1Quad(f32 x0, f32 x1, f32 y0, f32 y1, f32 z)
    {
        struct EngineVertex
        {
            glm::vec3 Position;
            glm::vec3 Normal;
            glm::vec2 TexCoord;
        };
        static_assert(sizeof(EngineVertex) == 32, "V1 stride is 32 bytes / 8 floats");
        std::array<EngineVertex, 4> vertices{
            EngineVertex{ { x0, y0, z }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            EngineVertex{ { x1, y0, z }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
            EngineVertex{ { x1, y1, z }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
            EngineVertex{ { x0, y1, z }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
        };
        u32 indices[] = { 0u, 1u, 2u, 2u, 3u, 0u };
        auto vao = VertexArray::Create();
        auto vb = VertexBuffer::Create(reinterpret_cast<f32*>(vertices.data()),
                                       static_cast<u32>(sizeof(vertices)));
        vb->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                        { ShaderDataType::Float3, "a_Normal" },
                        { ShaderDataType::Float2, "a_TexCoord" } });
        vao->AddVertexBuffer(vb);
        vao->SetIndexBuffer(IndexBuffer::Create(indices, 6));
        return vao;
    }

    // Submit one decal packet into a pass's own bucket. The bucket allocates
    // through the pass's CommandAllocator, so nothing here needs Renderer3D's
    // per-frame arena.
    void SubmitDecalPacket(DecalRenderPass& pass, const DrawDecalCommand& command)
    {
        PacketMetadata metadata;
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 1u, 0u, 0u);
        CommandPacket* packet = pass.GetCommandBucket().Submit(command, metadata);
        ASSERT_NE(packet, nullptr);
        packet->SetCommandType(CommandType::DrawDecal);
        packet->SetDispatchFunction(&FixtureDecalDispatch);
    }
} // namespace

// =============================================================================
// Decal (#691 Wave C item 9): the ATTACHMENT-MAP MATRIX (ADR item A4).
//
// DecalRenderPass::ExecuteOnGBuffer switches between FIVE draw-attachment maps
// (each 5 entries wide, RHI::NoAttachment for the holes) plus per-attachment
// colour masks plus additive blend on RT2, once per decal MODE, so a decal
// writes only the G-Buffer channels its mode owns and leaves every other one —
// and every non-masked component of its own — exactly as the underlying
// surface left it. That is the contract this tenant pins, and it is the first
// thing on Vulkan to need VK_ATTACHMENT_UNUSED holes in a VkRenderingInfo
// array, per-attachment colourWriteMask, and per-attachment blendEnable all at
// once (the last two also being why VulkanDevice now enables independentBlend
// — without it every pAttachments element must be IDENTICAL and the whole
// facade family is undefined).
//
// Contract shape: "untouched" is asserted BYTE-EXACT and IN-FRAME, by
// comparing footprint texels against texels outside the footprint of the SAME
// attachment. The decal covers only the box's projection, so a leaked write
// separates the two; a clear-vs-clear comparison could not tell a masked write
// from a no-op draw, which is why the targeted RT is asserted to DIFFER in the
// same breath.
//
// Geometry: the projection is depth-driven (screenUV -> depth -> world ->
// decal-local, discard outside [-0.5, 0.5]^3), so with an identity
// inverse-view-projection and a uniform 0.749 depth the world point is
// (u*2-1, v*2-1, 0.498) and a box at translate(0,0,0.5) carves the central
// 64x64 square out of a WIDER proxy quad. One quad, not the production cube:
// coverage exactly once is what the additive emissive mode needs, and it takes
// the front-face-winding question off the table.
//
// DEFECT FOUND HERE, FIXED IN THIS CHANGE (issue #770): Normal-mode decals used
// to write NOTHING, on EITHER backend. Decal_GBuffer_Normal.glsl declared its
// output at `layout(location = 0)` while the mode's draw map is {NONE, 1, NONE,
// NONE, NONE} — location 0 mapped to no attachment and location 1 was never
// written. The other three modes' locations and maps already agreed (the rule
// is "output location == G-Buffer RT index"), so the shader moved to
// `layout(location = 1)`. The Vulkan validation layer had said it in as many
// words while this tenant ran ("writes to [Output variable, Location 0,
// \"gNormalRoughAO\"] but there is no VkRenderingInfo::pColorAttachments[0] ...
// and this write is unused" — a WARNING, so it never tripped the zero-error
// gate), which was corroboration from outside the engine. The Normal block
// below now asserts the mode's real contract, like the other three.
// =============================================================================
TEST_F(VulkanPassSuite, DecalGBufferModeMatrixMasksItsTargetRenderTargets)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    // --- the four G-Buffer decal variants + the two forward ones ------------
    DecalDispatchScaffold scaffold;
    scaffold.AlbedoShader = Shader::Create("assets/shaders/Decal_GBuffer.glsl");
    scaffold.NormalShader = Shader::Create("assets/shaders/Decal_GBuffer_Normal.glsl");
    scaffold.RmaShader = Shader::Create("assets/shaders/Decal_GBuffer_RMA.glsl");
    scaffold.EmissiveShader = Shader::Create("assets/shaders/Decal_GBuffer_Emissive.glsl");
    ASSERT_TRUE(scaffold.AlbedoShader && scaffold.NormalShader && scaffold.RmaShader && scaffold.EmissiveShader);
    for (const auto& shader : { scaffold.AlbedoShader, scaffold.NormalShader, scaffold.RmaShader,
                                scaffold.EmissiveShader })
    {
        ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
            << "every decal G-Buffer variant must compile through shaderc (V1 pull branch)";
    }
    // The two forward-path variants ride the same V1 pull branch; their pass
    // shape (scene colour / WB-OIT) is the OIT machinery earlier tenants
    // already pin, so here they are compile-level.
    for (const char* forward : { "assets/shaders/Decal.glsl", "assets/shaders/Decal_OIT.glsl" })
    {
        auto shader = Shader::Create(forward);
        ASSERT_TRUE(shader);
        EXPECT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
            << forward << " must compile through shaderc (V1 pull branch)";
    }

    // --- scaffolding resources ----------------------------------------------
    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    cameraData.PrevViewProjection = glm::mat4(1.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    scaffold.InstanceSSBO = StorageBuffer::Create(sizeof(InstanceData), ShaderBindingLayout::SSBO_INSTANCE_DATA);
    scaffold.DecalUbo = UniformBuffer::Create(ShaderBindingLayout::DecalUBO::GetSize(), ShaderBindingLayout::UBO_DECAL);
    scaffold.ProxyVao = MakeV1Quad(-1.0f, 1.0f, -1.0f, 1.0f, 0.5f);
    scaffold.ProxyIndexCount = 6;
    ASSERT_TRUE(scaffold.InstanceSSBO && scaffold.DecalUbo && scaffold.ProxyVao);
    g_DecalScaffold = &scaffold;

    // Decal source textures: white everywhere (the mode's channel routing, not
    // the texture content, is what is under test). RMA reads R=roughness,
    // G=metallic, B=AO; the normal map's (0.5, 0.5, 1) is the flat tangent
    // normal.
    auto whiteTexture = MakeSolidTexture(8, 255, 255, 255, 255);
    auto rmaTexture = MakeSolidTexture(8, 255, 255, 255, 255);
    auto normalTexture = MakeSolidTexture(8, 128, 128, 255, 255);
    ASSERT_TRUE(whiteTexture && rmaTexture && normalTexture);

    // --- the G-Buffer and its (separate) depth source -----------------------
    auto gbuffer = GBuffer::Create(kSize, kSize, 1);
    ASSERT_TRUE(gbuffer);
    Ref<Framebuffer> gbufferFB = gbuffer->GetSamplingFramebuffer();

    // The production per-sample call passes a DIFFERENT framebuffer for depth
    // sampling than for writing; this tenant uses that same two-argument form
    // deliberately. Sampling the write target's OWN depth while it is attached
    // is the documented mid-pass layout gap (a descriptor baked
    // SHADER_READ_ONLY against an image the scope holds in
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL) — out of scope here and not what the
    // attachment-map matrix is about.
    FramebufferSpecification depthSpec;
    depthSpec.Width = kSize;
    depthSpec.Height = kSize;
    depthSpec.Attachments = { FramebufferTextureFormat::Depth };
    Ref<Framebuffer> depthSourceFB = Framebuffer::Create(depthSpec);
    ASSERT_TRUE(depthSourceFB);

    auto decalPass = Ref<DecalRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    decalPass->SetupFramebuffer(kSize, kSize);

    // Cleared G-Buffer state every chain starts from: a mid grey on the float
    // RTs (0.25 -> 64/255 on RT0) and entity id 7.
    constexpr f32 kClear = 0.25f;
    constexpr int kClearEntity = 7;
    // The world point the depth sample reconstructs: 0.749 * 2 - 1 = 0.498,
    // which sits in the middle of a box spanning z in [0, 1].
    constexpr f32 kSceneDepth = 191.0f / 255.0f;

    const auto makeCommand = [&](DrawDecalCommand::DecalMode mode)
    {
        DrawDecalCommand command{};
        command.header.type = CommandType::DrawDecal;
        command.vertexArrayID = scaffold.ProxyVao->GetRHIHandle();
        command.indexCount = scaffold.ProxyIndexCount;
        command.shaderRendererID = scaffold.ShaderFor(mode)->GetRHIHandle();
        command.decalTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.5f));
        command.inverseDecalTransform = glm::inverse(command.decalTransform);
        command.inverseViewProjection = glm::mat4(1.0f);
        command.decalColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        // fadeDistance 0.001: the edge smoothstep saturates to 1 everywhere
        // except a sub-texel rim, so the footprint is a hard square.
        command.decalParams = glm::vec4(0.001f, 0.0f, 0.0f, 0.0f);
        command.albedoTextureID = whiteTexture->GetRHIHandle();
        command.normalTextureID = normalTexture->GetRHIHandle();
        command.rmaTextureID = rmaTexture->GetRHIHandle();
        command.mode = mode;
        command.transparent = 0;
        command.entityID = 42;
        return command;
    };

    auto* vkGbuffer = static_cast<VulkanFramebuffer*>(gbufferFB.Raw());

    // Readback of one attachment, plus the two probe texels: (64, 64) is dead
    // centre of the decal footprint, (8, 8) is far outside it.
    struct AttachmentReadback
    {
        std::vector<u8> Bytes;
        u32 BytesPerPixel = 4;
    };
    const auto readAttachment = [&](u32 index, u32 bytesPerPixel)
    {
        AttachmentReadback out;
        out.BytesPerPixel = bytesPerPixel;
        EXPECT_TRUE(vkGbuffer->GetColorAttachmentImage(index)->GetData(out.Bytes, 0))
            << "G-Buffer RT" << index << " readback failed";
        return out;
    };
    const auto texel = [&](const AttachmentReadback& rb, u32 x, u32 y)
    {
        const sizet offset = (static_cast<sizet>(y) * kSize + x) * rb.BytesPerPixel;
        return std::vector<u8>(rb.Bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                               rb.Bytes.begin() + static_cast<std::ptrdiff_t>(offset + rb.BytesPerPixel));
    };

    // Which attachments the mode's draw MAP puts in the rendering scope. Note
    // this is the map, NOT the shader's outputs: an attached-but-unwritten
    // attachment still takes a COLOR_ATTACHMENT_WRITE from the storeOp at
    // vkCmdEndRendering (which is exactly how its content survives), so its
    // readback barrier must name that as the source scope. Naming the transfer
    // clear instead is a WRITE_AFTER_WRITE the sync validation reports — RMA's
    // RT0, attached so its .a can be written while .rgb is masked off, is the
    // standing case that separates the two.
    const auto attachedBy = [](DrawDecalCommand::DecalMode mode)
    {
        switch (mode)
        {
            case DrawDecalCommand::DecalMode::Normal: // { NONE, 1, NONE, NONE, NONE }
                return std::array<bool, 5>{ false, true, false, false, false };
            case DrawDecalCommand::DecalMode::RMA: // { 0, 1, NONE, NONE, NONE }
                return std::array<bool, 5>{ true, true, false, false, false };
            case DrawDecalCommand::DecalMode::Emissive: // { NONE, NONE, 2, NONE, NONE }
                return std::array<bool, 5>{ false, false, true, false, false };
            case DrawDecalCommand::DecalMode::Albedo: // { 0, NONE, NONE, NONE, NONE }
            default:
                return std::array<bool, 5>{ true, false, false, false, false };
        }
    };

    const auto runMode = [&](DrawDecalCommand::DecalMode mode)
    {
        decalPass->ResetCommandBucket();
        SubmitDecalPacket(*decalPass, makeCommand(mode));
        scaffold.Draws = 0;
        const auto attached = attachedBy(mode);

        SubmitFrame(
            [&]()
            {
                // Author the frame's starting state INSIDE the bracket: the
                // facade transfer clears are recorded commands.
                gbufferFB->ClearAllAttachments(glm::vec4(kClear), kClearEntity);
                RenderCommand::ClearFramebufferDepth(gbufferFB->GetRHIHandle(), 1.0f);
                RenderCommand::ClearFramebufferDepth(depthSourceFB->GetRHIHandle(), kSceneDepth);

                decalPass->ExecuteOnGBuffer(gbufferFB, depthSourceFB);

                // Every colour attachment plus the entity RT to the readback
                // layout, each naming its OWN last producer as the barrier's
                // source scope (the decal's attachment write where the mode's
                // map routed an output, the transfer clear everywhere else).
                for (u32 attachment = 0; attachment < 5u; ++attachment)
                {
                    RHI::Barrier toSampled{};
                    toSampled.Resource = gbufferFB->GetColorAttachmentHandle(attachment);
                    toSampled.Before = attached[attachment] ? RHI::Access::ColorAttachmentWrite
                                                            : RHI::Access::TransferWrite;
                    toSampled.After = RHI::Access::ShaderSampleRead;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                }
            });

        EXPECT_EQ(scaffold.Draws, 1u) << "the mode's single decal packet must reach the dispatch";
        EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u) << "a decal draw dropped silently";
    };

    // --- Albedo: RT0.rgb only -----------------------------------------------
    {
        runMode(DrawDecalCommand::DecalMode::Albedo);
        const auto rt0 = readAttachment(0, 4);
        const auto inside = texel(rt0, 64, 64);
        const auto outside = texel(rt0, 8, 8);
        EXPECT_EQ(inside[0], 255) << "RT0.r takes the decal colour inside the projected box";
        EXPECT_EQ(inside[1], 0);
        EXPECT_EQ(inside[2], 0);
        EXPECT_EQ(inside[3], outside[3]) << "RT0.a (metallic) is masked out and must survive";
        EXPECT_EQ(outside[0], 64) << "outside the box the clear survives (0.25 -> 64)";

        for (const auto& [attachment, bytesPerPixel] :
             { std::pair<u32, u32>{ 1u, 8u }, { 2u, 8u }, { 3u, 4u }, { 4u, 4u } })
        {
            const auto rb = readAttachment(attachment, bytesPerPixel);
            EXPECT_EQ(texel(rb, 64, 64), texel(rb, 8, 8))
                << "Albedo mode must leave RT" << attachment << " byte-exact (NoAttachment hole)";
        }
    }

    // --- Emissive: RT2.rgb only, ADDITIVE, RT2.a preserved ------------------
    {
        runMode(DrawDecalCommand::DecalMode::Emissive);
        const auto rt2 = readAttachment(2, 8);
        const auto* halves = reinterpret_cast<const u16*>(rt2.Bytes.data());
        const auto emissiveAt = [&](u32 x, u32 y)
        {
            const sizet base = (static_cast<sizet>(y) * kSize + x) * 4;
            return std::array<f32, 4>{ HalfToFloat(halves[base]), HalfToFloat(halves[base + 1]),
                                       HalfToFloat(halves[base + 2]), HalfToFloat(halves[base + 3]) };
        };
        const auto inside = emissiveAt(64, 64);
        const auto outside = emissiveAt(8, 8);
        // The emissive decal is white * u_DecalColor (1,0,0) and RT2 blends
        // One/One over the 0.25 clear => 1.25 on red, clear elsewhere.
        EXPECT_NEAR(inside[0], kClear + 1.0f, 0.01f) << "RT2.r accumulates ADDITIVELY over the clear";
        EXPECT_NEAR(inside[1], kClear, 0.01f) << "the decal's green is 0 and the blend is additive";
        EXPECT_NEAR(inside[2], kClear, 0.01f);
        EXPECT_NEAR(inside[3], outside[3], 1e-4f) << "RT2.a (the unlit flag) is masked out";
        EXPECT_NEAR(outside[0], kClear, 0.01f);

        for (const auto& [attachment, bytesPerPixel] :
             { std::pair<u32, u32>{ 0u, 4u }, { 1u, 8u }, { 3u, 4u }, { 4u, 4u } })
        {
            const auto rb = readAttachment(attachment, bytesPerPixel);
            EXPECT_EQ(texel(rb, 64, 64), texel(rb, 8, 8))
                << "Emissive mode must leave RT" << attachment << " byte-exact";
        }
    }

    // --- RMA: RT0.a + RT1.zw, across TWO attachments ------------------------
    {
        runMode(DrawDecalCommand::DecalMode::RMA);
        const auto rt0 = readAttachment(0, 4);
        const auto inside0 = texel(rt0, 64, 64);
        const auto outside0 = texel(rt0, 8, 8);
        EXPECT_EQ(inside0[0], outside0[0]) << "RT0.rgb (albedo) is masked out under RMA";
        EXPECT_EQ(inside0[1], outside0[1]);
        EXPECT_EQ(inside0[2], outside0[2]);
        EXPECT_NE(inside0[3], outside0[3]) << "RT0.a (metallic) is the RMA mode's own channel";

        const auto rt1 = readAttachment(1, 8);
        const auto* halves = reinterpret_cast<const u16*>(rt1.Bytes.data());
        const auto normalAt = [&](u32 x, u32 y)
        {
            const sizet base = (static_cast<sizet>(y) * kSize + x) * 4;
            return std::array<f32, 4>{ HalfToFloat(halves[base]), HalfToFloat(halves[base + 1]),
                                       HalfToFloat(halves[base + 2]), HalfToFloat(halves[base + 3]) };
        };
        const auto inside1 = normalAt(64, 64);
        const auto outside1 = normalAt(8, 8);
        EXPECT_NEAR(inside1[0], outside1[0], 1e-4f) << "RT1.xy (the oct normal) is masked out under RMA";
        EXPECT_NEAR(inside1[1], outside1[1], 1e-4f);
        EXPECT_GT(std::abs(inside1[2] - outside1[2]) + std::abs(inside1[3] - outside1[3]), 0.01f)
            << "RT1.zw (roughness, AO) is the RMA mode's second channel pair";

        for (const auto& [attachment, bytesPerPixel] : { std::pair<u32, u32>{ 2u, 8u }, { 3u, 4u }, { 4u, 4u } })
        {
            const auto rb = readAttachment(attachment, bytesPerPixel);
            EXPECT_EQ(texel(rb, 64, 64), texel(rb, 8, 8))
                << "RMA mode must leave RT" << attachment << " byte-exact";
        }
    }

    // --- Normal: RT1.xy only, RT1.zw preserved (issue #770) -----------------
    {
        runMode(DrawDecalCommand::DecalMode::Normal);
        const auto rt1 = readAttachment(1, 8);
        const auto* halves = reinterpret_cast<const u16*>(rt1.Bytes.data());
        const auto normalAt = [&](u32 x, u32 y)
        {
            const sizet base = (static_cast<sizet>(y) * kSize + x) * 4;
            return std::array<f32, 4>{ HalfToFloat(halves[base]), HalfToFloat(halves[base + 1]),
                                       HalfToFloat(halves[base + 2]), HalfToFloat(halves[base + 3]) };
        };
        const auto inside = normalAt(64, 64);
        const auto outside = normalAt(8, 8);
        // The oct-encoded normal is asserted as "not the clear" rather than a
        // literal: it comes out of a screen-space derivative frame, so its exact
        // value depends on the reconstructed surface orientation. What the mode
        // owes is that RT1.xy MOVED inside the footprint — which is precisely
        // what a location-0 output could never do.
        EXPECT_GT(std::abs(inside[0] - outside[0]) + std::abs(inside[1] - outside[1]), 0.01f)
            << "RT1.xy (the oct normal) is the Normal mode's own channel pair — a decal that writes "
               "nothing here is issue #770 back (shader output location must equal the G-Buffer RT index)";
        EXPECT_NEAR(inside[2], outside[2], 1e-4f) << "RT1.z (roughness) is masked out and must survive";
        EXPECT_NEAR(inside[3], outside[3], 1e-4f) << "RT1.w (AO) is masked out and must survive";

        for (const auto& [attachment, bytesPerPixel] :
             { std::pair<u32, u32>{ 0u, 4u }, { 2u, 8u }, { 3u, 4u }, { 4u, 4u } })
        {
            const auto rb = readAttachment(attachment, bytesPerPixel);
            EXPECT_EQ(texel(rb, 64, 64), texel(rb, 8, 8))
                << "Normal mode must leave RT" << attachment << " byte-exact (NoAttachment hole)";
        }
    }

    // The entity RT is never in any decal draw map — decals must not stamp
    // their pickability over the underlying mesh.
    {
        const auto rt4 = readAttachment(4, 4);
        const auto* ids = reinterpret_cast<const i32*>(rt4.Bytes.data());
        EXPECT_EQ(ids[static_cast<sizet>(64) * kSize + 64], kClearEntity)
            << "RT4 (entity id) is outside every decal draw map";
    }

    g_DecalScaffold = nullptr;
    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "the attachment-map matrix must ride real implementations, not stubs";
}

// =============================================================================
// DeferredOpaqueDecal (#691 Wave C item 9, second half): the graph node that
// owns the drain and the G-Buffer EXPORTS.
//
// The node declares TransferDest writes on the blackboard's G-Buffer texture
// slots, drains the opaque decals into the G-Buffer, then CopyImageSubData's
// the attachments into those exported textures — which is how downstream
// passes read the G-Buffer without importing the pass's own attachments. This
// tenant runs the UNMODIFIED node in the real graph and pins the EXPORT half:
// a seeded export target must come back carrying the G-Buffer's content, which
// only happens if Setup declared the slot, Execute resolved it, and the copy
// ran with the right extents.
//
// The DRAIN half is pinned by the ExecuteOnGBuffer tenant above and cannot be
// re-pinned here at ONE sample: in that configuration the node hands
// ExecuteOnGBuffer the same framebuffer for writing AND for depth sampling, so
// the decal fragment samples the very depth attachment the rendering scope
// holds — a descriptor baked SHADER_READ_ONLY against an image in
// DEPTH_STENCIL_ATTACHMENT_OPTIMAL. That is the already-documented mid-pass
// layout gap (its real fix is a read-only depth attachment layout the scope
// builder would have to choose from the depth-write state), not something to
// paper over with a tolerance: the sampled value is undefined, so the decal's
// footprint is not deterministic. The bucket is therefore left EMPTY here and
// the gap is reported.
//
// The multisample arm (GetSampleCount() > 1 + PerSampleLighting — which is
// also the configuration that would give the drain a separate depth
// framebuffer — needs multisample VulkanTexture2D attachments, the MS
// CopyImageSubData pair and GBuffer::Resolve, none of which this backend has
// yet. Reported, not worked around.
// =============================================================================
TEST_F(VulkanPassSuite, DeferredOpaqueDecalExportsTheGBufferThroughTheGraph)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto gbuffer = GBuffer::Create(kSize, kSize, 1);
    ASSERT_TRUE(gbuffer);
    Ref<Framebuffer> gbufferFB = gbuffer->GetSamplingFramebuffer();

    // Export targets: caller-owned textures imported under the blackboard names
    // the node writes, so they survive the frame for readback. Each is seeded
    // with a value the copy has to overwrite — an untouched target and a
    // correctly copied one must not be able to look the same.
    const auto makeSeededExport = [&](ImageFormat format, u32 bytesPerPixel, u8 seedValue)
    {
        TextureSpecification exportSpec;
        exportSpec.Width = kSize;
        exportSpec.Height = kSize;
        exportSpec.Format = format;
        exportSpec.GenerateMips = false;
        auto texture = Texture2D::Create(exportSpec);
        if (texture)
        {
            std::vector<u8> seed(static_cast<sizet>(kSize) * kSize * bytesPerPixel, seedValue);
            texture->SetData(seed.data(), static_cast<u32>(seed.size()));
        }
        return texture;
    };
    // Each export target must be TEXEL-SIZE COMPATIBLE with the G-Buffer
    // attachment it copies from (VUID-vkCmdCopyImage-srcImage-01548): albedo
    // is RGBA8 (RT0), normals are RGBA16F (RT1). A copy is not a conversion.
    // The 16F seed ships 16 bytes/texel — the facade's f32-per-channel client
    // contract (the backend converts to halves); the seed's exact value is
    // irrelevant, it only has to differ from the copied G-Buffer content.
    auto exportedAlbedo = makeSeededExport(ImageFormat::RGBA8, 4u, 17u);
    auto exportedNormals = makeSeededExport(ImageFormat::RGBA16F, 16u, 19u);
    ASSERT_TRUE(exportedAlbedo && exportedNormals);

    auto decalPass = Ref<DecalRenderPass>::Create();
    decalPass->SetupFramebuffer(kSize, kSize);

    auto opaqueDecalPass = Ref<DeferredOpaqueDecalPass>::Create();
    opaqueDecalPass->SetGBuffer(gbuffer);
    opaqueDecalPass->SetDecalPass(decalPass);
    opaqueDecalPass->SetPerSampleLighting(false);

    RenderGraph graph;
    graph.SetTransientMaterializationEnabled(true);
    auto& blackboard = graph.GetBlackboard();

    RGResourceDesc importDesc;
    importDesc.Kind = RGResourceHandle::Kind::Texture2D;
    importDesc.Format = RGResourceFormat::RGBA8UNorm;
    importDesc.Width = kSize;
    importDesc.Height = kSize;
    blackboard.GBuffer.GBufferAlbedo =
        graph.ImportTextureHandle(ResourceNames::GBufferAlbedo, exportedAlbedo->GetRHIHandle(), importDesc);
    // SceneNormals copies the G-Buffer NORMAL attachment (RGBA16F) — the
    // node's second, independently-declared export slot.
    RGResourceDesc normalDesc = importDesc;
    normalDesc.Format = RGResourceFormat::RGBA16Float;
    blackboard.Scene.SceneNormals =
        graph.ImportTextureHandle(ResourceNames::SceneNormals, exportedNormals->GetRHIHandle(), normalDesc);

    graph.AddNode(opaqueDecalPass);
    graph.SetFinalPass("DeferredOpaqueDecalPass");
    graph.BuildFrameGraph();

    SubmitFrame(
        [&]()
        {
            // Authored G-Buffer content the exports must carry: albedo 0.25 on
            // every channel (-> 64) and entity id 7.
            gbufferFB->ClearAllAttachments(glm::vec4(0.25f), 7);

            graph.Execute();

            for (const auto& handle : { exportedAlbedo->GetRHIHandle(), exportedNormals->GetRHIHandle() })
            {
                RHI::Barrier toSampled{};
                toSampled.Resource = handle;
                toSampled.Before = RHI::Access::TransferWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            }
        });

    EXPECT_TRUE(opaqueDecalPass->GetTarget()) << "the node early-returned";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "DeferredOpaqueDecal resolve failure: pass='" << failure.PassName << "' reason='"
                      << failure.Reason << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    std::vector<u8> exported;
    ASSERT_TRUE(exportedAlbedo->GetData(exported, 0));
    ASSERT_EQ(exported.size(), static_cast<sizet>(kSize) * kSize * 4);
    for (const auto& [x, y] : { std::pair<u32, u32>{ 8, 8 }, { 64, 64 }, { 120, 120 } })
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        EXPECT_EQ(exported[i], 64) << "the exported albedo must carry the G-Buffer's content at (" << x << ","
                                   << y << ")";
        EXPECT_NE(exported[i], 17) << "the seed must have been overwritten — the copy has to have run";
    }

    std::vector<u8> exportedNormalBytes;
    ASSERT_TRUE(exportedNormals->GetData(exportedNormalBytes, 0));
    ASSERT_EQ(exportedNormalBytes.size(), static_cast<sizet>(kSize) * kSize * 8);
    {
        const auto* halves = reinterpret_cast<const u16*>(exportedNormalBytes.data());
        EXPECT_NEAR(HalfToFloat(halves[(static_cast<sizet>(64) * kSize + 64) * 4]), 0.25f, 1e-3f)
            << "the SceneNormals export is a SECOND declared slot — its RGBA16F copy must run too";
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore);
}

// =============================================================================
// Occlusion queries (#691 Wave C item 10, second half — ADR item A6).
//
// The facade's query family is GL's: N independent query names, glBeginQuery /
// glEndQuery around the proxy draws, a host read next frame. Vulkan's is one
// VkQueryPool with N slots and three disciplines GL has no equivalent of — a
// per-slot vkCmdResetQueryPool before the write, a begin/end pair that may not
// straddle a render pass instance boundary, and a WAIT read that deadlocks on
// a slot no command buffer ever wrote. VulkanQueryRegistry + the scope-ending
// Begin/EndQuery are those three; this tenant is what proves them.
//
// Chain 1 (the counts): an occluder quad at z = 0.3 covering the LEFT half
// writes depth; two proxies at z = 0.6 are then drawn under Less with depth
// writes off, each inside its own query. The left proxy sits behind the
// occluder (zero samples pass); the right one is unoccluded (thousands do).
// The pair is what makes the assertion meaningful — "zero" alone is also what
// a query that never ran returns.
//
// Chain 2 (conditional rendering): BeginConditionalRender over the OCCLUDED
// query must skip the draws that follow, and over the VISIBLE one must not.
// The predicate is evaluated HOST-side here rather than through
// VK_EXT_conditional_rendering (see VulkanRendererAPI::BeginConditionalRender
// for why); the engine's only caller passes the previous frame's query, whose
// result is already resolved on the host, so the two are equivalent for it.
// Each framebuffer takes an UNCONDITIONAL baseline draw first: a pending clear
// with no draw behind it never materialises on Vulkan (the lazy scope folds it
// into a loadOp that never opens), so a skipped-only target has no defined
// content to assert against.
// =============================================================================
TEST_F(VulkanPassSuite, OcclusionQueriesCountSamplesAndGateConditionalRendering)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto depthOnlyShader = Shader::Create("assets/shaders/DepthPrepass.glsl");
    auto proxyShader = Shader::Create("assets/shaders/OcclusionProxy.glsl");
    ASSERT_TRUE(depthOnlyShader && proxyShader);
    ASSERT_EQ(depthOnlyShader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
        << "DepthPrepass.glsl must compile through shaderc (V1 pull branch)";
    ASSERT_EQ(proxyShader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
        << "OcclusionProxy.glsl must compile through shaderc (V1 pull branch)";
    // The masked prepass variant rides the same branch with SPARSE attribute
    // locations (0 and 2, no normal) — compile-pinned here.
    {
        auto maskShader = Shader::Create("assets/shaders/DepthPrepass_Mask.glsl");
        ASSERT_TRUE(maskShader);
        EXPECT_EQ(maskShader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
            << "DepthPrepass_Mask.glsl must compile (sparse locations 0 and 2 over the same 8-float stride)";
    }

    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    cameraData.PrevViewProjection = glm::mat4(1.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    const InstanceData identityInstance{};
    auto instanceSSBO = StorageBuffer::Create(sizeof(InstanceData), ShaderBindingLayout::SSBO_INSTANCE_DATA);
    instanceSSBO->SetData(&identityInstance, sizeof(identityInstance));
    instanceSSBO->Bind();

    auto occluder = MakeV1Quad(-1.0f, 0.0f, -1.0f, 1.0f, 0.3f);
    auto occludedProxy = MakeV1Quad(-0.8f, -0.2f, -0.5f, 0.5f, 0.6f);
    auto visibleProxy = MakeV1Quad(0.2f, 0.8f, -0.5f, 0.5f, 0.6f);
    ASSERT_TRUE(occluder && occludedProxy && visibleProxy);

    FramebufferSpecification sceneSpec;
    sceneSpec.Width = kSize;
    sceneSpec.Height = kSize;
    sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
    ASSERT_TRUE(sceneFramebuffer);

    std::array<RHI::ResourceHandle, 2> queries{ RHI::NullResource, RHI::NullResource };
    RenderCommand::CreateQueries(RHI::QueryType::OcclusionAnySamples, queries);
    ASSERT_TRUE(queries[0].IsValid()) << "CreateQueries must mint a real VkQueryPool-backed identity";
    ASSERT_TRUE(queries[1].IsValid());
    EXPECT_FALSE(RenderCommand::IsQueryResultAvailable(queries[0]))
        << "a query no command buffer has written must never report a result (the WAIT-deadlock guard)";

    // --- chain 1: the counts -------------------------------------------------
    SubmitFrame(
        [&]()
        {
            sceneFramebuffer->Bind();
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear(); // colour + depth -> 1.0, folded into the first draw's loadOp
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();

            // The occluder writes depth 0.3 over the left half.
            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
            RenderCommand::SetDepthMask(true);
            depthOnlyShader->Bind();
            occluder->Bind();
            RenderCommand::DrawIndexed(occluder, 6);

            // Proxies read depth, never write it (the production shape).
            RenderCommand::SetDepthMask(false);
            proxyShader->Bind();

            RenderCommand::BeginQuery(RHI::QueryType::OcclusionAnySamples, queries[0]);
            occludedProxy->Bind();
            RenderCommand::DrawIndexed(occludedProxy, 6);
            RenderCommand::EndQuery(RHI::QueryType::OcclusionAnySamples);

            RenderCommand::BeginQuery(RHI::QueryType::OcclusionAnySamples, queries[1]);
            visibleProxy->Bind();
            RenderCommand::DrawIndexed(visibleProxy, 6);
            RenderCommand::EndQuery(RHI::QueryType::OcclusionAnySamples);
        });

    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 3u) << "occluder + two proxies";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    ASSERT_TRUE(RenderCommand::IsQueryResultAvailable(queries[0]))
        << "a submitted+fenced occlusion query must report available";
    ASSERT_TRUE(RenderCommand::IsQueryResultAvailable(queries[1]));
    EXPECT_EQ(RenderCommand::GetQueryResultU32(queries[0]), 0u)
        << "the proxy behind the occluder must pass ZERO samples";
    const u32 visibleSamples = RenderCommand::GetQueryResultU32(queries[1]);
    EXPECT_GT(visibleSamples, 0u) << "the unoccluded proxy must pass samples (a zero here would make the "
                                     "occluded assertion above vacuous)";
    EXPECT_EQ(RenderCommand::GetQueryResultU64(queries[1]), static_cast<u64>(visibleSamples))
        << "the 64-bit read must agree with the 32-bit one";

    // --- chain 2: the conditional-render gate --------------------------------
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    auto blackTexture = MakeSolidTexture(kSize, 0, 0, 0, 255);
    auto redTexture = MakeSolidTexture(kSize, 255, 0, 0, 255);
    ASSERT_TRUE(blackTexture && redTexture);

    FramebufferSpecification flatSpec;
    flatSpec.Width = kSize;
    flatSpec.Height = kSize;
    flatSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    Ref<Framebuffer> skippedTarget = Framebuffer::Create(flatSpec);
    Ref<Framebuffer> drawnTarget = Framebuffer::Create(flatSpec);
    ASSERT_TRUE(skippedTarget && drawnTarget);

    SubmitFrame(
        [&]()
        {
            const auto drawPattern = [&](const Ref<Texture2D>& pattern)
            {
                blitShader->Bind();
                RenderCommand::BindTexture(0, pattern->GetRHIHandle());
                const auto va = MeshPrimitives::GetFullscreenTriangle();
                va->Bind();
                RenderCommand::DrawIndexed(va);
            };

            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();

            // Plain array, not an initializer_list: its elements are const and
            // Ref<T> const-propagates, so Bind() would be unreachable.
            std::array<std::pair<Ref<Framebuffer>, RHI::ResourceHandle>, 2> chains{
                std::pair<Ref<Framebuffer>, RHI::ResourceHandle>{ skippedTarget, queries[0] },
                std::pair<Ref<Framebuffer>, RHI::ResourceHandle>{ drawnTarget, queries[1] }
            };
            for (auto& [target, query] : chains)
            {
                target->Bind();
                RenderCommand::SetViewport(0, 0, kSize, kSize);
                drawPattern(blackTexture); // unconditional baseline
                RenderCommand::BeginConditionalRender(query);
                drawPattern(redTexture);
                RenderCommand::EndConditionalRender();
                target->Unbind();
            }

            for (const auto& target : { skippedTarget, drawnTarget })
            {
                RHI::Barrier toSampled{};
                toSampled.Resource = target->GetColorAttachmentHandle(0);
                toSampled.Before = RHI::Access::ColorAttachmentWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            }
        });

    EXPECT_EQ(api.GetConditionallySkippedDrawsThisRecording(), 1u)
        << "exactly the draw predicated on the fully-occluded query";
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 3u) << "two baselines + the one un-predicated draw";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u) << "a conditional skip is not a drop";

    {
        std::vector<u8> skipped;
        std::vector<u8> drawn;
        auto* vkSkipped = static_cast<VulkanFramebuffer*>(skippedTarget.Raw());
        auto* vkDrawn = static_cast<VulkanFramebuffer*>(drawnTarget.Raw());
        ASSERT_TRUE(vkSkipped->GetColorAttachmentImage(0)->GetData(skipped, 0));
        ASSERT_TRUE(vkDrawn->GetColorAttachmentImage(0)->GetData(drawn, 0));
        const sizet centre = ((static_cast<sizet>(64) * kSize) + 64) * 4;
        EXPECT_EQ(skipped[centre + 0], 0) << "the occluded predicate must have skipped the red draw";
        EXPECT_EQ(drawn[centre + 0], 255) << "the visible predicate must have let the red draw through";
        EXPECT_EQ(drawn[centre + 1], 0);
    }

    // Query pools are device objects: retire them before the fixture's device
    // teardown or the object tracker reports the leak as a validation error.
    vkDeviceWaitIdle(m_Device->GetDevice());
    RenderCommand::DeleteQueries(queries);
    EXPECT_FALSE(RenderCommand::IsQueryResultAvailable(queries[0]))
        << "a deleted query's handle must be stale, never a silent zero";

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "the query family must ride real VkQueryPool implementations, not stubs";
}

// =============================================================================
// SceneRenderPass (#691 Wave C item 10, first half): the Deferred FLOOR.
//
// The full opaque bucket is DISPROPORTIONATE here and this is exactly what it
// would take: CommandDispatch::DrawMesh reads Renderer3D's own per-frame
// objects (s_Data.CameraUBO / MaterialUBO / BoneMatricesUBO, the SSBO-15
// InstanceBuffer) through CommandDispatch::SetUBOReferences, and mid-suite
// those are live GL-currency objects with no setter to re-home them onto this
// device — so a packet-path bucket would bind foreign handles over the
// fixture's Vulkan ones. Seeding it needs either a Renderer3D re-home on
// Vulkan (the ParticleBatchRenderer discipline, but for the whole renderer) or
// a fixture dispatch like the decal tenant's; the material half additionally
// wants CommandDispatch::UploadMaterialForDirectDraw. Documented, not faked.
//
// What this tenant DOES run is the rest of SceneRenderPass::Execute, unmodified
// and in the real graph, on the Deferred path with DebugChannel 3:
//   * the deferred resource preparation — a real 6-attachment GBuffer created
//     by the pass itself at the Init spec's size;
//   * BOTH clears the pass owes (the scene FB's, which exists so a Forward ->
//     Deferred switch cannot leave stale entity-ID / normal attachments, and
//     the G-Buffer's);
//   * BlitGBufferDebug(3) — the RMA channel, which is a REAL fullscreen draw
//     (DebugGBuffer_RMA.glsl, whose V3 pull branch is this batch's sibling
//     change) narrowed onto attachment 0 by SetFramebufferDrawAttachments,
//     followed by RestoreAllFramebufferDrawAttachments and a depth
//     BlitFramebuffer from the G-Buffer;
//   * the pass's shader/VAO unbind hygiene (BindShaderProgram(NullResource)).
//
// The contract is arithmetic, not "it drew something": the RMA shader gathers
// (RT1.z, RT0.a, RT1.w) = (roughness, metallic, ao), and the G-Buffer clear the
// pass itself performs is (0.1, 0.1, 0.1, 1.0) — so attachment 0 must come out
// (0.1, 1.0, 1.0) exactly. A narrowing that failed to restore, a blit that hit
// the wrong attachment, or a G-Buffer that was never cleared all move it.
// =============================================================================
TEST_F(VulkanPassSuite, ScenePassDeferredFloorClearsTheGBufferAndBlitsTheRmaDebugChannel)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

    // The pass branches on process-global renderer settings; force the
    // Deferred + RMA-debug configuration and restore on exit.
    auto& settings = Renderer3D::GetRendererSettings();
    const RenderingPath prevPath = settings.Path;
    const u32 prevDebugChannel = settings.Deferred.DebugChannel;
    const u32 prevSamples = settings.Deferred.MSAASampleCount;
    const bool prevPerSample = settings.Deferred.PerSampleLighting;
    settings.Path = RenderingPath::Deferred;
    settings.Deferred.DebugChannel = 3; // the RMA gather — the one channel that is a DRAW
    settings.Deferred.MSAASampleCount = 1;
    settings.Deferred.PerSampleLighting = false;

    // The scene FB the graph hands the pass: colour + an entity-ID attachment
    // (so the "clear the scene FB even in Deferred" half has something to
    // prove) + depth for the debug blit's destination.
    FramebufferSpecification sceneSpec;
    sceneSpec.Width = kSize;
    sceneSpec.Height = kSize;
    sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER,
                              FramebufferTextureFormat::Depth };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
    ASSERT_TRUE(sceneFramebuffer);

    auto scenePass = Ref<SceneRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER,
                             FramebufferTextureFormat::Depth };
    scenePass->Init(initSpec);

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
    // Scene.SceneDepth / GBuffer.Velocity are deliberately left unset: those
    // export slots lower to CopyImageSubData, and a D24S8 -> RGBA8 copy is a
    // format-incompatible transfer (a depth/stencil source demands an exactly
    // matching destination). Exporting into real depth-format transients is
    // the graph's production shape and a separate concern from this floor.

    graph.AddNode(scenePass);
    graph.SetFinalPass("SceneRenderPass");
    graph.BuildFrameGraph();

    SubmitFrame(
        [&]()
        {
            graph.Execute();

            RHI::Barrier colorToSampled{};
            colorToSampled.Resource = sceneFramebuffer->GetColorAttachmentHandle(0);
            colorToSampled.Before = RHI::Access::ColorAttachmentWrite;
            colorToSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &colorToSampled, 1 });

            RHI::Barrier entityToSampled{};
            entityToSampled.Resource = sceneFramebuffer->GetColorAttachmentHandle(1);
            entityToSampled.Before = RHI::Access::TransferWrite; // its last write is the pass's clear
            entityToSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &entityToSampled, 1 });
        });

    EXPECT_TRUE(scenePass->GetTarget()) << "the pass early-returned before resolving its target";
    ASSERT_TRUE(scenePass->GetGBuffer()) << "the Deferred path must have created a G-Buffer";
    EXPECT_EQ(scenePass->GetGBuffer()->GetWidth(), kSize);
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "ScenePass resolve failure: pass='" << failure.PassName << "' reason='" << failure.Reason
                      << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 1u) << "exactly the RMA debug gather";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    std::vector<u8> rendered;
    auto* vkScene = static_cast<VulkanFramebuffer*>(sceneFramebuffer.Raw());
    ASSERT_TRUE(vkScene->GetColorAttachmentImage(0)->GetData(rendered, 0));
    ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4);
    for (const auto& [x, y] : { std::pair<u32, u32>{ 8, 8 }, { 64, 64 }, { 120, 120 } })
    {
        const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
        EXPECT_NEAR(rendered[i + 0], 26, 1) << "RMA red = roughness = the G-Buffer RT1.z clear (0.1) at (" << x
                                            << "," << y << ")";
        EXPECT_EQ(rendered[i + 1], 255) << "RMA green = metallic = the G-Buffer RT0.a clear (1.0)";
        EXPECT_EQ(rendered[i + 2], 255) << "RMA blue = ao = the G-Buffer RT1.w clear (1.0)";
    }

    std::vector<u8> entityBytes;
    ASSERT_TRUE(vkScene->GetColorAttachmentImage(1)->GetData(entityBytes, 0));
    const auto* entityIds = reinterpret_cast<const i32*>(entityBytes.data());
    EXPECT_EQ(entityIds[static_cast<sizet>(64) * kSize + 64], -1)
        << "the Deferred path still clears the SCENE framebuffer's non-colour attachments";

    settings.Path = prevPath;
    settings.Deferred.DebugChannel = prevDebugChannel;
    settings.Deferred.MSAASampleCount = prevSamples;
    settings.Deferred.PerSampleLighting = prevPerSample;
}

// =============================================================================
// Wave C item 11 — SHADOW: the layered depth path + the skinned two-stream pull
// =============================================================================
namespace
{
    // The V2 bone stream (32 B: uvec4 BoneIDs @0, vec4 Weights @16) as a
    // SECOND vertex buffer on the same VAO — exactly what
    // MeshSource::BuildBoneInfluenceBuffer builds, so it lands on VAO stream 1
    // and AssembleAndPushRootData resolves it for pull binding 63.
    // Ref<T> const-propagates, so a const& would make AddVertexBuffer unreachable.
    void AddBoneStream(Ref<VertexArray>& vao, u32 vertexCount, u32 boneIndex)
    {
        struct BoneVertex
        {
            u32 BoneIDs[4];
            f32 Weights[4];
        };
        static_assert(sizeof(BoneVertex) == 32, "V2 stride is 32 bytes / 8 floats");
        std::vector<BoneVertex> influences(vertexCount);
        for (auto& influence : influences)
        {
            influence.BoneIDs[0] = boneIndex;
            influence.BoneIDs[1] = 0u;
            influence.BoneIDs[2] = 0u;
            influence.BoneIDs[3] = 0u;
            influence.Weights[0] = 1.0f;
            influence.Weights[1] = 0.0f;
            influence.Weights[2] = 0.0f;
            influence.Weights[3] = 0.0f;
        }
        auto boneVB = VertexBuffer::Create(reinterpret_cast<f32*>(influences.data()),
                                           static_cast<u32>(influences.size() * sizeof(BoneVertex)));
        boneVB->SetLayout({ { ShaderDataType::Int4, "a_BoneIDs" }, { ShaderDataType::Float4, "a_BoneWeights" } });
        vao->AddVertexBuffer(boneVB);
    }

    // Read ONE layer of a depth texture array back to the host. No engine
    // readback exists for a Texture2DArray and none is added for a test: this
    // is VulkanTexture2D::GetData's body with the layer in the copy's
    // subresource, and it assumes the caller left the array in
    // SHADER_READ_ONLY (the tenant issues that barrier inside its frame, so
    // the engine's layout tracker agrees).
    bool ReadDepthArrayLayer(const Ref<Texture2DArray>& array, u32 layer, std::vector<f32>& outDepth)
    {
        outDepth.clear();
        auto* device = VulkanDevice::Get();
        if (device == nullptr || !array)
            return false;
        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(array->GetRHIHandle());
        if (native == 0u)
            return false;
        const auto image = reinterpret_cast<VkImage>(native);
        const u32 width = array->GetWidth();
        const u32 height = array->GetHeight();
        const u64 sizeBytes = static_cast<u64>(width) * height * sizeof(f32);

        VkBufferCreateInfo readbackInfo{};
        readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        readbackInfo.size = sizeBytes;
        readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo readbackAlloc{};
        readbackAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        readbackAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo readbackOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &readbackInfo, &readbackAlloc, &readback, &readbackAllocation,
                            &readbackOut) != VK_SUCCESS)
        {
            return false;
        }

        const auto recordBarrier = [&](VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout)
        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, layer, 1u };
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        };

        const bool ok = VulkanOneShot::Submit(
            "VulkanPassSuite::ReadDepthArrayLayer",
            [&](VkCommandBuffer cmd)
            {
                recordBarrier(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, layer, 1u };
                region.imageExtent = { width, height, 1u };
                vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);
                recordBarrier(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });

        if (ok)
        {
            vmaInvalidateAllocation(device->GetAllocator(), readbackAllocation, 0, sizeBytes);
            outDepth.resize(static_cast<sizet>(width) * height);
            std::memcpy(outDepth.data(), readbackOut.pMappedData, sizeBytes);
        }
        vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
        return ok;
    }
} // namespace

// The §4 layered-depth contract, and the A3 two-stream skinned pull that rides
// with it. ONE depth-only framebuffer walks TWO layers of a 2-layer depth array
// exactly as ShadowRenderPass walks its cascades: AttachDepthTextureArrayLayer,
// ClearDepthOnly, draw. Cascade 0 draws through ShadowDepth.glsl (V1 pull only);
// cascade 1 through ShadowDepthSkinned.glsl, whose vertices are skinned by BONE
// 1 — a pure +1.0 x translation. Both cascades author the SAME quad on the LEFT
// half, so:
//   * a stubbed AttachDepthTextureArrayLayer leaves both layers at the clear
//     value (nothing renders into the array at all),
//   * a scope that survives the layer change paints cascade 1 into cascade 0,
//   * an unfed bone stream (binding 63 -> zero address) reads bone id 0, the
//     IDENTITY palette entry, and leaves cascade 1's quad on the LEFT.
// The assertions therefore read both halves of both layers.
TEST_F(VulkanPassSuite, ShadowCascadesRenderIntoTheirOwnDepthArrayLayers)
{
    constexpr u32 kSize = 64;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto shadowShader = Shader::Create("assets/shaders/ShadowDepth.glsl");
    auto skinnedShader = Shader::Create("assets/shaders/ShadowDepthSkinned.glsl");
    ASSERT_TRUE(shadowShader && skinnedShader);
    ASSERT_EQ(shadowShader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
        << "ShadowDepth.glsl must compile through shaderc (V1 pull branch)";
    ASSERT_EQ(skinnedShader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
        << "ShadowDepthSkinned.glsl must compile through shaderc (V1 + V2 two-stream pull)";
    // The other four skinned shaders ride the identical branch — compile-pinned
    // here so a broken bone-stream declaration cannot reach a live scene
    // unnoticed (they need G-buffer/forward targets a shadow tenant has not).
    for (const char* path : { "assets/shaders/PBR_MultiLight_Skinned.glsl",
                              "assets/shaders/PBR_GBuffer_Skinned.glsl",
                              "assets/shaders/DepthPrepass_Skinned.glsl",
                              "assets/shaders/DepthPrepass_MaskSkinned.glsl" })
    {
        auto skinned = Shader::Create(path);
        ASSERT_TRUE(skinned) << path;
        EXPECT_EQ(skinned->GetCompilationStatus(), ShaderCompilationStatus::Ready)
            << path << " must compile through shaderc (V1 @57 + V2 @63)";
    }

    // --- every binding the two shaders declare -------------------------------
    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    cameraData.PrevViewProjection = glm::mat4(1.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    const InstanceData identityInstance{};
    auto instanceSSBO = StorageBuffer::Create(sizeof(InstanceData), ShaderBindingLayout::SSBO_INSTANCE_DATA);
    instanceSSBO->SetData(&identityInstance, sizeof(identityInstance));
    instanceSSBO->Bind();

    // AnimationMatrices (binding 4): 100 mat4. Entry 0 is the IDENTITY (the
    // value a zero-address bone pull would select), entry 1 translates +1.0 in
    // x — one full NDC half — so a correctly pulled bone id lands the quad on
    // the RIGHT and a dropped one leaves it on the LEFT.
    std::vector<glm::mat4> bonePalette(100, glm::mat4(1.0f));
    bonePalette[1] = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    auto boneUbo = UniformBuffer::Create(static_cast<u32>(bonePalette.size() * sizeof(glm::mat4)),
                                         ShaderBindingLayout::UBO_ANIMATION);
    boneUbo->SetData(bonePalette.data(), static_cast<u32>(bonePalette.size() * sizeof(glm::mat4)));

    // Both cascades author the same LEFT-half quad; the skinned one is moved
    // right by bone 1. Different depths so a layer that received the wrong
    // cascade is visible in the value as well as the position.
    constexpr f32 kStaticDepth = 0.25f;
    constexpr f32 kSkinnedDepth = 0.75f;
    auto staticQuad = MakeV1Quad(-1.0f, 0.0f, -1.0f, 1.0f, kStaticDepth);
    auto skinnedQuad = MakeV1Quad(-1.0f, 0.0f, -1.0f, 1.0f, kSkinnedDepth);
    ASSERT_TRUE(staticQuad && skinnedQuad);
    AddBoneStream(skinnedQuad, 4u, 1u);

    // The layered depth array + the depth-only framebuffer that walks it —
    // ShadowRenderPass::Init's shape verbatim (ShadowDepth attachment, the
    // internal depth texture the per-layer attach replaces).
    Texture2DArraySpecification arraySpec;
    arraySpec.Width = kSize;
    arraySpec.Height = kSize;
    arraySpec.Layers = 2;
    arraySpec.Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
    arraySpec.DepthComparisonMode = true;
    auto cascades = Texture2DArray::Create(arraySpec);
    ASSERT_TRUE(cascades);
    ASSERT_EQ(cascades->GetLayers(), 2u);

    FramebufferSpecification shadowSpec;
    shadowSpec.Width = kSize;
    shadowSpec.Height = kSize;
    shadowSpec.Attachments = { FramebufferTextureFormat::ShadowDepth };
    Ref<Framebuffer> shadowFramebuffer = Framebuffer::Create(shadowSpec);
    ASSERT_TRUE(shadowFramebuffer);

    SubmitFrame(
        [&]()
        {
            shadowFramebuffer->Bind();
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetBlendState(false);
            // The production pass front-face-culls; the tenant's quads are
            // authored in raw clip space with no projection seam applied, so
            // culling is off (the seam's global winding flip would otherwise
            // decide the result rather than the layer selection).
            RenderCommand::DisableCulling();
            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
            RenderCommand::SetDepthMask(true);

            // --- cascade 0: static caster ------------------------------------
            shadowFramebuffer->AttachDepthTextureArrayLayer(cascades->GetRHIHandle(), 0u);
            RenderCommand::ClearDepthOnly();
            shadowShader->Bind();
            staticQuad->Bind();
            RenderCommand::DrawIndexed(staticQuad, 6);

            // --- cascade 1: skinned caster -----------------------------------
            shadowFramebuffer->AttachDepthTextureArrayLayer(cascades->GetRHIHandle(), 1u);
            RenderCommand::ClearDepthOnly();
            skinnedShader->Bind();
            skinnedQuad->Bind();
            RenderCommand::DrawIndexed(skinnedQuad, 6);

            // Hand both layers to the readback in the layout its one-shot
            // assumes, through the engine's own barrier path so the layout
            // tracker stays true.
            std::array<RHI::Barrier, 2> toSampled{};
            for (u32 layer = 0; layer < 2u; ++layer)
            {
                toSampled[layer].Resource = cascades->GetRHIHandle();
                toSampled[layer].Range.BaseMip = 0u;
                toSampled[layer].Range.MipCount = 1u;
                toSampled[layer].Range.BaseLayer = layer;
                toSampled[layer].Range.LayerCount = 1u;
                toSampled[layer].Before = RHI::Access::DepthStencilAttachmentWrite;
                toSampled[layer].After = RHI::Access::ShaderSampleRead;
            }
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ toSampled });
        });

    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 2u) << "one draw per cascade";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u) << "a cascade draw dropped silently";
    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "AttachDepthTextureArrayLayer must no longer be a Phase 6 stub";

    std::vector<f32> cascade0;
    std::vector<f32> cascade1;
    ASSERT_TRUE(ReadDepthArrayLayer(cascades, 0u, cascade0)) << "layer 0 readback failed";
    ASSERT_TRUE(ReadDepthArrayLayer(cascades, 1u, cascade1)) << "layer 1 readback failed";
    ASSERT_EQ(cascade0.size(), static_cast<sizet>(kSize) * kSize);
    ASSERT_EQ(cascade1.size(), static_cast<sizet>(kSize) * kSize);

    const auto sample = [&](const std::vector<f32>& layer, u32 x, u32 y)
    { return layer[static_cast<sizet>(y) * kSize + x]; };
    constexpr u32 kLeftX = kSize / 4; // inside the authored quad
    constexpr u32 kRightX = 3 * kSize / 4;
    constexpr u32 kRow = kSize / 2;

    // Layer 0 carries ONLY the static cascade: near on the left, cleared right.
    EXPECT_NEAR(sample(cascade0, kLeftX, kRow), kStaticDepth, 1e-4f)
        << "cascade 0's left half must carry the static caster's depth";
    EXPECT_NEAR(sample(cascade0, kRightX, kRow), 1.0f, 1e-4f)
        << "cascade 0's right half must stay at the ClearDepthOnly value — a cascade-1 draw that leaked "
           "into layer 0 (a scope surviving the layer change) lands exactly here";

    // Layer 1 carries ONLY the skinned cascade, translated to the RIGHT half by
    // bone 1. Its left half must be clear: that is the assertion the two-stream
    // pull actually earns — a zero-address bone buffer selects bone 0
    // (identity) and would leave the quad on the left instead.
    EXPECT_NEAR(sample(cascade1, kRightX, kRow), kSkinnedDepth, 1e-4f)
        << "cascade 1's right half must carry the SKINNED caster — bone 1's +1.0 x translation, i.e. the "
           "V2 bone stream really arrived on pull binding 63";
    EXPECT_NEAR(sample(cascade1, kLeftX, kRow), 1.0f, 1e-4f)
        << "cascade 1's left half must be clear — a dropped bone stream reads bone id 0 (identity) and "
           "would leave the quad here";
}

// =============================================================================
// Wave C item 12 — PLANAR REFLECTION: the mirror camera through the Y-flip seam
// =============================================================================
//
// PlanarReflectionRenderPass swaps the GLOBAL camera to a mirrored one (oblique
// near-clip included) and overrides the front winding to Clockwise for the
// replay. On Vulkan that composes with TWO backend conventions at once:
//   * the A8 projection seam (RHI::AdjustProjectionForBackend) negates clip y
//     for EVERY rasterizer-consumed matrix, the mirror camera's included;
//   * the A1 winding half — VulkanPipelineBuilder translates the RECORDED GL
//     winding to its OPPOSITE VkFrontFace, so the pass-local Clockwise
//     override must flip WITH the seam, not against it.
// Get either wrong and the reflection lands in the wrong half of the screen or
// is culled outright. This tenant pins both.
//
// Configuration chosen so "the mirror is the vertical mirror" is EXACT rather
// than approximate: the camera sits ON the reflection plane (y = 0) looking
// horizontally, which makes the mirrored view the y-negation of the direct one
// in view space — and MakeObliqueProjection only rewrites the clip-Z row, so
// the XY layout is untouched by the oblique clip. An asymmetric caster (upper
// LEFT, off-centre in both axes) makes every wrong composition — a missing
// flip, a doubled flip, a transposed one — land somewhere the assertion sees.
//
// Both renders target LAYERS of one depth array through the item-11 layered
// path, so the coverage readback is the same helper and the two images cannot
// differ by anything but the camera + winding.
TEST_F(VulkanPassSuite, PlanarReflectionMirrorCameraProducesTheVerticallyMirroredImage)
{
    constexpr u32 kSize = 64;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto depthShader = Shader::Create("assets/shaders/ShadowDepth.glsl");
    ASSERT_TRUE(depthShader);
    ASSERT_EQ(depthShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    // Camera ON the plane, looking down -z with +y up.
    const glm::vec3 cameraPosition{ 0.0f, 0.0f, 3.0f };
    const glm::mat4 view = glm::lookAt(cameraPosition, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    const glm::vec4 waterPlane{ 0.0f, 1.0f, 0.0f, 0.0f };
    const auto mirror = PlanarReflection::BuildReflectionMatrices(view, projection, cameraPosition, waterPlane);

    const InstanceData identityInstance{};
    auto instanceSSBO = StorageBuffer::Create(sizeof(InstanceData), ShaderBindingLayout::SSBO_INSTANCE_DATA);
    instanceSSBO->SetData(&identityInstance, sizeof(identityInstance));
    instanceSSBO->Bind();

    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    const auto uploadCamera = [&](const glm::mat4& viewMatrix, const glm::mat4& viewProjection)
    {
        // Byte-for-byte what CommandDispatch::UploadCameraUBO writes for the
        // mirror camera (the pass's own path): the stored matrices stay
        // GL-convention, the GPU-visible copies go through the A8 seam.
        ShaderBindingLayout::CameraUBO cameraData{};
        cameraData.ViewProjection = RHI::AdjustProjectionForBackend(viewProjection);
        cameraData.View = viewMatrix;
        cameraData.Projection = RHI::AdjustProjectionForBackend(projection);
        cameraData.PrevViewProjection = cameraData.ViewProjection;
        cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
    };

    // The asymmetric caster: a quad ABOVE the water, offset LEFT, entirely
    // inside the direct view's upper-left quadrant.
    auto caster = MakeV1Quad(-0.9f, -0.1f, 0.3f, 0.9f, 0.0f);
    ASSERT_TRUE(caster);

    // Four renders of ONE caster into four layers of one depth array. The pairs
    // isolate the two halves of the composition from each other:
    //   0/1 = direct / mirror with culling OFF  -> the A8 projection half
    //   2/3 = the same two with culling ON      -> the A1 winding half
    // so a failure names which half broke instead of just "nothing drew".
    constexpr u32 kDirectNoCull = 0;
    constexpr u32 kMirrorNoCull = 1;
    constexpr u32 kDirectCulledCCW = 2;
    constexpr u32 kDirectCulledCW = 3;
    constexpr u32 kMirrorCulledCCW = 4;
    constexpr u32 kMirrorCulledCW = 5;
    constexpr u32 kLayers = 6;

    Texture2DArraySpecification arraySpec;
    arraySpec.Width = kSize;
    arraySpec.Height = kSize;
    arraySpec.Layers = kLayers;
    arraySpec.Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
    auto images = Texture2DArray::Create(arraySpec);
    ASSERT_TRUE(images);

    FramebufferSpecification depthSpec;
    depthSpec.Width = kSize;
    depthSpec.Height = kSize;
    depthSpec.Attachments = { FramebufferTextureFormat::ShadowDepth };
    Ref<Framebuffer> depthFramebuffer = Framebuffer::Create(depthSpec);
    ASSERT_TRUE(depthFramebuffer);

    SubmitFrame(
        [&]()
        {
            depthFramebuffer->Bind();
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetBlendState(false);
            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
            RenderCommand::SetDepthMask(true);
            depthShader->Bind();

            const auto renderInto = [&](u32 layer, const glm::mat4& viewMatrix, const glm::mat4& viewProjection,
                                        bool culling, RHI::FrontFace winding)
            {
                if (culling)
                {
                    RenderCommand::EnableCulling();
                    RenderCommand::SetCullFace(RHI::CullMode::Back);
                }
                else
                {
                    RenderCommand::DisableCulling();
                }
                RenderCommand::SetFrontFace(winding);
                uploadCamera(viewMatrix, viewProjection);
                depthFramebuffer->AttachDepthTextureArrayLayer(images->GetRHIHandle(), layer);
                RenderCommand::ClearDepthOnly();
                caster->Bind();
                RenderCommand::DrawIndexed(caster, 6);
            };

            renderInto(kDirectNoCull, view, projection * view, false, RHI::FrontFace::CounterClockwise);
            renderInto(kMirrorNoCull, mirror.MirrorView, mirror.ViewProjection, false,
                       RHI::FrontFace::CounterClockwise);
            // Both windings under culling, for both cameras: a triangle is
            // either front or back, so the PAIR is a complete statement — one
            // of each pair must survive and the other must vanish. Asserting
            // WHICH one is what pins the A1 composition; asserting only
            // "something survived" would pass with the flip inverted.
            renderInto(kDirectCulledCCW, view, projection * view, true, RHI::FrontFace::CounterClockwise);
            renderInto(kDirectCulledCW, view, projection * view, true, RHI::FrontFace::Clockwise);
            renderInto(kMirrorCulledCCW, mirror.MirrorView, mirror.ViewProjection, true,
                       RHI::FrontFace::CounterClockwise);
            // A reflection reverses handedness, so the replay declares CW the
            // front winding — PlanarReflectionRenderPass's exact call.
            renderInto(kMirrorCulledCW, mirror.MirrorView, mirror.ViewProjection, true,
                       RHI::FrontFace::Clockwise);
            RenderCommand::DisableCulling();
            RenderCommand::SetFrontFace(RHI::FrontFace::CounterClockwise);

            std::array<RHI::Barrier, kLayers> toSampled{};
            for (u32 layer = 0; layer < kLayers; ++layer)
            {
                toSampled[layer].Resource = images->GetRHIHandle();
                toSampled[layer].Range.BaseMip = 0u;
                toSampled[layer].Range.MipCount = 1u;
                toSampled[layer].Range.BaseLayer = layer;
                toSampled[layer].Range.LayerCount = 1u;
                toSampled[layer].Before = RHI::Access::DepthStencilAttachmentWrite;
                toSampled[layer].After = RHI::Access::ShaderSampleRead;
            }
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ toSampled });
        });

    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), kLayers);
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);
    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore);

    std::array<std::vector<f32>, kLayers> layers;
    for (u32 layer = 0; layer < kLayers; ++layer)
    {
        ASSERT_TRUE(ReadDepthArrayLayer(images, layer, layers[layer])) << "layer " << layer << " readback failed";
        ASSERT_EQ(layers[layer].size(), static_cast<sizet>(kSize) * kSize);
    }

    // Coverage, not depth: the oblique near-clip rewrites the clip-Z row, so
    // the mirror's depth VALUES are deliberately warped while its XY footprint
    // is the exact mirror.
    const auto covered = [&](u32 layer, u32 x, u32 y)
    { return layers[layer][static_cast<sizet>(y) * kSize + x] < 0.999f; };
    const auto coveredCount = [&](u32 layer)
    {
        u32 count = 0;
        for (u32 y = 0; y < kSize; ++y)
            for (u32 x = 0; x < kSize; ++x)
                count += covered(layer, x, y) ? 1u : 0u;
        return count;
    };

    // Baseline: the direct render must draw SOMETHING, or every comparison
    // below is vacuous.
    // The caster projects to roughly 15 x 11 texels at this camera; anything
    // above ~100 covered texels means the quad really rasterized.
    constexpr u32 kMinCoverage = 100u;
    EXPECT_GT(coveredCount(kDirectNoCull), kMinCoverage) << "the direct render drew nothing";
    EXPECT_GT(coveredCount(kMirrorNoCull), kMinCoverage) << "the mirror render drew nothing";

    // Where the caster landed pins the A8 half INDEPENDENTLY of the mirror
    // comparison (which is symmetric and would pass with or without the flip).
    // The caster is ABOVE the plane and the camera looks horizontally, so a
    // seam-flipped matrix puts it in the FIRST half of memory: Vulkan's memory
    // row 0 is ndc_y = -1, and the flip sends world +y to negative ndc_y.
    u32 directMinRow = kSize;
    u32 directMaxRow = 0;
    for (u32 y = 0; y < kSize; ++y)
        for (u32 x = 0; x < kSize; ++x)
            if (covered(kDirectNoCull, x, y))
            {
                directMinRow = std::min(directMinRow, y);
                directMaxRow = std::max(directMaxRow, y);
            }
    EXPECT_LT(directMaxRow, kSize / 2u)
        << "the caster occupies rows " << directMinRow << ".." << directMaxRow
        << "; above the horizon must mean the FIRST half of memory, or the A8 projection flip never "
           "reached this matrix";

    // (1) The A8 half: the mirrored image is the exact vertical mirror.
    u32 mirrorMismatches = 0;
    for (u32 y = 0; y < kSize; ++y)
        for (u32 x = 0; x < kSize; ++x)
            mirrorMismatches += (covered(kDirectNoCull, x, y) != covered(kMirrorNoCull, x, kSize - 1u - y)) ? 1u : 0u;
    EXPECT_EQ(mirrorMismatches, 0u)
        << "the mirrored image must be the exact vertical mirror of the direct one — " << mirrorMismatches
        << " of " << (kSize * kSize)
        << " pixels disagree, which is what a wrong Y-flip composition on the mirror camera looks like";

    // ...and the two are genuinely DIFFERENT images (a seam that dropped the
    // mirror matrix would render the caster twice in the same place, and a
    // self-mirror-symmetric result would satisfy (1) vacuously).
    u32 identicalRows = 0;
    for (u32 y = 0; y < kSize; ++y)
    {
        bool same = true;
        for (u32 x = 0; x < kSize && same; ++x)
            same = covered(kDirectNoCull, x, y) == covered(kMirrorNoCull, x, y);
        identicalRows += same ? 1u : 0u;
    }
    EXPECT_LT(identicalRows, kSize) << "every row identical => the mirror camera was never applied";

    // (2) The A1 half: culling ON must change NOTHING for either camera. The
    // caster is front-facing to the direct camera under the recorded CCW
    // winding, and front-facing to the mirror camera under the pass's
    // Clockwise override — the backend composes each with the seam's own flip.
    // Invert that composition and the quad becomes a back face and vanishes.
    const auto delta = [&](u32 a, u32 b)
    {
        u32 count = 0;
        for (u32 y = 0; y < kSize; ++y)
            for (u32 x = 0; x < kSize; ++x)
                count += (covered(a, x, y) != covered(b, x, y)) ? 1u : 0u;
        return count;
    };

    // The caster is authored counter-clockwise in world space, so it is front
    // facing to the DIRECT camera under the recorded CounterClockwise winding —
    // the engine's default and what every solid pass records. The reflection
    // reverses handedness, so the SAME caster is front facing to the MIRROR
    // camera only under PlanarReflectionRenderPass's Clockwise override. The
    // backend composes each with the seam's own front-face flip; invert that
    // composition and all four of these assertions swap.
    EXPECT_EQ(delta(kDirectCulledCCW, kDirectNoCull), 0u)
        << "back-face culling removed the DIRECT caster under the recorded CounterClockwise winding: the "
           "A1 front-face composition is inverted (observed CW coverage "
        << coveredCount(kDirectCulledCW) << ")";
    EXPECT_EQ(coveredCount(kDirectCulledCW), 0u)
        << "the DIRECT caster survived a Clockwise front winding — it must be a BACK face there";
    EXPECT_EQ(delta(kMirrorCulledCW, kMirrorNoCull), 0u)
        << "back-face culling removed the MIRRORED caster under PlanarReflectionRenderPass's Clockwise "
           "override (observed CCW coverage "
        << coveredCount(kMirrorCulledCCW) << ")";
    EXPECT_EQ(coveredCount(kMirrorCulledCCW), 0u)
        << "the MIRRORED caster survived a CounterClockwise front winding — the reflection's handedness "
           "reversal did not reach the rasterizer";
}

// =============================================================================
// Wave C item 13 — the GPU-OCCLUSION PAIR: two-phase indirect
// =============================================================================
//
// GPUDrivenOcclusionPass / DeferredGPUOcclusionPass share one shape: phase 1
// rasterizes the survivors, a TextureBarrier orders that framebuffer write
// against the following texture FETCH, a compute cull re-tests the rejects
// against the freshly-written depth and writes the phase-2 DrawElementsIndirect
// args, and phase 2 replays through those args. This tenant pins that shape on
// Vulkan end-to-end: RenderCommand::TextureBarrier, a GPU-authored args buffer,
// and DrawBoundElementsIndirect honouring an instanceCount the GPU decided.
//
// The production cull (assets/shaders/compute/InstanceOcclusionCull.comp) is
// NOT the compute stage here. It DOES compile on Vulkan now — its 15 bare
// default-block uniforms moved into the std140 InstanceCullParams block at
// binding 71 in this batch, and Phase7MigratedComputeShadersCompileOnVulkan
// asserts exactly that — so the original reason (SPIR-V cannot express a bare
// uniform) no longer applies. What still keeps it out is that driving it needs
// the instance-buffer plumbing this device-local fixture has no producer for.
// The stand-in below makes the SAME decision from the SAME input (phase 1's
// depth attachment, sampled after the barrier) and writes the SAME
// DrawElementsIndirectCommand POD, so what is under test — the backend
// entry points and the ordering — is the production path; only the cull's
// arithmetic is stubbed. Driving the real cull here is a Phase 8a follow-up.
//
// Two chains, differing ONLY in where phase 1's occluder sits, so the contract
// is a paired differential rather than an absolute:
//   * OCCLUDED: the occluder covers the probe -> the cull writes instanceCount
//     0 -> phase 2's indirect draw issues and paints NOTHING.
//   * VISIBLE:  the occluder sits on the other half -> instanceCount 1 -> the
//     same indirect draw paints the pattern.
// An indirect entry that ignored its args would paint in both (or neither).
TEST_F(VulkanPassSuite, GpuOcclusionPhaseTwoDrawsThroughGpuAuthoredIndirectArgs)
{
    constexpr u32 kSize = 64;
    constexpr u32 kProbeX = kSize / 4; // inside the LEFT half
    constexpr u32 kProbeY = kSize / 2;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto depthShader = Shader::Create("assets/shaders/DepthPrepass.glsl");
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(depthShader && blitShader);
    ASSERT_EQ(depthShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    const std::string cullSource = std::format(R"(#version 450 core
layout(local_size_x = 1) in;

// Phase 1's depth attachment, sampled AFTER RenderCommand::TextureBarrier().
layout(binding = 0) uniform sampler2D u_Phase1Depth;

// The phase-2 args. Field order is OloEngine::DrawElementsIndirectCommand ==
// VkDrawIndexedIndirectCommand, field for field (no translation on either
// backend).
layout(std430, binding = 17) buffer Phase2Args
{{
    uint IndexCount;
    uint InstanceCount;
    uint FirstIndex;
    uint BaseVertex;
    uint BaseInstance;
}} b_Args;

void main()
{{
    // "Did phase 1 already cover this probe?" — the reduced form of the
    // production reject test against the freshly built Hi-Z.
    float d = texelFetch(u_Phase1Depth, ivec2({}, {}), 0).r;
    b_Args.IndexCount = 3u;
    b_Args.InstanceCount = (d < 0.9) ? 0u : 1u;
    b_Args.FirstIndex = 0u;
    b_Args.BaseVertex = 0u;
    b_Args.BaseInstance = 0u;
}}
)",
                                               kProbeX, kProbeY);
    auto cullShader = ComputeShader::CreateFromSource("Phase2ArgsProbe", cullSource);
    ASSERT_TRUE(cullShader);

    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    cameraData.PrevViewProjection = glm::mat4(1.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    const InstanceData identityInstance{};
    auto instanceSSBO = StorageBuffer::Create(sizeof(InstanceData), ShaderBindingLayout::SSBO_INSTANCE_DATA);
    instanceSSBO->SetData(&identityInstance, sizeof(identityInstance));
    instanceSSBO->Bind();

    // FullscreenBlit reads DRSParams@33 — an unbound UBO would collapse every
    // sample to texel (0,0) (the fixture contract in this file's header).
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));
    auto redTexture = MakeSolidTexture(kSize, 255, 0, 0, 255);
    ASSERT_TRUE(redTexture);

    // The phase-1 occluders: one over the probe, one on the far side.
    auto occluderOverProbe = MakeV1Quad(-1.0f, 0.0f, -1.0f, 1.0f, 0.3f);
    auto occluderElsewhere = MakeV1Quad(0.0f, 1.0f, -1.0f, 1.0f, 0.3f);
    ASSERT_TRUE(occluderOverProbe && occluderElsewhere);

    struct Chain
    {
        Ref<VertexArray> Occluder;
        Ref<Framebuffer> Phase1;
        Ref<Framebuffer> Phase2;
        Ref<StorageBuffer> Args;
        u32 ExpectedInstances;
        const char* Name;
    };

    FramebufferSpecification phase1Spec;
    phase1Spec.Width = kSize;
    phase1Spec.Height = kSize;
    phase1Spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
    FramebufferSpecification phase2Spec;
    phase2Spec.Width = kSize;
    phase2Spec.Height = kSize;
    phase2Spec.Attachments = { FramebufferTextureFormat::RGBA8 };

    std::array<Chain, 2> chains{
        Chain{ occluderOverProbe, Framebuffer::Create(phase1Spec), Framebuffer::Create(phase2Spec),
               StorageBuffer::Create(5u * sizeof(u32), 17u), 0u, "occluded" },
        Chain{ occluderElsewhere, Framebuffer::Create(phase1Spec), Framebuffer::Create(phase2Spec),
               StorageBuffer::Create(5u * sizeof(u32), 17u), 1u, "visible" },
    };
    for (auto& chain : chains)
    {
        ASSERT_TRUE(chain.Phase1 && chain.Phase2 && chain.Args) << chain.Name;
        // Seed the args with a value NEITHER outcome can produce, so a cull
        // dispatch that silently never ran cannot be mistaken for either arm.
        const std::array<u32, 5> poison{ 3u, 7u, 0u, 0u, 0u };
        chain.Args->SetData(poison.data(), static_cast<u32>(poison.size() * sizeof(u32)));
    }

    for (auto& chain : chains)
    {
        SubmitFrame(
            [&]()
            {
                // --- phase 1: rasterize the occluder -------------------------
                chain.Phase1->Bind();
                RenderCommand::SetViewport(0, 0, kSize, kSize);
                RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
                RenderCommand::Clear();
                RenderCommand::SetBlendState(false);
                RenderCommand::DisableCulling();
                RenderCommand::SetDepthTest(true);
                RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
                RenderCommand::SetDepthMask(true);
                depthShader->Bind();
                chain.Occluder->Bind();
                RenderCommand::DrawIndexed(chain.Occluder, 6);
                chain.Phase1->Unbind();

                // The phase-1 draws just wrote this depth through the
                // fixed-function pipeline; the cull samples it as a texture.
                // This is GPUDrivenOcclusionPass's own call at its own place.
                RenderCommand::TextureBarrier();

                // --- the cull: phase 2's args, decided on the GPU ------------
                cullShader->Bind();
                RenderCommand::BindTexture(0, chain.Phase1->GetDepthAttachmentHandle());
                chain.Args->Bind();
                RenderCommand::DispatchCompute(1, 1, 1);
                // SSBO write -> indirect-args fetch.
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

                // --- phase 2: the indirect replay ----------------------------
                chain.Phase2->Bind();
                RenderCommand::SetViewport(0, 0, kSize, kSize);
                RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
                RenderCommand::Clear();
                RenderCommand::SetDepthTest(false);
                RenderCommand::SetDepthMask(false);
                blitShader->Bind();
                RenderCommand::BindTexture(0, redTexture->GetRHIHandle());
                const auto triangle = MeshPrimitives::GetFullscreenTriangle();
                // DrawBoundElementsIndirect draws the RAW-bound vertex array
                // (the packet path's shape) — VertexArray::Bind() is a no-op on
                // this backend, so the handle must be published explicitly or
                // the draw finds no index buffer and drops.
                RenderCommand::BindVertexArrayRaw(triangle->GetRHIHandle());
                RenderCommand::DrawBoundElementsIndirect(chain.Args->GetRHIHandle());
                RenderCommand::BindVertexArrayRaw(RHI::NullResource);
                chain.Phase2->Unbind();

                auto& vkApi = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
                RHI::Barrier toSampled{};
                toSampled.Resource = chain.Phase2->GetColorAttachmentHandle(0);
                toSampled.Before = RHI::Access::ColorAttachmentWrite;
                toSampled.After = RHI::Access::ShaderSampleRead;
                vkApi.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
            });

        EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u) << chain.Name << ": a draw dropped silently";

        std::array<u32, 5> args{};
        chain.Args->GetData(args.data(), static_cast<u32>(args.size() * sizeof(u32)));
        EXPECT_EQ(args[0], 3u) << chain.Name << ": the cull must have written the index count";
        EXPECT_EQ(args[1], chain.ExpectedInstances)
            << chain.Name << ": the cull's instanceCount decision (7 here means the dispatch never ran)";

        std::vector<u8> rendered;
        auto* vkPhase2 = static_cast<VulkanFramebuffer*>(chain.Phase2.Raw());
        ASSERT_TRUE(vkPhase2->GetColorAttachmentImage(0) &&
                    vkPhase2->GetColorAttachmentImage(0)->GetData(rendered, 0))
            << chain.Name << ": phase-2 readback failed";
        ASSERT_EQ(rendered.size(), static_cast<sizet>(kSize) * kSize * 4u);
        const sizet centre = (static_cast<sizet>(kSize / 2) * kSize + kSize / 2) * 4u;
        if (chain.ExpectedInstances == 0u)
        {
            EXPECT_LT(rendered[centre + 0], 8u)
                << "instanceCount 0 must draw NOTHING — an indirect entry that ignored its args paints here";
        }
        else
        {
            EXPECT_GT(rendered[centre + 0], 200u)
                << "instanceCount 1 must draw the pattern — the indirect entry never issued";
        }
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "TextureBarrier / DrawBoundElementsIndirect must no longer be Phase 6 stubs";
}

// =============================================================================
// Wave C item 15 — DDGI: the direction-addressed capture's row convention
// =============================================================================
//
// RHIProjectionSeam.h carried a KNOWN LIMIT from batch 1: a cubemap FACE BAKE
// is addressed by DIRECTION, not by uv, so the seam's clip-y negation stores
// every face ROW-MIRRORED relative to the GL bake while direction->texel
// addressing stays API-identical — the relight then reads the wrong row of the
// right face. DDGIProbeUpdatePass::CaptureProbe is the engine's live instance
// of that shape (six 90-degree faces into one atlas tile each).
//
// The fix is at the PROJECTION, not at the readback and not at the face basis:
// RHI::AdjustCaptureProjectionForBackend applies the seam's z remap WITHOUT its
// y flip, so the stored rows stay byte-identical to the GL bake while depth
// keeps its GL-shaped contents. (Negating the face's UP vector — the first
// thing one reaches for — does NOT work: lookAt derives right =
// cross(forward, up), so it rolls the face 180 degrees instead of mirroring it.
// This tenant caught that, which is why the note is here.)
//
// The tenant renders one face of the capture recipe twice: once through the
// production path and once through the SCREEN seam the capture must not use.
// The contract has two halves, and both are needed:
//   * the two are exact vertical MIRRORS — i.e. the bug is real and, without
//     this pin, would have been silent;
//   * the capture path puts a caster displaced along the face's -up direction
//     in the SECOND half of memory, which is where GL's bake puts it (memory
//     row 0 is clip y = -w on BOTH backends, and the face basis maps world +y
//     to POSITIVE view y here, so ndc_y > 0).
//
// The full pass body is out of reach here for the batch-3 reason: CaptureProbe
// drives Renderer3D/HeapBinding statics and a caster registry that are
// GL-currency mid-suite with no setter. What is under test is the seam
// decision the port turns on, driven through the pass's own lookAt recipe.
TEST_F(VulkanPassSuite, DdgiCaptureFaceBasisCancelsTheSeamRowMirror)
{
    constexpr u32 kSize = 64;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

    auto depthShader = Shader::Create("assets/shaders/ShadowDepth.glsl");
    ASSERT_TRUE(depthShader);
    ASSERT_EQ(depthShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    // DDGIProbeUpdatePass's face 0 (+X) basis, verbatim.
    const glm::vec3 probe{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 faceTarget{ 1.0f, 0.0f, 0.0f };
    const glm::vec3 faceUp{ 0.0f, -1.0f, 0.0f };
    const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, 50.0f);

    const InstanceData identityInstance{};
    auto instanceSSBO = StorageBuffer::Create(sizeof(InstanceData), ShaderBindingLayout::SSBO_INSTANCE_DATA);
    instanceSSBO->SetData(&identityInstance, sizeof(identityInstance));
    instanceSSBO->Bind();

    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    const auto uploadFace = [&](bool captureSeam)
    {
        const glm::mat4 view = glm::lookAt(probe, probe + faceTarget, faceUp);
        const glm::mat4 vp = projection * view;
        ShaderBindingLayout::CameraUBO cameraData{};
        cameraData.ViewProjection = captureSeam ? RHI::AdjustCaptureProjectionForBackend(vp)
                                                : RHI::AdjustProjectionForBackend(vp);
        cameraData.View = view;
        cameraData.Projection = captureSeam ? RHI::AdjustCaptureProjectionForBackend(projection)
                                            : RHI::AdjustProjectionForBackend(projection);
        cameraData.PrevViewProjection = cameraData.ViewProjection;
        cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
    };

    // A caster in front of the probe (+x), displaced toward world +y — i.e.
    // along -faceUp. Asymmetric in BOTH axes so a row mirror is unmistakable.
    // MakeV1Quad authors the xy plane, so build the +x-facing quad directly.
    struct EngineVertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec2 TexCoord;
    };
    const std::array<EngineVertex, 4> casterVertices{
        EngineVertex{ { 2.0f, 0.4f, -0.9f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
        EngineVertex{ { 2.0f, 0.4f, -0.2f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
        EngineVertex{ { 2.0f, 1.2f, -0.2f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
        EngineVertex{ { 2.0f, 1.2f, -0.9f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
    };
    u32 casterIndices[] = { 0u, 1u, 2u, 2u, 3u, 0u };
    auto caster = VertexArray::Create();
    auto casterVb = VertexBuffer::Create(const_cast<f32*>(reinterpret_cast<const f32*>(casterVertices.data())),
                                         static_cast<u32>(sizeof(casterVertices)));
    casterVb->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                          { ShaderDataType::Float3, "a_Normal" },
                          { ShaderDataType::Float2, "a_TexCoord" } });
    caster->AddVertexBuffer(casterVb);
    caster->SetIndexBuffer(IndexBuffer::Create(casterIndices, 6));

    constexpr u32 kCaptureSeam = 0; // production Vulkan capture path
    constexpr u32 kScreenSeam = 1;  // what the screen seam would store

    Texture2DArraySpecification arraySpec;
    arraySpec.Width = kSize;
    arraySpec.Height = kSize;
    arraySpec.Layers = 2;
    arraySpec.Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
    auto faces = Texture2DArray::Create(arraySpec);
    ASSERT_TRUE(faces);

    FramebufferSpecification depthSpec;
    depthSpec.Width = kSize;
    depthSpec.Height = kSize;
    depthSpec.Attachments = { FramebufferTextureFormat::ShadowDepth };
    Ref<Framebuffer> faceFramebuffer = Framebuffer::Create(depthSpec);
    ASSERT_TRUE(faceFramebuffer);

    SubmitFrame(
        [&]()
        {
            faceFramebuffer->Bind();
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetBlendState(false);
            // The capture must SEE backfaces (in-wall probe classification), so
            // the production loop disables culling — copied here so the face
            // basis, not the winding, decides the result.
            RenderCommand::DisableCulling();
            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
            RenderCommand::SetDepthMask(true);
            depthShader->Bind();

            const auto renderFace = [&](u32 layer, bool captureSeam)
            {
                uploadFace(captureSeam);
                faceFramebuffer->AttachDepthTextureArrayLayer(faces->GetRHIHandle(), layer);
                RenderCommand::ClearDepthOnly();
                caster->Bind();
                RenderCommand::DrawIndexed(caster, 6);
            };

            renderFace(kCaptureSeam, true);
            renderFace(kScreenSeam, false);

            std::array<RHI::Barrier, 2> toSampled{};
            for (u32 layer = 0; layer < 2u; ++layer)
            {
                toSampled[layer].Resource = faces->GetRHIHandle();
                toSampled[layer].Range.BaseMip = 0u;
                toSampled[layer].Range.MipCount = 1u;
                toSampled[layer].Range.BaseLayer = layer;
                toSampled[layer].Range.LayerCount = 1u;
                toSampled[layer].Before = RHI::Access::DepthStencilAttachmentWrite;
                toSampled[layer].After = RHI::Access::ShaderSampleRead;
            }
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ toSampled });
        });

    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 2u);
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);

    std::array<std::vector<f32>, 2> stored;
    for (u32 layer = 0; layer < 2u; ++layer)
    {
        ASSERT_TRUE(ReadDepthArrayLayer(faces, layer, stored[layer])) << "face layer " << layer << " readback failed";
        ASSERT_EQ(stored[layer].size(), static_cast<sizet>(kSize) * kSize);
    }
    const auto covered = [&](u32 layer, u32 x, u32 y)
    { return stored[layer][static_cast<sizet>(y) * kSize + x] < 0.999f; };

    u32 compensatedPixels = 0;
    u32 minRow = kSize;
    u32 maxRow = 0;
    u32 mirrorMismatches = 0;
    for (u32 y = 0; y < kSize; ++y)
    {
        for (u32 x = 0; x < kSize; ++x)
        {
            if (covered(kCaptureSeam, x, y))
            {
                ++compensatedPixels;
                minRow = std::min(minRow, y);
                maxRow = std::max(maxRow, y);
            }
            mirrorMismatches += (covered(kCaptureSeam, x, y) != covered(kScreenSeam, x, kSize - 1u - y)) ? 1u : 0u;
        }
    }

    EXPECT_GT(compensatedPixels, 50u) << "the capture face rasterized nothing";
    EXPECT_EQ(mirrorMismatches, 0u)
        << "the capture-seam and screen-seam faces must be exact vertical mirrors — that difference IS the "
           "row-mirroring the screen seam would otherwise bake into every direction-addressed capture";

    // Memory row 0 is clip y = -w on BOTH backends, so a capture whose
    // projection carries no y flip stores exactly what GL stores. Face 0's
    // basis is up = (0,-1,0) — lookAt's camera-up IS that vector — so the
    // caster at world +y sits at NEGATIVE view y, hence ndc_y < 0, hence the
    // FIRST half of memory, the same half GL's bake writes.
    EXPECT_LT(maxRow, kSize / 2u)
        << "the caster must land in the first half of the stored face (rows " << minRow << ".." << maxRow
        << ") — the second half means the screen seam's clip-y negation reached the capture projection";
}

// =============================================================================
// Wave C item 14 — WATER: the tessellated pipeline shape (A10)
// =============================================================================
//
// Water.glsl is the engine's only four-stage program (vertex -> TCS -> TES ->
// fragment), so it is the first shader to need a pipeline shape the backend did
// not have: VkPipelineTessellationStateCreateInfo, PATCH_LIST topology, and the
// patch size as a PSO axis (patchControlPoints is dynamic only under
// extendedDynamicState2PatchControlPoints, which is NOT on the ADR 0010 floor,
// so SetPatchVertexCount joins VulkanPipelineBuilder::Key). This tenant is the
// floor the survey named: the tessellated pipeline BUILDS and a patch draw
// RECORDS AND RASTERIZES. A full water-surface contract (Gerstner displacement,
// refraction, foam, SSR) is disproportionate here — the water fragment stage
// samples 11 textures and reads the scene's own colour/depth, and its physical
// contract is already pinned on GL by WaterRenderingTest /
// WaterVisualEvidenceTest.
//
// The draw goes through RenderCommand::DrawIndexedPatches — the facade entry
// DrawTerrainPatch/DrawWater decode to — so the recorded patch size, the
// PATCH_LIST topology and the TCS/TES stages all have to line up or the draw
// drops (counted) instead of silently rendering as triangles.
TEST_F(VulkanPassSuite, WaterTessellatedPipelineBuildsAndRasterizesAPatchDraw)
{
    constexpr u32 kSize = 64;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto waterShader = Shader::Create("assets/shaders/Water.glsl");
    ASSERT_TRUE(waterShader);
    ASSERT_EQ(waterShader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
        << "Water.glsl must compile all FOUR stages through shaderc (V1 pull branch in the vertex stage; "
           "the tessellation stages consume varyings and need none)";

    // --- every binding the four stages declare that decides geometry ---------
    // Identity camera + identity model: the patch is authored in clip space, so
    // "did it rasterize" is a plain coverage question. The samplers the
    // fragment stage declares (normal maps, scene depth/colour, foam, planar
    // reflection, environment) are deliberately left to the typed NULL
    // descriptors — they tint the surface, they do not decide whether it
    // rasterizes.
    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    cameraData.PrevViewProjection = glm::mat4(1.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    const InstanceData identityInstance{};
    auto instanceSSBO = StorageBuffer::Create(sizeof(InstanceData), ShaderBindingLayout::SSBO_INSTANCE_DATA);
    instanceSSBO->SetData(&identityInstance, sizeof(identityInstance));
    instanceSSBO->Bind();

    ShaderBindingLayout::WaterUBO water{};
    water.WaveParams = { 0.0f, 1.0f, 0.0f, 1.0f }; // time 0, no amplitude — a FLAT patch
    water.WaveDir0 = { 1.0f, 0.0f, 0.0f, 10.0f };
    water.WaveDir1 = { 0.0f, 1.0f, 0.0f, 10.0f };
    water.WaterColor = { 0.0f, 0.4f, 0.8f, 1.0f };     // opaque so the coverage test is unambiguous
    water.WaterDeepColor = { 0.0f, 0.1f, 0.3f, 0.0f }; // reflectivity 0 — no env/planar contribution
    water.VisualParams = { 5.0f, 0.0f, 1.0f, 0.0f };
    water.NormalMapScroll = { 0.0f, 0.0f, 0.0f, 0.0f };
    water.NormalMapSpeed = { 0.0f, 0.0f, 0.0f, 0.0f };
    water.LightDirection = { 0.0f, -1.0f, 0.0f, 0.0f };
    water.ScreenParams = { static_cast<f32>(kSize), static_cast<f32>(kSize), 1.0f / kSize, 1.0f / kSize };
    water.DepthRefractionParams = { 1.0f, 0.0f, 0.0f, 0.0f };
    water.RefractionColor = { 1.0f, 1.0f, 1.0f, 0.0f };
    water.FoamParams = { 10.0f, 1.0f, 1.0f, 0.0f };
    water.FoamParams2 = { 1.0f, 1.0f, 0.0f, 0.0f };
    water.SSSColor = { 0.0f, 0.0f, 0.0f, 0.0f };
    water.SSRParams = { 0.0f, 0.0f, 0.0f, 0.0f }; // SSR off
    // THE knob under test: tessellation ACTIVE, frustum cull off (the patch is
    // already in clip space, so the TCS's distance-based cull has no camera to
    // reason about).
    water.TessParams = { 4.0f, 1.0f, 100.0f, 0.0f };
    water.FFTParams = { 0.0f, 1.0f, 1.0f, 1.0f }; // Gerstner, not FFT
    auto waterUbo = UniformBuffer::Create(ShaderBindingLayout::WaterUBO::GetSize(), ShaderBindingLayout::UBO_WATER);
    waterUbo->SetData(&water, ShaderBindingLayout::WaterUBO::GetSize());

    // PlanarReflectionParams (43): disabled. Left unbound this would read the
    // null address — deterministic zeros, which happens to mean "disabled" too,
    // but the fixture contract is to bind every declared block.
    struct PlanarReflectionParamsBlock
    {
        glm::mat4 ViewProjection{ 1.0f };
        glm::vec4 Params{ 0.0f };
    } planarParams;
    auto planarUbo = UniformBuffer::Create(static_cast<u32>(sizeof(planarParams)), 43u);
    planarUbo->SetData(&planarParams, static_cast<u32>(sizeof(planarParams)));

    // The water fragment stage writes o_Color@0, o_EntityID@1, o_ViewNormal@2,
    // o_Velocity@3 — the scene framebuffer's shape. An attachment short of that
    // is a validation warning about an output with no attachment.
    FramebufferSpecification sceneSpec;
    sceneSpec.Width = kSize;
    sceneSpec.Height = kSize;
    sceneSpec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::RED_INTEGER,
                              FramebufferTextureFormat::RG16F, FramebufferTextureFormat::RG16F,
                              FramebufferTextureFormat::Depth };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
    ASSERT_TRUE(sceneFramebuffer);

    // Coverage is read off DEPTH, not colour. The water fragment stage's colour
    // is a function of eleven samplers this tenant deliberately leaves at the
    // typed NULL descriptors (scene depth/colour, normal maps, foam, planar
    // reflection, environment) plus a full lighting/refraction/foam evaluation
    // — it can legitimately resolve to black, and "the pipeline built and the
    // patch rasterized" must not hinge on that arithmetic. Depth is written by
    // the fixed-function pipeline for every fragment the patch produces (the
    // stage's only `discard` is the render-from-below waterline branch, which
    // u_NormalMapSpeed.w = 0 keeps inert), so it is the honest witness. It
    // rides the item-11 layered path, whose readback helper this reuses.
    Texture2DArraySpecification depthArraySpec;
    depthArraySpec.Width = kSize;
    depthArraySpec.Height = kSize;
    depthArraySpec.Layers = 1;
    depthArraySpec.Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
    auto patchDepth = Texture2DArray::Create(depthArraySpec);
    ASSERT_TRUE(patchDepth);

    // One triangular PATCH covering the lower-left half of the target. The
    // engine `Vertex` layout (V1) is what the water VAO carries in production.
    struct EngineVertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec2 TexCoord;
    };
    const std::array<EngineVertex, 3> patchVertices{
        EngineVertex{ { -0.9f, -0.9f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
        EngineVertex{ { 0.9f, -0.9f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
        EngineVertex{ { -0.9f, 0.9f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
    };
    u32 patchIndices[] = { 0u, 1u, 2u };
    auto patchVao = VertexArray::Create();
    auto patchVb = VertexBuffer::Create(const_cast<f32*>(reinterpret_cast<const f32*>(patchVertices.data())),
                                        static_cast<u32>(sizeof(patchVertices)));
    patchVb->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                         { ShaderDataType::Float3, "a_Normal" },
                         { ShaderDataType::Float2, "a_TexCoord" } });
    patchVao->AddVertexBuffer(patchVb);
    patchVao->SetIndexBuffer(IndexBuffer::Create(patchIndices, 3));

    SubmitFrame(
        [&]()
        {
            sceneFramebuffer->Bind();
            sceneFramebuffer->AttachDepthTextureArrayLayer(patchDepth->GetRHIHandle(), 0u);
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();
            RenderCommand::SetBlendState(false);
            // A tessellated patch's winding after subdivision is the TES's
            // business (`layout(..., ccw)` there); culling off keeps this
            // tenant about the pipeline shape, not about winding.
            RenderCommand::DisableCulling();
            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
            RenderCommand::SetDepthMask(true);

            waterShader->Bind();
            patchVao->Bind();
            // 3 control points per patch — WaterRenderPass's own value, and the
            // TCS's `layout(vertices = 3) out`.
            RenderCommand::DrawIndexedPatches(patchVao, 3u, 3u);
            sceneFramebuffer->Unbind();

            RHI::Barrier toSampled{};
            toSampled.Resource = patchDepth->GetRHIHandle();
            toSampled.Range.BaseMip = 0u;
            toSampled.Range.MipCount = 1u;
            toSampled.Range.BaseLayer = 0u;
            toSampled.Range.LayerCount = 1u;
            toSampled.Before = RHI::Access::DepthStencilAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 1u)
        << "the patch draw must reach the recorder — a tessellation pipeline that failed to build drops it";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);
    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "DrawIndexedPatches must no longer be a Phase 6 stub";

    std::vector<f32> depth;
    ASSERT_TRUE(ReadDepthArrayLayer(patchDepth, 0u, depth)) << "patch depth readback failed";
    ASSERT_EQ(depth.size(), static_cast<sizet>(kSize) * kSize);
    const auto covered = [&](u32 x, u32 y)
    { return depth[static_cast<sizet>(y) * kSize + x] < 0.999f; };

    // The patch is the lower-left half of clip space (hypotenuse x + y = 0) and
    // the camera matrices are raw identity here, so ndc == the authored
    // position and buffer row r samples ndc_y = 2r/kSize - 1:
    //   (8, 40)  -> ndc (-0.75,  0.25): inside
    //   (56, 56) -> ndc ( 0.75,  0.75): outside
    EXPECT_TRUE(covered(8u, 40u))
        << "the tessellated patch rasterized NOTHING — a PATCH_LIST draw against a pipeline carrying no "
           "tessellation state produces no primitives at all";
    EXPECT_FALSE(covered(56u, 56u))
        << "outside the patch must stay at the clear depth — a patch that rasterized as a full-screen "
           "primitive would fail here";

    u32 coveredTexels = 0;
    for (u32 y = 0; y < kSize; ++y)
        for (u32 x = 0; x < kSize; ++x)
            coveredTexels += covered(x, y) ? 1u : 0u;
    // Half of a 64x64 target is 2048; the authored triangle spans 0.9 of each
    // axis, so ~1650. Bounded loosely on both sides: the point is that the
    // subdivided patch covers its triangle and not the whole target.
    EXPECT_GT(coveredTexels, 1000u) << "the patch covered " << coveredTexels << " texels";
    EXPECT_LT(coveredTexels, 2600u) << "the patch covered " << coveredTexels << " texels";

    // --- Phase 2: the SAME patch with back-face culling ON (#691 Phase 8, the
    // water-murk regression). The TES declares `ccw` against GL's LOWER_LEFT
    // tessellation domain; Vulkan's default UPPER_LEFT origin mirrors the
    // generated v coordinate, which flips every emitted triangle's winding —
    // this exact draw back-culled to nothing while the editor's sea shaded its
    // BACK face into a grey murk (normal down, fresnel collapsed, deep-colour
    // blend pinned) with every binding, UBO and mirror input verified correct.
    // Pinned by VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT on every patch
    // pipeline: culled coverage here means the domain origin regressed.
    SubmitFrame(
        [&]()
        {
            sceneFramebuffer->Bind();
            sceneFramebuffer->AttachDepthTextureArrayLayer(patchDepth->GetRHIHandle(), 0u);
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();
            RenderCommand::SetBlendState(false);
            RenderCommand::BackCull();
            RenderCommand::SetFrontFace(RHI::FrontFace::CounterClockwise);
            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
            RenderCommand::SetDepthMask(true);

            waterShader->Bind();
            patchVao->Bind();
            RenderCommand::DrawIndexedPatches(patchVao, 3u, 3u);
            RenderCommand::DisableCulling();
            sceneFramebuffer->Unbind();

            RHI::Barrier toSampled{};
            toSampled.Resource = patchDepth->GetRHIHandle();
            toSampled.Range.BaseMip = 0u;
            toSampled.Range.MipCount = 1u;
            toSampled.Range.BaseLayer = 0u;
            toSampled.Range.LayerCount = 1u;
            toSampled.Before = RHI::Access::DepthStencilAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    std::vector<f32> culledDepth;
    ASSERT_TRUE(ReadDepthArrayLayer(patchDepth, 0u, culledDepth)) << "phase-2 depth readback failed";
    ASSERT_EQ(culledDepth.size(), static_cast<sizet>(kSize) * kSize);
    EXPECT_LT(culledDepth[static_cast<sizet>(40) * kSize + 8], 0.999f)
        << "the ccw tessellated patch was BACK-CULLED — the pipeline's tessellation domain origin is not "
           "GL's LOWER_LEFT, so the tessellator mirrored the generated winding";
}

// =============================================================================
// FinalRenderPass — the swapchain import (#691 Phase 7, the LAST pass).
//
// Every other pass in the suite renders into a VulkanFramebuffer the graph
// owns. This one targets "the default framebuffer", which on GL is a fixed
// object (name 0) and on Vulkan is a DIFFERENT image every frame — whichever
// vkAcquireNextImageKHR just handed the swap loop. VulkanContext publishes
// that image for the duration of one recording (SetFrameBackbuffer) and
// BindDefaultFramebuffer resolves to it.
//
// WINDOWLESS PROOF: the publication needs exactly three things from its image
// — an RHI identity, a VulkanImageInfoRegistry entry (format/aspect) and an
// attachment view — and a plain colour attachment carries all three, created
// the same way the swapchain images are registered. So a framebuffer's own
// attachment stands in for the acquired image and the seam is provable with no
// surface, no swapchain and no window. What this canNOT prove is the surface
// half (acquire/present semaphores, the PRESENT_SRC layout actually being
// accepted by the presentation engine) — that is what the live run is for.
//
// Two arms, because the pass has two exits:
//  1. with a resolvable input: clear + fullscreen blit reach the backbuffer;
//  2. with NO input: the pass clears and RETURNS. GL clears eagerly, this
//     backend folds a clear into the next draw's loadOp — so a clear with no
//     draw behind it used to leave the presented image undefined. Arm 2 pins
//     FinalizeBackbufferForPresent materialising it.
// =============================================================================
TEST_F(VulkanPassSuite, FinalPassBlitsThroughTheDefaultFramebufferIntoThePublishedBackbuffer)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    // The stand-in backbuffer (see the header comment).
    FramebufferSpecification backbufferSpec;
    backbufferSpec.Width = kSize;
    backbufferSpec.Height = kSize;
    backbufferSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    Ref<Framebuffer> backbufferOwner = Framebuffer::Create(backbufferSpec);
    ASSERT_TRUE(backbufferOwner) << "stand-in backbuffer creation failed";
    auto* vkBackbufferOwner = static_cast<VulkanFramebuffer*>(backbufferOwner.Raw());
    Ref<VulkanTexture2D> backbufferImage = vkBackbufferOwner->GetColorAttachmentImage(0);
    ASSERT_TRUE(backbufferImage);
    const VkImageView backbufferView = backbufferImage->GetOrCreateAttachmentView();
    ASSERT_NE(backbufferView, VK_NULL_HANDLE);
    const RHI::ResourceHandle backbufferHandle = backbufferImage->GetRHIHandle();
    ASSERT_TRUE(backbufferHandle.IsValid());

    // A VERTICALLY ASYMMETRIC pattern, because the thing most likely to be
    // wrong about "the frame reached the screen" is which way up it is: the
    // A8 projection seam leaves every graph-owned image in GL's row order,
    // and the swapchain is the one target that displays row 0 at the TOP.
    // A solid colour cannot see that; two stacked bands can.
    // Neither band is a colour any clear path here produces, so "the blit
    // landed" and "something cleared" can never be confused either.
    constexpr u8 kTopR = 40;
    constexpr u8 kTopG = 180;
    constexpr u8 kTopB = 90;
    constexpr u8 kBottomR = 200;
    constexpr u8 kBottomG = 60;
    constexpr u8 kBottomB = 30;
    std::vector<u8> bandRgba(static_cast<sizet>(kSize) * kSize * 4);
    for (u32 y = 0; y < kSize; ++y)
    {
        const bool firstHalf = y < kSize / 2u;
        for (u32 x = 0; x < kSize; ++x)
        {
            const sizet i = (static_cast<sizet>(y) * kSize + x) * 4;
            bandRgba[i + 0] = firstHalf ? kTopR : kBottomR;
            bandRgba[i + 1] = firstHalf ? kTopG : kBottomG;
            bandRgba[i + 2] = firstHalf ? kTopB : kBottomB;
            bandRgba[i + 3] = 255;
        }
    }
    TextureSpecification bandSpec;
    bandSpec.Width = kSize;
    bandSpec.Height = kSize;
    bandSpec.Format = ImageFormat::RGBA8;
    auto patternInput = Texture2D::Create(bandSpec);
    ASSERT_NE(patternInput, nullptr);
    patternInput->SetData(bandRgba.data(), static_cast<u32>(bandRgba.size()));
    auto blitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
    ASSERT_TRUE(blitShader);
    ASSERT_EQ(blitShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    // FullscreenBlit's fragment reads DRSParams@33 — both the producer's copy
    // and FinalRenderPass's own. Defaults are the DRS-inactive (1,1).
    DRSUBOData drsData{};
    auto drsUbo = UniformBuffer::Create(sizeof(DRSUBOData), 33);
    drsUbo->SetData(&drsData, sizeof(drsData));

    auto finalPass = Ref<FinalRenderPass>::Create();
    FramebufferSpecification initSpec;
    initSpec.Width = kSize;
    initSpec.Height = kSize;
    initSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    finalPass->Init(initSpec);
    finalPass->SetupFramebuffer(kSize, kSize);

    auto producer = Ref<PatternProducerPass>::Create(patternInput, blitShader);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());

    // --- Arm 1: producer -> FinalRenderPass -> the published backbuffer -----
    RenderGraph graph;
    graph.SetTransientMaterializationEnabled(true);

    RGResourceDesc fbDesc;
    fbDesc.Kind = RGResourceHandle::Kind::Framebuffer;
    fbDesc.Format = RGResourceFormat::RGBA8UNorm;
    fbDesc.Width = kSize;
    fbDesc.Height = kSize;

    // The producer's target is CALLER-BACKED so the chain's own row order can
    // be read back and compared against the presented image — the orientation
    // contract below is stated as a relation between the two, which is
    // convention-free (it holds whatever GL's texture-row convention is).
    FramebufferSpecification chainSpec;
    chainSpec.Width = kSize;
    chainSpec.Height = kSize;
    chainSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    Ref<Framebuffer> chainFramebuffer = Framebuffer::Create(chainSpec);
    ASSERT_TRUE(chainFramebuffer);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.PostProcessColor =
        graph.DeclareTransientFramebuffer(ResourceNames::PostProcessColor, fbDesc, chainFramebuffer);

    graph.AddNode(producer);
    graph.AddNode(finalPass);
    graph.SetFinalPass("FinalRenderPass");
    graph.BuildFrameGraph();

    bool armOneFinalized = false;
    SubmitFrame(
        [&]()
        {
            // The publication is recording-scoped: VulkanContext does exactly
            // this between BeginRecording and the callback.
            api.SetFrameBackbuffer(backbufferHandle, backbufferView, kSize, kSize);
            graph.Execute();
            armOneFinalized = api.FinalizeBackbufferForPresent(true);

            // The present layout is where the real frame ends; the readback
            // path wants the steady-state sampled layout, so walk both images
            // on.
            std::array<RHI::Barrier, 2> toSampled{};
            toSampled[0].Resource = backbufferHandle;
            toSampled[0].Range.MipCount = 1u;
            toSampled[0].Range.LayerCount = 1u;
            toSampled[0].Before = RHI::Access::Present;
            toSampled[0].After = RHI::Access::ShaderSampleRead;
            toSampled[1].Resource = chainFramebuffer->GetColorAttachmentHandle(0);
            toSampled[1].Range.MipCount = 1u;
            toSampled[1].Range.LayerCount = 1u;
            toSampled[1].Before = RHI::Access::ColorAttachmentWrite;
            toSampled[1].After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>{ toSampled });
        });

    EXPECT_TRUE(producer->DidDraw) << "the producer pass early-returned";
    for (const auto& failure : graph.GetResolveFailures())
    {
        ADD_FAILURE() << "FinalRenderPass: resolve failure pass='" << failure.PassName << "' reason='"
                      << failure.Reason << "' x" << failure.Count;
    }
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 2u)
        << "the producer's blit and the final blit must BOTH record — a final blit dropped for want of a "
           "target is exactly the bug the swapchain import fixes";
    EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u) << "a draw dropped silently";
    EXPECT_TRUE(armOneFinalized) << "the frame never touched the backbuffer";

    std::vector<u8> presented;
    ASSERT_TRUE(backbufferImage->GetData(presented, 0)) << "backbuffer readback failed";
    ASSERT_EQ(presented.size(), static_cast<sizet>(kSize) * kSize * 4);
    std::vector<u8> chain;
    auto* vkChain = static_cast<VulkanFramebuffer*>(chainFramebuffer.Raw());
    ASSERT_TRUE(vkChain->GetColorAttachmentImage(0) != nullptr &&
                vkChain->GetColorAttachmentImage(0)->GetData(chain, 0))
        << "chain framebuffer readback failed";
    ASSERT_EQ(chain.size(), presented.size());

    const auto texel = [&](const std::vector<u8>& image, u32 x, u32 y, u32 channel)
    { return static_cast<int>(image[(static_cast<sizet>(y) * kSize + x) * 4 + channel]); };

    // Content: both bands survived the two blits (no clear, no black frame).
    {
        const int topBandRed = texel(presented, 64, 8, 0);
        const int bottomBandRed = texel(presented, 64, kSize - 9, 0);
        EXPECT_NE(topBandRed, bottomBandRed)
            << "the presented image is a flat colour — the two-band pattern did not survive the chain";
        const bool bandsPresent =
            (std::abs(topBandRed - kTopR) <= 2 && std::abs(bottomBandRed - kBottomR) <= 2) ||
            (std::abs(topBandRed - kBottomR) <= 2 && std::abs(bottomBandRed - kTopR) <= 2);
        EXPECT_TRUE(bandsPresent) << "presented bands are " << topBandRed << " / " << bottomBandRed
                                  << ", neither authored colour";
    }

    // ORIENTATION: the presented image is the vertical MIRROR of the chain's
    // own row order. That is not a quirk to be tolerated — it is the contract.
    // The A8 seam authors every graph image in GL's row order (row 0 = bottom
    // of the picture) so that screen-space shaders stay source-identical
    // across backends; the swapchain displays row 0 at the TOP, so the final
    // blit MUST flip. Without the flip the whole live frame is upside down on
    // screen while every in-chain readback still looks perfect — which is
    // exactly how it shipped past 45 green tenants until someone looked at
    // the window.
    for (const u32 row : { 2u, 30u, 64u, 100u, kSize - 3u })
    {
        const u32 mirrored = kSize - 1u - row;
        for (const u32 channel : { 0u, 1u, 2u })
        {
            EXPECT_NEAR(texel(presented, 64, row, channel), texel(chain, 64, mirrored, channel), 2)
                << "presented row " << row << " must mirror chain row " << mirrored << " (channel " << channel << ")";
        }
    }

    // --- Arm 2: the clear-only exit ----------------------------------------
    // FinalRenderPass with no resolvable input clears and returns. Recorded
    // by hand rather than through a second graph so the assertion is about
    // the BACKEND contract (a pending clear with no draw behind it) and not
    // about how a graph with no producer culls.
    bool armTwoFinalized = false;
    SubmitFrame(
        [&]()
        {
            api.SetFrameBackbuffer(backbufferHandle, backbufferView, kSize, kSize);
            RenderCommand::BindDefaultFramebuffer();
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();
            // No draw — the FinalRenderPass "missing input" exit.
            armTwoFinalized = api.FinalizeBackbufferForPresent(true);

            RHI::Barrier toSampled{};
            toSampled.Resource = backbufferHandle;
            toSampled.Range.BaseMip = 0u;
            toSampled.Range.MipCount = 1u;
            toSampled.Range.BaseLayer = 0u;
            toSampled.Range.LayerCount = 1u;
            toSampled.Before = RHI::Access::Present;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    EXPECT_TRUE(armTwoFinalized)
        << "a clear-only frame must still count as rendered — otherwise the backend's clear fallback "
           "silently replaces the pass's own clear colour";
    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 0u) << "arm 2 records no draw by construction";

    std::vector<u8> cleared;
    ASSERT_TRUE(backbufferImage->GetData(cleared, 0)) << "backbuffer readback failed";
    ASSERT_EQ(cleared.size(), presented.size());
    const auto clearedTexel = [&](u32 x, u32 y, u32 channel)
    { return static_cast<int>(cleared[(static_cast<sizet>(y) * kSize + x) * 4 + channel]); };
    for (const auto& [x, y] : { std::pair<u32, u32>{ 4, 4 }, { 64, 64 }, { 123, 123 } })
    {
        EXPECT_GE(clearedTexel(x, y, 0), 250) << "clear-only frame (" << x << "," << y << ") must be red";
        EXPECT_LE(clearedTexel(x, y, 1), 5) << "clear-only frame (" << x << "," << y << ") must be red";
        EXPECT_LE(clearedTexel(x, y, 2), 5) << "clear-only frame (" << x << "," << y << ") must be red";
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u);
}

// =============================================================================
// Issue #691 Phase 7 — the compute bare-uniform sweep.
//
// GLSL-for-Vulkan forbids a non-opaque uniform outside a block, so every
// `uniform float u_Foo;` in a .comp was a HARD SPIR-V compile error on the
// Vulkan route ("'non-opaque uniforms outside a block' : not allowed"), and
// VulkanComputeShader::Set* is a deliberate no-op — so a shader that somehow
// did compile would still read zeros. A live --rhi=vulkan editor run showed 13
// compute programs failing outright, taking GPU particles, virtual geometry,
// Forward+ light culling, snow, wind and terrain erosion with them.
//
// The two tests below are the sweep's contract, and they are deliberately of
// two different kinds:
//
//  * the device-gated one PROVES the fix — every migrated shader really goes
//    through shaderc(vulkan_1_4) and produces a valid program;
//  * the headless one STOPS THE DEBT RETURNING — it is a pure text scan, so it
//    runs in CI with no GPU at all and fails the moment someone reintroduces a
//    default-block uniform into one of these files.
//
// Neither one is redundant: a text scan cannot prove the SPIR-V compiles, and a
// device-gated test SKIPs on every machine CI actually runs on.
// =============================================================================

namespace
{
    // The files this sweep migrated. Each declares exactly one pass-owned
    // std140 block (some SHARE one block across the sibling shaders of a
    // system — see UBOStructures in ShaderBindingLayout.h for which). The
    // Phase 8 tail (CloudNoise/CloudShadow, the three Ocean FFT passes,
    // Precipitation_Feed) carried the identical debt but never appeared in the
    // Phase 7 live log — their passes never ran in that session.
    constexpr const char* kPhase7MigratedComputeShaders[] = {
        "assets/shaders/compute/Particle_Simulate.comp",
        "assets/shaders/compute/Particle_Emit.comp",
        "assets/shaders/compute/Particle_Compact.comp",
        "assets/shaders/compute/Wind_Generate.comp",
        "assets/shaders/compute/Snow_Accumulate.comp",
        "assets/shaders/compute/Snow_Deform.comp",
        "assets/shaders/compute/Terrain_Erosion.comp",
        "assets/shaders/compute/LightCulling.comp",
        "assets/shaders/compute/VirtualClusterCull.comp",
        "assets/shaders/compute/VirtualClusterRaster.comp",
        "assets/shaders/compute/VirtualDebugColorize.comp",
        "assets/shaders/compute/InstanceOcclusionCull.comp",
        "assets/shaders/compute/InstanceFrustumCull.comp",
        // Issue #691 Phase 8 — the sweep's completion.
        "assets/shaders/compute/CloudNoise_Generate.comp",
        "assets/shaders/compute/CloudShadow_Generate.comp",
        "assets/shaders/compute/Ocean_SpectrumEvolve.comp",
        "assets/shaders/compute/Ocean_FFTButterfly.comp",
        "assets/shaders/compute/Ocean_Assemble.comp",
        "assets/shaders/compute/Precipitation_Feed.comp",
        "assets/shaders/compute/ReflectionProbeCull.comp",
    };
} // namespace

// The decisive check: each migrated .comp compiles through the REAL Vulkan
// compute path (shaderc -> SPIR-V -> VkShaderModule + compute pipeline). Before
// the sweep every one of these returned an invalid program.
TEST_F(VulkanPassSuite, Phase7MigratedComputeShadersCompileOnVulkan)
{
    ASSERT_EQ(RendererAPI::GetAPI(), RendererAPI::API::Vulkan)
        << "the fixture must have switched the process-global backend to Vulkan";

    std::vector<std::string> failures;
    for (const char* path : kPhase7MigratedComputeShaders)
    {
        Ref<ComputeShader> shader = ComputeShader::Create(path);
        if (!shader || !shader->IsValid())
            failures.emplace_back(path);
    }

    // VirtualClusterRaster_Int64 — the define-injected variant of
    // VirtualClusterRaster.comp — is DELIBERATELY NOT BUILT HERE, and the reason
    // is worth writing down because it looks like a gap.
    //
    // It appeared in the live log under its own name, for the same bare-uniform
    // reason as the rest; that half is fixed and is covered twice over — the
    // block lives in the shared source, so the base variant compiling above
    // proves it parses, and the ratchet below proves the file declares no bare
    // uniform in either variant.
    //
    // What building it here would additionally exercise is a DIFFERENT contract
    // that this sweep does not own and currently does not hold: the module
    // declares the SPIR-V `Int64` capability (it uses uint64_t), which requires
    // VkPhysicalDeviceFeatures::shaderInt64 — a separate feature from the
    // shaderBufferInt64Atomics that VulkanDevice enables and that
    // RenderCommand::SupportsInt64ShaderAtomics() reports. So on this backend
    // the facade answers "yes, use the 64-bit path", vkCreateShaderModule then
    // raises VUID-VkShaderModuleCreateInfo-pCode-08740, and production falls
    // back to the portable two-pass raster with a warning. Attempting it here
    // would fail this test on that unrelated device-feature gap AND trip the
    // fixture's zero-validation-error teardown, turning a bare-uniform gate into
    // a permanent red for something a Platform/Vulkan/ change has to fix.

    std::string joined;
    for (const auto& f : failures)
    {
        joined += "\n  " + f;
    }
    EXPECT_TRUE(failures.empty())
        << failures.size() << " migrated compute shader(s) still fail to compile on Vulkan:" << joined
        << "\nCheck OloEngine.log for the glslang diagnostic. A 'non-opaque uniforms outside a block' "
           "error means a bare uniform came back (issue #691 Phase 7).";
}

// The ratchet. Pure text, no device: fails on any machine the moment a
// default-block uniform reappears in a migrated file. glslang rejects a
// default-block uniform regardless of leading whitespace, so the line is
// TRIMMED before matching — an earlier column-0-only test would have missed an
// indented `    uniform float u_Foo;`. An opaque uniform always carries a
// `layout(binding = N)` prefix, so it never begins the trimmed line.
TEST(VulkanComputeBareUniformSweep, MigratedComputeShadersDeclareNoBareUniforms)
{
    ASSERT_TRUE(ChangeToOloEditorDir()) << "OloEditor/ not found from the test cwd";

    std::vector<std::string> offenders;
    for (const char* path : kPhase7MigratedComputeShaders)
    {
        std::ifstream file{ path };
        ASSERT_TRUE(file.is_open()) << "could not open " << path;
        std::string line;
        u32 lineNumber = 0;
        while (std::getline(file, line))
        {
            ++lineNumber;
            const auto firstNonSpace = line.find_first_not_of(" \t");
            const std::string_view trimmed =
                firstNonSpace == std::string::npos ? std::string_view{} : std::string_view{ line }.substr(firstNonSpace);
            // "uniform" followed by any whitespace — a tab separator counts.
            if (trimmed.starts_with("uniform") && trimmed.size() > 7 &&
                (trimmed[7] == ' ' || trimmed[7] == '\t'))
            {
                offenders.emplace_back(std::string(path) + ":" + std::to_string(lineNumber) + " -> " + line);
            }
        }
    }

    std::string joined;
    for (const auto& o : offenders)
    {
        joined += "\n  " + o;
    }
    EXPECT_TRUE(offenders.empty())
        << "default-block uniform(s) reintroduced into a shader the #691 Phase 7 sweep migrated:" << joined
        << "\nThe Vulkan SPIR-V route rejects these outright and VulkanComputeShader::Set* is a no-op "
           "there — move the value into the shader's pass-owned std140 block instead.";
}

// =============================================================================
// ReadTextureSubImage / ReadTextureImage (#691 Phase 8b): the MCP diagnostics
// readback spine — olo_screenshot / olo_render_capture_target, thumbnail
// capture, the probe bakers and entity picking all lower onto these. Pins
// three contracts: the identity path returns the uploaded bytes verbatim
// (rows unflipped — glGetTextureSubImage semantics), the conversion path
// decodes native texels to the caller's float format, and a sub-region read
// addresses texels in image coordinates.
// =============================================================================
TEST_F(VulkanPassSuite, ReadTextureSubImageReadsBackUploadedTexelsWithConversion)
{
    constexpr u32 kSize = 8;
    VulkanFrameArena::Get().BeginFrame(0);

    TextureSpecification spec;
    spec.Width = kSize;
    spec.Height = kSize;
    spec.Format = ImageFormat::RGBA8;
    spec.GenerateMips = false;
    auto texture = Texture2D::Create(spec);
    ASSERT_NE(texture, nullptr);

    std::vector<u8> texels(static_cast<sizet>(kSize) * kSize * 4u);
    for (u32 y = 0; y < kSize; ++y)
    {
        for (u32 x = 0; x < kSize; ++x)
        {
            u8* t = texels.data() + (static_cast<sizet>(y) * kSize + x) * 4u;
            t[0] = static_cast<u8>(x * 16u);
            t[1] = static_cast<u8>(y * 16u);
            t[2] = 128u;
            t[3] = 255u;
        }
    }
    texture->SetData(texels.data(), static_cast<u32>(texels.size()));

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    // Identity: RGBA8 image read as RGBA8 — byte-exact, no row flip.
    std::vector<u8> identity(texels.size(), 0u);
    ASSERT_TRUE(api.ReadTextureSubImage(texture->GetRHIHandle(), 0, 0, 0, 0, kSize, kSize, 1u,
                                        RHI::Format::RGBA8UNorm, identity.size(), identity.data()));
    EXPECT_EQ(identity, texels) << "identity readback must return the uploaded bytes verbatim";

    // Conversion: the render-graph/MCP capture shape — any attachment read
    // as floats.
    std::vector<f32> floats(static_cast<sizet>(kSize) * kSize * 4u, -1.0f);
    ASSERT_TRUE(api.ReadTextureSubImage(texture->GetRHIHandle(), 0, 0, 0, 0, kSize, kSize, 1u,
                                        RHI::Format::RGBA32Float, floats.size() * sizeof(f32), floats.data()));
    const sizet probe = (static_cast<sizet>(5) * kSize + 3) * 4u; // texel (3, 5)
    EXPECT_NEAR(floats[probe + 0], 48.0f / 255.0f, 1.0e-6f);
    EXPECT_NEAR(floats[probe + 1], 80.0f / 255.0f, 1.0e-6f);
    EXPECT_NEAR(floats[probe + 2], 128.0f / 255.0f, 1.0e-6f);
    EXPECT_NEAR(floats[probe + 3], 1.0f, 1.0e-6f);

    // Sub-region: 2x2 at (4, 2), image coordinates.
    std::vector<u8> region(2u * 2u * 4u, 0u);
    ASSERT_TRUE(api.ReadTextureSubImage(texture->GetRHIHandle(), 0, 4, 2, 0, 2u, 2u, 1u,
                                        RHI::Format::RGBA8UNorm, region.size(), region.data()));
    EXPECT_EQ(region[0], 64u) << "region texel (0,0) must be image texel (4,2).r";
    EXPECT_EQ(region[1], 32u) << "region texel (0,0) must be image texel (4,2).g";
    EXPECT_EQ(region[4], 80u) << "region texel (1,0) must be image texel (5,2).r";

    // Whole-level convenience wrapper sizes the read from the registry extent.
    std::vector<u8> whole(texels.size(), 0u);
    ASSERT_TRUE(api.ReadTextureImage(texture->GetRHIHandle(), 0, RHI::Format::RGBA8UNorm, whole.size(),
                                     whole.data()));
    EXPECT_EQ(whole, texels);

    // Half-float client contract (#691 Phase 8): the GL facade takes f32 PER
    // CHANNEL for the 16F formats and converts driver-side — this backend
    // must convert CPU-side, not size-assert (SlugFontProcessor's RGBA16F
    // curve texture crashed the first Vulkan play of a scene with UI text)
    // and not reinterpret f32 bits as halves. Round-trip through the float
    // readback proves both the accepted size and the converted values.
    TextureSpecification halfSpec;
    halfSpec.Width = 2;
    halfSpec.Height = 2;
    halfSpec.Format = ImageFormat::RGBA16F;
    halfSpec.GenerateMips = false;
    auto halfTexture = Texture2D::Create(halfSpec);
    ASSERT_NE(halfTexture, nullptr);
    const std::array<f32, 16> halfClient{ 0.25f, 0.5f, 0.75f, 1.0f, 2.0f, 4.0f, -1.0f, 0.0f,
                                          8.0f, 16.0f, 0.1f, 1.0f, -0.5f, 0.0f, 1.5f, 1.0f };
    halfTexture->SetData(const_cast<f32*>(halfClient.data()), static_cast<u32>(halfClient.size() * sizeof(f32)));
    std::vector<f32> halfBack(halfClient.size(), -99.0f);
    ASSERT_TRUE(api.ReadTextureSubImage(halfTexture->GetRHIHandle(), 0, 0, 0, 0, 2u, 2u, 1u,
                                        RHI::Format::RGBA32Float, halfBack.size() * sizeof(f32), halfBack.data()));
    for (sizet i = 0; i < halfClient.size(); ++i)
    {
        // Half precision: ~11 bits of mantissa — 1e-2 covers every value above.
        EXPECT_NEAR(halfBack[i], halfClient[i], 1.0e-2f) << "texel float " << i;
    }

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore)
        << "the readback family must not fall through to a stub anymore";
}

// =============================================================================
// Command-ordered SSBO SetData (#691 Phase 8): the batched-instance archetype.
//
// CommandDispatch::DrawMeshInstanced re-uploads the ONE shared
// ModelInstanceBuffer (SSBO 15) before every batch and records the draw
// between the writes. GL executes upload/draw in command order; on Vulkan the
// draws execute at submit, so with a life-stable buffer address every draw in
// the frame read the LAST upload — which emptied every auto-batched instanced
// draw in the sandbox scenes (spheres/cubes vanished while unique-mesh draws
// survived; found by the Phase 8 Step 3 screenshot parity gate, root-caused
// through an all-far-plane SceneDepth capture). VulkanStorageBuffer::SetData
// now snapshots the written range into the frame arena and draws embed the
// snapshot's address (GetRootDataAddress), so each recorded draw keeps the
// bytes that were current when it was recorded.
//
// The tenant is the archetype in miniature: one SSBO at binding 15, upload a
// LEFT transform, draw, upload a RIGHT transform, draw — both quads must land.
// Under last-write-wins ordering the first draw also reads RIGHT and the left
// half stays background.
// =============================================================================
TEST_F(VulkanPassSuite, InterleavedInstanceBufferUploadsKeepCommandOrderAcrossDraws)
{
    constexpr u32 kSize = 128;
    VulkanFrameArena::Get().BeginFrame(0);

    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    const u64 stubsBefore = api.GetPhase6StubHitCount();

    auto lightCubeShader = Shader::Create("assets/shaders/LightCube.glsl");
    ASSERT_TRUE(lightCubeShader);
    ASSERT_EQ(lightCubeShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    // A flat quad in the engine Vertex shape (32 B: pos3 @0, normal3 @12,
    // uv2 @24) — the stride LightCube's pull branch hard-codes (8 floats).
    const f32 quadVertices[] = {
        -0.5f,
        -0.5f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f, // bottom-left
        0.5f,
        -0.5f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f, // bottom-right
        0.5f,
        0.5f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        1.0f, // top-right
        -0.5f,
        0.5f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f, // top-left
    };
    u32 quadIndices[] = { 0u, 1u, 2u, 2u, 3u, 0u };
    auto vao = VertexArray::Create();
    auto quadVB = VertexBuffer::Create(quadVertices, sizeof(quadVertices));
    quadVB->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                        { ShaderDataType::Float3, "a_Normal" },
                        { ShaderDataType::Float2, "a_TexCoord" } });
    vao->AddVertexBuffer(quadVB);
    vao->SetIndexBuffer(IndexBuffer::Create(quadIndices, 6));

    ShaderBindingLayout::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    cameraData.Position = glm::vec3(0.0f);
    cameraData.PrevViewProjection = glm::mat4(1.0f);
    cameraData.RenderOrigin = glm::vec3(0.0f);
    auto cameraUbo = UniformBuffer::Create(ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
    cameraUbo->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());

    // Quad scaled to 0.8 wide and shifted -0.5 / +0.5: LEFT spans x NDC
    // [-0.9, -0.1], RIGHT [0.1, 0.9]; both span y [-0.4, 0.4]. The strip
    // between them stays background under EITHER ordering, so the probe set
    // separates the two behaviours unambiguously.
    const auto makeInstance = [](f32 xShift)
    {
        InstanceData inst{};
        inst.Transform = glm::translate(glm::mat4(1.0f), { xShift, 0.0f, 0.0f }) *
                         glm::scale(glm::mat4(1.0f), { 0.8f, 0.8f, 1.0f });
        inst.Normal = glm::mat4(1.0f);
        inst.PrevTransform = inst.Transform;
        return inst;
    };
    const InstanceData leftInstance = makeInstance(-0.5f);
    const InstanceData rightInstance = makeInstance(0.5f);

    auto instanceSSBO = StorageBuffer::Create(sizeof(InstanceData), ShaderBindingLayout::SSBO_INSTANCE_DATA);

    FramebufferSpecification sceneSpec;
    sceneSpec.Width = kSize;
    sceneSpec.Height = kSize;
    sceneSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
    Ref<Framebuffer> sceneFramebuffer = Framebuffer::Create(sceneSpec);
    ASSERT_TRUE(sceneFramebuffer);

    SubmitFrame(
        [&]()
        {
            sceneFramebuffer->Bind();
            RenderCommand::SetViewport(0, 0, kSize, kSize);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::SetBlendState(false);
            RenderCommand::DisableCulling();

            lightCubeShader->Bind();
            vao->Bind();

            // The archetype: write, draw, write, draw — same buffer object.
            instanceSSBO->SetData(&leftInstance, sizeof(InstanceData));
            instanceSSBO->Bind();
            RenderCommand::DrawIndexed(vao, 6);

            instanceSSBO->SetData(&rightInstance, sizeof(InstanceData));
            instanceSSBO->Bind();
            RenderCommand::DrawIndexed(vao, 6);

            RHI::Barrier toSampled{};
            toSampled.Resource = sceneFramebuffer->GetColorAttachmentHandle(0);
            toSampled.Before = RHI::Access::ColorAttachmentWrite;
            toSampled.After = RHI::Access::ShaderSampleRead;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
        });

    EXPECT_EQ(api.GetPreparedDrawsThisRecording(), 2u);
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

    // LightCube writes solid white. Centres: LEFT x NDC -0.5 -> column 32,
    // RIGHT +0.5 -> column 96, both row 64 (y-symmetric quads, so either row
    // orientation covers them).
    EXPECT_EQ(px(32, 64)[0], 255)
        << "the FIRST draw must render the transform uploaded BEFORE it (command-ordered SetData) — "
           "a black left quad means every draw read the frame's final upload";
    EXPECT_EQ(px(96, 64)[0], 255) << "the second draw must render the transform uploaded before it";
    EXPECT_EQ(px(64, 64)[0], 0) << "the strip between the quads keeps the clear";
    EXPECT_EQ(px(5, 5)[0], 0) << "corner background keeps the clear";

    EXPECT_EQ(api.GetPhase6StubHitCount(), stubsBefore);
}

#endif // OLO_WITH_VULKAN
