// OLO_TEST_LAYER: plumbing
// =============================================================================
// GpuPathTracerDeviceParityTest.cpp — the GPU reference path tracer against
// the CPU reference path tracer, on a real ray-tracing device (issue #1055).
//
// THE ORACLE HAS TO AGREE WITH THE ORACLE. GpuPathTracer.glsl is written to
// mirror PathTracer.cpp term by term — the same Owen-scrambled Sobol' sampler
// consuming dimensions in the same order, the same NEE + power-heuristic MIS
// structure, the same closure — so the two are compared here on the SAME
// Cornell box, built for both worlds from ONE description
// (ReferenceSceneFixtures.h's MakeCornellBoxScene, whose vertices and
// instances are uploaded verbatim), at the same seed and sample count.
//
// WHAT IS ASSERTED, AND THE DISAGREEMENT BUDGET — written down here rather
// than tuned into. With a bit-identical sampler the two tracers draw the same
// paths; what remains between them is float rounding in a different
// intersector (a hardware BVH against a CPU BVH: hit points and distances
// agree to a few ulps, and a Russian-roulette decision within those ulps of
// its threshold can flip). Those differences are sparse and small, so:
//
//   * region MEANS anchored to world points agree to 2% relative with a
//     0.01-radiance absolute floor at 64 spp on a 64x64 film;
//   * the whole-frame mean agrees to 1%.
//
// A budget wider than that is a bug in one of the two tracers, not a
// tolerance to raise; a Monte Carlo argument does not cover it, because the
// noise the two share cancels in the difference.
//
// The two other properties the issue asks for are asserted the only way they
// can be — by demonstration, not assertion:
//   * DETERMINISM: two draws with identical inputs are byte-identical;
//   * ACCUMULATION: 32 samples then 32 more over the history equal one draw of
//     64, to fp32 summation order, with the count exactly 64 — and a draw with
//     the history withheld restarts at 32. That is the invalidation path the
//     registry takes, exercised on the device.
//
// The device fixture is the RayTracingDeviceTest one — the same skip ladder
// and the same zero-validation-error bar every Vulkan tenant carries. Every
// CI runner skips at rung 1 or 3; the evidence for this test is local.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "ReferenceSceneFixtures.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/EmissiveTriangleTable.h"
#include "OloEngine/Renderer/PathTracing/GpuPathTracerTypes.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include "../RenderingTestUtils.h"

#if OLO_WITH_VULKAN
#include "../VulkanTestSupport.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanRayTracingBackend.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanTexture.h"
#endif

#include <stb_image/stb_image_write.h>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace RT = OloEngine::RayTracing;
    using namespace OloEngine::PathTracing;
    using namespace OloEngine::Tests::PathTracingFixtures;

#if !OLO_WITH_VULKAN
    TEST(GpuPathTracerDeviceParity, SkipsWhenNotCompiledIn)
    {
        GTEST_SKIP() << "OLO_WITH_VULKAN is off — the Vulkan backend is not compiled into this build.";
    }
#else

    namespace
    {
        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;
        constexpr u32 kSamples = 64;
        constexpr u32 kMaxBounces = 6;
        constexpr u32 kRussianRouletteStart = 4;
        constexpr u32 kSeed = 0x709u;

        // The disagreement budget. See the header.
        constexpr f32 kRegionRelativeBudget = 0.02f;
        constexpr f32 kRegionAbsoluteFloor = 0.01f;
        constexpr f32 kFrameRelativeBudget = 0.01f;
        constexpr i32 kRegionRadius = 3;

        // Mirrors RayTracingPathTracerParams (GpuPathTracer.glsl), std140.
        struct PathTracerParams
        {
            glm::mat4 InvViewProjection{ 1.0f };
            glm::vec4 CameraPosition{ 0.0f };
            glm::uvec4 TlasAddress{ 0u };
            glm::uvec4 SlotCounts{ 0u };
            glm::uvec4 EmissiveTable{ 0u };
            glm::uvec4 PathParams{ 0u };
            glm::vec4 RayParams{ 0.0f };
            glm::vec4 Environment{ 0.0f };
            glm::vec4 ScreenParams{ 0.0f };
            glm::vec4 DebugParams{ 0.0f };
        };
        static_assert(sizeof(PathTracerParams) == sizeof(UBOStructures::RayTracingPathTracerUBO));

        // The suite's tests chdir, and the shader is loaded by a path relative
        // to OloEditor/. Same local copy every device-backed Vulkan test carries.
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

        [[nodiscard]] glm::uvec2 SplitAddress(u64 address)
        {
            return glm::uvec2{ static_cast<u32>(address & 0xFFFFFFFFull), static_cast<u32>(address >> 32) };
        }

        // Row-major 3x4 rows from a column-major glm matrix — the GPU Scene's
        // and VkTransformMatrixKHR's encoding.
        [[nodiscard]] std::array<glm::vec4, 3> RowsOf(const glm::mat4& m)
        {
            return { glm::vec4(m[0][0], m[1][0], m[2][0], m[3][0]), glm::vec4(m[0][1], m[1][1], m[2][1], m[3][1]),
                     glm::vec4(m[0][2], m[1][2], m[2][2], m[3][2]) };
        }

        // The GPU twin of a ReferenceScene: the same vertices and indices in
        // engine buffers, one BLAS per geometry, the GPU Scene tables the
        // shader resolves hits through, and the emissive table it samples.
        struct GpuSceneTwin
        {
            std::vector<Ref<VertexBuffer>> VertexBuffers;
            std::vector<Ref<IndexBuffer>> IndexBuffers;
            std::vector<GPUSceneGeometry> Geometries;
            std::vector<GPUSceneMaterial> Materials;
            std::vector<GPUSceneInstance> Instances;
            std::vector<RT::BlasBuildRequest> BlasBuilds;
            std::vector<RT::InstanceRecord> TlasInstances;
            std::vector<EmissiveTriangleRecord> Emissive;
            f32 EmissiveArea = 0.0f;

            Ref<StorageBuffer> InstanceSsbo;
            Ref<StorageBuffer> GeometrySsbo;
            Ref<StorageBuffer> MaterialSsbo;
            Ref<StorageBuffer> LightSsbo;
            Ref<StorageBuffer> EmissiveSsbo;
        };

        [[nodiscard]] bool BuildTwin(const ReferenceScene& scene, GpuSceneTwin& twin)
        {
            const u32 geometryCount = scene.GetGeometryCount();
            for (u32 g = 0; g < geometryCount; ++g)
            {
                const ReferenceGeometry& geometry = scene.GetGeometry(g);
                const auto& vertices = geometry.GetVertices();
                const auto& indices = geometry.GetIndices();
                auto vertexBuffer =
                    VertexBuffer::Create(vertices.data(), static_cast<u32>(vertices.size() * sizeof(Vertex)));
                auto indexBuffer = IndexBuffer::Create(const_cast<u32*>(indices.data()), static_cast<u32>(indices.size()));
                if (!vertexBuffer || !indexBuffer || vertexBuffer->GetDeviceAddress() == 0u ||
                    indexBuffer->GetDeviceAddress() == 0u)
                    return false;

                GPUSceneGeometry record{};
                record.VertexAddress = vertexBuffer->GetDeviceAddress();
                record.IndexAddress = indexBuffer->GetDeviceAddress();
                record.VertexFormat = static_cast<u32>(GPUSceneVertexFormat::OloVertex);
                record.IndexFormat = static_cast<u32>(GPUSceneIndexFormat::UInt32);
                record.FirstIndex = 0;
                record.IndexCount = static_cast<u32>(indices.size());
                record.BaseVertex = 0;
                record.VertexCount = static_cast<u32>(vertices.size());
                record.Generation = 1;
                record.Flags = GPUSceneGeometryFlagActive;
                twin.Geometries.push_back(record);

                RT::BlasBuildRequest build{};
                build.Key = RT::GeometryKey{ g, 1u };
                build.Class = RT::GeometryClass::Static;
                build.VertexAddress = record.VertexAddress;
                build.IndexAddress = record.IndexAddress;
                build.VertexStride = sizeof(Vertex);
                build.VertexCount = record.VertexCount;
                build.FirstIndex = 0;
                build.IndexCount = record.IndexCount;
                build.BaseVertex = 0;
                twin.BlasBuilds.push_back(build);

                twin.VertexBuffers.push_back(vertexBuffer);
                twin.IndexBuffers.push_back(indexBuffer);
            }

            const auto& materials = scene.GetMaterials();
            for (u32 m = 0; m < static_cast<u32>(materials.size()); ++m)
            {
                const ReferenceMaterial& material = materials[m];
                GPUSceneMaterial record{};
                record.BaseColorFactor = glm::vec4(material.BaseColor, 1.0f);
                record.EmissiveFactor = glm::vec4(material.Emissive, 0.0f);
                record.MetallicFactor = material.Metallic;
                record.RoughnessFactor = material.Roughness;
                record.AlphaCutoff = 0.5f;
                record.AlphaMode = static_cast<u32>(AlphaMode::Opaque);
                record.ClosureVersion = static_cast<u32>(material.Model);
                record.Flags = GPUSceneMaterialFlagActive | GPUSceneMaterialFlagPBR |
                               (material.TwoSidedEmission ? GPUSceneMaterialFlagTwoSided : 0u);
                record.StableIndex = m;
                record.Generation = 1;
                twin.Materials.push_back(record);
            }

            const auto& instances = scene.GetInstances();
            for (u32 i = 0; i < static_cast<u32>(instances.size()); ++i)
            {
                const ReferenceInstance& instance = instances[i];
                const auto rows = RowsOf(instance.Transform);

                GPUSceneInstance record{};
                record.CurrentTransform.Row0 = rows[0];
                record.CurrentTransform.Row1 = rows[1];
                record.CurrentTransform.Row2 = rows[2];
                record.PreviousTransform = record.CurrentTransform;
                record.GeometryIndex = instance.GeometryIndex;
                record.GeometryGeneration = 1;
                record.MaterialIndex = instance.MaterialIndex;
                record.MaterialGeneration = 1;
                record.StableIndex = i;
                record.VisibilityMask = ~0u;
                record.Flags = GPUSceneInstanceFlagActive;
                record.Generation = 1;
                twin.Instances.push_back(record);

                RT::InstanceRecord tlasInstance{};
                tlasInstance.Transform = rows;
                tlasInstance.CustomIndex = i;
                tlasInstance.Mask = RT::kInstanceMaskAll;
                tlasInstance.ForceOpaque = true;
                tlasInstance.Geometry = RT::GeometryKey{ instance.GeometryIndex, 1u };
                twin.TlasInstances.push_back(tlasInstance);

                // The emissive table, from the same instances in the same
                // order the reference walks them.
                const ReferenceMaterial& material = materials[instance.MaterialIndex];
                if (std::max({ material.Emissive.x, material.Emissive.y, material.Emissive.z }) > 0.0f)
                {
                    const ReferenceGeometry& geometry = scene.GetGeometry(instance.GeometryIndex);
                    twin.EmissiveArea = EmissiveTriangleTable::AppendTriangles(
                        std::span<const Vertex>(geometry.GetVertices()), std::span<const u32>(geometry.GetIndices()), 0u,
                        static_cast<u32>(geometry.GetIndices().size()), 0, instance.Transform, glm::vec3(0.0f),
                        material.Emissive, material.TwoSidedEmission, twin.EmissiveArea, twin.Emissive);
                }
            }
            EmissiveTriangleTable::Finalize(twin.Emissive, twin.EmissiveArea);

            // The tables at their canonical bindings, exactly what the
            // production pass binds before its draw. The light table is empty
            // (the Cornell box has only its emitter) but the shader declares
            // the block, so a real buffer must sit at the slot.
            twin.InstanceSsbo = StorageBuffer::Create(static_cast<u32>(twin.Instances.size() * sizeof(GPUSceneInstance)),
                                                      GPUSceneBindingLayout::Instances);
            twin.GeometrySsbo = StorageBuffer::Create(static_cast<u32>(twin.Geometries.size() * sizeof(GPUSceneGeometry)),
                                                      GPUSceneBindingLayout::Geometries);
            twin.MaterialSsbo = StorageBuffer::Create(static_cast<u32>(twin.Materials.size() * sizeof(GPUSceneMaterial)),
                                                      GPUSceneBindingLayout::Materials);
            twin.LightSsbo = StorageBuffer::Create(static_cast<u32>(sizeof(GPUSceneLight)), GPUSceneBindingLayout::Lights);
            twin.EmissiveSsbo = StorageBuffer::Create(
                static_cast<u32>(std::max<sizet>(twin.Emissive.size(), 1u) * sizeof(EmissiveTriangleRecord)),
                GPUSceneBindingLayout::Environments);
            if (!twin.InstanceSsbo || !twin.GeometrySsbo || !twin.MaterialSsbo || !twin.LightSsbo || !twin.EmissiveSsbo)
                return false;

            twin.InstanceSsbo->SetData(twin.Instances.data(), static_cast<u32>(twin.Instances.size() * sizeof(GPUSceneInstance)));
            twin.GeometrySsbo->SetData(twin.Geometries.data(), static_cast<u32>(twin.Geometries.size() * sizeof(GPUSceneGeometry)));
            twin.MaterialSsbo->SetData(twin.Materials.data(), static_cast<u32>(twin.Materials.size() * sizeof(GPUSceneMaterial)));
            const GPUSceneLight noLight{};
            twin.LightSsbo->SetData(&noLight, static_cast<u32>(sizeof(GPUSceneLight)));
            if (!twin.Emissive.empty())
                twin.EmissiveSsbo->SetData(twin.Emissive.data(), static_cast<u32>(twin.Emissive.size() * sizeof(EmissiveTriangleRecord)));
            return true;
        }

        // A frame of the tracer: the resolved accumulation planes read back
        // as linear floats, top-down rows (the Vulkan off-screen convention,
        // which is also the CPU film's).
        struct TracedFrame
        {
            std::vector<glm::vec4> Accum; // rgb sum, a count
            std::vector<glm::vec4> Albedo;
            std::vector<glm::vec4> Normal;
        };

        [[nodiscard]] bool ReadAttachment(const Ref<Framebuffer>& framebuffer, u32 index, std::vector<glm::vec4>& out)
        {
            const auto* vkFramebuffer = static_cast<const VulkanFramebuffer*>(framebuffer.Raw());
            const auto image = vkFramebuffer->GetColorAttachmentImage(index);
            if (image == nullptr)
                return false;
            std::vector<u8> bytes;
            if (!image->GetData(bytes, 0))
                return false;
            if (bytes.size() != static_cast<sizet>(kWidth) * kHeight * sizeof(glm::vec4))
                return false;
            out.resize(static_cast<sizet>(kWidth) * kHeight);
            std::memcpy(out.data(), bytes.data(), bytes.size());
            return true;
        }

        [[nodiscard]] glm::vec3 RegionMean(const std::vector<glm::vec3>& image, glm::ivec2 center, i32 radius)
        {
            glm::dvec3 sum(0.0);
            u32 count = 0;
            for (i32 y = center.y - radius; y <= center.y + radius; ++y)
            {
                for (i32 x = center.x - radius; x <= center.x + radius; ++x)
                {
                    if (x < 0 || y < 0 || x >= static_cast<i32>(kWidth) || y >= static_cast<i32>(kHeight))
                        continue;
                    sum += glm::dvec3(image[static_cast<sizet>(y) * kWidth + static_cast<sizet>(x)]);
                    ++count;
                }
            }
            return count > 0 ? glm::vec3(sum / static_cast<f64>(count)) : glm::vec3(0.0f);
        }

        [[nodiscard]] std::vector<glm::vec3> MeanImage(const std::vector<glm::vec4>& accum)
        {
            std::vector<glm::vec3> image(accum.size());
            for (sizet i = 0; i < accum.size(); ++i)
                image[i] = accum[i].a > 0.0f ? glm::vec3(accum[i]) / accum[i].a : glm::vec3(0.0f);
            return image;
        }

        void WriteEvidencePng(const std::vector<glm::vec3>& image, const char* name)
        {
            namespace fs = std::filesystem;
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ReferenceFilm film(kWidth, kHeight);
            film.GetPixels() = image;
            std::vector<u8> rgba;
            film.EncodeRgba8(rgba, /*tonemap*/ 1, /*exposure*/ 1.0f, /*applyGamma*/ true);
            const std::string path = (dir / (std::string(name) + ".png")).string();
            if (::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4, rgba.data(),
                                 static_cast<int>(kWidth * 4)) == 0)
                std::cout << "[evidence] stbi_write_png failed for " << path << "\n";
            else
                std::cout << "[evidence] wrote " << path << "\n";
        }
    } // namespace

    class GpuPathTracerDevice : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            const auto gate = ProbeVulkanDeviceTestGate();
            if (!gate.Available)
                GTEST_SKIP() << gate.Reason;
            if (!ChangeToOloEditorDir())
                GTEST_SKIP() << "Could not locate OloEditor/assets/shaders from the working directory.";

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
            if (!m_Device)
                return;
            m_Backend.reset();
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

            record(api);

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
            VulkanDeferredReclaim::Get().NotifyFrameCompleted();
        }

        // The whole rig: scene twin, acceleration structures, shader, targets.
        // Returns false with a gtest failure recorded, or SKIPs, when something
        // outside this test's contract is missing.
        struct Rig
        {
            CornellBoxScene Fixture;
            GpuSceneTwin Twin;
            Ref<Shader> Shader;
            Ref<UniformBuffer> Params;
            Ref<VertexArray> Triangle;
            Ref<VertexBuffer> TriangleVertices;
            Ref<IndexBuffer> TriangleIndices;
            Ref<Texture2D> Dummy;
            std::array<Ref<Framebuffer>, 2> Targets;
            std::array<bool, 2> TargetWritten{ false, false };
            u64 TlasAddress = 0;
        };

        [[nodiscard]] bool BuildRig(Rig& rig)
        {
            rig.Fixture = MakeCornellBoxScene(18.0f, PBRModel::ClosureV2);
            if (!BuildTwin(rig.Fixture.Scene, rig.Twin))
            {
                ADD_FAILURE() << "could not upload the Cornell box twin";
                return false;
            }

            m_Backend = RT::CreateVulkanRayTracingBackend();
            if (!m_Backend || !m_Backend->GetCapabilities().Supported)
            {
                ADD_FAILURE() << "the ray-tracing backend refused a device that reports ray queries";
                return false;
            }

            RecordAndSubmit(
                [&](VulkanRendererAPI&)
                {
                    m_Backend->RecordBlasBuilds(rig.Twin.BlasBuilds);
                    m_Backend->RecordTlasBuild(rig.Twin.TlasInstances, RT::TlasBuildReason::FirstBuild);
                    m_Backend->RecordBuildToReadBarrier();
                });
            rig.TlasAddress = m_Backend->GetTlasDeviceAddress();
            if (rig.TlasAddress == 0u)
            {
                ADD_FAILURE() << "no TLAS address after the build";
                return false;
            }

            rig.Shader = Shader::Create("assets/shaders/GpuPathTracer.glsl");
            if (!rig.Shader || !rig.Shader->IsReady())
            {
                ADD_FAILURE() << "GpuPathTracer.glsl failed to load/compile on this device";
                return false;
            }

            rig.Params = UniformBuffer::Create(static_cast<u32>(sizeof(PathTracerParams)), ShaderBindingLayout::UBO_RAY_TRACING);

            // The fullscreen triangle in the 5-float layout the vertex stage
            // pulls (position xyz, uv) — MeshPrimitives' geometry, built here
            // so it lives on THIS backend selection.
            const f32 triangle[] = { -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f, -1.0f, 0.0f, 2.0f, 0.0f, -1.0f, 3.0f, 0.0f, 0.0f, 2.0f };
            u32 triangleIndices[] = { 0u, 1u, 2u };
            rig.TriangleVertices = VertexBuffer::Create(triangle, static_cast<u32>(sizeof(triangle)));
            rig.TriangleIndices = IndexBuffer::Create(triangleIndices, 3u);
            rig.Triangle = VertexArray::Create();
            rig.Triangle->AddVertexBuffer(rig.TriangleVertices);
            rig.Triangle->SetIndexBuffer(rig.TriangleIndices);

            // A valid sampler2D for the history units on a frame with no
            // history: the shader never samples them then, but a declared
            // sampler left unbound is undefined behaviour, not a zero read.
            TextureSpecification dummySpec;
            dummySpec.Width = 4;
            dummySpec.Height = 4;
            dummySpec.Format = ImageFormat::RGBA32F;
            dummySpec.GenerateMips = false;
            rig.Dummy = Texture2D::Create(dummySpec);
            std::vector<f32> zeros(4 * 4 * 4, 0.0f);
            rig.Dummy->SetData(zeros.data(), static_cast<u32>(zeros.size() * sizeof(f32)));

            for (auto& target : rig.Targets)
            {
                FramebufferSpecification spec;
                spec.Width = kWidth;
                spec.Height = kHeight;
                spec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::RGBA32F,
                                     FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::RGBA32F,
                                     FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::RGBA16F };
                target = Framebuffer::Create(spec);
                if (!target)
                {
                    ADD_FAILURE() << "six-attachment framebuffer refused";
                    return false;
                }
            }
            return true;
        }

        // One frame of the tracer into Targets[targetIndex], reading the
        // history planes from Targets[historyIndex] when `historyIndex` is
        // set. `samplesPerFrame` draws (count + s) for s below it, exactly as
        // the production pass does.
        void TraceFrame(Rig& rig, u32 targetIndex, std::optional<u32> historyIndex, u32 samplesPerFrame, u32 seed)
        {
            PathTracerParams params{};
            const glm::mat4 viewProjection = CornellBoxScene::ViewProjection(kWidth, kHeight);
            params.InvViewProjection = glm::inverse(viewProjection);
            params.CameraPosition = glm::vec4(CornellBoxScene::EyePosition(), 0.0f);
            const auto tlas = SplitAddress(rig.TlasAddress);
            params.TlasAddress = glm::uvec4(tlas.x, tlas.y, RT::kInstanceMaskAll, seed);
            params.SlotCounts = glm::uvec4(static_cast<u32>(rig.Twin.Instances.size()),
                                           static_cast<u32>(rig.Twin.Geometries.size()),
                                           static_cast<u32>(rig.Twin.Materials.size()), 0u);
            const auto emissive = SplitAddress(rig.Twin.EmissiveSsbo->GetDeviceAddress());
            const u32 flags = kGpuPathTracerFlagNextEventEstimation | (historyIndex ? kGpuPathTracerFlagHistoryValid : 0u);
            params.EmissiveTable = glm::uvec4(emissive.x, emissive.y, static_cast<u32>(rig.Twin.Emissive.size()), flags);
            params.PathParams = glm::uvec4(kMaxBounces, kRussianRouletteStart, samplesPerFrame, 0u);
            params.RayParams = glm::vec4(1e-3f, 0.0f, 1.0e5f, 0.0f);
            params.Environment = glm::vec4(rig.Fixture.Scene.GetEnvironment().Radiance,
                                           rig.Twin.EmissiveArea > 0.0f ? 1.0f / rig.Twin.EmissiveArea : 0.0f);
            params.ScreenParams = glm::vec4(static_cast<f32>(kWidth), static_cast<f32>(kHeight), 1.0f / kWidth, 1.0f / kHeight);
            params.DebugParams = glm::vec4(0.0f, 1.0f / 256.0f, 100.0f, 0.0f);
            rig.Params->SetData(&params, static_cast<u32>(sizeof(params)));

            Ref<Framebuffer> target = rig.Targets[targetIndex];
            const bool firstUse = !rig.TargetWritten[targetIndex];
            rig.TargetWritten[targetIndex] = true;

            RecordAndSubmit(
                [&](VulkanRendererAPI& api)
                {
                    // The graph's job, done by hand: every attachment to
                    // COLOR_ATTACHMENT before the draw, back to sampled after.
                    std::array<RHI::Barrier, 6> toColor{};
                    for (u32 i = 0; i < 6; ++i)
                    {
                        toColor[i].Resource = target->GetColorAttachmentHandle(i);
                        toColor[i].Before = firstUse ? RHI::Access::Undefined : RHI::Access::ShaderSampleRead;
                        toColor[i].After = RHI::Access::ColorAttachmentWrite;
                    }
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>(toColor));

                    target->Bind();
                    api.SetViewport(0, 0, kWidth, kHeight);
                    constexpr std::array<u32, 6> drawBuffers{ 0u, 1u, 2u, 3u, 4u, 5u };
                    api.SetDrawBuffers(std::span<const u32>(drawBuffers));
                    api.SetDepthTest(false);
                    api.SetBlendState(false);
                    api.DisableCulling();

                    rig.Shader->Bind();
                    rig.Params->Bind();
                    rig.Twin.InstanceSsbo->Bind();
                    rig.Twin.GeometrySsbo->Bind();
                    rig.Twin.MaterialSsbo->Bind();
                    rig.Twin.LightSsbo->Bind();

                    const RHI::ResourceHandle dummy = rig.Dummy->GetRHIHandle();
                    api.BindTexture(0, dummy);
                    if (historyIndex)
                    {
                        Ref<Framebuffer> history = rig.Targets[*historyIndex];
                        api.BindTexture(1, history->GetColorAttachmentHandle(1)); // accum
                        api.BindTexture(2, history->GetColorAttachmentHandle(4)); // normal
                        api.BindTexture(3, history->GetColorAttachmentHandle(3)); // albedo
                        api.BindTexture(4, history->GetColorAttachmentHandle(2)); // moments
                    }
                    else
                    {
                        for (u32 unit = 1; unit <= 4; ++unit)
                            api.BindTexture(unit, dummy);
                    }
                    api.BindTexture(ShaderBindingLayout::TEX_USER_1, RHI::NullResource);

                    api.DrawIndexed(rig.Triangle, 3);
                    target->Unbind();

                    std::array<RHI::Barrier, 6> toSampled{};
                    for (u32 i = 0; i < 6; ++i)
                    {
                        toSampled[i].Resource = target->GetColorAttachmentHandle(i);
                        toSampled[i].Before = RHI::Access::ColorAttachmentWrite;
                        toSampled[i].After = RHI::Access::ShaderSampleRead;
                    }
                    api.IssueBarrierBatch(MemoryBarrierFlags::None, std::span<const RHI::Barrier>(toSampled));
                });
        }

        [[nodiscard]] bool ReadFrame(const Rig& rig, u32 targetIndex, TracedFrame& frame)
        {
            return ReadAttachment(rig.Targets[targetIndex], 1, frame.Accum) &&
                   ReadAttachment(rig.Targets[targetIndex], 3, frame.Albedo) &&
                   ReadAttachment(rig.Targets[targetIndex], 4, frame.Normal);
        }

        std::unique_ptr<VulkanDevice> m_Device;
        std::unique_ptr<RT::IRayTracingBackend> m_Backend;
        VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
        VkFence m_Fence = VK_NULL_HANDLE;
    };

    // =========================================================================
    // CPU vs GPU on the Cornell box, plus determinism and accumulation
    // =========================================================================

    TEST_F(GpuPathTracerDevice, CornellBoxAgreesWithTheCpuReferenceWithinTheBudget)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        VulkanFrameArena::Get().BeginFrame(0);

        Rig rig;
        ASSERT_TRUE(BuildRig(rig));

        // --- the GPU frame ------------------------------------------------
        TraceFrame(rig, 0, std::nullopt, kSamples, kSeed);
        TracedFrame gpu;
        ASSERT_TRUE(ReadFrame(rig, 0, gpu));
        for (const glm::vec4& texel : gpu.Accum)
        {
            ASSERT_EQ(texel.a, static_cast<f32>(kSamples)) << "every pixel draws exactly the samples asked for";
            ASSERT_TRUE(std::isfinite(texel.x) && std::isfinite(texel.y) && std::isfinite(texel.z));
        }
        const std::vector<glm::vec3> gpuImage = MeanImage(gpu.Accum);

        // --- the CPU frame, same scene, same seed, same samples --------------
        PathTracerSettings settings;
        settings.SamplesPerPixel = kSamples;
        settings.MaxBounces = kMaxBounces;
        settings.RussianRouletteStartBounce = kRussianRouletteStart;
        settings.Seed = kSeed;
        settings.EnableNextEventEstimation = true;
        ReferenceFilm film(kWidth, kHeight);
        PathTracer::Render(rig.Fixture.Scene, rig.Fixture.MakeCamera(kWidth, kHeight), settings, film);
        const std::vector<glm::vec3>& cpuImage = film.GetPixels();

        WriteEvidencePng(gpuImage, "GpuPathTracer_CornellBox");
        WriteEvidencePng(cpuImage, "GpuPathTracer_CornellBox_CpuReference");

        // --- whole-frame agreement ----------------------------------------
        glm::dvec3 gpuSum(0.0), cpuSum(0.0);
        f64 absoluteDifference = 0.0;
        f32 worstPixel = 0.0f;
        u32 pixelsOverTenPercent = 0;
        for (sizet i = 0; i < gpuImage.size(); ++i)
        {
            gpuSum += glm::dvec3(gpuImage[i]);
            cpuSum += glm::dvec3(cpuImage[i]);
            const glm::vec3 delta = glm::abs(gpuImage[i] - cpuImage[i]);
            const f32 gap = std::max({ delta.x, delta.y, delta.z });
            absoluteDifference += gap;
            worstPixel = std::max(worstPixel, gap);
            const f32 scale = std::max({ cpuImage[i].x, cpuImage[i].y, cpuImage[i].z, 0.05f });
            if (gap > 0.1f * scale)
                ++pixelsOverTenPercent;
        }
        const glm::vec3 gpuMean = glm::vec3(gpuSum / static_cast<f64>(gpuImage.size()));
        const glm::vec3 cpuMean = glm::vec3(cpuSum / static_cast<f64>(cpuImage.size()));
        std::cout << "[parity] frame mean GPU " << gpuMean.x << " " << gpuMean.y << " " << gpuMean.z << " | CPU "
                  << cpuMean.x << " " << cpuMean.y << " " << cpuMean.z << " | mean abs pixel gap "
                  << absoluteDifference / static_cast<f64>(gpuImage.size()) << " | worst pixel " << worstPixel
                  << " | pixels over 10%: " << pixelsOverTenPercent << " / " << gpuImage.size() << "\n";
        for (int c = 0; c < 3; ++c)
        {
            const f32 scale = std::max(cpuMean[c], kRegionAbsoluteFloor);
            EXPECT_LE(std::abs(gpuMean[c] - cpuMean[c]), kFrameRelativeBudget * scale)
                << "frame mean channel " << c << " GPU " << gpuMean[c] << " CPU " << cpuMean[c];
        }

        // --- region means at world-anchored points ------------------------
        struct Region
        {
            const char* Name;
            glm::vec3 World;
        };
        const Region regions[] = {
            { "floor by the red wall", glm::vec3(-0.75f, -0.99f, 0.2f) },
            { "floor by the green wall", glm::vec3(0.75f, -0.99f, 0.2f) },
            { "back wall", glm::vec3(0.5f, 0.3f, -0.99f) },
            { "block top", glm::vec3(-0.3f, -0.2f, -0.3f) },
            { "ceiling (indirect only)", glm::vec3(0.6f, 0.99f, -0.2f) },
            { "emitter", glm::vec3(0.0f, 0.98f, 0.0f) },
        };
        for (const Region& region : regions)
        {
            const glm::ivec2 pixel = CornellBoxScene::ProjectToPixel(region.World, kWidth, kHeight);
            ASSERT_GE(pixel.x, 0) << region.Name;
            const glm::vec3 gpuRegion = RegionMean(gpuImage, pixel, kRegionRadius);
            const glm::vec3 cpuRegion = RegionMean(cpuImage, pixel, kRegionRadius);
            std::cout << "[parity] " << region.Name << " @ " << pixel.x << "," << pixel.y << " GPU " << gpuRegion.x << " "
                      << gpuRegion.y << " " << gpuRegion.z << " | CPU " << cpuRegion.x << " " << cpuRegion.y << " "
                      << cpuRegion.z << "\n";
            for (int c = 0; c < 3; ++c)
            {
                const f32 scale = std::max(cpuRegion[c], kRegionAbsoluteFloor);
                EXPECT_LE(std::abs(gpuRegion[c] - cpuRegion[c]), kRegionRelativeBudget * scale)
                    << region.Name << " channel " << c << " GPU " << gpuRegion[c] << " CPU " << cpuRegion[c];
            }
        }

        // --- the AOVs are the first hit, not the radiance ------------------
        const glm::ivec2 floorPixel = CornellBoxScene::ProjectToPixel(regions[0].World, kWidth, kHeight);
        const glm::vec4 albedo = gpu.Albedo[static_cast<sizet>(floorPixel.y) * kWidth + floorPixel.x];
        EXPECT_NEAR(albedo.a, static_cast<f32>(kSamples), 0.5f) << "every sample of a floor pixel hits something";
        EXPECT_NEAR(albedo.x / albedo.a, 0.73f, 1e-3f) << "the floor's albedo factor, averaged";
        const glm::vec4 normal = gpu.Normal[static_cast<sizet>(floorPixel.y) * kWidth + floorPixel.x];
        EXPECT_GT(normal.y / static_cast<f32>(kSamples), 0.99f) << "the floor's normal points up";
    }

    TEST_F(GpuPathTracerDevice, AFixedSeedAndSampleCountIsDeterministicRunToRun)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        VulkanFrameArena::Get().BeginFrame(0);

        Rig rig;
        ASSERT_TRUE(BuildRig(rig));

        TraceFrame(rig, 0, std::nullopt, 16u, kSeed);
        TraceFrame(rig, 1, std::nullopt, 16u, kSeed);
        TracedFrame first, second;
        ASSERT_TRUE(ReadFrame(rig, 0, first));
        ASSERT_TRUE(ReadFrame(rig, 1, second));
        ASSERT_EQ(first.Accum.size(), second.Accum.size());
        EXPECT_EQ(std::memcmp(first.Accum.data(), second.Accum.data(), first.Accum.size() * sizeof(glm::vec4)), 0)
            << "the same seed and sample count must reproduce the same bytes";

        // A different seed is a different image, or the seed is not wired.
        TraceFrame(rig, 1, std::nullopt, 16u, kSeed + 1u);
        ASSERT_TRUE(ReadFrame(rig, 1, second));
        EXPECT_NE(std::memcmp(first.Accum.data(), second.Accum.data(), first.Accum.size() * sizeof(glm::vec4)), 0);
    }

    TEST_F(GpuPathTracerDevice, AccumulationOverFramesDrawsTheSameSequenceAsOneFrame)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        VulkanFrameArena::Get().BeginFrame(0);

        Rig rig;
        ASSERT_TRUE(BuildRig(rig));

        // One frame of 32, then 32 more over its history, against one frame
        // of 64: the per-sample values are identical by construction, so the
        // sums agree to fp32 summation order and the counts agree exactly.
        TraceFrame(rig, 0, std::nullopt, 32u, kSeed);
        TraceFrame(rig, 1, 0u, 32u, kSeed);
        TracedFrame progressive;
        ASSERT_TRUE(ReadFrame(rig, 1, progressive));

        TraceFrame(rig, 0, std::nullopt, 64u, kSeed);
        TracedFrame oneShot;
        ASSERT_TRUE(ReadFrame(rig, 0, oneShot));

        u32 countMismatches = 0;
        f32 worstRelative = 0.0f;
        for (sizet i = 0; i < oneShot.Accum.size(); ++i)
        {
            // The count is an integer carried in an f32 (exact to 2^24).
            if (std::lround(progressive.Accum[i].a) != 64)
                ++countMismatches;
            for (int c = 0; c < 3; ++c)
            {
                const f32 scale = std::max(std::abs(oneShot.Accum[i][c]), 1e-3f);
                worstRelative = std::max(worstRelative, std::abs(progressive.Accum[i][c] - oneShot.Accum[i][c]) / scale);
            }
        }
        EXPECT_EQ(countMismatches, 0u) << "the history's count plus this frame's samples is the new count";
        EXPECT_LE(worstRelative, 1e-4f) << "the same 64 sample indices were drawn, in two frames rather than one";

        // The invalidation path: a frame told the history is invalid restarts
        // the count, whatever the previous target holds.
        TraceFrame(rig, 1, std::nullopt, 8u, kSeed);
        TracedFrame restarted;
        ASSERT_TRUE(ReadFrame(rig, 1, restarted));
        for (const glm::vec4& texel : restarted.Accum)
            ASSERT_EQ(std::lround(texel.a), 8);
    }

    TEST_F(GpuPathTracerDevice, AZeroTlasAddressPassesTheInputThroughAndWritesNoSamples)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        VulkanFrameArena::Get().BeginFrame(0);

        Rig rig;
        ASSERT_TRUE(BuildRig(rig));

        // The off switch the production pass uploads when it stands down.
        rig.TlasAddress = 0u;
        TraceFrame(rig, 0, std::nullopt, 16u, kSeed);
        TracedFrame frame;
        ASSERT_TRUE(ReadFrame(rig, 0, frame));
        for (const glm::vec4& texel : frame.Accum)
            ASSERT_EQ(texel.a, 0.0f) << "no sample may be drawn against a zero TLAS address";
    }

#endif // OLO_WITH_VULKAN
} // namespace OloEngine::Tests
