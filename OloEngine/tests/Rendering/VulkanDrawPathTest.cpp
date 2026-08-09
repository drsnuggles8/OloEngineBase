// OLO_TEST_LAYER: plumbing
// =============================================================================
// VulkanDrawPathTest — #691 Phase 7 Stage 1.6a: the facade draw path.
//
// The first draw that travels the REAL pass shape end-to-end on Vulkan with
// no pilot scaffolding: Framebuffer::Bind() publishes the target,
// Shader::Bind() selects the program, UniformBuffer::Bind() occupies its
// binding point, BindTexture stages a heap slot through the descriptor slot
// cache, and DrawIndexed assembles the root struct from VulkanBindingState,
// fetches a thin PSO, opens the lazy dynamic-rendering scope (Clear folded
// into loadOp), pushes the 8-byte root pointer and draws — exactly the
// sequence an unmodified GL-shaped pass body records.
//
// Also pins the upload→sample seam: the sampled texture's layout was seeded
// by the load-time one-shot (VulkanImageInfo::InitialLayout), so NO barrier
// is needed before sampling it — if the seeding broke, the descriptor's
// baked SHADER_READ_ONLY would disagree with the tracker and validation
// (asserted zero) would name it.
//
// Device-gated; SKIPs cleanly headless (the VulkanShaderPipelineTest ladder).
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanDrawPath, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <volk.h>

#include <algorithm>
#include <cstring>
#include <memory>
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

    // The minimal §5f-shaped pair: vertex pulling from the reserved binding
    // 57, a UBO tint at binding 3, a sampled texture at binding 0. The
    // fullscreen triangle covers every pixel, so the whole target must read
    // tint x texel.
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
} // namespace

class VulkanDrawPath : public ::testing::Test
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
        if (!m_Device)
            return;
        vkDeviceWaitIdle(m_Device->GetDevice());
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

    std::unique_ptr<VulkanDevice> m_Device;
    VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
    VkFence m_Fence = VK_NULL_HANDLE;
};

TEST_F(VulkanDrawPath, FacadeDrawRendersTintedTextureThroughRootData)
{
    ScopedVulkanApiSelection vulkanApi;
    VulkanFrameArena::Get().BeginFrame(0);

    // --- resources through the ordinary engine factories -------------------
    FramebufferSpecification fbSpec;
    fbSpec.Width = 64;
    fbSpec.Height = 64;
    fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    auto framebuffer = Framebuffer::Create(fbSpec);
    ASSERT_NE(framebuffer, nullptr);

    // Fullscreen triangle, position-only (2 floats per vertex).
    const f32 vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    auto vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
    u32 indices[] = { 0, 1, 2 };
    auto indexBuffer = IndexBuffer::Create(indices, 3);
    auto vertexArray = VertexArray::Create();
    vertexArray->AddVertexBuffer(vertexBuffer);
    vertexArray->SetIndexBuffer(indexBuffer);

    // 4x4 solid white sampled input — uploaded via the one-shot, which seeds
    // the layout tracker with SHADER_READ_ONLY.
    TextureSpecification texSpec;
    texSpec.Width = 4;
    texSpec.Height = 4;
    texSpec.Format = ImageFormat::RGBA8;
    texSpec.GenerateMips = false;
    auto texture = Texture2D::Create(texSpec);
    ASSERT_NE(texture, nullptr);
    std::vector<u8> white(4 * 4 * 4, 0xFF);
    texture->SetData(white.data(), static_cast<u32>(white.size()));

    auto tintUbo = UniformBuffer::Create(16, 3);
    const f32 green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    tintUbo->SetData(green, sizeof(green));

    auto shader = Ref<VulkanShader>::Create("DrawPathTriangle", kVertexSrc, kFragmentSrc);
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    VulkanRendererAPI api;
    const auto colorHandle = framebuffer->GetColorAttachmentHandle(0);
    ASSERT_TRUE(colorHandle.IsValid());

    SubmitFrame(api,
                [&]()
                {
                    // The graph's job, done by hand here: attachment →
                    // COLOR_ATTACHMENT before the pass writes it.
                    RHI::Barrier toColor{};
                    toColor.Resource = colorHandle;
                    toColor.Before = RHI::Access::Undefined;
                    toColor.After = RHI::Access::ColorAttachmentWrite;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toColor, 1 });

                    // The unmodified GL-shaped pass body.
                    framebuffer->Bind();
                    api.SetViewport(0, 0, 64, 64);
                    api.SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
                    api.Clear();
                    shader->Bind();
                    tintUbo->Bind();
                    api.BindTexture(0, texture->GetRHIHandle());
                    api.DrawIndexed(vertexArray, 3);
                    framebuffer->Unbind();

                    // Attachment → sampled, so the steady-state readback
                    // below transitions from the layout the tracker knows.
                    RHI::Barrier toSampled{};
                    toSampled.Resource = colorHandle;
                    toSampled.Before = RHI::Access::ColorAttachmentWrite;
                    toSampled.After = RHI::Access::ShaderSampleRead;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                });

    EXPECT_EQ(api.GetPhase6StubHitCount(), 0u) << "the draw path must not fall through to a stub";

    // Readback: the attachment is a VulkanTexture2D sitting in
    // SHADER_READ_ONLY — GetData's steady-state contract.
    auto* vkFramebuffer = static_cast<VulkanFramebuffer*>(framebuffer.Raw());
    const auto attachment = vkFramebuffer->GetColorAttachmentImage(0);
    ASSERT_NE(attachment, nullptr);
    std::vector<u8> pixels;
    ASSERT_TRUE(attachment->GetData(pixels, 0));
    ASSERT_EQ(pixels.size(), sizet{ 64 * 64 * 4 });

    // Fullscreen triangle x white texel x green tint: every pixel pure green.
    u32 wrongPixels = 0;
    for (sizet i = 0; i < pixels.size(); i += 4)
    {
        const bool green = pixels[i + 0] == 0x00 && pixels[i + 1] == 0xFF && pixels[i + 2] == 0x00 &&
                           pixels[i + 3] == 0xFF;
        wrongPixels += green ? 0u : 1u;
    }
    EXPECT_EQ(wrongPixels, 0u) << "expected the whole 64x64 target tinted green";
}

#endif // OLO_WITH_VULKAN
