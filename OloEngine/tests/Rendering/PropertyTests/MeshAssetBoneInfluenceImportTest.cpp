// OLO_TEST_LAYER: integration
//
// An asset-imported MeshSource must carry the bone influences its source file contains
// (issue #1272).
//
// Every mesh ASSET resolves through MeshSourceSerializer::TryLoadData ->
// MeshImporterRegistry::Import -> AssimpMeshImporter, and that importer used to hardcode
// Model, which has no bone handling at all. AnimatedModel -- the one importer that extracts
// influences, bone info and the skeleton -- was reachable only through
// AnimationStateComponent::SourceFilePath, never from an AssetHandle. So
// MeshSource::HasBoneInfluences() was false for EVERY registered mesh asset, whatever the
// source file held, and a VirtualMeshComponent (which addresses its mesh by handle and
// nothing else) could never be skinned: the character stood in its bind pose while its
// skeleton animated.
//
// What these tests pin, and why each one is here:
//
//   * The import result for a rigged file carries influences, bone info and a skeleton --
//     the defect itself, on the path an asset actually takes.
//   * It carries them on the COLD and the WARM path SEPARATELY. The routing decision is
//     made from a cache header, so "it works" measured once only proves whichever half ran.
//     A single run passing is exactly how this could regress unnoticed.
//   * Materials survive the re-route. Rigged files now reach materials through a DIFFERENT
//     code path than before (AnimatedModel::ProcessMaterial rather than Model's
//     ImportedMaterialCodec), and a submesh whose material index does not address the table
//     shipped beside it renders flat engine-default grey -- the #629 failure mode.
//   * A STATIC file is unaffected. The re-route must not capture meshes that have no bones.
//   * Influences survive auto-LOD (issue #1278 taught the simplifier to carry them). A
//     rigged asset that keeps its influences at LOD 0 and loses them at LOD 1 deforms at
//     close range and snaps to bind pose as the camera pulls back.
//
// Model::LoadModel builds GPU buffers, so the import tests need a GL context and SKIP
// without one, like their neighbours (ModelWarmCacheGeometryIdentityTest). The
// .omesh header round-trip below needs no GPU and runs everywhere.

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/Interchange/MeshImporterRegistry.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Serialization/MeshBinaryFormat.h"
#include "OloEngine/Serialization/MeshBinarySerializer.h"

#include "OloEngine/Animation/MorphTargets/MorphTarget.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        // CesiumMan: 4,672 triangles over 19 joints, one material. The same file the
        // VirtualGeometrySkinned sample scene uses for all three of its characters, so a
        // failure here and a bind-posed character on screen are the same fact.
        std::filesystem::path RiggedModelPath()
        {
            return std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "assets/models/CesiumMan/CesiumMan.gltf";
        }

        // No bones anywhere in the file — the control for the re-route.
        std::filesystem::path StaticModelPath()
        {
            return std::filesystem::path{ OLO_TEST_EDITOR_ROOT } /
                   "assets/models/DeccerCubes/SM_Deccer_Cubes_Colored.glb";
        }

        // Drop BOTH cache namespaces so the next import is genuinely cold. The static and
        // animated prefixes are separate files; clearing one and not the other is how a
        // "cold" test quietly measures a warm path.
        void MakeColdImport(const std::filesystem::path& path)
        {
            MeshCache::InvalidateCache(path);
            MeshCache::InvalidateCache(path, AnimatedModel::kCachePrefix);
        }

        // Real skinning data, not merely a pre-allocated influence array: MeshSource sizes
        // m_BoneInfluences to the vertex count for every source, rigged or not, so length
        // alone proves nothing (see MeshSource::HasBoneInfluences).
        ::testing::AssertionResult HasRealSkinning(const MeshSource& source)
        {
            if (!source.HasBoneInfluences())
            {
                return ::testing::AssertionFailure()
                       << "MeshSource carries no bone influences (every weight is zero) — the import went "
                          "down the static path";
            }
            if (source.GetBoneInfluences().Num() != source.GetVertices().Num())
            {
                return ::testing::AssertionFailure()
                       << "bone influences (" << source.GetBoneInfluences().Num() << ") and vertices ("
                       << source.GetVertices().Num()
                       << ") are different lengths — every consumer indexes them in parallel, so this "
                          "deforms vertices by another vertex's bones";
            }
            return ::testing::AssertionSuccess();
        }

        // The invariant #629 is about: every submesh's material index must address the
        // table shipped with the MeshSource, or the submesh resolves someone else's
        // material (or falls off the end into engine-default grey).
        ::testing::AssertionResult MaterialsAddressEverySubmesh(const MeshSource& source)
        {
            const auto& materials = source.GetImportedMaterials();
            if (materials.empty())
            {
                return ::testing::AssertionFailure()
                       << "MeshSource ships no imported materials — a handle-addressed consumer has nothing "
                          "to resolve and renders flat engine-default grey";
            }

            const auto& submeshes = source.GetSubmeshes();
            for (i32 i = 0; i < submeshes.Num(); ++i)
            {
                const u32 index = submeshes[i].m_MaterialIndex;
                if (index >= static_cast<u32>(materials.size()))
                {
                    return ::testing::AssertionFailure()
                           << "submesh " << i << " has material index " << index << " but only "
                           << materials.size() << " material(s) ship with the source";
                }
                if (!materials[index])
                {
                    return ::testing::AssertionFailure()
                           << "submesh " << i << " resolves to a NULL material at index " << index;
                }
            }
            return ::testing::AssertionSuccess();
        }
    } // namespace

    // ── The defect, on the cold path ────────────────────────────────────────────────────
    TEST(MeshAssetBoneInfluenceImport, ColdImportOfARiggedFileCarriesBoneInfluences)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = RiggedModelPath();
        ASSERT_TRUE(std::filesystem::exists(path)) << "CesiumMan fixture missing: " << path.string();

        MakeColdImport(path);

        MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(result.Succeeded()) << result.Error;
        ASSERT_TRUE(result.Source);

        const MeshSource& source = *result.Source;
        EXPECT_TRUE(HasRealSkinning(source));
        EXPECT_TRUE(source.HasSkeleton()) << "no skeleton — the bone palette has nothing to index";
        EXPECT_FALSE(source.GetBoneInfo().IsEmpty()) << "no bone info — the inverse bind poses are missing";
        EXPECT_TRUE(source.IsSourceRigged())
            << "the source-rigged bit is clear, so the .omesh this import writes cannot tell the NEXT, "
               "warm load to route here again";

        // Every submesh of a rigged file must be MARKED rigged, or the renderer's skinned
        // submission path skips it while the skeleton animates.
        const auto& submeshes = source.GetSubmeshes();
        ASSERT_GT(submeshes.Num(), 0);
        for (i32 i = 0; i < submeshes.Num(); ++i)
        {
            EXPECT_TRUE(submeshes[i].m_IsRigged) << "submesh " << i << " is not marked rigged";
        }

        EXPECT_TRUE(MaterialsAddressEverySubmesh(source));
    }

    // ── ...and on the warm path, which is a DIFFERENT decision ──────────────────────────
    TEST(MeshAssetBoneInfluenceImport, WarmImportOfARiggedFileCarriesBoneInfluencesToo)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = RiggedModelPath();
        ASSERT_TRUE(std::filesystem::exists(path)) << "CesiumMan fixture missing: " << path.string();

        MakeColdImport(path);

        // First import: cold, and it writes the animated .omesh that the second one reads.
        MeshImportResult const cold = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(cold.Succeeded()) << cold.Error;
        ASSERT_TRUE(cold.Source);
        ASSERT_TRUE(HasRealSkinning(*cold.Source)) << "the cold half failed; the warm comparison would be vacuous";

        ASSERT_TRUE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix))
            << "the cold import wrote no ANIMATED cache — the warm path below would just re-run the cold one";
        EXPECT_TRUE(MeshCache::IsCachedSourceRigged(path, AnimatedModel::kCachePrefix))
            << "the animated cache header does not record that the source is rigged, so the warm import "
               "cannot route by it and falls back to the static path";

        // Second import: served from that cache, routed by its header alone.
        MeshImportResult const warm = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(warm.Succeeded()) << warm.Error;
        ASSERT_TRUE(warm.Source);

        const MeshSource& source = *warm.Source;
        EXPECT_TRUE(HasRealSkinning(source))
            << "the WARM import lost the influences the cold one produced — the routing decision is not "
               "surviving the cache round-trip";
        EXPECT_TRUE(source.HasSkeleton());
        EXPECT_EQ(source.GetVertices().Num(), cold.Source->GetVertices().Num())
            << "warm and cold imports disagree about the geometry";
        EXPECT_EQ(source.GetIndices().Num(), cold.Source->GetIndices().Num());
        EXPECT_EQ(source.GetSubmeshes().Num(), cold.Source->GetSubmeshes().Num());

        // Materials arrive on the warm path through the SAME re-opened-scene mechanism the
        // cold one uses, but that is an assumption worth asserting rather than trusting:
        // losing them here is the recorded flat-grey regression.
        EXPECT_TRUE(MaterialsAddressEverySubmesh(source));
    }

    // ── The control: a file with no bones must not be captured by the re-route ──────────
    TEST(MeshAssetBoneInfluenceImport, AStaticFileStillImportsStatically)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = StaticModelPath();
        ASSERT_TRUE(std::filesystem::exists(path)) << "deccer-cubes fixture missing: " << path.string();

        MakeColdImport(path);

        MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(result.Succeeded()) << result.Error;
        ASSERT_TRUE(result.Source);

        const MeshSource& source = *result.Source;
        EXPECT_FALSE(source.HasBoneInfluences()) << "a static mesh came back with skinning data";
        EXPECT_FALSE(source.IsSourceRigged()) << "a static mesh is recorded as rigged, which would send every "
                                                 "future load of it through the animated importer and cost it "
                                                 "its cooked cluster DAG";
        EXPECT_TRUE(MaterialsAddressEverySubmesh(source));
    }

    // ── Morph targets are the same routing question as bones ────────────────────────────
    // Model drops them exactly as it drops bones (aiProcess_PreTransformVertices rebuilds
    // every mesh without its anim meshes), so a file with morph targets and no skeleton -- a
    // sail, a flag -- reached its MeshComponent as a mesh nothing could deform. Cold and
    // warm are asked separately for the reason the rigged tests give: the warm route is
    // decided from a cache header, and the header has to say "come back here".
    TEST(MeshAssetBoneInfluenceImport, AMorphOnlyFileKeepsItsMorphTargetsColdAndWarm)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } /
                                           "SandboxProject/Assets/Models/DriftBoat/DriftBoatSail_LOD2.gltf";
        ASSERT_TRUE(std::filesystem::exists(path)) << "the Drift boat's sail is missing: " << path.string();

        MakeColdImport(path);

        for (const char* pass : { "cold", "warm" })
        {
            SCOPED_TRACE(pass);
            MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
            ASSERT_TRUE(result.Succeeded()) << result.Error;
            ASSERT_TRUE(result.Source);

            const MeshSource& source = *result.Source;
            EXPECT_FALSE(source.HasBoneInfluences()) << "the sail has no skeleton";
            ASSERT_TRUE(source.HasMorphTargets())
                << "the file's morph targets did not survive the import: the mesh cannot be deformed";
            const Ref<MorphTargetSet>& targets = source.GetMorphTargets();
            EXPECT_EQ(targets->GetTargetCount(), 10u);
            EXPECT_EQ(targets->CheckCompatibility(static_cast<u32>(source.GetVertices().Num())),
                      MorphTargetSet::ECompatibility::Compatible)
                << "the targets do not span the vertices that were imported with them";
            EXPECT_TRUE(source.IsSourceRigged())
                << "the cache header does not say this file needs the animated importer, so the next, "
                   "warm load takes the static path and drops the targets";
            EXPECT_TRUE(MaterialsAddressEverySubmesh(source));
        }
        EXPECT_TRUE(MeshCache::IsCachedSourceRigged(path, AnimatedModel::kCachePrefix));
    }

    // ── #1278's new expectation: influences must survive the auto-LOD chain ─────────────
    TEST(MeshAssetBoneInfluenceImport, ARiggedAssetKeepsItsInfluencesThroughAutoLOD)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = RiggedModelPath();
        ASSERT_TRUE(std::filesystem::exists(path)) << "CesiumMan fixture missing: " << path.string();

        MakeColdImport(path);

        MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(result.Succeeded()) << result.Error;
        ASSERT_TRUE(result.Source);
        ASSERT_TRUE(HasRealSkinning(*result.Source));

        // Half the triangles, which is well past the point where the simplifier has to
        // collapse and re-fetch vertices rather than pass them through.
        Ref<MeshSource> const lod = MeshOptimization::GenerateLODMeshWithAttributes(*result.Source, 0.5f);
        ASSERT_TRUE(lod) << "LOD generation failed outright";
        ASSERT_GT(lod->GetVertices().Num(), 0);

        EXPECT_TRUE(HasRealSkinning(*lod))
            << "the generated LOD dropped the bone influences. The mesh deforms at LOD 0 and snaps to its "
               "bind pose the moment the camera pulls back far enough to select LOD 1 (issue #1278 carries "
               "the influences through the same vertex-fetch remap the positions take).";
        EXPECT_TRUE(lod->HasSkeleton()) << "the generated LOD has no skeleton to index its bone palette with";
    }

    // ── The header bit the routing rests on, with no GPU in the way ─────────────────────
    TEST(MeshAssetBoneInfluenceImport, TheSourceRiggedBitSurvivesTheOmeshRoundTrip)
    {
        // The whole routing scheme rests on one header bit. It is what lets a WARM cache
        // answer "was this source rigged?" without a second full Assimp parse — and a bit
        // that silently failed to persist would read as an honest "not rigged", which is
        // the original bug wearing a different hat.
        const std::filesystem::path file = Tests::TempFile("source_rigged_bit.omesh");
        std::error_code ec;

        for (const bool rigged : { false, true })
        {
            auto source = Ref<MeshSource>::Create();
            source->GetVertices().Add(Vertex{});
            source->GetVertices().Add(Vertex{});
            source->GetVertices().Add(Vertex{});
            source->GetIndices().Add(0);
            source->GetIndices().Add(1);
            source->GetIndices().Add(2);
            source->SetSourceIsRigged(rigged);

            ASSERT_TRUE(MeshBinarySerializer::Write(file, *source, /*sourceTimestamp*/ 1234u))
                << "write failed for rigged=" << rigged;

            u32 flags = 0;
            ASSERT_TRUE(MeshBinarySerializer::ReadHeaderFlags(file, flags))
                << "header-only flag read failed for rigged=" << rigged;
            EXPECT_EQ((flags & OMeshFormat::FlagSourceRigged) != 0, rigged)
                << "the header FLAGS word does not carry FlagSourceRigged for rigged=" << rigged;

            Ref<MeshSource> const read = MeshBinarySerializer::Read(file);
            ASSERT_TRUE(read) << "read back failed for rigged=" << rigged;
            EXPECT_EQ(read->IsSourceRigged(), rigged)
                << "the deserialized MeshSource does not restore IsSourceRigged for rigged=" << rigged;
        }

        std::filesystem::remove(file, ec);
    }

    // ── The fourth acceptance criterion: influences reach a SHIPPED build ────────────────
    //
    // The asset pack is the only artifact a packaged game reads. An import that carries
    // influences in the editor and drops them in the pack moves the bug rather than fixing
    // it — and the failure would show up nowhere but in a shipped build, where the
    // character is bind-posed and there is no importer left to blame.
    class MeshAssetBoneInfluenceProjectTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_TempDir = Tests::TempDir();

            std::error_code ec;
            std::filesystem::remove_all(m_TempDir, ec);
            std::filesystem::create_directories(m_TempDir / "Assets", ec);
            ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

            const std::filesystem::path projectFile = m_TempDir / "Test.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: MeshAssetBoneInfluenceProject\n"
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
            // Project::Load and Project::SetAssetManager both write process-wide statics, so
            // resetting the local Ref leaves the ACTIVE project pointing at the directory
            // removed below. MeshCache resolves its cache directory through that project, so
            // a later case in the same process (a local full-suite run — ctest gives each
            // case its own) would address a deleted tree.
            Project::Unload();
            m_AssetManager.Reset();
            std::error_code ec;
            std::filesystem::remove_all(m_TempDir, ec);
        }

        Ref<MeshSource> RoundTrip(AssetHandle handle)
        {
            const std::filesystem::path packPath = m_TempDir / "mesh.pack";
            MeshSourceSerializer serializer;
            AssetSerializationInfo info{};
            {
                FileStreamWriter writer(packPath);
                EXPECT_TRUE(writer.IsStreamGood());
                EXPECT_TRUE(serializer.SerializeToAssetPack(handle, writer, info));
            }

            AssetPackFile::AssetInfo assetInfo{};
            assetInfo.Handle = static_cast<AssetHandle>(0xB0E5ULL);
            assetInfo.PackedOffset = info.Offset;
            assetInfo.PackedSize = info.Size;
            assetInfo.Type = AssetType::MeshSource;

            FileStreamReader reader(packPath);
            EXPECT_TRUE(reader.IsStreamGood());
            return serializer.DeserializeFromAssetPack(reader, assetInfo).As<MeshSource>();
        }

        std::filesystem::path m_TempDir;
        Ref<EditorAssetManager> m_AssetManager;
    };

    TEST_F(MeshAssetBoneInfluenceProjectTest, ARiggedAssetRoundTripsItsInfluencesThroughTheAssetPack)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = RiggedModelPath();
        ASSERT_TRUE(std::filesystem::exists(path)) << "CesiumMan fixture missing: " << path.string();

        MakeColdImport(path);

        MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(result.Succeeded()) << result.Error;
        ASSERT_TRUE(result.Source);
        ASSERT_TRUE(HasRealSkinning(*result.Source)) << "the import half failed; the pack half would be vacuous";

        // Importers hand back an unbuilt source (the headless cache path wants it that way);
        // the asset pipeline Builds it, so pack what the pipeline would pack.
        Ref<MeshSource> imported = result.Source;
        imported->Build();
        AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(imported);
        Ref<MeshSource> const unpacked = RoundTrip(handle);
        ASSERT_TRUE(unpacked) << "DeserializeFromAssetPack returned null";

        EXPECT_TRUE(HasRealSkinning(*unpacked))
            << "the packed MeshSource carries no bone influences — the editor animates and a SHIPPED "
               "build stands in its bind pose";
        EXPECT_TRUE(unpacked->HasSkeleton()) << "the pack dropped the skeleton";
        EXPECT_FALSE(unpacked->GetBoneInfo().IsEmpty()) << "the pack dropped the inverse bind poses";
        EXPECT_EQ(unpacked->GetVertices().Num(), result.Source->GetVertices().Num());
        EXPECT_EQ(unpacked->GetBoneInfluences().Num(), result.Source->GetBoneInfluences().Num());

        const auto& submeshes = unpacked->GetSubmeshes();
        ASSERT_GT(submeshes.Num(), 0);
        for (i32 i = 0; i < submeshes.Num(); ++i)
        {
            EXPECT_TRUE(submeshes[i].m_IsRigged) << "packed submesh " << i << " lost its rigged flag";
        }
    }

    // ── The routing predicate must not answer for a file that has since changed ─────────
    TEST_F(MeshAssetBoneInfluenceProjectTest, TheCachedRiggedBitStopsAnsweringOnceTheSourceChanges)
    {
        // IsCachedSourceRigged is read BEFORE any importer runs, so it is the one place that
        // decides a rigged file's fate. Its flag word describes the source as it was when the
        // cache was written; answering from a stale entry means a mesh re-exported WITHOUT
        // its rig keeps being routed to AnimatedModel, on the strength of a cache that the
        // routing then never reads. No GPU needed — this is a cache-header question.
        const std::filesystem::path source = m_TempDir / "Assets" / "rigged_source.gltf";
        {
            // The smallest valid glTF: the cache key reads the descriptor for its external
            // buffers (a .gltf that does not parse is never cached), and it has none.
            std::ofstream f(source);
            f << R"({ "asset": { "version": "2.0" } })" << '\n';
        }
        ASSERT_TRUE(std::filesystem::exists(source));

        auto meshSource = Ref<MeshSource>::Create();
        meshSource->GetVertices().Add(Vertex{});
        meshSource->GetVertices().Add(Vertex{});
        meshSource->GetVertices().Add(Vertex{});
        meshSource->GetIndices().Add(0);
        meshSource->GetIndices().Add(1);
        meshSource->GetIndices().Add(2);
        meshSource->SetSourceIsRigged(true);

        ASSERT_TRUE(MeshCache::SaveMeshToCache(source, *meshSource, AnimatedModel::kCachePrefix));
        ASSERT_TRUE(MeshCache::IsCachedSourceRigged(source, AnimatedModel::kCachePrefix))
            << "a freshly written cache does not report the rigged bit — the warm route cannot work";

        // Re-export the source. The cache file is untouched and still says "rigged".
        std::error_code ec;
        const auto bumped = std::filesystem::last_write_time(source, ec) + std::chrono::seconds(120);
        ASSERT_FALSE(ec);
        std::filesystem::last_write_time(source, bumped, ec);
        ASSERT_FALSE(ec);

        EXPECT_FALSE(MeshCache::IsCachedSourceRigged(source, AnimatedModel::kCachePrefix))
            << "a STALE cache still answers the routing question. A mesh re-exported without its rig "
               "would keep being handed to AnimatedModel because a cache entry that no longer matches "
               "the file says it has bones.";
    }

    // The animated route has to bring a file's EMBEDDED textures with it. AnimatedModel
    // read "*0" as a file name beside the model and found nothing, which stayed out of
    // sight while only rigged files went that way (the rigged fixtures all name external
    // images) and became a regression for every morph-only file the moment those were
    // routed there: a sail or a flag that embedded its map kept its targets and lost its
    // colour. One triangle, one morph target, one 4x4 PNG as a data URI.
    TEST_F(MeshAssetBoneInfluenceProjectTest, AMorphOnlyFileKeepsItsEmbeddedTextureColdAndWarm)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // In the fixture's project, because the cook writes the texture into the active
        // project's cache: with none mounted that is the repository's own assets folder,
        // and the test would leave a file outside its temp directory.
        const std::filesystem::path path = m_TempDir / "Assets" / "morph_only_embedded_texture.gltf";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(out.good()) << path.string();
            out
                << R"gltf({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"name":"Flag"}],"meshes":[{"n)gltf"
                << R"gltf(ame":"Flag","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":3,"material":0,"targets":[{"PO)gltf"
                << R"gltf(SITION":2}]}],"weights":[0.0],"extras":{"targetNames":["Lift"]}}],"materials":[{"name":"FlagCloth","pbrMetalli)gltf"
                << R"gltf(cRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0.0}}],"textures":[{"source":0}],"images":[{"uri")gltf"
                << R"gltf(:"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAGklEQVR4nGM4oSEnZ3MCQjLAWSc05BhwygAAx0)gltf"
                << R"gltf(MRgeQ5GPwAAAAASUVORK5CYII=","name":"FlagColour"}],"buffers":[{"byteLength":104,"uri":"data:application/octet-s)gltf"
                << R"gltf(tream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAAA/A)gltf"
                << R"gltf(AAAAAAAAAAAAAA/AAAAAAAAAAAAAAA/AAABAAIAAAA="}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"targ)gltf"
                << R"gltf(et":34962},{"buffer":0,"byteOffset":36,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":60,"byteLength)gltf"
                << R"gltf(":36},{"buffer":0,"byteOffset":96,"byteLength":6,"target":34963}],"accessors":[{"bufferView":0,"componentType")gltf"
                << R"gltf(:5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"typ)gltf"
                << R"gltf(e":"VEC2"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0.5],"max":[0,0,0.5]},{"buf)gltf"
                << R"gltf(ferView":3,"componentType":5123,"count":3,"type":"SCALAR"}]})gltf";
        }

        MakeColdImport(path);

        for (const char* pass : { "cold", "warm" })
        {
            SCOPED_TRACE(pass);
            MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
            ASSERT_TRUE(result.Succeeded()) << result.Error;
            ASSERT_TRUE(result.Source);

            const MeshSource& source = *result.Source;
            ASSERT_TRUE(source.HasMorphTargets()) << "the file did not take the animated route";
            EXPECT_EQ(source.GetMorphTargets()->GetTargetCount(), 1u);
            ASSERT_TRUE(MaterialsAddressEverySubmesh(source));

            const Ref<Material>& material = source.GetImportedMaterials()[source.GetSubmeshes()[0].m_MaterialIndex];
            const Ref<Texture2D> albedo = material->GetAlbedoMap();
            ASSERT_TRUE(albedo) << "the embedded base-colour texture was not loaded: the material has no albedo map";
            EXPECT_TRUE(albedo->IsLoaded());
            EXPECT_EQ(albedo->GetWidth(), 4u);
            EXPECT_EQ(albedo->GetHeight(), 4u);

            // The second pass is only the warm one if this pass left an animated cache that
            // says to come back this way; without it the loop would run the cold route twice
            // and pass, having never rebuilt a material from the cache.
            ASSERT_TRUE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix))
                << "the " << pass << " import left no valid animated cache";
            ASSERT_TRUE(MeshCache::IsCachedSourceRigged(path, AnimatedModel::kCachePrefix))
                << "the " << pass << " import's cache does not route the next load to the animated importer";
        }
        MakeColdImport(path);
    }

    // A morph-only mesh takes its node's transform through the animated importer, as the
    // static importer (aiProcess_PreTransformVertices) always gave it. AnimatedModel reads
    // vertices in the mesh's own space and leaves a skinned mesh's node to its skeleton; a
    // mesh with no bones has no skeleton, so before this a morph-only mesh on a moved or
    // scaled node arrived at the origin in its local size once it was routed here.
    TEST_F(MeshAssetBoneInfluenceProjectTest, AMorphOnlyMeshOnAMovedAndScaledNodeIsImportedWhereTheNodePutsIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // One triangle in the XY plane with one morph target that lifts it a metre along Z,
        // on a node moved ten metres along X and scaled by two. The second pass mirrors the
        // node in X as well: the triangle must still face the way its normal points.
        for (const f32 scaleX : { 2.0f, -2.0f })
        {
            SCOPED_TRACE(scaleX > 0.0f ? "scaled node" : "mirrored node");
            const std::string stem = scaleX > 0.0f ? "morph_on_a_node" : "morph_on_a_mirrored_node";
            const std::filesystem::path source = m_TempDir / "Assets" / (stem + ".gltf");
            {
                const f32 positions[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
                const f32 normals[9] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
                const f32 deltas[9] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
                const u16 indices[4] = { 0, 1, 2, 0 }; // the fourth pads the view to four bytes
                std::ofstream bin(m_TempDir / "Assets" / (stem + ".bin"), std::ios::binary);
                bin.write(reinterpret_cast<const char*>(positions), sizeof(positions));
                bin.write(reinterpret_cast<const char*>(normals), sizeof(normals));
                bin.write(reinterpret_cast<const char*>(deltas), sizeof(deltas));
                bin.write(reinterpret_cast<const char*>(indices), sizeof(indices));
                ASSERT_TRUE(bin.good());

                std::ofstream gltf(source);
                gltf << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0, "translation": [ 10.0, 0.0, 0.0 ], "scale": [ )"
                     << scaleX << R"(, 2.0, 2.0 ] } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "NORMAL": 1 }, "indices": 3,
                                 "targets": [ { "POSITION": 2 } ] } ], "weights": [ 0.0 ] } ],
  "buffers": [ { "uri": ")"
                     << stem << R"(.bin", "byteLength": 116 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 72, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 108, "byteLength": 6, "target": 34963 } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [ 0.0, 0.0, 0.0 ], "max": [ 1.0, 1.0, 0.0 ] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3", "min": [ 0.0, 0.0, 1.0 ], "max": [ 0.0, 0.0, 1.0 ] },
    { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" } ]
})" << '\n';
                ASSERT_TRUE(gltf.good());
            }

            MakeColdImport(source);
            MeshImportResult const result = MeshImporterRegistry::Get().Import(source);
            ASSERT_TRUE(result.Succeeded()) << result.Error;
            ASSERT_TRUE(result.Source);
            const MeshSource& imported = *result.Source;
            ASSERT_TRUE(imported.HasMorphTargets()) << "the morph target did not survive: this is not the animated import";
            ASSERT_EQ(imported.GetVertices().Num(), 3);
            ASSERT_EQ(imported.GetIndices().Num(), 3);

            glm::vec3 lo(std::numeric_limits<f32>::max());
            glm::vec3 hi(std::numeric_limits<f32>::lowest());
            for (const Vertex& vertex : imported.GetVertices())
            {
                lo = glm::min(lo, vertex.Position);
                hi = glm::max(hi, vertex.Position);
            }
            EXPECT_NEAR(std::min(std::abs(lo.x - 10.0f), std::abs(hi.x - 10.0f)), 0.0f, 1.0e-4f)
                << "the mesh was imported at the origin, without its node's translation";
            EXPECT_NEAR(hi.x - lo.x, 2.0f, 1.0e-4f) << "the mesh was imported without its node's scale";
            EXPECT_NEAR(scaleX > 0.0f ? hi.x : lo.x, 10.0f + scaleX, 1.0e-4f);
            EXPECT_NEAR(hi.y, 2.0f, 1.0e-4f);

            // The triangle faces where its normals point, mirrored or not.
            const auto& vertices = imported.GetVertices();
            const auto& indices = imported.GetIndices();
            const glm::vec3 face = glm::cross(vertices[static_cast<i32>(indices[1])].Position - vertices[static_cast<i32>(indices[0])].Position,
                                              vertices[static_cast<i32>(indices[2])].Position - vertices[static_cast<i32>(indices[0])].Position);
            EXPECT_GT(glm::dot(face, vertices[0].Normal), 0.0f)
                << "the node mirrors the mesh and the triangle was not turned back: it faces away from its own normal";
            EXPECT_NEAR(vertices[0].Normal.z, 1.0f, 1.0e-4f);

            const auto& target = imported.GetMorphTargets()->Targets[0];
            ASSERT_EQ(target.Vertices.size(), 3u);
            for (const MorphTargetVertex& delta : target.Vertices)
            {
                EXPECT_NEAR(delta.DeltaPosition.x, 0.0f, 1.0e-4f) << "a delta took the node's translation: it is a difference, not a point";
                EXPECT_NEAR(delta.DeltaPosition.z, 2.0f, 1.0e-4f) << "the morph delta was not scaled with the mesh it moves";
            }
        }
    }
} // namespace OloEngine::Tests
