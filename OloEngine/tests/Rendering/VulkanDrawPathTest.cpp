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
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
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

#include "VulkanTestSupport.h"

#include <volk.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace
{
    using namespace OloEngine;
    using OloEngine::Tests::ProbeVulkanDeviceTestGate;
    using OloEngine::Tests::ScopedVulkanRenderCommandSelection;

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

    class ScopedOloEditorWorkingDirectory
    {
      public:
        ScopedOloEditorWorkingDirectory()
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            m_Original = fs::current_path(ec);
            if (ec)
                return;

            fs::path candidate = m_Original;
            for (int i = 0; i < 6; ++i)
            {
                const fs::path editorDir = candidate / "OloEditor";
                if (fs::exists(editorDir / "assets" / "shaders", ec) && !ec)
                {
                    fs::current_path(editorDir, ec);
                    m_Valid = !ec;
                    return;
                }
                ec.clear();
                if (!candidate.has_parent_path() || candidate.parent_path() == candidate)
                    break;
                candidate = candidate.parent_path();
            }

            m_Valid = fs::exists(fs::path("assets") / "shaders", ec) && !ec;
        }

        ~ScopedOloEditorWorkingDirectory()
        {
            if (!m_Original.empty())
            {
                std::error_code ec;
                std::filesystem::current_path(m_Original, ec);
            }
        }

        [[nodiscard]] bool IsValid() const
        {
            return m_Valid;
        }

      private:
        std::filesystem::path m_Original;
        bool m_Valid = false;
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
        const auto gate = ProbeVulkanDeviceTestGate();
        if (!gate.Available)
            GTEST_SKIP() << gate.Reason;

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
// ADR 0011 §4.2: the production frustum-cull pass writes the root data for its
// compatible indirect draw. The exact shader produces the compacted instances,
// DrawElementsIndirect args, and the root address; the graphics draw consumes
// all three with no CPU readback; the GPU-owned address is preserved while
// command-ordered static fields complete the reflected struct.
// =============================================================================
TEST_F(VulkanDrawPath, ProductionFrustumCullWritesRootDataForIndirectDraw)
{
    // ONE BACKEND PER PROCESS — and this binary breaks that rule.
    //
    // This tenant drives the production CommandDispatch::DrawMeshInstanced,
    // which binds Renderer3D's shared UBOs. Those are process statics created
    // by the FIRST Renderer::Init in the process, under whichever backend ran
    // it — and in this binary that is always OpenGL, because
    // RendererAttachedTest (the only fixture that calls Renderer::Init) creates
    // a GL 4.6 context and nothing initialises Renderer3D on Vulkan.
    //
    // So a GL renderer test registered ahead of this one leaves
    // CommandDispatch binding GL-owned UBO handles here. They cannot resolve in
    // VulkanRootObjectRegistry, BindUniformBuffer counts two
    // PreconditionFailure stubs, and the zero-stub assertion below fails — for
    // a reason that has nothing to do with the draw path it is testing. A
    // shipped app never reaches this: --rhi picks one backend before
    // Renderer::Init and never switches.
    //
    // Skipping is honest rather than lossy: the assertion keeps its full
    // strength whenever the test DOES run (any filtered Vulkan run, which is
    // how this ladder is normally exercised), and the skip reason names the
    // precondition instead of leaving a confusing red in the full suite. The
    // underlying "two backends share Renderer3D's statics in one test process"
    // is tracked separately.
    if (Renderer3D::HasInitialized())
    {
        GTEST_SKIP() << "Renderer3D was initialised earlier in this process (an OpenGL renderer test); "
                        "CommandDispatch would bind that backend's UBOs, which cannot resolve on Vulkan. "
                        "Run this tenant in a Vulkan-filtered invocation.";
    }

    ScopedOloEditorWorkingDirectory editorWorkingDirectory;
    ASSERT_TRUE(editorWorkingDirectory.IsValid())
        << "Could not locate OloEditor/assets/shaders from " << std::filesystem::current_path().string();
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr const char* kVertexSrc = R"(
#version 460 core
void main()
{
    const vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
)";
    constexpr const char* kFragmentSrc = R"(
#version 460 core
struct InstanceData {
    mat4 Transform;
    mat4 Normal;
    mat4 PrevTransform;
    vec4 Color;
    int EntityID;
    float Custom;
    uvec2 StableID;
    vec4 LightmapScaleOffset;
};
layout(std430, binding = 15) readonly buffer InstanceCullOutput {
    InstanceData outputInstances[];
};
layout(std140, binding = 3) uniform TintBlock {
    vec4 u_Tint;
};
layout(location = 0) out vec4 o_Color;
void main()
{
    o_Color = outputInstances[0].Color * u_Tint;
}
)";

    FramebufferSpecification fbSpec;
    fbSpec.Width = 32;
    fbSpec.Height = 32;
    fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    auto framebuffer = Framebuffer::Create(fbSpec);
    ASSERT_NE(framebuffer, nullptr);
    const std::array<f32, 6> positions = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    std::array<u32, 3> indices = { 0u, 1u, 2u };
    auto vertexBuffer = VertexBuffer::Create(positions.data(), static_cast<u32>(sizeof(positions)));
    auto indexBuffer = IndexBuffer::Create(indices.data(), static_cast<u32>(indices.size()));
    auto vertexArray = VertexArray::Create();
    ASSERT_TRUE(vertexBuffer && indexBuffer && vertexArray);
    vertexArray->AddVertexBuffer(vertexBuffer);
    vertexArray->SetIndexBuffer(indexBuffer);

    auto stats = StorageBuffer::Create(144u, ShaderBindingLayout::SSBO_GPU_STATS, StorageBufferUsage::DynamicCopy);
    auto camera = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(), 0u);
    auto tint = UniformBuffer::Create(16u, 3u);
    ASSERT_TRUE(stats && camera && tint);

    InstanceData visible;
    visible.Color = { 0.0f, 1.0f, 0.0f, 1.0f };
    const std::array<u32, 36> disabledStats{};
    stats->SetData(disabledStats.data(), static_cast<u32>(sizeof(disabledStats)));
    UBOStructures::CameraUBO cameraData{};
    cameraData.ViewProjection = glm::mat4(1.0f);
    cameraData.View = glm::mat4(1.0f);
    cameraData.Projection = glm::mat4(1.0f);
    camera->SetData(&cameraData, UBOStructures::CameraUBO::GetSize());
    const glm::vec4 whiteTint{ 1.0f };
    tint->SetData(&whiteTint, static_cast<u32>(sizeof(whiteTint)));

    auto drawShader = Ref<VulkanShader>::Create("GpuWrittenRootConsumer", kVertexSrc, kFragmentSrc);
    ASSERT_EQ(drawShader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    auto& api = renderCommandSelection.Get();
    auto culler = Ref<GPUFrustumCuller>::Create();
    culler->BeginFrame();
    GpuDrivenRootDataLayout rootLayout{};
    ASSERT_TRUE(api.QueryGpuDrivenRootDataLayout(
        drawShader->GetRHIHandle(), ShaderBindingLayout::SSBO_INSTANCE_DATA, rootLayout));
    ASSERT_TRUE(rootLayout.IsValid());
    ASSERT_EQ(rootLayout.SizeBytes, 16u);
    ASSERT_EQ(rootLayout.GpuWrittenFieldOffsetBytes, 8u)
        << "the tint UBO must precede the GPU-written instance address";

    const bool ownsFrameData = !FrameDataBufferManager::IsInitialized();
    if (ownsFrameData)
        FrameDataBufferManager::Init();
    struct FrameDataRestore
    {
        bool Owns;
        ~FrameDataRestore()
        {
            if (Owns)
                FrameDataBufferManager::Shutdown();
        }
    } frameDataRestore{ ownsFrameData };
    auto& frameData = FrameDataBufferManager::Get();
    frameData.Reset();

    PODMaterialData material{};
    material.shaderRendererID = drawShader->GetRHIHandle();
    material.enablePBR = false;
    const u16 materialIndex = frameData.AllocateMaterialData(material);
    PODRenderState renderState{};
    renderState.depthTestEnabled = false;
    renderState.depthWriteMask = false;
    const u16 renderStateIndex = frameData.AllocateRenderState(renderState);
    ASSERT_NE(materialIndex, INVALID_MATERIAL_DATA_INDEX);
    ASSERT_NE(renderStateIndex, INVALID_RENDER_STATE_INDEX);

    const auto colorHandle = framebuffer->GetColorAttachmentHandle(0);
    ASSERT_TRUE(colorHandle.IsValid());
    bool producedGpuRoot = false;
    SubmitFrame(api,
                [&]()
                {
                    stats->Bind();
                    camera->Bind();
                    tint->Bind();

                    const std::array<InstanceData, 1> instances{ visible };
                    const auto cullResult = culler->Cull(
                        instances, 3u, 0u, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), 1.0f, rootLayout);
                    ASSERT_TRUE(cullResult.OutputBuffer && cullResult.IndirectBuffer && cullResult.RootDataBuffer);
                    producedGpuRoot = cullResult.RootDataAddressOffsetBytes == rootLayout.GpuWrittenFieldOffsetBytes;

                    RHI::Barrier toColor{};
                    toColor.Resource = colorHandle;
                    toColor.Before = RHI::Access::Undefined;
                    toColor.After = RHI::Access::ColorAttachmentWrite;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toColor, 1 });
                    framebuffer->Bind();
                    api.SetViewport(0, 0, 32, 32);
                    api.SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
                    api.Clear();

                    DrawMeshInstancedCommand command{};
                    command.vertexArrayID = vertexArray->GetRHIHandle();
                    command.indexCount = 3u;
                    command.transformCount = 1u;
                    command.instanceCount = 1u;
                    command.materialDataIndex = materialIndex;
                    command.renderStateIndex = renderStateIndex;
                    command.cullOutputInstanceBufferID = cullResult.OutputBuffer->GetStorage()->GetRHIHandle();
                    command.cullIndirectBufferID = cullResult.IndirectBuffer->GetRHIHandle();
                    command.cullRootDataBufferID = cullResult.RootDataBuffer->GetRHIHandle();
                    command.cullRootDataAddressOffsetBytes = cullResult.RootDataAddressOffsetBytes;
                    CommandDispatch::ResetState();
                    CommandDispatch::DrawMeshInstanced(&command, api);
                    framebuffer->Unbind();

                    RHI::Barrier toSampled{};
                    toSampled.Resource = colorHandle;
                    toSampled.Before = RHI::Access::ColorAttachmentWrite;
                    toSampled.After = RHI::Access::ShaderSampleRead;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                });

    EXPECT_TRUE(producedGpuRoot);
    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u);
    EXPECT_EQ(api.GetGpuWrittenRootDrawsThisRecording(), 1u)
        << "the indirect draw must consume the compute-written root buffer, not the CPU root fallback";
    std::vector<u8> pixels;
    ASSERT_TRUE(static_cast<VulkanFramebuffer*>(framebuffer.Raw())->GetColorAttachmentImage(0)->GetData(pixels, 0));
    ASSERT_EQ(pixels.size(), sizet{ 32 * 32 * 4 });
    for (sizet i = 0; i < pixels.size(); i += 4)
    {
        EXPECT_EQ(pixels[i + 0], 0x00u);
        EXPECT_EQ(pixels[i + 1], 0xFFu);
        EXPECT_EQ(pixels[i + 2], 0x00u);
        EXPECT_EQ(pixels[i + 3], 0xFFu);
    }
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

// =============================================================================
// A per-attachment blend opinion outranks the global enable, BOTH WAYS
// (issue #896, and the half of #823 that was deliberately left open).
//
// The recorded per-attachment blend state used to be a bool that could only
// OR onto the global flag, so `SetBlendStateForAttachment(i, false)` did not
// mean what its name says: the moment anything enabled blending globally the
// call was a no-op. OITResolveRenderPass asks for exactly that on RT1 (entity
// ID, an integer target) and RT2 (view normals) and did not get it on Vulkan,
// where GL would have disabled them. It was invisible because the same pass
// also colour-masks those two attachments to zero — a broken contract with no
// symptom, waiting for the first caller who disables without also masking.
//
// It is a TRI-STATE now, so all three states need pinning, and they need
// pinning from ONE recording: the state is process-scoped, so a fresh frame
// per case would reset the very thing under test.
//
//   ForceOff  — survives a global ENABLE.                  (the #896 fix)
//   ForceOn   — survives a global DISABLE.                 (the #823 tenant:
//               DecalRenderPass's Emissive additive accumulation, which the
//               naive "match GL" fix deleted and which this must not.)
//   Inherit   — after ResetBlendStateForAttachment, follows the global again.
//
// Each phase is the next one's control. Attachments 0 and 2 are never given an
// opinion in any phase, so they read out the global flag and say whether the
// blend that phase asserts about was live at all — without them a build where
// blending never happened would pass the ForceOff assertion trivially.
//
// The probe: clear RED, draw GREEN with a One/One blend func. A texel that
// blended reads YELLOW (green + red); one that did not reads GREEN.
// =============================================================================
TEST_F(VulkanDrawPath, PerAttachmentBlendOpinionOutranksTheGlobalEnable)
{
    ScopedVulkanApiSelection vulkanApi;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr u32 kSize = 32;
    constexpr u32 kAttachments = 3;

    constexpr const char* kMrtBlendFragmentSrc = R"(
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
    auto forcedOff = Framebuffer::Create(fbSpec);
    ASSERT_NE(forcedOff, nullptr);
    auto forcedOn = Framebuffer::Create(fbSpec);
    ASSERT_NE(forcedOn, nullptr);
    auto withdrawn = Framebuffer::Create(fbSpec);
    ASSERT_NE(withdrawn, nullptr);

    const f32 vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    auto vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
    u32 indices[] = { 0, 1, 2 };
    auto indexBuffer = IndexBuffer::Create(indices, 3);
    auto vertexArray = VertexArray::Create();
    vertexArray->AddVertexBuffer(vertexBuffer);
    vertexArray->SetIndexBuffer(indexBuffer);

    auto tintUbo = UniformBuffer::Create(16, 3);
    auto shader = Ref<VulkanShader>::Create("DrawPathMrtBlend", kVertexSrc, kMrtBlendFragmentSrc);
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    VulkanRendererAPI api;

    constexpr f32 kGreen[4] = { 0.0f, 1.0f, 0.0f, 1.0f };

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

    const auto drawGreenOver = [&](Ref<Framebuffer>& target, const std::function<void()>& installBlendState)
    {
        target->Bind();
        api.SetViewport(0, 0, kSize, kSize);
        api.SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
        api.Clear();
        installBlendState();
        tintUbo->SetData(kGreen, sizeof(kGreen));
        api.DrawIndexed(vertexArray, 3);
        target->Unbind();
    };

    SubmitFrame(api,
                [&]()
                {
                    transitionAll(forcedOff, RHI::Access::Undefined, RHI::Access::ColorAttachmentWrite);
                    transitionAll(forcedOn, RHI::Access::Undefined, RHI::Access::ColorAttachmentWrite);
                    transitionAll(withdrawn, RHI::Access::Undefined, RHI::Access::ColorAttachmentWrite);

                    shader->Bind();
                    tintUbo->Bind();
                    api.SetBlendFunc(RHI::BlendFactor::One, RHI::BlendFactor::One);

                    // Phase 1 — the OITResolveRenderPass shape. One attachment
                    // disabled per-attachment, and the global enable AFTER it:
                    // the opposite order would still pass on a build where
                    // SetBlendState flattens the array, because the ForceOff
                    // would be installed after the flatten. Stating the opinion
                    // first is what makes this an assertion about SURVIVING a
                    // global enable rather than merely outranking one.
                    drawGreenOver(forcedOff,
                                  [&]()
                                  {
                                      api.SetBlendStateForAttachment(1, false);
                                      api.SetBlendState(true);
                                  });

                    // Phase 2 — the DecalRenderPass Emissive shape, with the
                    // global call AFTER the indexed one, the order
                    // ApplyPODRenderState actually produces.
                    drawGreenOver(forcedOn,
                                  [&]()
                                  {
                                      api.SetBlendStateForAttachment(1, true);
                                      api.SetBlendState(false);
                                  });

                    // Phase 3 — the withdrawal. Global is still false from
                    // phase 2, so attachment 1 must stop blending; if the
                    // reset did nothing it would still carry phase 2's
                    // ForceOn and blend.
                    drawGreenOver(withdrawn, [&]()
                                  { api.ResetBlendStateForAttachment(1); });

                    transitionAll(forcedOff, RHI::Access::ColorAttachmentWrite, RHI::Access::ShaderSampleRead);
                    transitionAll(forcedOn, RHI::Access::ColorAttachmentWrite, RHI::Access::ShaderSampleRead);
                    transitionAll(withdrawn, RHI::Access::ColorAttachmentWrite, RHI::Access::ShaderSampleRead);
                });

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u) << "the MRT blend path must not fall through to a stub";

    // `target` is deliberately NOT const — see the same note on the colour-mask
    // test above (Ref<T>::Raw() const-propagates).
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

    // green over red, One/One: R and G both saturate, alpha 1+1 clamps to 1.
    constexpr std::array<u8, 4> kBlendedTexel{ 0xFF, 0xFF, 0x00, 0xFF };
    constexpr std::array<u8, 4> kGreenTexel{ 0x00, 0xFF, 0x00, 0xFF };

    std::array<u8, 4> texel{};

    // Phase 1. Attachments 0 and 2 are the control: they carry no opinion, so
    // they show whether the global enable was live at all.
    ASSERT_TRUE(centreTexel(forcedOff, 0, texel));
    EXPECT_EQ(texel, kBlendedTexel) << "attachment 0 has no per-attachment opinion and must follow SetBlendState(true)";
    ASSERT_TRUE(centreTexel(forcedOff, 2, texel));
    EXPECT_EQ(texel, kBlendedTexel) << "attachment 2 has no per-attachment opinion and must follow SetBlendState(true)";
    ASSERT_TRUE(centreTexel(forcedOff, 1, texel));
    EXPECT_EQ(texel, kGreenTexel)
        << "SetBlendStateForAttachment(1, false) did not survive the global SetBlendState(true) — a "
           "per-attachment DISABLE has to outrank the global enable, which is what OITResolveRenderPass "
           "asks for on its entity-ID and view-normal targets (issue #896)";

    // Phase 2. The direction the naive #823 fix broke: this is what
    // DecalRenderPass's Emissive additive accumulation rides on.
    ASSERT_TRUE(centreTexel(forcedOn, 0, texel));
    EXPECT_EQ(texel, kGreenTexel) << "attachment 0 has no opinion and must follow SetBlendState(false)";
    ASSERT_TRUE(centreTexel(forcedOn, 2, texel));
    EXPECT_EQ(texel, kGreenTexel) << "attachment 2 has no opinion and must follow SetBlendState(false)";
    ASSERT_TRUE(centreTexel(forcedOn, 1, texel));
    EXPECT_EQ(texel, kBlendedTexel)
        << "SetBlendStateForAttachment(1, true) did not survive the global SetBlendState(false) that followed "
           "it — DecalRenderPass enables RT2 this way for an Emissive decal whose PODRenderState carries "
           "blendEnabled=false (issue #823)";

    // Phase 3. Withdrawing the opinion puts the attachment back on the global
    // flag — the only way out of the two states above, and the reason a pass
    // restoring its state must not just pass `false`.
    for (u32 i = 0; i < kAttachments; ++i)
    {
        ASSERT_TRUE(centreTexel(withdrawn, i, texel)) << "attachment " << i << " readback failed";
        EXPECT_EQ(texel, kGreenTexel)
            << "attachment " << i
            << " blended after ResetBlendStateForAttachment(1) with the global enable off — the withdrawal "
               "must put attachment 1 back on SetBlendState (issue #896)";
    }

    // Leave nothing standing for the next test in this binary — the very leak
    // the tri-state makes possible.
    for (u32 i = 0; i < VulkanRecordedPipelineState::kMaxAttachments; ++i)
        api.ResetBlendStateForAttachment(i);
    api.SetBlendState(false);
    api.SetBlendFunc(RHI::BlendFactor::SrcAlpha, RHI::BlendFactor::OneMinusSrcAlpha);
}

#endif // OLO_WITH_VULKAN
