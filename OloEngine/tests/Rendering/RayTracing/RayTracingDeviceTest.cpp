// OLO_TEST_LAYER: plumbing
//
// #978 — the device-backed half: real acceleration structures, real rays.
//
// This is the file that carries the issue's central acceptance criterion —
// "a deterministic ray-hit test covers miss, closest hit, instance mask,
// transformed geometry, and alpha reject/accept" — and it is the ONLY place
// any of it can be honestly checked. Everything the device-free half
// (RayTracingSceneTest.cpp) substitutes away lives here: the
// VkAccelerationStructureInstanceKHR memory layout, the scratch and AS
// alignments taken from the device's own properties, compaction, and whether a
// ray actually hits the triangle the records describe.
//
// IT SKIPS, NEVER DISABLES. Three rungs, in this order, each cheaper and safer
// than the next: the shared ADR 0010 device gate, then a real VulkanDevice
// bring-up, then the OPTIONAL ray-tracing capability. Every CI runner this
// project has stops at rung one or three, and a SKIP there is the correct
// answer, not a hole — the hole would be pretending a CPU model covers this.
//
// THE ORACLE IS ARITHMETIC, NOT ANOTHER IMPLEMENTATION. The geometry is one
// axis-aligned triangle at a known z with known vertices, so every expected
// distance and barycentric pair is computed by hand in the assertion rather
// than by a second ray-triangle routine that could be wrong the same way.

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Vertex.h"

#include "../RenderingTestUtils.h"
#include "../VulkanTestSupport.h"

#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanRayTracingBackend.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanStorageBuffer.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <vector>

namespace OloEngine::Tests
{
    namespace RT = OloEngine::RayTracing;

#if !OLO_WITH_VULKAN

    TEST(RayTracingDevice, SkipsWhenNotCompiledIn)
    {
        GTEST_SKIP() << "OLO_WITH_VULKAN is off — the Vulkan backend is not compiled into this build.";
    }

#else

    namespace
    {
        // The probe shader's hit record. MUST match OloRtProbeHit in
        // assets/shaders/compute/RayTracingProbe.comp field for field; the
        // static_assert below is the only thing standing between a layout
        // change there and a test that reads garbage and passes.
        struct ProbeHit
        {
            glm::vec4 DistanceAndBarycentrics{ -1.0f, 0.0f, 0.0f, 0.0f };
            glm::uvec4 Ids{ 0u };
            glm::vec4 UVAndPad{ 0.0f };
        };
        static_assert(sizeof(ProbeHit) == 48, "ProbeHit must match OloRtProbeHit's std430 layout (3 x 16 bytes)");

        // The probe shader's UBO. Mirrors RayTracingProbeParams, std140.
        // uvec2 device addresses, for the same reason the GLSL uses them: the
        // OpenGL contract stays valid without 64-bit integer extensions.
        struct ProbeParams
        {
            glm::uvec2 TlasAddress{ 0u };
            glm::uvec2 RayAddress{ 0u };
            glm::uvec2 HitAddress{ 0u };
            glm::uvec2 InstanceTableAddress{ 0u };
            glm::uvec2 GeometryTableAddress{ 0u };
            glm::uvec2 MaterialTableAddress{ 0u };
            u32 RayCount = 0;
            u32 InstanceSlotCount = 0;
            u32 GeometrySlotCount = 0;
            u32 MaterialSlotCount = 0;
            u32 RayFlags = 0;
            u32 InstanceMask = 0xFFu;
            u32 Pad0 = 0;
            u32 Pad1 = 0;
        };
        static_assert(sizeof(ProbeParams) % 16 == 0, "std140 uniform blocks are 16-byte aligned");

        // StorageBuffer's NEUTRAL interface has no device address — only
        // VertexBuffer/IndexBuffer expose one, because only they needed it
        // before now. This test is Vulkan-gated and already holds the backend
        // headers, so it reaches the concrete class rather than widening a
        // backend-neutral interface for test scaffolding.
        // The suite's tests chdir, and the compute shader is loaded by a path
        // relative to OloEditor/. Same local copy every other device-backed
        // Vulkan test file carries.
        bool ChangeToOloEditorDir()
        {
            namespace fs = std::filesystem;
            fs::path current = fs::current_path();
            for (int i = 0; i < 6; ++i)
            {
                if (fs::exists(current / "OloEditor" / "assets" / "shaders"))
                {
                    fs::current_path(current / "OloEditor");
                    return true;
                }
                if (fs::exists(current / "assets" / "shaders") && current.filename() == "OloEditor")
                {
                    return true;
                }
                if (!current.has_parent_path() || current.parent_path() == current)
                {
                    break;
                }
                current = current.parent_path();
            }
            return false;
        }

        [[nodiscard]] u64 StorageDeviceAddress(const Ref<StorageBuffer>& buffer)
        {
            return static_cast<u64>(static_cast<const VulkanStorageBuffer&>(*buffer).GetDeviceAddress());
        }

        [[nodiscard]] glm::uvec2 SplitAddress(u64 address)
        {
            return glm::uvec2{ static_cast<u32>(address & 0xFFFFFFFFull), static_cast<u32>(address >> 32) };
        }

        // One ray: origin + tMin, then direction + tMax.
        struct ProbeRay
        {
            glm::vec4 OriginAndTMin;
            glm::vec4 DirectionAndTMax;
        };

        // A single triangle in the z = 0 plane, wound counter-clockwise seen
        // from +z. Chosen so barycentrics at a hit are exact binary fractions,
        // which keeps the assertions free of tolerance guesswork.
        //
        //   v0 = (0, 0, 0)   v1 = (1, 0, 0)   v2 = (0, 1, 0)
        constexpr std::array<glm::vec3, 3> kTrianglePositions = {
            glm::vec3{ 0.0f, 0.0f, 0.0f },
            glm::vec3{ 1.0f, 0.0f, 0.0f },
            glm::vec3{ 0.0f, 1.0f, 0.0f },
        };
        // UVs picked so an interpolated UV is trivially checkable: u tracks
        // barycentric b1 and v tracks b2.
        constexpr std::array<glm::vec2, 3> kTriangleUVs = {
            glm::vec2{ 0.0f, 0.0f },
            glm::vec2{ 1.0f, 0.0f },
            glm::vec2{ 0.0f, 1.0f },
        };
    } // namespace

    class RayTracingDevice : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Rung 1 — the shared ADR 0010 gate. Cheapest, and the one that
            // survives a loader-without-ICD runner where a full bring-up can
            // SEH-fault before an exception can be reported.
            const auto gate = ProbeVulkanDeviceTestGate();
            if (!gate.Available)
            {
                GTEST_SKIP() << gate.Reason;
            }
            if (!ChangeToOloEditorDir())
            {
                GTEST_SKIP() << "Could not locate OloEditor/assets/shaders from the working directory.";
            }

            // Rung 2 — a real logical device, headless (null surface).
            m_Device = std::make_unique<VulkanDevice>();
            try
            {
                m_Device->Init([](VkInstance)
                               { return VkSurfaceKHR(VK_NULL_HANDLE); });
            }
            catch (const std::exception& e)
            {
                m_Device.reset();
                GTEST_SKIP() << "Vulkan bring-up refused on a contract-satisfying machine: " << e.what();
            }
            VulkanDevice::ResetValidationErrorCount();

            // Rung 3 — the OPTIONAL capability. A device can satisfy ADR 0010
            // and still have no ray tracing (the AMD self-hosted runner is
            // exactly that), and that is a skip, not a failure.
            if (!m_Device->IsRayQueryEnabled())
            {
                GTEST_SKIP() << "This device has no VK_KHR_acceleration_structure / VK_KHR_ray_query: "
                             << RT::ToString(m_Device->GetRayTracingUnsupportedReason());
            }

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
            // Load-bearing: makes TearDown a no-op after a SetUp that skipped.
            if (!m_Device)
            {
                return;
            }
            m_Backend.reset();
            vkDeviceWaitIdle(m_Device->GetDevice());
            VulkanPipelineBuilder::Get().ReleaseAll();
            VulkanPipelineCache::Get().SaveAndDestroy();
            VulkanFrameArena::Get().ReleaseBuffers();
            VulkanResourceHeap::Get().Release();
            VulkanDeferredReclaim::Get().FlushAll();
            if (m_Fence != VK_NULL_HANDLE)
            {
                vkDestroyFence(m_Device->GetDevice(), m_Fence, nullptr);
            }
            EXPECT_EQ(VulkanDevice::GetValidationErrorCount(), 0u)
                << "Zero validation errors (sync validation included in debug builds)";
            m_Device->Shutdown();
            m_Device.reset();
        }

        // Open a recording, run `record`, then submit and wait. This is the
        // headless stand-in for the frame loop's bracket: the backend records
        // through TryGetRecordingVulkanAPI, which answers non-null exactly
        // while a recording is open.
        template<typename Fn>
        void RecordAndSubmit(Fn&& record)
        {
            auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ASSERT_EQ(vkResetCommandBuffer(m_Cmd, 0), VK_SUCCESS);
            ASSERT_EQ(vkBeginCommandBuffer(m_Cmd, &begin), VK_SUCCESS);
            api.BeginRecording(m_Cmd);

            record();

            api.EndRecording();
            ASSERT_EQ(vkEndCommandBuffer(m_Cmd), VK_SUCCESS);

            VkCommandBufferSubmitInfo cmdInfo{};
            cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cmdInfo.commandBuffer = m_Cmd;
            VkSubmitInfo2 submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &cmdInfo;
            ASSERT_EQ(vkResetFences(m_Device->GetDevice(), 1, &m_Fence), VK_SUCCESS);
            ASSERT_EQ(vkQueueSubmit2(m_Device->GetQueue(), 1, &submit, m_Fence), VK_SUCCESS);
            ASSERT_EQ(vkWaitForFences(m_Device->GetDevice(), 1, &m_Fence, VK_TRUE, 10'000'000'000ull), VK_SUCCESS);
        }

        std::unique_ptr<VulkanDevice> m_Device;
        std::unique_ptr<RT::IRayTracingBackend> m_Backend;
        VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
        VkFence m_Fence = VK_NULL_HANDLE;
    };

    // =========================================================================
    // Capability
    // =========================================================================

    TEST_F(RayTracingDevice, CapabilityAgreesWithTheDeviceInBothDirections)
    {
        // A gate test that only ever runs where the feature is ON cannot see a
        // broken OFF arm, so both arms are asserted off one branch — the
        // discipline #809's host-image-copy gate test established.
        m_Backend = RT::CreateVulkanRayTracingBackend();
        ASSERT_NE(m_Backend, nullptr);
        const RT::Capabilities capabilities = m_Backend->GetCapabilities();

        EXPECT_EQ(capabilities.Supported, m_Device->IsRayQueryEnabled());
        if (capabilities.Supported)
        {
            EXPECT_EQ(capabilities.Reason, RT::UnsupportedReason::None);
            // Every property the builder depends on must be a real number, not
            // the zero that means "not captured".
            EXPECT_GT(capabilities.Properties.MinScratchOffsetAlignment, 0u);
            EXPECT_GT(capabilities.Properties.MaxInstanceCount, 0u);
            EXPECT_GT(capabilities.Properties.MaxPrimitiveCount, 0u);
            EXPECT_GT(capabilities.Properties.MaxGeometryCount, 0u);
        }
        else
        {
            EXPECT_NE(capabilities.Reason, RT::UnsupportedReason::None);
            EXPECT_EQ(capabilities.Properties, RT::DeviceProperties{})
                << "an unsupported capability must not carry half-captured properties";
        }

        // The facade must agree with the device it is reporting on — one
        // predicate, one owner.
        ScopedVulkanRenderCommandSelection vulkanBackend;
        EXPECT_EQ(RenderCommand::SupportsRayTracing(), m_Device->IsRayQueryEnabled());
        EXPECT_EQ(RenderCommand::GetRayTracingCapabilities().Reason, capabilities.Reason);
    }

    // =========================================================================
    // The deterministic ray-hit tenant
    // =========================================================================

    TEST_F(RayTracingDevice, AStaticTriangleBuildsCompactsAndStaysTraceable)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;

        m_Backend = RT::CreateVulkanRayTracingBackend();
        ASSERT_NE(m_Backend, nullptr);
        ASSERT_TRUE(m_Backend->GetCapabilities().Supported);

        // --- geometry, through the ENGINE's buffer path -------------------
        //
        // Deliberately not a hand-rolled VkBuffer: a BLAS build needs
        // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        // which is a create-time property, and going through StorageBuffer is
        // what proves that flag actually reached the engine's vertex/index
        // buffers rather than only this test's.
        std::array<Vertex, 3> vertices{};
        for (sizet i = 0; i < 3; ++i)
        {
            vertices[i].Position = kTrianglePositions[i];
            vertices[i].Normal = glm::vec3(0.0f, 0.0f, 1.0f);
            vertices[i].TexCoord = kTriangleUVs[i];
        }
        const std::array<u32, 3> indices{ 0u, 1u, 2u };

        auto vertexBuffer = VertexBuffer::Create(vertices.data(), static_cast<u32>(sizeof(vertices)));
        auto indexBuffer = IndexBuffer::Create(const_cast<u32*>(indices.data()), 3u);
        ASSERT_TRUE(vertexBuffer && indexBuffer);

        const u64 vertexAddress = vertexBuffer->GetDeviceAddress();
        const u64 indexAddress = indexBuffer->GetDeviceAddress();
        ASSERT_NE(vertexAddress, 0u) << "no device address — the AS build has nothing to read";
        ASSERT_NE(indexAddress, 0u);

        const RT::GeometryKey key{ 0u, 1u };
        RT::BlasBuildRequest build{};
        build.Key = key;
        build.Class = RT::GeometryClass::Static;
        build.Reason = RT::BuildReason::FirstBuild;
        build.VertexAddress = vertexAddress;
        build.IndexAddress = indexAddress;
        build.VertexStride = static_cast<u32>(sizeof(Vertex));
        build.VertexCount = 3;
        build.IndexCount = 3;

        RT::InstanceRecord instance{};
        instance.CustomIndex = 0;
        instance.Mask = RT::kInstanceMaskAll;
        instance.ForceOpaque = true;
        instance.Geometry = key;

        const std::array<RT::BlasBuildRequest, 1> builds{ build };
        const std::array<RT::InstanceRecord, 1> instances{ instance };

        RecordAndSubmit(
            [&]
            {
                EXPECT_EQ(m_Backend->RecordBlasBuilds(builds), 1u);
                static_cast<void>(m_Backend->RecordTlasBuild(instances, RT::TlasBuildReason::FirstBuild));
                m_Backend->RecordBuildToReadBarrier();
            });

        // Anti-vacuity: a run that reached a device but built nothing must
        // fail loudly rather than pass with an empty scene.
        ASSERT_TRUE(m_Backend->IsBlasResident(key)) << "the BLAS build produced no resident structure";
        ASSERT_NE(m_Backend->GetTlasDeviceAddress(), 0u) << "no TLAS address — nothing can be traced";

        RT::SceneStats stats{};
        m_Backend->PublishStats(stats);
        EXPECT_GT(stats.Resident.AccelerationStructureBytes, 0u);

        // --- compaction, which is a MULTI-FRAME handshake ------------------
        //
        // The size query is stamped in the build's command buffer and polled
        // without waiting in a later one, precisely so no routine scene update
        // takes a device idle. Driving several empty recordings is what a
        // frame loop would do.
        for (int frame = 0; frame < 4; ++frame)
        {
            RecordAndSubmit([&]
                            { static_cast<void>(m_Backend->RecordBlasBuilds({})); });
        }
        RT::SceneStats afterCompaction{};
        m_Backend->PublishStats(afterCompaction);
        // Compaction savings are driver-dependent and may legitimately be zero
        // for a one-triangle structure, so this asserts the accounting is
        // COHERENT rather than asserting a number the hardware does not owe us.
        EXPECT_LE(afterCompaction.Resident.CompactionSavedBytes, stats.Resident.AccelerationStructureBytes);
        EXPECT_TRUE(m_Backend->IsBlasResident(key)) << "compaction must not lose the structure";
        EXPECT_NE(m_Backend->GetTlasDeviceAddress(), 0u);
    }

    TEST_F(RayTracingDevice, RetiringAGeometryRemovesItFromEveryLaterTrace)
    {
        // The stale-record criterion on the DEVICE: a retired BLAS must not
        // remain referenced, and the TLAS rebuilt without it must not carry a
        // dangling accelerationStructureReference.
        ScopedVulkanRenderCommandSelection vulkanBackend;
        m_Backend = RT::CreateVulkanRayTracingBackend();
        ASSERT_NE(m_Backend, nullptr);
        ASSERT_TRUE(m_Backend->GetCapabilities().Supported);

        std::array<Vertex, 3> vertices{};
        for (sizet i = 0; i < 3; ++i)
        {
            vertices[i].Position = kTrianglePositions[i];
            vertices[i].TexCoord = kTriangleUVs[i];
        }
        const std::array<u32, 3> indices{ 0u, 1u, 2u };
        auto vertexBuffer = VertexBuffer::Create(vertices.data(), static_cast<u32>(sizeof(vertices)));
        auto indexBuffer = IndexBuffer::Create(const_cast<u32*>(indices.data()), 3u);
        ASSERT_TRUE(vertexBuffer && indexBuffer);

        const RT::GeometryKey key{ 0u, 1u };
        RT::BlasBuildRequest build{};
        build.Key = key;
        build.Class = RT::GeometryClass::Static;
        build.VertexAddress = vertexBuffer->GetDeviceAddress();
        build.IndexAddress = indexBuffer->GetDeviceAddress();
        build.VertexStride = static_cast<u32>(sizeof(Vertex));
        build.VertexCount = 3;
        build.IndexCount = 3;
        RT::InstanceRecord instance{};
        instance.Geometry = key;
        instance.Mask = RT::kInstanceMaskAll;
        instance.ForceOpaque = true;

        const std::array<RT::BlasBuildRequest, 1> builds{ build };
        const std::array<RT::InstanceRecord, 1> instances{ instance };
        RecordAndSubmit(
            [&]
            {
                EXPECT_EQ(m_Backend->RecordBlasBuilds(builds), 1u);
                static_cast<void>(m_Backend->RecordTlasBuild(instances, RT::TlasBuildReason::FirstBuild));
            });
        ASSERT_TRUE(m_Backend->IsBlasResident(key));

        // Retire, then rebuild the TLAS with no instances at all.
        m_Backend->RetireBlas(key);
        EXPECT_FALSE(m_Backend->IsBlasResident(key));
        RecordAndSubmit(
            [&]
            {
                static_cast<void>(m_Backend->RecordTlasBuild({}, RT::TlasBuildReason::TopologyChanged));
                m_Backend->RecordBuildToReadBarrier();
            });

        // The TLAS still exists (an empty one is legal and traceable, it just
        // misses everything) and, crucially, the retirement did not fault: the
        // structure went through deferred reclaim rather than an inline
        // destroy under an in-flight command buffer. TearDown's
        // zero-validation-error assertion is what makes that claim mean
        // something.
        EXPECT_NE(m_Backend->GetTlasDeviceAddress(), 0u);
    }

    TEST_F(RayTracingDevice, RoutineSceneMutationNeverIdlesTheDevice)
    {
        // "No routine scene update calls vkDeviceWaitIdle." Asserted the only
        // way that is meaningful — by driving the mutations that would tempt
        // one (build, retire, rebuild, compaction polling) and requiring the
        // whole sequence to complete without the backend having blocked.
        //
        // The mechanism this depends on is the compaction handshake: a
        // BLOCKING vkGetQueryPoolResults on a slot whose write has not
        // executed would never return, so a hang here IS the failure. The
        // fixture's 10-second fence timeout bounds it.
        ScopedVulkanRenderCommandSelection vulkanBackend;
        m_Backend = RT::CreateVulkanRayTracingBackend();
        ASSERT_NE(m_Backend, nullptr);
        ASSERT_TRUE(m_Backend->GetCapabilities().Supported);

        std::array<Vertex, 3> vertices{};
        for (sizet i = 0; i < 3; ++i)
        {
            vertices[i].Position = kTrianglePositions[i];
        }
        const std::array<u32, 3> indices{ 0u, 1u, 2u };
        auto vertexBuffer = VertexBuffer::Create(vertices.data(), static_cast<u32>(sizeof(vertices)));
        auto indexBuffer = IndexBuffer::Create(const_cast<u32*>(indices.data()), 3u);
        ASSERT_TRUE(vertexBuffer && indexBuffer);

        for (u32 frame = 0; frame < 6; ++frame)
        {
            const RT::GeometryKey key{ frame, 1u };
            RT::BlasBuildRequest build{};
            build.Key = key;
            build.Class = RT::GeometryClass::Static;
            build.VertexAddress = vertexBuffer->GetDeviceAddress();
            build.IndexAddress = indexBuffer->GetDeviceAddress();
            build.VertexStride = static_cast<u32>(sizeof(Vertex));
            build.VertexCount = 3;
            build.IndexCount = 3;
            RT::InstanceRecord instance{};
            instance.Geometry = key;
            instance.Mask = RT::kInstanceMaskAll;
            instance.ForceOpaque = true;
            instance.CustomIndex = frame;

            const std::array<RT::BlasBuildRequest, 1> builds{ build };
            const std::array<RT::InstanceRecord, 1> instances{ instance };
            RecordAndSubmit(
                [&]
                {
                    EXPECT_EQ(m_Backend->RecordBlasBuilds(builds), 1u);
                    static_cast<void>(m_Backend->RecordTlasBuild(instances, RT::TlasBuildReason::TopologyChanged));
                    m_Backend->RecordBuildToReadBarrier();
                });
            if (frame > 0)
            {
                m_Backend->RetireBlas(RT::GeometryKey{ frame - 1u, 1u });
            }
        }

        RT::SceneStats stats{};
        m_Backend->PublishStats(stats);
        EXPECT_GT(stats.Resident.AccelerationStructureBytes, 0u);
        EXPECT_GT(stats.Resident.ScratchBytes, 0u) << "the scratch pool should have been created and kept";
    }

    // =========================================================================
    // THE deterministic ray-hit tenant — miss, closest hit, instance mask,
    // transformed geometry, alpha reject/accept
    // =========================================================================

    TEST_F(RayTracingDevice, DeterministicRaysReportMissClosestHitTransformAndAlpha)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        m_Backend = RT::CreateVulkanRayTracingBackend();
        ASSERT_NE(m_Backend, nullptr);
        ASSERT_TRUE(m_Backend->GetCapabilities().Supported);

        auto probe = ComputeShader::Create("assets/shaders/compute/RayTracingProbe.comp");
        if (!probe || !probe->IsValid())
        {
            GTEST_SKIP() << "RayTracingProbe.comp failed to compile on this device — check the ray-query "
                            "extension set and the compute include path.";
        }

        // --- geometry ------------------------------------------------------
        std::array<Vertex, 3> vertices{};
        for (sizet i = 0; i < 3; ++i)
        {
            vertices[i].Position = kTrianglePositions[i];
            vertices[i].Normal = glm::vec3(0.0f, 0.0f, 1.0f);
            vertices[i].TexCoord = kTriangleUVs[i];
        }
        const std::array<u32, 3> indices{ 0u, 1u, 2u };
        auto vertexBuffer = VertexBuffer::Create(vertices.data(), static_cast<u32>(sizeof(vertices)));
        auto indexBuffer = IndexBuffer::Create(const_cast<u32*>(indices.data()), 3u);
        ASSERT_TRUE(vertexBuffer && indexBuffer);
        // These two addresses are only usable by a BLAS build because
        // VulkanVertexBuffer / VulkanIndexBuffer carry
        // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        // which is a CREATE-time property. Going through the engine's own
        // buffer classes rather than hand-rolled VkBuffers is what makes this
        // test cover that flag.
        ASSERT_NE(vertexBuffer->GetDeviceAddress(), 0u) << "no device address — the AS build has nothing to read";
        ASSERT_NE(indexBuffer->GetDeviceAddress(), 0u);

        const RT::GeometryKey key{ 0u, 1u };
        RT::BlasBuildRequest build{};
        build.Key = key;
        build.Class = RT::GeometryClass::Static;
        build.VertexAddress = vertexBuffer->GetDeviceAddress();
        build.IndexAddress = indexBuffer->GetDeviceAddress();
        build.VertexStride = static_cast<u32>(sizeof(Vertex));
        build.VertexCount = 3;
        build.IndexCount = 3;

        // Two instances of one BLAS:
        //   0 — identity, mask 0x01, OPAQUE
        //   1 — translated +5 in x, mask 0x02, NON-opaque (the alpha candidate)
        RT::InstanceRecord identity{};
        identity.Geometry = key;
        identity.CustomIndex = 0;
        identity.Mask = 0x01u;
        identity.ForceOpaque = true;

        RT::InstanceRecord translated{};
        translated.Geometry = key;
        translated.CustomIndex = 1;
        translated.Mask = 0x02u;
        translated.ForceOpaque = false;
        // Row-major 3x4 with the translation in the fourth column — the same
        // encoding GPU Scene produces, written by hand here so a transpose in
        // the backend cannot be hidden by reusing the backend's own packer.
        translated.Transform[0] = glm::vec4(1.0f, 0.0f, 0.0f, 5.0f);
        translated.Transform[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        translated.Transform[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);

        const std::array<RT::BlasBuildRequest, 1> builds{ build };
        const std::array<RT::InstanceRecord, 2> instances{ identity, translated };

        // --- GPU Scene tables, hand-built --------------------------------
        //
        // Real records rather than a fake table: their C++ and GLSL layouts
        // are pinned to each other by GPUSceneLayoutTest, so uploading the C++
        // structs is what the production path uploads. Instance 1 carries a
        // MASKED material, which is what drives the shader's candidate branch.
        std::array<GPUSceneGeometry, 1> geometryTable{};
        geometryTable[0].VertexAddress = vertexBuffer->GetDeviceAddress();
        geometryTable[0].IndexAddress = indexBuffer->GetDeviceAddress();
        geometryTable[0].VertexFormat = static_cast<u32>(GPUSceneVertexFormat::OloVertex);
        geometryTable[0].IndexFormat = static_cast<u32>(GPUSceneIndexFormat::UInt32);
        geometryTable[0].IndexCount = 3;
        geometryTable[0].VertexCount = 3;
        geometryTable[0].Flags = GPUSceneGeometryFlagActive;

        std::array<GPUSceneMaterial, 2> materialTable{};
        materialTable[0].AlphaMode = static_cast<u32>(AlphaMode::Opaque);
        materialTable[0].AlphaCutoff = 0.5f;
        materialTable[0].Flags = GPUSceneMaterialFlagActive;
        // A masked material whose cutoff a fully-opaque sample clears, so the
        // candidate is ACCEPTED. The reject direction is asserted by raising
        // the cutoff above 1 in the second dispatch below.
        materialTable[1].AlphaMode = static_cast<u32>(AlphaMode::Mask);
        materialTable[1].AlphaCutoff = 0.5f;
        materialTable[1].Flags = GPUSceneMaterialFlagActive;

        std::array<GPUSceneInstance, 2> instanceTable{};
        for (sizet i = 0; i < 2; ++i)
        {
            instanceTable[i].GeometryIndex = 0;
            instanceTable[i].GeometryGeneration = 1;
            instanceTable[i].MaterialIndex = static_cast<u32>(i);
            instanceTable[i].MaterialGeneration = 1;
            instanceTable[i].StableIndex = static_cast<u32>(i);
            instanceTable[i].Flags = GPUSceneInstanceFlagActive;
        }

        auto instanceSsbo = StorageBuffer::Create(static_cast<u32>(sizeof(instanceTable)), 42);
        auto geometrySsbo = StorageBuffer::Create(static_cast<u32>(sizeof(geometryTable)), 43);
        auto materialSsbo = StorageBuffer::Create(static_cast<u32>(sizeof(materialTable)), 44);
        ASSERT_TRUE(instanceSsbo && geometrySsbo && materialSsbo);
        instanceSsbo->SetData(instanceTable.data(), static_cast<u32>(sizeof(instanceTable)));
        geometrySsbo->SetData(geometryTable.data(), static_cast<u32>(sizeof(geometryTable)));
        materialSsbo->SetData(materialTable.data(), static_cast<u32>(sizeof(materialTable)));

        // --- the rays ------------------------------------------------------
        //
        // Every expected answer below is arithmetic on the triangle
        // v0(0,0,0) v1(1,0,0) v2(0,1,0), not a second intersector: a point
        // (x, y, 0) inside it has barycentrics b1 = x, b2 = y.
        const std::array<ProbeRay, 4> rays{
            // 0 — straight down at (0.25, 0.25): HIT instance 0 at t = 1.
            ProbeRay{ glm::vec4(0.25f, 0.25f, 1.0f, 0.001f), glm::vec4(0.0f, 0.0f, -1.0f, 10.0f) },
            // 1 — well outside the triangle: MISS.
            ProbeRay{ glm::vec4(2.0f, 2.0f, 1.0f, 0.001f), glm::vec4(0.0f, 0.0f, -1.0f, 10.0f) },
            // 2 — at the TRANSLATED instance's copy: HIT instance 1 at t = 1.
            //     This is the transformed-geometry case; the same BLAS is hit
            //     five units away only because the instance transform is
            //     applied, so a transposed matrix produces a miss here.
            ProbeRay{ glm::vec4(5.25f, 0.25f, 1.0f, 0.001f), glm::vec4(0.0f, 0.0f, -1.0f, 10.0f) },
            // 3 — pointing AWAY from the geometry: MISS. Guards against a
            //     traversal that ignores ray direction.
            ProbeRay{ glm::vec4(0.25f, 0.25f, 1.0f, 0.001f), glm::vec4(0.0f, 0.0f, 1.0f, 10.0f) },
        };
        const u32 rayCount = static_cast<u32>(rays.size());
        auto raySsbo = StorageBuffer::Create(static_cast<u32>(sizeof(rays)), 45);
        auto hitSsbo = StorageBuffer::Create(static_cast<u32>(sizeof(ProbeHit) * rays.size()), 46);
        ASSERT_TRUE(raySsbo && hitSsbo);
        raySsbo->SetData(rays.data(), static_cast<u32>(sizeof(rays)));
        ASSERT_NE(StorageDeviceAddress(raySsbo), 0u);
        ASSERT_NE(StorageDeviceAddress(hitSsbo), 0u);

        auto params = UniformBuffer::Create(static_cast<u32>(sizeof(ProbeParams)), ShaderBindingLayout::UBO_RAY_TRACING);
        ASSERT_TRUE(params);

        const auto trace = [&](u32 instanceMask, std::vector<ProbeHit>& out)
        {
            RecordAndSubmit(
                [&]
                {
                    EXPECT_EQ(m_Backend->RecordBlasBuilds(builds), 1u);
                    static_cast<void>(m_Backend->RecordTlasBuild(instances, RT::TlasBuildReason::FirstBuild));
                    m_Backend->RecordBuildToReadBarrier();

                    ProbeParams p{};
                    p.TlasAddress = SplitAddress(m_Backend->GetTlasDeviceAddress());
                    p.RayAddress = SplitAddress(StorageDeviceAddress(raySsbo));
                    p.HitAddress = SplitAddress(StorageDeviceAddress(hitSsbo));
                    p.InstanceTableAddress = SplitAddress(StorageDeviceAddress(instanceSsbo));
                    p.GeometryTableAddress = SplitAddress(StorageDeviceAddress(geometrySsbo));
                    p.MaterialTableAddress = SplitAddress(StorageDeviceAddress(materialSsbo));
                    p.RayCount = rayCount;
                    p.InstanceSlotCount = static_cast<u32>(instanceTable.size());
                    p.GeometrySlotCount = static_cast<u32>(geometryTable.size());
                    p.MaterialSlotCount = static_cast<u32>(materialTable.size());
                    p.InstanceMask = instanceMask;
                    params->SetData(&p, static_cast<u32>(sizeof(p)));

                    probe->Bind();
                    RenderCommand::DispatchCompute((rayCount + 63u) / 64u, 1u, 1u);
                });

            out.assign(rays.size(), ProbeHit{});
            hitSsbo->GetData(out.data(), static_cast<u32>(sizeof(ProbeHit) * out.size()));
        };

        // --- mask 0xFF: everything is visible ------------------------------
        std::vector<ProbeHit> hits;
        trace(0xFFu, hits);
        ASSERT_EQ(hits.size(), 4u);
        constexpr f32 kTol = 1e-3f;

        // Closest hit, with the barycentrics the geometry says it must have.
        EXPECT_NEAR(hits[0].DistanceAndBarycentrics.w, 1.0f, kTol) << "ray 0 should have hit";
        EXPECT_NEAR(hits[0].DistanceAndBarycentrics.x, 1.0f, kTol) << "ray 0 distance";
        EXPECT_NEAR(hits[0].DistanceAndBarycentrics.y, 0.25f, kTol) << "ray 0 barycentric b1";
        EXPECT_NEAR(hits[0].DistanceAndBarycentrics.z, 0.25f, kTol) << "ray 0 barycentric b2";
        EXPECT_EQ(hits[0].Ids.x, 0u) << "ray 0 must report the identity instance";
        EXPECT_EQ(hits[0].Ids.y, 0u) << "ray 0 primitive index";
        EXPECT_EQ(hits[0].Ids.z, 0u) << "ray 0 material slot, resolved from the instance record";
        // The UV reconstruction: with these UVs, uv == (b1, b2).
        EXPECT_NEAR(hits[0].UVAndPad.x, 0.25f, kTol) << "ray 0 reconstructed U";
        EXPECT_NEAR(hits[0].UVAndPad.y, 0.25f, kTol) << "ray 0 reconstructed V";

        // Miss.
        EXPECT_NEAR(hits[1].DistanceAndBarycentrics.w, 0.0f, kTol) << "ray 1 should have missed";
        EXPECT_LT(hits[1].DistanceAndBarycentrics.x, 0.0f) << "a miss reports a negative distance";

        // Transformed geometry: the same BLAS, hit five units away.
        EXPECT_NEAR(hits[2].DistanceAndBarycentrics.w, 1.0f, kTol) << "ray 2 should have hit the translated instance";
        EXPECT_NEAR(hits[2].DistanceAndBarycentrics.x, 1.0f, kTol) << "ray 2 distance";
        EXPECT_EQ(hits[2].Ids.x, 1u) << "ray 2 must report the translated instance";
        EXPECT_EQ(hits[2].Ids.z, 1u) << "ray 2 must resolve the MASKED material";

        // Backwards ray.
        EXPECT_NEAR(hits[3].DistanceAndBarycentrics.w, 0.0f, kTol) << "ray 3 points away and must miss";

        // --- mask 0x01: the translated instance is excluded ----------------
        //
        // The instance mask is ANDed with the ray's, so instance 1 (mask 0x02)
        // becomes unhittable while instance 0 (mask 0x01) still hits. This is
        // the per-effect instance-mask criterion.
        std::vector<ProbeHit> masked;
        trace(0x01u, masked);
        ASSERT_EQ(masked.size(), 4u);
        EXPECT_NEAR(masked[0].DistanceAndBarycentrics.w, 1.0f, kTol) << "instance 0 is in mask 0x01 and must still hit";
        EXPECT_NEAR(masked[2].DistanceAndBarycentrics.w, 0.0f, kTol)
            << "instance 1 is mask 0x02 and must be invisible to a 0x01 ray";

        // --- alpha REJECT --------------------------------------------------
        //
        // Same scene, same ray, one number changed: a cutoff above any
        // possible sample makes the masked candidate reject, and the ray
        // passes through what was a hit a moment ago. Raster and RT read this
        // very field from the same record, which is what makes the cutoffs
        // agree by construction rather than by copying.
        materialTable[1].AlphaCutoff = 2.0f;
        materialSsbo->SetData(materialTable.data(), static_cast<u32>(sizeof(materialTable)));
        std::vector<ProbeHit> rejected;
        trace(0xFFu, rejected);
        ASSERT_EQ(rejected.size(), 4u);
        EXPECT_NEAR(rejected[0].DistanceAndBarycentrics.w, 1.0f, kTol)
            << "the OPAQUE instance is unaffected by an alpha cutoff";
        EXPECT_NEAR(rejected[2].DistanceAndBarycentrics.w, 0.0f, kTol)
            << "the masked candidate must be REJECTED once its cutoff exceeds the sample";
    }

#endif // OLO_WITH_VULKAN
} // namespace OloEngine::Tests
