// OLO_TEST_LAYER: plumbing
// =============================================================================
// VulkanRawBufferUploadTest — issue #1052, the upload half.
//
// PR #1054 stopped virtual geometry from losing the device on Vulkan. It did
// not make it RENDER, and the reason turned out not to be the draw call at all:
// nothing was ever uploaded into the geometry arenas.
//
// VirtualMeshRegistry::CopyThroughRing stages every page load through a
// persistent-mapped ring (GPUCircularBuffer, #704) and falls back to a direct
// RenderCommand::UploadBufferSubData when the ring did not map. On Vulkan
// BOTH ends of that were `#691` no-op stubs:
//
//   * AllocatePersistentUploadStorage returned nullptr, so
//     GPUCircularBuffer::Create read "mapping failed" and never created a ring;
//   * UploadBufferSubData — the fallback that then ran for EVERY page load —
//     discarded its payload silently.
//
// So the vertex arena and the index arena were empty on Vulkan no matter which
// raster arm ran, which is why even software raster (84-98% of clusters on both
// test scenes) drew nothing. A wide capture showed the ground plane, the
// skybox, and no helmets.
//
// What is pinned here is the contract those two owe, at the RHI facade rather
// than through virtual geometry: a persistent upload mapping is real host-
// coherent memory you can write through, and a sub-range upload lands at its
// offset without touching its neighbours. The index-buffer half of the same
// issue is pinned where it belongs — through a real indexed draw, in
// VulkanPassSuite.VirtualGeometryMdiCountDrawsHandAuthoredClusters.
//
// Device-gated; SKIPs cleanly headless, like the rest of the Vulkan ladder.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanRawBufferUpload, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"

#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"

#include "VulkanTestSupport.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using OloEngine::Tests::ProbeVulkanDeviceTestGate;
        using OloEngine::Tests::ScopedVulkanRenderCommandSelection;

        constexpr u64 kRingBytes = 64u * 1024u;
        constexpr u64 kTargetBytes = 256u;
    } // namespace

    class VulkanRawBufferUpload : public ::testing::Test
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
        }

        void TearDown() override
        {
            if (m_Device == nullptr)
                return;
            VulkanFrameArena::Get().ReleaseBuffers();
            VulkanDeferredReclaim::Get().FlushAll();
            EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
                << "the raw upload paths must raise no validation error";
            m_Device->Shutdown();
            m_Device.reset();
        }

        [[nodiscard]] static VulkanRendererAPI& Api()
        {
            return static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
        }

        std::unique_ptr<VulkanDevice> m_Device;
    };

    // The regression that emptied the arenas. Before #1052 this returned
    // nullptr and every caller degraded to the (equally stubbed) direct upload.
    TEST_F(VulkanRawBufferUpload, PersistentUploadStorageHandsBackAWritableCoherentMapping)
    {
        ScopedVulkanRenderCommandSelection vulkan;
        const u64 stubsBefore = Api().GetUnimplementedStubHitCount();

        const RHI::ResourceHandle handle = RenderCommand::CreateBufferHandle();
        ASSERT_TRUE(handle.IsValid()) << "CreateBufferHandle minted nothing";

        auto* mapped = static_cast<u8*>(RenderCommand::AllocatePersistentUploadStorage(handle, kRingBytes));
        ASSERT_NE(mapped, nullptr)
            << "no persistent mapping — GPUCircularBuffer::Create reads this as 'mapping failed' and every "
               "VirtualMeshRegistry page upload falls back to a direct upload (issue #1052)";

        auto* entry = VulkanRawBufferRegistry::Get().Lookup(handle);
        ASSERT_NE(entry, nullptr) << "the persistent allocation did not register in the raw registry";
        EXPECT_EQ(entry->Mapped, mapped) << "the returned pointer must be the allocation's own mapping";
        EXPECT_EQ(entry->Size, kRingBytes);
        EXPECT_TRUE(entry->Coherent)
            << "GL's mapping is GL_MAP_COHERENT_BIT and the ring memcpys then issues a GPU copy with no flush "
               "anywhere — a non-coherent placement would read stale bytes on the device";

        // Write through it the way GPUCircularBuffer::Reserve's caller does,
        // then read it back through the facade. Coherent memory needs no flush,
        // which is exactly the property asserted above.
        std::vector<u8> pattern(512);
        for (sizet i = 0; i < pattern.size(); ++i)
            pattern[i] = static_cast<u8>((i * 7u + 13u) & 0xFFu);
        std::memcpy(mapped + 1024, pattern.data(), pattern.size());

        std::vector<u8> readBack(pattern.size(), 0u);
        RenderCommand::ReadBufferSubData(handle, 1024, readBack.size(), readBack.data());
        EXPECT_EQ(readBack, pattern) << "a write through the persistent mapping did not survive a read back";

        // UnmapBuffer is a complete lowering, not a stub: the VMA placement is
        // MAPPED for its life, so there is nothing to release — but it must not
        // COUNT as a fall-through either, or the "no stubs on this path"
        // assertion below can never hold once the ring exists.
        RenderCommand::UnmapBuffer(handle);
        RenderCommand::DeleteBuffer(handle);

        EXPECT_EQ(Api().GetUnimplementedStubHitCount(), stubsBefore)
            << "the persistent-upload path must ride real implementations, not stubs";
    }

    // The other end of CopyThroughRing: the direct upload the ring falls back
    // to. It discarded its payload silently, which is what a #691 no-op does.
    TEST_F(VulkanRawBufferUpload, SubDataUploadLandsAtItsOffsetAndLeavesNeighboursAlone)
    {
        ScopedVulkanRenderCommandSelection vulkan;
        const u64 stubsBefore = Api().GetUnimplementedStubHitCount();

        // DeviceToHost so the result is readable without a second copy; the
        // upload path itself does not care about the residency.
        const RHI::ResourceHandle handle = RenderCommand::CreateBufferHandle();
        ASSERT_TRUE(handle.IsValid());
        RenderCommand::AllocateBufferStorage(handle, kTargetBytes, RHI::MemoryResidency::DeviceToHost);

        // Ground the whole range first — VMA does not zero an allocation, so
        // without this the "neighbours untouched" half would assert on garbage.
        const std::vector<u8> ground(kTargetBytes, 0xAAu);
        RenderCommand::UploadBufferSubData(handle, 0, ground.size(), ground.data());

        constexpr u64 kOffset = 64;
        const std::array<u8, 16> payload{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
        RenderCommand::UploadBufferSubData(handle, kOffset, payload.size(), payload.data());

        std::vector<u8> readBack(kTargetBytes, 0u);
        RenderCommand::ReadBufferSubData(handle, 0, readBack.size(), readBack.data());

        for (sizet i = 0; i < payload.size(); ++i)
        {
            EXPECT_EQ(readBack[kOffset + i], payload[i])
                << "the sub-range upload was discarded at byte " << i << " (issue #1052)";
        }
        EXPECT_EQ(readBack[kOffset - 1], 0xAAu) << "the upload wrote before its offset";
        EXPECT_EQ(readBack[kOffset + payload.size()], 0xAAu) << "the upload wrote past its size";

        EXPECT_EQ(Api().GetUnimplementedStubHitCount(), stubsBefore)
            << "UploadBufferSubData must ride a real implementation, not a stub";

        // ... and a write past the end is a caller bug, refused and counted —
        // not a vkCmdCopyBuffer VUID violation. The sibling ReadBufferSubData
        // already draws this line; the write direction owes the same.
        const u64 stubsBeforeOverrun = Api().GetUnimplementedStubHitCount();
        RenderCommand::UploadBufferSubData(handle, kTargetBytes - 4, payload.size(), payload.data());
        EXPECT_GT(Api().GetUnimplementedStubHitCount(), stubsBeforeOverrun)
            << "an out-of-range UploadBufferSubData must be refused loudly, not recorded";

        std::vector<u8> afterOverrun(kTargetBytes, 0u);
        RenderCommand::ReadBufferSubData(handle, 0, afterOverrun.size(), afterOverrun.data());
        EXPECT_EQ(afterOverrun, readBack) << "the refused write must not have touched the buffer";

        RenderCommand::DeleteBuffer(handle);
    }

    // Recording the raw element buffer on a raw VAO must be a real lowering.
    // What it DRAWS is pinned through the real indexed draw in
    // VulkanPassSuite.VirtualGeometryMdiCountDrawsHandAuthoredClusters; what is
    // pinned here is that the entry point no longer falls through, and that an
    // unresolvable handle still refuses LOUDLY rather than being ignored (the
    // silence is what turned #1052's unlowered path into a device loss).
    TEST_F(VulkanRawBufferUpload, RecordingTheRawElementBufferIsNotAStubButAJunkHandleStillIs)
    {
        ScopedVulkanRenderCommandSelection vulkan;

        const RHI::ResourceHandle vao = RenderCommand::CreateVertexArrayHandle();
        ASSERT_TRUE(vao.IsValid());
        const RHI::ResourceHandle indices = RenderCommand::CreateBufferHandle();
        ASSERT_TRUE(indices.IsValid());
        RenderCommand::AllocateBufferStorage(indices, 4096, RHI::MemoryResidency::DeviceLocal);

        const u64 stubsBefore = Api().GetUnimplementedStubHitCount();
        RenderCommand::SetVertexArrayIndexBuffer(vao, indices);
        EXPECT_EQ(Api().GetUnimplementedStubHitCount(), stubsBefore)
            << "SetVertexArrayIndexBuffer on a raw VAO + raw element buffer must not fall through to a stub";

        // Detaching is glVertexArrayElementBuffer(vao, 0), not a failure.
        RenderCommand::SetVertexArrayIndexBuffer(vao, RHI::NullResource);
        EXPECT_EQ(Api().GetUnimplementedStubHitCount(), stubsBefore) << "detaching the element buffer is legal";

        // A vertex array this backend never minted is a caller bug and has to
        // be counted, not swallowed.
        const RHI::ResourceHandle strayVao{ 0xFFFFu, 0xFFFFu };
        RenderCommand::SetVertexArrayIndexBuffer(strayVao, indices);
        EXPECT_GT(Api().GetUnimplementedStubHitCount(), stubsBefore)
            << "an unresolvable vertex array must be refused loudly and counted";

        RenderCommand::DeleteBuffer(indices);
        RenderCommand::DeleteVertexArray(vao);
    }
} // namespace OloEngine::Tests

#endif // OLO_WITH_VULKAN
