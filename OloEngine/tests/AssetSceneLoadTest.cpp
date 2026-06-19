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
// Renderer dependency (why this needs a GL context, like the visual tests)
// ------------------------------------------------------------------------
// Deserialising a scene through the production path eagerly builds GPU
// resources: `MeshComponent` primitives and animated/static models call
// `MeshSource::Build()` (`glCreateVertexArrays` / `glCreate*Buffer`),
// material/texture handles call `Texture2D::Create()`, text components call
// `Font::Create()` (atlas upload), shader-graph materials compile shaders.
// Every one of those needs a live GL 4.6 context (the function pointers are
// only loaded after a context is current). Run headless, the first such call
// is a null function-pointer dereference — `AnimationIKTest.olo` SEH-crashes
// (0xc0000005) inside `MeshSource::Build()` right after the Floor plane's
// `OptimizeMesh`, at the first `VertexBuffer::Create`.
//
// This is the SAME GL-context coupling the `RendererAttachedTest` /
// visual-evidence tests have — not a deserialiser-specific bug — so the test
// is no longer `DISABLED_`. Like those tests, it brings the process-wide
// renderer up (when a GL 4.6 context exists) and exercises the full editor
// "File → Open Scene" path; on a headless box with no GPU it `GTEST_SKIP`s
// cleanly. The deserialise loop is wrapped in a `GLStateGuard(Restore)` so the
// GPU resources it creates can't poison later GPU tests in the same process.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RendererTypes.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include "Rendering/PropertyTests/RenderPropertyTest.h"

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

    TEST(AssetSceneLoad, AllSandboxScenesDeserialiseThroughEditorAssetManager)
    {
        // The full deserialise path builds GPU resources (meshes, textures,
        // fonts, shader-graph shaders), so it needs a live GL 4.6 context.
        // Skip cleanly on a headless box — same gate as the visual-evidence
        // tests — so this never *fails* CI without a GPU.
        OLO_ENSURE_GPU_OR_SKIP();

        // Bring the process-wide renderer up once (idempotent across suites via
        // Renderer3D::IsInitialized — see RendererAttachedTest). Required so the
        // shader library / default materials a scene may resolve are present,
        // matching the editor's state on "File → Open Scene". Teardown is the
        // process-wide Renderer::Shutdown in the test main(), not here.
        if (!Renderer3D::IsInitialized())
        {
            Renderer::Init(RendererType::Renderer3D, /*loadingWindow=*/nullptr);
        }

        // Contain the GPU bindings the deserialise loop creates (bound VAO /
        // buffers / textures) so they can't poison later GPU tests in the
        // shared process+context.
        GLStateGuard glGuard("AssetSceneLoad.Deserialize", GLStateGuard::Policy::Restore);

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
        // No file watcher: this test only reads. A watcher would spawn a
        // background thread on the temp dir we delete at scope exit.
        assetManager->Initialize(/*startFileWatcher=*/false);
        Project::SetAssetManager(assetManager);

        // Deserialising loads GPU assets (textures / meshes / fonts) INTO the
        // asset manager, which Project holds in a static Ref. Left to destruct
        // at process exit, those GPU Refs free GL objects after the renderer's
        // memory-tracker / FrameResourceManager singletons are gone — the same
        // SIGSEGV the test main() avoids for the renderer statics. Release them
        // here, while the GL context and those singletons are still alive.
        // Declared AFTER `cleanup` so it destructs FIRST: Shutdown serialises
        // the registry back to the temp project, which must happen before the
        // temp dir is removed.
        struct AssetManagerShutdown
        {
            Ref<EditorAssetManager> Mgr;
            ~AssetManagerShutdown()
            {
                if (Mgr)
                    Mgr->Shutdown();
            }
        } assetManagerShutdown{ assetManager };

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
