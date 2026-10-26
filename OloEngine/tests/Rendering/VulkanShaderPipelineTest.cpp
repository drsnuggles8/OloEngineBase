// OLO_TEST_LAYER: plumbing
//
// #691 — the shader path end to end on a live device: VulkanShader
// (direct SPIR-V, vulkan_1_4 tier), VulkanResourceHeap (VK_EXT_descriptor_heap
// descriptors), VulkanPipelineBuilder (thin PSO, dynamic rendering, root-data
// binding mappings), VulkanFrameArena (root structs in mapped BDA memory) and
// vkCmdPushDataEXT — the ADR 0011 §4/§5 shape every ported pass copies.
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

#include "OloEngine/Renderer/PipelineManifest.h"
#include "OloEngine/Renderer/PipelineReadiness.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderRegistry.h"
#include "../TestTempDir.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanPipelinePrecache.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <volk.h>

#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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
        // Non-throwing overloads throughout: a filesystem failure must skip
        // the fixture, not abort SetUp with an unhandled exception (the
        // RenderPropertyTest helper's rule).
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path candidate = fs::current_path(ec);
        if (ec)
        {
            return false;
        }
        for (int i = 0; i < 6; ++i)
        {
            const fs::path editorDir = candidate / "OloEditor";
            if (fs::exists(editorDir / "assets" / "shaders", ec) && !ec)
            {
                fs::current_path(editorDir, ec);
                return !ec;
            }
            ec.clear();
            if (!candidate.has_parent_path() || candidate.parent_path() == candidate)
            {
                break;
            }
            candidate = candidate.parent_path();
        }
        const bool alreadyThere = fs::exists(fs::path("assets") / "shaders", ec) && !ec;
        return alreadyThere;
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

            // Both results checked + the VK_INCOMPLETE resize (the exec-test
            // ladder): trailing VK_NULL_HANDLE entries must never reach
            // VulkanCapabilities::Evaluate.
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
                    devices.resize(deviceCount);
                }
                else
                {
                    vkDestroyInstance(probe, nullptr);
                    GTEST_SKIP() << "vkEnumeratePhysicalDevices (list) failed on this machine.";
                }
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
                << "The bar: ZERO validation errors (sync validation included in debug builds)";
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

    Ref<Shader> shader = Shader::Create("PilotTriangle", vertexSrc, fragmentSrc);
    ASSERT_TRUE(shader);
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    auto* vkShader = static_cast<VulkanShader*>(shader.get());
    ASSERT_EQ(vkShader->GetBindings().Num(), 2u) << "Reflection must surface the SSBO and the UBO";

    const VulkanRootDataLayout layout = VulkanRootDataLayout::Build(std::span{ vkShader->GetBindings().GetData(), static_cast<sizet>(vkShader->GetBindings().Num()) });
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

        // HOST_ACCESS_RANDOM prefers HOST_CACHED memory, which may be
        // non-coherent — device writes need an explicit invalidate before the
        // host read (no-op on coherent memory). A failed invalidate means the
        // read below could be stale — stop rather than compare garbage.
        ASSERT_EQ(vmaInvalidateAllocation(device.GetAllocator(), allocation, 0, byteSize), VK_SUCCESS);

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

    const VulkanRootDataLayout layout = VulkanRootDataLayout::Build(std::span{ vkShader->GetBindings().GetData(), static_cast<sizet>(vkShader->GetBindings().Num()) });
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
        OLO_CORE_INFO("[VulkanFXAA] vs GL golden RMSE = {}", rmse);
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
    ASSERT_GT(pairs, 0u) << "FXAA produced blends in only one direction (dark=" << darkBlends.size()
                         << ", bright=" << brightBlends.size() << ") — the complementary invariant is untestable";
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

TEST_F(VulkanShaderPipeline, SuccessfulReloadAdvancesRevisionAndFailurePreservesLiveProgram)
{
    // Isolate both the temporary source and Vulkan's relative shader cache.
    // Never leave cache entries whose source disappears after this test.
    struct ScratchDirectory
    {
        std::filesystem::path Previous = std::filesystem::current_path();
        std::filesystem::path Directory = OloEngine::Tests::TempDir("reload");
        ~ScratchDirectory()
        {
            std::error_code error;
            std::filesystem::current_path(Previous, error);
        }
    } scratch;
    std::filesystem::current_path(scratch.Directory);
    const auto sourcePath = scratch.Directory / "reload.glsl";
    const auto writeSource = [&sourcePath](std::string_view fragmentBody)
    {
        std::ofstream source(sourcePath, std::ios::trunc);
        source << "#type vertex\n#version 460 core\n"
                  "void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n"
                  "#type fragment\n#version 460 core\n"
                  "layout(location = 0) out vec4 color;\nvoid main() { "
               << fragmentBody << " }\n";
        source.close();
        return !source.fail();
    };
    ASSERT_TRUE(writeSource("color = vec4(1.0, 0.0, 0.0, 1.0);"));
    auto shader = Ref<VulkanShader>::Create(sourcePath.generic_string());
    ASSERT_TRUE(shader->IsReady());
    EXPECT_EQ(shader->GetReloadRevision(), 0u);
    const auto identity = shader->GetRHIHandle();
    const auto initialFragment = shader->GetModule(VK_SHADER_STAGE_FRAGMENT_BIT);
    ASSERT_NE(initialFragment, VK_NULL_HANDLE);

    ASSERT_TRUE(writeSource("color = vec4(0.0, 1.0, 0.0, 1.0);"));
    ASSERT_TRUE(shader->Reload());
    EXPECT_EQ(shader->GetReloadRevision(), 1u);
    EXPECT_EQ(shader->GetRHIHandle(), identity);
    ASSERT_TRUE(shader->IsReady());
    const auto replacementFragment = shader->GetModule(VK_SHADER_STAGE_FRAGMENT_BIT);
    EXPECT_NE(replacementFragment, initialFragment);
    const auto replacementSpirv = shader->GetSPIRV();

    ASSERT_TRUE(writeSource("color = undeclared_reload_identifier;"));
    EXPECT_FALSE(shader->Reload());
    EXPECT_EQ(shader->GetReloadRevision(), 1u);
    EXPECT_EQ(shader->GetRHIHandle(), identity);
    EXPECT_TRUE(shader->IsReady());
    EXPECT_EQ(shader->GetModule(VK_SHADER_STAGE_FRAGMENT_BIT), replacementFragment);
    EXPECT_EQ(shader->GetSPIRV(), replacementSpirv);
}

// -----------------------------------------------------------------------------
// Issue #607: a shader edit reaches the PIXELS after a reload on Vulkan. The
// module test above stops at "new VkShaderModule"; what olo_shader_reload
// promises is a different frame, and between the two sits the PSO cache. A
// pipeline built from the old modules and still cached would draw the old
// colour with every module check green. So: draw red through the pipeline
// builder, edit the file to green, reload, draw again through the SAME lookup
// the draw path uses, and require green, plus the pipeline counts the MCP
// tool reports. Negative control: with the InvalidateShader call in
// VulkanShader::Reload removed, the second draw is red and this fails.
// -----------------------------------------------------------------------------
TEST_F(VulkanShaderPipeline, ReloadedShaderEditReachesThePixelsThroughThePipelineCache)
{
    ScopedVulkanApiSelection vulkanSelected;

    struct ScratchDirectory
    {
        std::filesystem::path Previous = std::filesystem::current_path();
        std::filesystem::path Directory = OloEngine::Tests::TempDir("reload-pixels");
        ~ScratchDirectory()
        {
            std::error_code error;
            std::filesystem::current_path(Previous, error);
        }
    } scratch;
    std::filesystem::current_path(scratch.Directory);
    const auto sourcePath = scratch.Directory / "ReloadPixels.glsl";
    const auto writeSource = [&sourcePath](std::string_view colour)
    {
        std::ofstream source(sourcePath, std::ios::trunc);
        // A full-viewport triangle from gl_VertexIndex: no vertex buffer, so
        // the root struct carries nothing and only the shader decides the pixel.
        source << "#type vertex\n#version 460 core\n"
                  "void main() {\n"
                  "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
                  "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                  "}\n"
                  "#type fragment\n#version 460 core\n"
                  "layout(location = 0) out vec4 o_Color;\n"
                  "void main() { o_Color = "
               << colour << "; }\n";
        source.close();
        return !source.fail();
    };

    OffscreenImage target = CreateOffscreen(*m_Device, 16, 16);
    ASSERT_NE(target.Image, VK_NULL_HANDLE);
    VulkanRenderTargetDesc targets;
    targets.ColorCount = 1;
    targets.ColorFormats[0] = target.Format;
    VulkanRecordedPipelineState state{};
    state.DepthTest = false;
    state.DepthWrite = false;

    auto& arena = VulkanFrameArena::Get();
    arena.BeginFrame(0);
    const u64 rootPlaceholder = 0;
    const auto rootAlloc = arena.Push(&rootPlaceholder, sizeof(rootPlaceholder), 16);
    ASSERT_TRUE(rootAlloc.IsValid());

    // Draw once through GetOrCreateGraphics, exactly as the draw path does, and
    // return the centre pixel.
    const auto drawCentre = [&](VulkanShader& shader) -> std::array<u8, 4>
    {
        const VulkanRootDataLayout& layout = shader.GetRootDataLayout();
        const VkPipeline pipeline = VulkanPipelineBuilder::Get().GetOrCreateGraphics(shader, layout, state, targets);
        EXPECT_NE(pipeline, VK_NULL_HANDLE);
        Submit([&](VkCommandBuffer cmd)
               {
            VulkanResourceHeap::Get().CmdBind(cmd);
            CmdImageBarrier(cmd, target.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            CmdBeginRendering(cmd, target, /*clear=*/true);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VulkanPipelineBuilder::FlushDynamicState(cmd, state, targets);
            CmdPushRootPointer(cmd, rootAlloc.Gpu);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd); });
        std::vector<u8> pixels;
        ReadbackImage(*this, *m_Device, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, pixels);
        const sizet centre = (static_cast<sizet>(8) * 16 + 8) * 4;
        if (pixels.size() < centre + 4)
            return { 0, 0, 0, 0 };
        return { pixels[centre], pixels[centre + 1], pixels[centre + 2], pixels[centre + 3] };
    };

    ASSERT_TRUE(writeSource("vec4(1.0, 0.0, 0.0, 1.0)"));
    auto shader = Ref<VulkanShader>::Create(sourcePath.generic_string());
    ASSERT_TRUE(shader->IsReady());

    // File-backed Vulkan shaders resolve by name, which is how
    // olo_shader_reload finds a pass-owned one.
    auto byName = ShaderRegistry::Get().FindShaders("ReloadPixels");
    EXPECT_TRUE(std::ranges::any_of(byName, [&shader](const Ref<Shader>& found)
                                    { return found.Raw() == shader.Raw(); }))
        << "a file-backed VulkanShader must register itself for reload by name";

    const auto red = drawCentre(*shader);
    EXPECT_EQ(red[0], 255);
    EXPECT_EQ(red[1], 0);
    ASSERT_EQ(shader->GetPipelineState().Live, 1u);

    ASSERT_TRUE(writeSource("vec4(0.0, 1.0, 0.0, 1.0)"));
    ASSERT_TRUE(shader->Reload());

    // The reload invalidated the one pipeline and nothing has rebuilt it yet:
    // the state olo_shader_reload reports as `pending`, not `ready`.
    ShaderPipelineState afterReload = shader->GetPipelineState();
    EXPECT_TRUE(afterReload.Tracked);
    EXPECT_EQ(afterReload.InvalidatedByLastReload, 1u);
    EXPECT_EQ(afterReload.Live, 0u);
    EXPECT_FALSE(afterReload.CreationFailed);

    const auto green = drawCentre(*shader);
    EXPECT_EQ(green[0], 0) << "the reload left a pipeline built from the OLD modules in the cache";
    EXPECT_EQ(green[1], 255);
    EXPECT_EQ(shader->GetPipelineState().Live, 1u) << "the draw rebuilt exactly one pipeline from the new modules";

    DestroyOffscreen(*m_Device, target);
    byName.clear(); // the lookup handed out strong Refs
    shader.Reset();
    EXPECT_TRUE(ShaderRegistry::Get().FindShaders("ReloadPixels").empty()) << "the destructor must unregister";
}

// =============================================================================
// #1345 — PSO precaching through the REAL builder on a device. The ledger's
// state machine is pinned device-free (PipelinePrecacheContractTest.cpp); these
// pin that the production paths drive it: a request created off the draw is a
// prepared hit at the first draw, a draw that beats its request (or had none)
// is counted as such and still gets a real pipeline, incompatible targets get
// distinct VkPipelines, a failed creation is counted and retried, a hot reload
// re-prepares its dependants against the NEW modules before any draw, and a
// readiness claim that a draw then contradicts is counted.
// =============================================================================
namespace
{
    // No bindings, no vertex buffer: only the shader and the targets decide the
    // pipeline. A full-viewport triangle from gl_VertexIndex.
    Ref<Shader> MakeConstantShader(const std::string& name, const char* colour = "vec4(1.0, 0.0, 1.0, 1.0)")
    {
        const std::string vertex = "#version 460 core\n"
                                   "void main() {\n"
                                   "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
                                   "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                                   "}\n";
        const std::string fragment = std::string("#version 460 core\nlayout(location = 0) out vec4 o_Color;\n"
                                                 "void main() { o_Color = ") +
                                     colour + "; }\n";
        return Shader::Create(name, vertex, fragment);
    }

    VulkanRenderTargetDesc ColourTargets(const VkFormat format, const u32 samples = 1,
                                         const VkFormat depth = VK_FORMAT_UNDEFINED)
    {
        VulkanRenderTargetDesc targets;
        targets.ColorCount = 1;
        targets.ColorFormats[0] = format;
        targets.DepthFormat = depth;
        targets.Samples = samples;
        return targets;
    }

    VulkanRecordedPipelineState NoDepthState()
    {
        VulkanRecordedPipelineState state{};
        state.DepthTest = false;
        state.DepthWrite = false;
        return state;
    }

    // Starts the collector without workers or files, so a test drives the queue
    // with PrepareQueued and nothing touches the machine's manifest.
    struct ScopedPrecache
    {
        explicit ScopedPrecache(std::string_view tag = {})
        {
            VulkanPipelinePrecache::Get().Start({ .LoadAndSaveManifest = false, .StartWorkers = false });
            VulkanPipelinePrecache::Get().SetConfigurationTag(tag);
        }
        ~ScopedPrecache()
        {
            VulkanPipelinePrecache::Get().Stop();
            VulkanPipelinePrecache::Get().SetManifestForTesting({});
            VulkanPipelinePrecache::Get().SetConfigurationTag({});
        }
        ScopedPrecache(const ScopedPrecache&) = delete;
        ScopedPrecache& operator=(const ScopedPrecache&) = delete;
    };
} // namespace

TEST_F(VulkanShaderPipeline, RequestedPipelineIsAPreparedHitAtItsFirstDraw)
{
    ScopedVulkanApiSelection vulkanSelected;
    auto& builder = VulkanPipelineBuilder::Get();
    builder.ResetRequestCounters();

    Ref<Shader> shader = MakeConstantShader("PsoPrepared");
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);
    auto& vkShader = *static_cast<VulkanShader*>(shader.get());
    const auto targets = ColourTargets(VK_FORMAT_R8G8B8A8_UNORM);

    const auto key = builder.RequestGraphics(vkShader, targets, 0);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(builder.GetRequestState(*key), VulkanPipelineBuilder::RequestState::Queued);
    EXPECT_EQ(builder.GetCachedPipelineCount(), 0u) << "A request creates nothing on the asking thread";

    ASSERT_EQ(VulkanPipelinePrecache::Get().PrepareQueued(8), 1u);
    EXPECT_EQ(builder.GetRequestState(*key), VulkanPipelineBuilder::RequestState::Ready);
    EXPECT_EQ(builder.GetCachedPipelineCount(), 1u);

    const VkPipeline pipeline =
        builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), NoDepthState(), targets);
    EXPECT_NE(pipeline, VK_NULL_HANDLE);
    EXPECT_EQ(builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), NoDepthState(), targets), pipeline);

    const auto stats = builder.GetRequestStats();
    EXPECT_EQ(stats.Counters.PreparedFirstUses, 1u) << "Counted once per pipeline, not per draw";
    EXPECT_EQ(stats.Counters.Misses(), 0u);
    EXPECT_EQ(stats.Counters.BlockingCreations, 0u) << "No synchronous creation on the draw";
    EXPECT_EQ(stats.Counters.PreparedCreations, 1u);
    EXPECT_GT(stats.Counters.PreparedCreateMs, 0.0);
    EXPECT_EQ(stats.Counters.DriverCacheHits + stats.Counters.DriverCacheMisses + stats.Counters.DriverCacheUnknown,
              1u)
        << "Creation feedback classifies the one creation";
}

TEST_F(VulkanShaderPipeline, ADrawThatBeatsOrLacksItsRequestIsCountedAndStillGetsAPipeline)
{
    ScopedVulkanApiSelection vulkanSelected;
    auto& builder = VulkanPipelineBuilder::Get();
    builder.ResetRequestCounters();

    Ref<Shader> shader = MakeConstantShader("PsoLate");
    auto& vkShader = *static_cast<VulkanShader*>(shader.get());
    const auto requested = ColourTargets(VK_FORMAT_R8G8B8A8_UNORM);
    const auto unrequested = ColourTargets(VK_FORMAT_R16G16B16A16_SFLOAT);

    ASSERT_TRUE(builder.RequestGraphics(vkShader, requested, 0).has_value());
    // The draw arrives before any worker ran the request.
    EXPECT_NE(builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), NoDepthState(), requested),
              VK_NULL_HANDLE);
    EXPECT_NE(builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), NoDepthState(), unrequested),
              VK_NULL_HANDLE);
    EXPECT_EQ(VulkanPipelinePrecache::Get().PrepareQueued(8), 0u) << "The taken-over request is not created twice";

    const auto stats = builder.GetRequestStats();
    EXPECT_EQ(stats.Counters.TooLateQueued, 1u);
    EXPECT_EQ(stats.Counters.NeverRequested, 1u);
    EXPECT_EQ(stats.Counters.BlockingCreations, 2u);
    EXPECT_EQ(stats.Counters.PreparedFirstUses, 0u);
    EXPECT_GT(stats.Counters.BlockingCreateMs, 0.0) << "The hitch is measured on the draw thread";
    EXPECT_EQ(builder.GetCachedPipelineCount(), 2u);
}

TEST_F(VulkanShaderPipeline, IncompatibleTargetsNeverShareAPipeline)
{
    ScopedVulkanApiSelection vulkanSelected;
    auto& builder = VulkanPipelineBuilder::Get();

    Ref<Shader> shader = MakeConstantShader("PsoKeys");
    Ref<Shader> variant = MakeConstantShader("PsoKeysVariant", "vec4(0.0, 1.0, 1.0, 1.0)");
    auto& vkShader = *static_cast<VulkanShader*>(shader.get());
    auto& vkVariant = *static_cast<VulkanShader*>(variant.get());
    const auto state = NoDepthState();
    const auto get = [&](VulkanShader& s, const VulkanRenderTargetDesc& targets)
    { return builder.GetOrCreateGraphics(s, s.GetRootDataLayout(), state, targets); };

    const VkPipeline single = get(vkShader, ColourTargets(VK_FORMAT_R8G8B8A8_UNORM));
    const VkPipeline msaa = get(vkShader, ColourTargets(VK_FORMAT_R8G8B8A8_UNORM, 4));
    const VkPipeline hdr = get(vkShader, ColourTargets(VK_FORMAT_R16G16B16A16_SFLOAT));
    const VkPipeline withDepth = get(vkShader, ColourTargets(VK_FORMAT_R8G8B8A8_UNORM, 1, VK_FORMAT_D32_SFLOAT));
    const VkPipeline otherMaterial = get(vkVariant, ColourTargets(VK_FORMAT_R8G8B8A8_UNORM));
    const std::array<VkPipeline, 5> all{ single, msaa, hdr, withDepth, otherMaterial };
    for (const VkPipeline pipeline : all)
    {
        ASSERT_NE(pipeline, VK_NULL_HANDLE);
    }
    for (sizet i = 0; i < all.size(); ++i)
    {
        for (sizet j = i + 1; j < all.size(); ++j)
        {
            EXPECT_NE(all[i], all[j]) << "configurations " << i << " and " << j << " share a pipeline";
        }
    }
    EXPECT_EQ(builder.GetCachedPipelineCount(), 5u);
    EXPECT_EQ(get(vkShader, ColourTargets(VK_FORMAT_R8G8B8A8_UNORM, 4)), msaa) << "The same configuration hits";
}

TEST_F(VulkanShaderPipeline, AFailedRequestIsCountedAndTheDrawRetriesIt)
{
    ScopedVulkanApiSelection vulkanSelected;
    auto& builder = VulkanPipelineBuilder::Get();
    builder.ResetRequestCounters();
    builder.ClearLastCreationFailure();

    Ref<Shader> shader = MakeConstantShader("PsoFails");
    auto& vkShader = *static_cast<VulkanShader*>(shader.get());
    const auto targets = ColourTargets(VK_FORMAT_R8G8B8A8_UNORM);

    const auto key = builder.RequestGraphics(vkShader, targets, 0);
    ASSERT_TRUE(key.has_value());
    builder.FailNextCreationsForTesting(1, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    ASSERT_EQ(VulkanPipelinePrecache::Get().PrepareQueued(8), 1u);
    EXPECT_EQ(builder.GetRequestState(*key), VulkanPipelineBuilder::RequestState::Failed);
    EXPECT_EQ(builder.GetRequestStats().Counters.FailedCreations, 1u);
    EXPECT_TRUE(vkShader.GetPipelineState().CreationFailed) << "olo_shader_reload's failure signal sees it too";
    EXPECT_EQ(builder.GetLastCreationFailure().Result, VK_ERROR_OUT_OF_DEVICE_MEMORY);

    const VkPipeline pipeline =
        builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), NoDepthState(), targets);
    EXPECT_NE(pipeline, VK_NULL_HANDLE);
    const auto stats = builder.GetRequestStats();
    EXPECT_EQ(stats.Counters.RetriedAfterFailure, 1u);
    EXPECT_EQ(stats.Counters.FailedCreations, 1u) << "The retry succeeded";
    EXPECT_EQ(builder.GetRequestState(*key), VulkanPipelineBuilder::RequestState::Ready);
    EXPECT_FALSE(vkShader.GetPipelineState().CreationFailed);
}

TEST_F(VulkanShaderPipeline, AReloadRePreparesItsDependentPipelinesAgainstTheNewModules)
{
    ScopedVulkanApiSelection vulkanSelected;
    ScopedPrecache precache;
    auto& builder = VulkanPipelineBuilder::Get();

    struct ScratchDirectory
    {
        std::filesystem::path Previous = std::filesystem::current_path();
        std::filesystem::path Directory = OloEngine::Tests::TempDir("pso-reload");
        ~ScratchDirectory()
        {
            std::error_code error;
            std::filesystem::current_path(Previous, error);
        }
    } scratch;
    std::filesystem::current_path(scratch.Directory);
    const auto sourcePath = scratch.Directory / "PsoReload.glsl";
    const auto writeSource = [&sourcePath](std::string_view colour)
    {
        std::ofstream source(sourcePath, std::ios::trunc);
        source << "#type vertex\n#version 460 core\n"
                  "void main() {\n"
                  "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
                  "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                  "}\n"
                  "#type fragment\n#version 460 core\n"
                  "layout(location = 0) out vec4 o_Color;\n"
                  "void main() { o_Color = "
               << colour << "; }\n";
        source.close();
        return !source.fail();
    };

    OffscreenImage target = CreateOffscreen(*m_Device, 16, 16);
    ASSERT_NE(target.Image, VK_NULL_HANDLE);
    const auto targets = ColourTargets(target.Format);
    const auto state = NoDepthState();
    auto& arena = VulkanFrameArena::Get();
    arena.BeginFrame(0);
    const u64 rootPlaceholder = 0;
    const auto rootAlloc = arena.Push(&rootPlaceholder, sizeof(rootPlaceholder), 16);
    ASSERT_TRUE(rootAlloc.IsValid());
    const auto drawCentre = [&](VulkanShader& shader) -> std::array<u8, 4>
    {
        const VkPipeline pipeline = builder.GetOrCreateGraphics(shader, shader.GetRootDataLayout(), state, targets);
        EXPECT_NE(pipeline, VK_NULL_HANDLE);
        Submit([&](VkCommandBuffer cmd)
               {
            VulkanResourceHeap::Get().CmdBind(cmd);
            CmdImageBarrier(cmd, target.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            CmdBeginRendering(cmd, target, /*clear=*/true);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VulkanPipelineBuilder::FlushDynamicState(cmd, state, targets);
            CmdPushRootPointer(cmd, rootAlloc.Gpu);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd); });
        std::vector<u8> pixels;
        ReadbackImage(*this, *m_Device, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, pixels);
        const sizet centre = (static_cast<sizet>(8) * 16 + 8) * 4;
        if (pixels.size() < centre + 4)
            return { 0, 0, 0, 0 };
        return { pixels[centre], pixels[centre + 1], pixels[centre + 2], pixels[centre + 3] };
    };

    ASSERT_TRUE(writeSource("vec4(1.0, 0.0, 0.0, 1.0)"));
    auto shader = Ref<VulkanShader>::Create(sourcePath.generic_string());
    ASSERT_TRUE(shader->IsReady());
    const auto red = drawCentre(*shader);
    EXPECT_EQ(red[0], 255);
    ASSERT_EQ(shader->GetPipelineState().Live, 1u);

    builder.ResetRequestCounters();
    ASSERT_TRUE(writeSource("vec4(0.0, 1.0, 0.0, 1.0)"));
    ASSERT_TRUE(shader->Reload());
    EXPECT_EQ(shader->GetPipelineState().Live, 0u) << "The old modules' pipeline is gone, not reused";
    EXPECT_EQ(builder.GetRequestStats().Counters.Requests, 1u) << "The dependent pipeline was re-requested";

    // Prepared off the draw: the pipeline exists BEFORE the next draw needs it.
    ASSERT_EQ(VulkanPipelinePrecache::Get().PrepareQueued(8), 1u);
    EXPECT_EQ(shader->GetPipelineState().Live, 1u);

    const auto green = drawCentre(*shader);
    EXPECT_EQ(green[0], 0) << "A pipeline built from the OLD modules reached the draw";
    EXPECT_EQ(green[1], 255);
    const auto stats = builder.GetRequestStats();
    EXPECT_EQ(stats.Counters.PreparedFirstUses, 1u);
    EXPECT_EQ(stats.Counters.BlockingCreations, 0u) << "The reload did not hitch the next draw";

    DestroyOffscreen(*m_Device, target);
    shader.Reset();
}

TEST_F(VulkanShaderPipeline, ReadinessComesFromTheManifestAndADrawThatContradictsItIsCounted)
{
    ScopedVulkanApiSelection vulkanSelected;
    auto& builder = VulkanPipelineBuilder::Get();
    builder.ResetRequestCounters();

    // The manifest says this shader was drawn into RGBA8 under the selected
    // configuration and into RGBA16F under another one.
    constexpr std::string_view kSelected = "Forward/msaa1/native/oit0";
    constexpr std::string_view kOther = "Forward/msaa4/native/oit0";
    PipelineManifest manifest;
    const u32 stages = static_cast<u32>(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    PipelineSignature rgba8;
    rgba8.ColorCount = 1;
    rgba8.ColorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    PipelineSignature rgba16;
    rgba16.ColorCount = 1;
    rgba16.ColorFormats[0] = VK_FORMAT_R16G16B16A16_SFLOAT;
    ASSERT_TRUE(manifest.Record("PsoReadiness", stages, rgba8, kSelected));
    ASSERT_TRUE(manifest.Record("PsoReadiness", stages, rgba16, kOther));

    ScopedPrecache precache(kSelected);
    VulkanPipelinePrecache::Get().SetManifestForTesting(manifest);

    // Content: the shader comes into existence and is requested at once — the
    // selected configuration's key and, speculatively, the other one.
    Ref<Shader> shader = MakeConstantShader("PsoReadiness");
    auto& vkShader = *static_cast<VulkanShader*>(shader.get());
    EXPECT_EQ(builder.GetRequestStats().Counters.Requests, 2u);

    PipelineReadinessSnapshot before;
    VulkanPipelinePrecache::Get().FillSnapshot(before);
    EXPECT_FALSE(before.RenderReady) << "Nothing is created yet";
    ASSERT_FALSE(before.PendingShaders.IsEmpty());

    ASSERT_EQ(VulkanPipelinePrecache::Get().PrepareQueued(8), 2u);
    PipelineReadinessSnapshot after;
    VulkanPipelinePrecache::Get().FillSnapshot(after);
    EXPECT_TRUE(after.RenderReady);
    EXPECT_GE(after.ReadyShaders, 1u);
    EXPECT_TRUE(after.PendingShaders.IsEmpty());

    // Both configurations' draws are prepared hits: a switch to the other
    // configuration found its pipeline ready.
    const auto state = NoDepthState();
    EXPECT_NE(builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), state,
                                          ColourTargets(VK_FORMAT_R8G8B8A8_UNORM)),
              VK_NULL_HANDLE);
    EXPECT_NE(builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), state,
                                          ColourTargets(VK_FORMAT_R16G16B16A16_SFLOAT)),
              VK_NULL_HANDLE);
    EXPECT_EQ(builder.GetRequestStats().Counters.PreparedFirstUses, 2u);
    EXPECT_EQ(builder.GetRequestStats().Counters.Misses(), 0u);

    // A draw the manifest never saw, on a shader declared ready: synchronous,
    // and counted as a broken claim rather than hidden behind it.
    EXPECT_NE(builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), state,
                                          ColourTargets(VK_FORMAT_B8G8R8A8_UNORM)),
              VK_NULL_HANDLE);
    const auto stats = builder.GetRequestStats();
    EXPECT_EQ(stats.Counters.NeverRequested, 1u);
    EXPECT_EQ(stats.Counters.ReadyClaimViolations, 1u);

    // The draw path recorded it, so the next session requests it; and a
    // PREPARED hit refreshed its own entry too: the RGBA16F key, recorded only
    // under the other configuration, was drawn under the selected one and now
    // says so (otherwise a key that keeps hitting would never be re-recorded,
    // and would be the first one Trim() drops).
    const auto recorded = VulkanPipelinePrecache::Get().CopyManifest();
    const auto entries = recorded.EntriesFor("PsoReadiness", stages, kSelected);
    EXPECT_EQ(entries.Num(), 3);
    const auto* hdr = entries.FindByPredicate([](const PipelineManifestEntry* e)
                                              { return e->Signature.ColorFormats[0] == VK_FORMAT_R16G16B16A16_SFLOAT; });
    ASSERT_NE(hdr, nullptr);
    EXPECT_TRUE((*hdr)->SeenUnder(kSelected));
    EXPECT_TRUE((*hdr)->SeenUnder(kOther));

    // A configuration switch: the renderer publishes the other tag, and its
    // draws start before the tag settles. The claim made for the selected
    // configuration is withdrawn at once, so a draw the new configuration
    // needs is an ordinary counted miss, not a broken claim.
    PipelineReadinessSnapshot claimed;
    VulkanPipelinePrecache::Get().FillSnapshot(claimed);
    ASSERT_TRUE(claimed.RenderReady);
    VulkanPipelinePrecache::Get().SetConfigurationTag(kOther);
    EXPECT_NE(builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), state,
                                          ColourTargets(VK_FORMAT_A2B10G10R10_UNORM_PACK32)),
              VK_NULL_HANDLE);
    const auto switched = builder.GetRequestStats().Counters;
    EXPECT_EQ(switched.NeverRequested, 2u);
    EXPECT_EQ(switched.ReadyClaimViolations, 1u) << "A draw during a switch was judged against the old claim";
    PipelineReadinessSnapshot settling;
    VulkanPipelinePrecache::Get().FillSnapshot(settling);
    EXPECT_FALSE(settling.RenderReady) << "Nothing is ready while the drawn configuration is not the selected one";
}

TEST_F(VulkanShaderPipeline, RequestsArePreparedByTheWorkersWhileNoFrameTicks)
{
    // A packaged runtime loads for tens of seconds before its first frame. The
    // workers' budget normally comes from Tick(); with no frame they must refill
    // it themselves, or every request made during loading lands on the first
    // frame as a synchronous creation (measured: 5.6 s of blocking in a cooked
    // Samoyed runtime, against 32 ms with this refill).
    ScopedVulkanApiSelection vulkanSelected;
    auto& builder = VulkanPipelineBuilder::Get();
    builder.ResetRequestCounters();

    constexpr std::string_view kTag = "Forward/msaa1/native/oit0";
    PipelineManifest manifest;
    PipelineSignature rgba8;
    rgba8.ColorCount = 1;
    rgba8.ColorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    ASSERT_TRUE(manifest.Record("PsoLoading", static_cast<u32>(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
                                rgba8, kTag));

    VulkanPipelinePrecache::Get().Start({ .LoadAndSaveManifest = false, .StartWorkers = true });
    VulkanPipelinePrecache::Get().SetConfigurationTag(kTag);
    VulkanPipelinePrecache::Get().SetManifestForTesting(manifest);
    Ref<Shader> shader = MakeConstantShader("PsoLoading");
    auto& vkShader = *static_cast<VulkanShader*>(shader.get());
    const auto key = builder.RequestGraphics(vkShader, ColourTargets(VK_FORMAT_R8G8B8A8_UNORM), 0);
    ASSERT_TRUE(key.has_value());

    // No Tick(), no PrepareQueued(): only the workers' own refill can run it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (builder.GetRequestState(*key) != VulkanPipelineBuilder::RequestState::Ready &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    VulkanPipelinePrecache::Get().Stop();
    VulkanPipelinePrecache::Get().SetManifestForTesting({});

    EXPECT_EQ(builder.GetRequestState(*key), VulkanPipelineBuilder::RequestState::Ready)
        << "A request made while no frame ticks was never prepared";
    EXPECT_NE(builder.GetOrCreateGraphics(vkShader, vkShader.GetRootDataLayout(), NoDepthState(),
                                          ColourTargets(VK_FORMAT_R8G8B8A8_UNORM)),
              VK_NULL_HANDLE);
    const auto stats = builder.GetRequestStats().Counters;
    EXPECT_GE(stats.PreparedCreations, 1u);
    EXPECT_EQ(stats.BlockingCreations, 0u);
    EXPECT_EQ(stats.PreparedFirstUses, 1u);
}

TEST_F(VulkanShaderPipeline, CompileCacheHitModuleAndPipelineCreationAreCountedApart)
{
    ScopedVulkanApiSelection vulkanSelected;
    auto& builder = VulkanPipelineBuilder::Get();
    builder.ResetRequestCounters();

    // A source no cache has seen: the token makes every SPIR-V cache key new.
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto sourcePath = OloEngine::Tests::TempFile("PsoPreparation.glsl");
    {
        std::ofstream source(sourcePath, std::ios::trunc);
        // Both stages carry the token: a stage whose text was ever compiled
        // before is a cache hit, and the counts below would depend on run order.
        source << "#type vertex\n#version 460 core\n"
               << "const float kUniqueV = float(" << (token % 1000000) << "u % 991u) / 991.0;\n"
               << "void main() {\n"
                  "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
                  "    gl_Position = vec4(p * 2.0 - 1.0, kUniqueV * 0.0, 1.0);\n"
                  "}\n"
                  "#type fragment\n#version 460 core\n"
                  "layout(location = 0) out vec4 o_Color;\n"
               << "const float kUnique = float(" << (token % 1000000) << "u % 997u) / 997.0;\n"
               << "void main() { o_Color = vec4(kUnique, 0.0, 0.0, 1.0); }\n";
    }

    // Source compile: new text, both stages compiled, two modules, no cache hit.
    const ShaderPreparationStats before = ShaderPreparationCounters::Get().Snapshot();
    auto first = Ref<VulkanShader>::Create(sourcePath.generic_string());
    ASSERT_TRUE(first->IsReady());
    const ShaderPreparationStats compiled = ShaderPreparationCounters::Get().Snapshot();
    EXPECT_EQ(compiled.SourceCompiles - before.SourceCompiles, 2u);
    EXPECT_EQ(compiled.SpirvCacheHits - before.SpirvCacheHits, 0u);
    EXPECT_EQ(compiled.ModuleCreates - before.ModuleCreates, 2u);

    // The same file again: both stages come from the SPIR-V cache, nothing is
    // compiled, and the modules are still created (a module is per shader).
    auto second = Ref<VulkanShader>::Create(sourcePath.generic_string());
    ASSERT_TRUE(second->IsReady());
    const ShaderPreparationStats cached = ShaderPreparationCounters::Get().Snapshot();
    EXPECT_EQ(cached.SourceCompiles - compiled.SourceCompiles, 0u);
    EXPECT_EQ(cached.SpirvCacheHits - compiled.SpirvCacheHits, 2u);
    EXPECT_EQ(cached.ModuleCreates - compiled.ModuleCreates, 2u);

    // Pipeline creation is counted by the builder, not as shader preparation.
    const auto targets = ColourTargets(VK_FORMAT_R8G8B8A8_UNORM);
    ASSERT_TRUE(builder.RequestGraphics(*first, targets, 0).has_value());
    ASSERT_EQ(VulkanPipelinePrecache::Get().PrepareQueued(8), 1u);
    const auto created = builder.GetRequestStats().Counters;
    EXPECT_EQ(created.PreparedCreations, 1u);
    EXPECT_EQ(ShaderPreparationCounters::Get().Snapshot().ModuleCreates, cached.ModuleCreates)
        << "Creating a pipeline creates no shader module";

    // Driver-cache hit: the same pipeline created again after invalidation is
    // served from the VkPipelineCache the first creation filled. Creation
    // feedback is the only signal that can tell; where a driver does not
    // report it, the creation is counted as unknown instead of guessed.
    (void)builder.InvalidateShader(first->GetPipelineIndexKey());
    ASSERT_TRUE(builder.RequestGraphics(*first, targets, 0).has_value());
    ASSERT_EQ(VulkanPipelinePrecache::Get().PrepareQueued(8), 1u);
    const auto recreated = builder.GetRequestStats().Counters;
    EXPECT_EQ(recreated.DriverCacheHits + recreated.DriverCacheMisses + recreated.DriverCacheUnknown, 2u);
    if (created.DriverCacheMisses == 1u && recreated.DriverCacheUnknown == created.DriverCacheUnknown)
    {
        EXPECT_EQ(recreated.DriverCacheHits - created.DriverCacheHits, 1u)
            << "The second creation of an identical pipeline must be a pipeline-cache hit";
    }

    second.Reset();
    first.Reset();
}

TEST_F(VulkanShaderPipeline, TwoConfigurationsDrawnEveryFrameAreBothSelectedAndClaimed)
{
    // A viewport and a preview with different settings render in the same
    // frame: both tags are published every frame. The selection settles on
    // both, both sets of keys are requested at the front, and readiness is
    // claimed instead of being withdrawn forever.
    ScopedVulkanApiSelection vulkanSelected;
    auto& builder = VulkanPipelineBuilder::Get();
    builder.ResetRequestCounters();

    constexpr std::string_view kViewport = "Deferred/msaa1/native/oit0";
    constexpr std::string_view kPreview = "Forward/msaa0/native/oit0";
    const u32 stages = static_cast<u32>(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    PipelineManifest manifest;
    PipelineSignature rgba8;
    rgba8.ColorCount = 1;
    rgba8.ColorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    PipelineSignature rgba16;
    rgba16.ColorCount = 1;
    rgba16.ColorFormats[0] = VK_FORMAT_R16G16B16A16_SFLOAT;
    ASSERT_TRUE(manifest.Record("PsoMixed", stages, rgba8, kViewport));
    ASSERT_TRUE(manifest.Record("PsoMixed", stages, rgba16, kPreview));

    ScopedPrecache precache(kViewport);
    VulkanPipelinePrecache::Get().SetManifestForTesting(manifest);
    Ref<Shader> shader = MakeConstantShader("PsoMixed");
    for (int frame = 0; frame < 4; ++frame)
    {
        VulkanPipelinePrecache::Get().SetConfigurationTag(kViewport);
        VulkanPipelinePrecache::Get().SetConfigurationTag(kPreview);
        VulkanPipelinePrecache::Get().Tick();
    }
    ASSERT_EQ(VulkanPipelinePrecache::Get().PrepareQueued(8), 2u);
    VulkanPipelinePrecache::Get().SetConfigurationTag(kViewport);
    VulkanPipelinePrecache::Get().SetConfigurationTag(kPreview);

    PipelineReadinessSnapshot snapshot;
    VulkanPipelinePrecache::Get().FillSnapshot(snapshot);
    EXPECT_TRUE(snapshot.RenderReady);
    EXPECT_NE(snapshot.ConfigurationTag.ToView().find(kViewport), std::string_view::npos);
    EXPECT_NE(snapshot.ConfigurationTag.ToView().find(kPreview), std::string_view::npos);
    EXPECT_TRUE(snapshot.PendingShaders.IsEmpty());
}

#endif // OLO_WITH_VULKAN
