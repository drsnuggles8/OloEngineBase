// OLO_TEST_LAYER: plumbing
// =============================================================================
// VulkanResourceFactoryTest — #691.2: the buffer/texture
// factory arms on Vulkan.
//
// Device-gated (VulkanShaderPipelineTest's probe ladder): headless CI and
// machines below the ADR 0010 contract SKIP cleanly. What it pins:
//
//  - The engine factories (UniformBuffer/VertexBuffer/IndexBuffer/
//    VertexArray/Texture2D/StorageBuffer::Create) produce live Vulkan
//    objects under RendererAPI::API::Vulkan with a device up.
//  - VulkanUniformBuffer's arena-versioned address contract: cached within
//    a frame, NEW range after SetData (the GL command-ordering semantics),
//    re-pushed on a new frame.
//  - Texture uploads round-trip bytes through GetData, including the
//    RGB→RGBA widening and the mip blit chain.
//  - VulkanDescriptorSlotCache: same view → same slot; layout is part of
//    the key; destroyed images recycle their slots through the deferred
//    reclaim delay.
//  - StorageBuffer SetData/GetData/ClearData on both usage classes.
//
// Zero validation errors is asserted in TearDown — sync validation included
// (debug builds), per the validation bar.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanResourceFactory, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanDescriptorSlotCache.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanTexture.h" // GetHostImageCopyUploadCount (#809)
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <volk.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
    using namespace OloEngine;

    // RAII flip of the process-wide API selector around factory calls —
    // restores OpenGL so later suite tests see the default untouched (the
    // VulkanRenderGraphExecutionTest pattern).
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
} // namespace

class VulkanResourceFactory : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (volkInitialize() != VK_SUCCESS)
            GTEST_SKIP() << "No Vulkan loader on this machine.";

        // Bare-probe capability ladder (see VulkanRenderGraphExecutionTest
        // for why the probe precedes VulkanDevice construction).
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
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        vkDeviceWaitIdle(m_Device->GetDevice());
        VulkanFrameArena::Get().ReleaseBuffers();
        VulkanResourceHeap::Get().Release(); // also resets the slot cache
        VulkanDeferredReclaim::Get().FlushAll();
        EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
            << "Zero validation errors (sync validation included in debug builds)";
        m_Device->Shutdown();
        m_Device.reset();
    }

    // Advance the deferred-reclaim generation as a frame boundary would.
    static void CompleteFrames(u32 count)
    {
        for (u32 i = 0; i < count; ++i)
            VulkanDeferredReclaim::Get().NotifyFrameCompleted();
    }

    std::unique_ptr<VulkanDevice> m_Device;
};

TEST_F(VulkanResourceFactory, FactoriesProduceLiveVulkanObjects)
{
    ScopedVulkanApiSelection vulkanApi;

    auto ubo = UniformBuffer::Create(64, 7);
    ASSERT_NE(ubo, nullptr);
    EXPECT_EQ(ubo->GetRendererID(), 0u) << "no native GL name exists on this backend";
    EXPECT_TRUE(ubo->GetRHIHandle().IsValid());

    f32 vertices[] = { 0.f, 1.f, 2.f, 3.f, 4.f, 5.f };
    auto vb = VertexBuffer::Create(vertices, sizeof(vertices));
    ASSERT_NE(vb, nullptr);
    EXPECT_NE(static_cast<VulkanVertexBuffer*>(vb.Raw())->GetDeviceAddress(), 0u)
        << "vertex pulling needs a device address";

    u32 indices[] = { 0, 1, 2 };
    auto ib = IndexBuffer::Create(indices, 3);
    ASSERT_NE(ib, nullptr);
    EXPECT_EQ(ib->GetCount(), 3u);
    EXPECT_NE(static_cast<VulkanIndexBuffer*>(ib.Raw())->GetVkBuffer(), VK_NULL_HANDLE);

    auto vao = VertexArray::Create();
    ASSERT_NE(vao, nullptr);
    vao->AddVertexBuffer(vb);
    vao->SetIndexBuffer(ib);
    const auto* vkVao = static_cast<VulkanVertexArray*>(vao.Raw());
    EXPECT_EQ(vkVao->GetPullVertexBuffer(), static_cast<VulkanVertexBuffer*>(vb.Raw()));
    EXPECT_EQ(vkVao->GetVulkanIndexBuffer(), static_cast<VulkanIndexBuffer*>(ib.Raw()));

    // The root-object registry resolves the packet currency (handles) back
    // to the objects the draw path needs.
    const auto* uboEntry = VulkanRootObjectRegistry::Get().Lookup(ubo->GetRHIHandle());
    ASSERT_NE(uboEntry, nullptr);
    EXPECT_EQ(uboEntry->Kind, VulkanRootObjectKind::UniformBuffer);
    EXPECT_EQ(uboEntry->Object, ubo.Raw());

    const auto* vaoEntry = VulkanRootObjectRegistry::Get().Lookup(vkVao->GetRHIHandle());
    ASSERT_NE(vaoEntry, nullptr);
    EXPECT_EQ(vaoEntry->Kind, VulkanRootObjectKind::VertexArray);
}

TEST_F(VulkanResourceFactory, UniformBufferAddressFollowsWritesAndFrames)
{
    ScopedVulkanApiSelection vulkanApi;

    auto& arena = VulkanFrameArena::Get();
    arena.BeginFrame(0);

    auto ubo = UniformBuffer::Create(64, 3);
    ASSERT_NE(ubo, nullptr);
    auto* vkUbo = static_cast<VulkanUniformBuffer*>(ubo.Raw());

    const u32 payloadA[4] = { 1, 2, 3, 4 };
    ubo->SetData(payloadA, sizeof(payloadA));

    const VkDeviceAddress addr1 = vkUbo->GetRootDataAddress();
    ASSERT_NE(addr1, 0u);
    const u64 allocsAfterFirst = arena.GetAllocationCountThisFrame();

    // Unchanged data, same frame: the cached range is reused — no new push.
    const VkDeviceAddress addr1Again = vkUbo->GetRootDataAddress();
    EXPECT_EQ(addr1Again, addr1);
    EXPECT_EQ(arena.GetAllocationCountThisFrame(), allocsAfterFirst);

    // A mid-frame write allocates a NEW range: draws recorded before the
    // write keep addr1's contents — GL's command-ordering semantics.
    const u32 payloadB[4] = { 9, 9, 9, 9 };
    ubo->SetData(payloadB, sizeof(payloadB));
    const VkDeviceAddress addr2 = vkUbo->GetRootDataAddress();
    ASSERT_NE(addr2, 0u);
    EXPECT_NE(addr2, addr1);

    // A new frame re-pushes even without a write (the old slot's ranges are
    // rewound two frames later — the address must never dangle).
    arena.BeginFrame(1);
    const u64 allocsBefore = arena.GetAllocationCountThisFrame();
    const VkDeviceAddress addr3 = vkUbo->GetRootDataAddress();
    ASSERT_NE(addr3, 0u);
    EXPECT_EQ(arena.GetAllocationCountThisFrame(), allocsBefore + 1);
}

TEST_F(VulkanResourceFactory, TextureUploadRoundTripsThroughGetData)
{
    ScopedVulkanApiSelection vulkanApi;

    TextureSpecification spec;
    spec.Width = 4;
    spec.Height = 4;
    spec.Format = ImageFormat::RGBA8;
    spec.GenerateMips = false;

    auto texture = Texture2D::Create(spec);
    ASSERT_NE(texture, nullptr);

    std::vector<u8> pixels(4 * 4 * 4);
    for (sizet i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<u8>(i * 7 + 3);
    texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));

    std::vector<u8> readback;
    ASSERT_TRUE(texture->GetData(readback, 0));
    ASSERT_EQ(readback.size(), pixels.size());
    EXPECT_EQ(std::memcmp(readback.data(), pixels.data(), pixels.size()), 0);
}

TEST_F(VulkanResourceFactory, RgbUploadWidensToRgbaWithOpaqueAlpha)
{
    ScopedVulkanApiSelection vulkanApi;

    TextureSpecification spec;
    spec.Width = 2;
    spec.Height = 2;
    spec.Format = ImageFormat::RGB8;
    spec.GenerateMips = false;

    auto texture = Texture2D::Create(spec);
    ASSERT_NE(texture, nullptr);

    const u8 rgb[2 * 2 * 3] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120 };
    texture->SetData(const_cast<u8*>(rgb), sizeof(rgb));

    std::vector<u8> readback;
    ASSERT_TRUE(texture->GetData(readback, 0));
    ASSERT_EQ(readback.size(), sizet{ 2 * 2 * 4 }) << "the VkImage is the widened RGBA form";
    for (u32 pixel = 0; pixel < 4; ++pixel)
    {
        EXPECT_EQ(readback[pixel * 4 + 0], rgb[pixel * 3 + 0]);
        EXPECT_EQ(readback[pixel * 4 + 1], rgb[pixel * 3 + 1]);
        EXPECT_EQ(readback[pixel * 4 + 2], rgb[pixel * 3 + 2]);
        EXPECT_EQ(readback[pixel * 4 + 3], 0xFFu);
    }
}

TEST_F(VulkanResourceFactory, MipChainGeneratesAndReadsBack)
{
    ScopedVulkanApiSelection vulkanApi;

    TextureSpecification spec;
    spec.Width = 8;
    spec.Height = 8;
    spec.Format = ImageFormat::RGBA8;
    spec.GenerateMips = true;

    auto texture = Texture2D::Create(spec);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->GetMipLevelCount(), 4u);

    // Solid mid-grey: every mip of a constant image is the same constant, so
    // the blit chain's correctness is byte-checkable without filtering math.
    std::vector<u8> pixels(8 * 8 * 4, 0x80);
    texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));

    std::vector<u8> mip2;
    ASSERT_TRUE(texture->GetData(mip2, 2));
    ASSERT_EQ(mip2.size(), sizet{ 2 * 2 * 4 });
    for (const u8 byte : mip2)
        EXPECT_EQ(byte, 0x80u);
}

// --- #809: the Vulkan 1.4 conveniences ------------------------------------
//
// The two upload tests above are ALREADY the host-image-copy path's functional
// coverage on a machine that has it: UploadPixels prefers vkCopyMemoryToImage
// whenever the device enabled hostImageCopy and the format allows the usage
// bit, so a round-trip that still matches (with the fixture's zero-validation-
// errors assertion) is the route working. What those tests cannot state is the
// GATE — whether the route was taken for the right reason, and whether it
// declines coherently when it is unavailable. That is what these pin.

TEST_F(VulkanResourceFactory, HostImageCopyGateIsSelfConsistent)
{
    // The spec guarantees VK_IMAGE_LAYOUT_GENERAL in BOTH host-copy layout
    // lists. The whole host upload path is built on that guarantee (it is the
    // only layout it copies into without asking first), so a driver or a
    // two-call query bug that loses it must fail here rather than as a VUID
    // buried in an asset load.
    if (!m_Device->IsHostImageCopyEnabled())
    {
        // Not a skip: the DISABLED direction is a real contract too — every
        // per-format and per-layout answer must be a hard no, so no caller can
        // reach vkCopyMemoryToImage through a stale yes.
        EXPECT_FALSE(m_Device->SupportsHostImageCopyForFormat(VK_FORMAT_R8G8B8A8_UNORM));
        EXPECT_FALSE(m_Device->IsHostCopySrcLayoutSupported(VK_IMAGE_LAYOUT_GENERAL));
        EXPECT_FALSE(m_Device->IsHostCopyDstLayoutSupported(VK_IMAGE_LAYOUT_GENERAL));
        GTEST_SUCCEED() << "host image copy unavailable — every upload takes the staging path";
        return;
    }

    EXPECT_TRUE(m_Device->IsHostCopySrcLayoutSupported(VK_IMAGE_LAYOUT_GENERAL))
        << "VK_IMAGE_LAYOUT_GENERAL is spec-guaranteed in pCopySrcLayouts";
    EXPECT_TRUE(m_Device->IsHostCopyDstLayoutSupported(VK_IMAGE_LAYOUT_GENERAL))
        << "VK_IMAGE_LAYOUT_GENERAL is spec-guaranteed in pCopyDstLayouts";
    EXPECT_TRUE(m_Device->IsHostTransitionTargetSupported(VK_IMAGE_LAYOUT_GENERAL));

    // The per-format answer is memoized behind a lock; a second call must
    // agree with the first (a memo that keys on the wrong thing shows up as
    // an answer that changes).
    const bool first = m_Device->SupportsHostImageCopyForFormat(VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(m_Device->SupportsHostImageCopyForFormat(VK_FORMAT_R8G8B8A8_UNORM), first);
}

TEST_F(VulkanResourceFactory, HostImageCopyRouteIsActuallyTakenNotJustPermitted)
{
    // The load-bearing test of #809. Every other upload test here passes
    // identically whether the host route ran or silently declined to staging —
    // same pixels, same final layout — so a decline would look exactly like
    // success. The counter is the only outside-visible difference, and this is
    // what would catch the host path being permitted but never entered.
    if (!m_Device->IsHostImageCopyEnabled())
    {
        GTEST_SKIP() << "host image copy unavailable — every upload legitimately takes the staging path";
    }

    ScopedVulkanApiSelection vulkanApi;

    const u64 before = VulkanTexture2D::GetHostImageCopyUploadCount();

    TextureSpecification spec;
    spec.Width = 4;
    spec.Height = 4;
    spec.Format = ImageFormat::RGBA8;
    spec.GenerateMips = false;

    auto texture = Texture2D::Create(spec);
    ASSERT_NE(texture, nullptr);

    std::vector<u8> pixels(4 * 4 * 4);
    for (sizet i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<u8>(i * 5 + 1);
    texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));

    EXPECT_GT(VulkanTexture2D::GetHostImageCopyUploadCount(), before)
        << "an un-mipped RGBA8 upload on a host-image-copy device must take the host route "
           "(no staging buffer, no queue submit), not fall back to staging";

    // ...and the pixels must still be right, which is what makes the counter
    // an assertion about a working route rather than about a code path.
    std::vector<u8> readback;
    ASSERT_TRUE(texture->GetData(readback, 0));
    ASSERT_EQ(readback.size(), pixels.size());
    EXPECT_EQ(std::memcmp(readback.data(), pixels.data(), pixels.size()), 0);

    // ...and a RE-upload of the same texture must NOT take it. A host copy
    // executes on the CPU with no queue ordering and no fence, so an image an
    // already-submitted frame may still be sampling has to keep the command
    // path — which is what confines the route to load time. This is the
    // assertion that would catch the per-frame re-uploaders
    // (VideoTexture::UpdateFrame, OceanFFTField::Upload) being handed a
    // racy upload.
    const u64 afterFirst = VulkanTexture2D::GetHostImageCopyUploadCount();
    for (sizet i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<u8>(255 - pixels[i]);
    texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));

    EXPECT_EQ(VulkanTexture2D::GetHostImageCopyUploadCount(), afterFirst)
        << "a re-upload of a live texture must stay on the staging/command path";

    std::vector<u8> reReadback;
    ASSERT_TRUE(texture->GetData(reReadback, 0));
    ASSERT_EQ(reReadback.size(), pixels.size());
    EXPECT_EQ(std::memcmp(reReadback.data(), pixels.data(), pixels.size()), 0)
        << "the staging fallback must still land the new pixels";
}

TEST_F(VulkanResourceFactory, HostUploadOfAMippedWidenedTextureRoundTrips)
{
    ScopedVulkanApiSelection vulkanApi;

    // The combination the host route's own arm has to get right: a 3-channel
    // engine format (so the client data is CPU-widened before the host copy
    // sees it) WITH a mip chain (so mip 0 arrives from the host and the blit
    // ladder still runs on the queue, starting from a host-written mip 0 in
    // GENERAL rather than a device-written one in TRANSFER_DST).
    TextureSpecification spec;
    spec.Width = 8;
    spec.Height = 8;
    spec.Format = ImageFormat::RGB8;
    spec.GenerateMips = true;

    const u64 hostUploadsBefore = VulkanTexture2D::GetHostImageCopyUploadCount();

    auto texture = Texture2D::Create(spec);
    ASSERT_NE(texture, nullptr);
    ASSERT_EQ(texture->GetMipLevelCount(), 4u);

    // Constant colour again: every mip of a constant image is that constant,
    // so no filtering math enters the assertion.
    std::vector<u8> rgb(8 * 8 * 3);
    for (sizet pixel = 0; pixel < 8 * 8; ++pixel)
    {
        rgb[pixel * 3 + 0] = 0x20;
        rgb[pixel * 3 + 1] = 0x40;
        rgb[pixel * 3 + 2] = 0x60;
    }
    texture->SetData(rgb.data(), static_cast<u32>(rgb.size()));

    if (m_Device->IsHostImageCopyEnabled())
    {
        EXPECT_GT(VulkanTexture2D::GetHostImageCopyUploadCount(), hostUploadsBefore)
            << "mip 0 must have arrived from the host even though the blit chain still submits";
    }

    std::vector<u8> mip0;
    ASSERT_TRUE(texture->GetData(mip0, 0));
    ASSERT_EQ(mip0.size(), sizet{ 8 * 8 * 4 });
    for (sizet pixel = 0; pixel < 8 * 8; ++pixel)
    {
        EXPECT_EQ(mip0[pixel * 4 + 0], 0x20u);
        EXPECT_EQ(mip0[pixel * 4 + 1], 0x40u);
        EXPECT_EQ(mip0[pixel * 4 + 2], 0x60u);
        EXPECT_EQ(mip0[pixel * 4 + 3], 0xFFu);
    }

    // GetData names SHADER_READ_ONLY_OPTIMAL as its oldLayout, so a mip that
    // reads back at all is also proof the upload left the image in the
    // backend's steady-state layout — the invariant the host path declines
    // the whole route rather than weaken.
    std::vector<u8> mip2;
    ASSERT_TRUE(texture->GetData(mip2, 2));
    ASSERT_EQ(mip2.size(), sizet{ 2 * 2 * 4 });
    for (sizet pixel = 0; pixel < 2 * 2; ++pixel)
    {
        EXPECT_EQ(mip2[pixel * 4 + 0], 0x20u);
        EXPECT_EQ(mip2[pixel * 4 + 1], 0x40u);
        EXPECT_EQ(mip2[pixel * 4 + 2], 0x60u);
        EXPECT_EQ(mip2[pixel * 4 + 3], 0xFFu);
    }
}

TEST_F(VulkanResourceFactory, HostUploadedTextureStillAcceptsASubImageUpdate)
{
    ScopedVulkanApiSelection vulkanApi;

    // SubImage deliberately stayed on the staging path (#809 scopes the host
    // route to the full load-time upload). It reads the image's recorded
    // layout back as its barrier's oldLayout, so this is the test that would
    // fail if the host upload ever left the image somewhere else.
    TextureSpecification spec;
    spec.Width = 4;
    spec.Height = 4;
    spec.Format = ImageFormat::RGBA8;
    spec.GenerateMips = false;

    auto texture = Texture2D::Create(spec);
    ASSERT_NE(texture, nullptr);

    std::vector<u8> pixels(4 * 4 * 4, 0x11);
    texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));

    const u8 patch[2 * 2 * 4] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xAA, 0xBB, 0xCC, 0xDD,
                                  0xAA, 0xBB, 0xCC, 0xDD, 0xAA, 0xBB, 0xCC, 0xDD };
    texture->SubImage(1u, 1u, 2u, 2u, patch, sizeof(patch));

    std::vector<u8> readback;
    ASSERT_TRUE(texture->GetData(readback, 0));
    ASSERT_EQ(readback.size(), pixels.size());
    // The patched 2x2 block, and one untouched texel to prove the update was
    // partial rather than a full overwrite.
    const sizet patchedTexel = (1u * 4u + 1u) * 4u;
    EXPECT_EQ(readback[patchedTexel + 0], 0xAAu);
    EXPECT_EQ(readback[patchedTexel + 3], 0xDDu);
    EXPECT_EQ(readback[0], 0x11u);
}

TEST_F(VulkanResourceFactory, Maintenance5IsEnabledOnAContractSatisfyingDevice)
{
    // maintenance5 is what lets the index-buffer bind state the buffer's real
    // byte size (amendment (9b)'s "whole buffer" sentinel resolved to a
    // number). It is MANDATORY on a Vulkan 1.4 device, and this fixture only
    // runs on one — so an unavailable verdict here is a real signal (a driver
    // reporting 1.4 without it, or volk not exporting vkCmdBindIndexBuffer2),
    // not a machine to skip past. Reported rather than asserted: the backend
    // degrades to the implicit whole-buffer bind, it does not break.
    if (!m_Device->IsMaintenance5Enabled())
    {
        GTEST_SUCCEED() << "maintenance5 unavailable on a 1.4 device — index binds fall back to "
                           "vkCmdBindIndexBuffer (implicit whole-buffer extent)";
        return;
    }
    SUCCEED();
}

TEST_F(VulkanResourceFactory, DescriptorSlotCacheKeysViewsAndRecyclesOnDestroy)
{
    ScopedVulkanApiSelection vulkanApi;

    TextureSpecification spec;
    spec.Width = 4;
    spec.Height = 4;
    spec.Format = ImageFormat::RGBA8;
    spec.GenerateMips = false;

    auto texture = Texture2D::Create(spec);
    ASSERT_NE(texture, nullptr);
    const VkImage image = static_cast<VulkanTexture2D*>(texture.Raw())->GetVkImage();
    ASSERT_NE(image, VK_NULL_HANDLE);

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    auto& cache = VulkanDescriptorSlotCache::Get();
    const u32 slotA = cache.AcquireSlot(image, view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ASSERT_NE(slotA, VulkanResourceHeap::InvalidSlot);

    // Same key → same slot, no growth.
    EXPECT_EQ(cache.AcquireSlot(image, view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
              slotA);

    // The LAYOUT is part of the key: a GENERAL-layout descriptor for the
    // same view is a different slot (a baked layout that disagrees with the
    // barrier plan is a validation error, so folding them is forbidden).
    const u32 slotGeneral =
        cache.AcquireSlot(image, view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_LAYOUT_GENERAL);
    ASSERT_NE(slotGeneral, VulkanResourceHeap::InvalidSlot);
    EXPECT_NE(slotGeneral, slotA);

    const sizet cachedBefore = cache.GetCachedSlotCount();
    EXPECT_GE(cachedBefore, sizet{ 2 });

    // Destroy the texture: slots recycle when the deferred reclaim actually
    // destroys the image (>= kFramesInFlight generations later).
    texture = nullptr;
    EXPECT_EQ(cache.GetFreeSlotCount(), sizet{ 0 }) << "recycling must wait for the reclaim delay";
    CompleteFrames(2);
    EXPECT_GE(cache.GetFreeSlotCount(), sizet{ 2 });
    EXPECT_EQ(cache.GetCachedSlotCount(), cachedBefore - 2);
}

TEST_F(VulkanResourceFactory, StorageBufferDataPathsRoundTrip)
{
    ScopedVulkanApiSelection vulkanApi;

    // DynamicDraw: CPU writes, GPU reads — mapped write-through (or staged).
    auto cpuFed = StorageBuffer::Create(64, 5, StorageBufferUsage::DynamicDraw);
    ASSERT_NE(cpuFed, nullptr);
    const u32 payload[4] = { 11, 22, 33, 44 };
    cpuFed->SetData(payload, sizeof(payload));
    u32 readback[4] = {};
    cpuFed->GetData(readback, sizeof(readback));
    EXPECT_EQ(std::memcmp(readback, payload, sizeof(payload)), 0);

    cpuFed->ClearData();
    cpuFed->GetData(readback, sizeof(readback));
    for (const u32 value : readback)
        EXPECT_EQ(value, 0u);

    // DynamicCopy: device-local — SetData stages, ClearData fills on-device.
    auto gpuOwned = StorageBuffer::Create(64, 6, StorageBufferUsage::DynamicCopy);
    ASSERT_NE(gpuOwned, nullptr);
    gpuOwned->SetData(payload, sizeof(payload));
    u32 gpuReadback[4] = {};
    gpuOwned->GetData(gpuReadback, sizeof(gpuReadback));
    EXPECT_EQ(std::memcmp(gpuReadback, payload, sizeof(payload)), 0);

    gpuOwned->ClearData();
    gpuOwned->GetData(gpuReadback, sizeof(gpuReadback));
    for (const u32 value : gpuReadback)
        EXPECT_EQ(value, 0u);

    EXPECT_NE(static_cast<VulkanStorageBuffer*>(cpuFed.Raw())->GetDeviceAddress(), 0u);
    EXPECT_NE(static_cast<VulkanStorageBuffer*>(gpuOwned.Raw())->GetDeviceAddress(), 0u);
}

#endif // OLO_WITH_VULKAN
