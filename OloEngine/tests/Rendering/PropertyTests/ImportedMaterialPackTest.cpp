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

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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

// ─────────────────────────────────────────────────────────────────────────────
// THE OTHER ORDER: Model writes the cache, the MeshSource importer reads it.
//
// The bug was symmetrical — "one cache, two texture identities, decided by scene-open
// order" — but only ONE of the two orders was pinned (MeshSource writes -> Model reads,
// below). This is the other half: the classic Model importer cold-imports and writes the
// .omesh, and the MESHSOURCE importer (the path a VirtualMeshComponent / the asset system
// takes) then warm-loads it. Its materials must be identical to a fresh import too — a
// warm load that resolved the albedo through the AssetManager's filename-based colour-space
// guess would get a LINEAR albedo for a hash-named texture, and an alpha-masked material
// whose albedo failed to load discards the whole submesh.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ImportedMaterialPackTest, MeshSourceImporterWarmLoadOfAModelWrittenCacheKeepsTheTextureIdentity)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const fs::path assets = m_TempDir / "Assets";
    const std::string textureName = "466164707995436622.png"; // hash-named: defeats the filename heuristic
    {
        const u32 pixels[4] = { 0xFF0000FFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF0000FFu };
        ASSERT_NE(::stbi_write_png((assets / textureName).string().c_str(), 2, 2, 4, pixels, 2 * 4), 0);
    }
    {
        std::ofstream mtl(assets / "cube.mtl");
        mtl << "newmtl Painted\nKd 1.0 1.0 1.0\nmap_Kd " << textureName << "\n";
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
    const AssetHandle textureHandle = m_AssetManager->ImportAsset(assets / textureName);
    const AssetHandle meshHandle = m_AssetManager->ImportAsset(modelPath);
    ASSERT_NE(static_cast<u64>(textureHandle), 0ULL);
    ASSERT_NE(static_cast<u64>(meshHandle), 0ULL);

    // ── Control: a fresh MeshSource import with no cache in play (ground truth) ──
    const auto meshSourceImport = [&]() -> Ref<MeshSource>
    {
        AssetMetadata metadata;
        metadata.Handle = meshHandle;
        metadata.FilePath = m_AssetManager->GetRelativePath(modelPath);
        metadata.Type = AssetType::MeshSource;

        MeshSourceSerializer serializer;
        Ref<Asset> asset;
        EXPECT_TRUE(serializer.TryLoadData(metadata, asset));
        return asset.As<MeshSource>();
    };

    MeshCache::InvalidateCache(modelPath);
    MeshCache::InvalidateCache(modelPath, "static_uvflip_v1");
    Ref<MeshSource> const cold = meshSourceImport();
    ASSERT_TRUE(cold);
    ASSERT_FALSE(cold->GetImportedMaterials().empty()) << "the fresh MeshSource import carries no materials";
    Ref<Material> const expected = cold->GetImportedMaterials()[0];
    ASSERT_TRUE(expected);
    ASSERT_TRUE(expected->GetAlbedoMap()) << "fixture is wrong: the fresh import has no albedo map";
    ASSERT_TRUE(expected->GetAlbedoMap()->GetSpecification().SRGB)
        << "fixture is wrong: an OBJ map_Kd is a COLOUR texture, so the importer must load it as sRGB";

    // ── Now let the MODEL importer write the cache (the reverse of the test below) ──
    MeshCache::InvalidateCache(modelPath);
    MeshCache::InvalidateCache(modelPath, "static_uvflip_v1");
    {
        Model writer{ modelPath.string() };
        ASSERT_FALSE(writer.GetMaterials().empty());
    }
    ASSERT_TRUE(MeshCache::IsMeshCacheValid(modelPath, "static_uvflip_v1"))
        << "the Model importer did not write the cache the MeshSource importer will read — the warm half of this "
           "test would be vacuous";

    // ── The MeshSource importer WARM-loads it. It must be identical to the control. ──
    Ref<MeshSource> const warm = meshSourceImport();
    ASSERT_TRUE(warm);
    ASSERT_FALSE(warm->GetImportedMaterials().empty())
        << "the warm MeshSource load lost the imported materials entirely — every submesh would render with the "
           "flat engine-default material";
    Ref<Material> const actual = warm->GetImportedMaterials()[0];
    ASSERT_TRUE(actual);

    Ref<Texture2D> const albedo = actual->GetAlbedoMap();
    ASSERT_TRUE(albedo) << "the warm MeshSource load lost the albedo texture. On an alpha-masked material a null "
                           "albedo fails the cutoff and the submesh VANISHES; on an opaque one it shades black.";
    EXPECT_TRUE(albedo->GetSpecification().SRGB)
        << "the warm load resolved the albedo through the AssetManager, whose colour-space guess is FILENAME-based "
           "and cannot recognise a hash-named texture — so it came back LINEAR. It must be realized from the source "
           "file with the sRGB intent the IMPORTER recorded for that material slot. This is the same bug as the "
           "other importer order, and it was unpinned: which importer opens the mesh first is a function of scene "
           "content, so half the fix being right is a 50/50 coin flip in a real project.";
    EXPECT_EQ(albedo->GetWidth(), expected->GetAlbedoMap()->GetWidth());
    EXPECT_EQ(actual->GetAlphaMode(), expected->GetAlphaMode());
    for (int c = 0; c < 4; ++c)
    {
        EXPECT_FLOAT_EQ(actual->GetBaseColorFactor()[c], expected->GetBaseColorFactor()[c]);
    }
}

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

// ═════════════════════════════════════════════════════════════════════════════
// EMBEDDED textures (the KNOWN GAP the codec header used to document)
//
// A texture EMBEDDED in a glTF/GLB/FBX — a base64 data URI, or a binary chunk — has
// no file on disk and no AssetHandle. Model decoded it straight into a GPU texture
// and nothing else, so ImportedMaterialCodec (which references textures BY HANDLE,
// with a source-path fallback) had nothing to write down: DescribeTexture produced an
// EMPTY TextureRef and RealizeTexture skipped the slot without even a warning.
//
// The consequence was not subtle. Cold-import a GLB and it renders correctly; reopen
// the editor (warm .omesh load) and the material comes back with its factors and NO
// MAPS, and a shipped/packed build loses the textures of every model that embeds them
// — which is most GLB files in the wild.
//
// The fix is upstream of the codec: Model now COOKS an embedded texture into a real
// .png asset under <AssetDir>/cache/embedded/ at import time and ImportAsset()s it, so
// it gains a stable path AND a registry handle like any other texture — after which
// both containers already worked, with zero codec changes.
//
// These tests author a glTF whose ONLY albedo is a base64 data URI (no texture file
// exists anywhere on disk) and drive it through both containers, comparing the actual
// GPU PIXELS — not merely "non-null" — against the cold import.
// ═════════════════════════════════════════════════════════════════════════════

namespace
{
    std::string Base64Encode(const u8* data, sizet size)
    {
        static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((size + 2) / 3) * 4);
        for (sizet i = 0; i < size; i += 3)
        {
            const u32 b0 = data[i];
            const u32 b1 = (i + 1 < size) ? data[i + 1] : 0u;
            const u32 b2 = (i + 2 < size) ? data[i + 2] : 0u;
            const u32 triple = (b0 << 16) | (b1 << 8) | b2;

            out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
            out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
            out.push_back((i + 1 < size) ? kAlphabet[(triple >> 6) & 0x3F] : '=');
            out.push_back((i + 2 < size) ? kAlphabet[triple & 0x3F] : '=');
        }
        return out;
    }

    // The 4x4 albedo the fixture embeds. Deliberately NOT uniform: a test that compared
    // two all-black textures would pass while proving nothing, so every assertion below
    // also checks the readback actually varies.
    std::vector<u32> EmbeddedAlbedoPixels()
    {
        return {
            0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu,
            0xFF112233u, 0xFF445566u, 0xFF778899u, 0xFFAABBCCu,
            0xFF00FFFFu, 0xFFFF00FFu, 0xFFFFFF00u, 0xFF000000u,
            0xFF203040u, 0xFF506070u, 0xFF8090A0u, 0xFFB0C0D0u
        };
    }

    // Author a minimal glTF 2.0 whose geometry AND albedo are both base64 data URIs, so
    // NOTHING but the .gltf file exists on disk. This is the "embedded texture" case the
    // codec header called out; a GLB binary chunk lands on the very same Assimp path
    // (aiScene::mTextures + a "*N" material reference), which is what the production code
    // keys off, so the data-URI form exercises it without hand-packing a binary container.
    fs::path AuthorGltfWithEmbeddedAlbedo(const fs::path& assets, const std::vector<u32>& pixels,
                                          int width, int height)
    {
        // Encode the PNG via a scratch file OUTSIDE the asset directory, then slurp its
        // bytes and delete it: the .gltf must be the only thing the importer can see, so a
        // stray .png inside Assets/ would give the loader a real file to fall back on and
        // quietly defeat the point of the fixture.
        const fs::path scratchPng = assets.parent_path() / "embed_source.png";
        if (::stbi_write_png(scratchPng.string().c_str(), width, height, 4, pixels.data(), width * 4) == 0)
        {
            return {};
        }
        std::vector<u8> pngBytes;
        {
            std::ifstream in(scratchPng, std::ios::binary);
            if (!in)
            {
                return {};
            }
            pngBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::error_code removeEc;
        fs::remove(scratchPng, removeEc);
        if (pngBytes.empty())
        {
            return {};
        }
        const std::string pngB64 = Base64Encode(pngBytes.data(), pngBytes.size());

        // 3 positions (vec3) + 3 uvs (vec2) + 3 indices (u16) = 36 + 24 + 6 = 66 bytes.
        std::vector<u8> buffer;
        const auto pushFloat = [&buffer](float v)
        {
            u8 bytes[sizeof(float)];
            std::memcpy(bytes, &v, sizeof(float));
            buffer.insert(buffer.end(), std::begin(bytes), std::end(bytes));
        };
        const auto pushU16 = [&buffer](u16 v)
        {
            buffer.push_back(static_cast<u8>(v & 0xFFu));
            buffer.push_back(static_cast<u8>((v >> 8) & 0xFFu));
        };
        for (const auto& p : { std::array<float, 3>{ 0.f, 0.f, 0.f },
                               std::array<float, 3>{ 1.f, 0.f, 0.f },
                               std::array<float, 3>{ 0.f, 1.f, 0.f } })
        {
            pushFloat(p[0]);
            pushFloat(p[1]);
            pushFloat(p[2]);
        }
        for (const auto& uv : { std::array<float, 2>{ 0.f, 0.f },
                                std::array<float, 2>{ 1.f, 0.f },
                                std::array<float, 2>{ 0.f, 1.f } })
        {
            pushFloat(uv[0]);
            pushFloat(uv[1]);
        }
        pushU16(0);
        pushU16(1);
        pushU16(2);

        const std::string bufB64 = Base64Encode(buffer.data(), buffer.size());

        const fs::path gltfPath = assets / "embedded.gltf";
        std::ofstream gltf(gltfPath);
        gltf << R"({"asset":{"version":"2.0"},"scene":0,)"
             << R"("scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
             << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],)"
             << R"("materials":[{"name":"EmbeddedPainted","pbrMetallicRoughness":{)"
             << R"("baseColorTexture":{"index":0},"baseColorFactor":[1,1,1,1],)"
             << R"("metallicFactor":0.25,"roughnessFactor":0.75}}],)"
             << R"("textures":[{"source":0}],)"
             << R"("images":[{"mimeType":"image/png","uri":"data:image/png;base64,)" << pngB64 << R"("}],)"
             << R"("buffers":[{"byteLength":)" << buffer.size()
             << R"(,"uri":"data:application/octet-stream;base64,)" << bufB64 << R"("}],)"
             << R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},)"
             << R"({"buffer":0,"byteOffset":36,"byteLength":24,"target":34962},)"
             << R"({"buffer":0,"byteOffset":60,"byteLength":6,"target":34963}],)"
             << R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
             << R"("min":[0,0,0],"max":[1,1,0]},)"
             << R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},)"
             << R"({"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}]})";
        gltf.close();
        return gltfPath;
    }

    // The texture's ACTUAL contents, straight off the GPU (level 0, RGBA8). The point of
    // reading the GPU rather than a file is that it compares what the renderer will
    // sample — independent of which path (cooked file / registry handle / in-memory
    // decode) the material happened to resolve through.
    std::vector<u8> ReadTexturePixels(const Ref<Texture2D>& texture)
    {
        if (!texture || texture->GetWidth() == 0 || texture->GetHeight() == 0)
        {
            return {};
        }
        std::vector<u8> pixels(static_cast<sizet>(texture->GetWidth()) * texture->GetHeight() * 4u);
        ::glGetTextureImage(texture->GetRendererID(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                            static_cast<GLsizei>(pixels.size()), pixels.data());
        return pixels;
    }

    bool HasVaryingPixels(const std::vector<u8>& pixels)
    {
        return !pixels.empty() &&
               std::any_of(pixels.begin(), pixels.end(), [&](u8 b)
                           { return b != pixels[0]; });
    }
} // namespace

// The .omesh dev cache: cold-import writes it, warm-load reads it. Before the cook, the
// warm load came back with a material that had NO albedo at all.
TEST_F(ImportedMaterialPackTest, EmbeddedTextureSurvivesTheOmeshCache)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const fs::path assets = m_TempDir / "Assets";
    const std::vector<u32> pixels = EmbeddedAlbedoPixels();
    const fs::path modelPath = AuthorGltfWithEmbeddedAlbedo(assets, pixels, 4, 4);
    ASSERT_FALSE(modelPath.empty()) << "failed to author the embedded-texture glTF fixture";

    // The fixture's whole point: there is NO texture file anywhere on disk to fall back on.
    for (const auto& entry : fs::directory_iterator(assets))
    {
        ASSERT_NE(entry.path().extension(), ".png")
            << "fixture is wrong: a loose .png exists, so the texture is not really embedded";
    }

    MeshCache::InvalidateCache(modelPath);
    MeshCache::InvalidateCache(modelPath, "static_uvflip_v1");

    // ── Cold import: Assimp hands us the embedded bitmap; this is the ground truth ──
    std::vector<u8> coldPixels;
    {
        Model cold{ modelPath.string() };
        ASSERT_FALSE(cold.GetMaterials().empty()) << "the cold import produced no materials";
        Ref<Material> const material = cold.GetMaterials()[0];
        ASSERT_TRUE(material);
        Ref<Texture2D> const albedo = material->GetAlbedoMap();
        ASSERT_TRUE(albedo) << "fixture is wrong: Assimp did not surface the embedded texture at all, so this test "
                               "could never observe the serialization bug it exists to pin";
        EXPECT_TRUE(albedo->GetSpecification().SRGB) << "a baseColorTexture is a COLOUR texture";

        coldPixels = ReadTexturePixels(albedo);
        ASSERT_EQ(coldPixels.size(), pixels.size() * 4u);
        ASSERT_TRUE(HasVaryingPixels(coldPixels))
            << "the cold albedo is a uniform block — comparing it to anything would prove nothing";
    }
    // A glTF is not an OBJ, so it takes the un-prefixed cache variant (the
    // "static_uvflip_v1" prefix is the OBJ default-flipped-UV one).
    ASSERT_TRUE(MeshCache::IsMeshCacheValid(modelPath))
        << "the cold import did not write an .omesh, so the warm half below would be vacuous";

    // ── Warm load: geometry AND materials come from the .omesh, with no Assimp re-import ──
    Model warm{ modelPath.string() };
    ASSERT_FALSE(warm.GetMaterials().empty()) << "the warm load lost the materials entirely";
    Ref<Material> const warmMaterial = warm.GetMaterials()[0];
    ASSERT_TRUE(warmMaterial);

    Ref<Texture2D> const warmAlbedo = warmMaterial->GetAlbedoMap();
    ASSERT_TRUE(warmAlbedo)
        << "the warm .omesh load came back with NO albedo. An embedded texture has neither a handle nor a path, so "
           "ImportedMaterialCodec wrote an empty TextureRef and the material lost its map — the mesh renders with "
           "the factors only. Model must COOK the embedded bitmap into a real texture asset at import time.";
    EXPECT_TRUE(warmAlbedo->GetSpecification().SRGB)
        << "the albedo came back LINEAR — an embedded base-colour map must keep its sRGB intent through the cache";

    const std::vector<u8> warmPixels = ReadTexturePixels(warmAlbedo);
    ASSERT_TRUE(HasVaryingPixels(warmPixels));
    EXPECT_EQ(warmPixels, coldPixels)
        << "the albedo survived the .omesh but its CONTENT changed — the cook must round-trip the exact pixels "
           "(orientation included), not merely produce some texture";
}

// The asset pack: what a SHIPPED build reads. The codec references textures by handle,
// so the cooked texture must be a REGISTERED asset (that is what puts it in the pack).
TEST_F(ImportedMaterialPackTest, EmbeddedTextureSurvivesTheAssetPack)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const fs::path assets = m_TempDir / "Assets";
    const std::vector<u32> pixels = EmbeddedAlbedoPixels();
    const fs::path modelPath = AuthorGltfWithEmbeddedAlbedo(assets, pixels, 4, 4);
    ASSERT_FALSE(modelPath.empty());

    MeshCache::InvalidateCache(modelPath);
    MeshCache::InvalidateCache(modelPath, "static_uvflip_v1");

    const AssetHandle meshHandle = m_AssetManager->ImportAsset(modelPath);
    ASSERT_NE(static_cast<u64>(meshHandle), 0ULL);

    Ref<MeshSource> const cold = AssetManager::GetAsset<MeshSource>(meshHandle);
    ASSERT_TRUE(cold);
    ASSERT_FALSE(cold->GetImportedMaterials().empty()) << "the import carries no materials";
    Ref<Material> const coldMaterial = cold->GetImportedMaterials()[0];
    ASSERT_TRUE(coldMaterial);
    Ref<Texture2D> const coldAlbedo = coldMaterial->GetAlbedoMap();
    ASSERT_TRUE(coldAlbedo) << "fixture is wrong: the import surfaced no embedded albedo";

    const std::vector<u8> coldPixels = ReadTexturePixels(coldAlbedo);
    ASSERT_TRUE(HasVaryingPixels(coldPixels));

    // THE packed-runtime requirement. A packed build has no loose files and no Assimp: it
    // addresses textures purely by AssetHandle, and the pack contains exactly the assets in
    // the REGISTRY. A cooked texture that is not registered would neither be packed nor
    // addressable, so the material would ship mapless however good the .omesh looked.
    const auto descs = ImportedMaterialCodec::Describe(cold->GetImportedMaterials());
    ASSERT_FALSE(descs.empty());
    EXPECT_FALSE(descs[0].Albedo.IsEmpty())
        << "the codec wrote an EMPTY albedo TextureRef — the embedded texture has neither a handle nor a path, which "
           "is exactly how a shipped build loses it";
    EXPECT_NE(static_cast<u64>(descs[0].Albedo.Handle), 0ULL)
        << "the cooked embedded texture has no ASSET HANDLE. The path fallback is editor-only (a packed build has no "
           "loose files), so without a handle the texture is unreachable in a shipped game.";
    EXPECT_TRUE(descs[0].Albedo.SRGB);
    EXPECT_NE(static_cast<u64>(m_AssetManager->GetAssetHandleFromFilePath(descs[0].Albedo.Path)), 0ULL)
        << "the cooked texture is not in the asset registry, so the asset-pack builder (which enumerates the "
           "registry) would never include it";

    // ── Through the real pack serializer, and back ──
    Ref<MeshSource> const packed = PackRoundTrip(meshHandle);
    ASSERT_TRUE(packed);
    ASSERT_FALSE(packed->GetImportedMaterials().empty())
        << "the pack round-trip lost the imported materials — every submesh falls back to the engine default";
    Ref<Material> const packedMaterial = packed->GetImportedMaterials()[0];
    ASSERT_TRUE(packedMaterial);

    Ref<Texture2D> const packedAlbedo = packedMaterial->GetAlbedoMap();
    ASSERT_TRUE(packedAlbedo)
        << "the material came back from the asset pack with NO albedo — this is the shipped-game texture loss the "
           "cook exists to prevent";

    const std::vector<u8> packedPixels = ReadTexturePixels(packedAlbedo);
    ASSERT_TRUE(HasVaryingPixels(packedPixels));
    EXPECT_EQ(packedPixels, coldPixels) << "the packed albedo's CONTENT differs from the imported one";
    EXPECT_FLOAT_EQ(packedMaterial->GetMetallicFactor(), 0.25f); // the factors still ride along
    EXPECT_FLOAT_EQ(packedMaterial->GetRoughnessFactor(), 0.75f);
}

// STABILITY: the cooked texture's handle must not change between imports. If it did, the
// asset pack would churn on every build and any scene reference to the texture would rot.
// The cooked filename is derived from FNV-1a-64(model path | Assimp texture ref) + the
// colour-space intent — no counters, no timestamps — and ImportAsset returns the EXISTING
// handle for a path it has already registered.
TEST_F(ImportedMaterialPackTest, CookedEmbeddedTextureKeepsAStableHandleAcrossReimports)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const fs::path assets = m_TempDir / "Assets";
    const fs::path modelPath = AuthorGltfWithEmbeddedAlbedo(assets, EmbeddedAlbedoPixels(), 4, 4);
    ASSERT_FALSE(modelPath.empty());

    const auto importAndDescribeAlbedo = [&]() -> ImportedMaterialCodec::TextureRef
    {
        MeshCache::InvalidateCache(modelPath);
        MeshCache::InvalidateCache(modelPath, "static_uvflip_v1");
        Model model{ modelPath.string() };
        EXPECT_FALSE(model.GetMaterials().empty());
        const auto descs = ImportedMaterialCodec::Describe(model.GetMaterials());
        return descs.empty() ? ImportedMaterialCodec::TextureRef{} : descs[0].Albedo;
    };

    const ImportedMaterialCodec::TextureRef first = importAndDescribeAlbedo();
    ASSERT_NE(static_cast<u64>(first.Handle), 0ULL) << "the first import did not cook a handled texture";

    const ImportedMaterialCodec::TextureRef second = importAndDescribeAlbedo();
    EXPECT_EQ(static_cast<u64>(second.Handle), static_cast<u64>(first.Handle))
        << "re-importing the same model produced a DIFFERENT handle for the same embedded texture. The asset pack "
           "would change on every build and every scene reference to the texture would rot.";
    EXPECT_EQ(second.Path, first.Path)
        << "the cooked filename is not deterministic";

    // Exactly ONE cooked file for the one embedded texture — a counter-based or random
    // name would have left a second copy behind.
    const fs::path cookedDir = assets / "cache" / "embedded";
    ASSERT_TRUE(fs::exists(cookedDir));
    sizet cooked = 0;
    for (const auto& entry : fs::directory_iterator(cookedDir))
    {
        cooked += (entry.path().extension() == ".png") ? 1u : 0u;
    }
    EXPECT_EQ(cooked, 1u) << "re-import duplicated the cooked texture instead of reusing it";
}
