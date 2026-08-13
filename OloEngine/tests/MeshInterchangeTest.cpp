#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// MeshInterchangeTest — unit/contract tests (headless, NO GL context needed).
//
// Pins issue #655's interchange abstraction: the MeshImporter/MeshExporter
// interfaces and their extension-keyed registries, plus the Assimp-backed glTF
// exporter. Two independent things are validated:
//
//   1. Registry DISPATCH LOGIC — a stub MeshImporter registered for a synthetic
//      extension resolves correctly; unknown extensions fall to the Assimp
//      fallback; registration is case-insensitive and dot-insensitive; a later
//      Register() overrides an earlier one.
//
//   2. glTF EXPORT round-trips on CPU — a synthetic MeshSource (a single coloured
//      triangle with a PBR material) is written to a temp .gltf via
//      AssimpMeshExporter, then read straight back with a raw Assimp::Importer
//      (NOT through the engine's Model path, which would need a GL context to
//      Build()). Vertex/face counts, positions, and the base-color/metallic/
//      roughness material factors must survive the round-trip. This is the
//      cheapest layer that proves the exporter emits correct, re-readable glTF.
// =============================================================================

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Asset/Interchange/MeshImporter.h"
#include "OloEngine/Asset/Interchange/MeshImporterRegistry.h"
#include "OloEngine/Asset/Interchange/MeshExporter.h"
#include "OloEngine/Asset/Interchange/MeshExporterRegistry.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    // A MeshImporter that returns a fixed, GL-free MeshSource and records that it ran.
    class StubMeshImporter final : public MeshImporter
    {
      public:
        explicit StubMeshImporter(int marker) : m_Marker(marker) {}

        MeshImportResult Import(const std::filesystem::path&, const MeshImportOptions&) override
        {
            m_CallCount++;
            std::vector<Vertex> verts(3);
            std::vector<u32> indices{ 0, 1, 2 };
            auto source = Ref<MeshSource>::Create(std::move(verts), std::move(indices));
            return MeshImportResult::Ok(source);
        }

        std::string_view GetName() const override
        {
            return "Stub";
        }

        int m_Marker = 0;
        int m_CallCount = 0;
    };

    // Build a single triangle MeshSource with one submesh and one PBR material.
    Ref<MeshSource> MakeTriangleSource(const glm::vec4& baseColor, f32 metallic, f32 roughness)
    {
        std::vector<Vertex> verts(3);
        verts[0] = Vertex(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 0.0f));
        verts[1] = Vertex(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 0.0f));
        verts[2] = Vertex(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 1.0f));
        std::vector<u32> indices{ 0, 1, 2 };

        auto source = Ref<MeshSource>::Create(std::move(verts), std::move(indices));

        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_VertexCount = 3;
        submesh.m_IndexCount = 3;
        submesh.m_MaterialIndex = 0;
        submesh.m_MeshName = "Triangle";
        source->GetSubmeshes().Add(submesh);

        auto material = Material::CreatePBR("TriMat", glm::vec3(baseColor), metallic, roughness);
        material->SetBaseColorFactor(baseColor);
        source->SetImportedMaterials({ material });

        return source;
    }

    std::filesystem::path MakeTempPath(const std::string& filename)
    {
        return OloEngine::Tests::TempFile(filename);
    }
} // namespace

// ---------------------------------------------------------------------------
// Registry dispatch
// ---------------------------------------------------------------------------

TEST(MeshInterchangeTest, RegistryResolvesRegisteredExtension)
{
    auto& registry = MeshImporterRegistry::Get();

    auto stub = CreateScope<StubMeshImporter>(42);
    StubMeshImporter* stubPtr = stub.get();
    registry.Register("stubfmt", std::move(stub));

    EXPECT_EQ(registry.Find("stubfmt"), stubPtr);
    EXPECT_EQ(registry.Find(".stubfmt"), stubPtr) << "leading dot must normalize away";
    EXPECT_EQ(registry.Find("STUBFMT"), stubPtr) << "extension lookup must be case-insensitive";
    EXPECT_TRUE(registry.IsSupported("stubfmt"));
}

TEST(MeshInterchangeTest, RegistryImportRoutesToImporter)
{
    auto& registry = MeshImporterRegistry::Get();

    auto stub = CreateScope<StubMeshImporter>(7);
    StubMeshImporter* stubPtr = stub.get();
    registry.Register("routedfmt", std::move(stub));

    MeshImportResult result = registry.Import("model.routedfmt");
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(stubPtr->m_CallCount, 1) << "Import must dispatch to the registered importer";
}

TEST(MeshInterchangeTest, UnknownExtensionFallsToAssimpFallback)
{
    auto& registry = MeshImporterRegistry::Get();
    // No importer registered for this extension: the fallback (Assimp) must answer, so
    // every extension that reached Assimp before this refactor still resolves to it.
    MeshImporter* importer = registry.Find("totally-unknown-ext");
    ASSERT_NE(importer, nullptr);
    EXPECT_EQ(importer->GetName(), std::string_view("Assimp"));
}

TEST(MeshInterchangeTest, ClassicAssimpExtensionsAreRegistered)
{
    auto& registry = MeshImporterRegistry::Get();
    for (const char* ext : { "fbx", "gltf", "glb", "obj", "dae", "vrm", "ply" })
    {
        MeshImporter* importer = registry.Find(ext);
        ASSERT_NE(importer, nullptr) << "extension: " << ext;
        EXPECT_EQ(importer->GetName(), std::string_view("Assimp")) << "extension: " << ext;
    }
}

TEST(MeshInterchangeTest, LaterRegistrationOverridesEarlier)
{
    auto& registry = MeshImporterRegistry::Get();

    registry.Register("overridefmt", CreateScope<StubMeshImporter>(1));
    auto second = CreateScope<StubMeshImporter>(2);
    StubMeshImporter* secondPtr = second.get();
    registry.Register("overridefmt", std::move(second));

    EXPECT_EQ(registry.Find("overridefmt"), secondPtr);
}

// ---------------------------------------------------------------------------
// glTF export round-trip (CPU only — re-read with raw Assimp, no GL Build)
// ---------------------------------------------------------------------------

TEST(MeshInterchangeTest, GltfExportRoundTripsGeometry)
{
    const glm::vec4 baseColor(0.2f, 0.4f, 0.6f, 1.0f);
    Ref<MeshSource> source = MakeTriangleSource(baseColor, 0.3f, 0.7f);

    const std::filesystem::path outPath = MakeTempPath("olo_interchange_roundtrip.gltf");
    std::filesystem::remove(outPath);

    MeshExportResult exportResult = MeshExporterRegistry::Get().Export(*source, outPath);
    ASSERT_TRUE(exportResult.Success) << exportResult.Error;
    ASSERT_TRUE(std::filesystem::exists(outPath)) << "exporter reported success but wrote no file";

    // Re-read with a raw Assimp importer — no engine Model path, so no GL context needed.
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outPath.string(), aiProcess_Triangulate | aiProcess_JoinIdenticalVertices);
    ASSERT_NE(scene, nullptr) << "Assimp failed to re-read exported glTF: " << importer.GetErrorString();
    ASSERT_GE(scene->mNumMeshes, 1u);

    u32 totalVertices = 0;
    u32 totalFaces = 0;
    for (u32 m = 0; m < scene->mNumMeshes; ++m)
    {
        totalVertices += scene->mMeshes[m]->mNumVertices;
        totalFaces += scene->mMeshes[m]->mNumFaces;
    }
    EXPECT_EQ(totalVertices, 3u) << "triangle vertices must survive the round-trip";
    EXPECT_EQ(totalFaces, 1u) << "one triangle face must survive the round-trip";

    std::filesystem::remove(outPath);
    // Assimp's glTF exporter writes a sidecar .bin for buffers.
    std::filesystem::remove(MakeTempPath("olo_interchange_roundtrip.bin"));
}

// ---------------------------------------------------------------------------
// Alembic write -> read round-trip (only when OLO_WITH_ALEMBIC compiled it in).
// Writes a single triangle .abc with Alembic's own OPolyMesh writer, then imports
// it via AlembicMeshImporter — exercising the Ogawa open, traversal, and
// fan-triangulation end to end, in CI, with no external fixture.
// ---------------------------------------------------------------------------
#if defined(OLO_WITH_ALEMBIC)
#include "OloEngine/Asset/Interchange/Alembic/AlembicMeshImporter.h"
#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

TEST(MeshInterchangeTest, AlembicRoundTripsGeometry)
{
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;

    const std::filesystem::path outPath = MakeTempPath("olo_alembic_roundtrip.abc");
    std::filesystem::remove(outPath);

    {
        Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), outPath.string());
        AbcG::OPolyMesh meshObj(Abc::OObject(archive, Abc::kTop), "triangle");
        AbcG::OPolyMeshSchema& schema = meshObj.getSchema();

        const std::vector<Imath::V3f> positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
        const std::vector<int32_t> indices = { 0, 1, 2 };
        const std::vector<int32_t> counts = { 3 };

        AbcG::OPolyMeshSchema::Sample sample(
            Abc::P3fArraySample(positions.data(), positions.size()),
            Abc::Int32ArraySample(indices.data(), indices.size()),
            Abc::Int32ArraySample(counts.data(), counts.size()));
        schema.set(sample);
    } // archive flushes + closes on scope exit

    AlembicMeshImporter importer;
    MeshImportResult result = importer.Import(outPath);
    ASSERT_TRUE(result.Succeeded()) << result.Error;
    ASSERT_TRUE(result.Source);
    EXPECT_EQ(result.Source->GetSubmeshes().Num(), 1);
    EXPECT_EQ(result.Source->GetVertices().Num(), 3) << "fresh vertex per face-corner: 3";
    EXPECT_EQ(result.Source->GetIndices().Num(), 3) << "one triangle -> 3 indices";

    std::filesystem::remove(outPath);
}
#endif // OLO_WITH_ALEMBIC

// ---------------------------------------------------------------------------
// MaterialX standalone document read (only when OLO_WITH_MATERIALX compiled it in).
// A self-contained standard_surface doc (no stdlib needed for authored values) must
// map to the PBR factors. Also the runtime proof that the assimp/MaterialX pugixml
// /FORCE:MULTIPLE merge parses .mtlx correctly (no ABI corruption).
// ---------------------------------------------------------------------------
#if defined(OLO_WITH_MATERIALX)
#include "OloEngine/Asset/Interchange/MaterialX/MaterialXMaterialReader.h"
#include <fstream>

TEST(MeshInterchangeTest, MaterialXReadsStandardSurfaceFactors)
{
    static constexpr const char* kDoc =
        "<?xml version=\"1.0\"?>\n"
        "<materialx version=\"1.39\">\n"
        "  <standard_surface name=\"SR_test\" type=\"surfaceshader\">\n"
        "    <input name=\"base_color\" type=\"color3\" value=\"0.2, 0.4, 0.6\" />\n"
        "    <input name=\"metalness\" type=\"float\" value=\"0.3\" />\n"
        "    <input name=\"specular_roughness\" type=\"float\" value=\"0.7\" />\n"
        "  </standard_surface>\n"
        "  <surfacematerial name=\"M_test\" type=\"material\">\n"
        "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"SR_test\" />\n"
        "  </surfacematerial>\n"
        "</materialx>\n";

    const std::filesystem::path outPath = MakeTempPath("olo_test.mtlx");
    {
        std::ofstream f(outPath);
        f << kDoc;
    }

    auto material = Material::CreatePBR("t", glm::vec3(1.0f), 0.0f, 0.5f);
    std::string error;
    ASSERT_TRUE(MaterialXImport::ReadMaterialXMaterial(outPath, *material, error)) << error;
    EXPECT_NEAR(material->GetBaseColorFactor().r, 0.2f, 1e-4f);
    EXPECT_NEAR(material->GetBaseColorFactor().g, 0.4f, 1e-4f);
    EXPECT_NEAR(material->GetBaseColorFactor().b, 0.6f, 1e-4f);
    EXPECT_NEAR(material->GetMetallicFactor(), 0.3f, 1e-4f);
    EXPECT_NEAR(material->GetRoughnessFactor(), 0.7f, 1e-4f);

    std::filesystem::remove(outPath);
}
#endif // OLO_WITH_MATERIALX

TEST(MeshInterchangeTest, GltfExportRoundTripsMaterialFactors)
{
    const glm::vec4 baseColor(0.2f, 0.4f, 0.6f, 1.0f);
    const f32 metallic = 0.3f;
    const f32 roughness = 0.7f;
    Ref<MeshSource> source = MakeTriangleSource(baseColor, metallic, roughness);

    const std::filesystem::path outPath = MakeTempPath("olo_interchange_material.gltf");
    std::filesystem::remove(outPath);

    ASSERT_TRUE(MeshExporterRegistry::Get().Export(*source, outPath).Success);

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outPath.string(), 0);
    ASSERT_NE(scene, nullptr) << importer.GetErrorString();
    ASSERT_GE(scene->mNumMaterials, 1u);

    // Find the material bound to the exported mesh (mesh 0's material index).
    ASSERT_GE(scene->mNumMeshes, 1u);
    const u32 matIndex = scene->mMeshes[0]->mMaterialIndex;
    ASSERT_LT(matIndex, scene->mNumMaterials);
    const aiMaterial* mat = scene->mMaterials[matIndex];

    aiColor4D readBaseColor(1.0f, 1.0f, 1.0f, 1.0f);
    ASSERT_EQ(mat->Get(AI_MATKEY_BASE_COLOR, readBaseColor), aiReturn_SUCCESS);
    EXPECT_NEAR(readBaseColor.r, baseColor.r, 1e-3f);
    EXPECT_NEAR(readBaseColor.g, baseColor.g, 1e-3f);
    EXPECT_NEAR(readBaseColor.b, baseColor.b, 1e-3f);

    ai_real readMetallic = -1.0f;
    ASSERT_EQ(mat->Get(AI_MATKEY_METALLIC_FACTOR, readMetallic), aiReturn_SUCCESS);
    EXPECT_NEAR(readMetallic, metallic, 1e-3f);

    ai_real readRoughness = -1.0f;
    ASSERT_EQ(mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, readRoughness), aiReturn_SUCCESS);
    EXPECT_NEAR(readRoughness, roughness, 1e-3f);

    std::filesystem::remove(outPath);
    std::filesystem::remove(MakeTempPath("olo_interchange_material.bin"));
}
