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
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MaterialShaderHeapTable.h"
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
#include "Platform/Vulkan/VulkanStorageBuffer.h"
#include "Platform/Vulkan/VulkanTexture.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include "VulkanTestSupport.h"
#include "TestTempDir.h"

#include <volk.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>
#include <stb_image/stb_image_write.h>

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

    // Poison-on-free is a DEBUG diagnostic in both backends' installs
    // (OpenGLRendererAPI::Init and VulkanDescriptorHeapBackend::
    // InstallOntoEngineHeap both gate HeapDesc::PoisonOnFree on OLO_DEBUG), so
    // the install this test just performed leaves it OFF in Release — and the
    // freed slot below keeps the dead texture's descriptor and samples 0xFF
    // instead of the poison's 0x00. That is issue #1087's entire Release-vs-
    // Debug split: not a missing barrier, not robustness2's nullDescriptor
    // (this backend's nulls are real 1x1 black images precisely because
    // nullDescriptor is not on the device floor), and not a stale binding.
    //
    // Re-initialise with poison ON rather than skipping the assertion in
    // Release: the contract in this test's name is a property of the heap, not
    // of the build, and a test that quietly stops checking half its name in
    // the shipping configuration is the green-run-that-tested-nothing shape.
    // Legal here because Initialize retires every live slot first and no view
    // has been minted yet.
    RHI::HeapDesc poisoningDesc = engineHeap.GetDesc();
    poisoningDesc.PoisonOnFree = true;
    RHI::DescriptorHeap::Get().Initialize(poisoningDesc, engineHeap.GetBackend());
    ASSERT_TRUE(engineHeap.IsPoisonOnFree());

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
    const u64 poisonedBefore = engineHeap.GetStats().SlotsPoisoned;
    engineHeap.DestroyView(view);
    EXPECT_FALSE(engineHeap.OffsetOf(view).IsValid()) << "a destroyed view's offset must reject";
    EXPECT_GT(engineHeap.GetStats().SlotsPoisoned, poisonedBefore)
        << "poison-on-free must be COUNTED, not just performed — the stat is how a caller learns the slot was "
           "overwritten (no-silent-fallbacks.md)";
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
    auto sampledCopy = Texture2D::Create(spec);
    ASSERT_NE(sampledCopy, nullptr);

    constexpr const char* kSampleCopySrc = R"(
#version 460 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 1) uniform sampler2D u_Source;
layout(binding = 0, rgba8) writeonly uniform image2D u_Output;
void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    imageStore(u_Output, pixel, texelFetch(u_Source, pixel, 0));
}
)";
    auto sampleCopy = ComputeShader::CreateFromSource("DrawPathUnifiedSampleCopy", kSampleCopySrc);
    ASSERT_NE(sampleCopy, nullptr);
    ASSERT_TRUE(sampleCopy->IsValid());

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

                    // Exercise the production sampled descriptor after a GPU
                    // storage write. Unified mode must keep GENERAL at bind
                    // time too; the optimal arm must still transition.
                    api.BindTexture(1, target->GetRHIHandle());
                    const auto image = static_cast<VulkanTexture2D*>(target.Raw())->GetVkImage();
                    const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
                    EXPECT_EQ(api.LayoutTracker().CurrentLayout(image, whole), m_Device->GetSampledImageLayout());
                    sampleCopy->Bind();
                    api.BindImageTexture(0, sampledCopy->GetRHIHandle(), 0, false, 0, RHI::Access::StorageWrite,
                                         RHI::Format::RGBA8UNorm);
                    api.DispatchCompute(2, 2, 1);
                    toSampled.Resource = sampledCopy->GetRHIHandle();
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                });

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u) << "the dispatch path must not fall through to a stub";

    std::vector<u8> pixels;
    ASSERT_TRUE(sampledCopy->GetData(pixels, 0));
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

// =============================================================================
// The command-ordered snapshot seam applies to CPU-fed buffers ONLY (#1058).
//
// VulkanStorageBuffer::SetData versions a mid-frame write into the frame arena
// so draws recorded after it read the new bytes and earlier ones keep what they
// recorded — GL's glNamedBufferSubData ordering, and the reason the auto-batched
// instanced draws stopped all sampling the last upload (#691). That mechanism is
// correct for a buffer whose ONLY producer is the CPU, and actively wrong for one
// the GPU produces: a compute dispatch resolves GetDeviceAddress and writes the
// PERSISTENT allocation, so a CPU snapshot hands every later draw a second copy
// the dispatch can never reach.
//
// That split is what made virtual geometry's mesh-shader arm draw nothing.
// PrepareFrame zeroes the draw-args buffer before the cull dispatches; the cull
// wrote the real DrawCount into the persistent buffer; the task stage is a DRAW,
// so it read the CPU's zero snapshot and issued EmitMeshTasksEXT(0) — a legal
// launch into an empty frame, invisible to every counter the backend keeps.
//
// BOTH directions are asserted from one recording, because either alone passes on
// a broken build: the DynamicCopy half is the fix, and the DynamicDraw half is
// what stops a future change from disabling the #691 mechanism outright to get it.
// The SubmitFrame bracket is load-bearing — PushSnapshot deliberately no-ops
// outside a recording bracket (load-time uploads ARE the ordered value), so the
// seam only engages here.
// =============================================================================
TEST_F(VulkanDrawPath, GpuProducedStorageBufferNeverServesADrawFromACpuSnapshot)
{
    // ScopedVulkanRenderCommandSelection, not the bare API-enum switch: the
    // snapshot seam probes the LIVE api through RenderCommand::GetRendererAPI()
    // (VulkanUpload::TryGetRecordingVulkanAPI), so a local VulkanRendererAPI the
    // facade does not publish leaves every SetData outside a recording bracket
    // and both halves resolve persistent — the fix's assertion would pass for
    // the wrong reason.
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr u32 kBytes = 64u;
    auto gpuProduced = StorageBuffer::Create(kBytes, 30u, StorageBufferUsage::DynamicCopy);
    auto cpuProduced = StorageBuffer::Create(kBytes, 31u, StorageBufferUsage::DynamicDraw);
    ASSERT_TRUE(gpuProduced && cpuProduced);

    auto* vkGpuProduced = static_cast<VulkanStorageBuffer*>(gpuProduced.Raw());
    auto* vkCpuProduced = static_cast<VulkanStorageBuffer*>(cpuProduced.Raw());
    const VkDeviceAddress gpuPersistent = vkGpuProduced->GetDeviceAddress();
    const VkDeviceAddress cpuPersistent = vkCpuProduced->GetDeviceAddress();
    ASSERT_NE(gpuPersistent, VkDeviceAddress{ 0 });
    ASSERT_NE(cpuPersistent, VkDeviceAddress{ 0 });

    const std::array<u32, kBytes / sizeof(u32)> payload{};

    auto& api = renderCommandSelection.Get();
    VkDeviceAddress gpuRootDuringFrame = 0;
    VkDeviceAddress cpuRootDuringFrame = 0;
    SubmitFrame(api,
                [&]()
                {
                    // The virtual-geometry shape: a whole-buffer CPU zero-init in
                    // front of the dispatch that will fill it.
                    gpuProduced->SetData(payload.data(), kBytes, 0);
                    cpuProduced->SetData(payload.data(), kBytes, 0);
                    gpuRootDuringFrame = vkGpuProduced->GetRootDataAddress();
                    cpuRootDuringFrame = vkCpuProduced->GetRootDataAddress();
                });

    EXPECT_EQ(gpuRootDuringFrame, gpuPersistent)
        << "a DynamicCopy (GPU-produced) buffer must resolve the PERSISTENT address for a draw — a frame-arena "
           "snapshot is a copy the compute dispatch that owns this buffer can never write, which is how the "
           "virtual-geometry task stage read a zeroed DrawCount and launched EmitMeshTasksEXT(0) (issue #1058)";
    EXPECT_NE(cpuRootDuringFrame, cpuPersistent)
        << "a DynamicDraw (CPU-fed) buffer written mid-frame must still hand later draws a fresh arena "
           "snapshot — without it every draw in the frame reads the LAST SetData at execute time, which is "
           "the batched-instance failure of issue #691";
    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u);

    // A snapshot is scoped to the arena FRAME, not to the recording bracket, so
    // it is still live here — and must not be after the next BeginFrame, whose
    // arena ranges recycle out from under it.
    EXPECT_NE(vkCpuProduced->GetRootDataAddress(), cpuPersistent);
    VulkanFrameArena::Get().BeginFrame(1);
    EXPECT_EQ(vkCpuProduced->GetRootDataAddress(), cpuPersistent)
        << "a snapshot from a previous frame generation must not outlive it — the arena range it points at "
           "has been rewound";
    EXPECT_EQ(vkGpuProduced->GetRootDataAddress(), gpuPersistent);
}

// A snapshot the draw can index past is the whole point of issue #1080:
// GetRootDataAddress hands the shader a bare device address with no length, so
// the ONLY thing that keeps a read inside this buffer is the snapshot spanning
// all of it. The measured case was a 176-byte snapshot standing in for an
// 11,264-byte binding-17 buffer — a shader reading element 20 walked into
// whatever the frame arena handed out next. Note the shape: every write
// observed in the wild started at offset 0, so the pre-existing prefix guard
// never engaged and only the TAIL was ever short.
TEST_F(VulkanDrawPath, PartialStorageBufferWriteSnapshotsTheWholeBufferOrNothing)
{
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr u32 kElements = 64u;
    constexpr u32 kBytes = kElements * sizeof(u32);
    auto buffer = StorageBuffer::Create(kBytes, 31u, StorageBufferUsage::DynamicDraw);
    ASSERT_TRUE(buffer);
    auto* vkBuffer = static_cast<VulkanStorageBuffer*>(buffer.Raw());
    const VkDeviceAddress persistent = vkBuffer->GetDeviceAddress();
    ASSERT_NE(persistent, VkDeviceAddress{ 0 });

    // Seed the whole buffer OUTSIDE a recording bracket, so this write is a
    // plain persistent write-through with no snapshot behind it — the setup
    // shape PushSnapshot's own no-recording guard describes.
    std::array<u32, kElements> seed{};
    for (u32 i = 0; i < kElements; ++i)
    {
        seed[i] = 0xA0000000u | i;
    }
    buffer->SetData(seed.data(), kBytes, 0);
    ASSERT_EQ(vkBuffer->GetLiveSnapshotBytes(), 0u)
        << "a write outside a recording bracket must not stage a snapshot at all";

    auto& api = renderCommandSelection.Get();
    const u64 refusedBefore = VulkanStorageBuffer::GetSnapshotRefusedCount();
    constexpr u32 kWrittenElements = 4u;
    constexpr u32 kWrittenBytes = kWrittenElements * sizeof(u32);
    const std::array<u32, kWrittenElements> head{ 0xB0000000u, 0xB0000001u, 0xB0000002u, 0xB0000003u };

    u32 headWriteSnapshotBytes = 0;
    u32 midWriteSnapshotBytes = 0;
    std::array<u32, kElements> afterHeadWrite{};
    std::array<u32, kElements> afterMidWrite{};
    VkDeviceAddress rootAfterHeadWrite = 0;

    SubmitFrame(api,
                [&]()
                {
                    // (1) The measured shape: offset 0, a fraction of the
                    // buffer. Pre-fix this produced a 16-byte snapshot for a
                    // 256-byte buffer.
                    buffer->SetData(head.data(), kWrittenBytes, 0);
                    headWriteSnapshotBytes = vkBuffer->GetLiveSnapshotBytes();
                    rootAfterHeadWrite = vkBuffer->GetRootDataAddress();
                    if (const auto* cpu = vkBuffer->GetLiveSnapshotCpuData(); cpu != nullptr)
                    {
                        std::memcpy(afterHeadWrite.data(), cpu,
                                    std::min<sizet>(kBytes, vkBuffer->GetLiveSnapshotBytes()));
                    }

                    // (2) A write that starts mid-buffer, layered on the live
                    // snapshot: the prefix comes from the snapshot, the tail
                    // must too.
                    const std::array<u32, 2> mid{ 0xC0000000u, 0xC0000001u };
                    buffer->SetData(mid.data(), static_cast<u32>(mid.size() * sizeof(u32)),
                                    static_cast<u32>(8 * sizeof(u32)));
                    midWriteSnapshotBytes = vkBuffer->GetLiveSnapshotBytes();
                    if (const auto* cpu = vkBuffer->GetLiveSnapshotCpuData(); cpu != nullptr)
                    {
                        std::memcpy(afterMidWrite.data(), cpu,
                                    std::min<sizet>(kBytes, vkBuffer->GetLiveSnapshotBytes()));
                    }
                });

    // The contract, stated as the issue states it: whole buffer, or nothing.
    // Both are safe; a snapshot in between is the out-of-bounds device read.
    EXPECT_TRUE(headWriteSnapshotBytes == kBytes || headWriteSnapshotBytes == 0u)
        << "a mid-frame partial SetData must snapshot all " << kBytes << " bytes or refuse the snapshot outright; "
        << "it staged " << headWriteSnapshotBytes
        << " and the draw's root-data address carries no length to bound the difference (issue #1080)";
    EXPECT_TRUE(midWriteSnapshotBytes == kBytes || midWriteSnapshotBytes == 0u)
        << "same rule for a write that starts mid-buffer — it staged " << midWriteSnapshotBytes;

    // This device gives DynamicDraw storage buffers a host-visible placement,
    // so the undefined bytes always have a CPU-readable source and the
    // snapshot must actually happen. If that ever stops being true the
    // EXPECT_TRUEs above still hold and this one names why it changed.
    ASSERT_EQ(headWriteSnapshotBytes, kBytes)
        << "no snapshot was staged at all — the buffer has no mapped placement and no live snapshot to source "
           "the undefined bytes from, which is a legal but different outcome than this test set up";
    EXPECT_NE(rootAfterHeadWrite, persistent);

    // Content, not just extent: bytes the write did not define must carry the
    // persistent buffer's real values, never a zero fill that only LOOKS
    // defined to a shader.
    for (u32 i = 0; i < kWrittenElements; ++i)
    {
        EXPECT_EQ(afterHeadWrite[i], head[i]) << "written element " << i;
    }
    for (u32 i = kWrittenElements; i < kElements; ++i)
    {
        EXPECT_EQ(afterHeadWrite[i], seed[i])
            << "tail element " << i << " must carry the persistent buffer's value, not a zero fill";
    }

    ASSERT_EQ(midWriteSnapshotBytes, kBytes);
    EXPECT_EQ(afterMidWrite[8], 0xC0000000u);
    EXPECT_EQ(afterMidWrite[9], 0xC0000001u);
    for (u32 i = 0; i < kWrittenElements; ++i)
    {
        EXPECT_EQ(afterMidWrite[i], head[i]) << "prefix element " << i << " must survive from the live snapshot";
    }
    for (u32 i = 10; i < kElements; ++i)
    {
        EXPECT_EQ(afterMidWrite[i], seed[i]) << "tail element " << i << " must survive from the live snapshot";
    }

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u);

    // The refusal path is the other half of the contract: whole-buffer OR
    // nothing means the "nothing" branch has to be observable, not just
    // greppable in a log. This buffer is mapped, so nothing should have been
    // refused here — and if a future change makes the arena refuse instead,
    // this says so rather than letting the EXPECT_TRUEs above pass on the
    // no-snapshot arm.
    EXPECT_EQ(VulkanStorageBuffer::GetSnapshotRefusedCount(), refusedBefore)
        << "a mapped DynamicDraw buffer inside a recording bracket must not need the refusal path";
}

// The whole-buffer rule of #1080 is only affordable because an UNREAD snapshot
// is rewritten in place. GPUScene::Upload issues one SetData per non-adjacent
// dirty range, so without reuse a frame that dirties N scattered records would
// claim N whole-buffer arena ranges — and the arena is where every draw's root
// data lives, so exhausting it drops root data for the entire frame.
TEST_F(VulkanDrawPath, ConsecutiveWritesShareOneSnapshotUntilADrawReadsIt)
{
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr u32 kElements = 64u;
    constexpr u32 kBytes = kElements * sizeof(u32);
    auto buffer = StorageBuffer::Create(kBytes, 31u, StorageBufferUsage::DynamicDraw);
    ASSERT_TRUE(buffer);
    auto* vkBuffer = static_cast<VulkanStorageBuffer*>(buffer.Raw());

    std::array<u32, kElements> seed{};
    buffer->SetData(seed.data(), kBytes, 0);

    auto& api = renderCommandSelection.Get();
    auto& arena = VulkanFrameArena::Get();
    u64 allocationsForBatch = 0;
    u64 allocationsAfterDraw = 0;
    VkDeviceAddress firstAddress = 0;
    VkDeviceAddress batchAddress = 0;
    VkDeviceAddress afterDrawAddress = 0;
    // Wide enough to cover every element the batch writes (0, 4, 8, 12) — a
    // 4-element readback observed only `e` and so could not tell an accumulating
    // reuse path from one that dropped b, c and d.
    std::array<u32, 16> readback{};

    SubmitFrame(api,
                [&]()
                {
                    // A batch of scattered writes with NO draw between them —
                    // the GPUScene::Upload shape.
                    const u64 before = arena.GetAllocationCountThisFrame();
                    const u32 a = 0xAAAAAAAAu;
                    buffer->SetData(&a, sizeof(a), 0);
                    firstAddress = vkBuffer->GetLiveSnapshotBytes() != 0 ? VkDeviceAddress{ 1 } : VkDeviceAddress{ 0 };
                    const u32 b = 0xBBBBBBBBu;
                    buffer->SetData(&b, sizeof(b), 4 * sizeof(u32));
                    const u32 c = 0xCCCCCCCCu;
                    buffer->SetData(&c, sizeof(c), 8 * sizeof(u32));
                    const u32 d = 0xDDDDDDDDu;
                    buffer->SetData(&d, sizeof(d), 12 * sizeof(u32));
                    allocationsForBatch = arena.GetAllocationCountThisFrame() - before;
                    batchAddress = vkBuffer->GetRootDataAddress(); // consumes it

                    // Now a write AFTER the address was handed to a draw: it
                    // must NOT overwrite what that draw is going to read.
                    const u64 beforeSecond = arena.GetAllocationCountThisFrame();
                    const u32 e = 0xEEEEEEEEu;
                    buffer->SetData(&e, sizeof(e), 0);
                    allocationsAfterDraw = arena.GetAllocationCountThisFrame() - beforeSecond;
                    afterDrawAddress = vkBuffer->GetRootDataAddress();
                    if (const auto* cpu = vkBuffer->GetLiveSnapshotCpuData(); cpu != nullptr)
                    {
                        std::memcpy(readback.data(), cpu, readback.size() * sizeof(u32));
                    }
                });

    EXPECT_EQ(firstAddress, VkDeviceAddress{ 1 }) << "the first write must stage a snapshot at all";
    EXPECT_EQ(allocationsForBatch, 1u)
        << "four consecutive SetData calls with no draw between them must share ONE whole-buffer arena range — "
           "one per write is what exhausts the frame arena and drops root data for the whole frame";
    EXPECT_EQ(allocationsAfterDraw, 1u)
        << "a write AFTER a draw embedded the address must stage a NEW range, or that draw's bytes change under "
           "it and command ordering (issue #691) is lost";
    EXPECT_NE(batchAddress, afterDrawAddress) << "the post-draw write must be a different arena range";

    // The reuse path must accumulate EVERY write, not just the last — and the
    // fresh range staged after the draw must inherit them, since it is sourced
    // from the snapshot the draw consumed.
    EXPECT_EQ(readback[0], 0xEEEEEEEEu) << "the post-draw write must land at element 0";
    EXPECT_EQ(readback[4], 0xBBBBBBBBu) << "write b was dropped — the batch did not accumulate";
    EXPECT_EQ(readback[8], 0xCCCCCCCCu) << "write c was dropped — the batch did not accumulate";
    EXPECT_EQ(readback[12], 0xDDDDDDDDu) << "write d was dropped — the batch did not accumulate";
    EXPECT_EQ(readback[1], 0u) << "bytes no writer touched must stay as the seeded zeros";
    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u);
}

// A GPU-side write recorded this frame makes the host mapping a liar: it
// executes at submit and never touches m_Mapped. Sourcing a partial write's
// undefined bytes from the mapping would put the cleared content straight back
// for every draw recorded afterwards.
TEST_F(VulkanDrawPath, AClearedRangeIsNotResurrectedByALaterPartialWrite)
{
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr u32 kElements = 64u;
    constexpr u32 kBytes = kElements * sizeof(u32);
    auto buffer = StorageBuffer::Create(kBytes, 31u, StorageBufferUsage::DynamicDraw);
    ASSERT_TRUE(buffer);
    auto* vkBuffer = static_cast<VulkanStorageBuffer*>(buffer.Raw());
    const VkDeviceAddress persistent = vkBuffer->GetDeviceAddress();

    std::array<u32, kElements> seed{};
    for (u32 i = 0; i < kElements; ++i)
    {
        seed[i] = 0xA0000000u | i;
    }
    buffer->SetData(seed.data(), kBytes, 0);

    auto& api = renderCommandSelection.Get();
    u64 refusedBefore = 0;
    u64 refusedAfter = 0;
    VkDeviceAddress rootAfter = 0;

    SubmitFrame(api,
                [&]()
                {
                    // Clear the tail inside the bracket: this records a fill
                    // into the frame command buffer and leaves m_Mapped alone.
                    buffer->ClearData(static_cast<u32>(32 * sizeof(u32)), static_cast<u32>(32 * sizeof(u32)));
                    refusedBefore = VulkanStorageBuffer::GetSnapshotRefusedCount();
                    // A partial write now cannot source the cleared tail from
                    // anywhere truthful, so it must refuse the snapshot rather
                    // than rebuild the pre-clear bytes out of the mapping.
                    const u32 v = 0xB0000000u;
                    buffer->SetData(&v, sizeof(v), 0);
                    refusedAfter = VulkanStorageBuffer::GetSnapshotRefusedCount();
                    rootAfter = vkBuffer->GetRootDataAddress();
                });

    EXPECT_EQ(refusedAfter, refusedBefore + 1)
        << "the snapshot must be refused, loudly and countably, once the host mapping can no longer supply the "
           "bytes the write does not define";
    EXPECT_EQ(rootAfter, persistent)
        << "and the draw must fall back to the PERSISTENT buffer, which is where the recorded clear actually "
           "lands — a snapshot rebuilt from m_Mapped would undo the clear for every later draw";
    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u);
}

// The mirror of the test above: a clear that goes through the MAPPED path
// updates m_Mapped itself, so the mapping stays a truthful source and a later
// partial write must still snapshot. Marking the mapping stale for every clear
// (rather than only for the recorded fill) would silently drop this buffer to
// last-write-wins ordering — safe, but a needless loss of the #691 guarantee,
// and it would bump the refusal counter where nothing is wrong.
TEST_F(VulkanDrawPath, AMappedClearOutsideTheBracketStillAllowsALaterSnapshot)
{
    ScopedVulkanRenderCommandSelection renderCommandSelection;
    VulkanFrameArena::Get().BeginFrame(0);

    constexpr u32 kElements = 64u;
    constexpr u32 kBytes = kElements * sizeof(u32);
    auto buffer = StorageBuffer::Create(kBytes, 31u, StorageBufferUsage::DynamicDraw);
    ASSERT_TRUE(buffer);
    auto* vkBuffer = static_cast<VulkanStorageBuffer*>(buffer.Raw());
    const VkDeviceAddress persistent = vkBuffer->GetDeviceAddress();

    std::array<u32, kElements> seed{};
    for (u32 i = 0; i < kElements; ++i)
    {
        seed[i] = 0xA0000000u | i;
    }
    buffer->SetData(seed.data(), kBytes, 0);

    // OUTSIDE any recording bracket: this takes the mapped memset arm, which
    // writes m_Mapped, so the mapping still tells the truth afterwards.
    buffer->ClearData(static_cast<u32>(32 * sizeof(u32)), static_cast<u32>(32 * sizeof(u32)));

    auto& api = renderCommandSelection.Get();
    const u64 refusedBefore = VulkanStorageBuffer::GetSnapshotRefusedCount();
    u32 snapshotBytes = 0;
    VkDeviceAddress root = 0;
    std::array<u32, kElements> readback{};

    SubmitFrame(api,
                [&]()
                {
                    const u32 v = 0xB0000000u;
                    buffer->SetData(&v, sizeof(v), 0);
                    snapshotBytes = vkBuffer->GetLiveSnapshotBytes();
                    root = vkBuffer->GetRootDataAddress();
                    if (const auto* cpu = vkBuffer->GetLiveSnapshotCpuData(); cpu != nullptr)
                    {
                        std::memcpy(readback.data(), cpu, std::min<sizet>(kBytes, snapshotBytes));
                    }
                });

    EXPECT_EQ(snapshotBytes, kBytes)
        << "a mapped clear keeps m_Mapped truthful, so the later partial write must still stage a whole-buffer "
           "snapshot rather than refuse";
    EXPECT_NE(root, persistent);
    EXPECT_EQ(VulkanStorageBuffer::GetSnapshotRefusedCount(), refusedBefore)
        << "nothing was unsourceable here, so the refusal counter must not move";

    // And the snapshot carries the CLEARED tail, not the pre-clear seed.
    EXPECT_EQ(readback[0], 0xB0000000u);
    EXPECT_EQ(readback[8], seed[8]) << "the untouched prefix must survive";
    EXPECT_EQ(readback[32], 0u) << "the cleared tail must read as zeros, not the pre-clear seed";
    EXPECT_EQ(readback[63], 0u);
    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u);
}

// =============================================================================
// #1200: the index bind's `addressFlags` are a TWO-DIRECTIONAL obligation, and
// the engine's two index-buffer families sit on OPPOSITE sides of it.
//
//   VUID-VkBindIndexBuffer3InfoKHR-addressRange-13122 REQUIRES
//     STORAGE_BUFFER_USAGE (or UNKNOWN_) when the backing buffer was created
//     with VK_BUFFER_USAGE_STORAGE_BUFFER_BIT — the raw dual-role
//     element/SSBO arena behind SetVertexArrayIndexBuffer.
//   VUID-...-13123 FORBIDS it when the buffer was created without the bit —
//     a VulkanIndexBuffer, which is INDEX|TRANSFER|ADDRESS only.
//
// So there is no value that is safe for both, and "silence the layer" is not a
// direction of travel: moving either family toward the other's answer trades
// one violation for the other. That is exactly the mistake issue #1200
// proposed (give the object-backed family STORAGE_BUFFER_USAGE), which is why
// the DECISION is asserted here and not just the drawing.
//
// The draws matter too: with a validation layer attached, the fixture's
// zero-errors TearDown is the layer adjudicating both VUIDs on real binds.
// =============================================================================
TEST_F(VulkanDrawPath, IndexBindAddressFlagsFollowEachFamilysCreateTimeStorageUsage)
{
    ScopedVulkanApiSelection vulkanApi;
    VulkanFrameArena::Get().BeginFrame(0);

    VulkanRendererAPI api;

    // --- the object-backed family ------------------------------------------
    const f32 vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    auto vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
    u32 indices[] = { 0u, 1u, 2u };
    auto indexBuffer = IndexBuffer::Create(indices, 3);
    auto objectVao = VertexArray::Create();
    objectVao->AddVertexBuffer(vertexBuffer);
    objectVao->SetIndexBuffer(indexBuffer);
    const auto* vkObjectVao = static_cast<const VulkanVertexArray*>(objectVao.Raw());

    EXPECT_EQ(VulkanRendererAPI::IndexBindAddressFlagsFor(vkObjectVao), VkAddressCommandFlagsKHR{ 0 })
        << "VulkanIndexBuffer is created without STORAGE_BUFFER_BIT, so VUID-13123 FORBIDS "
           "VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR on its bind";

    // --- the raw dual-role family ------------------------------------------
    // Built the way VirtualMeshRegistry builds it, not substituted for an
    // object-backed spelling: the raw arena is the one that carries
    // STORAGE_BUFFER_BIT, and substituting the object form here would test the
    // buffer that cannot fail (substituted-seams-compound.md).
    const RHI::ResourceHandle rawIndices = api.CreateBufferHandle();
    ASSERT_TRUE(rawIndices.IsValid());
    api.AllocateBufferStorage(rawIndices, sizeof(indices), RHI::MemoryResidency::DeviceLocal);
    api.UploadBufferSubData(rawIndices, 0, sizeof(indices), indices);
    const RHI::ResourceHandle rawVaoHandle = api.CreateVertexArrayHandle();
    ASSERT_TRUE(rawVaoHandle.IsValid());
    api.SetVertexArrayIndexBuffer(rawVaoHandle, rawIndices);

    // Released on EVERY exit, not just the last line: the ASSERT_* macros
    // below return early, and the raw registry is process-global — a handle
    // that survives this test keeps its VMA allocation alive into the
    // fixture's vmaDestroyAllocator (VulkanRawBufferRegistry's own header
    // documents that abort).
    struct RawHandleGuard
    {
        VulkanRendererAPI& Api;
        RHI::ResourceHandle Vao;
        RHI::ResourceHandle Buffer;
        ~RawHandleGuard()
        {
            Api.DeleteVertexArray(Vao);
            Api.DeleteBuffer(Buffer);
        }
    } rawHandleGuard{ api, rawVaoHandle, rawIndices };

    const auto* rawEntry = VulkanRootObjectRegistry::Get().Lookup(rawVaoHandle);
    ASSERT_NE(rawEntry, nullptr);
    ASSERT_EQ(rawEntry->Kind, VulkanRootObjectKind::VertexArray);
    const auto* vkRawVao = static_cast<const VulkanVertexArray*>(rawEntry->Object);

    EXPECT_EQ(VulkanRendererAPI::IndexBindAddressFlagsFor(vkRawVao),
              VkAddressCommandFlagsKHR{ VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR })
        << "the raw family's usage set includes STORAGE_BUFFER_BIT, so VUID-13122 REQUIRES the flag";

    // --- and both actually draw, with the layer watching --------------------
    // gl_VertexIndex only: a raw VAO has no VulkanVertexBuffer, so a shader
    // declaring the pull SSBO would read the zero address.
    //
    // The tint UBO is not decoration. A shader that declares NO root data at
    // all has an empty root-data layout, and the frame arena refuses a
    // zero-byte push — so every draw is dropped before it reaches
    // BindIndexBufferFor and this test would assert against a layer that
    // adjudicated nothing. One binding is the cheapest way to keep the draws
    // real; see the empty-layout drop this test's sibling commit made loud.
    constexpr const char* kIndexOnlyVertexSrc = R"(
#version 460 core
void main()
{
    vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
)";
    constexpr const char* kSolidFragmentSrc = R"(
#version 460 core
layout(location = 0) out vec4 o_Color;
layout(std140, binding = 3) uniform TintBlock
{
    vec4 u_Tint;
};
void main()
{
    o_Color = u_Tint;
}
)";
    auto shader = Ref<VulkanShader>::Create("IndexBindAddressFlags", kIndexOnlyVertexSrc, kSolidFragmentSrc);
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    auto tintUbo = UniformBuffer::Create(16, 3);
    const f32 green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    tintUbo->SetData(green, sizeof(green));

    FramebufferSpecification fbSpec;
    fbSpec.Width = 32;
    fbSpec.Height = 32;
    fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
    auto framebuffer = Framebuffer::Create(fbSpec);
    ASSERT_NE(framebuffer, nullptr);
    const auto colorHandle = framebuffer->GetColorAttachmentHandle(0);
    ASSERT_TRUE(colorHandle.IsValid());

    SubmitFrame(api,
                [&]()
                {
                    RHI::Barrier toColor{};
                    toColor.Resource = colorHandle;
                    toColor.Before = RHI::Access::Undefined;
                    toColor.After = RHI::Access::ColorAttachmentWrite;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toColor, 1 });

                    framebuffer->Bind();
                    api.SetViewport(0, 0, 32, 32);
                    api.SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
                    api.Clear();
                    shader->Bind();
                    tintUbo->Bind();

                    // Object-backed bind, then the raw one. Two different
                    // addresses AND two different addressFlags in one command
                    // buffer, which is the pairing the redundant-bind cache
                    // could otherwise hide.
                    api.DrawIndexed(objectVao, 3);
                    api.BindVertexArrayRaw(rawVaoHandle);
                    api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, 3, RHI::IndexType::UInt32, 0);

                    framebuffer->Unbind();

                    RHI::Barrier toSampled{};
                    toSampled.Resource = colorHandle;
                    toSampled.Before = RHI::Access::ColorAttachmentWrite;
                    toSampled.After = RHI::Access::ShaderSampleRead;
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                });

    EXPECT_EQ(api.GetUnimplementedStubHitCount(), 0u) << "neither family may fall through to a stub";

    // The CENSUS, not GetPreparedDrawsThisRecording(): that counter moves
    // inside PrepareDraw, BEFORE the index bind, so a raw-family draw refused
    // by BindIndexBufferFor would leave it at 2 and leave the layer with
    // nothing to adjudicate — while the target still reads green from the
    // object-backed draw. The census entry is incremented at the vkCmdDraw*
    // itself, so 2 here means BOTH binds were issued.
    const auto census = api.GetDrawCensus();
    const auto entry = census.find("IndexBindAddressFlags");
    ASSERT_NE(entry, census.end()) << "no draw reached a vkCmdDraw* at all";
    EXPECT_EQ(entry->second.Prepared, 2u) << "both families must have ISSUED a draw, index bind included";
    EXPECT_EQ(entry->second.Dropped, 0u) << "a dropped draw would make the layer's silence vacuous";

    // The draws really rasterised — otherwise nothing was bound and the
    // zero-validation-errors assertion below proves nothing.
    auto* vkFramebuffer = static_cast<VulkanFramebuffer*>(framebuffer.Raw());
    const auto attachment = vkFramebuffer->GetColorAttachmentImage(0);
    ASSERT_NE(attachment, nullptr);
    std::vector<u8> pixels;
    ASSERT_TRUE(attachment->GetData(pixels, 0));
    ASSERT_EQ(pixels.size(), sizet{ 32 * 32 * 4 });
    EXPECT_EQ(pixels[0], 0x00);
    EXPECT_EQ(pixels[1], 0xFF) << "the indexed draws must have covered the target, not left the red clear";
    EXPECT_EQ(pixels[2], 0x00);
}

// #805: the production material shader, table builder and frustum-cull/root
// handoff. Only the material-selection policy is a probe compute shader.
// CPU readback happens after the draw, never between selection and consumption.
TEST_F(VulkanDrawPath, GBufferGpuSelectsTexturesInSingleIndirectDraw)
{
    ScopedOloEditorWorkingDirectory directory;
    ASSERT_TRUE(directory.IsValid());
    ScopedVulkanRenderCommandSelection selection;
    auto& api = selection.Get();
    VulkanFrameArena::Get().BeginFrame(0);
    struct HeapShutdown
    {
        ~HeapShutdown()
        {
            RHI::DescriptorHeap::Get().Shutdown();
        }
    } heapShutdown;
    ASSERT_TRUE(VulkanDescriptorHeapBackend::InstallOntoEngineHeap());
    ASSERT_TRUE(HeapBinding::ShaderHeapIndexingSupported());

    constexpr u32 kSize = 96u;
    FramebufferSpecification spec;
    spec.Width = kSize;
    spec.Height = kSize;
    spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RGBA16F,
                         FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::RG16F,
                         FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::RGBA16F };
    auto framebuffer = Framebuffer::Create(spec);
    ASSERT_TRUE(framebuffer);
    auto shader = Ref<VulkanShader>::Create("assets/shaders/PBR_GBuffer.glsl");
    ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

    // Two separated quads. A reference draw per material and one indirect draw
    // with both instances must produce identical MRT bytes.
    const std::array<f32, 32> vertices{
        -0.4f, -0.6f, 0.5f, 0, 0, 1, 0, 0,
        0.4f, -0.6f, 0.5f, 0, 0, 1, 1, 0,
        0.4f, 0.6f, 0.5f, 0, 0, 1, 1, 1,
        -0.4f, 0.6f, 0.5f, 0, 0, 1, 0, 1
    };
    std::array<u32, 6> indices{ 0, 1, 2, 2, 3, 0 };
    auto vb = VertexBuffer::Create(vertices.data(), sizeof(vertices));
    auto ib = IndexBuffer::Create(indices.data(), static_cast<u32>(indices.size()));
    auto vao = VertexArray::Create();
    ASSERT_TRUE(vb && ib && vao);
    vao->AddVertexBuffer(vb);
    vao->SetIndexBuffer(ib);

    std::array<std::array<u8, 4>, 10> texels{ { { 255, 0, 0, 255 }, { 0, 128, 255, 255 }, { 128, 128, 255, 255 }, { 64, 64, 64, 255 }, { 0, 0, 255, 255 }, { 0, 255, 0, 255 }, { 0, 64, 128, 255 }, { 192, 128, 238, 255 }, { 192, 192, 192, 255 }, { 255, 0, 0, 255 } } };
    std::array<Ref<Texture2D>, 10> textures;
    std::array<u32, 10> offsets{};
    TextureSpecification textureSpec;
    textureSpec.Width = textureSpec.Height = 1;
    textureSpec.Format = ImageFormat::RGBA8;
    textureSpec.GenerateMips = false;
    for (sizet i = 0; i < textures.size(); ++i)
    {
        textures[i] = Texture2D::Create(textureSpec);
        ASSERT_TRUE(textures[i]);
        textures[i]->SetData(texels[i].data(), 4u);
        const auto offset = HeapBinding::ResolveShaderHeapTexture(textures[i]->GetRHIHandle());
        ASSERT_TRUE(offset.IsValid());
        offsets[i] = offset.Value;
    }
    const auto sampler = HeapBinding::ResolveShaderHeapSampler(HeapBinding::MaterialTexture2DSampler());
    const auto nullTexture = HeapBinding::ResolveShaderHeapNullTexture();
    ASSERT_TRUE(sampler.IsValid() && nullTexture.IsValid());

    GPUScene scene;
    const GPUSceneGeometryKey geometryKey{ .m_VertexBuffer = 1, .m_IndexBuffer = 2 };
    const std::array<GPUSceneMaterialKey, 2> keys{
        GPUSceneMaterialKey{ .m_Owner = 1 }, GPUSceneMaterialKey{ .m_Owner = 2 }
    };
    scene.BeginExtraction(1, glm::vec3(0.0f));
    scene.ExtractGeometry(geometryKey, GPUSceneGeometryInput{
                                           .m_VertexBuffer = vb->GetRHIHandle(), .m_IndexBuffer = ib->GetRHIHandle(), .m_IndexCount = 6, .m_VertexCount = 4 });
    for (u32 i = 0; i < 2u; ++i)
    {
        GPUSceneMaterialInput material;
        material.m_MetallicFactor = 1.0f;
        material.m_EmissiveFactor = glm::vec4(1.0f);
        material.m_Flags = GPUSceneMaterialFlagPBR;
        material.m_Albedo.m_Handle = textures[i * 5u]->GetRHIHandle();
        material.m_MetallicRoughness.m_Handle = textures[i * 5u + 1u]->GetRHIHandle();
        material.m_Normal.m_Handle = textures[i * 5u + 2u]->GetRHIHandle();
        material.m_Occlusion.m_Handle = textures[i * 5u + 3u]->GetRHIHandle();
        material.m_Emissive.m_Handle = textures[i * 5u + 4u]->GetRHIHandle();
        scene.ExtractMaterial(keys[i], material);
        scene.ExtractInstance({ .m_EntityId = i + 1u, .m_Geometry = geometryKey }, GPUSceneInstanceInput{ .m_Material = keys[i] });
    }
    (void)scene.EndExtraction();
    scene.InitializeGPU();
    scene.Upload();
    MaterialShaderHeapTable table;
    table.Update(scene);
    auto tableInfo = table.GetAddressAndCount();
    ASSERT_EQ(tableInfo.z, 2u);
    ASSERT_EQ(table.GetUnresolvedCount(), 0u);
    // Steady updates reuse the immutable snapshot.
    table.Update(scene);
    EXPECT_EQ(table.GetAddressAndCount().x, tableInfo.x);
    EXPECT_EQ(table.GetAddressAndCount().y, tableInfo.y);

    UBOStructures::CameraUBO cameraData{};
    cameraData.ViewProjection = cameraData.View = cameraData.Projection = glm::mat4(1.0f);
    auto camera = UniformBuffer::Create(sizeof(cameraData), ShaderBindingLayout::UBO_CAMERA);
    camera->SetData(&cameraData, sizeof(cameraData));
    const std::array<glm::mat4, 2> motionData{ glm::mat4(1.0f), glm::mat4(1.0f) };
    auto motion = UniformBuffer::Create(sizeof(motionData), 8u);
    motion->SetData(motionData.data(), sizeof(motionData));
    const glm::uvec4 zeros{ 0u };
    auto lightmap = UniformBuffer::Create(16u, 1u);
    lightmap->SetData(&zeros, 16u);
    auto materialUBO = UniformBuffer::Create(sizeof(ShaderBindingLayout::PBRMaterialUBO), ShaderBindingLayout::UBO_MATERIAL);
    auto params = UniformBuffer::Create(48u, 2u);
    auto stats = StorageBuffer::Create(144u, ShaderBindingLayout::SSBO_GPU_STATS, StorageBufferUsage::DynamicCopy);
    const std::array<u32, 36> noStats{};
    stats->SetData(noStats.data(), sizeof(noStats));
    auto input = Ref<InstanceBuffer>::Create(2u);
    auto culler = Ref<GPUFrustumCuller>::Create();
    GpuDrivenRootDataLayout layout{};
    ASSERT_TRUE(api.QueryGpuDrivenRootDataLayout(shader->GetRHIHandle(), ShaderBindingLayout::SSBO_INSTANCE_DATA, layout));

    // The probe changes only the policy that chooses a material; the culler,
    // output InstanceData, indirect command, root layout and draw are production.
    const auto selector = ComputeShader::CreateFromSource("MaterialSelection805", R"(
#version 460 core
layout(local_size_x=1) in;
layout(std430,binding=15) buffer Instances { uint words[]; };
layout(std430,binding=17) readonly buffer Indirect { uint count; uint instanceCount; };
layout(std140,binding=2) uniform SelectParams { uvec4 refs[2]; uvec4 mode; };
void main() {
    uint i=gl_GlobalInvocationID.x;
    if (i>=instanceCount) return;
    uint base=i*64u;
    uint selected=(words[base+52u]+mode.x)&1u;
    uvec4 ref=refs[selected];
    if (mode.y==1u) ref.w+=1u;
    if (mode.y==2u) ref.z=0xffffffffu;
    for (uint lane=0u;lane<4u;++lane) words[base+60u+lane]=ref[lane];
}
)");
    ASSERT_TRUE(selector && selector->IsValid());
    static_assert(sizeof(InstanceData) / sizeof(u32) == 64u);
    static_assert(offsetof(InstanceData, EntityID) / sizeof(u32) == 52u);
    static_assert(offsetof(InstanceData, GPUSceneRef) / sizeof(u32) == 60u);

    std::array<InstanceData, 2> instances;
    for (u32 i = 0; i < 2u; ++i)
    {
        instances[i].Transform[3].x = i == 0u ? -0.5f : 0.5f;
        instances[i].PrevTransform = instances[i].Transform;
        instances[i].EntityID = static_cast<i32>(i);
    }
    const auto fallbackMaterial = [&]()
    {
        ShaderBindingLayout::PBRMaterialUBO material{};
        material.BaseColorFactor = { 1.0f, 0.0f, 1.0f, 1.0f };
        material.EmissiveFactor = glm::vec4(1.0f);
        material.MetallicFactor = material.RoughnessFactor = material.NormalScale = material.OcclusionStrength = 1.0f;
        material.HeapOffsets[0] = glm::uvec4(nullTexture.Value);
        material.HeapOffsets[1] = { nullTexture.Value, 0u, 0u, 0u };
        material.HeapOffsets[2] = { nullTexture.Value, nullTexture.Value, nullTexture.Value, sampler.Value };
        return material;
    };
    using Images = std::array<std::vector<u8>, 6>;
    const auto capture = [&](bool merged, u32 phase, u32 invalidMode, Images& images)
    {
        culler->BeginFrame();
        std::array<glm::uvec4, 3> selectParams{};
        for (u32 i = 0; i < 2u; ++i)
        {
            const auto handle = scene.FindMaterial(keys[i]);
            selectParams[i] = { 0u, 0u, handle.m_Index, handle.m_Generation };
        }
        selectParams[2] = { phase, invalidMode, 0u, 0u };
        params->SetData(selectParams.data(), sizeof(selectParams));
        SubmitFrame(api, [&]()
                    {
            camera->Bind(); motion->Bind(); lightmap->Bind(); stats->Bind();
            GPUFrustumCuller::CullResult culled;
            if (merged)
            {
                culled = culler->Cull(instances, 6u, 0u, glm::vec4(0, 0, 0.5f, 0.8f), 1.0f, layout);
                ASSERT_TRUE(culled.OutputBuffer && culled.IndirectBuffer && culled.RootDataBuffer);
                params->Bind();
                culled.OutputBuffer->Bind(); culled.IndirectBuffer->Bind();
                selector->Bind();
                api.DispatchCompute(2u, 1u, 1u);
                api.MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);
            }
            ASSERT_TRUE(scene.BindMaterials()); // slot 17 was the indirect args in compute
            framebuffer->Bind();
            api.SetViewport(0, 0, kSize, kSize);
            api.SetClearColor({ 0, 0, 0, 0 });
            api.Clear();
            api.SetDepthTest(false); api.SetDepthMask(false); api.SetBlendState(false); api.DisableCulling();
            shader->Bind();
                api.BindVertexArrayRaw(vao->GetRHIHandle());
            if (merged)
            {
                auto material = fallbackMaterial();
                material.HeapOffsets[1].y = tableInfo.x;
                material.HeapOffsets[1].z = tableInfo.y;
                material.HeapOffsets[1].w = tableInfo.z;
                materialUBO->SetData(&material, sizeof(material)); materialUBO->Bind();
                culled.OutputBuffer->Bind();
                ASSERT_TRUE(api.SetNextDrawRootData(culled.RootDataBuffer->GetRHIHandle(), ShaderBindingLayout::SSBO_INSTANCE_DATA,
                                                    culled.RootDataAddressOffsetBytes));
                api.DrawBoundElementsIndirect(culled.IndirectBuffer->GetRHIHandle(), RHI::PrimitiveTopology::TriangleList);
            }
            else
            {
                for (u32 i = 0; i < 2u; ++i)
                {
                    auto material = fallbackMaterial();
                    material.BaseColorFactor = glm::vec4(1.0f);
                    material.UseAlbedoMap = material.UseMetallicRoughnessMap = material.UseNormalMap = material.UseAOMap = material.UseEmissiveMap = 1;
                    const u32 first = ((i + phase) & 1u) * 5u;
                    material.HeapOffsets[0] = { offsets[first], offsets[first + 1u], offsets[first + 2u], offsets[first + 3u] };
                    material.HeapOffsets[1].x = offsets[first + 4u];
                    materialUBO->SetData(&material, sizeof(material)); materialUBO->Bind();
                    input->Upload(std::span{ &instances[i], 1 }); input->Bind();
                    api.DrawBoundIndexed(RHI::PrimitiveTopology::TriangleList, 6u, RHI::IndexType::UInt32, 0u);
                }
            }
            framebuffer->Unbind(); });
        EXPECT_EQ(api.GetDroppedDrawsThisRecording(), 0u);
        EXPECT_EQ(api.GetPreparedDrawsThisRecording(), merged ? 1u : 2u);
        EXPECT_EQ(api.GetGpuWrittenRootDrawsThisRecording(), merged ? 1u : 0u);
        auto* vkFramebuffer = static_cast<VulkanFramebuffer*>(framebuffer.Raw());
        for (u32 attachment = 0; attachment < 6u; ++attachment)
            ASSERT_TRUE(vkFramebuffer->GetColorAttachmentImage(attachment)->GetData(images[attachment], 0u));
    };

    Images reference, merged, switchedReference, switched, stale, outOfRange;
    capture(false, 0u, 0u, reference);
    capture(true, 0u, 0u, merged);
    for (u32 attachment = 0; attachment < 6u; ++attachment)
        EXPECT_EQ(merged[attachment], reference[attachment]) << "MRT " << attachment;
    capture(false, 1u, 0u, switchedReference);
    capture(true, 1u, 0u, switched);
    for (u32 attachment = 0; attachment < 6u; ++attachment)
        EXPECT_EQ(switched[attachment], switchedReference[attachment]) << "swapped MRT " << attachment;
    EXPECT_NE(merged[0], switched[0]) << "selection control must visibly change the texture";
    capture(true, 0u, 1u, stale);
    capture(true, 0u, 2u, outOfRange);
    EXPECT_EQ(stale[0], outOfRange[0]);
    const auto pixel = [](const std::vector<u8>& rgba, u32 x)
    {
        const sizet offset = (kSize / 2u * kSize + x) * 4u;
        return std::array<u8, 3>{ rgba.at(offset), rgba.at(offset + 1u), rgba.at(offset + 2u) };
    };
    EXPECT_EQ(pixel(merged[0], kSize / 4u), (std::array<u8, 3>{ 255, 0, 0 }));
    EXPECT_EQ(pixel(merged[0], kSize * 3u / 4u), (std::array<u8, 3>{ 0, 255, 0 }));
    EXPECT_EQ(pixel(stale[0], kSize / 4u), (std::array<u8, 3>{ 255, 0, 255 }));
    // OLO_TEMP_DIR_OK: persistent visual evidence, outside cleanup, with a process-unique leaf.
    const auto evidenceDir = std::filesystem::temp_directory_path() / "olo-805-evidence" / OloEngine::Tests::TempRoot().filename();
    std::filesystem::create_directories(evidenceDir);
    for (const auto& [name, images] : std::array<std::pair<const char*, const Images*>, 4>{
             { { "reference", &reference }, { "merged", &merged }, { "switched", &switched }, { "stale", &stale } } })
    {
        const auto path = (evidenceDir / (std::string(name) + ".png")).string();
        EXPECT_NE(stbi_write_png(path.c_str(), kSize, kSize, 4, (*images)[0].data(), kSize * 4u), 0);
    }
    RecordProperty("ReferenceDraws", 2);
    RecordProperty("IndirectDraws", 1);
    RecordProperty("MaterialTextureBinds", 0);
    RecordProperty("EvidenceDirectory", evidenceDir.string());

    // Reload keeps the RHI identity but replaces its VkImage/descriptor. The
    // table must re-resolve it even when GPU Scene records did not change.
    const auto identity = textures[0]->GetRHIHandle();
    const std::array<u8, 4> blue{ 0, 0, 255, 255 };
    textures[0]->Invalidate("material-selection-805", 1u, 1u, blue.data(), 4u);
    ASSERT_EQ(textures[0]->GetRHIHandle(), identity);
    table.Update(scene);
    tableInfo = table.GetAddressAndCount();
    ASSERT_EQ(table.GetUnresolvedCount(), 0u);
    offsets[0] = HeapBinding::ResolveShaderHeapTexture(identity).Value;
    Images reloadedReference, reloaded;
    capture(false, 0u, 0u, reloadedReference);
    capture(true, 0u, 0u, reloaded);
    EXPECT_EQ(reloaded[0], reloadedReference[0]);
    EXPECT_EQ(pixel(reloaded[0], kSize / 4u), (std::array<u8, 3>{ 0, 0, 255 }));

    // A dead texture keeps a valid material: clear only its failed-map flag,
    // so the factor (white) is used instead of sampling the null (black).
    textures[0].Reset();
    table.Update(scene);
    tableInfo = table.GetAddressAndCount();
    EXPECT_EQ(table.GetUnresolvedCount(), 1u);
    Images missingMap;
    capture(true, 0u, 0u, missingMap);
    EXPECT_EQ(pixel(missingMap[0], kSize / 4u), (std::array<u8, 3>{ 255, 255, 255 }));
}

#endif // OLO_WITH_VULKAN
