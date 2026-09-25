// OLO_TEST_LAYER: plumbing
// =============================================================================
// VulkanCompressedTextureUploadTest — issue #1453: a cooked block-compressed
// texture (.olotex, the asset pack's format) uploads and SAMPLES on Vulkan.
//
// Before #1453, Texture2D::Create(CompressedTextureImage) returned null on this
// backend, so every cooked texture was simply missing. The upload has the
// classic block-format traps, and each case here is built to hit one:
//
//   - every format the cook emits: BC7 sRGB and linear, BC5, BC4 (which must
//     sample (R, R, R, 1), the engine's contract, via the view swizzle), BC6H
//     unsigned and signed;
//   - a power-of-two chain and a chain whose extents are not multiples of 4,
//     both running down into the 2x2 and 1x1 tail levels that are one partial
//     block each — the copy extent must be the level's texel extent, which is
//     legal only because it reaches the subresource edge;
//   - a SHORTER chain than the extents allow, which is what the cook ships for
//     an alpha cutout (AlphaCoverageMips::CappedLevelCount): the image must
//     hold exactly the cooked levels.
//
// Each level of each chain is a different solid colour, and the draw reads each
// level with textureLod, so a level copied to the wrong mip, at the wrong
// offset or with the wrong extent reads the wrong colour. The fixture's
// teardown requires zero validation errors (sync validation in Debug), which
// covers the copy-extent VUIDs.
//
// Device-gated: SKIPs cleanly where no device satisfies the ADR 0010 contract.
// =============================================================================

// Initialize the HAL before gtest can introduce Windows Yield/MemoryBarrier macros.
#include "OloEnginePCH.h"
#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanCompressedTextureUpload, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCompression.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanTexture.h"

#include "VulkanTestSupport.h"

#include <volk.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
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

    // A fullscreen triangle that reads ONE mip level: u_Lod.x is the level.
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
layout(std140, binding = 3) uniform LodBlock
{
    vec4 u_Lod;
};

void main()
{
    o_Color = textureLod(u_Texture, v_TexCoord, u_Lod.x);
}
)";

    // A distinct solid colour per level, far enough apart that reading a
    // neighbouring level is unmistakable.
    [[nodiscard]] std::array<u8, 4> LevelColour(u32 level)
    {
        constexpr std::array<std::array<u8, 4>, 6> kColours = { {
            { 230, 40, 40, 255 },
            { 40, 200, 60, 255 },
            { 50, 70, 220, 255 },
            { 220, 200, 40, 255 },
            { 200, 50, 210, 255 },
            { 40, 210, 210, 255 },
        } };
        return kColours[level % kColours.size()];
    }

    // Builds a cooked chain level by level, each level encoded from its own
    // solid colour, so the chain's CONTENT says which level is which.
    [[nodiscard]] CompressedTextureImage SolidLevelChain(TextureCompressionFormat format, u32 width, u32 height, u32 levels,
                                                         bool srgb)
    {
        CompressedTextureImage chain;
        for (u32 level = 0; level < levels; ++level)
        {
            const u32 w = std::max(1u, width >> level);
            const u32 h = std::max(1u, height >> level);
            const std::array<u8, 4> colour = LevelColour(level);
            CompressedTextureImage one;
            if (format == TextureCompressionFormat::BC6H || format == TextureCompressionFormat::BC6HSigned)
            {
                std::vector<f32> rgb(static_cast<sizet>(w) * h * 3u);
                for (sizet i = 0; i < rgb.size(); i += 3)
                {
                    for (u32 c = 0; c < 3u; ++c)
                        rgb[i + c] = static_cast<f32>(colour[c]) / 255.0f;
                }
                one = TextureCompression::EncodeBC6H(rgb.data(), w, h, 3, format == TextureCompressionFormat::BC6HSigned,
                                                     false);
            }
            else
            {
                std::vector<u8> rgba(static_cast<sizet>(w) * h * 4u);
                for (sizet i = 0; i < rgba.size(); i += 4)
                    std::copy(colour.begin(), colour.end(), rgba.begin() + static_cast<std::ptrdiff_t>(i));
                if (format == TextureCompressionFormat::BC5)
                    one = TextureCompression::EncodeBC5(rgba.data(), w, h, 4, false);
                else if (format == TextureCompressionFormat::BC4)
                    one = TextureCompression::EncodeBC4(rgba.data(), w, h, 4, false);
                else
                    one = TextureCompression::EncodeBC7(rgba.data(), w, h, 4, srgb, false);
            }
            if (!one.IsValid())
                return {};
            if (level == 0u)
            {
                chain = one;
                chain.Mips.Reset();
            }
            chain.Mips.Add(one.Mips[0]);
        }
        chain.Width = width;
        chain.Height = height;
        // Every colour is opaque; EncodeBC7 reports alpha for any 4-channel
        // source, which CompressImageFile corrects from the pixels.
        chain.HasAlpha = false;
        return chain;
    }

    // What the attachment (RGBA8 UNORM) must hold after sampling one level:
    // the CPU decode of that level, through the format's sampling rules.
    [[nodiscard]] std::array<f32, 4> ExpectedSample(const CompressedTextureImage& image, u32 level)
    {
        std::array<f32, 4> out{ 0.0f, 0.0f, 0.0f, 1.0f };
        u32 w = 0;
        u32 h = 0;
        if (image.Format == TextureCompressionFormat::BC6H || image.Format == TextureCompressionFormat::BC6HSigned)
        {
            TArray64<f32> rgba;
            if (!TextureCompression::DecodeToRGBAFloat(image, level, rgba, w, h))
                return out;
            for (u32 c = 0; c < 4u; ++c)
                out[c] = std::clamp(rgba[c], 0.0f, 1.0f) * 255.0f;
            return out;
        }
        TArray64<u8> rgba;
        if (!TextureCompression::DecodeToRGBA8(image, level, rgba, w, h))
            return out;
        for (u32 c = 0; c < 4u; ++c)
        {
            f32 value = static_cast<f32>(rgba[c]) / 255.0f;
            // An sRGB image samples as LINEAR light.
            if (image.SRGB && c < 3u)
                value = value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
            out[c] = value * 255.0f;
        }
        return out;
    }
} // namespace

namespace OloEngine::Tests
{
    class VulkanCompressedTextureUpload : public ::testing::Test
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

    TEST_F(VulkanCompressedTextureUpload, EveryCookedFormatSamplesEachOfItsLevelsOnVulkan)
    {
        ScopedVulkanApiSelection vulkanApi;
        VulkanFrameArena::Get().BeginFrame(0);

        FramebufferSpecification fbSpec;
        fbSpec.Width = 8;
        fbSpec.Height = 8;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        auto framebuffer = Framebuffer::Create(fbSpec);
        ASSERT_NE(framebuffer, nullptr);
        auto* vkFramebuffer = static_cast<VulkanFramebuffer*>(framebuffer.Raw());
        const auto colorHandle = framebuffer->GetColorAttachmentHandle(0);
        ASSERT_TRUE(colorHandle.IsValid());

        const f32 vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
        auto vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
        u32 indices[] = { 0, 1, 2 };
        auto indexBuffer = IndexBuffer::Create(indices, 3);
        auto vertexArray = VertexArray::Create();
        vertexArray->AddVertexBuffer(vertexBuffer);
        vertexArray->SetIndexBuffer(indexBuffer);
        auto lodUbo = UniformBuffer::Create(16, 3);

        auto shader = Ref<VulkanShader>::Create("CompressedLevelSample", kVertexSrc, kFragmentSrc);
        ASSERT_EQ(shader->GetCompilationStatus(), ShaderCompilationStatus::Ready);

        VulkanRendererAPI api;
        const auto sampleLevel = [&](const Ref<Texture2D>& texture, u32 level, TArray64<u8>& outPixels)
        {
            const f32 lod[4] = { static_cast<f32>(level), 0.0f, 0.0f, 0.0f };
            lodUbo->SetData(lod, sizeof(lod));
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
                            api.SetClearColor({ 0.0f, 0.0f, 0.0f, 0.0f });
                            api.Clear();
                            shader->Bind();
                            lodUbo->Bind();
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
        };

        struct Case
        {
            const char* Name;
            TextureCompressionFormat Format;
            bool SRGB;
            u32 Width;
            u32 Height;
            u32 Levels;
        };
        // 20x12 runs 20x12, 10x6, 5x3, 2x1, 1x1: partial blocks at every level
        // and two tail levels smaller than a block. 16x16 is the aligned chain.
        // 64x64 with 3 of its 7 levels is the capped chain a cooked cutout ships.
        const std::array<Case, 10> cases = { {
            { "BC7 sRGB 16x16", TextureCompressionFormat::BC7, true, 16, 16, 5 },
            { "BC7 linear 16x16", TextureCompressionFormat::BC7, false, 16, 16, 5 },
            { "BC7 sRGB 20x12", TextureCompressionFormat::BC7, true, 20, 12, 5 },
            { "BC7 sRGB 64x64 capped", TextureCompressionFormat::BC7, true, 64, 64, 3 },
            { "BC5 20x12", TextureCompressionFormat::BC5, false, 20, 12, 5 },
            { "BC4 16x16", TextureCompressionFormat::BC4, false, 16, 16, 5 },
            { "BC4 20x12", TextureCompressionFormat::BC4, false, 20, 12, 5 },
            { "BC6H 16x16", TextureCompressionFormat::BC6H, false, 16, 16, 5 },
            { "BC6H signed 20x12", TextureCompressionFormat::BC6HSigned, false, 20, 12, 5 },
            { "BC5 64x64 capped", TextureCompressionFormat::BC5, false, 64, 64, 3 },
        } };

        for (const Case& c : cases)
        {
            SCOPED_TRACE(c.Name);
            const CompressedTextureImage image = SolidLevelChain(c.Format, c.Width, c.Height, c.Levels, c.SRGB);
            ASSERT_TRUE(image.IsValid());
            ASSERT_EQ(image.MipLevels(), c.Levels);

            Ref<Texture2D> texture = Texture2D::Create(image);
            ASSERT_NE(texture, nullptr) << "Vulkan returned null for a cooked texture (the pre-#1453 stub)";
            ASSERT_TRUE(texture->IsLoaded());
            EXPECT_EQ(texture->GetMipLevelCount(), c.Levels) << "the image must hold exactly the cooked levels";
            EXPECT_EQ(texture->GetWidth(), c.Width);
            EXPECT_EQ(texture->GetHeight(), c.Height);
            EXPECT_FALSE(texture->HasAlphaChannel()) << "the encoders report no alpha for these opaque sources";

            for (u32 level = 0; level < c.Levels; ++level)
            {
                TArray64<u8> pixels;
                sampleLevel(texture, level, pixels);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                const std::array<f32, 4> expected = ExpectedSample(image, level);
                // The centre pixel; the level is one solid colour, so any texel.
                const sizet centre = (static_cast<sizet>(fbSpec.Height / 2u) * fbSpec.Width + fbSpec.Width / 2u) * 4u;
                for (u32 ch = 0; ch < 4u; ++ch)
                {
                    EXPECT_NEAR(static_cast<f32>(pixels[static_cast<i64>(centre + ch)]), expected[ch], 2.5f)
                        << "level " << level << " channel " << ch;
                }
            }
        }
    }

    TEST_F(VulkanCompressedTextureUpload, BC4SamplesRedInEveryColourChannel)
    {
        // The swizzle is registered with the image and rides on its sampled
        // view descriptions; this pins the registration itself, so a view site
        // that forgets to copy it is caught by the draw test above and a
        // texture that forgets to register it is caught here.
        ScopedVulkanApiSelection vulkanApi;
        const CompressedTextureImage image = SolidLevelChain(TextureCompressionFormat::BC4, 16, 16, 1, false);
        Ref<Texture2D> texture = Texture2D::Create(image);
        ASSERT_TRUE(texture && texture->IsLoaded());
        const auto* vkTexture = static_cast<const VulkanTexture2D*>(texture.get());
        const VulkanImageInfo* info = VulkanImageInfoRegistry::Get().Lookup(vkTexture->GetVkImage());
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->Components.r, VK_COMPONENT_SWIZZLE_R);
        EXPECT_EQ(info->Components.g, VK_COMPONENT_SWIZZLE_R);
        EXPECT_EQ(info->Components.b, VK_COMPONENT_SWIZZLE_R);
        EXPECT_EQ(info->Components.a, VK_COMPONENT_SWIZZLE_ONE);
    }

    TEST_F(VulkanCompressedTextureUpload, AMalformedChainIsRefusedNotUploaded)
    {
        // A level whose byte count does not match its extent would leave a
        // mip unwritten, which on Vulkan samples undefined memory. The texture
        // exists (callers read IsLoaded, not null) and is not loaded.
        ScopedVulkanApiSelection vulkanApi;
        CompressedTextureImage image = SolidLevelChain(TextureCompressionFormat::BC7, 16, 16, 3, true);
        ASSERT_TRUE(image.IsValid());
        image.Mips[1].SetNum(image.Mips[1].Num() - 16);
        Ref<Texture2D> texture = Texture2D::Create(image);
        ASSERT_NE(texture, nullptr);
        EXPECT_FALSE(texture->IsLoaded());
    }

    TEST_F(VulkanCompressedTextureUpload, ACookedCutoutReportsTheAlphaTheCookMeasured)
    {
        // HasAlphaChannel sorts a texture into the transparent pass. For a
        // cooked texture it is the cook's measurement, as on GL: an opaque BC7
        // albedo reported as alpha-bearing would sort as transparent.
        ScopedVulkanApiSelection vulkanApi;
        CompressedTextureImage image = SolidLevelChain(TextureCompressionFormat::BC7, 16, 16, 1, true);
        image.HasAlpha = true;
        Ref<Texture2D> withAlpha = Texture2D::Create(image);
        ASSERT_TRUE(withAlpha && withAlpha->IsLoaded());
        EXPECT_TRUE(withAlpha->HasAlphaChannel());
        image.HasAlpha = false;
        Ref<Texture2D> opaque = Texture2D::Create(image);
        ASSERT_TRUE(opaque && opaque->IsLoaded());
        EXPECT_FALSE(opaque->HasAlphaChannel());
    }
} // namespace OloEngine::Tests

#endif // OLO_WITH_VULKAN
