// OLO_TEST_LAYER: shaderpipe
//
// GPU/CPU parity for the virtualized-geometry cluster cull (issue #629).
// VirtualClusterCull.comp implements the DAG-cut selection that the CPU
// reference VirtualMesh::IsClusterSelectedProjected defines, plus frustum
// (Frustum::Update contract) and normal-cone rejection (the sphere-based
// formula documented on meshopt_computeClusterBounds). This test uploads a
// real built VirtualMesh, dispatches the production compute shader, reads the
// compacted survivors back, and requires the exact same cluster set the CPU
// predicts — at several error thresholds. A drifting shader (or a broken
// std430 mirror) flips this test.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include <glad/gl.h>

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"
#include "PropertyTests/RenderPropertyTest.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // Same icosphere fixture as VirtualMeshBuilderTest (kept local: test files
    // must stay self-contained; the mesh is procedural either way).
    Ref<MeshSource> MakeIcosphereMesh(u32 subdivisions)
    {
        const f32 t = (1.0f + std::sqrt(5.0f)) / 2.0f;
        std::vector<glm::vec3> positions = {
            { -1.0f, t, 0.0f },
            { 1.0f, t, 0.0f },
            { -1.0f, -t, 0.0f },
            { 1.0f, -t, 0.0f },
            { 0.0f, -1.0f, t },
            { 0.0f, 1.0f, t },
            { 0.0f, -1.0f, -t },
            { 0.0f, 1.0f, -t },
            { t, 0.0f, -1.0f },
            { t, 0.0f, 1.0f },
            { -t, 0.0f, -1.0f },
            { -t, 0.0f, 1.0f },
        };
        for (auto& p : positions)
        {
            p = glm::normalize(p);
        }
        std::vector<u32> indices = {
            0,
            11,
            5,
            0,
            5,
            1,
            0,
            1,
            7,
            0,
            7,
            10,
            0,
            10,
            11,
            1,
            5,
            9,
            5,
            11,
            4,
            11,
            10,
            2,
            10,
            7,
            6,
            7,
            1,
            8,
            3,
            9,
            4,
            3,
            4,
            2,
            3,
            2,
            6,
            3,
            6,
            8,
            3,
            8,
            9,
            4,
            9,
            5,
            2,
            4,
            11,
            6,
            2,
            10,
            8,
            6,
            7,
            9,
            8,
            1,
        };
        for (u32 s = 0; s < subdivisions; ++s)
        {
            std::map<std::pair<u32, u32>, u32> midpointCache;
            auto midpoint = [&](u32 a, u32 b) -> u32
            {
                std::pair<u32, u32> const key = std::minmax(a, b);
                if (auto it = midpointCache.find(key); it != midpointCache.end())
                {
                    return it->second;
                }
                auto index = static_cast<u32>(positions.size());
                positions.push_back(glm::normalize((positions[a] + positions[b]) * 0.5f));
                midpointCache.emplace(key, index);
                return index;
            };
            std::vector<u32> next;
            next.reserve(indices.size() * 4);
            for (sizet i = 0; i + 2 < indices.size(); i += 3)
            {
                u32 const a = indices[i];
                u32 const b = indices[i + 1];
                u32 const c = indices[i + 2];
                u32 const ab = midpoint(a, b);
                u32 const bc = midpoint(b, c);
                u32 const ca = midpoint(c, a);
                for (u32 idx : { a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca })
                {
                    next.push_back(idx);
                }
            }
            indices = std::move(next);
        }

        TArray<Vertex> vertices;
        vertices.Reserve(static_cast<i32>(positions.size()));
        constexpr f32 kPi = 3.14159265358979323846f;
        for (const glm::vec3& p : positions)
        {
            glm::vec2 const uv{ std::atan2(p.z, p.x) / (2.0f * kPi) + 0.5f,
                                std::asin(std::clamp(p.y, -1.0f, 1.0f)) / kPi + 0.5f };
            vertices.Add(Vertex(p, p, uv));
        }
        TArray<u32> meshIndices;
        meshIndices.Reserve(static_cast<i32>(indices.size()));
        for (u32 const index : indices)
        {
            meshIndices.Add(index);
        }
        return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(meshIndices));
    }

    // Shader-block prefix of UBOStructures::CameraUBO (see CameraMatrices in
    // VirtualClusterCull.comp — std140 allows the buffer to be exactly this).
    struct CameraBlock
    {
        glm::mat4 ViewProjection;
        glm::mat4 View;
        glm::mat4 Projection;
        glm::vec3 Position;
        f32 _Pad0 = 0.0f;
    };

    struct GpuDrawCommand
    {
        u32 Count, InstanceCount, FirstIndex, BaseVertex, BaseInstance, _P0, _P1, _P2;
    };

    // CPU reference of the sphere-based cone rejection documented on
    // meshopt_computeClusterBounds (meshoptimizer.h): reject when
    //   dot(center - camera, axis) >= cutoff * |center - camera| + radius.
    bool ConeRejects(const VirtualCluster& cluster, const glm::vec3& cameraPosition)
    {
        if (cluster.ConeCutoff >= 1.0f)
        {
            return false;
        }
        glm::vec3 const toCluster = cluster.BoundsCenter - cameraPosition;
        return glm::dot(toCluster, cluster.ConeAxis) >=
               cluster.ConeCutoff * glm::length(toCluster) + cluster.BoundsRadius;
    }
} // namespace

TEST(VirtualClusterCullParity, GpuSelectionMatchesCpuReferenceAtEveryThreshold)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto meshSource = MakeIcosphereMesh(4); // 5120 triangles, multi-level DAG
    auto vm = VirtualMeshBuilder::Build(*meshSource);
    ASSERT_TRUE(vm.IsValid());
    auto packed = PackVirtualMeshForGpu(vm);
    ASSERT_TRUE(packed.IsValid());

    auto const clusterCount = static_cast<u32>(packed.Clusters.size());

    // Camera: unit sphere fully in view from 4 units away
    constexpr f32 kZNear = 0.1f;
    constexpr f32 kViewportHeight = 1080.0f;
    glm::vec3 const cameraPosition{ 0.0f, 0.5f, 4.0f };
    CameraBlock camera;
    camera.Projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, kZNear, 100.0f);
    camera.View = glm::lookAt(cameraPosition, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    camera.ViewProjection = camera.Projection * camera.View;
    camera.Position = cameraPosition;

    auto cameraUBO = UniformBuffer::Create(sizeof(CameraBlock), ShaderBindingLayout::UBO_CAMERA);
    cameraUBO->SetData(&camera, sizeof(CameraBlock));

    // Static pools
    auto clusterBuffer = StorageBuffer::Create(static_cast<u32>(packed.Clusters.size() * sizeof(VirtualClusterGpuRecord)),
                                               ShaderBindingLayout::SSBO_VIRTUAL_CLUSTERS, StorageBufferUsage::DynamicDraw);
    clusterBuffer->SetData(packed.Clusters.data(), static_cast<u32>(packed.Clusters.size() * sizeof(VirtualClusterGpuRecord)), 0);
    auto groupBuffer = StorageBuffer::Create(static_cast<u32>(packed.Groups.size() * sizeof(VirtualGroupGpuRecord)),
                                             ShaderBindingLayout::SSBO_VIRTUAL_GROUPS, StorageBufferUsage::DynamicDraw);
    groupBuffer->SetData(packed.Groups.data(), static_cast<u32>(packed.Groups.size() * sizeof(VirtualGroupGpuRecord)), 0);

    // Streaming states (slice 5): all pages resident so the residency clamp is
    // inert and the parity contract stays purely about the cut + culling math.
    std::vector<u32> const allResident(packed.Groups.size(), 1u);
    auto groupStatesBuffer = StorageBuffer::Create(static_cast<u32>(allResident.size() * sizeof(u32)),
                                                   ShaderBindingLayout::SSBO_VIRTUAL_GROUP_STATES,
                                                   StorageBufferUsage::DynamicCopy);
    groupStatesBuffer->SetData(allResident.data(), static_cast<u32>(allResident.size() * sizeof(u32)), 0);

    // Software-raster list bound but routing disabled (threshold 0)
    auto swListBuffer = StorageBuffer::Create(16u + clusterCount * static_cast<u32>(sizeof(VirtualVisibleCluster)),
                                              ShaderBindingLayout::SSBO_VIRTUAL_SW_LIST, StorageBufferUsage::DynamicCopy);

    auto commandBuffer = StorageBuffer::Create(clusterCount * 32u,
                                               ShaderBindingLayout::SSBO_VIRTUAL_DRAW_COMMANDS, StorageBufferUsage::DynamicCopy);
    auto argsBuffer = StorageBuffer::Create(sizeof(VirtualDrawArgs),
                                            ShaderBindingLayout::SSBO_VIRTUAL_DRAW_ARGS, StorageBufferUsage::DynamicCopy);
    auto visibleBuffer = StorageBuffer::Create(clusterCount * static_cast<u32>(sizeof(VirtualVisibleCluster)),
                                               ShaderBindingLayout::SSBO_VIRTUAL_VISIBLE, StorageBufferUsage::DynamicCopy);
    auto instanceBuffer = StorageBuffer::Create(sizeof(VirtualInstanceGpuRecord),
                                                ShaderBindingLayout::SSBO_VIRTUAL_INSTANCES, StorageBufferUsage::DynamicDraw);

    auto cullShader = ComputeShader::Create("assets/shaders/compute/VirtualClusterCull.comp");
    ASSERT_TRUE(cullShader);

    Frustum frustum;
    frustum.Update(camera.ViewProjection);

    const f32 thresholds[] = { 0.5f, 2.0f, 8.0f, 32.0f };
    for (f32 const thresholdPixels : thresholds)
    {
        // Identity transform => world space == mesh space, uniform scale
        VirtualInstanceGpuRecord instance;
        instance.ClusterBase = 0;
        instance.ClusterCount = clusterCount;
        instance.GroupBase = 0;
        instance.EntityID = 42;
        instance.MaxScale = 1.0f;
        instance.ErrorThresholdPixels = thresholdPixels;
        instance.CommandBase = 0;
        instance.Flags = VirtualInstanceGpuRecord::kFlagUniformScale;
        instanceBuffer->SetData(&instance, sizeof(instance), 0);

        VirtualDrawArgs const zeroArgs{};
        argsBuffer->SetData(&zeroArgs, sizeof(zeroArgs), 0);

        clusterBuffer->Bind();
        groupBuffer->Bind();
        instanceBuffer->Bind();
        commandBuffer->Bind();
        argsBuffer->Bind();
        visibleBuffer->Bind();
        groupStatesBuffer->Bind();
        swListBuffer->Bind();
        cameraUBO->Bind();

        cullShader->Bind();
        cullShader->SetUint("u_InstanceIndex", 0);
        cullShader->SetFloat("u_ViewportHeight", kViewportHeight);
        cullShader->SetFloat("u_SwRasterThresholdPixels", 0.0f);
        RenderCommand::DispatchCompute((clusterCount + 63u) / 64u, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // CPU prediction: DAG cut (production reference) + frustum + cone
        f32 const projectionScale = camera.Projection[1][1];
        std::vector<u32> expected;
        for (u32 c = 0; c < clusterCount; ++c)
        {
            // The CPU reference works in threshold units of [0..1] screen
            // height; the GPU works in pixels — divide to match.
            if (!vm.IsClusterSelectedProjected(c, cameraPosition, kZNear, projectionScale,
                                               thresholdPixels / kViewportHeight))
            {
                continue;
            }
            const VirtualCluster& cluster = vm.Clusters[c];
            if (!frustum.IsSphereVisible(cluster.BoundsCenter, cluster.BoundsRadius))
            {
                continue;
            }
            if (ConeRejects(cluster, cameraPosition))
            {
                continue;
            }
            expected.push_back(c);
        }

        VirtualDrawArgs args{};
        argsBuffer->GetData(&args, sizeof(args), 0);
        ASSERT_EQ(args.TestedCount, clusterCount) << "threshold " << thresholdPixels;

        std::vector<VirtualVisibleCluster> visible(args.DrawCount);
        if (args.DrawCount > 0)
        {
            visibleBuffer->GetData(visible.data(), args.DrawCount * static_cast<u32>(sizeof(VirtualVisibleCluster)), 0);
        }

        std::vector<u32> actual;
        actual.reserve(visible.size());
        for (const VirtualVisibleCluster& record : visible)
        {
            EXPECT_EQ(record.InstanceIndex, 0u);
            actual.push_back(record.ClusterIndex);
        }
        std::ranges::sort(actual);
        std::ranges::sort(expected);
        EXPECT_EQ(actual, expected) << "GPU cull selection diverged from the CPU reference at threshold "
                                    << thresholdPixels << "px (expected " << expected.size() << " clusters, got "
                                    << actual.size() << ")";

        ASSERT_FALSE(expected.empty()) << "test setup must keep the mesh visible at threshold " << thresholdPixels;

        // Command records must route each survivor's geometry window
        std::vector<GpuDrawCommand> commands(args.DrawCount);
        commandBuffer->GetData(commands.data(), args.DrawCount * static_cast<u32>(sizeof(GpuDrawCommand)), 0);
        for (u32 slot = 0; slot < args.DrawCount; ++slot)
        {
            const VirtualClusterGpuRecord& record = packed.Clusters[visible[slot].ClusterIndex];
            EXPECT_EQ(commands[slot].Count, record.IndexCount);
            EXPECT_EQ(commands[slot].InstanceCount, 1u);
            EXPECT_EQ(commands[slot].FirstIndex, record.IndexBase);
            EXPECT_EQ(commands[slot].BaseVertex, record.VertexBase);
            EXPECT_EQ(commands[slot].BaseInstance, slot);
        }
    }

    // GL hygiene (issue #485): leave no dangling bindings for later tests.
    for (u32 slot = ShaderBindingLayout::SSBO_VIRTUAL_CLUSTERS; slot <= ShaderBindingLayout::SSBO_VIRTUAL_VERTICES; ++slot)
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, 0);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_CAMERA, 0);
    glUseProgram(0);
}
