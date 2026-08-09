// OLO_TEST_LAYER: plumbing
//
// #691 Phase 6 — the shader path end to end on a live device: VulkanShader
// (direct SPIR-V, vulkan_1_4 tier), VulkanResourceHeap (VK_EXT_descriptor_heap
// descriptors), VulkanPipelineBuilder (thin PSO, dynamic rendering, root-data
// binding mappings), VulkanFrameArena (root structs in mapped BDA memory) and
// vkCmdPushDataEXT — the ADR 0011 §4/§5 shape every Phase 7 pass copies.
//
// The checkpoint test renders the SAME shader the GL golden pins
// (PostProcess_FXAA.glsl) over the SAME procedurally generated hard-edge
// input, reads the result back, and holds it to the golden baseline plus the
// two driver-independent FXAA property invariants from GoldenImageTests
// (#734). Device-gated: SKIPs cleanly without a contract-satisfying device.

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanShaderPipeline, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "OLO_WITH_VULKAN is off — the Vulkan backend is not compiled into this build.";
}

#else

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Shader.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <volk.h>

#include <stb_image/stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace
{
    using namespace OloEngine;

    // Walk up from CWD to the folder containing OloEditor/ and chdir into
    // OloEditor so "assets/shaders/..." (and the shader include resolution)
    // work — the RenderPropertyTest fixture's rule, replicated for this
    // device-gated fixture.
    bool ChangeToOloEditorDir()
    {
        namespace fs = std::filesystem;
        fs::path candidate = fs::current_path();
        for (int i = 0; i < 6; ++i)
        {
            const fs::path editorDir = candidate / "OloEditor";
            if (fs::exists(editorDir / "assets" / "shaders"))
            {
                fs::current_path(editorDir);
                return true;
            }
            if (!candidate.has_parent_path() || candidate.parent_path() == candidate)
            {
                break;
            }
            candidate = candidate.parent_path();
        }
        return fs::exists(fs::path("assets") / "shaders");
    }

    // Restores the process-wide API selection (these tests run inside the GL
    // suite binary).
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

    class VulkanShaderPipeline : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (volkInitialize() != VK_SUCCESS)
            {
                GTEST_SKIP() << "No Vulkan loader on this machine.";
            }

            // Bare probe instance first — constructing VulkanDevice on a
            // driverless machine SEH-faults under ASan (the execution test's
            // ladder, same order).
            VkApplicationInfo appInfo{};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.apiVersion = VulkanCapabilities::kMinApiVersion;
            VkInstanceCreateInfo instanceInfo{};
            instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            instanceInfo.pApplicationInfo = &appInfo;
            VkInstance probe = VK_NULL_HANDLE;
            if (vkCreateInstance(&instanceInfo, nullptr, &probe) != VK_SUCCESS)
            {
                GTEST_SKIP() << "vkCreateInstance failed (no Vulkan 1.4 runtime).";
            }
            volkLoadInstance(probe);

            u32 deviceCount = 0;
            vkEnumeratePhysicalDevices(probe, &deviceCount, nullptr);
            std::vector<VkPhysicalDevice> devices(deviceCount);
            if (deviceCount > 0)
            {
                vkEnumeratePhysicalDevices(probe, &deviceCount, devices.data());
            }
            const bool anySatisfying = std::ranges::any_of(devices, [](VkPhysicalDevice d)
                                                           { return VulkanCapabilities::Evaluate(d).Satisfied; });
            vkDestroyInstance(probe, nullptr);
            if (!anySatisfying)
            {
                GTEST_SKIP() << "No device satisfies the ADR 0010 capability contract.";
            }
            if (volkInitialize() != VK_SUCCESS)
            {
                GTEST_SKIP() << "volk re-init failed.";
            }

            if (!ChangeToOloEditorDir())
            {
                GTEST_SKIP() << "Could not locate OloEditor/assets/shaders from CWD.";
            }

            m_Device = std::make_unique<VulkanDevice>();
            try
            {
                m_Device->Init([](VkInstance)
                               { return VkSurfaceKHR(VK_NULL_HANDLE); });
            }
            catch (const std::exception& e)
            {
                m_Device.reset();
                GTEST_SKIP() << "VulkanDevice::Init failed: " << e.what();
            }
            VulkanDevice::ResetValidationErrorCount();

            VkCommandBufferAllocateInfo cmdInfo{};
            cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cmdInfo.commandPool = m_Device->GetCommandPool();
            cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmdInfo.commandBufferCount = 1;
            ASSERT_EQ(vkAllocateCommandBuffers(m_Device->GetDevice(), &cmdInfo, &m_Cmd), VK_SUCCESS);

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            ASSERT_EQ(vkCreateFence(m_Device->GetDevice(), &fenceInfo, nullptr, &m_Fence), VK_SUCCESS);
        }

        void TearDown() override
        {
            if (!m_Device)
            {
                return;
            }
            vkDeviceWaitIdle(m_Device->GetDevice());
            // Process-wide singletons hold objects belonging to THIS test's
            // device — release before the device goes.
            VulkanPipelineBuilder::Get().ReleaseAll();
            VulkanResourceHeap::Get().Release();
            VulkanFrameArena::Get().ReleaseBuffers();
            VulkanDeferredReclaim::Get().FlushAll();
            VulkanPipelineCache::Get().SaveAndDestroy();
            if (m_Fence != VK_NULL_HANDLE)
            {
                vkDestroyFence(m_Device->GetDevice(), m_Fence, nullptr);
            }
            EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
                << "The Phase 5/6 bar: ZERO validation errors (sync validation included in debug builds)";
            m_Device->Shutdown();
            m_Device.reset();
        }

        void Submit(const std::function<void(VkCommandBuffer)>& record)
        {
            ASSERT_EQ(vkResetCommandBuffer(m_Cmd, 0), VK_SUCCESS);
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ASSERT_EQ(vkBeginCommandBuffer(m_Cmd, &beginInfo), VK_SUCCESS);
            record(m_Cmd);
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

    // Image helpers -----------------------------------------------------------

    struct OffscreenImage
    {
        VkImage Image = VK_NULL_HANDLE;
        VmaAllocation Allocation = VK_NULL_HANDLE;
        VkImageView AttachmentView = VK_NULL_HANDLE;
        VkFormat Format = VK_FORMAT_R8G8B8A8_UNORM;
        u32 Width = 0;
        u32 Height = 0;
    };

    OffscreenImage CreateOffscreen(VulkanDevice& device, u32 width, u32 height)
    {
        OffscreenImage img;
        img.Width = width;
        img.Height = height;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = img.Format;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(device.GetAllocator(), &imageInfo, &allocInfo, &img.Image, &img.Allocation, nullptr) !=
            VK_SUCCESS)
        {
            return {};
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = img.Image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = img.Format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device.GetDevice(), &viewInfo, nullptr, &img.AttachmentView) != VK_SUCCESS)
        {
            vmaDestroyImage(device.GetAllocator(), img.Image, img.Allocation);
            return {};
        }
        return img;
    }

    void DestroyOffscreen(VulkanDevice& device, OffscreenImage& img)
    {
        if (img.AttachmentView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device.GetDevice(), img.AttachmentView, nullptr);
        }
        if (img.Image != VK_NULL_HANDLE)
        {
            vmaDestroyImage(device.GetAllocator(), img.Image, img.Allocation);
        }
        img = {};
    }

    [[nodiscard]] VkImageViewCreateInfo SampledViewInfo(const OffscreenImage& img)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = img.Image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = img.Format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return viewInfo;
    }

    void CmdImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                         VkAccessFlags2 dstAccess)
    {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    void CmdBeginRendering(VkCommandBuffer cmd, const OffscreenImage& target, bool clear)
    {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = target.AttachmentView;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = { { 0, 0 }, { target.Width, target.Height } };
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);

        const VkViewport viewport{ 0.0f, 0.0f, static_cast<f32>(target.Width), static_cast<f32>(target.Height),
                                   0.0f, 1.0f };
        vkCmdSetViewportWithCount(cmd, 1, &viewport);
        const VkRect2D scissor{ { 0, 0 }, { target.Width, target.Height } };
        vkCmdSetScissorWithCount(cmd, 1, &scissor);
    }

    void CmdPushRootPointer(VkCommandBuffer cmd, VkDeviceAddress rootAddress)
    {
        VkPushDataInfoEXT pushInfo{};
        pushInfo.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT;
        pushInfo.offset = 0;
        pushInfo.data = { .address = &rootAddress, .size = sizeof(rootAddress) };
        vkCmdPushDataEXT(cmd, &pushInfo);
    }

    void ReadbackImage(VulkanShaderPipeline& fixture, VulkanDevice& device, const OffscreenImage& img,
                       VkImageLayout currentLayout, std::vector<u8>& outRgba8);

    // The FXAA input pattern — byte-for-byte the GoldenImageTests generator.
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
} // namespace

// -----------------------------------------------------------------------------
// A minimal source-string shader travels the whole thin-PSO + root-data path:
// vertex pulling from a frame-arena buffer (a root-struct SSBO address — §5's
// "the axis is removed"), a UBO block reached through INDIRECT_ADDRESS (§4),
// dynamic rendering, and a readback assertion on the produced pixels.
// -----------------------------------------------------------------------------
TEST_F(VulkanShaderPipeline, RootDataAndVertexPullingRenderATriangle)
{
    ScopedVulkanApiSelection vulkanSelected;

    // Vertex pulling: positions live in an SSBO the root struct points at.
    // The tint UBO proves the second mapping kind in the same draw.
    const std::string vertexSrc = R"(
#version 460 core
layout(std430, binding = 30) readonly buffer VertexPull { vec2 positions[]; };
layout(location = 0) out vec2 v_Uv;
void main()
{
    vec2 p = positions[gl_VertexIndex];
    v_Uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
    const std::string fragmentSrc = R"(
#version 460 core
layout(std140, binding = 31) uniform Tint { vec4 u_Tint; };
layout(location = 0) in vec2 v_Uv;
layout(location = 0) out vec4 o_Color;
void main()
{
    o_Color = vec4(u_Tint.rgb, 1.0);
}
)";

    Ref<Shader> shader = Shader::Create("Phase6PilotTriangle", vertexSrc, fragmentSrc);
    ASSERT_TRUE(shader);
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    auto* vkShader = static_cast<VulkanShader*>(shader.get());
    ASSERT_EQ(vkShader->GetBindings().size(), 2u) << "Reflection must surface the SSBO and the UBO";

    const VulkanRootDataLayout layout = VulkanRootDataLayout::Build(vkShader->GetBindings());
    ASSERT_NE(layout.Find(0, 30), nullptr);
    ASSERT_NE(layout.Find(0, 31), nullptr);
    EXPECT_EQ(layout.Find(0, 30)->Offset % 8, 0u);

    OffscreenImage target = CreateOffscreen(*m_Device, 64, 64);
    ASSERT_NE(target.Image, VK_NULL_HANDLE);

    auto& arena = VulkanFrameArena::Get();
    arena.BeginFrame(0);

    // Full-viewport triangle + a pure-green tint.
    const f32 positions[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    const auto vertexAlloc = arena.Push(positions, sizeof(positions), 16);
    ASSERT_TRUE(vertexAlloc.IsValid());
    const f32 tint[] = { 0.0f, 1.0f, 0.0f, 1.0f };
    const auto tintAlloc = arena.Push(tint, sizeof(tint), 256); // UBO-offset-aligned
    ASSERT_TRUE(tintAlloc.IsValid());

    // The root struct: written through the SAME layout the mappings use.
    std::vector<u8> rootData(layout.SizeBytes, 0);
    const u64 vertexAddress = vertexAlloc.Gpu;
    const u64 tintAddress = tintAlloc.Gpu;
    std::memcpy(rootData.data() + layout.Find(0, 30)->Offset, &vertexAddress, sizeof(u64));
    std::memcpy(rootData.data() + layout.Find(0, 31)->Offset, &tintAddress, sizeof(u64));
    const auto rootAlloc = arena.Push(rootData.data(), rootData.size(), 16);
    ASSERT_TRUE(rootAlloc.IsValid());

    VulkanRenderTargetDesc targets;
    targets.ColorCount = 1;
    targets.ColorFormats[0] = target.Format;

    VulkanRecordedPipelineState state{}; // defaults: no depth target used, no blend
    state.DepthTest = false;
    state.DepthWrite = false;

    const VkPipeline pipeline = VulkanPipelineBuilder::Get().GetOrCreateGraphics(
        *vkShader, layout, state, targets);
    ASSERT_NE(pipeline, VK_NULL_HANDLE);
    EXPECT_EQ(VulkanPipelineBuilder::Get().GetCachedPipelineCount(), 1u);
    // Second lookup hits the cache, no second pipeline.
    EXPECT_EQ(VulkanPipelineBuilder::Get().GetOrCreateGraphics(*vkShader, layout, state, targets), pipeline);

    Submit([&](VkCommandBuffer cmd)
           {
        VulkanResourceHeap::Get().CmdBind(cmd);
        CmdImageBarrier(cmd, target.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        CmdBeginRendering(cmd, target, /*clear=*/true);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VulkanPipelineBuilder::FlushDynamicState(cmd, state, targets);
        CmdPushRootPointer(cmd, rootAlloc.Gpu);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd); });

    std::vector<u8> pixels;
    ReadbackImage(*this, *m_Device, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, pixels);
    ASSERT_EQ(pixels.size(), 64u * 64u * 4u);

    // Every pixel must be the tint — a wrong root offset "samples a different
    // real buffer, not black" (ADR 0011's own framing), so assert the exact
    // value, not just non-black.
    u32 wrong = 0;
    for (sizet p = 0; p < pixels.size(); p += 4)
    {
        if (pixels[p + 0] != 0 || pixels[p + 1] != 255 || pixels[p + 2] != 0)
        {
            ++wrong;
        }
    }
    EXPECT_EQ(wrong, 0u) << "Root-data pointer / vertex pulling produced wrong pixels";

    DestroyOffscreen(*m_Device, target);
    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
}

namespace
{
    void ReadbackImage(VulkanShaderPipeline& fixture, VulkanDevice& device, const OffscreenImage& img,
                       VkImageLayout currentLayout, std::vector<u8>& outRgba8)
    {
        const VkDeviceSize byteSize = static_cast<VkDeviceSize>(img.Width) * img.Height * 4;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = byteSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo mapInfo{};
        ASSERT_EQ(vmaCreateBuffer(device.GetAllocator(), &bufferInfo, &allocInfo, &buffer, &allocation, &mapInfo),
                  VK_SUCCESS);

        // Static member access hack: reuse the fixture's Submit via friendship
        // is unnecessary — a local submit keeps this helper standalone.
        VkCommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAlloc.commandPool = device.GetCommandPool();
        cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        ASSERT_EQ(vkAllocateCommandBuffers(device.GetDevice(), &cmdAlloc, &cmd), VK_SUCCESS);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ASSERT_EQ(vkBeginCommandBuffer(cmd, &begin), VK_SUCCESS);

        CmdImageBarrier(cmd, img.Image, currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { img.Width, img.Height, 1 };
        vkCmdCopyImageToBuffer(cmd, img.Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        ASSERT_EQ(vkQueueSubmit(device.GetQueue(), 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);
        ASSERT_EQ(vkQueueWaitIdle(device.GetQueue()), VK_SUCCESS);

        outRgba8.resize(static_cast<sizet>(byteSize));
        std::memcpy(outRgba8.data(), mapInfo.pMappedData, static_cast<sizet>(byteSize));

        vkFreeCommandBuffers(device.GetDevice(), device.GetCommandPool(), 1, &cmd);
        vmaDestroyBuffer(device.GetAllocator(), buffer, allocation);
        (void)fixture;
    }
} // namespace

// -----------------------------------------------------------------------------
// THE PHASE 6 CHECKPOINT. The golden-tested pass (PostProcess_FXAA.glsl,
// GoldenImageTest.FxaaHardEdgeGolden) renders on Vulkan end-to-end in the
// target shape: real shader file through the vulkan_1_4 SPIR-V tier, input
// texture uploaded and sampled through the DESCRIPTOR HEAP with the heap slot
// index and the PostProcess UBO address carried in ONE root struct, pushed as
// ONE 8-byte pointer. Output is held to the GL golden baseline (RMSE) and the
// #734 driver-independent property invariants.
// -----------------------------------------------------------------------------
TEST_F(VulkanShaderPipeline, FxaaGoldenPassRendersCorrectlyOnVulkan)
{
    ScopedVulkanApiSelection vulkanSelected;

    constexpr u32 kSize = 128;

    // --- The golden input, uploaded through the frame arena ------------------
    const std::vector<f32> pattern = MakeHardEdgePattern(kSize);
    std::vector<u8> patternRgba8(static_cast<sizet>(kSize) * kSize * 4);
    for (sizet i = 0; i < patternRgba8.size(); ++i)
    {
        patternRgba8[i] = static_cast<u8>(std::lround(std::clamp(pattern[i], 0.0f, 1.0f) * 255.0f));
    }

    OffscreenImage input = CreateOffscreen(*m_Device, kSize, kSize);
    OffscreenImage output = CreateOffscreen(*m_Device, kSize, kSize);
    ASSERT_NE(input.Image, VK_NULL_HANDLE);
    ASSERT_NE(output.Image, VK_NULL_HANDLE);

    auto& arena = VulkanFrameArena::Get();
    arena.BeginFrame(0);
    const auto staging = arena.Push(patternRgba8.data(), patternRgba8.size(), 16);
    ASSERT_TRUE(staging.IsValid());

    // --- The golden-tested shader, through the Vulkan tier -------------------
    Ref<Shader> shader = Shader::Create("assets/shaders/PostProcess_FXAA.glsl");
    ASSERT_TRUE(shader);
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready)
        << "PostProcess_FXAA.glsl must compile through shaderc(vulkan_1_4)";
    auto* vkShader = static_cast<VulkanShader*>(shader.get());

    const VulkanRootDataLayout layout = VulkanRootDataLayout::Build(vkShader->GetBindings());
    const auto* uboField = layout.Find(0, 7);   // PostProcessUBO
    const auto* texField = layout.Find(0, 0);   // u_Texture
    const auto* pullField = layout.Find(0, 57); // OloVertexPull (§5 conversion)
    ASSERT_NE(uboField, nullptr) << "FXAA must declare the PostProcess UBO at binding 7";
    ASSERT_NE(texField, nullptr) << "FXAA must declare its input sampler at binding 0";
    ASSERT_NE(pullField, nullptr) << "FXAA's OLO_VULKAN vertex stage must declare the vertex-pull SSBO at binding 57";

    // The fullscreen triangle, in the exact {vec3 position, vec2 uv} stream
    // the attribute path consumes (MeshPrimitives::GetFullscreenTriangle).
    const f32 fullscreenTriangle[] = {
        -1.0f,
        -1.0f,
        0.0f,
        0.0f,
        0.0f, //
        3.0f,
        -1.0f,
        0.0f,
        2.0f,
        0.0f, //
        -1.0f,
        3.0f,
        0.0f,
        0.0f,
        2.0f, //
    };

    // --- Heap descriptor for the input ---------------------------------------
    ASSERT_TRUE(VulkanResourceHeap::Get().EnsureCreated());
    const u32 slot = VulkanResourceHeap::Get().AllocateSlot();
    ASSERT_NE(slot, VulkanResourceHeap::InvalidSlot);
    ASSERT_TRUE(VulkanResourceHeap::Get().WriteSampledImage(slot, SampledViewInfo(input),
                                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

    // --- PostProcess UBO payload ---------------------------------------------
    // Field defaults are the engine's own (Gamma 2.2, Reinhard); FXAA reads
    // the texel-size pair. The struct is the engine's C++ std140 mirror.
    OloEngine::PostProcessUBOData ubo{};
    ubo.TexelSizeX = 1.0f / static_cast<f32>(kSize);
    ubo.TexelSizeY = 1.0f / static_cast<f32>(kSize);
    ubo.InverseScreenWidth = 1.0f / static_cast<f32>(kSize);
    ubo.InverseScreenHeight = 1.0f / static_cast<f32>(kSize);
    const auto uboAlloc = arena.Push(&ubo, sizeof(ubo), 256);
    ASSERT_TRUE(uboAlloc.IsValid());

    const auto pullAlloc = arena.Push(fullscreenTriangle, sizeof(fullscreenTriangle), 16);
    ASSERT_TRUE(pullAlloc.IsValid());

    // --- The root struct ------------------------------------------------------
    std::vector<u8> rootData(layout.SizeBytes, 0);
    const u64 uboAddress = uboAlloc.Gpu;
    const u64 pullAddress = pullAlloc.Gpu;
    std::memcpy(rootData.data() + uboField->Offset, &uboAddress, sizeof(u64));
    std::memcpy(rootData.data() + pullField->Offset, &pullAddress, sizeof(u64));
    std::memcpy(rootData.data() + texField->Offset, &slot, sizeof(u32));
    const auto rootAlloc = arena.Push(rootData.data(), rootData.size(), 16);
    ASSERT_TRUE(rootAlloc.IsValid());

    VulkanRenderTargetDesc targets;
    targets.ColorCount = 1;
    targets.ColorFormats[0] = output.Format;
    VulkanRecordedPipelineState state{};
    state.DepthTest = false;
    state.DepthWrite = false;

    const VkPipeline pipeline =
        VulkanPipelineBuilder::Get().GetOrCreateGraphics(*vkShader, layout, state, targets);
    ASSERT_NE(pipeline, VK_NULL_HANDLE);

    Submit([&](VkCommandBuffer cmd)
           {
        VulkanResourceHeap::Get().CmdBind(cmd);

        // Upload: arena staging -> input image, then to SHADER_READ_ONLY.
        CmdImageBarrier(cmd, input.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = staging.Offset;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { kSize, kSize, 1 };
        // The arena's slot-0 buffer backs the staging allocation.
        vkCmdCopyBufferToImage(cmd, VulkanFrameArena::Get().GetSlotBuffer(0), input.Image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        CmdImageBarrier(cmd, input.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        // FXAA fullscreen draw — the OLO_VULKAN vertex stage pulls the
        // triangle from the arena buffer the root struct points at (§5).
        CmdImageBarrier(cmd, output.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        CmdBeginRendering(cmd, output, /*clear=*/true);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VulkanPipelineBuilder::FlushDynamicState(cmd, state, targets);
        CmdPushRootPointer(cmd, rootAlloc.Gpu);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd); });

    std::vector<u8> result;
    ReadbackImage(*this, *m_Device, output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, result);
    ASSERT_EQ(result.size(), static_cast<sizet>(kSize) * kSize * 4);

    // --- Hold to the GL golden baseline --------------------------------------
    {
        int w = 0;
        int h = 0;
        int comp = 0;
        stbi_uc* golden = stbi_load("assets/tests/golden/fxaa_hard_edge.png", &w, &h, &comp, 4);
        ASSERT_NE(golden, nullptr) << "Golden baseline missing (assets/tests/golden/fxaa_hard_edge.png)";
        ASSERT_EQ(w, static_cast<int>(kSize));
        ASSERT_EQ(h, static_cast<int>(kSize));

        f64 sumSq = 0.0;
        for (sizet i = 0; i < result.size(); ++i)
        {
            const f64 diff = (static_cast<f64>(result[i]) - static_cast<f64>(golden[i])) / 255.0;
            sumSq += diff * diff;
        }
        stbi_image_free(golden);
        const f64 rmse = std::sqrt(sumSq / static_cast<f64>(result.size()));
        EXPECT_LT(rmse, 0.02) << "Vulkan FXAA output diverges from the GL golden beyond the hard-fail band";
        OLO_CORE_INFO("[Phase6] Vulkan FXAA vs GL golden RMSE = {}", rmse);
    }

    // --- The #734 driver-independent property invariants ---------------------
    constexpr u32 kLsbTolerance = 1;
    std::vector<u32> darkBlends;
    std::vector<u32> brightBlends;
    for (sizet p = 0; p < static_cast<sizet>(kSize) * kSize; ++p)
    {
        const u32 outValue = result[p * 4];
        if (pattern[p * 4] > 0.5f)
        {
            if (outValue + kLsbTolerance < 255u)
            {
                brightBlends.push_back(outValue);
            }
        }
        else if (outValue > kLsbTolerance)
        {
            darkBlends.push_back(outValue);
        }
    }
    const u32 altered = static_cast<u32>(darkBlends.size() + brightBlends.size());
    const f32 alteredFraction = static_cast<f32>(altered) / static_cast<f32>(kSize * kSize);
    EXPECT_GT(alteredFraction, 0.005f) << "FXAA anti-aliased nothing — the input never reached the shader";
    EXPECT_LT(alteredFraction, 0.10f) << "FXAA smeared the frame — parameters reached it wrong";

    std::ranges::sort(darkBlends);
    std::ranges::sort(brightBlends, std::greater<>());
    const sizet pairs = std::min(darkBlends.size(), brightBlends.size());
    u32 mismatched = 0;
    for (sizet i = 0; i < pairs; ++i)
    {
        const u32 sum = darkBlends[i] + brightBlends[i];
        if (sum + 2 < 255u || sum > 257u)
        {
            ++mismatched;
        }
    }
    EXPECT_LT(static_cast<f32>(mismatched), static_cast<f32>(pairs) * 0.05f)
        << "FXAA's complementary-blend invariant broke — asymmetric filtering";

    DestroyOffscreen(*m_Device, input);
    DestroyOffscreen(*m_Device, output);
    EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u);
}

#endif // OLO_WITH_VULKAN
