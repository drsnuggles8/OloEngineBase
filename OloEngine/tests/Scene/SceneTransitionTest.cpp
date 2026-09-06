#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// SceneTransitionTest — unit test (headless, no GL, no physics).
//
// Pins the host-side half of runtime scene switching (issue #642):
// SceneTransition::ResolveScenePath and SceneTransition::LoadSceneFile, shared
// by OloRuntime's RuntimeLayer and the editor's Play mode so a scene name means
// the same thing in a shipped game as it does in the editor.
//
// Why these two are worth pinning on their own, away from the two-scene
// Functional transition test:
//
//   1. Resolution is the ONLY thing standing between a script's `LoadScene
//      ("Level2")` and a file on disk. It is pure string/filesystem work with
//      several fallbacks, and a regression in any one of them shows up as
//      "the level button does nothing" at runtime with no other symptom.
//   2. LoadSceneFile's contract is specifically that it FAILS WITHOUT SIDE
//      EFFECTS — it deserializes and validates into a fresh Scene so a bad
//      target leaves the running game on its current scene. A version that
//      returned a half-built scene, or that validated after handing the scene
//      over, would still pass a happy-path switch test.
//   3. Both hosts refuse a scene with no primary camera, because the runtime
//      has no editor camera to fall back on. That refusal is a deliberate
//      "stay where we are" rather than a black screen, so it is a contract,
//      not an implementation detail.
// =============================================================================

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Project/Project.h"
#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/SceneTransition.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace OloEngine;

namespace
{
    // Author a minimal, valid scene file at `path`. `withCamera` controls the
    // one thing LoadSceneFile validates beyond "it parses".
    void WriteSceneFile(const std::filesystem::path& path, const std::string& markerName, bool withCamera)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        Ref<Scene> scene = Scene::Create();
        scene->SetName(path.stem().string());
        (void)scene->CreateEntity(markerName);
        if (withCamera)
        {
            Entity camera = scene->CreateEntity("Camera");
            camera.AddComponent<CameraComponent>().Primary = true;
        }
        SceneSerializer(scene).Serialize(path);
    }
} // namespace

class SceneTransitionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_PreviousProject = Project::GetActive();
        std::error_code ec;
        m_Root = OloEngine::Tests::TempDir("game");
        std::filesystem::create_directories(m_Root / "Scenes", ec);
        ASSERT_FALSE(ec) << "could not create the temp game directory: " << ec.message();

        ProjectConfig config;
        config.Name = "SceneTransitionTest";
        config.AssetDirectory = "Assets";
        ASSERT_TRUE(Project::NewInMemory(m_Root, config));
    }

    void TearDown() override
    {
        if (m_PreviousProject)
        {
            Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
        }
        else
        {
            Project::Unload();
        }
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    Ref<Project> m_PreviousProject;
    std::filesystem::path m_Root;
};

// -----------------------------------------------------------------------------
// Resolution: every spelling a script might plausibly use finds the same file.
// -----------------------------------------------------------------------------
TEST_F(SceneTransitionTest, EverySpellingOfASceneNameResolvesToTheSameFile)
{
    const auto scenePath = m_Root / "Scenes" / "Level2.olo";
    WriteSceneFile(scenePath, "Level2Marker", /*withCamera=*/true);

    // A bare name, a file name, and a path relative to the game root are all
    // things a designer will type into a script. All three must land here.
    for (const char* request : { "Level2", "Level2.olo", "Scenes/Level2.olo" })
    {
        const auto resolved = SceneTransition::ResolveScenePath(request, m_Root);
        ASSERT_FALSE(resolved.empty()) << "request '" << request << "' resolved to nothing.";
        EXPECT_TRUE(std::filesystem::equivalent(resolved, scenePath))
            << "request '" << request << "' resolved to " << resolved.string()
            << " instead of " << scenePath.string();
    }
}

TEST_F(SceneTransitionTest, ASceneInASubdirectoryIsFoundByBareName)
{
    // Scenes are copied into a shipped game preserving their authored folder
    // structure, so a name-only request has to search, not just probe
    // Scenes/<name>.olo.
    const auto scenePath = m_Root / "Scenes" / "Chapter1" / "Boss.olo";
    WriteSceneFile(scenePath, "BossMarker", /*withCamera=*/true);

    const auto resolved = SceneTransition::ResolveScenePath("Boss", m_Root);
    ASSERT_FALSE(resolved.empty()) << "a scene nested under Scenes/ was not found by bare name.";
    EXPECT_TRUE(std::filesystem::equivalent(resolved, scenePath));
}

TEST_F(SceneTransitionTest, ADirectHitOutranksTheRecursiveSearch)
{
    // Two scenes share a file name. The one sitting exactly where the request
    // points must win, or a request can silently resolve to the wrong level.
    const auto direct = m_Root / "Scenes" / "Arena.olo";
    const auto nested = m_Root / "Scenes" / "Bonus" / "Arena.olo";
    WriteSceneFile(direct, "DirectMarker", /*withCamera=*/true);
    WriteSceneFile(nested, "NestedMarker", /*withCamera=*/true);

    const auto resolved = SceneTransition::ResolveScenePath("Scenes/Arena.olo", m_Root);
    ASSERT_FALSE(resolved.empty());
    EXPECT_TRUE(std::filesystem::equivalent(resolved, direct))
        << "an exact path was outranked by the recursive fallback.";
}

TEST_F(SceneTransitionTest, AnUnknownSceneResolvesToNothingRatherThanGuessing)
{
    WriteSceneFile(m_Root / "Scenes" / "Level2.olo", "Level2Marker", /*withCamera=*/true);

    // Returning *some* scene for a typo'd name would be far worse than
    // returning none: the host reports the failure and stays put.
    EXPECT_TRUE(SceneTransition::ResolveScenePath("Levle2", m_Root).empty());
    EXPECT_TRUE(SceneTransition::ResolveScenePath("", m_Root).empty());
}

TEST_F(SceneTransitionTest, ANonSceneFileIsNotResolvable)
{
    const auto notAScene = m_Root / "Scenes" / "readme.txt";
    std::ofstream(notAScene) << "not a scene";

    EXPECT_TRUE(SceneTransition::ResolveScenePath("readme.txt", m_Root).empty())
        << "a non-scene file was accepted as a scene target.";
}

TEST_F(SceneTransitionTest, AParentDirectoryEscapeIsRejected)
{
    // Scene requests come from script source; there is no legitimate reason for
    // one to climb out of the game's data directory, and allowing it would let
    // a scene name reach arbitrary files on the player's disk.
    const auto outside = m_Root.parent_path() / "Outside.olo";
    WriteSceneFile(outside, "OutsideMarker", /*withCamera=*/true);

    EXPECT_TRUE(SceneTransition::ResolveScenePath("../Outside.olo", m_Root).empty())
        << "a '..' component was allowed to escape the game directory.";
    EXPECT_TRUE(SceneTransition::ResolveScenePath("Scenes/../../Outside.olo", m_Root).empty());

    std::error_code ec;
    std::filesystem::remove(outside, ec);
}

TEST_F(SceneTransitionTest, AnAbsoluteRequestIsRejected)
{
    // An absolute request escapes the root WITHOUT any '..', because appending
    // an absolute path replaces the left operand ([fs.path.append]) — so
    // `root / "C:/anywhere/x.olo"` is just `C:/anywhere/x.olo`. If absolute
    // requests were honoured, the '..' rejection above would be decorative.
    const auto scenePath = m_Root / "Scenes" / "Level2.olo";
    WriteSceneFile(scenePath, "Level2Marker", /*withCamera=*/true);
    ASSERT_TRUE(scenePath.is_absolute()) << "the fixture's staging root is not absolute; test is void.";

    EXPECT_TRUE(SceneTransition::ResolveScenePath(scenePath.string(), m_Root).empty())
        << "an absolute path resolved, so a script could name any scene file on the player's disk "
           "and sidestep the root entirely.";

    // The same scene is still reachable the legitimate way.
    EXPECT_FALSE(SceneTransition::ResolveScenePath("Level2", m_Root).empty())
        << "rejecting absolute requests also broke ordinary relative resolution.";
}

// -----------------------------------------------------------------------------
// Loading: succeed with a live scene, or fail leaving the caller nothing.
// -----------------------------------------------------------------------------
TEST_F(SceneTransitionTest, LoadingAValidSceneReturnsAStartableScene)
{
    const auto scenePath = m_Root / "Scenes" / "Level2.olo";
    WriteSceneFile(scenePath, "Level2Marker", /*withCamera=*/true);

    auto result = SceneTransition::LoadSceneFile(scenePath, /*requirePrimaryCamera=*/true);
    ASSERT_TRUE(static_cast<bool>(result)) << "load failed: " << result.Error;
    EXPECT_TRUE(result.Error.empty());

    // The scene is populated but NOT started — the caller still owns the
    // ordering of OnRuntimeStop on the outgoing scene vs OnRuntimeStart here.
    EXPECT_TRUE(static_cast<bool>(result.LoadedScene->FindEntityByName("Level2Marker")))
        << "the loaded scene is missing its entities.";
    EXPECT_FALSE(result.LoadedScene->IsRunning())
        << "LoadSceneFile started the scene — the caller must control that ordering, because the "
           "outgoing scene has to release the process-wide script context first.";
}

TEST_F(SceneTransitionTest, ContinueRestoresBeforeReturningAStartableScene)
{
    const auto scenePath = m_Root / "Scenes" / "Drift.olo";
    WriteSceneFile(scenePath, "AuthoredMarker", /*withCamera=*/true);

    auto savedScene = Scene::Create();
    savedScene->SetName("Drift");
    (void)savedScene->CreateEntity("RestoredMarker");
    Entity camera = savedScene->CreateEntity("Camera");
    camera.AddComponent<CameraComponent>().Primary = true;

    const auto payload = SaveGameSerializer::CaptureSceneState(*savedScene);
    ASSERT_FALSE(payload.empty());
    SaveGameHeader header;
    header.EntityCount = 2;
    SaveGameMetadata metadata;
    metadata.DisplayName = "Drift voyage";
    metadata.SceneName = "Drift";
    metadata.EntityCount = 2;
    std::filesystem::create_directories(SaveGameManager::GetSaveDirectory());
    ASSERT_TRUE(SaveGameFile::Write(SaveGameManager::GetSaveFilePath("drift_voyage"),
                                    header, metadata, {}, payload));

    auto loaded = SceneTransition::LoadSceneFile(scenePath, /*requirePrimaryCamera=*/true, "drift_voyage");
    ASSERT_TRUE(static_cast<bool>(loaded)) << loaded.Error;
    EXPECT_TRUE(static_cast<bool>(loaded.LoadedScene->FindEntityByName("RestoredMarker")));
    EXPECT_FALSE(static_cast<bool>(loaded.LoadedScene->FindEntityByName("AuthoredMarker")));
    EXPECT_FALSE(loaded.LoadedScene->IsRunning())
        << "the shared transition helper must restore before either host starts the scene.";

    auto missing = SceneTransition::LoadSceneFile(scenePath, /*requirePrimaryCamera=*/true, "missing_slot");
    EXPECT_FALSE(static_cast<bool>(missing))
        << "a missing save must discard the incoming scene so the host keeps the outgoing scene alive.";
    EXPECT_FALSE(missing.Error.empty());
}

TEST_F(SceneTransitionTest, ASceneWithNoPrimaryCameraIsRefused)
{
    const auto scenePath = m_Root / "Scenes" / "NoCamera.olo";
    WriteSceneFile(scenePath, "Marker", /*withCamera=*/false);

    auto refused = SceneTransition::LoadSceneFile(scenePath, /*requirePrimaryCamera=*/true);
    EXPECT_FALSE(static_cast<bool>(refused))
        << "a camera-less scene was accepted — the runtime has no editor camera to fall back on, so "
           "switching to it would drop the player into a black screen.";
    EXPECT_FALSE(refused.Error.empty()) << "a failed load must say why.";

    // The check is opt-in: the editor's own scene loading has an editor camera.
    auto accepted = SceneTransition::LoadSceneFile(scenePath, /*requirePrimaryCamera=*/false);
    EXPECT_TRUE(static_cast<bool>(accepted)) << accepted.Error;
}

TEST_F(SceneTransitionTest, AMissingOrMalformedTargetFailsWithoutASceneAndWithAReason)
{
    // Missing file.
    auto missing = SceneTransition::LoadSceneFile(m_Root / "Scenes" / "Ghost.olo", true);
    EXPECT_FALSE(static_cast<bool>(missing));
    EXPECT_FALSE(missing.Error.empty());

    // Wrong extension.
    const auto textFile = m_Root / "Scenes" / "notes.txt";
    std::ofstream(textFile) << "hello";
    auto wrongType = SceneTransition::LoadSceneFile(textFile, true);
    EXPECT_FALSE(static_cast<bool>(wrongType));

    // Right extension, garbage contents. This is the one that matters most:
    // the host tears the running scene down only AFTER this returns, so a
    // parse failure here has to be reported rather than half-applied.
    //
    // Unparseable bytes reach `YAML::LoadFile` inside
    // `SceneSerializer::Deserialize`, which throws and catches
    // `YAML::Exception` — a frame shape nothing else in the suite exercises
    // (every SceneSerializerFuzzRegression input PARSES cleanly and fails later
    // in the schema walk, so none of them throw). That made this the branch the
    // clang-cl ASan throw-dispatch bug crashed on (issue #661); the LLVM 23.1.0
    // pin in asan.yml fixes it and the guard that used to wrap this block is
    // gone. If it comes back as SEH 0xc0000005 with no ASan report, the question
    // is which clang the job resolved, not whether the engine regressed.
    const auto corrupt = m_Root / "Scenes" / "Corrupt.olo";
    std::ofstream(corrupt) << "\t\tthis: [is, not, a: scene\n";
    auto broken = SceneTransition::LoadSceneFile(corrupt, true);
    EXPECT_FALSE(static_cast<bool>(broken))
        << "a corrupt scene file was accepted; the host would have stopped the running scene for it.";
    EXPECT_FALSE(broken.Error.empty());
}

// -----------------------------------------------------------------------------
// The Scene-side request slot. Reload and load are one mechanism with two
// targets, so they must not both fire from a single tick.
// -----------------------------------------------------------------------------
TEST_F(SceneTransitionTest, AFreshSceneHasNoPendingTransition)
{
    auto scene = Scene::Create();
    EXPECT_FALSE(scene->HasPendingSceneLoad());
    EXPECT_FALSE(scene->GetPendingReload());
}

TEST_F(SceneTransitionTest, TheLastTransitionRequestOfATickIsTheOneThatHappens)
{
    Ref<Scene> scene = Scene::Create();

    scene->SetPendingReload(true);
    scene->SetPendingSceneLoad("Level2");
    EXPECT_TRUE(scene->HasPendingSceneLoad());
    EXPECT_FALSE(scene->GetPendingReload())
        << "a load request left the reload flag set — the host would switch scenes and then "
           "immediately reload the scene it just switched to.";
    EXPECT_EQ(scene->GetPendingSceneLoad(), "Level2");

    scene->SetPendingReload(true);
    EXPECT_FALSE(scene->HasPendingSceneLoad())
        << "a reload request left a stale load target behind.";
    EXPECT_TRUE(scene->GetPendingReload());

    // Clearing the reload flag (what a host does when it services one) must not
    // disturb an unrelated load target.
    scene->SetPendingSceneLoad("Level3");
    scene->SetPendingReload(false);
    EXPECT_EQ(scene->GetPendingSceneLoad(), "Level3");
}

TEST_F(SceneTransitionTest, ClearingAPendingLoadLeavesNoRequestBehind)
{
    Ref<Scene> scene = Scene::Create();
    scene->SetPendingSceneLoad("Level2");
    scene->ClearPendingSceneLoad();
    EXPECT_FALSE(scene->HasPendingSceneLoad());
    EXPECT_TRUE(scene->GetPendingSceneLoad().empty());
}

TEST_F(SceneTransitionTest, ContinueRequestKeepsSaveSlotPairedWithItsScene)
{
    Ref<Scene> scene = Scene::Create();
    scene->SetPendingSceneLoadFromSave("Drift", "drift_voyage");

    ASSERT_TRUE(scene->HasPendingSceneLoad());
    EXPECT_EQ(scene->GetPendingSceneLoad(), "Drift");
    EXPECT_EQ(scene->GetPendingSceneLoadSaveSlot(), "drift_voyage");

    // A later ordinary load must not accidentally restore the earlier save,
    // and clearing the request must clear both halves atomically.
    scene->SetPendingSceneLoad("Credits");
    EXPECT_TRUE(scene->GetPendingSceneLoadSaveSlot().empty());
    scene->SetPendingSceneLoadFromSave("Drift", "drift_voyage");
    scene->ClearPendingSceneLoad();
    EXPECT_FALSE(scene->HasPendingSceneLoad());
    EXPECT_TRUE(scene->GetPendingSceneLoadSaveSlot().empty());

    scene->SetPendingSceneLoadFromSave("Drift", "drift_voyage");
    scene->SetPendingReload(true);
    EXPECT_FALSE(scene->HasPendingSceneLoad());
    EXPECT_TRUE(scene->GetPendingSceneLoadSaveSlot().empty());
}
