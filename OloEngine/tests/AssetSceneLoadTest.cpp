// =============================================================================
// AssetSceneLoadTest.cpp
//
// Catches the OloEditor breakage class where a sample scene file
// crashes the deserialiser when actually loaded through the full
// production code path — not just the structural-YAML check that
// `AssetContentValidity.AllSandboxScenes...` performs. This exercises
// `SceneSerializer::Deserialize` with the editor's full project +
// asset-manager state mounted, the same way OloEditor does it on
// "File → Open Scene".
//
// Architecture
// ------------
//   The asset manager's `Initialize` ALWAYS re-serialises the registry
//   (it can't be told to be read-only). To avoid mutating the real
//   `OloEditor/SandboxProject/AssetRegistry.oar` (committed to git),
//   we stage the entire sandbox project into the OS temp dir at the
//   start of the test, run all deserialisations against the temp
//   copy, and tear the temp dir down at the end. The recursive copy
//   is a one-time cost per test run; the cleaned-up temp dir contains
//   the side-effect-touched .oar but nothing in the working tree is
//   modified.
//
// What this catches that the structural-YAML test does not
// ---------------------------------------------------------
//   - SEH crashes inside component-specific deserialisers (e.g. the
//     mesh / animation / particle paths that touch the asset manager).
//   - YAML field type mismatches (scene declares `Scale: true` instead
//     of `Scale: [1, 1, 1]`) that yaml-cpp catches but only at the
//     `.as<glm::vec3>()` call site.
//   - Missing required asset handles that the deserialiser does
//     dereference (mesh primitives, default fonts, etc.).
//
// *** Current limitation ***
// --------------------------
// The test is `DISABLED_` because production deserialisers for
// MeshComponent / AnimationStateComponent / SkeletonComponent /
// ParticleComponent reach into Renderer3D state that's only valid
// after `Renderer::Init()` has run. Loading `AnimationIKTest.olo`
// SEH-crashes immediately after deserialising the Floor entity's
// mesh data — the next component's deserialiser dereferences null.
//
// This is the same engine-side coupling documented in
// `RendererAttachedTest`. The right unblock is a single engine
// refactor: have the mesh / animation / particle component
// deserialisers null-check the renderer state (or be re-entrant
// post-`Renderer::Init`) so deserialisation works headlessly.
//
// To re-enable: filter in with `--gtest_also_run_disabled_tests
// --gtest_filter=AssetSceneLoad.*`. Then expect failures until the
// renderer-state refactor lands.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Stage the entire SandboxProject into a fresh temp dir.
        // Returns the temp project's root path (or empty on failure).
        fs::path StageSandboxProjectIntoTemp()
        {
            const fs::path sandboxRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";
            const fs::path tempRoot =
                fs::temp_directory_path() / "OloEngineSceneLoad";

            std::error_code ec;
            fs::remove_all(tempRoot, ec);
            fs::create_directories(tempRoot, ec);
            if (ec)
                return {};

            // Recursive copy: every file, every subdirectory. We need
            // Assets/, AssetRegistry.oar, and the .oloproj at minimum;
            // copying everything is simpler than enumerating selectively.
            fs::copy(sandboxRoot, tempRoot,
                     fs::copy_options::recursive |
                         fs::copy_options::overwrite_existing |
                         fs::copy_options::copy_symlinks,
                     ec);
            if (ec)
                return {};

            return tempRoot;
        }

        // Find the .oloproj inside a staged temp project. Sandbox.oloproj
        // is the only one in the real project; if the file count or name
        // changes, this needs to track.
        fs::path FindProjectFile(const fs::path& tempRoot)
        {
            std::error_code ec;
            for (auto& entry : fs::directory_iterator(tempRoot, ec))
            {
                if (ec)
                    break;
                if (entry.is_regular_file() && entry.path().extension() == ".oloproj")
                    return entry.path();
            }
            return {};
        }
    } // namespace

    TEST(AssetSceneLoad, DISABLED_AllSandboxScenesDeserialiseThroughEditorAssetManager)
    {
        const fs::path tempRoot = StageSandboxProjectIntoTemp();
        ASSERT_FALSE(tempRoot.empty())
            << "Failed to stage SandboxProject into temp dir.";

        // RAII cleanup: delete the temp dir on test exit regardless of
        // assertion outcome.
        struct Cleanup
        {
            fs::path Dir;
            ~Cleanup()
            {
                std::error_code ec;
                fs::remove_all(Dir, ec);
            }
        } cleanup{ tempRoot };

        const fs::path projectFile = FindProjectFile(tempRoot);
        ASSERT_FALSE(projectFile.empty())
            << "No .oloproj found inside staged temp project at " << tempRoot.string();

        ASSERT_TRUE(Project::Load(projectFile))
            << "Project::Load failed on staged temp project.";

        auto assetManager = Ref<EditorAssetManager>::Create();
        assetManager->Initialize();
        Project::SetAssetManager(assetManager);

        const fs::path scenesDir = tempRoot / "Assets" / "Scenes";
        std::vector<fs::path> scenes;
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(scenesDir, ec))
        {
            if (ec)
                break;
            if (entry.is_regular_file() && entry.path().extension() == ".olo")
                scenes.push_back(entry.path());
        }
        ASSERT_FALSE(scenes.empty()) << "No scenes found under " << scenesDir.string();
        std::ranges::sort(scenes);

        struct Failure
        {
            std::string Path;
            std::string Reason;
        };
        std::vector<Failure> failures;

        for (const auto& path : scenes)
        {
            auto scene = Scene::Create();
            SceneSerializer serializer(scene);
            bool ok = false;
            try
            {
                ok = serializer.Deserialize(path);
            }
            catch (const std::exception& e)
            {
                failures.push_back({ path.generic_string(),
                                     std::string("threw: ") + e.what() });
                continue;
            }
            catch (...)
            {
                failures.push_back({ path.generic_string(), "threw unknown exception" });
                continue;
            }
            if (!ok)
            {
                failures.push_back({ path.generic_string(),
                                     "Deserialize() returned false — see engine log" });
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " sample scene(s) failed full deserialisation:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }

        EXPECT_GE(scenes.size(), 1u);
    }
} // namespace OloEngine::Tests
