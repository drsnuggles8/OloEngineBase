#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// RuntimeProjectMountTest — unit test (headless, no GL, no physics).
//
// Pins the shipped-game project mount (`Project::NewInMemory`) and the path
// contract it exists to satisfy.
//
// A built game has no `.oloproj` — `GameBuildPipeline` flattens the project into
// `game.manifest` plus an asset pack — so `OloRuntime` historically ran with NO
// active project at all. But the engine resolves several asset-relative paths
// through the `Project` statics, every one of which asserts on a null
// `s_ActiveProject`. The one that bit hardest:
// `Scene::OnRuntimeStart` resolves each `LuaScriptComponent::ScriptFile` through
// `Project::GetAssetFileSystemPath`, so a shipped game died the instant it
// loaded a scene carrying a Lua script — i.e. Lua scripting was editor-only,
// which quietly made the Lua half of runtime scene switching (issue #642)
// unusable in the very builds it was for.
//
// The fix is two halves that must agree on ONE layout, and a mismatch between
// them is silent (the script simply never loads, no crash, no error):
//
//   * `GameBuildPipeline::CopyScriptFiles` writes each .lua to
//     `<game>/Assets/<asset-relative path>`,
//   * `RuntimeLayer::MountGameProject` mounts a project rooted at `<game>` with
//     `AssetDirectory = "Assets"`.
//
// So `GetAssetFileSystemPath(<asset-relative path>)` must land exactly on the
// copied file. That round trip is what this file tests, against a staged
// directory laid out the way the build pipeline lays one out.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Project/Project.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace OloEngine;

class RuntimeProjectMountTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Whatever project an earlier test left mounted. Restored in TearDown so
        // this test can't strand the suite on a directory it then deletes.
        m_PreviousProject = Project::GetActive();

        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::error_code ec;
        m_GameDir = std::filesystem::temp_directory_path() / "OloEngineRuntimeProject" /
                    std::string(info ? info->name() : "Unknown");
        std::filesystem::remove_all(m_GameDir, ec);
        std::filesystem::create_directories(m_GameDir, ec);
        ASSERT_FALSE(ec) << "could not stage the fake game directory: " << ec.message();
    }

    void TearDown() override
    {
        if (m_PreviousProject)
        {
            Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
        }
        std::error_code ec;
        std::filesystem::remove_all(m_GameDir, ec);
    }

    /// Lay a file down the way `GameBuildPipeline::CopyScriptFiles` does:
    /// `<game>/Assets/<asset-relative path>`.
    std::filesystem::path StageShippedAsset(const std::filesystem::path& assetRelative) const
    {
        const auto full = m_GameDir / "Assets" / assetRelative;
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        std::ofstream(full) << "-- staged\n";
        return full;
    }

    /// The config `RuntimeLayer::MountGameProject` builds.
    [[nodiscard]] static ProjectConfig ShippedGameConfig()
    {
        ProjectConfig config;
        config.Name = "StagedGame";
        config.AssetDirectory = "Assets";
        return config;
    }

    Ref<Project> m_PreviousProject;
    std::filesystem::path m_GameDir;
};

TEST_F(RuntimeProjectMountTest, MountingInMemoryMakesTheProjectStaticsUsable)
{
    // The whole point: before the mount these statics assert, so a shipped game
    // could not resolve a single asset-relative path.
    Ref<Project> project = Project::NewInMemory(m_GameDir, ShippedGameConfig());

    ASSERT_TRUE(static_cast<bool>(project));
    EXPECT_EQ(Project::GetActive(), project) << "NewInMemory did not make the project active.";
    EXPECT_EQ(Project::GetProjectDirectory(), m_GameDir);
    EXPECT_EQ(Project::GetAssetDirectory(), m_GameDir / "Assets");
    EXPECT_EQ(project->GetConfig().Name, "StagedGame")
        << "the supplied config was not adopted.";
}

TEST_F(RuntimeProjectMountTest, AProjectRelativeLuaScriptResolvesToTheShippedFile)
{
    // This is the contract that was broken. The path stored in the scene is
    // project-relative; the file the build pipeline shipped is under Assets/.
    const std::filesystem::path scriptRelative = "Scripts/LuaScripts/Demo.lua";
    const auto shipped = StageShippedAsset(scriptRelative);
    ASSERT_TRUE(std::filesystem::exists(shipped));

    Project::NewInMemory(m_GameDir, ShippedGameConfig());

    const auto resolved = Project::GetAssetFileSystemPath(scriptRelative);
    EXPECT_TRUE(std::filesystem::exists(resolved))
        << "a project-relative Lua ScriptFile did not resolve to the file the build pipeline ships at "
        << shipped.string() << " — it resolved to " << resolved.string()
        << ". LuaScriptEngine loads scripts with a plain filesystem read, so this mismatch means every "
           "Lua script in a shipped game silently fails to load.";
    EXPECT_TRUE(std::filesystem::equivalent(resolved, shipped));
}

TEST_F(RuntimeProjectMountTest, TheAssetRelativeRoundTripIsStable)
{
    // The editor stores the relative form; the runtime expands it. If these two
    // ever disagree, scripts authored in the editor stop resolving once shipped.
    const std::filesystem::path scriptRelative = "Scripts/LuaScripts/Nested/Deep.lua";
    (void)StageShippedAsset(scriptRelative);

    Project::NewInMemory(m_GameDir, ShippedGameConfig());

    const auto absolute = Project::GetAssetFileSystemPath(scriptRelative);
    const auto backToRelative = Project::GetAssetRelativeFileSystemPath(absolute);
    EXPECT_EQ(backToRelative.generic_string(), scriptRelative.generic_string())
        << "asset-relative <-> absolute is not a round trip.";
}

TEST_F(RuntimeProjectMountTest, MountingDoesNotRequireTheDirectoriesToExist)
{
    // MountGameProject runs before anything is loaded, and a game directory
    // missing an Assets/ folder (no packed assets, no scripts) is legal. The
    // mount must still succeed rather than assert — the statics are pure path
    // math, and callers check existence themselves.
    const auto missing = m_GameDir / "does-not-exist";
    Project::NewInMemory(missing, ShippedGameConfig());

    EXPECT_EQ(Project::GetProjectDirectory(), missing);
    EXPECT_EQ(Project::GetAssetFileSystemPath("Scripts/Nothing.lua"),
              missing / "Assets" / "Scripts/Nothing.lua");
}
