// OLO_TEST_LAYER: plumbing
// =============================================================================
// VulkanRawBufferStorageBindingTest — issue #1052.
//
// A RAW buffer (RenderCommand::CreateBufferHandle + AllocateBufferStorage) bound
// to an SSBO binding point must reach the shader as a real device address.
//
// It did not. `VulkanRendererAPI::BindStorageBuffer` resolved only
// `VulkanRootObjectRegistry` — the object-backed family — so a raw handle took
// the `entry == nullptr` arm, which SILENTLY called
// `SetStorageBuffer(binding, nullptr)` and returned. The publication site then
// found no occupant and substituted the frame arena's null block.
//
// That substitution is deliberate and its comment is right about what it
// prevents: handing a shader address 0 is a GPU page fault that escalates to
// VK_ERROR_DEVICE_LOST. What it cannot prevent is a read that walks OFF a
// legitimately mapped but tiny block — and that is exactly what virtual
// geometry does, because SSBO_VIRTUAL_INDICES (binding 42) is a raw handle
// (`VirtualMeshRegistry::m_IndexBuffer`, "element-buffer + SSBO arena") and its
// shaders index it as `localIndices[cluster.IndexBase + ...]` with offsets in
// the millions. Every virtual-geometry scene on Vulkan died this way:
//
//     [RHI/Vulkan] 'VirtualClusterRaster_Int64' buffer binding 42 has no
//                  published occupant — null block
//     [Vulkan] DEVICE FAULT REPORT: fault address 0x457fd8000 — READ of invalid address
//     === CRASH: vkQueueSubmit2 failed (VkResult -4) ===
//
// So the contract pinned here is the one that was missing: a raw buffer is a
// legitimate SSBO occupant, it publishes a NON-ZERO address, and retiring its
// storage unstages that address rather than leaving a freed allocation bound.
//
// Device-gated; SKIPs cleanly headless, like the rest of the Vulkan ladder.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if !OLO_WITH_VULKAN

TEST(VulkanRawBufferStorageBinding, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "Built with OLO_WITH_VULKAN=OFF — the Vulkan backend is not compiled in.";
}

#else

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"

#include "VulkanTestSupport.h"

#include <memory>

namespace OloEngine::Tests
{
    namespace
    {
        using OloEngine::Tests::ProbeVulkanDeviceTestGate;
        using OloEngine::Tests::ScopedVulkanRenderCommandSelection;

        // The binding the bug was found on. Any SSBO point would do; using the
        // real one keeps the test pointing at the failure it describes.
        constexpr u32 kBinding = ShaderBindingLayout::SSBO_VIRTUAL_INDICES;
        constexpr u64 kBytes = 64u * 1024u;
    } // namespace

    class VulkanRawBufferStorageBinding : public ::testing::Test
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
            VulkanBindingState::Get().SetStorageBufferAddress(kBinding, 0);
            VulkanBindingState::Get().SetStorageBuffer(kBinding, nullptr);
            VulkanFrameArena::Get().ReleaseBuffers();
            VulkanDeferredReclaim::Get().FlushAll();
            EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
                << "a raw-buffer SSBO bind must raise no validation error "
                   "(SHADER_DEVICE_ADDRESS is a create-time usage bit)";
            m_Device->Shutdown();
            m_Device.reset();
        }

        std::unique_ptr<VulkanDevice> m_Device;
    };

    // The regression. Before #1052 the staged address was 0 and the shader was
    // handed the null block instead.
    TEST_F(VulkanRawBufferStorageBinding, ARawBufferBoundToAnSsboPointPublishesItsAddress)
    {
        ScopedVulkanRenderCommandSelection vulkan;

        const RHI::ResourceHandle handle = RenderCommand::CreateBufferHandle();
        ASSERT_TRUE(handle.IsValid()) << "CreateBufferHandle minted nothing";
        RenderCommand::AllocateBufferStorage(handle, kBytes, RHI::MemoryResidency::DeviceLocal);

        // The registry must have taken an address at all — that needs
        // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT at CREATE time, which the
        // raw family did not set.
        auto* entry = VulkanRawBufferRegistry::Get().Lookup(handle);
        ASSERT_NE(entry, nullptr) << "AllocateBufferStorage did not register the raw buffer";
        EXPECT_NE(entry->Buffer, VK_NULL_HANDLE);
        ASSERT_NE(entry->DeviceAddress, 0u)
            << "no device address for a raw buffer — the usage bit is missing again";

        RenderCommand::BindStorageBuffer(kBinding, handle);

        // The whole point: the binding resolves to THIS buffer, not to nothing.
        EXPECT_EQ(VulkanBindingState::Get().GetStorageBufferAddress(kBinding), entry->DeviceAddress)
            << "a raw buffer bound to an SSBO point did not publish its address — the shader would "
               "read the frame arena's null block and walk off it (issue #1052)";

        // ... and it is NOT masquerading as an object-backed occupant.
        EXPECT_EQ(VulkanBindingState::Get().GetStorageBuffer(kBinding), nullptr);

        RenderCommand::DeleteBuffer(handle);
    }

    // Retiring the storage must unstage the address. A bind point still holding
    // a freed allocation's address is the dangling-pointer version of the same
    // fault.
    TEST_F(VulkanRawBufferStorageBinding, DeletingTheBufferUnstagesItsAddress)
    {
        ScopedVulkanRenderCommandSelection vulkan;

        const RHI::ResourceHandle handle = RenderCommand::CreateBufferHandle();
        ASSERT_TRUE(handle.IsValid());
        RenderCommand::AllocateBufferStorage(handle, kBytes, RHI::MemoryResidency::DeviceLocal);
        RenderCommand::BindStorageBuffer(kBinding, handle);
        ASSERT_NE(VulkanBindingState::Get().GetStorageBufferAddress(kBinding), 0u)
            << "precondition: the address must be staged before this test means anything";

        RenderCommand::DeleteBuffer(handle);

        EXPECT_EQ(VulkanBindingState::Get().GetStorageBufferAddress(kBinding), 0u)
            << "a deleted raw buffer left its address staged — the next publication would hand a "
               "shader a freed allocation";
    }

    // A re-allocate under the same identity (GL's glNamedBufferData orphaning)
    // retires the old storage too, so the stale address must not survive it.
    TEST_F(VulkanRawBufferStorageBinding, ReallocatingUnstagesTheOldAddress)
    {
        ScopedVulkanRenderCommandSelection vulkan;

        const RHI::ResourceHandle handle = RenderCommand::CreateBufferHandle();
        ASSERT_TRUE(handle.IsValid());
        RenderCommand::AllocateBufferStorage(handle, kBytes, RHI::MemoryResidency::DeviceLocal);
        RenderCommand::BindStorageBuffer(kBinding, handle);
        const VkDeviceAddress first = VulkanBindingState::Get().GetStorageBufferAddress(kBinding);
        ASSERT_NE(first, 0u);

        RenderCommand::AllocateBufferStorage(handle, kBytes * 2u, RHI::MemoryResidency::DeviceLocal);

        // Either unstaged, or already re-staged to the NEW storage — never left
        // pointing at the retired allocation.
        const VkDeviceAddress after = VulkanBindingState::Get().GetStorageBufferAddress(kBinding);
        auto* entry = VulkanRawBufferRegistry::Get().Lookup(handle);
        ASSERT_NE(entry, nullptr);
        EXPECT_TRUE(after == 0u || after == entry->DeviceAddress)
            << "the bind point still points at the orphaned allocation";

        RenderCommand::DeleteBuffer(handle);
    }
} // namespace OloEngine::Tests

#endif // OLO_WITH_VULKAN
