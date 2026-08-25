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
#include <string>
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
#include "VirtualMeshFixtures.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

using OloEngine::Tests::VirtualMeshFixtures::MakeIcosphereMesh;
namespace
{

    // Shader-block prefix of UBOStructures::CameraUBO (see CameraMatrices in
    // VirtualClusterCull.comp — std140 allows the buffer to be exactly this).
    struct CameraBlock
    {
        glm::mat4 ViewProjection;
        glm::mat4 View;
        glm::mat4 Projection;
        glm::vec3 Position;
        f32 Pad0 = 0.0f;
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

    // VirtualClusterCull.comp's parameters (issue #691): one std140
    // block where the shader used to declare ~18 bare uniforms. Refilled before
    // each dispatch below.
    auto cullParamsUBO = UniformBuffer::Create(UBOStructures::VirtualClusterCullUBO::GetSize(),
                                               ShaderBindingLayout::UBO_VIRTUAL_CLUSTER_CULL);

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

    // TWO-SIDEDNESS (C5, issue #629) is part of the cull contract, not a rendering detail:
    // kFlagTwoSided EXEMPTS a cluster from the normal-cone backface rejection, because a
    // two-sided material (Sponza's foliage: single-quad leaf cards) is visible from behind.
    // The cull test used to set only kFlagUniformScale, so the CPU/GPU mirror could not catch
    // a regression on two-sided geometry at all — which is exactly what C5 was. Sweeping both
    // flag sets over the same mesh also makes the fixture prove the cone test is ACTIVE: the
    // two-sided run must select STRICTLY MORE clusters than the one-sided run (an icosphere
    // seen from outside has back-facing clusters), and if it ever does not, the cone rejection
    // has silently stopped doing anything and the one-sided half of this test is vacuous.
    struct FlagCase
    {
        const char* Name;
        u32 Flags;
        bool ConeCullActive;
    };
    const FlagCase flagCases[] = {
        { "one-sided", VirtualInstanceGpuRecord::kFlagUniformScale, true },
        { "two-sided",
          VirtualInstanceGpuRecord::kFlagUniformScale | VirtualInstanceGpuRecord::kFlagTwoSided, false },
    };
    // std::string key, not const char*: identical string literals are not guaranteed to be pooled
    // into the same pointer, and a pointer-keyed map would then silently look up nothing.
    std::map<std::pair<std::string, f32>, sizet> selectedCounts;

    const f32 thresholds[] = { 0.5f, 2.0f, 8.0f, 32.0f };
    for (const FlagCase& flagCase : flagCases)
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
            instance.Flags = flagCase.Flags;
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
            // The cull's parameters moved into the VirtualClusterCullParams
            // std140 block at UBO_VIRTUAL_CLUSTER_CULL (issue #691 —
            // GLSL-for-Vulkan forbids default-block uniforms). The block is
            // value-initialised, so every control this parity case does not set
            // (ortho mode, occlusion, the two-phase and debug flags) is a
            // deterministic 0 — which is exactly the single-phase, no-occlusion
            // configuration it means to test.
            UBOStructures::VirtualClusterCullUBO cullParams{};
            cullParams.InstanceIndex = 0;
            cullParams.ViewportHeight = kViewportHeight;
            cullParams.SwRasterThresholdPixels = 0.0f;
            cullParamsUBO->SetData(&cullParams, sizeof(cullParams));
            cullParamsUBO->Bind();
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
                // The cone test is SKIPPED for a two-sided instance: a two-sided material is
                // visible from behind, so "every triangle faces away" is not a reason to drop it.
                if (flagCase.ConeCullActive && ConeRejects(cluster, cameraPosition))
                {
                    continue;
                }
                expected.push_back(c);
            }
            selectedCounts[{ std::string(flagCase.Name), thresholdPixels }] = expected.size();

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
            EXPECT_EQ(actual, expected) << "GPU cull selection diverged from the CPU reference for a " << flagCase.Name
                                        << " instance at threshold " << thresholdPixels << "px (expected "
                                        << expected.size() << " clusters, got " << actual.size()
                                        << "). For a TWO-SIDED instance the normal-cone backface rejection must be "
                                           "SKIPPED — kFlagTwoSided (bit 2) has to reach the cull shader and be honoured "
                                           "there, or two-sided foliage loses every cluster whose triangles all face away.";

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

    // NON-VACUITY of the two-sided half: the cone cull must actually REJECT clusters somewhere
    // in this sweep, or "two-sided selects the same set" would be trivially true and the flag
    // could be ignored entirely without failing anything above.
    //
    // Not at EVERY threshold, though: a coarse cut selects the DAG's root clusters, whose
    // normal cones span most of a hemisphere (ConeCutoff >= 1 disables the test), so the cone
    // rejects nothing there — measured, 5 vs 5 clusters at 8px and 3 vs 3 at 32px. The
    // contract is: exempting the cone can only ADD clusters (never remove), and it must add
    // some at a fine cut, where the leaf clusters have tight cones and half of them face away.
    u32 thresholdsWhereConeCullBites = 0;
    for (f32 const thresholdPixels : thresholds)
    {
        sizet const oneSided = selectedCounts[{ std::string("one-sided"), thresholdPixels }];
        sizet const twoSided = selectedCounts[{ std::string("two-sided"), thresholdPixels }];
        EXPECT_GE(twoSided, oneSided) << "at threshold " << thresholdPixels
                                      << "px the TWO-SIDED instance selected FEWER clusters (" << twoSided << ") than "
                                      << "the one-sided one (" << oneSided
                                      << ") — skipping the cone test can only ever ADD survivors";
        thresholdsWhereConeCullBites += (twoSided > oneSided) ? 1u : 0u;
    }
    EXPECT_GT(thresholdsWhereConeCullBites, 0u)
        << "the normal-cone rejection culled NOTHING at any threshold on this fixture, so the kFlagTwoSided "
           "exemption is untested — the cull shader could ignore the flag entirely and every assertion above would "
           "still pass. An icosphere seen from outside MUST have back-facing leaf clusters; if it no longer does, "
           "the cone bounds or the fixture have changed and this test needs re-pointing.";

    // GL hygiene (issue #485): leave no dangling bindings for later tests.
    for (u32 slot = ShaderBindingLayout::SSBO_VIRTUAL_CLUSTERS; slot <= ShaderBindingLayout::SSBO_VIRTUAL_VERTICES; ++slot)
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, 0);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_CAMERA, 0);
    glUseProgram(0);
}

// -----------------------------------------------------------------------------
// Issue #862: the software-raster work-list append (`atomicAdd(swList.Count, 1);
// swList.Records[swSlot] = ...`) used to have NO bounds check at all, unlike the
// reject-list append a few lines below it in the same shader
// (`if (slot < u_RejectCapacity)`). VirtualGeometryPass now sizes the SW-list
// buffer's Records array to exactly `u_SwCapacity` (the frame's cluster count)
// and the shader only writes when `swSlot < u_SwCapacity` — but that guard is
// only proven by dispatching against a REAL, deliberately undersized buffer:
// the math-only VirtualClusterTwoPhaseOcclusion.SwListAllocationHolds... test
// proves the allocation matches the bound, not that the shader honours it.
//
// This forces every eligible cluster to route to the SW path (a huge
// threshold) against a Records[] array sized to HALF the cluster count, so the
// append genuinely overflows capacity. A live GPU run that corrupted memory
// here would show up as garbage InstanceIndex/ClusterIndex values in the
// records that DID get written, or as a GL error/crash — this is the actual
// defect class #862's Vulkan device fault came from, exercised for real.
// -----------------------------------------------------------------------------
TEST(VirtualClusterCullParity, SwListAppendNeverWritesPastAnUndersizedCapacity)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto meshSource = MakeIcosphereMesh(4); // 5120 triangles, multi-level DAG
    auto vm = VirtualMeshBuilder::Build(*meshSource);
    ASSERT_TRUE(vm.IsValid());
    auto packed = PackVirtualMeshForGpu(vm);
    ASSERT_TRUE(packed.IsValid());

    auto const clusterCount = static_cast<u32>(packed.Clusters.size());
    ASSERT_GT(clusterCount, 1u);

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

    auto cullParamsUBO = UniformBuffer::Create(UBOStructures::VirtualClusterCullUBO::GetSize(),
                                               ShaderBindingLayout::UBO_VIRTUAL_CLUSTER_CULL);

    auto clusterBuffer = StorageBuffer::Create(static_cast<u32>(packed.Clusters.size() * sizeof(VirtualClusterGpuRecord)),
                                               ShaderBindingLayout::SSBO_VIRTUAL_CLUSTERS, StorageBufferUsage::DynamicDraw);
    clusterBuffer->SetData(packed.Clusters.data(), static_cast<u32>(packed.Clusters.size() * sizeof(VirtualClusterGpuRecord)), 0);
    auto groupBuffer = StorageBuffer::Create(static_cast<u32>(packed.Groups.size() * sizeof(VirtualGroupGpuRecord)),
                                             ShaderBindingLayout::SSBO_VIRTUAL_GROUPS, StorageBufferUsage::DynamicDraw);
    groupBuffer->SetData(packed.Groups.data(), static_cast<u32>(packed.Groups.size() * sizeof(VirtualGroupGpuRecord)), 0);

    std::vector<u32> const allResident(packed.Groups.size(), 1u);
    auto groupStatesBuffer = StorageBuffer::Create(static_cast<u32>(allResident.size() * sizeof(u32)),
                                                   ShaderBindingLayout::SSBO_VIRTUAL_GROUP_STATES,
                                                   StorageBufferUsage::DynamicCopy);
    groupStatesBuffer->SetData(allResident.data(), static_cast<u32>(allResident.size() * sizeof(u32)), 0);

    auto commandBuffer = StorageBuffer::Create(clusterCount * 32u,
                                               ShaderBindingLayout::SSBO_VIRTUAL_DRAW_COMMANDS, StorageBufferUsage::DynamicCopy);
    auto argsBuffer = StorageBuffer::Create(sizeof(VirtualDrawArgs),
                                            ShaderBindingLayout::SSBO_VIRTUAL_DRAW_ARGS, StorageBufferUsage::DynamicCopy);
    auto visibleBuffer = StorageBuffer::Create(clusterCount * static_cast<u32>(sizeof(VirtualVisibleCluster)),
                                               ShaderBindingLayout::SSBO_VIRTUAL_VISIBLE, StorageBufferUsage::DynamicCopy);
    auto instanceBuffer = StorageBuffer::Create(sizeof(VirtualInstanceGpuRecord),
                                                ShaderBindingLayout::SSBO_VIRTUAL_INSTANCES, StorageBufferUsage::DynamicDraw);

    VirtualInstanceGpuRecord instance;
    instance.ClusterBase = 0;
    instance.ClusterCount = clusterCount;
    instance.GroupBase = 0;
    instance.EntityID = 42;
    instance.MaxScale = 1.0f;
    instance.ErrorThresholdPixels = 0.5f; // fine cut: select (almost) every leaf cluster
    instance.CommandBase = 0;
    instance.Flags = VirtualInstanceGpuRecord::kFlagUniformScale;
    instanceBuffer->SetData(&instance, sizeof(instance), 0);

    auto cullShader = ComputeShader::Create("assets/shaders/compute/VirtualClusterCull.comp");
    ASSERT_TRUE(cullShader);

    clusterBuffer->Bind();
    groupBuffer->Bind();
    instanceBuffer->Bind();
    commandBuffer->Bind();
    argsBuffer->Bind();
    visibleBuffer->Bind();
    groupStatesBuffer->Bind();
    cameraUBO->Bind();
    cullShader->Bind();

    UBOStructures::VirtualClusterCullUBO cullParams{};
    cullParams.InstanceIndex = 0;
    cullParams.ViewportHeight = kViewportHeight;
    // Huge threshold: every surviving cluster's projected radius is under it,
    // so every one of them attempts to route to the SW list.
    cullParams.SwRasterThresholdPixels = 1.0e6f;

    // Pass 1 — CALIBRATION: dispatch against a generously-sized SW list (capacity ==
    // clusterCount, guaranteed never to overflow) purely to measure how many clusters
    // this camera/threshold combination actually routes to the SW path. Hardcoding an
    // expected count would silently stop testing the overflow path if the icosphere
    // fixture or cull math ever changes.
    {
        auto calibrationSwList = StorageBuffer::Create(16u + clusterCount * static_cast<u32>(sizeof(VirtualVisibleCluster)),
                                                       ShaderBindingLayout::SSBO_VIRTUAL_SW_LIST,
                                                       StorageBufferUsage::DynamicCopy);
        calibrationSwList->Bind();
        VirtualDrawArgs const zeroArgs{};
        argsBuffer->SetData(&zeroArgs, sizeof(zeroArgs), 0);
        cullParams.SwCapacity = clusterCount;
        cullParamsUBO->SetData(&cullParams, sizeof(cullParams));
        cullParamsUBO->Bind();
        RenderCommand::DispatchCompute((clusterCount + 63u) / 64u, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    VirtualDrawArgs calibrationArgs{};
    argsBuffer->GetData(&calibrationArgs, sizeof(calibrationArgs), 0);
    ASSERT_EQ(calibrationArgs.TestedCount, clusterCount);
    u32 const eligible = calibrationArgs.SwCount + calibrationArgs.DrawCount;
    ASSERT_GT(eligible, 1u) << "test setup must route at least 2 clusters to exercise the overflow path — widen "
                               "ErrorThresholdPixels";

    // Deliberately smaller than what pass 1 measured: with every eligible cluster
    // still routed to SW below, appends WILL exceed this.
    u32 const swCapacity = eligible / 2u;
    ASSERT_GT(swCapacity, 0u);
    ASSERT_LT(swCapacity, eligible);

    // Pass 2 — the buffer under test: sized to EXACTLY swCapacity records, not
    // clusterCount — an unguarded append would write past this allocation.
    auto swListBuffer = StorageBuffer::Create(16u + swCapacity * static_cast<u32>(sizeof(VirtualVisibleCluster)),
                                              ShaderBindingLayout::SSBO_VIRTUAL_SW_LIST, StorageBufferUsage::DynamicCopy);
    swListBuffer->Bind();
    VirtualDrawArgs const zeroArgs{};
    argsBuffer->SetData(&zeroArgs, sizeof(zeroArgs), 0);
    cullParams.SwCapacity = swCapacity;
    cullParamsUBO->SetData(&cullParams, sizeof(cullParams));
    cullParamsUBO->Bind();
    RenderCommand::DispatchCompute((clusterCount + 63u) / 64u, 1, 1);
    RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "cull dispatch against an undersized SW-list buffer "
                                                                 "raised a GL error — the append overran it";

    VirtualDrawArgs args{};
    argsBuffer->GetData(&args, sizeof(args), 0);
    ASSERT_EQ(args.TestedCount, clusterCount);
    ASSERT_EQ(args.SwCount + args.DrawCount, eligible)
        << "the same camera/threshold must select the same eligible-cluster count as pass 1's calibration run";

    // The guard must have accepted EXACTLY swCapacity records: every append
    // past that must have fallen through to the hardware path instead
    // (args.DrawCount), never dropped and never written out of bounds.
    EXPECT_EQ(args.SwCount, swCapacity) << "the SW list accepted a different count than its capacity — either the "
                                           "guard let an overflow through, or it rejected a record that should "
                                           "have fit";
    EXPECT_EQ(args.DrawCount, eligible - swCapacity)
        << "clusters rejected by the full SW list must fall through to the hardware compaction path, not vanish";

    // Every record the shader actually wrote must be sane — an OOB write from
    // a broken guard would show up here as a corrupted ClusterIndex/InstanceIndex
    // (or would already have faulted the GL error check above).
    std::vector<VirtualVisibleCluster> swRecords(swCapacity);
    swListBuffer->GetData(swRecords.data(), swCapacity * static_cast<u32>(sizeof(VirtualVisibleCluster)), 16u);
    for (const VirtualVisibleCluster& record : swRecords)
    {
        EXPECT_EQ(record.InstanceIndex, 0u);
        EXPECT_LT(record.ClusterIndex, clusterCount);
    }

    for (u32 slot = ShaderBindingLayout::SSBO_VIRTUAL_CLUSTERS; slot <= ShaderBindingLayout::SSBO_VIRTUAL_VERTICES; ++slot)
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, 0);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_CAMERA, 0);
    glUseProgram(0);
}
