// OLO_TEST_LAYER: plumbing
// =============================================================================
// VulkanTextureInPlaceReloadTest — issue #1078: a Vulkan texture hot-reload
// must refresh the image *in place* and every consumer must SAMPLE the new
// pixels afterwards.
//
// Why this file exists at all, and why it draws instead of reading back:
// `TextureInPlaceReloadTest` (issue #544/#1067) proves the same contract on
// OpenGL with a `Texture2D::Create` -> `Reload()` -> readback round-trip. That
// shape is the substituted seam here (docs/agent-rules/substituted-seams-compound.md):
// `VulkanTexture2D::GetData` copies out of the VkImage directly, so it reports
// byte-exact new pixels even when every DESCRIPTOR that samples the texture
// still describes the destroyed storage. The failure #1078 reports — a surface
// that renders flat red after an otherwise successful reload — is invisible to
// it, and invisible to the validation layers.
//
// So each case here binds the texture through the real facade
// (`VulkanRendererAPI::BindTexture`, i.e. the descriptor slot cache), draws a
// fullscreen triangle that samples it, and reads the ATTACHMENT back. That is
// the seam the editor actually uses.
//
// TWO cases, and the second is the one that matters:
//
//   1. ...ShowsTheNewPixelsAfterReload — the #1078 contract end to end:
//      Reload() refreshes in place and a draw sees the new pixels.
//   2. ASamplerArrayElementReadsItsOwnSlotNotAnAdjacentOne — the DEFECT that
//      made the reload render flat red, isolated from reloading entirely. A
//      sampler ARRAY element used to be resolved by heap-slot ADJACENCY, which
//      nothing guarantees. Case 1 alone does not catch it (a scalar sampler is
//      array element 0, which resolved correctly either way), which is exactly
//      the substituted-seam trap this file's header warns about.
//
// Device-gated: SKIPs cleanly where no device satisfies the ADR 0010 contract.
// =============================================================================

// Initialize the HAL before gtest can introduce Windows Yield/MemoryBarrier macros.
#include "OloEnginePCH.h"
#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanTextureInPlaceReload, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Renderer/AlphaCoverageMips.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanTexture.h"

#include "VulkanTestSupport.h"
#include "TestTempDir.h"

#include <stb_image/stb_image_write.h>

#include <volk.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <system_error>
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

    // Solid fills sidestep every orientation question — the assertions only
    // need the colour to differ before and after the reload. Written with 3
    // channels on purpose: the texture the issue was found on
    // (DriftMenuBackground-v2.png) is RGB, which takes VulkanTexture2D's
    // widen-to-RGBA upload path, and a 4-channel probe would not cover it.
    bool WriteSolidRgbPng(const std::filesystem::path& path, int w, int h, u8 r, u8 g, u8 b)
    {
        TArray64<u8> pixels(static_cast<sizet>(w) * static_cast<sizet>(h) * 3u);
        for (sizet i = 0; i < pixels.Num(); i += 3)
        {
            pixels[i + 0] = r;
            pixels[i + 1] = g;
            pixels[i + 2] = b;
        }
        return ::stbi_write_png(path.string().c_str(), w, h, 3, pixels.GetData(), w * 3) != 0;
    }

    // The minimal sampling pair, copied in shape from VulkanDrawPathTest:
    // vertex pulling from the reserved binding 57, a UBO tint at binding 3, a
    // sampled texture at binding 0. The fullscreen triangle covers every
    // pixel, so the whole target reads tint x texel.
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

    // The ARRAY shape, sampling element 1 — Renderer2D_Quad's declaration in
    // miniature. Element 1 rather than 0 on purpose: element 0 resolves
    // correctly under both the old and the new mapping, so a test that sampled
    // it would pass with the #1078 defect fully present.
    constexpr const char* kArrayFragmentSrc = R"(
#version 460 core
layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

layout(binding = 0) uniform sampler2D u_Textures[4];

void main()
{
    o_Color = texture(u_Textures[1], v_TexCoord);
}
)";

    // Array shapes this backend cannot map. Each must FAIL the shader build
    // rather than quietly binding as a single descriptor — a count of 1 selects
    // the scalar adjacency mapping, which is #1078 reintroduced for exactly the
    // declarations we could not read.
    //
    // Past kMaxBindingArrayCount (256).
    constexpr const char* kOversizedArrayFragmentSrc = R"(
#version 460 core
layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

layout(binding = 0) uniform sampler2D u_Textures[512];

void main()
{
    o_Color = texture(u_Textures[1], v_TexCoord);
}
)";

    // Specialization-constant-sized: spirv-cross reports the CONSTANT'S ID in
    // `type.array`, not a length, and `array_size_literal` is the only tell.
    // Read as a literal it yields a plausible-looking nonsense count.
    constexpr const char* kSpecConstantArrayFragmentSrc = R"(
#version 460 core
layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

layout(constant_id = 0) const int c_Count = 4;
layout(binding = 0) uniform sampler2D u_Textures[c_Count];

void main()
{
    o_Color = texture(u_Textures[1], v_TexCoord);
}
)";
} // namespace

namespace OloEngine::Tests
{
    class VulkanTextureInPlaceReload : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            OLO_VULKAN_DEVICE_OR_SKIP();

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

    // -------------------------------------------------------------------------
    // The #1078 contract: draw sampling the texture, edit the file on disk,
    // Reload(), draw again — the SECOND draw must show the new pixels. Both
    // halves of the bug fail here: the pre-fix inherited refusal (no Reload()
    // override) leaves the first colour on screen, and the naive in-place path
    // leaves a descriptor describing storage that no longer exists.
    // -------------------------------------------------------------------------
    TEST_F(VulkanTextureInPlaceReload, ASampledTextureShowsTheNewPixelsAfterReload)
    {
        ScopedVulkanApiSelection vulkanApi;
        VulkanFrameArena::Get().BeginFrame(0);

        const std::filesystem::path path = OloEngine::Tests::TempFile("olo_vk_texture_inplace_reload_1078.png");
        ASSERT_TRUE(WriteSolidRgbPng(path, 8, 8, 255, 0, 0)) << "failed to write the initial PNG";

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

        auto tintUbo = UniformBuffer::Create(16, 3);
        const f32 white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        tintUbo->SetData(white, sizeof(white));

        auto shader = Ref<VulkanShader>::Create("TextureReloadSample", kVertexSrc, kFragmentSrc);
        ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

        // srgb=false so the sampled value equals the bytes on disk.
        Ref<Texture2D> texture = Texture2D::Create(path.string(), /*srgb=*/false);
        ASSERT_NE(texture, nullptr);
        ASSERT_TRUE(texture->IsLoaded());
        Texture2D* const objectBefore = texture.get();

        VulkanRendererAPI api;
        const auto colorHandle = framebuffer->GetColorAttachmentHandle(0);
        ASSERT_TRUE(colorHandle.IsValid());
        auto* vkFramebuffer = static_cast<VulkanFramebuffer*>(framebuffer.Raw());

        const auto drawAndReadBack = [&](TArray64<u8>& outPixels)
        {
            SubmitFrame(api,
                        [&]()
                        {
                            RHI::Barrier toColor{};
                            toColor.Resource = colorHandle;
                            toColor.Before = RHI::Access::Undefined;
                            toColor.After = RHI::Access::ColorAttachmentWrite;
                            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toColor, 1 });

                            framebuffer->Bind();
                            api.SetViewport(0, 0, fbSpec.Width, fbSpec.Height);
                            api.SetClearColor({ 0.0f, 0.0f, 1.0f, 1.0f });
                            api.Clear();
                            shader->Bind();
                            tintUbo->Bind();
                            api.BindTexture(0, texture->GetRHIHandle());
                            api.DrawIndexed(vertexArray, 3);
                            framebuffer->Unbind();

                            RHI::Barrier toSampled{};
                            toSampled.Resource = colorHandle;
                            toSampled.Before = RHI::Access::ColorAttachmentWrite;
                            toSampled.After = RHI::Access::ShaderSampleRead;
                            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                        });

            const auto attachment = vkFramebuffer->GetColorAttachmentImage(0);
            ASSERT_NE(attachment, nullptr);
            outPixels.Reset();
            ASSERT_TRUE(attachment->GetData(outPixels, 0));
            ASSERT_EQ(outPixels.Num(), static_cast<sizet>(fbSpec.Width) * fbSpec.Height * 4u);
        };

        // Baseline: the pre-edit red must actually reach the attachment,
        // otherwise the post-reload assertion proves nothing.
        {
            TArray64<u8> before;
            drawAndReadBack(before);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            EXPECT_EQ(before[0], 0xFF) << "the probe texture did not sample as red before the reload";
            EXPECT_EQ(before[1], 0x00);
            EXPECT_EQ(before[2], 0x00);
        }

        // What the editor sees when someone saves the .png.
        ASSERT_TRUE(WriteSolidRgbPng(path, 8, 8, 0, 255, 0)) << "failed to overwrite the PNG";

        EXPECT_TRUE(texture->Reload()) << "VulkanTexture2D::Reload refused an in-place refresh of a loose .png";
        // In place: every Ref<Texture2D> a material captured still points here.
        EXPECT_EQ(objectBefore, texture.get());
        EXPECT_TRUE(texture->IsLoaded());

        // Several frames, not one. The pre-edit image is handed to
        // VulkanDeferredReclaim, which destroys it kFramesInFlight generations
        // later — and THAT is when VulkanDescriptorSlotCache::ReleaseSlotsForImage
        // poisons its heap slots and returns them to the free list. A single
        // post-reload frame asserts before any of that has happened, which is
        // the editor's situation only for the first frame after a save.
        for (u32 frame = 0; frame < 6u; ++frame)
        {
            TArray64<u8> after;
            drawAndReadBack(after);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            EXPECT_EQ(after[0], 0x00) << "frame " << frame
                                      << ": the draw still samples the pre-edit red — the reload did not reach the "
                                         "descriptor a consumer samples through";
            EXPECT_EQ(after[1], 0xFF) << "frame " << frame << ": the draw does not sample the new green pixels";
            EXPECT_EQ(after[2], 0x00) << "frame " << frame;
        }

        texture.Reset();
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    // -------------------------------------------------------------------------
    // The defect UNDER #1078, isolated from the reload entirely.
    //
    // `sampler2D u_Textures[N]` is ONE binding with N elements. It used to be
    // mapped through VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_EXT,
    // which derives element i's descriptor as `base + i * heapArrayStride` —
    // i.e. it silently REQUIRES the array's N textures to sit in N consecutive
    // heap slots. Nothing allocates them that way: VulkanDescriptorSlotCache
    // hands out slots from a free list and a bump allocator. A freshly built
    // batch got consecutive slots by luck and rendered correctly, and the first
    // texture to land on a non-adjacent slot made every element but [0] sample
    // an unrelated slot — flat garbage, no validation error, byte-correct
    // pixels in the image itself.
    //
    // This case forces exactly that: bind unit 1 to a texture whose slot is NOT
    // unit 0's slot + 1, by putting a third texture's slot in between. It fails
    // on the pre-fix mapping and passes on INDIRECT_INDEX_ARRAY, which reads
    // each element's index out of the root struct.
    // -------------------------------------------------------------------------
    TEST_F(VulkanTextureInPlaceReload, ASamplerArrayElementReadsItsOwnSlotNotAnAdjacentOne)
    {
        ScopedVulkanApiSelection vulkanApi;
        VulkanFrameArena::Get().BeginFrame(0);

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

        auto shader = Ref<VulkanShader>::Create("SamplerArrayElement", kVertexSrc, kArrayFragmentSrc);
        ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

        const auto makeSolid = [](u8 r, u8 g, u8 b)
        {
            TextureSpecification spec;
            spec.Width = 4;
            spec.Height = 4;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            auto tex = Texture2D::Create(spec);
            TArray64<u8> pixels(4u * 4u * 4u);
            for (sizet i = 0; i < pixels.Num(); i += 4)
            {
                pixels[i + 0] = r;
                pixels[i + 1] = g;
                pixels[i + 2] = b;
                pixels[i + 3] = 0xFF;
            }
            tex->SetData(pixels.GetData(), static_cast<u32>(pixels.Num()));
            return tex;
        };

        auto white = makeSolid(0xFF, 0xFF, 0xFF);
        auto decoy = makeSolid(0xFF, 0x00, 0x00); // takes the slot right after unit 0's
        auto probe = makeSolid(0x00, 0xFF, 0x00);
        ASSERT_TRUE(white && decoy && probe);

        VulkanRendererAPI api;
        const auto colorHandle = framebuffer->GetColorAttachmentHandle(0);
        ASSERT_TRUE(colorHandle.IsValid());
        auto* vkFramebuffer = static_cast<VulkanFramebuffer*>(framebuffer.Raw());

        const auto drawWithUnit1 = [&](const Ref<Texture2D>& unit1, TArray64<u8>& outPixels)
        {
            SubmitFrame(api,
                        [&]()
                        {
                            RHI::Barrier toColor{};
                            toColor.Resource = colorHandle;
                            toColor.Before = RHI::Access::Undefined;
                            toColor.After = RHI::Access::ColorAttachmentWrite;
                            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toColor, 1 });

                            framebuffer->Bind();
                            api.SetViewport(0, 0, fbSpec.Width, fbSpec.Height);
                            api.SetClearColor({ 0.0f, 0.0f, 1.0f, 1.0f });
                            api.Clear();
                            shader->Bind();
                            api.BindTexture(0, white->GetRHIHandle());
                            api.BindTexture(1, unit1->GetRHIHandle());
                            api.DrawIndexed(vertexArray, 3);
                            framebuffer->Unbind();

                            RHI::Barrier toSampled{};
                            toSampled.Resource = colorHandle;
                            toSampled.Before = RHI::Access::ColorAttachmentWrite;
                            toSampled.After = RHI::Access::ShaderSampleRead;
                            api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span{ &toSampled, 1 });
                        });

            const auto attachment = vkFramebuffer->GetColorAttachmentImage(0);
            ASSERT_NE(attachment, nullptr);
            outPixels.Reset();
            ASSERT_TRUE(attachment->GetData(outPixels, 0));
            ASSERT_EQ(outPixels.Num(), static_cast<sizet>(fbSpec.Width) * fbSpec.Height * 4u);
        };

        // Draw once with the decoy at unit 1. This is what claims the slot
        // immediately after unit 0's, so the probe below cannot land on it.
        {
            TArray64<u8> red;
            drawWithUnit1(decoy, red);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            EXPECT_EQ(red[0], 0xFF) << "the decoy did not reach u_Textures[1]";
            EXPECT_EQ(red[1], 0x00);
        }

        // Now the probe, whose slot is NOT unit-0-slot + 1. Under the old
        // mapping the draw keeps sampling the decoy's slot and stays red.
        {
            TArray64<u8> green;
            drawWithUnit1(probe, green);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());

            // ASSERT THE PREMISE, now that the probe is the bound occupant of
            // unit 1. This case only covers #1078 while unit 1's slot is
            // genuinely NOT unit 0's + 1: a different slot-cache allocation
            // order would hand the probe the adjacent slot, and the case would
            // then pass with the defect fully present. Check it rather than
            // assume it — the staged slots are readable.
            {
                const auto& staged = VulkanBindingState::Global();
                const u32 unit0Slot = staged.GetTextureHeapSlot(0);
                const u32 unit1Slot = staged.GetTextureHeapSlot(1);
                ASSERT_NE(unit0Slot, VulkanBindingState::kNoHeapSlot);
                ASSERT_NE(unit1Slot, VulkanBindingState::kNoHeapSlot);
                ASSERT_NE(unit1Slot, unit0Slot + 1u)
                    << "the probe landed on the slot ADJACENT to unit 0 (" << unit0Slot << " -> " << unit1Slot
                    << "), so this case cannot tell the per-element mapping from the old adjacency one — the "
                       "decoy is supposed to claim that slot first.";
            }

            EXPECT_EQ(green[0], 0x00) << "u_Textures[1] sampled the slot ADJACENT to unit 0 instead of the slot "
                                         "actually staged for unit 1 (issue #1078)";
            EXPECT_EQ(green[1], 0xFF) << "u_Textures[1] did not sample the texture bound to unit 1";
            EXPECT_EQ(green[2], 0x00);
        }
    }

    // -------------------------------------------------------------------------
    // Releasing a texture unbinds it: delete implies unbind, as it already did
    // for buffers and framebuffers (VulkanBindingState's header).
    //
    // A texture unit used to keep naming a released image's heap slot until
    // that slot was retired generations later. The next RecordParallel fork
    // transitions every sampled image the global binding state names
    // (TransitionSeededSampledImagesForFork), so a graph transient released by
    // a resize was put into a new command buffer after it had been queued for
    // destruction: VUID-vkDestroyImage-image-01000 on every Deferred -> Forward
    // switch followed by a viewport resize, once the forward screen-space AO
    // work (#1452) left the view-normals export bound at the end of a frame.
    // -------------------------------------------------------------------------
    TEST_F(VulkanTextureInPlaceReload, ReleasingATextureClearsEveryUnitThatNamesIt)
    {
        ScopedVulkanApiSelection vulkanApi;
        VulkanFrameArena::Get().BeginFrame(0);

        FramebufferSpecification fbSpec;
        fbSpec.Width = 8;
        fbSpec.Height = 8;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        auto framebuffer = Framebuffer::Create(fbSpec);
        ASSERT_NE(framebuffer, nullptr);

        const auto makeTexture = []()
        {
            TextureSpecification spec;
            spec.Width = 4;
            spec.Height = 4;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            auto tex = Texture2D::Create(spec);
            TArray64<u8> pixels(4u * 4u * 4u);
            for (sizet i = 0; i < pixels.Num(); ++i)
                pixels[i] = 0x80;
            tex->SetData(pixels.GetData(), static_cast<u32>(pixels.Num()));
            return tex;
        };
        auto kept = makeTexture();
        auto released = makeTexture();
        ASSERT_TRUE(kept && released);

        constexpr u32 kKeptUnit = 3;
        constexpr u32 kReleasedUnit = 4;
        // The released image bound at a SECOND unit too: every unit that names
        // it must be cleared, not just the first one found.
        constexpr u32 kReleasedUnitB = 7;
        VulkanRendererAPI api;
        SubmitFrame(api,
                    [&]()
                    {
                        framebuffer->Bind();
                        api.BindTexture(kKeptUnit, kept->GetRHIHandle());
                        api.BindTexture(kReleasedUnit, released->GetRHIHandle());
                        api.BindTexture(kReleasedUnitB, released->GetRHIHandle());
                        framebuffer->Unbind();
                    });

        const auto& staged = VulkanBindingState::Global();
        // The premise: both units name a slot before anything is released.
        const u32 keptSlot = staged.GetTextureHeapSlot(kKeptUnit);
        ASSERT_NE(keptSlot, VulkanBindingState::kNoHeapSlot);
        ASSERT_NE(staged.GetTextureHeapSlot(kReleasedUnit), VulkanBindingState::kNoHeapSlot);
        ASSERT_NE(staged.GetTextureHeapSlot(kReleasedUnitB), VulkanBindingState::kNoHeapSlot);

        released = nullptr; // the last reference: the image is queued for reclaim

        EXPECT_EQ(staged.GetTextureHeapSlot(kReleasedUnit), VulkanBindingState::kNoHeapSlot)
            << "a unit still names the slot of an image that is queued for destruction; the next fork's "
               "seeded-image transition would record a barrier on it";
        EXPECT_EQ(staged.GetTextureHeapSlot(kReleasedUnitB), VulkanBindingState::kNoHeapSlot)
            << "the second unit naming the released image was left bound";
        EXPECT_EQ(staged.GetTextureHeapSlot(kKeptUnit), keptSlot)
            << "releasing one texture cleared a unit that names a different, live one";
    }

    // -------------------------------------------------------------------------
    // An array shape the backend cannot map must FAIL the shader build.
    //
    // The first version of this fix logged an error and returned a count of 1,
    // which selects the SCALAR mapping — so the shader loaded, rendered, and
    // sampled an unrelated descriptor for every element past [0]. That is #1078
    // reintroduced for precisely the declarations that could not be read, and a
    // loud log does not stop a wrong frame from being drawn. Refusing the build
    // is what makes it unrepresentable.
    //
    // (A runtime-sized `sampler2D u[]` is the third unsupported shape. It needs
    // GL_EXT_nonuniform_qualifier and is rejected by the SPIR-V compile before
    // reflection ever sees it, so there is nothing for this file to assert
    // about it — noted rather than silently omitted.)
    // -------------------------------------------------------------------------
    TEST_F(VulkanTextureInPlaceReload, RefusesToBuildAShaderWhoseArrayShapeCannotBeMapped)
    {
        ScopedVulkanApiSelection vulkanApi;
        VulkanFrameArena::Get().BeginFrame(0);

        {
            auto oversized = Ref<VulkanShader>::Create("OversizedSamplerArray", kVertexSrc,
                                                       kOversizedArrayFragmentSrc);
            EXPECT_NE(oversized->GetCompilationStatus(), ShaderCompilationStatus::Ready)
                << "a sampler array past kMaxBindingArrayCount must not build — bound as a single descriptor "
                   "it silently samples the wrong resource for every element past [0]";
        }

        {
            auto specConstant = Ref<VulkanShader>::Create("SpecConstantSamplerArray", kVertexSrc,
                                                          kSpecConstantArrayFragmentSrc);
            EXPECT_NE(specConstant->GetCompilationStatus(), ShaderCompilationStatus::Ready)
                << "a specialization-constant-sized sampler array must not build — spirv-cross reports the "
                   "constant's ID, not a length, so any count read from it is nonsense";
        }
    }

    // -------------------------------------------------------------------------
    // Issue #1441 on this backend: an alpha-tested texture's chain is built on
    // the CPU and COPIED level by level instead of blitted, and stops at the
    // AlphaCoverageMips level cap. Read back through the image itself and
    // compared byte for byte with the builder the GL twin uploads, so the two
    // backends are held to the same levels. The fixture's zero-validation-error
    // teardown (sync validation in Debug) covers the barriers around the copies.
    // -------------------------------------------------------------------------
    TEST_F(VulkanTextureInPlaceReload, AnAlphaTestedTextureUploadsTheCoverageChainLevelByLevel)
    {
        ScopedVulkanApiSelection vulkanApi;

        constexpr u32 kSize = 256;
        constexpr f32 kCutoff = 0.5f;
        const std::filesystem::path path = OloEngine::Tests::TempFile("olo_vk_alpha_coverage_1441.png");
        TArray64<u8> pixels(static_cast<sizet>(kSize) * kSize * 4u);
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                u32 h = x * 73856093u ^ y * 19349663u;
                h ^= h >> 13;
                h *= 0x5bd1e995u;
                h ^= h >> 15;
                u8* texel = &pixels[(static_cast<i64>(y) * kSize + x) * 4];
                texel[0] = 60u;
                texel[1] = 140u;
                texel[2] = 30u;
                texel[3] = (h % 100u) < 30u ? 255u : 0u;
            }
        }
        ASSERT_NE(::stbi_write_png(path.string().c_str(), kSize, kSize, 4, pixels.GetData(), kSize * 4), 0);

        Ref<Texture2D> texture = Texture2D::Create(path.string(), /*srgb=*/true);
        ASSERT_TRUE(texture && texture->IsLoaded());
        ASSERT_EQ(texture->GetMipLevelCount(), 9u) << "a file texture gets its full chain before any cutoff is set";

        const auto levelBytes = [&](u32 level)
        {
            TArray64<u8> bytes;
            EXPECT_TRUE(texture->GetData(bytes, level)) << "level " << level;
            return bytes;
        };
        const auto coverageOf = [&](const TArray64<u8>& bytes)
        { return AlphaCoverageMips::Coverage({ bytes.GetData(), static_cast<sizet>(bytes.Num()) }, kCutoff); };

        // The control: the blit chain thins this cutout by level 2.
        const TArray64<u8> level0 = levelBytes(0);
        const f32 base = coverageOf(level0);
        EXPECT_LT(coverageOf(levelBytes(2)), 0.5f * base) << "the blit chain no longer thins this cutout; the control is void";

        const RHI::ResourceHandle identity = texture->GetRHIHandle();
        texture->SetAlphaCoverageCutoff(kCutoff);
        EXPECT_EQ(texture->GetRHIHandle(), identity) << "rebuilding the chain must keep the texture's identity";
        ASSERT_EQ(texture->GetMipLevelCount(), AlphaCoverageMips::CappedLevelCount(kSize, kSize, 9u));
        ASSERT_EQ(texture->GetMipLevelCount(), 3u) << "256 -> 128 -> 64";

        const AlphaCoverageMips::Chain expected = AlphaCoverageMips::Build(
            { level0.GetData(), static_cast<sizet>(level0.Num()) }, kSize, kSize, 3u, /*srgb=*/true, kCutoff);
        EXPECT_EQ(levelBytes(0), level0) << "rebuilding the chain changed level 0";
        for (i32 i = 0; i < expected.Levels.Num(); ++i)
        {
            const TArray64<u8> actual = levelBytes(static_cast<u32>(i) + 1u);
            const AlphaCoverageMips::Level& level = expected.Levels[i];
            ASSERT_EQ(static_cast<u64>(actual.Num()), static_cast<u64>(level.Width) * level.Height * 4u);
            i64 mismatches = 0;
            for (i64 b = 0; b < actual.Num(); ++b)
                mismatches += actual[b] != expected.Bytes[static_cast<i64>(level.Offset) + b] ? 1 : 0;
            EXPECT_EQ(mismatches, 0) << "level " << (i + 1) << " is not what the CPU builder (and the GL twin) upload";
            EXPECT_NEAR(coverageOf(actual), base, 0.05f) << "level " << (i + 1);
        }

        // An in-place reload keeps the cutoff and the capped chain.
        ASSERT_TRUE(texture->Reload());
        EXPECT_EQ(texture->GetMipLevelCount(), 3u);
        EXPECT_NEAR(coverageOf(levelBytes(2)), base, 0.05f) << "a reload dropped the coverage-preserving chain";

        // Clearing it restores the full blit chain.
        texture->SetAlphaCoverageCutoff(0.0f);
        EXPECT_EQ(texture->GetMipLevelCount(), 9u);
        EXPECT_LT(coverageOf(levelBytes(2)), 0.5f * base);

        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
} // namespace OloEngine::Tests

#endif // OLO_WITH_VULKAN
