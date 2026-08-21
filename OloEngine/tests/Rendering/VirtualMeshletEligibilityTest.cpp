// OLO_TEST_LAYER: unit
//
// Mesh-shader raster eligibility for virtual geometry (issue #813).
//
// The mesh-shader path renders ONE cluster per mesh workgroup, and
// VirtualMeshletGBuffer.glsl declares max_vertices/max_primitives as
// compile-time layout constants — so a cluster that exceeds them cannot be
// drawn by that pipeline at all. The contract under test:
//   * PackVirtualMeshForGpu writes each cluster's VertexCount into the GPU
//     record (it was padding before #813; SetMeshOutputsEXT needs it).
//   * IsMeshletCompatible answers "does EVERY cluster fit one workgroup",
//     which the registry stamps per part and the draw loop routes on.
//   * The default cook config, the C++ limits, AND the shader's layout
//     constants (the OLO_MESHLET_MAX_* defines in
//     include/VirtualGeometryGpuStructs.glsl) all agree — the three-way
//     contract whose GLSL leg would otherwise be comment-only, and whose
//     failure mode is SetMeshOutputsEXT past the declared maxima: undefined
//     behavior only on mesh-capable Vulkan devices.
// All pure CPU — the GPU halves are covered by the Vulkan pass-suite tenant
// and the raster parity evidence.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"
#include "VirtualMeshFixtures.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred
using OloEngine::Tests::VirtualMeshFixtures::MakeIcosphereMesh;

// The shader's layout constants and the default cook config must agree, or
// default-cooked content silently loses the mesh path. If a future cook-config
// change trips this, either move the constants together or accept that default
// content routes to MDI — but do it knowingly.
TEST(VirtualMeshletEligibility, DefaultCookConfigMatchesTheDeclaredMeshletLimits)
{
    VirtualMeshBuildConfig const defaults;
    EXPECT_EQ(defaults.MaxClusterVertices, kMeshletMaxVertices);
    EXPECT_EQ(defaults.MaxClusterTriangles, kMeshletMaxTriangles);
}

// The launch ceiling is a PER-FRAME gate, not a per-part one, and these pin
// which is which. VK_EXT_mesh_shader guarantees only 65535 workgroups per grid
// dimension, and the task stage emits one per cluster the instance selected
// THIS FRAME — so the bound belongs on that live count. Folding it into
// IsMeshletCompatible (a per-part stamp over the whole cooked cluster set)
// would permanently demote large meshes that never select more than a few
// thousand clusters in any one frame, i.e. exactly the content the mesh path
// exists to serve. Boundary cases both ways, so neither the ceiling nor the
// comparison's strictness can drift silently.
TEST(VirtualMeshletEligibility, TheLaunchCeilingGatesTheFrameCountNotThePart)
{
    // At the ceiling: still the mesh path. One past it: MDI, no clamping and
    // therefore no silently dropped clusters.
    EXPECT_TRUE(ShouldUseMeshRaster(true, true, kMeshletMaxClustersPerInstance));
    EXPECT_FALSE(ShouldUseMeshRaster(true, true, kMeshletMaxClustersPerInstance + 1));

    // The ceiling is the SPEC minimum, not a queried device value: a generous
    // dev GPU must not be what decides whether min-spec content renders.
    EXPECT_EQ(kMeshletMaxClustersPerInstance, 65535u);

    // Zero clusters is a legal frame (nothing selected); it must not be treated
    // as an overflow, and EmitMeshTasksEXT(0) is a legal no-op launch.
    EXPECT_TRUE(ShouldUseMeshRaster(true, true, 0u));

    // Either of the other two gates alone still routes to MDI, whatever the
    // count — the availability flag is the effective route (mesh shader present
    // AND its program compiled), not raw device capability.
    EXPECT_FALSE(ShouldUseMeshRaster(false, true, 1u));
    EXPECT_FALSE(ShouldUseMeshRaster(true, false, 1u));
}

// The GLSL leg of the three-way contract: parse the OLO_MESHLET_MAX_* defines
// out of include/VirtualGeometryGpuStructs.glsl (the ONE GLSL spelling — the
// mesh stage's layout(max_vertices/max_primitives) consumes them) and pin them
// to the C++ constants. Without this, raising kMeshletMax* together with the
// cook config keeps every other test green while SetMeshOutputsEXT exceeds
// the shader's declared maxima on mesh-capable devices.
TEST(VirtualMeshletEligibility, ShaderLayoutConstantsMatchTheCppLimits)
{
    namespace fs = std::filesystem;
    const fs::path relative = fs::path("OloEditor") / "assets" / "shaders" / "include" / "VirtualGeometryGpuStructs.glsl";
    const fs::path candidates[] = {
        relative,
        fs::current_path() / relative,
        fs::current_path().parent_path() / relative,
    };
    fs::path shaderPath;
    for (const auto& candidate : candidates)
    {
        std::error_code ec;
        if (fs::exists(candidate, ec))
        {
            shaderPath = candidate;
            break;
        }
    }
    ASSERT_FALSE(shaderPath.empty()) << "VirtualGeometryGpuStructs.glsl not found (cwd = "
                                     << fs::current_path().generic_string() << ")";

    std::ifstream in(shaderPath);
    ASSERT_TRUE(in.is_open());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    const auto parseDefine = [&source](const char* name) -> long
    {
        const std::regex pattern(std::string("#define\\s+") + name + "\\s+(\\d+)");
        std::smatch match;
        if (!std::regex_search(source, match, pattern))
        {
            return -1;
        }
        return std::strtol(match[1].str().c_str(), nullptr, 10);
    };

    EXPECT_EQ(parseDefine("OLO_MESHLET_MAX_VERTICES"), static_cast<long>(kMeshletMaxVertices))
        << "the shader's max_vertices layout constant has drifted from kMeshletMaxVertices";
    EXPECT_EQ(parseDefine("OLO_MESHLET_MAX_PRIMITIVES"), static_cast<long>(kMeshletMaxTriangles))
        << "the shader's max_primitives layout constant has drifted from kMeshletMaxTriangles";
}

TEST(VirtualMeshletEligibility, PackWritesPerClusterVertexCounts)
{
    auto meshSource = MakeIcosphereMesh(3); // 1280 triangles, multi-cluster
    auto vm = VirtualMeshBuilder::Build(*meshSource);
    ASSERT_TRUE(vm.IsValid());
    auto packed = PackVirtualMeshForGpu(vm);
    ASSERT_TRUE(packed.IsValid());
    ASSERT_EQ(packed.Clusters.size(), vm.Clusters.size());

    sizet const clusterCount = packed.Clusters.size();
    for (sizet i = 0; i < clusterCount; ++i)
    {
        const VirtualClusterGpuRecord& record = packed.Clusters[i];
        EXPECT_EQ(record.VertexCount, vm.Clusters[i].VertexCount) << "cluster " << i;
        EXPECT_GT(record.VertexCount, 0u) << "cluster " << i;
        // SetMeshOutputsEXT(VertexCount, IndexCount / 3) must address only
        // cluster-owned data: every local index below VertexCount.
        EXPECT_EQ(record.IndexCount, vm.Clusters[i].TriangleCount * 3u) << "cluster " << i;
        for (u32 k = 0; k < record.IndexCount; ++k)
        {
            ASSERT_LT(packed.Indices[record.IndexBase + k], record.VertexCount)
                << "cluster " << i << " local index " << k;
        }
    }
}

TEST(VirtualMeshletEligibility, DefaultCookIsMeshletCompatible)
{
    auto meshSource = MakeIcosphereMesh(3);
    auto vm = VirtualMeshBuilder::Build(*meshSource);
    ASSERT_TRUE(vm.IsValid());
    auto packed = PackVirtualMeshForGpu(vm);
    ASSERT_TRUE(packed.IsValid());
    EXPECT_TRUE(IsMeshletCompatible(packed));
}

TEST(VirtualMeshletEligibility, OversizedCookConfigIsNotMeshletCompatible)
{
    VirtualMeshBuildConfig config;
    config.MaxClusterVertices = 256;
    config.MaxClusterTriangles = 384;

    auto meshSource = MakeIcosphereMesh(4); // 5120 triangles — enough to fill oversized clusters
    auto vm = VirtualMeshBuilder::Build(*meshSource, config);
    ASSERT_TRUE(vm.IsValid());
    auto packed = PackVirtualMeshForGpu(vm);
    ASSERT_TRUE(packed.IsValid());

    // Fixture sanity: the assertion below is vacuous unless the build actually
    // produced at least one cluster past the meshlet limits (the
    // cluster-lod-simplification.md rule — assert the fixture's premise or a
    // config change silently turns the test into a no-op).
    bool anyOversized = false;
    for (const VirtualClusterGpuRecord& record : packed.Clusters)
    {
        if (record.VertexCount > kMeshletMaxVertices || record.IndexCount > kMeshletMaxTriangles * 3u)
        {
            anyOversized = true;
            break;
        }
    }
    ASSERT_TRUE(anyOversized) << "fixture produced no cluster past the meshlet limits — "
                                 "raise the subdivision count or the cook config";

    EXPECT_FALSE(IsMeshletCompatible(packed));
}

TEST(VirtualMeshletEligibility, EmptyDataIsNotMeshletCompatible)
{
    EXPECT_FALSE(IsMeshletCompatible(VirtualMeshGpuData{}));
}
