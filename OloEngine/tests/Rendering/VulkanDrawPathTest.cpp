// OLO_TEST_LAYER: plumbing
// =============================================================================
// VulkanDrawPathTest — #691.6a: the facade draw path.
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

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanDescriptorHeapBackend.h"
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
#include <array>
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

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u) << "the draw path must not fall through to a stub";

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

// =============================================================================
// The ENGINE heap (RHI::DescriptorHeap) running on Vulkan — the amendment
// (56) deferral, proven end-to-end: engine-managed slots are shader-reachable
// through the same heap buffer the draw path binds, memoisation and stale
// rejection behave as on GL, and poison-on-free is DETERMINISTIC (a stale
// offset reads the extension's null descriptor — zeros — never the dead
// image's descriptor and never a hang).
// =============================================================================
TEST_F(VulkanDrawPath, EngineHeapServesShaderReachableSlotsAndPoisonsFreedOnes)
{
    ScopedVulkanApiSelection vulkanApi;
    VulkanFrameArena::Get().BeginFrame(0);

    // Amendment (34) discipline: this test displaces the process-wide engine
    // heap; put back exactly what it found.
    auto& engineHeap = RHI::DescriptorHeap::Get();
    RHI::IDescriptorHeapBackend* priorBackend = engineHeap.GetBackend();
    const RHI::HeapDesc priorDesc = engineHeap.GetDesc();
    const bool hadPrior = priorBackend != nullptr;
    struct RestoreHeap
    {
        bool Had;
        RHI::HeapDesc Desc;
        RHI::IDescriptorHeapBackend* Backend;
        ~RestoreHeap()
        {
            if (Had)
                RHI::DescriptorHeap::Get().Initialize(Desc, Backend);
            else
                RHI::DescriptorHeap::Get().Shutdown();
        }
    } restore{ hadPrior, priorDesc, priorBackend };

    ASSERT_TRUE(VulkanDescriptorHeapBackend::InstallOntoEngineHeap());
    engineHeap.SetEnabled(true);
    ASSERT_TRUE(engineHeap.IsEnabled());

    // --- scene resources (same shape as the facade draw test) --------------
    FramebufferSpecification fbSpec;
    fbSpec.Width = 32;
    fbSpec.Height = 32;
    fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    auto framebuffer = Framebuffer::Create(fbSpec);
    ASSERT_NE(framebuffer, nullptr);

    const f32 vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    auto vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
    u32 indices[] = { 0, 1, 2 };
    auto indexBuffer = IndexBuffer::Create(indices, 3);
    auto vertexArray = VertexArray::Create();
    vertexArray->AddVertexBuffer(vertexBuffer);
    vertexArray->SetIndexBuffer(indexBuffer);

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

    auto shader = Ref<VulkanShader>::Create("EngineHeapTriangle", kVertexSrc, kFragmentSrc);
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    // --- engine-heap view: memoised, allocatable-range slot ----------------
    RHI::ViewDesc viewDesc;
    viewDesc.Resource = texture->GetRHIHandle();
    const RHI::ViewHandle view = engineHeap.GetOrCreateView(texture->GetRHIHandle(), viewDesc, RHI::SamplerDesc{},
                                                            RHI::HeapSlotLifetime::Persistent);
    ASSERT_TRUE(view.IsValid());
    EXPECT_EQ(engineHeap
                  .GetOrCreateView(texture->GetRHIHandle(), viewDesc, RHI::SamplerDesc{},
                                   RHI::HeapSlotLifetime::Persistent)
                  .Index,
              view.Index)
        << "persistent views must memoise";

    const RHI::HeapOffset offset = engineHeap.OffsetOf(view);
    ASSERT_GE(offset.Value, RHI::kFirstAllocatableHeapSlot);
    engineHeap.Flush(); // redeem staged tokens into real descriptor writes

    VulkanRendererAPI api;
    const auto colorHandle = framebuffer->GetColorAttachmentHandle(0);

    const auto drawWithSlot = [&](u32 heapSlot, RHI::Access from)
    {
        SubmitFrame(api,
                    [&]()
                    {
                        RHI::Barrier toColor{};
                        toColor.Resource = colorHandle;
                        toColor.Before = from;
                        toColor.After = RHI::Access::ColorAttachmentWrite;
                        api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toColor, 1 });

                        framebuffer->Bind();
                        api.SetViewport(0, 0, 32, 32);
                        api.SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
                        api.Clear();
                        shader->Bind();
                        tintUbo->Bind();
                        // Feed the ENGINE heap's slot index straight into the
                        // root-data path — the whole point: engine-managed
                        // slots live in the same heap the draw binds.
                        VulkanBindingState::Get().SetTextureHeapSlot(0, heapSlot);
                        api.DrawIndexed(vertexArray, 3);
                        framebuffer->Unbind();

                        RHI::Barrier toSampled{};
                        toSampled.Resource = colorHandle;
                        toSampled.Before = RHI::Access::ColorAttachmentWrite;
                        toSampled.After = RHI::Access::ShaderSampleRead;
                        api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                    });
    };

    // Live view: white texel x green tint = green everywhere.
    drawWithSlot(offset.Value, RHI::Access::Undefined);
    auto* vkFramebuffer = static_cast<VulkanFramebuffer*>(framebuffer.Raw());
    std::vector<u8> pixels;
    ASSERT_TRUE(vkFramebuffer->GetColorAttachmentImage(0)->GetData(pixels, 0));
    EXPECT_EQ(pixels[1], 0xFFu) << "live engine-heap slot must sample the white texture (green output)";
    EXPECT_EQ(pixels[0], 0x00u);

    // Destroy the view: OffsetOf rejects, and the freed slot reads DETERMINISTIC
    // zeros (null descriptor) — tint x zero = black, never the old texture and
    // never undefined behaviour.
    engineHeap.DestroyView(view);
    EXPECT_FALSE(engineHeap.OffsetOf(view).IsValid()) << "a destroyed view's offset must reject";
    engineHeap.Flush(); // publish the poison write

    drawWithSlot(offset.Value, RHI::Access::ShaderSampleRead);
    std::vector<u8> poisoned;
    ASSERT_TRUE(vkFramebuffer->GetColorAttachmentImage(0)->GetData(poisoned, 0));
    EXPECT_EQ(poisoned[1], 0x00u) << "a freed slot must sample zeros (poison), not the dead texture";
    EXPECT_EQ(poisoned[3], 0x00u) << "null-descriptor alpha reads zero too";

    // Frame-transient ring: always mints, and the reset retires wholesale.
    const RHI::ViewHandle transient = engineHeap.GetOrCreateView(
        texture->GetRHIHandle(), viewDesc, RHI::SamplerDesc{}, RHI::HeapSlotLifetime::FrameTransient);
    ASSERT_TRUE(transient.IsValid());
    engineHeap.ResetFrameTransients();
    EXPECT_FALSE(engineHeap.OffsetOf(transient).IsValid()) << "the ring reset must retire transient views";

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u);
}

// =============================================================================
// The compute SPIR-V route (#691.3, the amendment (56)
// deferral): a .comp source compiles through shaderc(vulkan_1_4), reflects
// into the shared binding vocabulary, gets a compute PSO with the SAME
// mapping chain as graphics, and a facade DispatchCompute assembles root data
// (UBO address + storage-image heap slot via the image-UNIT namespace) and
// writes a deterministic pattern the readback verifies.
// =============================================================================
TEST_F(VulkanDrawPath, ComputeDispatchWritesStorageImageThroughRootData)
{
    ScopedVulkanApiSelection vulkanApi;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr const char* kComputeSrc = R"(
#version 460 core
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0, rgba8) writeonly uniform image2D u_Output;
layout(std140, binding = 3) uniform TintBlock
{
    vec4 u_Tint;
};

void main()
{
    imageStore(u_Output, ivec2(gl_GlobalInvocationID.xy), u_Tint);
}
)";

    TextureSpecification spec;
    spec.Width = 16;
    spec.Height = 16;
    spec.Format = ImageFormat::RGBA8;
    spec.GenerateMips = false;
    spec.SRGB = false; // sRGB drops STORAGE usage (the format rule)
    auto target = Texture2D::Create(spec);
    ASSERT_NE(target, nullptr);

    auto tintUbo = UniformBuffer::Create(16, 3);
    const f32 magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
    tintUbo->SetData(magenta, sizeof(magenta));

    auto compute = ComputeShader::CreateFromSource("DrawPathComputeTint", kComputeSrc);
    ASSERT_NE(compute, nullptr);
    ASSERT_TRUE(compute->IsValid()) << "the compute SPIR-V route must compile a plain .comp source";

    VulkanRendererAPI api;
    SubmitFrame(api,
                [&]()
                {
                    // The graph's job by hand: the storage target must sit in
                    // GENERAL before the dispatch writes it.
                    RHI::Barrier toStorage{};
                    toStorage.Resource = target->GetRHIHandle();
                    toStorage.Before = RHI::Access::Undefined;
                    toStorage.After = RHI::Access::StorageWrite;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toStorage, 1 });

                    compute->Bind();
                    tintUbo->Bind();
                    api.BindImageTexture(0, target->GetRHIHandle(), 0, false, 0, RHI::Access::StorageWrite,
                                         RHI::Format::RGBA8UNorm);
                    api.DispatchCompute(2, 2, 1); // 2x2 groups x 8x8 = 16x16

                    // Storage write -> sampled, so GetData's steady-state
                    // readback transitions from the tracked layout.
                    RHI::Barrier toSampled{};
                    toSampled.Resource = target->GetRHIHandle();
                    toSampled.Before = RHI::Access::StorageWrite;
                    toSampled.After = RHI::Access::ShaderSampleRead;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                });

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u) << "the dispatch path must not fall through to a stub";

    std::vector<u8> pixels;
    ASSERT_TRUE(target->GetData(pixels, 0));
    ASSERT_EQ(pixels.size(), sizet{ 16 * 16 * 4 });
    u32 wrongPixels = 0;
    for (sizet i = 0; i < pixels.size(); i += 4)
    {
        const bool magentaPixel = pixels[i + 0] == 0xFF && pixels[i + 1] == 0x00 && pixels[i + 2] == 0xFF &&
                                  pixels[i + 3] == 0xFF;
        wrongPixels += magentaPixel ? 0u : 1u;
    }
    EXPECT_EQ(wrongPixels, 0u) << "every texel must carry the UBO tint the dispatch stored";
}

// =============================================================================
// The GL global-overwrite rule for per-attachment state (issue #823).
//
// glColorMask is DEFINED as glColorMaski for every draw buffer, so it clears
// any divergence an earlier indexed call installed — and
// CommandDispatch::ApplyRenderState leans on exactly that: it only ever
// DISABLES attachments named by PODRenderState::colorAttachmentWriteMask and
// never re-enables one, because the global call preceding it is supposed to
// have. The Vulkan arm kept its own per-attachment array that the global
// setter did not touch, so a narrowing indexed call was PERMANENT for the rest
// of the process: one Renderer3D::DrawLine (mask 0x01) left every G-Buffer
// attachment above 0 dead, the deferred emissive target kept its cleared
// alpha=1 "unlit" flag, and every deferred frame came back a light-independent
// flat grey.
//
// Both directions are asserted from ONE recording, because either alone
// passes on a broken build: phase A proves the indexed mask really acts (a
// no-op mask would satisfy phase B trivially), phase B proves the global call
// takes it back.
// =============================================================================
TEST_F(VulkanDrawPath, GlobalColorMaskResetsPerAttachmentDivergence)
{
    ScopedVulkanApiSelection vulkanApi;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr u32 kSize = 32;
    constexpr u32 kAttachments = 3;

    // The G-Buffer's shape in miniature: more than one colour attachment, all
    // written by one fragment stage.
    constexpr const char* kMrtFragmentSrc = R"(
#version 460 core
layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Rt0;
layout(location = 1) out vec4 o_Rt1;
layout(location = 2) out vec4 o_Rt2;

layout(std140, binding = 3) uniform TintBlock
{
    vec4 u_Tint;
};

void main()
{
    o_Rt0 = u_Tint;
    o_Rt1 = u_Tint;
    o_Rt2 = u_Tint;
}
)";

    FramebufferSpecification fbSpec;
    fbSpec.Width = kSize;
    fbSpec.Height = kSize;
    fbSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RGBA8,
                           FramebufferTextureFormat::RGBA8 };
    // Three targets, ONE recording — the leak was process-scoped, not
    // scope-scoped, so the state has to be observed crossing framebuffer binds
    // rather than reset by a fresh frame between each case.
    auto masked = Framebuffer::Create(fbSpec);
    ASSERT_NE(masked, nullptr);
    auto restored = Framebuffer::Create(fbSpec);
    ASSERT_NE(restored, nullptr);
    // The other ordering: a global NARROW followed by an indexed WIDEN.
    // glColorMaski wins over the preceding glColorMask, so the lowering must
    // read the array alone — ANDing the recorded global mask on top would make
    // this attachment stay masked.
    auto widened = Framebuffer::Create(fbSpec);
    ASSERT_NE(widened, nullptr);

    const f32 vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    auto vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
    u32 indices[] = { 0, 1, 2 };
    auto indexBuffer = IndexBuffer::Create(indices, 3);
    auto vertexArray = VertexArray::Create();
    vertexArray->AddVertexBuffer(vertexBuffer);
    vertexArray->SetIndexBuffer(indexBuffer);

    auto tintUbo = UniformBuffer::Create(16, 3);
    auto shader = Ref<VulkanShader>::Create("DrawPathMrtMask", kVertexSrc, kMrtFragmentSrc);
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    VulkanRendererAPI api;

    // Clear red, first draw green, second draw blue: reading back which of the
    // three a texel carries says exactly which draw reached that attachment.
    constexpr f32 kGreen[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    constexpr f32 kBlue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

    const auto transitionAll = [&api](const Ref<Framebuffer>& target, const RHI::Access before,
                                      const RHI::Access after)
    {
        for (u32 i = 0; i < kAttachments; ++i)
        {
            RHI::Barrier barrier{};
            barrier.Resource = target->GetColorAttachmentHandle(i);
            barrier.Before = before;
            barrier.After = after;
            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &barrier, 1 });
        }
    };

    SubmitFrame(api,
                [&]()
                {
                    transitionAll(masked, RHI::Access::Undefined, RHI::Access::ColorAttachmentWrite);
                    transitionAll(restored, RHI::Access::Undefined, RHI::Access::ColorAttachmentWrite);
                    transitionAll(widened, RHI::Access::Undefined, RHI::Access::ColorAttachmentWrite);

                    shader->Bind();
                    tintUbo->Bind();

                    // The narrowing indexed call, in the shape
                    // CommandDispatch::ApplyRenderState emits for a command
                    // whose colorAttachmentWriteMask excludes an attachment.
                    masked->Bind();
                    api.SetViewport(0, 0, kSize, kSize);
                    api.SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
                    api.Clear();
                    api.SetColorMaskForAttachment(1, false, false, false, false);
                    tintUbo->SetData(kGreen, sizeof(kGreen));
                    api.DrawIndexed(vertexArray, 3);
                    masked->Unbind();

                    // The global call that must take the narrowing back.
                    restored->Bind();
                    api.SetViewport(0, 0, kSize, kSize);
                    api.SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
                    api.Clear();
                    api.SetColorMask(true, true, true, true);
                    tintUbo->SetData(kBlue, sizeof(kBlue));
                    api.DrawIndexed(vertexArray, 3);
                    restored->Unbind();

                    // Global mask off for every channel, then attachment 1
                    // widened back on its own.
                    widened->Bind();
                    api.SetViewport(0, 0, kSize, kSize);
                    api.SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
                    api.Clear();
                    api.SetColorMask(false, false, false, false);
                    api.SetColorMaskForAttachment(1, true, true, true, true);
                    tintUbo->SetData(kGreen, sizeof(kGreen));
                    api.DrawIndexed(vertexArray, 3);
                    api.SetColorMask(true, true, true, true);
                    widened->Unbind();

                    transitionAll(masked, RHI::Access::ColorAttachmentWrite, RHI::Access::ShaderSampleRead);
                    transitionAll(restored, RHI::Access::ColorAttachmentWrite, RHI::Access::ShaderSampleRead);
                    transitionAll(widened, RHI::Access::ColorAttachmentWrite, RHI::Access::ShaderSampleRead);
                });

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u) << "the MRT draw path must not fall through to a stub";

    // `target` is deliberately NOT const: Ref<T>::Raw() const-propagates, so a
    // const Ref hands back a const Framebuffer* and the downcast to the
    // backend type would have to cast the qualifier away (clang-cl rejects it;
    // MSVC let it through).
    const auto centreTexel = [](Ref<Framebuffer>& target, u32 attachmentIndex,
                                std::array<u8, 4>& out) -> bool
    {
        auto* vkFramebuffer = static_cast<VulkanFramebuffer*>(target.Raw());
        const auto attachment = vkFramebuffer->GetColorAttachmentImage(attachmentIndex);
        if (attachment == nullptr)
            return false;
        std::vector<u8> pixels;
        if (!attachment->GetData(pixels, 0) || pixels.size() != sizet{ kSize } * kSize * 4)
            return false;
        const sizet offset = ((sizet{ kSize } / 2) * kSize + kSize / 2) * 4;
        out = { pixels[offset + 0], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3] };
        return true;
    };

    constexpr std::array<u8, 4> kGreenTexel{ 0x00, 0xFF, 0x00, 0xFF };
    constexpr std::array<u8, 4> kBlueTexel{ 0x00, 0x00, 0xFF, 0xFF };
    constexpr std::array<u8, 4> kRedTexel{ 0xFF, 0x00, 0x00, 0xFF };

    // The control: the indexed mask has to have DONE something, or the
    // assertion below proves nothing — a no-op mask passes it trivially.
    std::array<u8, 4> texel{};
    ASSERT_TRUE(centreTexel(masked, 0, texel));
    EXPECT_EQ(texel, kGreenTexel) << "attachment 0 was never masked and must carry the first draw";
    ASSERT_TRUE(centreTexel(masked, 1, texel));
    EXPECT_EQ(texel, kRedTexel) << "SetColorMaskForAttachment(1, false x4) did not mask attachment 1";
    ASSERT_TRUE(centreTexel(masked, 2, texel));
    EXPECT_EQ(texel, kGreenTexel) << "attachment 2 was never masked and must carry the first draw";

    // The contract this test exists for: a global colour mask clears the
    // per-attachment divergence an earlier indexed call installed (issue #823).
    for (u32 i = 0; i < kAttachments; ++i)
    {
        ASSERT_TRUE(centreTexel(restored, i, texel)) << "attachment " << i << " readback failed";
        EXPECT_EQ(texel, kBlueTexel)
            << "attachment " << i
            << " missed the draw after SetColorMask(true x4) — glColorMask is the indexed call for EVERY draw "
               "buffer, so it must clear a narrowing SetColorMaskForAttachment (issue #823)";
    }

    // The indexed setter wins over the global one it follows, both ways round.
    ASSERT_TRUE(centreTexel(widened, 0, texel));
    EXPECT_EQ(texel, kRedTexel) << "attachment 0 stayed under the global SetColorMask(false x4)";
    ASSERT_TRUE(centreTexel(widened, 1, texel));
    EXPECT_EQ(texel, kGreenTexel)
        << "SetColorMaskForAttachment(1, true x4) after a global SetColorMask(false x4) did not reach "
           "attachment 1 — glColorMaski overrides the glColorMask before it, so the per-attachment array "
           "is the lowering's only input";
    ASSERT_TRUE(centreTexel(widened, 2, texel));
    EXPECT_EQ(texel, kRedTexel) << "attachment 2 stayed under the global SetColorMask(false x4)";

    // Leave the global mask wide, so a later test in this binary is not handed
    // a narrowed one — the very leak this test pins.
    api.SetColorMask(true, true, true, true);
}

#endif // OLO_WITH_VULKAN
