// OLO_TEST_LAYER: L7
// Executes the production ReSTIR_PT.glsl shader on a real Vulkan ray-query
// device, using scene tables and a nonempty TLAS. Synthetic plane texels and CPU
// primary intersections in Cornell deliberately substitute rasterization: this does not validate
// GBuffer generation, the render graph or editor integration. The availability
// regression additionally exercises the shipping pass's real scene admission.
// Storage records and HDR attachments are read back after real GPU execution.

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "../PathTracing/ReferenceSceneFixtures.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRPTGPU.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/Passes/ReSTIRPTPass.h"
#include "OloEngine/Renderer/PathTracing/EmissiveTriangleTable.h"
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

#if OLO_WITH_VULKAN
#include "../VulkanTestSupport.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanDescriptorSlotCache.h"
#include "Platform/Vulkan/VulkanSamplerHeap.h"
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

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace RT = OloEngine::RayTracing;
    namespace PT = OloEngine::ReSTIR::PT;
    using namespace OloEngine::PathTracing;
#if !OLO_WITH_VULKAN
    TEST(ReSTIRPTDevice, SkipsWhenNotCompiledIn)
    {
        GTEST_SKIP() << "Vulkan backend is not compiled in";
    }
#else
    namespace
    {
        constexpr u32 kWidth = 16u;
        constexpr u32 kHeight = 16u;
        constexpr u32 kPixels = kWidth * kHeight;
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

    } // namespace
    class ReSTIRPTDevice : public ::testing::Test
    {
      public:
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
            // The slot cache and the sampler heap outlive a device only as
            // stale handles; both are lazily re-creatable, like the heap.
            VulkanDescriptorSlotCache::Get().Reset();
            VulkanSamplerHeap::Get().Release();
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

        struct Rig
        {
            ReferenceScene Scene;
            std::vector<Ref<VertexBuffer>> Vertices;
            std::vector<Ref<IndexBuffer>> Indices;
            std::array<Ref<StorageBuffer>, 4> Tables;
            std::array<Ref<StorageBuffer>, 4> Paths;
            Ref<StorageBuffer> Counters;
            Ref<StorageBuffer> EmptyEmitter;
            Ref<Shader> Program;
            Ref<UniformBuffer> Params;
            Ref<VertexArray> Triangle;
            Ref<Framebuffer> Target;
            std::array<Ref<Texture2D>, 5> GBuffer;
            PT::Parameters Values;
            bool TargetWritten = false;
        };

        void BuildRig(Rig& rig, bool secondaryWall = false, f32 roughness = 0.5f, f32 metallic = 0.0f,
                      bool closedCornell = false)
        {
            ReferenceMaterial material;
            material.BaseColor = glm::vec3(0.65f, 0.55f, 0.45f);
            material.Roughness = roughness;
            material.Metallic = metallic;
            material.Model = PBRModel::ClosureV2;
            if (closedCornell)
            {
                const auto cornell = PathTracingFixtures::MakeCornellBoxScene(18.0f, PBRModel::ClosureV2);
                for (auto original : cornell.Scene.GetMaterials())
                {
                    if (glm::length(original.Emissive) < 1.0e-6f)
                    {
                        original.Roughness = roughness;
                        original.Metallic = metallic;
                    }
                    rig.Scene.AddMaterial(original);
                }
                for (u32 geometry = 0; geometry < cornell.Scene.GetGeometryCount(); ++geometry)
                {
                    const auto& original = cornell.Scene.GetGeometry(geometry);
                    rig.Scene.AddGeometry(original.GetVertices(), original.GetIndices());
                }
                for (const auto& original : cornell.Scene.GetInstances())
                    rig.Scene.AddInstance(original.GeometryIndex, original.Transform, original.MaterialIndex);
                const u32 front = rig.Scene.AddQuadGeometry({ -1, -1, 1 }, { -1, 1, 1 }, { 1, 1, 1 }, { 1, -1, 1 });
                rig.Scene.AddInstance(front, glm::mat4(1.0f), cornell.WhiteMaterial);
            }
            else
            {
                const u32 materialIndex = rig.Scene.AddMaterial(material);
                PathTracingFixtures::AddBackWallQuad(rig.Scene, 0.0f, -100.0f, 100.0f, -100.0f, 100.0f, materialIndex);
                if (secondaryWall)
                    PathTracingFixtures::AddRightWallQuad(rig.Scene, 2.0f, -10.0f, 10.0f, 0.0f, 10.0f, materialIndex);
            }
            ReferenceEnvironment environment;
            environment.Radiance = glm::vec3(closedCornell ? 0.0f : 1.0f);
            rig.Scene.SetEnvironment(environment);
            rig.Scene.Build();

            std::vector<GPUSceneGeometry> geometries;
            std::vector<RT::BlasBuildRequest> builds;
            for (u32 i = 0; i < rig.Scene.GetGeometryCount(); ++i)
            {
                const auto& geometry = rig.Scene.GetGeometry(i);
                auto vertices = VertexBuffer::Create(geometry.GetVertices().data(),
                                                     static_cast<u32>(geometry.GetVertices().size() * sizeof(Vertex)));
                auto indices = IndexBuffer::Create(const_cast<u32*>(geometry.GetIndices().data()),
                                                   static_cast<u32>(geometry.GetIndices().size()));
                ASSERT_TRUE(vertices && indices);
                GPUSceneGeometry record{};
                record.VertexAddress = vertices->GetDeviceAddress();
                record.IndexAddress = indices->GetDeviceAddress();
                ASSERT_NE(record.VertexAddress, 0u);
                ASSERT_NE(record.IndexAddress, 0u);
                record.VertexFormat = static_cast<u32>(GPUSceneVertexFormat::OloVertex);
                record.IndexFormat = static_cast<u32>(GPUSceneIndexFormat::UInt32);
                record.IndexCount = static_cast<u32>(geometry.GetIndices().size());
                record.VertexCount = static_cast<u32>(geometry.GetVertices().size());
                record.Generation = 1u;
                record.Flags = GPUSceneGeometryFlagActive;
                geometries.push_back(record);
                RT::BlasBuildRequest build{};
                build.Key = RT::GeometryKey{ i, 1u };
                build.Class = RT::GeometryClass::Static;
                build.VertexAddress = record.VertexAddress;
                build.IndexAddress = record.IndexAddress;
                build.VertexStride = sizeof(Vertex);
                build.VertexCount = record.VertexCount;
                build.IndexCount = record.IndexCount;
                builds.push_back(build);
                rig.Vertices.push_back(vertices);
                rig.Indices.push_back(indices);
            }

            std::vector<GPUSceneInstance> instances;
            std::vector<RT::InstanceRecord> tlasInstances;
            for (u32 i = 0; i < static_cast<u32>(rig.Scene.GetInstances().size()); ++i)
            {
                const auto& instance = rig.Scene.GetInstances()[i];
                const auto rows = RowsOf(instance.Transform);
                GPUSceneInstance record{};
                record.CurrentTransform.Row0 = rows[0];
                record.CurrentTransform.Row1 = rows[1];
                record.CurrentTransform.Row2 = rows[2];
                record.PreviousTransform = record.CurrentTransform;
                record.GeometryIndex = instance.GeometryIndex;
                record.GeometryGeneration = 1u;
                record.MaterialIndex = instance.MaterialIndex;
                record.MaterialGeneration = 1u;
                record.StableIndex = i;
                record.VisibilityMask = ~0u;
                record.Flags = GPUSceneInstanceFlagActive;
                record.Generation = 1u;
                instances.push_back(record);
                RT::InstanceRecord tlas{};
                tlas.Transform = rows;
                tlas.CustomIndex = i;
                tlas.Mask = RT::kInstanceMaskAll;
                tlas.ForceOpaque = true;
                tlas.Geometry = RT::GeometryKey{ instance.GeometryIndex, 1u };
                tlasInstances.push_back(tlas);
            }
            std::vector<GPUSceneMaterial> gpuMaterials;
            for (const auto& sourceMaterial : rig.Scene.GetMaterials())
            {
                GPUSceneMaterial gpuMaterial{};
                gpuMaterial.BaseColorFactor = glm::vec4(sourceMaterial.BaseColor, 1.0f);
                gpuMaterial.EmissiveFactor = glm::vec4(sourceMaterial.Emissive, 0.0f);
                gpuMaterial.RoughnessFactor = sourceMaterial.Roughness;
                gpuMaterial.MetallicFactor = sourceMaterial.Metallic;
                gpuMaterial.NormalScale = 1.0f;
                gpuMaterial.ClosureVersion = static_cast<u32>(sourceMaterial.Model);
                gpuMaterial.Flags = GPUSceneMaterialFlagActive | GPUSceneMaterialFlagPBR;
                gpuMaterial.StableIndex = static_cast<u32>(gpuMaterials.size());
                gpuMaterial.Generation = 1u;
                gpuMaterials.push_back(gpuMaterial);
            }
            const GPUSceneLight noLight{};
            rig.Tables[0] = StorageBuffer::Create(static_cast<u32>(instances.size() * sizeof(GPUSceneInstance)),
                                                  GPUSceneBindingLayout::Instances);
            rig.Tables[1] = StorageBuffer::Create(static_cast<u32>(geometries.size() * sizeof(GPUSceneGeometry)),
                                                  GPUSceneBindingLayout::Geometries);
            rig.Tables[2] = StorageBuffer::Create(static_cast<u32>(gpuMaterials.size() * sizeof(GPUSceneMaterial)),
                                                  GPUSceneBindingLayout::Materials);
            rig.Tables[3] = StorageBuffer::Create(sizeof(noLight), GPUSceneBindingLayout::Lights);
            for (const auto& table : rig.Tables)
                ASSERT_TRUE(table);
            rig.Tables[0]->SetData(instances.data(), static_cast<u32>(instances.size() * sizeof(GPUSceneInstance)));
            rig.Tables[1]->SetData(geometries.data(), static_cast<u32>(geometries.size() * sizeof(GPUSceneGeometry)));
            rig.Tables[2]->SetData(gpuMaterials.data(), static_cast<u32>(gpuMaterials.size() * sizeof(GPUSceneMaterial)));
            rig.Tables[3]->SetData(&noLight, sizeof(noLight));

            m_Backend = RT::CreateVulkanRayTracingBackend();
            ASSERT_TRUE(m_Backend && m_Backend->GetCapabilities().Supported);
            RecordAndSubmit([&](VulkanRendererAPI&)
                            {
                m_Backend->RecordBlasBuilds(builds);
                m_Backend->RecordTlasBuild(tlasInstances, RT::TlasBuildReason::FirstBuild);
                m_Backend->RecordBuildToReadBarrier(); });
            ASSERT_NE(m_Backend->GetTlasDeviceAddress(), 0u) << "No RT evidence is valid with an empty TLAS";

            rig.Program = Shader::Create("assets/shaders/ReSTIR_PT.glsl");
            ASSERT_TRUE(rig.Program && rig.Program->IsReady()) << "Production PT shader must compile on a capable device";
            rig.Params = UniformBuffer::Create(sizeof(PT::Parameters), ShaderBindingLayout::UBO_RAY_TRACING);
            ASSERT_TRUE(rig.Params);
            for (auto& paths : rig.Paths)
            {
                paths = StorageBuffer::Create(kPixels * sizeof(PT::PathRecord), StorageBuffer::kNoBinding,
                                              StorageBufferUsage::DynamicCopy);
                ASSERT_TRUE(paths);
                ASSERT_NE(paths->GetDeviceAddress(), 0u);
                paths->ClearData();
            }
            rig.Counters = StorageBuffer::Create(PT::kCounterCount * sizeof(u32), StorageBuffer::kNoBinding,
                                                 StorageBufferUsage::DynamicCopy);
            ASSERT_TRUE(rig.Counters);
            rig.Counters->ClearData();
            std::vector<EmissiveTriangleRecord> emitters;
            f32 emitterArea = 0.0f;
            for (const auto& instance : rig.Scene.GetInstances())
            {
                const auto& sourceMaterial = rig.Scene.GetMaterial(instance.MaterialIndex);
                if (glm::length(sourceMaterial.Emissive) < 1.0e-6f)
                    continue;
                const auto& geometry = rig.Scene.GetGeometry(instance.GeometryIndex);
                emitterArea = EmissiveTriangleTable::AppendTriangles(
                    EmissiveTriangleTable::TriangleRange{
                        .Vertices = geometry.GetVertices(), .Indices = geometry.GetIndices(), .FirstIndex = 0u, .IndexCount = static_cast<u32>(geometry.GetIndices().size()), .BaseVertex = 0 },
                    EmissiveTriangleTable::Emitter{ .WorldTransform = instance.Transform,
                                                    .RenderOrigin = glm::vec3(0.0f),
                                                    .Radiance = sourceMaterial.Emissive,
                                                    .TwoSided = sourceMaterial.TwoSidedEmission },
                    emitterArea, emitters);
            }
            EmissiveTriangleTable::Finalize(emitters, emitterArea);
            rig.EmptyEmitter = StorageBuffer::Create(
                static_cast<u32>(std::max<sizet>(emitters.size(), 1u) * sizeof(EmissiveTriangleRecord)), StorageBuffer::kNoBinding);
            ASSERT_TRUE(rig.EmptyEmitter);
            const EmissiveTriangleRecord noEmitter{};
            if (emitters.empty())
                rig.EmptyEmitter->SetData(&noEmitter, sizeof(noEmitter));
            else
                rig.EmptyEmitter->SetData(emitters.data(), static_cast<u32>(emitters.size() * sizeof(EmissiveTriangleRecord)));

            const f32 triangle[] = { -1, -1, 0, 0, 0, 3, -1, 0, 2, 0, -1, 3, 0, 0, 2 };
            u32 triangleIndices[] = { 0, 1, 2 };
            auto vertices = VertexBuffer::Create(triangle, sizeof(triangle));
            auto indices = IndexBuffer::Create(triangleIndices, 3u);
            ASSERT_TRUE(vertices && indices);
            rig.Triangle = VertexArray::Create();
            rig.Triangle->AddVertexBuffer(vertices);
            rig.Triangle->SetIndexBuffer(indices);
            FramebufferSpecification framebuffer;
            framebuffer.Width = kWidth;
            framebuffer.Height = kHeight;
            framebuffer.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::RGBA32F,
                                        FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::RGBA32F };
            rig.Target = Framebuffer::Create(framebuffer);
            ASSERT_TRUE(rig.Target);

            const glm::vec3 eye(0.0f, 0.0f, closedCornell ? 0.8f : 2.0f);
            const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0, 1, 0));
            const glm::mat4 projection = glm::perspective(glm::radians(35.0f), 1.0f, 0.1f, 20.0f);
            const glm::vec4 clip = projection * view * glm::vec4(0, 0, 0, 1);
            const f32 depth = (clip.z / clip.w) * 0.5f + 0.5f;
            // Plane texels exactly match the fixture's primary surface. The
            // optional wall is outside primary frustum but visible to bounce rays.
            const std::array<glm::vec4, 5> texels{ glm::vec4(depth), glm::vec4(material.BaseColor, metallic),
                                                   glm::vec4(0, 0, roughness, 1), glm::vec4(0, 0, 0, 2), glm::vec4(0) };
            std::array<std::vector<glm::vec4>, 5> imageData;
            for (sizet i = 0; i < imageData.size(); ++i)
                imageData[i].assign(kPixels, texels[i]);
            if (closedCornell)
            {
                const auto inverse = glm::inverse(projection * view);
                for (u32 y = 0; y < kHeight; ++y)
                {
                    for (u32 x = 0; x < kWidth; ++x)
                    {
                        const glm::vec2 uv((static_cast<f32>(x) + 0.5f) / kWidth, (static_cast<f32>(y) + 0.5f) / kHeight);
                        glm::vec4 point = inverse * glm::vec4(uv * 2.0f - 1.0f, 1.0f, 1.0f);
                        point /= point.w;
                        SurfaceInteraction hit;
                        ASSERT_TRUE(rig.Scene.Intersect(Ray(eye, glm::normalize(glm::vec3(point) - eye)), hit));
                        const auto& hitMaterial = rig.Scene.GetMaterial(hit.MaterialIndex);
                        const glm::vec4 hitClip = projection * view * glm::vec4(hit.Position, 1.0f);
                        glm::vec3 normal = glm::normalize(hit.ShadingNormal);
                        if (glm::dot(normal, eye - hit.Position) < 0.0f)
                            normal = -normal;
                        normal /= std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
                        glm::vec2 oct(normal);
                        if (normal.z < 0.0f)
                            oct = (glm::vec2(1.0f) - glm::abs(glm::vec2(oct.y, oct.x))) *
                                  glm::vec2(oct.x >= 0.0f ? 1.0f : -1.0f, oct.y >= 0.0f ? 1.0f : -1.0f);
                        const u32 pixel = y * kWidth + x;
                        imageData[0][pixel] = glm::vec4((hitClip.z / hitClip.w) * 0.5f + 0.5f);
                        imageData[1][pixel] = glm::vec4(hitMaterial.BaseColor, hitMaterial.Metallic);
                        imageData[2][pixel] = glm::vec4(oct, hitMaterial.Roughness, 1.0f);
                        imageData[3][pixel] = glm::vec4(hitMaterial.Emissive, 2.0f);
                    }
                }
            }
            for (sizet i = 0; i < rig.GBuffer.size(); ++i)
            {
                TextureSpecification specification;
                specification.Width = kWidth;
                specification.Height = kHeight;
                specification.Format = ImageFormat::RGBA32F;
                specification.GenerateMips = false;
                rig.GBuffer[i] = Texture2D::Create(specification);
                ASSERT_TRUE(rig.GBuffer[i]);
                rig.GBuffer[i]->SetData(imageData[i].data(), static_cast<u32>(imageData[i].size() * sizeof(glm::vec4)));
            }
            const auto tlas = SplitAddress(m_Backend->GetTlasDeviceAddress());
            const auto emitter = SplitAddress(rig.EmptyEmitter->GetDeviceAddress());
            rig.Values.InvView = glm::inverse(view);
            rig.Values.InvProjection = glm::inverse(projection);
            rig.Values.View = view;
            rig.Values.TlasAddressAndFrame = glm::uvec4(tlas, RT::kInstanceMaskAll, 1u);
            rig.Values.SlotCounts = glm::uvec4(static_cast<u32>(instances.size()), static_cast<u32>(geometries.size()),
                                               static_cast<u32>(gpuMaterials.size()), 0u);
            rig.Values.EmissiveTable = glm::uvec4(emitter, static_cast<u32>(emitters.size()), 0u);
            rig.Values.MaterialTable = glm::uvec4(0u, 0u, 0u, RHI::HeapOffset::Invalid);
            rig.Values.Counts = glm::uvec4(64u, 0u, 7u, 0x12345u);
            rig.Values.Params = glm::vec4(1.0e-3f, 1.0e-3f, 1.0e4f, 0.25f);
            rig.Values.EstimatorParams = glm::vec4(emitterArea > 0.0f ? 1.0f / emitterArea : 0.0f, 0.0f, 0.0f, 2.0f);
            rig.Values.Environment = glm::vec4(environment.Radiance, 0.0f);
            rig.Values.Screen = glm::vec4(kWidth, kHeight, 1.0f / kWidth, 1.0f / kHeight);
            rig.Values.Reuse = glm::uvec4(1u, 1u, 1u, 0x23456u);
            rig.Values.Debug = glm::vec4(0.0f, 1211.0f, 256.0f, static_cast<f32>(PT::kLayoutVersion));
        }

        void Draw(Rig& rig, u32 stage, u32 source, u32 destination, u32 neighbour = 0u, u32 history = 0u)
        {
            rig.Values.Counts.y = stage;
            rig.Values.SourceAddresses = glm::uvec4(SplitAddress(rig.Paths[source]->GetDeviceAddress()),
                                                    SplitAddress(rig.Paths[neighbour]->GetDeviceAddress()));
            rig.Values.DestinationAddresses = glm::uvec4(SplitAddress(rig.Paths[destination]->GetDeviceAddress()),
                                                         SplitAddress(rig.Counters->GetDeviceAddress()));
            rig.Values.HistoryAddresses = glm::uvec4(SplitAddress(rig.Paths[history]->GetDeviceAddress()),
                                                     SplitAddress(rig.Paths[0]->GetDeviceAddress()));
            rig.Params->SetData(&rig.Values, sizeof(rig.Values));
            const bool firstUse = !rig.TargetWritten;
            rig.TargetWritten = true;
            RecordAndSubmit([&](VulkanRendererAPI& api)
                            {
                api.MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
                std::array<RHI::Barrier, 4> barriers{};
                for (u32 i = 0; i < 4u; ++i)
                {
                    barriers[i].Resource = rig.Target->GetColorAttachmentHandle(i);
                    barriers[i].Before = firstUse ? RHI::Access::Undefined : RHI::Access::ShaderSampleRead;
                    barriers[i].After = RHI::Access::ColorAttachmentWrite;
                }
                api.IssueBarrierBatch(MemoryBarrierFlags::None, barriers);
                rig.Target->Bind();
                api.SetViewport(0, 0, kWidth, kHeight);
                const std::array<u32, 4> draws{ 0u, 1u, 2u, 3u };
                api.SetDrawBuffers(draws);
                api.SetDepthTest(false);
                api.SetBlendState(false);
                api.DisableCulling();
                rig.Program->Bind();
                rig.Params->Bind();
                for (const auto& table : rig.Tables)
                    table->Bind();
                const std::array<u32, 5> bindings{ 19u, 43u, 44u, 45u, 46u };
                for (sizet i = 0; i < bindings.size(); ++i)
                    api.BindTexture(bindings[i], rig.GBuffer[i]->GetRHIHandle());
                api.BindTexture(ShaderBindingLayout::TEX_USER_1, RHI::NullResource);
                api.DrawIndexed(rig.Triangle, 3u);
                rig.Target->Unbind();
                for (auto& barrier : barriers)
                {
                    barrier.Before = RHI::Access::ColorAttachmentWrite;
                    barrier.After = RHI::Access::ShaderSampleRead;
                }
                api.IssueBarrierBatch(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate, barriers); });
        }

        static std::vector<PT::PathRecord> ReadPaths(const Rig& rig, u32 index)
        {
            std::vector<PT::PathRecord> records(kPixels);
            rig.Paths[index]->GetData(records.data(), static_cast<u32>(records.size() * sizeof(PT::PathRecord)));
            return records;
        }

        static std::array<u32, PT::kCounterCount> ReadCounters(const Rig& rig)
        {
            std::array<u32, PT::kCounterCount> counters{};
            rig.Counters->GetData(counters.data(), sizeof(counters));
            return counters;
        }

        static void ReadRadiance(const Rig& rig, std::vector<glm::vec4>& output)
        {
            const auto* framebuffer = static_cast<const VulkanFramebuffer*>(rig.Target.Raw());
            const auto image = framebuffer->GetColorAttachmentImage(0u);
            ASSERT_TRUE(image);
            std::vector<u8> bytes;
            ASSERT_TRUE(image->GetData(bytes, 0u));
            ASSERT_EQ(bytes.size(), kPixels * sizeof(glm::vec4));
            output.resize(kPixels);
            std::memcpy(output.data(), bytes.data(), bytes.size());
        }

        static void ExpectFinitePaths(const std::vector<PT::PathRecord>& paths)
        {
            for (const auto& path : paths)
            {
                EXPECT_TRUE(std::isfinite(path.State.x) && std::isfinite(path.State.y) &&
                            std::isfinite(path.State.z) && std::isfinite(path.State.w));
                EXPECT_TRUE(std::isfinite(path.Value.x) && std::isfinite(path.Value.y) && std::isfinite(path.Value.z));
                EXPECT_GE(path.State.x, 0.0f);
                EXPECT_GE(path.State.y, 0.0f);
            }
        }

        std::unique_ptr<VulkanDevice> m_Device;
        std::unique_ptr<RT::IRayTracingBackend> m_Backend;
        VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
        VkFence m_Fence = VK_NULL_HANDLE;
    };

    TEST_F(ReSTIRPTDevice, ShippingPassRejectsIncompleteSceneCoverageAndRestoresSupportedAvailability)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        Rig rig;
        ASSERT_NO_FATAL_FAILURE(BuildRig(rig));
        GPUScene scene;
        scene.InitializeGPU();
        const GPUSceneGeometryKey geometryKey{ 1211u, 1212u, 0u };
        const GPUSceneMaterialKey materialKey{ 1211u, 0u, 0u };
        const GPUSceneInstanceKey instanceKey{ 1211u, geometryKey, 0u };
        GPUSceneGeometryInput geometry{};
        geometry.m_VertexBuffer = rig.Vertices.front()->GetRHIHandle();
        geometry.m_IndexBuffer = rig.Indices.front()->GetRHIHandle();
        geometry.m_VertexAddress = rig.Vertices.front()->GetDeviceAddress();
        geometry.m_IndexAddress = rig.Indices.front()->GetDeviceAddress();
        geometry.m_VertexFormat = static_cast<u32>(GPUSceneVertexFormat::OloVertex);
        geometry.m_IndexFormat = static_cast<u32>(GPUSceneIndexFormat::UInt32);
        geometry.m_VertexCount = static_cast<u32>(rig.Scene.GetGeometry(0u).GetVertices().size());
        geometry.m_IndexCount = static_cast<u32>(rig.Scene.GetGeometry(0u).GetIndices().size());
        const auto extract = [&](u32 unsupportedCount, bool transmission)
        {
            scene.BeginExtraction(1211u, glm::vec3(0.0f));
            scene.ExtractGeometry(geometryKey, geometry);
            GPUSceneMaterialInput material{};
            material.m_ClosureVersion = static_cast<u32>(PBRModel::ClosureV2);
            material.m_RoughnessFactor = 0.5f;
            material.m_Flags = GPUSceneMaterialFlagPBR | (transmission ? GPUSceneMaterialFlagTransmission : 0u);
            scene.ExtractMaterial(materialKey, material);
            GPUSceneInstanceInput instance{};
            instance.m_Material = materialKey;
            scene.ExtractInstance(instanceKey, instance);
            if (unsupportedCount != 0u)
                scene.ReportUnsupported(GPUSceneUnsupportedCategory::Skinned, unsupportedCount);
            [[maybe_unused]] const auto update = scene.EndExtraction();
            scene.Upload();
        };
        extract(0u, false);
        RT::RayTracingScene rayScene;
        rayScene.Init();
        ASSERT_NO_FATAL_FAILURE(RecordAndSubmit([&](VulkanRendererAPI&)
                                                {
            rayScene.Update(scene);
            rayScene.RecordBuildToReadBarrier(); }));
        ASSERT_TRUE(rayScene.IsAvailable());
        ASSERT_NE(rayScene.GetTlasDeviceAddress(), 0u);
        ASSERT_GT(rayScene.GetStats().Resident.TlasInstances, 0u);

        ReSTIRPTPass pass;
        FramebufferSpecification spec{};
        spec.Width = kWidth;
        spec.Height = kHeight;
        pass.Init(spec);
        ReSTIRPTSettings settings{};
        settings.Enabled = true;
        pass.SetSettings(settings);
        pass.SetEnabled(true);
        pass.SetGPUScene(&scene);
        pass.SetRayTracingScene(&rayScene);
        pass.SetEnvironment(glm::vec3(1.0f), 0.0f, false);
        pass.SetSceneEpoch(1u);
        pass.ResolveAvailabilityForFrame(true, false);
        ASSERT_TRUE(pass.GetStats().Active) << pass.GetStats().FallbackReason;
        EXPECT_GT(pass.GetStats().ReservoirBytes, 0u);

        pass.ResolveAvailabilityForFrame(false, false);
        EXPECT_TRUE(pass.GetStats().Requested);
        EXPECT_FALSE(pass.GetStats().Active);
        EXPECT_EQ(pass.GetStats().FallbackReason, "deferred path required");
        pass.ResolveAvailabilityForFrame(true, true);
        EXPECT_FALSE(pass.GetStats().Active);
        EXPECT_EQ(pass.GetStats().FallbackReason, "participating media unsupported");

        // These objects never enter the TLAS; its nonempty status alone cannot
        // establish complete scene coverage. Preserve the supported instance.
        extract(3u, false);
        EXPECT_EQ(scene.GetLastFrameUpdate().m_Stats.m_UnsupportedTotal, 3u);
        EXPECT_GT(rayScene.GetStats().Resident.TlasInstances, 0u);
        pass.ResolveAvailabilityForFrame(true, false);
        EXPECT_TRUE(pass.GetStats().Requested);
        EXPECT_FALSE(pass.GetStats().Active);
        EXPECT_FALSE(pass.GetStats().HistoryValid);
        EXPECT_EQ(pass.GetStats().FallbackReason, "incomplete GPU scene geometry coverage");

        extract(0u, true);
        pass.ResolveAvailabilityForFrame(true, false);
        EXPECT_FALSE(pass.GetStats().Active);
        EXPECT_EQ(pass.GetStats().FallbackReason, "unsupported material");

        extract(0u, false);
        pass.ResolveAvailabilityForFrame(true, false);
        EXPECT_TRUE(pass.GetStats().Active) << pass.GetStats().FallbackReason;
        EXPECT_EQ(pass.GetStats().FallbackReason, "none");
        EXPECT_FALSE(pass.GetStats().CountersValid) << "Admission does not claim shader execution";
        rayScene.Shutdown();
        scene.Shutdown();
    }

    TEST_F(ReSTIRPTDevice, InitialRecordsAndDisabledTemporalCopyPreserveTheDeviceABI)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        Rig rig;
        ASSERT_NO_FATAL_FAILURE(BuildRig(rig));
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 0u, 0u));
        const auto initial = ReadPaths(rig, 0u);
        ExpectFinitePaths(initial);
        u32 selected = 0u;
        for (u32 pixel = 0; pixel < kPixels; ++pixel)
        {
            const auto& path = initial[pixel];
            EXPECT_EQ(path.Lineage.w >> 16u, PT::kLayoutVersion);
            EXPECT_NE(path.Lineage.w & 1u, 0u);
            if ((path.Lineage.w & 2u) == 0u)
                continue;
            ++selected;
            EXPECT_EQ(path.Metadata.x, 0u) << "A convex plane has no secondary surface bounce";
            EXPECT_EQ(path.Metadata.y, 1u);
            EXPECT_EQ(path.Metadata.z, rig.Values.Counts.w);
            EXPECT_EQ(path.Metadata.w, rig.Values.Reuse.w);
            EXPECT_EQ(path.Lineage.y, pixel);
            EXPECT_FLOAT_EQ(path.Endpoint.PositionKind.w, 1.0f);
            EXPECT_NEAR(path.Vertices[0].PositionRoughness.z, 0.0f, 1.0e-5f);
            EXPECT_NEAR(path.Vertices[0].GeometricNormalMetallic.z, 1.0f, 1.0e-6f);
        }
        ASSERT_GT(selected, kPixels / 2u) << "Selected-path checks must not pass on an all-empty output";
        rig.Values.Reuse.x = 0u;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 0u, 1u));
        const auto copied = ReadPaths(rig, 1u);
        ASSERT_EQ(copied.size(), initial.size());
        EXPECT_EQ(std::memcmp(initial.data(), copied.data(), initial.size() * sizeof(PT::PathRecord)), 0)
            << "A disabled reuse stage must preserve every field, including integer identity and inactive vertices";
    }

    TEST_F(ReSTIRPTDevice, EveryMappingRunsTemporalSpatialAndResolveOnRealGeometry)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        Rig rig;
        ASSERT_NO_FATAL_FAILURE(BuildRig(rig, true));
        for (const u32 mapping : { 1u, 2u, 4u })
        {
            SCOPED_TRACE(mapping);
            rig.Counters->ClearData();
            rig.Values.Counts.z = mapping;
            rig.Values.TlasAddressAndFrame.w = 1u;
            ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 3u, 3u));
            rig.Values.TlasAddressAndFrame.w = 2u;
            ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 0u, 0u));
            ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 0u, 1u, 0u, 3u));
            ExpectFinitePaths(ReadPaths(rig, 1u));
            ASSERT_NO_FATAL_FAILURE(Draw(rig, 2u, 1u, 2u, 1u, 3u));
            ExpectFinitePaths(ReadPaths(rig, 2u));
            ASSERT_NO_FATAL_FAILURE(Draw(rig, 3u, 2u, 2u, 1u, 3u));
            std::vector<glm::vec4> radiance;
            ASSERT_NO_FATAL_FAILURE(ReadRadiance(rig, radiance));
            f64 energy = 0.0;
            for (const auto& pixel : radiance)
            {
                ASSERT_TRUE(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z));
                EXPECT_GE(pixel.x, 0.0f);
                EXPECT_FLOAT_EQ(pixel.w, 1.0f);
                energy += pixel.x + pixel.y + pixel.z;
            }
            EXPECT_GT(energy, 1.0);
            const auto counters = ReadCounters(rig);
            EXPECT_GT(counters[0], 0u) << "Instrumented ray counter must report actual work";
            EXPECT_GT(counters[4], 0u) << "Temporal acceptance must be observed, not inferred from settings";
            EXPECT_GT(counters[5], 0u) << "Spatial acceptance must be observed, not inferred from settings";
            const u32 acceptedCounter = mapping == 1u ? 6u : (mapping == 2u ? 7u : 8u);
            EXPECT_GT(counters[acceptedCounter], 0u);
            EXPECT_EQ(counters[13], 0u) << "No non-finite path may be silently sanitized into a passing frame";
        }
    }

    TEST_F(ReSTIRPTDevice, IncompatibleRecordAndParameterVersionsFailClosed)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        Rig rig;
        ASSERT_NO_FATAL_FAILURE(BuildRig(rig));
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 3u, 3u));
        const auto history = ReadPaths(rig, 3u);
        rig.Values.TlasAddressAndFrame.w = 2u;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 0u, 0u));
        const auto canonical = ReadPaths(rig, 0u);
        ASSERT_TRUE(std::any_of(canonical.begin(), canonical.end(), [](const PT::PathRecord& path)
                                { return (path.Lineage.w & 2u) != 0u; }));

        for (const u32 version : { 0u, PT::kLayoutVersion + 1u })
        {
            SCOPED_TRACE(version);
            auto incompatible = history;
            for (auto& path : incompatible)
                path.Lineage.w = (path.Lineage.w & 0xffffu) | (version << 16u);
            rig.Paths[3]->SetData(incompatible.data(), static_cast<u32>(incompatible.size() * sizeof(PT::PathRecord)));
            rig.Counters->ClearData();
            ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 0u, 1u, 0u, 3u));
            const auto counters = ReadCounters(rig);
            EXPECT_GT(counters[9], 0u);
            EXPECT_EQ(counters[4], 0u);
            const auto output = ReadPaths(rig, 1u);
            EXPECT_EQ(std::memcmp(output.data(), canonical.data(), canonical.size() * sizeof(PT::PathRecord)), 0)
                << "Incompatible history must preserve the valid canonical reservoir";
        }

        // Even a disabled copy stage must not propagate incompatible records.
        rig.Values.Reuse.x = 0u;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 3u, 1u));
        for (const auto& path : ReadPaths(rig, 1u))
        {
            EXPECT_EQ(path.Lineage.w >> 16u, PT::kLayoutVersion);
            EXPECT_EQ(path.Lineage.w & 7u, 0u);
        }

        // A mismatched CPU layout could imply a different buffer stride. Do
        // not even clear/write those records: leave the buffer untouched and
        // return a transparent inactive output without dispatching a ray.
        rig.Paths[1]->SetData(canonical.data(), static_cast<u32>(canonical.size() * sizeof(PT::PathRecord)));
        rig.Values.Debug.w = static_cast<f32>(PT::kLayoutVersion + 1u);
        rig.Counters->ClearData();
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 0u, 1u));
        const auto untouched = ReadPaths(rig, 1u);
        EXPECT_EQ(std::memcmp(untouched.data(), canonical.data(), canonical.size() * sizeof(PT::PathRecord)), 0);
        EXPECT_EQ(ReadCounters(rig)[0], 0u);
        std::vector<glm::vec4> radiance;
        ASSERT_NO_FATAL_FAILURE(ReadRadiance(rig, radiance));
        for (const auto& pixel : radiance)
            EXPECT_FLOAT_EQ(pixel.w, 0.0f);
    }

    TEST_F(ReSTIRPTDevice, EpochAndInconsistentReplayCoordinatesProduceMeasuredRejections)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        Rig rig;
        ASSERT_NO_FATAL_FAILURE(BuildRig(rig));
        rig.Values.Counts.z = 2u;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 3u, 3u));
        const auto history = ReadPaths(rig, 3u);
        rig.Values.TlasAddressAndFrame.w = 2u;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 0u, 0u));

        auto corrupt = history;
        for (auto& path : corrupt)
            ++path.Metadata.z;
        rig.Paths[3]->SetData(corrupt.data(), static_cast<u32>(corrupt.size() * sizeof(PT::PathRecord)));
        rig.Counters->ClearData();
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 0u, 1u, 0u, 3u));
        const auto epochCounters = ReadCounters(rig);
        EXPECT_GT(epochCounters[9], 0u) << "Wrong scene epoch must be counted as outside the shift domain";
        EXPECT_EQ(epochCounters[4], 0u);
        ExpectFinitePaths(ReadPaths(rig, 1u));

        corrupt = history;
        for (auto& path : corrupt)
        {
            // Keep the discrete lobe choice, but alter its 2D sample without
            // altering the stored endpoint. Forward replay can trace this chart;
            // inverse replay cannot reconstruct the original retained direction.
            path.Vertices[0].Randoms.y = std::fmod(path.Vertices[0].Randoms.y + 0.31f, 1.0f);
            path.Vertices[0].Randoms.z = std::fmod(path.Vertices[0].Randoms.z + 0.27f, 1.0f);
        }
        rig.Paths[3]->SetData(corrupt.data(), static_cast<u32>(corrupt.size() * sizeof(PT::PathRecord)));
        rig.Counters->ClearData();
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 0u, 1u, 0u, 3u));
        const auto inverseCounters = ReadCounters(rig);
        EXPECT_GT(inverseCounters[10], 0u) << "Inverse mismatch must be exercised by a broken chart record";
        EXPECT_EQ(inverseCounters[4], 0u);
        EXPECT_EQ(inverseCounters[13], 0u);
        ExpectFinitePaths(ReadPaths(rig, 1u));
    }

    TEST_F(ReSTIRPTDevice, MultibounceIdentityReplayAndMappingCountPreserveJacobianAndConfidence)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        Rig rig;
        ASSERT_NO_FATAL_FAILURE(BuildRig(rig, false, 0.45f, 0.0f, true));
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 3u, 3u));
        const auto history = ReadPaths(rig, 3u);
        const auto secondary = std::count_if(history.begin(), history.end(), [](const PT::PathRecord& path)
                                             { return (path.Lineage.w & 2u) != 0u && path.Metadata.x > 0u; });
        ASSERT_GT(secondary, 16) << "Identity replay must cover actual secondary vertices";
        rig.Values.TlasAddressAndFrame.w = 2u;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 0u, 0u));
        rig.Values.Counts.z = 2u;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 0u, 1u, 0u, 3u));
        const auto replayed = ReadPaths(rig, 1u);
        u32 selectedReplay = 0u;
        for (const auto& path : replayed)
        {
            if ((path.Lineage.w & 2u) == 0u || path.Lineage.z != 2u || path.Metadata.x == 0u)
                continue;
            ++selectedReplay;
            EXPECT_NEAR(path.State.w, 0.0f, 5.0e-4f)
                << "Same receiver and raw uniforms imply identity replay, including each secondary factor";
        }
        ASSERT_GT(selectedReplay, 4u);
        rig.Values.Counts.z = 1u;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 0u, 1u, 0u, 3u));
        const auto singleMapping = ReadPaths(rig, 1u);
        rig.Values.Counts.z = 7u;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 0u, 2u, 0u, 3u));
        const auto allMappings = ReadPaths(rig, 2u);
        u32 combined = 0u;
        for (u32 pixel = 0; pixel < kPixels; ++pixel)
        {
            EXPECT_FLOAT_EQ(allMappings[pixel].State.y, singleMapping[pixel].State.y)
                << "Three maps of one source must not triple its represented proposal count";
            if (singleMapping[pixel].State.y > static_cast<f32>(rig.Values.Counts.x))
            {
                ++combined;
                EXPECT_FLOAT_EQ(singleMapping[pixel].State.y, 2.0f * static_cast<f32>(rig.Values.Counts.x));
            }
        }
        ASSERT_GT(combined, 16u);
    }

    TEST_F(ReSTIRPTDevice, ClosedCornellDiffuseAndGlossyIndirectMatchExactlyTruncatedReferenceSupport)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        // Independent oracle: BSDF-only CPU tracing at depth 5 minus depth 2,
        // paired with identical random streams. Depth 2 includes ALL emission
        // reached directly after primary scattering; depth 1 would not. With no
        // environment and NEE disabled, the difference retains precisely emitter
        // paths after one, two or three secondary reflections. GPU terminal NEE
        // and BSDF strategies estimate that SAME integral through their own MIS.
        // This tests real secondary transport, not an environment-only reduction.
        constexpr u32 blocks = 8u;
        constexpr u32 cpuSamples = 512u;
        constexpr u32 framesPerBlock = 8u;
        for (const bool glossy : { false, true })
        {
            SCOPED_TRACE(glossy ? "glossy closed Cornell" : "diffuse closed Cornell");
            Rig rig;
            ASSERT_NO_FATAL_FAILURE(BuildRig(rig, false, glossy ? 0.12f : 0.8f, glossy ? 0.6f : 0.0f, true));
            std::array<f64, blocks> cpuMeans{};
            std::array<f64, blocks> gpuMeans{};
            std::array<f64, blocks> combinedMeans{};
            PathTracerSettings settings;
            settings.EnableNextEventEstimation = false;
            settings.RussianRouletteStartBounce = 0u;
            settings.MaxRadianceClamp = 0.0f;
            const glm::vec3 eye(rig.Values.InvView[3]);
            const glm::mat4 inverse = rig.Values.InvView * rig.Values.InvProjection;
            for (u32 block = 0; block < blocks; ++block)
            {
                for (u32 y = 0; y < kHeight; ++y)
                {
                    for (u32 x = 0; x < kWidth; ++x)
                    {
                        const glm::vec2 uv((static_cast<f32>(x) + 0.5f) / kWidth, (static_cast<f32>(y) + 0.5f) / kHeight);
                        glm::vec4 point = inverse * glm::vec4(uv * 2.0f - 1.0f, 1.0f, 1.0f);
                        point /= point.w;
                        const Ray primary(eye, glm::normalize(glm::vec3(point) - eye));
                        const u32 pixelSeed = SamplerDetail::HashCombine(0x979u + block * 1211u, y * kWidth + x);
                        for (u32 sample = 0; sample < cpuSamples; ++sample)
                        {
                            PathSampler wholeSampler(pixelSeed, sample);
                            PathSampler directSampler(pixelSeed, sample);
                            settings.MaxBounces = 5u;
                            const glm::vec3 whole = PathTracer::TracePath(rig.Scene, primary, settings, wholeSampler);
                            settings.MaxBounces = 2u;
                            const glm::vec3 direct = PathTracer::TracePath(rig.Scene, primary, settings, directSampler);
                            const glm::vec3 indirect = whole - direct;
                            cpuMeans[block] += glm::dot(glm::dvec3(indirect), glm::dvec3(0.2126, 0.7152, 0.0722));
                        }
                    }
                }
                cpuMeans[block] /= static_cast<f64>(kPixels * cpuSamples);
                rig.Values.Debug.y = static_cast<f32>(1211u + block * 1229u);
                rig.Values.TlasAddressAndFrame.w = 0u;
                ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 3u, 3u));
                for (u32 frame = 0; frame < framesPerBlock; ++frame)
                {
                    rig.Values.TlasAddressAndFrame.w = frame + 1u;
                    ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 0u, 0u));
                    ASSERT_NO_FATAL_FAILURE(Draw(rig, 3u, 0u, 1u));
                    std::vector<glm::vec4> radiance;
                    ASSERT_NO_FATAL_FAILURE(ReadRadiance(rig, radiance));
                    for (const auto& pixel : radiance)
                    {
                        ASSERT_TRUE(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z));
                        gpuMeans[block] += glm::dot(glm::dvec3(pixel), glm::dvec3(0.2126, 0.7152, 0.0722));
                    }
                    ASSERT_NO_FATAL_FAILURE(Draw(rig, 1u, 0u, 1u, 0u, 3u));
                    ASSERT_NO_FATAL_FAILURE(Draw(rig, 2u, 1u, 2u, 1u, 3u));
                    ASSERT_NO_FATAL_FAILURE(Draw(rig, 3u, 2u, 1u, 1u, 3u));
                    ASSERT_NO_FATAL_FAILURE(ReadRadiance(rig, radiance));
                    for (const auto& pixel : radiance)
                    {
                        ASSERT_TRUE(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z));
                        combinedMeans[block] += glm::dot(glm::dvec3(pixel), glm::dvec3(0.2126, 0.7152, 0.0722));
                    }
                    // Preserve only this frame's fresh initial proposals as next
                    // temporal input. Mixed temporal/spatial winners never become
                    // the next frame's canonical history source.
                    std::swap(rig.Paths[0], rig.Paths[3]);
                }
                gpuMeans[block] /= static_cast<f64>(kPixels * framesPerBlock);
                combinedMeans[block] /= static_cast<f64>(kPixels * framesPerBlock);
            }
            f64 expected = 0.0;
            for (u32 block = 0; block < blocks; ++block)
                expected += cpuMeans[block] / blocks;
            ASSERT_GT(expected, 0.01);
            for (const bool combined : { false, true })
            {
                const auto& observedBlocks = combined ? combinedMeans : gpuMeans;
                f64 observed = 0.0;
                for (const f64 mean : observedBlocks)
                    observed += mean / blocks;
                f64 variance = 0.0;
                for (u32 block = 0; block < blocks; ++block)
                    variance += std::pow(cpuMeans[block] - expected, 2.0) + std::pow(observedBlocks[block] - observed, 2.0);
                const f64 standardError = std::sqrt(variance / (blocks * (blocks - 1u)));
                ASSERT_LT(standardError, expected * 0.03)
                    << "Oracle must resolve the indirect signal; noisy evidence is not a wider error budget";
                EXPECT_NEAR(observed, expected, 4.0 * standardError + 5.0e-4)
                    << (combined ? "temporal plus spatial MIS" : "initial proposals");
                std::cout << "[ReSTIR PT closed Cornell] glossy=" << glossy << " combined=" << combined
                          << " reference=" << expected << " observed=" << observed
                          << " combined standard error=" << standardError << '\n';
            }
            const auto paths = ReadPaths(rig, 3u);
            u32 secondary = 0u;
            for (const auto& path : paths)
                secondary += ((path.Lineage.w & 2u) != 0u && path.Metadata.x > 0u) ? 1u : 0u;
            ASSERT_GT(secondary, 16u);
            const auto counters = ReadCounters(rig);
            EXPECT_GT(counters[1], 0u) << "GPU NEE must actually trace visibility rays";
            EXPECT_GT(counters[4], 0u);
            EXPECT_GT(counters[5], 0u);
            EXPECT_GT(counters[6], 0u);
            EXPECT_GT(counters[7], 0u);
            EXPECT_GT(counters[8], 0u);
            EXPECT_EQ(counters[13], 0u);
            if (glossy)
                EXPECT_GT(counters[12], 0u);
        }
    }

    TEST_F(ReSTIRPTDevice, DeviceNegativeControlsExerciseNonIdentityJacobianAndReplayMappings)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        Rig rig;
        ASSERT_NO_FATAL_FAILURE(BuildRig(rig, false, 0.22f, 0.8f, true));
        std::ifstream input("assets/shaders/ReSTIR_PT.glsl", std::ios::binary);
        ASSERT_TRUE(input.good());
        const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto vertexMarker = source.find("#type vertex");
        const auto fragmentMarker = source.find("#type fragment");
        ASSERT_NE(vertexMarker, std::string::npos);
        ASSERT_NE(fragmentMarker, std::string::npos);
        const auto vertexStart = source.find('\n', vertexMarker) + 1u;
        const auto fragmentStart = source.find('\n', fragmentMarker) + 1u;
        const std::string vertex = source.substr(vertexStart, fragmentMarker - vertexStart);
        const std::string fragment = source.substr(fragmentStart);
        const auto versionEnd = fragment.find('\n');
        ASSERT_NE(versionEnd, std::string::npos);
        std::array<Ref<Shader>, 3> programs;
        programs[0] = rig.Program;
        const std::array<const char*, 2> defines{ "OLO_RESTIR_PT_TEST_OMIT_JACOBIAN", "OLO_RESTIR_PT_TEST_OMIT_MAPPING" };
        for (sizet i = 0; i < defines.size(); ++i)
        {
            std::string variant = fragment;
            variant.insert(versionEnd + 1u, std::string("#define ") + defines[i] + " 1\n");
            programs[i + 1u] = Shader::Create(std::string("ReSTIRPTDevice_") + defines[i], vertex, variant);
            ASSERT_TRUE(programs[i + 1u] && programs[i + 1u]->IsReady()) << defines[i];
        }
        // Compile variants use the production shader's explicit test hooks;
        // forward/inverse/domain checks run before either deliberate corruption.
        // No runtime setting or ordinary shader changes its estimator here.
        rig.Values.EstimatorParams.w = 6.0f;
        ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 0u, 0u));
        for (const u32 mapping : { 1u, 2u, 4u })
        {
            SCOPED_TRACE(mapping);
            rig.Values.Counts.z = mapping;
            std::array<std::vector<glm::vec4>, 3> images;
            for (sizet variant = 0; variant < programs.size(); ++variant)
            {
                rig.Program = programs[variant];
                ASSERT_NO_FATAL_FAILURE(Draw(rig, 2u, 0u, 2u, 0u));
                if (variant == 0u)
                {
                    const auto paths = ReadPaths(rig, 2u);
                    const auto nonidentity = std::count_if(paths.begin(), paths.end(), [](const PT::PathRecord& path)
                                                           { return (path.Lineage.w & 2u) != 0u && std::abs(path.State.w) > 0.01f; });
                    ASSERT_GT(nonidentity, 0) << "A Jacobian ablation on identity shifts would prove nothing";
                }
                ASSERT_NO_FATAL_FAILURE(Draw(rig, 3u, 2u, 1u, 0u));
                ASSERT_NO_FATAL_FAILURE(ReadRadiance(rig, images[variant]));
            }
            for (sizet variant = 1; variant < images.size(); ++variant)
            {
                f64 squaredDifference = 0.0;
                for (u32 pixel = 0; pixel < kPixels; ++pixel)
                {
                    const glm::dvec3 difference = glm::dvec3(images[variant][pixel]) - glm::dvec3(images[0][pixel]);
                    ASSERT_TRUE(std::isfinite(difference.x) && std::isfinite(difference.y) && std::isfinite(difference.z));
                    squaredDifference += glm::dot(difference, difference);
                }
                const f64 rms = std::sqrt(squaredDifference / (3.0 * kPixels));
                EXPECT_GT(rms, 5.0e-5) << "The deliberately broken device estimator must change the HDR answer";
                std::cout << "[ReSTIR PT negative control] map=" << mapping << " variant=" << variant
                          << " RMS change=" << rms << '\n';
            }
        }
    }

    TEST_F(ReSTIRPTDevice, DiffuseAndNearSpecularEnvironmentAgreeWithTheWholeReferenceIntegral)
    {
        ScopedVulkanRenderCommandSelection vulkanBackend;
        // This scene has no emissive or analytic lights and no secondary hit.
        // Consequently the whole CPU tracer integral IS the PT ambient term;
        // there is no unsupported subtraction of a depth-one reference image.
        // Independent sampler streams justify a statistical frame-mean budget,
        // unlike same-seed pathwise GPU-tracer parity. 16*64*256 proposals retain
        // roughly 32768 nonzero n=0/BSDF strata; a 3% budget is conservative for
        // the approximately 0.5% stratum-count standard error. No clamps are used.
        for (const bool metal : { false, true })
        {
            SCOPED_TRACE(metal ? "near-specular metal" : "diffuse dielectric");
            Rig rig;
            ASSERT_NO_FATAL_FAILURE(BuildRig(rig, false, metal ? 0.08f : 0.5f, metal ? 1.0f : 0.0f));
            PathTracerSettings settings;
            settings.SamplesPerPixel = 1024u;
            settings.MaxBounces = 5u;
            settings.RussianRouletteStartBounce = 0u;
            settings.MaxRadianceClamp = 0.0f;
            settings.Seed = 0x979u;
            ReferenceFilm reference(kWidth, kHeight);
            const glm::mat4 projection = glm::inverse(rig.Values.InvProjection);
            const auto camera = ReferenceCamera::FromViewProjection(projection * rig.Values.View, glm::vec3(0, 0, 2));
            PathTracer::Render(rig.Scene, camera, settings, reference);
            glm::dvec3 expected(0.0);
            for (const auto& pixel : reference.GetPixels())
                expected += glm::dvec3(pixel);
            expected /= static_cast<f64>(kPixels);
            glm::dvec3 observed(0.0);
            constexpr u32 frames = 16u;
            for (u32 frame = 0; frame < frames; ++frame)
            {
                rig.Values.TlasAddressAndFrame.w = frame + 1u;
                ASSERT_NO_FATAL_FAILURE(Draw(rig, 0u, 0u, 0u));
                ASSERT_NO_FATAL_FAILURE(Draw(rig, 3u, 0u, 1u));
                std::vector<glm::vec4> radiance;
                ASSERT_NO_FATAL_FAILURE(ReadRadiance(rig, radiance));
                for (const auto& pixel : radiance)
                {
                    ASSERT_TRUE(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z));
                    observed += glm::dvec3(pixel);
                }
            }
            observed /= static_cast<f64>(kPixels * frames);
            for (glm::length_t channel = 0; channel < 3; ++channel)
            {
                ASSERT_GT(expected[channel], 0.1);
                EXPECT_NEAR(observed[channel], expected[channel], expected[channel] * 0.03);
            }
            std::cout << "[ReSTIR PT furnace] metal=" << metal << " reference=" << expected.x << ','
                      << expected.y << ',' << expected.z << " observed=" << observed.x << ','
                      << observed.y << ',' << observed.z << '\n';
        }
    }
#endif
} // namespace OloEngine::Tests
