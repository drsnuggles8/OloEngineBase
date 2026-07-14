// OLO_TEST_LAYER: L3
//
// The materials a mesh was IMPORTED with must survive every container that persists a
// MeshSource (issue #629).
//
// Before this, `MeshSource::m_ImportedMaterials` — the vector every submesh's
// m_MaterialIndex addresses — was written by NOBODY: not by the asset pack
// (MeshSourceSerializer::SerializeToAssetPack wrote only the `TMap<u32, AssetHandle>`
// map, which the import path never populates) and not by the .omesh dev cache. The
// EDITOR masked it by re-importing the source file through Assimp on every warm-cache
// load purely to rebuild materials; a PACKED runtime has no source file and no Assimp,
// so every mesh in a shipped game rendered with the flat engine-default material —
// ship the game, lose every texture.
//
// These tests drive the production serializers end to end:
//   * asset pack  -> every submesh resolves to the SAME material (texture handles,
//     alpha mode + cutoff, factors, flags) it had before the round trip;
//   * .omesh      -> the same, through the cache the editor actually reads;
//   * a pre-v5 pack (reader pinned to v4) -> the appended material field is NOT read,
//     the desync guard docs/agent-rules/binary-format-versioning.md requires;
//   * the codec's own wire format -> every field round-trips (no GPU needed).
//
// The pack/.omesh cases need a GL context: they build real Texture2D assets (so the
// texture *handles* in the table are meaningful) and DeserializeFromAssetPack calls
// MeshSource::Build(). They SKIP cleanly on headless CI.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Serialization/ImportedMaterialCodec.h"
#include "OloEngine/Serialization/MeshBinarySerializer.h"

#include <stb_image/stb_image_write.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

using namespace OloEngine;

namespace
{
    // A two-submesh quad pair: submesh 0 uses material 0, submesh 1 uses material 1,
    // submesh 2 re-uses material 0 — the deduplicated indexing MeshSource actually gets
    // from Model::CreateCombinedMeshSource.
    Ref<MeshSource> MakeTwoMaterialMesh()
    {
        TArray<Vertex> vertices;
        TArray<u32> indices;
        for (u32 s = 0; s < 3; ++s)
        {
            auto const baseVertex = static_cast<u32>(vertices.Num());
            auto const x = static_cast<f32>(s);
            vertices.Add(Vertex({ x + 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }));
            vertices.Add(Vertex({ x + 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }));
            vertices.Add(Vertex({ x + 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }));
            vertices.Add(Vertex({ x + 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }));

            for (u32 i : { 0u, 1u, 2u, 2u, 3u, 0u })
            {
                indices.Add(baseVertex + i);
            }
        }

        auto meshSource = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));

        TArray<Submesh> submeshes;
        for (u32 s = 0; s < 3; ++s)
        {
            Submesh submesh;
            submesh.m_BaseVertex = s * 4;
            submesh.m_BaseIndex = s * 6;
            submesh.m_VertexCount = 4;
            submesh.m_IndexCount = 6;
            submesh.m_MaterialIndex = (s == 1) ? 1u : 0u;
            submesh.m_NodeName = "Submesh_" + std::to_string(s);
            submesh.m_BoundingBox = BoundingBox(glm::vec3(static_cast<f32>(s), 0.0f, 0.0f),
                                                glm::vec3(static_cast<f32>(s) + 1.0f, 1.0f, 0.0f));
            submeshes.Add(submesh);
        }
        meshSource->SetSubmeshes(submeshes);
        return meshSource;
    }

    // A 1x1 texture registered as a memory-only asset, so it has a real AssetHandle —
    // which is exactly how a packed build addresses textures.
    Ref<Texture2D> MakeRegisteredTexture(u32 rgba, bool srgb)
    {
        TextureSpecification spec;
        spec.Width = 1;
        spec.Height = 1;
        spec.Format = ImageFormat::RGBA8;
        spec.SRGB = srgb;
        Ref<Texture2D> texture = Texture2D::Create(spec);
        if (!texture)
        {
            return nullptr;
        }
        texture->SetData(&rgba, sizeof(rgba));
        AssetManager::AddMemoryOnlyAsset(texture);
        return texture;
    }
} // namespace

class ImportedMaterialPackTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string folder = std::string(info->test_suite_name()) + "." + info->name();
        m_TempDir = fs::temp_directory_path() / "OloEngineImportedMaterials" / folder;

        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
        fs::create_directories(m_TempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = m_TempDir / "Test.oloproj";
        {
            std::ofstream proj(projectFile);
            proj << "Project:\n"
                    "  Name: ImportedMaterialPackTest\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }
        ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << m_TempDir.string();

        m_AssetManager = Ref<EditorAssetManager>::Create();
        m_AssetManager->Initialize(/*startFileWatcher=*/false);
        Project::SetAssetManager(m_AssetManager);
    }

    void TearDown() override
    {
        m_AssetManager.Reset();
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    Ref<MeshSource> PackRoundTrip(AssetHandle handle, u32 readerVersion = AssetPackFile::Version)
    {
        const fs::path packPath = m_TempDir / "mesh.pack";
        MeshSourceSerializer serializer;
        AssetSerializationInfo info{};
        {
            FileStreamWriter writer(packPath);
            EXPECT_TRUE(writer.IsStreamGood());
            EXPECT_TRUE(serializer.SerializeToAssetPack(handle, writer, info));
        }

        AssetPackFile::AssetInfo assetInfo{};
        assetInfo.Handle = static_cast<AssetHandle>(0xBEEFULL);
        assetInfo.PackedOffset = info.Offset;
        assetInfo.PackedSize = info.Size;
        assetInfo.Type = AssetType::MeshSource;

        FileStreamReader reader(packPath);
        EXPECT_TRUE(reader.IsStreamGood());
        reader.SetArchiveVersion(readerVersion);
        return serializer.DeserializeFromAssetPack(reader, assetInfo).As<MeshSource>();
    }

    // Two distinct, fully-specified materials, each with real (handled) textures.
    void BuildMaterials()
    {
        m_Albedo0 = MakeRegisteredTexture(0xFF0000FFu, /*srgb=*/true);
        m_Normal0 = MakeRegisteredTexture(0xFF8080FFu, /*srgb=*/false);
        m_MR0 = MakeRegisteredTexture(0xFF00FF00u, /*srgb=*/false);
        m_Albedo1 = MakeRegisteredTexture(0xFF00FF00u, /*srgb=*/true);
        ASSERT_TRUE(m_Albedo0 && m_Normal0 && m_MR0 && m_Albedo1);

        Ref<Material> stone = Material::CreatePBR("Stone", glm::vec3(0.6f, 0.5f, 0.4f), 0.25f, 0.7f);
        stone->SetAlbedoMap(m_Albedo0);
        stone->SetNormalMap(m_Normal0);
        stone->SetMetallicRoughnessMap(m_MR0);
        stone->SetAlphaMode(AlphaMode::Opaque);
        stone->SetNormalScale(0.5f);
        stone->SetOcclusionStrength(0.25f);
        stone->SetEmissiveFactor(glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));
        stone->SetFlag(MaterialFlag::TwoSided, true);

        // The alpha-masked one — the material MaterialAsset's YAML could not express
        // (it carries neither AlphaMode nor AlphaCutoff), and the one that turns Sponza's
        // foliage into solid quads when it is lost.
        Ref<Material> foliage = Material::CreatePBR("Foliage", glm::vec3(0.2f, 0.9f, 0.3f), 0.0f, 0.9f);
        foliage->SetAlbedoMap(m_Albedo1);
        foliage->SetAlphaMode(AlphaMode::Mask);
        foliage->SetAlphaCutoff(0.37f);

        m_Materials = { stone, foliage };
    }

    void ExpectMaterialsMatch(const Ref<MeshSource>& unpacked)
    {
        ASSERT_TRUE(unpacked);
        ASSERT_EQ(unpacked->GetImportedMaterials().size(), m_Materials.size())
            << "the imported-material table did not survive — every submesh would render "
               "with the engine-default material in a packed build";

        // Resolve through the SAME accessor the renderer uses, per submesh, so the test
        // pins the mapping (submesh -> material index -> material), not just the table.
        for (u32 submesh = 0; submesh < 3; ++submesh)
        {
            u32 const expectedIndex = (submesh == 1) ? 1u : 0u;
            Ref<Material> const expected = m_Materials[expectedIndex];
            Ref<Material> const actual = unpacked->GetImportedMaterialForSubmesh(submesh);
            ASSERT_TRUE(actual) << "submesh " << submesh << " resolved to NO material";

            EXPECT_EQ(actual->GetName(), expected->GetName());
            EXPECT_EQ(actual->GetAlphaMode(), expected->GetAlphaMode());
            EXPECT_FLOAT_EQ(actual->GetAlphaCutoff(), expected->GetAlphaCutoff());
            EXPECT_EQ(actual->GetFlags(), expected->GetFlags());
            EXPECT_FLOAT_EQ(actual->GetMetallicFactor(), expected->GetMetallicFactor());
            EXPECT_FLOAT_EQ(actual->GetRoughnessFactor(), expected->GetRoughnessFactor());
            EXPECT_FLOAT_EQ(actual->GetNormalScale(), expected->GetNormalScale());
            EXPECT_FLOAT_EQ(actual->GetOcclusionStrength(), expected->GetOcclusionStrength());
            for (int c = 0; c < 4; ++c)
            {
                EXPECT_FLOAT_EQ(actual->GetBaseColorFactor()[c], expected->GetBaseColorFactor()[c]);
                EXPECT_FLOAT_EQ(actual->GetEmissiveFactor()[c], expected->GetEmissiveFactor()[c]);
            }

            // Textures resolve back to the SAME asset (by handle), not to a copy and not
            // to a placeholder.
            auto const expectHandle = [](const Ref<Texture2D>& actualTex, const Ref<Texture2D>& expectedTex, const char* slot)
            {
                if (!expectedTex)
                {
                    EXPECT_FALSE(actualTex) << slot << " came back set although the source material had none";
                    return;
                }
                ASSERT_TRUE(actualTex) << slot << " was dropped by the round trip";
                EXPECT_EQ(static_cast<u64>(actualTex->GetHandle()), static_cast<u64>(expectedTex->GetHandle()))
                    << slot << " resolved to a different asset";
            };
            expectHandle(actual->GetAlbedoMap(), expected->GetAlbedoMap(), "AlbedoMap");
            expectHandle(actual->GetNormalMap(), expected->GetNormalMap(), "NormalMap");
            expectHandle(actual->GetMetallicRoughnessMap(), expected->GetMetallicRoughnessMap(), "MetallicRoughnessMap");
            expectHandle(actual->GetAOMap(), expected->GetAOMap(), "AOMap");
            expectHandle(actual->GetEmissiveMap(), expected->GetEmissiveMap(), "EmissiveMap");
        }
    }

    fs::path m_TempDir;
    Ref<EditorAssetManager> m_AssetManager;
    Ref<Texture2D> m_Albedo0, m_Normal0, m_MR0, m_Albedo1;
    std::vector<Ref<Material>> m_Materials;
};

TEST_F(ImportedMaterialPackTest, EverySubmeshResolvesItsMaterialAfterThePackRoundTrip)
{
    OLO_ENSURE_GPU_OR_SKIP();

    BuildMaterials();
    Ref<MeshSource> source = MakeTwoMaterialMesh();
    source->SetImportedMaterials(m_Materials);
    ASSERT_TRUE(source->GetImportedMaterialForSubmesh(1));

    AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(source);
    Ref<MeshSource> unpacked = PackRoundTrip(handle);
    ASSERT_TRUE(unpacked) << "DeserializeFromAssetPack returned null";

    ExpectMaterialsMatch(unpacked);
}

TEST_F(ImportedMaterialPackTest, EverySubmeshResolvesItsMaterialAfterTheOMeshRoundTrip)
{
    OLO_ENSURE_GPU_OR_SKIP();

    BuildMaterials();
    Ref<MeshSource> source = MakeTwoMaterialMesh();
    source->SetImportedMaterials(m_Materials);

    const fs::path cachePath = m_TempDir / "mesh.omesh";
    ASSERT_TRUE(MeshBinarySerializer::Write(cachePath, *source, /*sourceTimestamp=*/4242));

    Ref<MeshSource> loaded = MeshBinarySerializer::Read(cachePath);
    ASSERT_TRUE(loaded) << "MeshBinarySerializer::Read returned null";

    ExpectMaterialsMatch(loaded);
}

TEST_F(ImportedMaterialPackTest, PreV5PackReaderDoesNotConsumeTheMaterialField)
{
    OLO_ENSURE_GPU_OR_SKIP();

    // The desync guard. A v1-v4 pack never wrote the trailing material table, so a reader
    // that sees Header.Version < 5 must not try to read it. Pinning the reader to v4
    // simulates loading an older pack: the geometry must come back intact and the material
    // table must simply be absent — NOT a misread of whatever bytes follow.
    BuildMaterials();
    Ref<MeshSource> source = MakeTwoMaterialMesh();
    source->SetImportedMaterials(m_Materials);

    AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(source);
    Ref<MeshSource> unpacked = PackRoundTrip(handle, AssetPackFile::ImportedMaterialsPackVersion - 1);

    ASSERT_TRUE(unpacked) << "an older pack must still load";
    EXPECT_TRUE(unpacked->GetImportedMaterials().empty())
        << "a pre-v5 reader must not consume the trailing material field";
    EXPECT_EQ(unpacked->GetVertices().Num(), source->GetVertices().Num());
    EXPECT_EQ(unpacked->GetIndices().Num(), source->GetIndices().Num());
    EXPECT_EQ(unpacked->GetSubmeshes().Num(), source->GetSubmeshes().Num());
}

TEST_F(ImportedMaterialPackTest, PreV4OMeshFileStillLoadsWithoutTheMaterialSection)
{
    OLO_ENSURE_GPU_OR_SKIP();

    // Same contract for the .omesh container: the section directory is sized by the FILE's
    // version, so a v3 file (8 entries, no ImportedMaterials section) must still read.
    BuildMaterials();
    Ref<MeshSource> source = MakeTwoMaterialMesh();
    source->SetImportedMaterials(m_Materials);

    const fs::path cachePath = m_TempDir / "v3_compat.omesh";
    ASSERT_TRUE(MeshBinarySerializer::Write(cachePath, *source, /*sourceTimestamp=*/7));

    // Patch FileHeader::Version (u32 at byte offset 4) from 4 down to 3. Section offsets
    // are absolute, so the v3 reader simply reads an 8-entry directory and never sees the
    // (still-present) ImportedMaterials entry.
    {
        std::fstream file(cachePath, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(4);
        u32 const v3 = 3;
        file.write(reinterpret_cast<const char*>(&v3), sizeof(v3));
    }

    Ref<MeshSource> loaded = MeshBinarySerializer::Read(cachePath);
    ASSERT_TRUE(loaded) << "a v3 .omesh must still load in the v4 reader";
    EXPECT_TRUE(loaded->GetImportedMaterials().empty());
    EXPECT_EQ(loaded->GetVertices().Num(), source->GetVertices().Num());
    EXPECT_EQ(loaded->GetSubmeshes().Num(), source->GetSubmeshes().Num());
}

// ─────────────────────────────────────────────────────────────────────────────
// The editor's warm-cache path, end to end, on a REAL textured glTF.
//
// Model::LoadModel used to re-import the source file through Assimp on every warm load
// purely to rebuild materials. That re-import is now conditional on the cache not having
// them — which is only safe if materials genuinely survive the .omesh. This test is the
// proof: import cold (writes the cache), then import again warm (reads it), and require
// the same materials, textures included. If the round trip were lossy, the second Model
// would come back with no/blank materials and the editor would quietly render grey.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ImportedMaterialWarmCacheTest, WarmCacheLoadKeepsTheMaterialsWithoutReimporting)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const fs::path modelPath = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets/models/DeccerCubes/SM_Deccer_Cubes_Textured.gltf";
    ASSERT_TRUE(fs::exists(modelPath)) << "fixture missing: " << modelPath.string();

    // Force a cold import so the cache is written by THIS build's serializer.
    MeshCache::InvalidateCache(modelPath);
    ASSERT_FALSE(MeshCache::IsMeshCacheValid(modelPath));

    Model cold{ modelPath.string() };
    ASSERT_GT(cold.GetMeshes().size(), 0u);
    const auto& coldMaterials = cold.GetMaterials();
    ASSERT_FALSE(coldMaterials.empty()) << "the cold import produced no materials at all";
    ASSERT_TRUE(MeshCache::IsMeshCacheValid(modelPath)) << "the cold import did not write a cache";

    // Now the warm path: geometry AND materials come from the .omesh.
    Model warm{ modelPath.string() };
    const auto& warmMaterials = warm.GetMaterials();

    ASSERT_EQ(warmMaterials.size(), coldMaterials.size())
        << "the warm load lost materials — every submesh would fall back to the default";

    bool sawAnyTexture = false;
    for (sizet i = 0; i < coldMaterials.size(); ++i)
    {
        ASSERT_TRUE(coldMaterials[i] && warmMaterials[i]) << "material " << i << " is null";
        EXPECT_EQ(warmMaterials[i]->GetName(), coldMaterials[i]->GetName());
        EXPECT_EQ(warmMaterials[i]->GetAlphaMode(), coldMaterials[i]->GetAlphaMode());
        EXPECT_FLOAT_EQ(warmMaterials[i]->GetMetallicFactor(), coldMaterials[i]->GetMetallicFactor());
        EXPECT_FLOAT_EQ(warmMaterials[i]->GetRoughnessFactor(), coldMaterials[i]->GetRoughnessFactor());
        for (int c = 0; c < 4; ++c)
        {
            EXPECT_FLOAT_EQ(warmMaterials[i]->GetBaseColorFactor()[c], coldMaterials[i]->GetBaseColorFactor()[c]);
        }

        // The textures are the whole point: this variant ships albedo maps, and they must
        // come back on the warm path too (resolved from the source path the table carries,
        // since a loose model file's textures are not registered assets).
        if (coldMaterials[i]->GetAlbedoMap())
        {
            sawAnyTexture = true;
            EXPECT_TRUE(warmMaterials[i]->GetAlbedoMap())
                << "material '" << coldMaterials[i]->GetName() << "' lost its albedo texture on the warm load";
        }
    }
    EXPECT_TRUE(sawAnyTexture) << "fixture no longer ships albedo textures — this test proves nothing";
}

// ─────────────────────────────────────────────────────────────────────────────
// TWO IMPORTERS, ONE CACHE — the blind spot that let a visual regression through
// a 4623-green suite (issue #629).
//
// The same model is reachable two ways, and they SHARE one .omesh keyed by source path:
//   * MeshSourceSerializer::TryLoadData  — the asset/handle importer (VirtualMeshComponent,
//     MeshComponent). Opens the model by an ABSOLUTE path.
//   * Model{path}                        — the classic ModelComponent path. Opens it by
//     whatever path the caller had, typically RELATIVE.
//
// Whichever runs first WRITES the cache the other one then READS. Before the fix, the
// texture references in that cache depended on which importer wrote it: the absolute-path
// importer resolved each texture to a registry AssetHandle, and a warm load then re-fetched
// it through the AssetManager — whose TextureSerializer decides colour space from the FILE
// NAME (IsLikelyColorTextureByName). A hash-named albedo (Sponza's "466164707995436622.jpg")
// is not recognised as colour, so it came back LINEAR instead of sRGB — and any file the
// asset path failed to load came back NULL, which makes an alpha-masked material discard the
// whole submesh (it disappears) and an opaque one shade black.
//
// So: import cold through the MeshSource importer, warm-load through Model, and require the
// materials to be IDENTICAL to a fresh Model import. The texture name here is deliberately
// hash-like — that is precisely what defeats the filename heuristic.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ImportedMaterialPackTest, WarmLoadIsIdenticalNoMatterWhichImporterWroteTheCache)
{
    OLO_ENSURE_GPU_OR_SKIP();

    // ── Author a tiny textured OBJ inside the project, with a HASH-NAMED albedo ──
    const fs::path assets = m_TempDir / "Assets";
    const std::string textureName = "466164707995436622.png"; // no "albedo"/"diffuse" in the name
    {
        // A 2x2 red/white texture. Content is irrelevant; the NAME is the point.
        const u32 pixels[4] = { 0xFF0000FFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF0000FFu };
        ASSERT_NE(::stbi_write_png((assets / textureName).string().c_str(), 2, 2, 4, pixels, 2 * 4), 0)
            << "failed to author the fixture texture";
    }
    {
        std::ofstream mtl(assets / "cube.mtl");
        mtl << "newmtl Painted\n"
               "Kd 1.0 1.0 1.0\n"
               "map_Kd "
            << textureName << "\n";
    }
    {
        std::ofstream obj(assets / "cube.obj");
        obj << "mtllib cube.mtl\n"
               "usemtl Painted\n"
               "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
               "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
               "vn 0 0 1\n"
               "f 1/1/1 2/2/1 3/3/1\n"
               "f 1/1/1 3/3/1 4/4/1\n";
    }

    const fs::path modelPath = assets / "cube.obj";
    ASSERT_TRUE(fs::exists(modelPath));

    // Register BOTH in the asset registry — this is what gives the texture a handle, and it
    // is exactly the state a real project is in (Sponza's textures are all registered).
    const AssetHandle textureHandle = m_AssetManager->ImportAsset(assets / textureName);
    const AssetHandle meshHandle = m_AssetManager->ImportAsset(modelPath);
    ASSERT_NE(static_cast<u64>(textureHandle), 0ULL);
    ASSERT_NE(static_cast<u64>(meshHandle), 0ULL);

    // ── Control: what a FRESH import produces (this is ground truth) ──
    MeshCache::InvalidateCache(modelPath);
    MeshCache::InvalidateCache(modelPath, "static_uvflip_v1");
    Ref<Material> expected;
    {
        Model cold{ modelPath.string() };
        ASSERT_FALSE(cold.GetMaterials().empty());
        expected = cold.GetMaterials()[0];
        ASSERT_TRUE(expected->GetAlbedoMap()) << "fixture is wrong: the fresh import has no albedo map";
        ASSERT_TRUE(expected->GetAlbedoMap()->GetSpecification().SRGB)
            << "fixture is wrong: an OBJ map_Kd is a COLOUR texture, so the importer must load it as sRGB";
    }

    // ── Now let the MESHSOURCE importer (absolute path -> handles) write the cache ──
    MeshCache::InvalidateCache(modelPath);
    MeshCache::InvalidateCache(modelPath, "static_uvflip_v1");
    {
        AssetMetadata metadata;
        metadata.Handle = meshHandle;
        metadata.FilePath = m_AssetManager->GetRelativePath(modelPath);
        metadata.Type = AssetType::MeshSource;

        MeshSourceSerializer serializer;
        Ref<Asset> asset;
        ASSERT_TRUE(serializer.TryLoadData(metadata, asset)) << "the MeshSource importer failed to cold-import";
        ASSERT_TRUE(asset.As<MeshSource>());
    }
    ASSERT_TRUE(MeshCache::IsMeshCacheValid(modelPath, "static_uvflip_v1"))
        << "the MeshSource importer did not write the cache the classic path will read";

    // ── The classic path now WARM-loads that cache. It must be identical to the control. ──
    Model warm{ modelPath.string() };
    ASSERT_EQ(warm.GetMaterials().size(), 1u);
    Ref<Material> actual = warm.GetMaterials()[0];
    ASSERT_TRUE(actual);

    Ref<Texture2D> albedo = actual->GetAlbedoMap();
    ASSERT_TRUE(albedo)
        << "the warm load lost the albedo texture. An alpha-masked material would now discard "
           "the whole submesh and it would VANISH from the frame.";
    EXPECT_TRUE(albedo->GetSpecification().SRGB)
        << "the warm load resolved the albedo through the AssetManager, whose colour-space guess "
           "is filename-based and cannot recognise a hash-named texture — so it came back LINEAR. "
           "It must be resolved with the colour space the IMPORTER recorded for that material slot.";
    EXPECT_EQ(albedo->GetWidth(), expected->GetAlbedoMap()->GetWidth());
    EXPECT_EQ(actual->GetAlphaMode(), expected->GetAlphaMode());
    for (int c = 0; c < 4; ++c)
    {
        EXPECT_FLOAT_EQ(actual->GetBaseColorFactor()[c], expected->GetBaseColorFactor()[c]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The codec's own wire format. No GPU, no asset manager: descriptors in,
// descriptors out — the layer both containers embed verbatim.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ImportedMaterialCodecTest, EveryFieldSurvivesTheWireFormat)
{
    using namespace ImportedMaterialCodec;

    MaterialDesc masked;
    masked.Name = "Foliage";
    masked.Type = static_cast<i32>(MaterialType::PBR);
    masked.Flags = std::to_underlying(MaterialFlag::DepthTest) | std::to_underlying(MaterialFlag::TwoSided);
    masked.AlphaMode = static_cast<i32>(AlphaMode::Mask);
    masked.AlphaCutoff = 0.37f;
    masked.BaseColorFactor = glm::vec4(0.2f, 0.9f, 0.3f, 1.0f);
    masked.EmissiveFactor = glm::vec4(0.05f, 0.0f, 0.1f, 1.0f);
    masked.MetallicFactor = 0.125f;
    masked.RoughnessFactor = 0.875f;
    masked.NormalScale = 0.5f;
    masked.OcclusionStrength = 0.25f;
    masked.EnableIBL = true;
    masked.Albedo = TextureRef{ AssetHandle(0x1234ULL), "Assets/leaf_albedo.png", true };
    masked.Normal = TextureRef{ AssetHandle(0x5678ULL), "Assets/leaf_normal.png", false };
    masked.MetallicRoughness = TextureRef{ AssetHandle(0), "Assets/leaf_mr.png", false };

    MaterialDesc absent;
    absent.Present = false; // a null entry in the source vector

    std::vector<u8> const blob = Encode({ masked, absent });
    ASSERT_FALSE(blob.empty());

    std::vector<MaterialDesc> decoded;
    ASSERT_TRUE(Decode(blob, decoded));
    ASSERT_EQ(decoded.size(), 2u);

    const auto& d = decoded[0];
    EXPECT_TRUE(d.Present);
    EXPECT_EQ(d.Name, masked.Name);
    EXPECT_EQ(d.Type, masked.Type);
    EXPECT_EQ(d.Flags, masked.Flags);
    EXPECT_EQ(d.AlphaMode, masked.AlphaMode);
    EXPECT_FLOAT_EQ(d.AlphaCutoff, masked.AlphaCutoff);
    EXPECT_FLOAT_EQ(d.MetallicFactor, masked.MetallicFactor);
    EXPECT_FLOAT_EQ(d.RoughnessFactor, masked.RoughnessFactor);
    EXPECT_FLOAT_EQ(d.NormalScale, masked.NormalScale);
    EXPECT_FLOAT_EQ(d.OcclusionStrength, masked.OcclusionStrength);
    EXPECT_TRUE(d.EnableIBL);
    for (int c = 0; c < 4; ++c)
    {
        EXPECT_FLOAT_EQ(d.BaseColorFactor[c], masked.BaseColorFactor[c]);
        EXPECT_FLOAT_EQ(d.EmissiveFactor[c], masked.EmissiveFactor[c]);
    }
    EXPECT_EQ(static_cast<u64>(d.Albedo.Handle), 0x1234ULL);
    EXPECT_EQ(d.Albedo.Path, "Assets/leaf_albedo.png");
    EXPECT_TRUE(d.Albedo.SRGB);
    EXPECT_EQ(static_cast<u64>(d.Normal.Handle), 0x5678ULL);
    EXPECT_FALSE(d.Normal.SRGB);
    EXPECT_EQ(static_cast<u64>(d.MetallicRoughness.Handle), 0ULL);
    EXPECT_EQ(d.MetallicRoughness.Path, "Assets/leaf_mr.png");
    EXPECT_TRUE(d.AO.IsEmpty());
    EXPECT_TRUE(d.Emissive.IsEmpty());

    // The null slot stays a null slot — Submesh::m_MaterialIndex addresses this table
    // positionally, so compacting it away would silently shift every later material.
    EXPECT_FALSE(decoded[1].Present);
}

TEST(ImportedMaterialCodecTest, GarbageBlobIsRejectedInsteadOfMisparsed)
{
    using namespace ImportedMaterialCodec;

    std::vector<MaterialDesc> decoded;

    // Empty is "no materials", not an error.
    EXPECT_TRUE(Decode({}, decoded));
    EXPECT_TRUE(decoded.empty());

    // Bad magic.
    std::vector<u8> const garbage(64, 0xAB);
    EXPECT_FALSE(Decode(garbage, decoded));

    // Truncated: a valid header that promises a material the bytes don't contain.
    MaterialDesc desc;
    desc.Name = "Truncated";
    std::vector<u8> blob = Encode({ desc });
    ASSERT_GT(blob.size(), 16u);
    blob.resize(16);
    EXPECT_FALSE(Decode(blob, decoded));
}
