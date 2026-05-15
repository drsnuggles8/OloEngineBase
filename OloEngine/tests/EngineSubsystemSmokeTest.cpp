// =============================================================================
// EngineSubsystemSmokeTest.cpp
//
// Smoke tests for engine subsystem init/lifecycle paths that OloEditor
// drives during startup. Catches regressions where a subsystem's Init
// crashes on a particular project shape ("OloEditor doesn't start when
// I open my project"), gets confused by malformed inputs ("editor hard-
// crashes on a corrupt .oloproj instead of surfacing an error"), or
// breaks across multiple Init→use→swap cycles ("opening a second
// project after closing the first leaves the asset manager in a bad
// state").
//
// Subsystems covered here are deliberately the ones with the cheapest
// init paths (no GL context, no full Renderer init, no Application
// instance required). Heavier subsystems (Renderer3D, full Scene
// runtime) are smoke-tested by `RendererAttachedTest` and the
// `Functional::FunctionalTest` family respectively.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Project/Project.h"

#include <filesystem>
#include <fstream>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        fs::path SandboxProjectFile()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" / "Sandbox.oloproj";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Project::Load — the real Sandbox project
    //
    // The on-disk Sandbox project file is the same one OloEditor loads on
    // every developer's machine when they open the editor. If Project::Load
    // chokes on it, every editor session breaks. This test exercises the
    // exact production path.
    // -------------------------------------------------------------------------
    TEST(EngineSubsystemSmoke, ProjectLoadSandboxProjectSucceeds)
    {
        const fs::path projectFile = SandboxProjectFile();
        ASSERT_TRUE(fs::exists(projectFile))
            << "Sandbox.oloproj not found at " << projectFile.string()
            << " — OLO_TEST_EDITOR_ROOT points at the wrong location?";

        Ref<Project> project = Project::Load(projectFile);
        ASSERT_TRUE(project) << "Project::Load returned null for the real Sandbox project.";

        const auto& cfg = project->GetConfig();
        EXPECT_FALSE(cfg.Name.empty())
            << "Loaded project has empty Name — ProjectSerializer didn't populate the config.";
        EXPECT_FALSE(cfg.AssetDirectory.empty())
            << "Loaded project has empty AssetDirectory — assets won't resolve.";

        // The static Project::s_ActiveProject should now point at this
        // load. Functional tests that run later will overwrite it with
        // their own temp project, so we don't try to reset it here.
        EXPECT_EQ(Project::GetActive(), project);
    }

    // -------------------------------------------------------------------------
    // Project::Load — missing file
    //
    // A wrong path passed by a user (or a deleted recent-projects entry)
    // must surface as a null Ref<Project>, not a hard crash that takes
    // down the editor.
    // -------------------------------------------------------------------------
    TEST(EngineSubsystemSmoke, ProjectLoadMissingPathFailsCleanly)
    {
        const fs::path missing = fs::temp_directory_path() /
                                 "OloEngineSubsystemSmoke_does_not_exist.oloproj";
        std::error_code ec;
        fs::remove(missing, ec); // Ensure clean state — ignore "not found" errors.

        Ref<Project> project = Project::Load(missing);
        EXPECT_FALSE(project)
            << "Project::Load returned non-null for a missing path; expected null.";
    }

    // -------------------------------------------------------------------------
    // Project::Load — malformed YAML
    //
    // A truncated or corrupted .oloproj (e.g. a merge conflict left
    // unresolved markers in the file) must fail to load gracefully.
    // Hard-crashing here means the user can't open the editor at all
    // and has no actionable error.
    // -------------------------------------------------------------------------
    TEST(EngineSubsystemSmoke, ProjectLoadMalformedYAMLFailsCleanly)
    {
        const fs::path garbage = fs::temp_directory_path() /
                                 "OloEngineSubsystemSmoke_garbage.oloproj";
        {
            std::ofstream out(garbage);
            // Deliberately broken — unterminated mapping, conflicting
            // indentation. A real YAML loader will reject this.
            out << "Project:\n"
                << "  Name: \"unclosed string\n"
                << "\t  AssetDirectory: Assets\n"
                << "        : invalid\n";
        }

        Ref<Project> project = Project::Load(garbage);
        std::error_code ec;
        fs::remove(garbage, ec); // Best-effort cleanup.

        EXPECT_FALSE(project)
            << "Project::Load returned non-null for a malformed .oloproj; expected null.";
    }

    // -------------------------------------------------------------------------
    // Project::SaveActive → Project::Load round-trip
    //
    // Catches asymmetry between the serializer and deserializer: a field
    // added on one side but not the other will not survive the round-trip,
    // surfacing in OloEditor as "I set X in project settings and saved,
    // but it's reset after reload."
    //
    // Note on path resolution: `AssetDirectory` is *deliberately* written
    // as a relative path and resolved to absolute on load (via
    // `weakly_canonical` against the project file's parent dir). We
    // therefore assert that the loaded value's filename matches what we
    // saved, not the full path.
    // -------------------------------------------------------------------------
    TEST(EngineSubsystemSmoke, ProjectSaveActiveLoadRoundTrip)
    {
        const fs::path tempDir = fs::temp_directory_path() / "OloEngineSubsystemSmoke_roundtrip";
        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = tempDir / "RoundTrip.oloproj";

        // 1. Build a Project in-memory with a recognisable Name and
        //    AssetDirectory. Project::New makes it the s_ActiveProject;
        //    SaveActive then serialises it to disk.
        {
            Ref<Project> created = Project::New();
            auto& cfg = created->GetConfig();
            cfg.Name = "RoundTripProject";
            cfg.AssetDirectory = "Assets";
            ASSERT_TRUE(Project::SaveActive(projectFile))
                << "Project::SaveActive failed for " << projectFile.string();
        }

        // 2. Load it back. The freshly-loaded project must match the
        //    config we wrote.
        Ref<Project> reloaded = Project::Load(projectFile);
        ASSERT_TRUE(reloaded) << "Project::Load failed on the just-saved file.";

        const auto& cfg = reloaded->GetConfig();
        EXPECT_EQ(cfg.Name, "RoundTripProject")
            << "Project.Name did not survive save/load round-trip.";
        // AssetDirectory is resolved relative→absolute on load. The
        // filename component is the invariant we control.
        EXPECT_EQ(cfg.AssetDirectory.filename().generic_string(), "Assets")
            << "Project.AssetDirectory filename did not survive save/load round-trip.";

        // Best-effort cleanup.
        fs::remove_all(tempDir, ec);
    }

    // -------------------------------------------------------------------------
    // Project::SaveActive → Project::Load round-trip — extended fields
    //
    // Covers the remaining user-visible ProjectConfig fields beyond the
    // basic Name/AssetDirectory check above. Each field exercised is one
    // a developer can set in OloEditor's project-settings panel; an
    // asymmetric serializer (write side updated but not read, or vice
    // versa) would surface as "I changed X in the settings panel,
    // hit save, and after reload it's reset."
    //
    // For path fields (StartScene, ScriptModulePath), we have to create
    // a placeholder file under the temp project's Assets/ directory
    // because the deserializer's `safeGetAssetPath` resolves the field
    // to absolute and warns when the resolved path doesn't exist.
    // -------------------------------------------------------------------------
    TEST(EngineSubsystemSmoke, ProjectSaveActiveLoadExtendedFieldsRoundTrip)
    {
        const fs::path tempDir = fs::temp_directory_path() /
                                 "OloEngineSubsystemSmoke_roundtrip_extended";
        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = tempDir / "Extended.oloproj";

        // Touch a fake start scene + script module so the on-load
        // path-resolution doesn't complain about missing files.
        {
            std::ofstream(tempDir / "Assets" / "Main.olo") << "Scene: Main\n";
            std::ofstream(tempDir / "Assets" / "Scripting.dll") << "";
        }

        // Distinctive non-default values so the round-trip catches
        // either-side asymmetry.
        constexpr bool expectedEnableAutoSave = false; // default true → false
        constexpr int expectedAutoSaveInterval = 73;   // default 300 → 73
        const std::string expectedName = "ExtendedRoundTrip";

        {
            Ref<Project> created = Project::New();
            auto& cfg = created->GetConfig();
            cfg.Name = expectedName;
            cfg.AssetDirectory = "Assets";
            cfg.StartScene = "Main.olo";
            cfg.ScriptModulePath = "Scripting.dll";
            cfg.EnableAutoSave = expectedEnableAutoSave;
            cfg.AutoSaveIntervalSeconds = expectedAutoSaveInterval;
            ASSERT_TRUE(Project::SaveActive(projectFile));
        }

        Ref<Project> reloaded = Project::Load(projectFile);
        ASSERT_TRUE(reloaded);

        const auto& cfg = reloaded->GetConfig();
        EXPECT_EQ(cfg.Name, expectedName);
        EXPECT_EQ(cfg.EnableAutoSave, expectedEnableAutoSave)
            << "Project.EnableAutoSave did not survive save/load round-trip.";
        EXPECT_EQ(cfg.AutoSaveIntervalSeconds, expectedAutoSaveInterval)
            << "Project.AutoSaveIntervalSeconds did not survive save/load round-trip.";

        // StartScene and ScriptModulePath both resolve relative→absolute
        // on load. The filename is the invariant we can control.
        EXPECT_EQ(cfg.StartScene.filename().generic_string(), "Main.olo")
            << "Project.StartScene filename did not survive save/load round-trip.";
        EXPECT_EQ(cfg.ScriptModulePath.filename().generic_string(), "Scripting.dll")
            << "Project.ScriptModulePath filename did not survive save/load round-trip.";

        fs::remove_all(tempDir, ec);
    }

    // -------------------------------------------------------------------------
    // SandboxProjectStartSceneResolves
    //
    // The `.oloproj` declares a `StartScene` — the scene OloEditor opens
    // automatically when the user loads the project. A stale `StartScene`
    // value (pointing at a scene that's been renamed or deleted) means
    // the editor either opens to an empty scene or surfaces a "file not
    // found" error on every project open.
    //
    // Empty StartScene is allowed (no default chosen); a non-empty value
    // must resolve to an existing file.
    // -------------------------------------------------------------------------
    TEST(EngineSubsystemSmoke, SandboxProjectStartSceneResolves)
    {
        Ref<Project> project = Project::Load(SandboxProjectFile());
        ASSERT_TRUE(project);

        const auto& cfg = project->GetConfig();
        if (cfg.StartScene.empty())
            return; // optional field — no value, no check.

        // Project::Load resolves StartScene to an absolute path via
        // weakly_canonical against the asset directory. We just verify
        // the resolved path exists.
        std::error_code ec;
        EXPECT_TRUE(fs::exists(cfg.StartScene, ec))
            << "Project StartScene = '" << cfg.StartScene.generic_string()
            << "' but no file exists at that path. OloEditor will fail to "
               "auto-load the start scene on project open.";
    }
} // namespace OloEngine::Tests
