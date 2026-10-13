#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// UsdStageImportTest — a real OpenUSD stage, opened and imported (#1575).
//
// MeshInterchangeTest pins the registry's dispatch with a stub importer; this
// opens an actual .usda through the extension registry, so UsdStage::Open, the
// statically registered Sdf file formats, the plugInfo tree staged next to the
// test binary and USD's oneTBB work dispatch all run. No test imported a stage
// before, so a USD build that linked and then failed to open anything would
// have passed the suite. It was written for #1575, which moved USD onto vcpkg's
// oneTBB, and it runs in Debug and Release.
//
// Compiled only when OLO_WITH_USD is on: OloEngine/tests/CMakeLists.txt gives
// this one TU the define, the way OloEngine/CMakeLists.txt does for the
// importer's own TUs (#1386).
// =============================================================================

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Asset/Interchange/MeshImporterRegistry.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>

#if defined(OLO_WITH_USD)

using namespace OloEngine;

namespace
{
    // One quad standing in the stage's XZ plane, authored Z-up in centimetres,
    // with vertex-interpolated UVs and no normals: every conversion the importer
    // owns -- unit scale, up axis, the UV origin, triangulation and the generated
    // face normal -- has a distinct expected value.
    constexpr std::string_view kQuadStage = R"(#usda 1.0
(
    upAxis = "Z"
    metersPerUnit = 0.01
)

def Mesh "Quad"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, 0, 0), (100, 0, 0), (100, 0, 100), (0, 0, 100)]
    texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
        interpolation = "vertex"
    )
}
)";

    void ExpectVec3Near(const glm::vec3& actual, const glm::vec3& expected, const char* what)
    {
        EXPECT_NEAR(actual.x, expected.x, 1.0e-5f) << what;
        EXPECT_NEAR(actual.y, expected.y, 1.0e-5f) << what;
        EXPECT_NEAR(actual.z, expected.z, 1.0e-5f) << what;
    }
} // namespace

TEST(UsdStageImportTest, AZUpCentimetreStageImportsAsAYUpMetreMesh)
{
    const std::filesystem::path path = OloEngine::Tests::TempFile("olo_usd_stage_quad.usda");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out) << path;
        out << kQuadStage;
    }

    MeshImporterRegistry& registry = MeshImporterRegistry::Get();
    const MeshImporter* importer = registry.Find("usda");
    ASSERT_NE(importer, nullptr);
    ASSERT_EQ(importer->GetName(), std::string_view("OpenUSD")) << "a USD build must route .usda to OpenUSD";

    const MeshImportResult result = registry.Import(path);
    ASSERT_TRUE(result.Succeeded()) << result.Error;
    const MeshSource& source = *result.Source;
    ASSERT_EQ(source.GetSubmeshes().Num(), 1);
    ASSERT_EQ(source.GetVertices().Num(), 4) << "one vertex per face corner";
    ASSERT_EQ(source.GetIndices().Num(), 6) << "the quad fans into two triangles";

    // Centimetres to metres, and the stage's +Z up to the engine's +Y up.
    const auto& v = source.GetVertices();
    ExpectVec3Near(v[0].Position, { 0.0f, 0.0f, 0.0f }, "corner 0");
    ExpectVec3Near(v[1].Position, { 1.0f, 0.0f, 0.0f }, "corner 1: +X stays +X");
    ExpectVec3Near(v[2].Position, { 1.0f, 1.0f, 0.0f }, "corner 2");
    ExpectVec3Near(v[3].Position, { 0.0f, 1.0f, 0.0f }, "corner 3: the stage's up is the engine's up");

    // USD's st origin is bottom-left; the engine's is top-left.
    EXPECT_NEAR(v[0].TexCoord.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(v[2].TexCoord.x, 1.0f, 1.0e-6f);
    EXPECT_NEAR(v[2].TexCoord.y, 0.0f, 1.0e-6f);

    // No authored normals: the face normal, counter-clockwise in the engine's
    // XY plane, faces +Z on every corner.
    for (u32 i = 0; i < 4; ++i)
    {
        ExpectVec3Near(v[i].Normal, { 0.0f, 0.0f, 1.0f }, "generated face normal");
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

#endif // OLO_WITH_USD
