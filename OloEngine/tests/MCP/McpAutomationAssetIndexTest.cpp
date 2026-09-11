// OLO_TEST_LAYER: unit

// The asset reference index (issue #1128 slice 1) against a real directory
// tree, with no project, no asset registry and no editor -- which is the whole
// reason the index was written as a pure function of a scope rather than as a
// call into AssetManager.
//
// What these cases are actually defending. "What references this asset" is the
// question every safe move and every refused delete is built on, and its two
// failure directions are not symmetric. A MISSED referrer is silent data loss:
// the delete goes through, the scene still loads, the mesh renders untextured
// and nothing says why. An INVENTED referrer is merely an annoying refusal. So
// the cases below lean hard on the missed direction -- indexed material maps,
// the five path spellings in checked-in content, references from a file format
// nobody thought about -- and on the one case where inventing a referrer would
// be catastrophic rather than annoying: two different files with the same name.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationAssetIndex.h"
#include "TestTempDir.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace OloEngine::Automation::Tests
{
    namespace
    {
        void Write(const std::filesystem::path& path, const std::string& text)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(output) << "cannot create " << path.string();
            output << text;
        }

        std::string Read(const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        }
    } // namespace

    class AutomationAssetIndexTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Root = OloEngine::Tests::TempDir("assetindex");
            // Two roots that both contain a Checkerboard.png, mirroring the real
            // tree: OloEditor/assets/textures/ (engine) and the project's own
            // Assets/Textures/. m_Root plays OloEditor/, m_Project plays
            // OloEditor/SandboxProject/.
            m_Project = m_Root / "SandboxProject";
            std::error_code ec;
            std::filesystem::create_directories(m_Project / "Assets" / "Scenes", ec);
            Write(m_Project / "Assets" / "Textures" / "Checkerboard.png", "project-png");
            Write(m_Root / "assets" / "textures" / "Checkerboard.png", "engine-png");

            m_Scope.ProjectRoot = m_Project;
            m_Scope.AssetDirectory = m_Project / "Assets";
            m_Scope.BaseDirectories = { m_Root };
        }

        [[nodiscard]] std::filesystem::path ProjectTexture() const
        {
            return std::filesystem::weakly_canonical(m_Project / "Assets" / "Textures" / "Checkerboard.png");
        }

        [[nodiscard]] AssetIndex Build() const
        {
            return BuildAssetIndex(m_Scope);
        }

        std::filesystem::path m_Root;
        std::filesystem::path m_Project;
        AssetIndexScope m_Scope;
    };

    // --- the load-bearing case: a scene naming a texture ---------------------

    TEST_F(AutomationAssetIndexTest, FindsAProjectRelativeSceneReference)
    {
        Write(m_Project / "Assets" / "Scenes" / "Test.olo",
              "Scene: Test\nEntities:\n  - Entity: 12345\n    DecalComponent:\n"
              "      AlbedoTexturePath: Assets/Textures/Checkerboard.png\n");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, ProjectTexture(), 0);
        ASSERT_EQ(referrers.size(), 1u) << "a scene naming a texture project-relative must be found -- this is the "
                                           "spelling SceneSerializer writes today";
        EXPECT_EQ(referrers[0].Key, "AlbedoTexturePath");
        EXPECT_EQ(referrers[0].RawValue, "Assets/Textures/Checkerboard.png");
        EXPECT_EQ(referrers[0].Anchor, AssetReferenceAnchor::ProjectRelative);
        EXPECT_EQ(referrers[0].Line, 5u);
    }

    // A same-named file under a DIFFERENT root must not be reported as a referrer
    // of this one. Matching on filename or path suffix would pass every other
    // case in this file and get this one wrong -- and the consequence is not just
    // a spurious refusal: a MOVE would rewrite a reference that was never about
    // this file, pointing it at somewhere the real target never was.
    //
    // The two directories deliberately differ by more than case. An earlier
    // version of this case used "assets/textures" against "Assets/Textures" and
    // asserted the reference resolved to the engine copy. That is true on Linux
    // and false on Windows, where the case-insensitive filesystem makes the
    // project-relative candidate exist and win -- which is also what
    // EditorAssetManager::ImportAsset does, so the index agreeing with it is
    // correct. But a test whose expected answer depends on the host filesystem
    // is not a test.
    TEST_F(AutomationAssetIndexTest, DoesNotInventAReferrerForASameNamedFileElsewhere)
    {
        Write(m_Project / "Assets" / "Textures" / "Shared.png", "project-copy");
        Write(m_Root / "engineassets" / "Shared.png", "engine-copy");
        Write(m_Project / "Assets" / "Scenes" / "Engine.olo",
              "Scene: Engine\nEntities:\n  - Entity: 1\n    DecalComponent:\n"
              "      AlbedoTexturePath: engineassets/Shared.png\n");

        const AssetIndex index = Build();
        const auto projectReferrers = FindReferrers(
            index, std::filesystem::weakly_canonical(m_Project / "Assets" / "Textures" / "Shared.png"), 0);
        const auto engineReferrers =
            FindReferrers(index, std::filesystem::weakly_canonical(m_Root / "engineassets" / "Shared.png"), 0);

        EXPECT_TRUE(projectReferrers.empty())
            << "the project's own Shared.png is not what 'engineassets/Shared.png' names; reporting it would be a "
               "fabricated referrer, and a move would then rewrite somebody else's reference";
        EXPECT_EQ(engineReferrers.size(), 1u) << "...and it must still be found against the root it does resolve to";
        EXPECT_EQ(engineReferrers[0].Anchor, AssetReferenceAnchor::BaseDirectory);
    }

    // Project::GetAssetFileSystemPath -- what Scene.cpp resolves a
    // LuaScriptComponent's ScriptFile with, and SceneSerializer several component
    // paths. Every Lua script and every audio clip in the real SandboxProject is
    // spelled this way, so an index without this anchor drops those referrers
    // silently. It WAS missing: a scan of the real project reported 30-odd of
    // them as unresolved, which is what found it.
    TEST_F(AutomationAssetIndexTest, ResolvesAgainstTheAssetDirectoryToo)
    {
        Write(m_Project / "Assets" / "Scripts" / "Goblin.lua", "-- script");
        Write(m_Project / "Assets" / "Scenes" / "Scripted.olo",
              "Scene: Scripted\nEntities:\n  - Entity: 1\n    LuaScriptComponent:\n"
              "      ScriptFile: Scripts/Goblin.lua\n");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(
            index, std::filesystem::weakly_canonical(m_Project / "Assets" / "Scripts" / "Goblin.lua"), 0);
        ASSERT_EQ(referrers.size(), 1u) << "a ScriptFile is asset-directory-relative, not project-relative";
        EXPECT_EQ(referrers[0].Anchor, AssetReferenceAnchor::AssetDirectoryRelative);
    }

    // Every scene in this project opens with `Scene: Courtyard.olo` -- its title,
    // which happens to end in an asset extension. Collecting those as path
    // references reported 99 of them as broken in the real SandboxProject and
    // buried any genuine breakage in the same list.
    TEST_F(AutomationAssetIndexTest, ABareNameThatResolvesToNothingIsNotAReference)
    {
        Write(m_Project / "Assets" / "Scenes" / "Named.olo", "Scene: Named.olo\nEntities: []\n");

        const AssetIndex index = Build();
        EXPECT_EQ(index.Coverage.UnresolvedReferences, 0u)
            << "a bare filename with no directory separator that resolves to nothing is a NAME, not a broken "
               "reference; the coverage note states the ambiguity rather than guessing either way";
    }

    TEST_F(AutomationAssetIndexTest, ResolvesTheLegacyProjectPrefixedSpelling)
    {
        // The spelling several checked-in scenes still carry (#1098). It only
        // ever resolved because the editor's cwd sits one level above the
        // project, and EditorAssetManager now strips the stale leading component.
        Write(m_Project / "Assets" / "Scenes" / "Legacy.olo",
              "Scene: Legacy\nEntities:\n  - Entity: 1\n    MeshComponent:\n"
              "      MeshPath: SandboxProject/Assets/Textures/Checkerboard.png\n");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, ProjectTexture(), 0);
        ASSERT_EQ(referrers.size(), 1u);
        EXPECT_EQ(referrers[0].Anchor, AssetReferenceAnchor::LegacyProjectPrefixed);
    }

    // --- the direction that loses data --------------------------------------

    // A StaticMesh's MaterialTable writes `Materials:` then `0: <handle>`. The
    // key "0" qualifies nothing on its own, so an index that classified handles
    // by key name alone would report ZERO referrers for a material half the
    // meshes in a project point at -- and the delete would go through.
    TEST_F(AutomationAssetIndexTest, FindsHandlesInsideAnIndexedMaterialMap)
    {
        Write(m_Project / "Assets" / "Meshes" / "Rock.olosmesh",
              "StaticMesh:\n  MeshSource: 111\n  MaterialTable:\n    Materials:\n"
              "      0: 4242\n      1: 4243\n");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, {}, 4242);
        ASSERT_EQ(referrers.size(), 1u) << "an indexed map entry under Materials: must inherit the block's name";
        EXPECT_EQ(referrers[0].Key, "Materials/0");
        EXPECT_EQ(referrers[0].HandleValue, 4242u);
        EXPECT_EQ(referrers[0].Kind, AssetReferenceKind::Handle);
    }

    // An entity UUID is a u64 in a scene exactly like an asset handle is. Listing
    // every entity as an asset dependency would make the dependency answer
    // useless, so handle candidates are key-filtered -- and these are the keys
    // that must never qualify.
    TEST_F(AutomationAssetIndexTest, EntityIdentityKeysAreNotAssetHandles)
    {
        Write(m_Project / "Assets" / "Scenes" / "Entities.olo",
              "Scene: Entities\nEntities:\n  - Entity: 900001\n    RelationshipComponent:\n"
              "      ParentHandle: 900002\n    PrefabComponent:\n"
              "      PrefabID: 900003\n      PrefabEntityID: 900004\n");

        const AssetIndex index = Build();
        EXPECT_TRUE(FindReferrers(index, {}, 900001).empty()) << "'Entity:' is an entity UUID";
        EXPECT_TRUE(FindReferrers(index, {}, 900002).empty()) << "'ParentHandle:' is an entity UUID";
        EXPECT_TRUE(FindReferrers(index, {}, 900004).empty()) << "'PrefabEntityID:' names an entity inside a prefab";
        EXPECT_EQ(FindReferrers(index, {}, 900003).size(), 1u)
            << "'PrefabID:' beside it IS the prefab asset handle, and dropping it would let a referenced prefab "
               "be deleted silently";
    }

    TEST_F(AutomationAssetIndexTest, ReportsAnUnresolvableReferenceInsteadOfDroppingIt)
    {
        Write(m_Project / "Assets" / "Scenes" / "Broken.olo",
              "Scene: Broken\nEntities:\n  - Entity: 1\n    MeshComponent:\n"
              "      MeshPath: Assets/Models/DoesNotExist.obj\n");

        const AssetIndex index = Build();
        EXPECT_EQ(index.Coverage.UnresolvedReferences, 1u)
            << "a path that resolves to nothing is a BROKEN reference and is worth reporting; dropping it would "
               "hide the very breakage these commands exist to prevent";
        const auto broken = std::ranges::find_if(index.References, [](const AssetReference& reference)
                                                 { return !reference.IsResolved(); });
        ASSERT_NE(broken, index.References.end());
        EXPECT_EQ(broken->RawValue, "Assets/Models/DoesNotExist.obj");
    }

    TEST_F(AutomationAssetIndexTest, IgnoresACommentedOutReference)
    {
        Write(m_Project / "Assets" / "Scenes" / "Commented.olo",
              "Scene: Commented\n# AlbedoTexturePath: Assets/Textures/Checkerboard.png\n"
              "Entities: []\n");

        const AssetIndex index = Build();
        EXPECT_TRUE(FindReferrers(index, ProjectTexture(), 0).empty())
            << "a commented-out line is not a reference, and treating it as one would refuse a legitimate delete";
    }

    TEST_F(AutomationAssetIndexTest, AnAssetIsNotItsOwnReferrer)
    {
        // A material that names its own file in a header field. Reporting that as
        // a referrer would make every such asset undeletable.
        const auto material = m_Project / "Assets" / "Materials" / "Self.olomaterial";
        Write(material, "Material:\n  Name: Self\n  SourceFilePath: Assets/Materials/Self.olomaterial\n");

        const AssetIndex index = Build();
        EXPECT_TRUE(FindReferrers(index, std::filesystem::weakly_canonical(material), 0).empty());
    }

    TEST_F(AutomationAssetIndexTest, CoverageNamesTheBinaryFormatsItDidNotSearch)
    {
        Write(m_Project / "Assets" / "Models" / "Ship.glb", "binary-ish");
        Write(m_Project / "Assets" / "Scenes" / "Empty.olo", "Scene: Empty\n");

        const AssetIndex index = Build();
        EXPECT_GE(index.Coverage.BinaryFilesSkipped, 1u);
        EXPECT_NE(std::ranges::find(index.Coverage.BinaryExtensionsSkipped, std::string(".glb")),
                  index.Coverage.BinaryExtensionsSkipped.end())
            << "a .glb can carry external references this scan cannot see; the result has to say so, because an "
               "empty referrer list otherwise reads as 'safe to delete'";
        EXPECT_FALSE(index.Coverage.Truncated);
    }

    TEST_F(AutomationAssetIndexTest, TruncationIsReportedRatherThanSilentlyShorteningTheIndex)
    {
        for (int i = 0; i < 8; ++i)
            Write(m_Project / "Assets" / "Scenes" / ("S" + std::to_string(i) + ".olo"), "Scene: S\n");
        m_Scope.MaxFiles = 3;

        const AssetIndex index = Build();
        EXPECT_TRUE(index.Coverage.Truncated)
            << "a bounded walk that stops early MUST say so -- every destructive command refuses on this flag";
        EXPECT_LE(index.Coverage.FilesScanned, 3u);
    }

    // A scan that never ran returns zero references. Without a flag saying so,
    // that is indistinguishable at the call site from "nothing references this
    // asset" -- and a destructive command gating only on Truncated would read the
    // clean-looking zero and go ahead. Reliable() is what both commands check.
    TEST_F(AutomationAssetIndexTest, AScanThatCouldNotRunIsNotReportedAsClean)
    {
        AssetIndexScope broken = m_Scope;
        broken.ProjectRoot = m_Root / "does-not-exist";

        const AssetIndex index = BuildAssetIndex(broken);
        EXPECT_TRUE(index.References.empty());
        EXPECT_FALSE(index.Coverage.Truncated) << "it did not hit the file limit -- it never started";
        EXPECT_FALSE(index.Coverage.Complete);
        EXPECT_FALSE(index.Coverage.ScanCompleted())
            << "an empty index from a scan that never ran must never read as 'nothing references this'";
        EXPECT_FALSE(index.Coverage.IncompleteReason.empty()) << "and it has to say why";
    }

    TEST_F(AutomationAssetIndexTest, AnEmptyProjectRootIsAlsoUnreliable)
    {
        AssetIndexScope broken;
        const AssetIndex index = BuildAssetIndex(broken);
        EXPECT_FALSE(index.Coverage.ScanCompleted());
        EXPECT_FALSE(index.Coverage.IncompleteReason.empty());
    }

    TEST_F(AutomationAssetIndexTest, AHealthyScanIsReliable)
    {
        Write(m_Project / "Assets" / "Scenes" / "Fine.olo",
              "Scene: Fine\n  AlbedoPath: Assets/Textures/Checkerboard.png\n");
        const AssetIndex index = Build();
        EXPECT_TRUE(index.Coverage.ScanCompleted());
        EXPECT_TRUE(index.Coverage.Complete);
        EXPECT_TRUE(index.Coverage.IncompleteReason.empty());
    }

    // A .gltf is JSON and names its textures by URI relative to itself. Assimp
    // resolves them that way, so a texture reachable only from a .gltf is really
    // referenced -- and leaving the format unscanned made it look deletable.
    TEST_F(AutomationAssetIndexTest, FindsAReferenceFromAGltfRelativeToItself)
    {
        // The real DamagedHelmet.gltf shape: quoted key, space before the colon,
        // trailing comma. None of those parsed before.
        Write(m_Project / "Assets" / "Models" / "Helmet" / "Helmet.gltf",
              "{\n"
              "    \"images\" : [\n"
              "        {\n"
              "            \"uri\" : \"Default_albedo.jpg\",\n"
              "            \"name\" : \"albedo\"\n"
              "        }\n"
              "    ]\n"
              "}\n");
        const auto texture = m_Project / "Assets" / "Models" / "Helmet" / "Default_albedo.jpg";
        Write(texture, "jpg-ish");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, std::filesystem::weakly_canonical(texture), 0);
        ASSERT_EQ(referrers.size(), 1u) << "a glTF URI is relative to the .gltf file itself";
        EXPECT_EQ(referrers[0].Anchor, AssetReferenceAnchor::SourceRelative);
        EXPECT_EQ(referrers[0].Key, "uri");
        EXPECT_EQ(referrers[0].RawValue, "Default_albedo.jpg")
            << "the trailing comma is punctuation, not part of the value";
    }

    // The source-relative anchor is tried LAST, so it can never take a value the
    // engine's own resolver would have resolved differently.
    TEST_F(AutomationAssetIndexTest, SourceRelativeNeverOutranksTheProjectAnchor)
    {
        Write(m_Project / "Assets" / "Scenes" / "Both.olo",
              "Scene: Both\n  AlbedoPath: Assets/Textures/Checkerboard.png\n");
        // A decoy at the same path relative to the SCENE's own directory.
        Write(m_Project / "Assets" / "Scenes" / "Assets" / "Textures" / "Checkerboard.png", "decoy");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, ProjectTexture(), 0);
        ASSERT_EQ(referrers.size(), 1u) << "the project-relative anchor must still win";
        EXPECT_EQ(referrers[0].Anchor, AssetReferenceAnchor::ProjectRelative);
    }

    // A minified .gltf is the whole document on one line, so not a single key
    // parses -- yet the file counted toward FilesScanned and the index called
    // itself complete. A move then skipped the rewrite and a delete could remove
    // a referenced texture. The quoted-literal sweep is what reads it.
    TEST_F(AutomationAssetIndexTest, FindsAReferenceInAMinifiedGltf)
    {
        Write(m_Project / "Assets" / "Models" / "Mini" / "Mini.gltf",
              "{\"asset\":{\"version\":\"2.0\"},\"images\":[{\"uri\":\"Albedo.png\"}]}\n");
        const auto texture = m_Project / "Assets" / "Models" / "Mini" / "Albedo.png";
        Write(texture, "png-ish");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, std::filesystem::weakly_canonical(texture), 0);
        ASSERT_EQ(referrers.size(), 1u) << "a one-line glTF still names its textures";
        EXPECT_EQ(referrers[0].Key, "uri") << "the literal before the colon names it";
        EXPECT_EQ(referrers[0].RawValue, "Albedo.png");
        EXPECT_EQ(referrers[0].Anchor, AssetReferenceAnchor::SourceRelative);
    }

    // A script names an asset inside a call, which is not a key/value line. These
    // formats were in the scanned set while contributing nothing, so the index
    // reported itself complete over files it had never really read.
    TEST_F(AutomationAssetIndexTest, FindsAReferenceInsideAScriptCall)
    {
        Write(m_Project / "Assets" / "Scripts" / "Boot.lua",
              "local tex = AssetManager.Load(\"Assets/Textures/Checkerboard.png\")\n");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, ProjectTexture(), 0);
        ASSERT_EQ(referrers.size(), 1u) << "a quoted path in a script is a reference";
        EXPECT_EQ(referrers[0].Key, "(string)");
        EXPECT_EQ(referrers[0].Anchor, AssetReferenceAnchor::ProjectRelative);
    }

    // SourceRelative exists because Assimp resolves a glTF URI that way. Nothing
    // else in the engine does, so enabling it everywhere would let this index
    // claim a sibling file resolves when EditorAssetManager::ImportAsset would
    // never find it -- a refused delete that should have gone ahead.
    TEST_F(AutomationAssetIndexTest, SourceRelativeIsGltfOnly)
    {
        Write(m_Project / "Assets" / "Scenes" / "Sibling.olo",
              "Scene: Sibling\n  AlbedoPath: Local.png\n");
        Write(m_Project / "Assets" / "Scenes" / "Local.png", "sibling texture");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(
            index, std::filesystem::weakly_canonical(m_Project / "Assets" / "Scenes" / "Local.png"), 0);
        EXPECT_TRUE(referrers.empty())
            << "a .olo does not resolve its paths relative to itself, and the index must not pretend it does";
    }

    // A file naming itself -- a scene title, a Lua header comment quoting its own
    // path -- is not a reference. Eleven of the sandbox project's scripts do the
    // latter, and the literal sweep collected every one until this was widened
    // past the bare-filename case it originally covered.
    TEST_F(AutomationAssetIndexTest, AFileQuotingItsOwnPathIsNotAReference)
    {
        const auto script = m_Project / "Assets" / "Scripts" / "Self.lua";
        Write(script, "-- Attach with ScriptFile = \"Scripts/Self.lua\"\n");

        const AssetIndex index = Build();
        EXPECT_TRUE(FindReferrers(index, std::filesystem::weakly_canonical(script), 0).empty());
        EXPECT_TRUE(FindDependencies(index, script).empty());
    }

    // --- the other graph direction ------------------------------------------

    TEST_F(AutomationAssetIndexTest, DependenciesAreTheReferencesInsideTheFile)
    {
        const auto scene = m_Project / "Assets" / "Scenes" / "Deps.olo";
        Write(scene, "Scene: Deps\nEntities:\n  - Entity: 1\n    DecalComponent:\n"
                     "      AlbedoTexturePath: Assets/Textures/Checkerboard.png\n"
                     "    MeshComponent:\n      MeshHandle: 777\n");

        const AssetIndex index = Build();
        const auto dependencies = FindDependencies(index, scene);
        ASSERT_EQ(dependencies.size(), 2u);
        EXPECT_EQ(dependencies[0].ResolvedFile, ProjectTexture());
        EXPECT_EQ(dependencies[1].HandleValue, 777u);
    }

    // --- rewriting ----------------------------------------------------------

    TEST_F(AutomationAssetIndexTest, RespellKeepsTheStyleTheFileAlreadyUsed)
    {
        Write(m_Project / "Assets" / "Scenes" / "Project.olo",
              "Scene: P\n  AlbedoTexturePath: Assets/Textures/Checkerboard.png\n");
        Write(m_Project / "Assets" / "Scenes" / "Legacy.olo",
              "Scene: L\n  MeshPath: SandboxProject/Assets/Textures/Checkerboard.png\n");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, ProjectTexture(), 0);
        ASSERT_EQ(referrers.size(), 2u);
        const auto moved = m_Project / "Assets" / "Textures" / "Moved" / "Checkerboard.png";

        for (const AssetReference& reference : referrers)
        {
            const std::string respelled = RespellReference(reference, ProjectTexture(), moved, m_Scope);
            if (reference.Anchor == AssetReferenceAnchor::ProjectRelative)
            {
                EXPECT_EQ(respelled, "Assets/Textures/Moved/Checkerboard.png");
            }
            else
            {
                // The stale prefix is put back rather than modernised: a move is
                // not the place to smuggle in an unrelated re-spelling.
                EXPECT_EQ(reference.Anchor, AssetReferenceAnchor::LegacyProjectPrefixed);
                EXPECT_EQ(respelled, "SandboxProject/Assets/Textures/Moved/Checkerboard.png");
            }
        }
    }

    // Found by moving a real asset in the live editor: TerrainVirtualTextureTest.olo
    // spells its texture "assets/textures/Checkerboard.png", which resolves on a
    // case-insensitive filesystem against a directory actually named
    // "Assets/Textures". Re-spelling from the filesystem alone rewrote the case,
    // so the move edited a line nobody asked it to change and moving back did not
    // restore it -- an unrelated re-spelling smuggled into somebody's diff, which
    // is precisely what this command set is careful not to do everywhere else.
    //
    // The reference is CONSTRUCTED rather than scanned, because a scan can only
    // produce a case-mismatched-but-resolved reference on a case-insensitive
    // filesystem: on Linux "assets/textures/..." simply does not resolve, so the
    // end-to-end version of this case passes on Windows and fails on Linux CI --
    // which is exactly what it did. What is under test is RespellReference, and
    // that is testable directly on either platform.
    TEST_F(AutomationAssetIndexTest, RespellPreservesTheCasingOfTheUnchangedPrefix)
    {
        AssetReference reference;
        reference.SourceFile = m_Project / "Assets" / "Scenes" / "LowerCase.olo";
        reference.Line = 2;
        reference.Key = "AlbedoPath";
        reference.RawValue = "assets/textures/Checkerboard.png";
        reference.Kind = AssetReferenceKind::Path;
        reference.ResolvedFile = ProjectTexture();
        reference.Anchor = AssetReferenceAnchor::ProjectRelative;

        const auto moved = m_Project / "Assets" / "Textures" / "Moved" / "Checkerboard.png";
        const std::string forward = RespellReference(reference, ProjectTexture(), moved, m_Scope);
        EXPECT_EQ(forward, "assets/textures/Moved/Checkerboard.png")
            << "only the part that changed may change; the casing the file used is not this move's business";

        // ...and the round trip is a no-op, which is the property that actually
        // matters: move it back and the file is byte-identical again.
        AssetReference movedReference = reference;
        movedReference.RawValue = forward;
        movedReference.ResolvedFile = std::filesystem::weakly_canonical(moved);
        EXPECT_EQ(RespellReference(movedReference, moved, ProjectTexture(), m_Scope),
                  "assets/textures/Checkerboard.png");
    }

    TEST_F(AutomationAssetIndexTest, AHandleReferenceNeedsNoRewrite)
    {
        Write(m_Project / "Assets" / "Meshes" / "M.olosmesh", "StaticMesh:\n  MeshSource: 555\n");
        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, {}, 555);
        ASSERT_EQ(referrers.size(), 1u);
        EXPECT_TRUE(RespellReference(referrers[0], {}, m_Project / "x.obj", m_Scope).empty())
            << "the handle IS the identity and survives a move; returning a path here would write a path into a "
               "handle field";
    }

    TEST_F(AutomationAssetIndexTest, ApplyEditReplacesOnlyTheAddressedSpan)
    {
        const auto scene = m_Project / "Assets" / "Scenes" / "Twice.olo";
        // The same text appears twice. A search-and-replace would edit both.
        Write(scene, "Scene: Twice\n  Note: Assets/Textures/Checkerboard.png\n"
                     "  AlbedoTexturePath: Assets/Textures/Checkerboard.png\n");

        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, ProjectTexture(), 0);
        ASSERT_EQ(referrers.size(), 2u);
        const auto target = std::ranges::find_if(referrers, [](const AssetReference& reference)
                                                 { return reference.Key == "AlbedoTexturePath"; });
        ASSERT_NE(target, referrers.end());

        std::string text = Read(scene);
        ASSERT_TRUE(ApplyReferenceEdit(text, *target, "Assets/Textures/New.png"));
        EXPECT_NE(text.find("  Note: Assets/Textures/Checkerboard.png\n"), std::string::npos)
            << "the untargeted line must be untouched";
        EXPECT_NE(text.find("  AlbedoTexturePath: Assets/Textures/New.png\n"), std::string::npos);
    }

    TEST_F(AutomationAssetIndexTest, ApplyEditRefusesWhenTheFileMovedUnderTheIndex)
    {
        const auto scene = m_Project / "Assets" / "Scenes" / "Shift.olo";
        Write(scene, "Scene: Shift\n  AlbedoTexturePath: Assets/Textures/Checkerboard.png\n");
        const AssetIndex index = Build();
        const auto referrers = FindReferrers(index, ProjectTexture(), 0);
        ASSERT_EQ(referrers.size(), 1u);

        // Somebody inserted a line. The recorded span now points at other text.
        std::string text = "Scene: Shift\n# inserted\n  AlbedoTexturePath: Assets/Textures/Checkerboard.png\n";
        EXPECT_FALSE(ApplyReferenceEdit(text, referrers[0], "Assets/Textures/New.png"))
            << "the honest answer is refusal; falling back to a textual replace here is exactly how a rewrite "
               "corrupts an unrelated line";
    }
} // namespace OloEngine::Automation::Tests
