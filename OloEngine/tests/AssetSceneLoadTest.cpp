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
#include "ProcessRendererInit.h"

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/ColorTransfer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RendererTypes.h"
#include "OloEngine/Renderer/TextureImportSettings.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include "Rendering/PropertyTests/RenderPropertyTest.h"
#include "Rendering/PropertyTests/RendererAttachedTest.h"

#include "OloEngine/Asset/AssetRegistry.h"

#include <stb_image/stb_image_write.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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
        // Returns the temp project's root path (or empty on failure, with the
        // reason written to `error` — a bare empty path made a permission
        // failure here indistinguishable from a copy failure).
        fs::path StageSandboxProjectIntoTemp(std::string& error)
        {
            const fs::path sandboxRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";

            // A FIXED name under a shared, sticky /tmp is not usable. This repo's
            // self-hosted CI runs the suite as its own account, so
            // /tmp/OloEngineSceneLoad can already exist owned by a different user —
            // and the sticky bit then makes it un-removable AND un-recreatable for
            // everyone else, failing this test forever. Two concurrent runs on one
            // box (several runners share this host) would likewise fight over the
            // same tree mid-copy.
            //
            // TempDir() hands back a directory under a root this process claimed
            // EXCLUSIVELY (TestTempDir.h): it is never a directory we merely found,
            // so it can never be a live staging tree belonging to a concurrent run.
            // That is the property the hand-rolled claim loop here used to provide.
            std::error_code ec;
            const fs::path stagingRoot = OloEngine::Tests::TempDir("sandbox");
            const fs::path tempRoot = stagingRoot / "SandboxProject";
            if (!fs::create_directories(tempRoot, ec))
            {
                error = "could not create a staging dir at " + tempRoot.string();
                return {};
            }

            // Recursive copy: every file, every subdirectory. We need
            // Assets/, AssetRegistry.oar, and the .oloproj at minimum;
            // copying everything is simpler than enumerating selectively.
            fs::copy(sandboxRoot, tempRoot,
                     fs::copy_options::recursive |
                         fs::copy_options::overwrite_existing |
                         fs::copy_options::copy_symlinks,
                     ec);
            if (ec)
            {
                error = "copy " + sandboxRoot.string() + " -> " + tempRoot.string() + " failed: " + ec.message();
                std::error_code cleanupEc;
                fs::remove_all(tempRoot, cleanupEc);
                if (cleanupEc)
                {
                    // Say so rather than swallow it: a partial tree left behind
                    // is the next run's confusing failure.
                    error += " (cleanup of " + tempRoot.string() + " also failed: " + cleanupEc.message() + ")";
                }
                return {};
            }

            // The staged registry retains its ../assets paths. Stage the
            // registered engine fonts in that same layout.
            const auto fonts = fs::path("assets/fonts/opensans");
            fs::create_directories((stagingRoot / fonts).parent_path(), ec);
            if (!ec)
                fs::copy(fs::path(OLO_TEST_EDITOR_ROOT) / fonts, stagingRoot / fonts, fs::copy_options::recursive, ec);
            if (ec)
            {
                error = "could not stage engine fonts: " + ec.message();
                return {};
            }

            // Drop the binary sidecars the copy brought along. They are
            // git-ignored caches the developer's own editor wrote, and a load
            // prefers one to the YAML: a stale sidecar (one written while a
            // crashed session's textures failed to load) made these tests read
            // that file instead of the code under test, red on one box only.
            std::vector<fs::path> sidecars;
            for (auto it = fs::recursive_directory_iterator(tempRoot, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_regular_file() && it->path().extension() == ".scenebin")
                {
                    sidecars.push_back(it->path());
                }
            }
            for (const auto& sidecar : sidecars)
            {
                if (!fs::remove(sidecar, ec) || ec)
                {
                    error = "could not remove the staged sidecar " + sidecar.string() + ": " + ec.message();
                    return {};
                }
            }
            if (ec)
            {
                error = "listing the staged sidecars under " + tempRoot.string() + " failed: " + ec.message();
                return {};
            }

            return tempRoot;
        }

        std::vector<char> ReadAllBytes(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            return in ? std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())
                      : std::vector<char>{};
        }

        // Every entry path of a v3 AssetRegistry.oar, read straight from its bytes:
        // Deserialize drops cache entries, so it cannot show that one was written.
        // Empty optional when the bytes are not a whole v3 registry.
        std::optional<std::vector<fs::path>> RawRegistryEntryPaths(const std::vector<char>& bytes)
        {
            sizet offset = 0;
            const auto take = [&](auto& value)
            {
                if (bytes.size() - offset < sizeof(value))
                    return false;
                std::memcpy(&value, bytes.data() + offset, sizeof(value));
                offset += sizeof(value);
                return true;
            };
            u32 version = 0;
            u32 count = 0;
            if (!take(version) || version != OloEngine::AssetRegistry::FileVersion || !take(count))
                return std::nullopt;
            std::vector<fs::path> paths;
            for (u32 i = 0; i < count; ++i)
            {
                u64 handle = 0;
                u32 type = 0;
                u32 length = 0;
                if (!take(handle) || !take(type) || !take(length) || bytes.size() - offset < length)
                    return std::nullopt;
                paths.emplace_back(std::string(bytes.data() + offset, length));
                offset += length;
            }
            if (offset != bytes.size())
                return std::nullopt;
            return paths;
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
        OloEngine::Tests::EnsureProcessRendererInitialized();

        // Contain the GPU bindings the deserialise loop creates (bound VAO /
        // buffers / textures) so they can't poison later GPU tests in the
        // shared process+context.
        GLStateGuard glGuard("AssetSceneLoad.Deserialize", GLStateGuard::Policy::Restore);

        std::string stageError;
        const fs::path tempRoot = StageSandboxProjectIntoTemp(stageError);
        ASSERT_FALSE(tempRoot.empty())
            << "Failed to stage SandboxProject into temp dir: " << stageError;

        // RAII cleanup: delete the temp dir on test exit regardless of
        // assertion outcome.
        struct Cleanup
        {
            fs::path Dir;
            ~Cleanup()
            {
                // Retry, and report if it still fails.
                //
                // remove_all walks bottom-up, and the asset manager recreates
                // its Assets/cache/{mesh,animation} directories during teardown
                // — if that lands between the child removal and the parent's,
                // the parent fails ENOTEMPTY and the tree is left behind. A
                // single silent attempt leaked a staging root per run, which
                // used to be masked because a fixed directory name meant the
                // next run reclaimed it; now that each run stages under its own
                // pid, a leak accumulates instead.
                std::error_code ec;
                for (int attempt = 0; attempt < 3; ++attempt)
                {
                    ec.clear();
                    fs::remove_all(Dir, ec);
                    // Judge success by whether the directory is GONE, not by
                    // what remove_all reported. The race here is a recreate
                    // landing after the removal, which leaves remove_all
                    // reporting success while the tree survives — so an
                    // `!ec ||` short-circuit would return without retrying in
                    // precisely the case the retry exists for.
                    //
                    // Non-throwing overload: a destructor is implicitly
                    // noexcept, so the throwing fs::exists(Dir) would call
                    // std::terminate rather than report a failure to clean up.
                    std::error_code existsEc;
                    if (!fs::exists(Dir, existsEc))
                        return;
                }
                OLO_CORE_WARN("AssetSceneLoad: could not remove staging dir '{}': {}", Dir.string(), ec.message());
            }
        } cleanup{ tempRoot };

        const std::vector<char> registryBefore = ReadAllBytes(tempRoot / "AssetRegistry.oar");
        ASSERT_FALSE(registryBefore.empty()) << "the staged project has no AssetRegistry.oar";

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
        SceneSerializer::AssetLoadCache sharedAssets;
        struct AssetManagerShutdown
        {
            Ref<EditorAssetManager> Mgr;
            SceneSerializer::AssetLoadCache& SharedAssets;
            ~AssetManagerShutdown()
            {
                SharedAssets.Clear();
                if (Mgr)
                {
                    // Keep the shared mesh/texture cache throughout the scene
                    // sweep, then release it once while the context is alive.
                    Mgr->UnloadLoadedAssets();
                    Mgr->Shutdown();
                    EXPECT_TRUE(Mgr->GetLoadedAssets().empty());
                }

                // Shutting the manager down is not the same as uninstalling it.
                // `Project` holds the active project and the asset manager in
                // two statics, and without this the process is left with a
                // SHUT-DOWN manager and a project whose directory `cleanup`
                // deletes a moment later — both still non-null.
                //
                // That corpse poisons every later test in a single-process run
                // (issue #1074). The renderer fixtures install their own temp
                // project guarded by `if (!Project::GetActive() ||
                // !Project::HasAssetManager())`; both are non-null here, so
                // they skip their own setup and adopt this one instead. Their
                // `AssetManager::AddMemoryOnlyAsset` calls then register into a
                // dead manager, `GetAsset<T>` hands back nothing, and the
                // subject of the test never renders — a visual-evidence test
                // measuring an empty frame ~2,000 tests later, with nothing
                // pointing back here. This single test accounted for 21 of the
                // 102 monolithic-run failures.
                //
                // `Unload()` drops both statics, which is the pristine state
                // every fixture is written to handle. Deliberately not "restore
                // whatever was there before": the previous value could itself
                // be a corpse, and `GetAssetManager()` asserts on null, so a
                // caller that forgets to check now fails loudly instead of
                // silently reading a dead manager.
                Project::Unload();
                FrameResourceManager::Get().FlushAllDeletionQueues();
                EXPECT_FALSE(Project::GetActive());
                EXPECT_FALSE(Project::HasAssetManager());
            }
        } assetManagerShutdown{ assetManager, sharedAssets };

        for (const char* name : { "FirstPersonPlayer", "ThirdPersonPlayer" })
        {
            SCOPED_TRACE(name);
            AssetMetadata metadata;
            metadata.FilePath = fs::path("Assets/Prefabs") / (std::string(name) + ".oloprefab");
            metadata.Type = AssetType::Prefab;
            Ref<Asset> asset;
            ASSERT_TRUE(PrefabSerializer().TryLoadData(metadata, asset));
            const auto prefab = asset.As<Prefab>();
            ASSERT_TRUE(prefab);
            Entity root = prefab->GetRootEntity();
            ASSERT_TRUE(root);
            EXPECT_EQ(root.GetName(), name);
        }

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
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_FALSE(scenes.empty()) << "No scenes found under " << scenesDir.string();

        // The benchmark and dog-authoring scene sources also use Sandbox assets.
        // Copy them into the staging tree so their sidecars stay off the source paths.
        const auto repoRoot = fs::path(OLO_TEST_EDITOR_ROOT).parent_path();
        const auto sourceScenes = tempRoot / "SourceScenes";
        fs::create_directories(sourceScenes);
        for (const auto& source : { repoRoot / "scripts/perf/fixtures/RuntimeDogAndFern.olo",
                                    repoRoot / "tools/dog-authoring/sources/Bernese/Bernese.olo" })
        {
            const auto staged = sourceScenes / source.filename();
            ASSERT_TRUE(fs::copy_file(source, staged)) << source;
            scenes.push_back(staged);
        }
        std::ranges::sort(scenes);

        struct Failure
        {
            std::string Path;
            std::string Reason;
        };
        std::vector<Failure> failures;

        for (const auto& path : scenes)
        {
            // Flush scene-local objects on every exit, including exceptions.
            // The manager retains shared assets for the next scene (#1668).
            // Declared before scene/serializer, so their Refs are gone first.
            struct PerSceneCleanup
            {
                ~PerSceneCleanup()
                {
                    FrameResourceManager::Get().FlushAllDeletionQueues();
                }
            } perSceneCleanup;

            auto scene = Scene::Create();
            SceneSerializer serializer(scene, &sharedAssets);
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
                continue;
            }

            // A scene deserialising cleanly is NOT the same as its geometry being reachable.
            //
            // Scene::ProcessScene3DSharedLogic's virtual-mesh loop resolves each
            // VirtualMeshComponent::m_MeshSource with AssetManager::GetAsset<MeshSource> and, if
            // that returns null, skips the entity. So a scene whose mesh handle does not resolve
            // loads with the right entity count and renders NOTHING — an empty viewport that
            // looks exactly like a camera or lighting problem. (Scene.cpp now warns once per
            // handle at that site; this is the build-time guard for the same class.)
            //
            // Checked here rather than in its own test on purpose: staging the sandbox project
            // into temp is by far the most expensive thing in this file, and it is already paid
            // for above. A second test doing its own staging doubled the cost for nothing.
            for (auto entity : scene->GetAllEntitiesWith<VirtualMeshComponent>())
            {
                const auto& vm = scene->GetAllEntitiesWith<VirtualMeshComponent>()
                                     .template get<VirtualMeshComponent>(entity);
                if (!vm.m_Enabled || static_cast<u64>(vm.m_MeshSource) == 0)
                {
                    continue;
                }

                const AssetMetadata metadata = assetManager->GetMetadata(vm.m_MeshSource);

                // An asset that is registered but absent from disk is a fetch step, not a defect:
                // some assets are deliberately not committed (scripts/Fetch-Assets.ps1).
                //
                // It must still DEGRADE rather than abort, so resolve it anyway and only then
                // skip the "did it load" check. Resolving it IS the assertion: before issue #694
                // this line took the process down. A missing asset substitutes a PlaceholderMesh
                // (AssetManager::ResolveAssetOrPlaceholder), whose MeshSource carries no submesh,
                // and Mesh's constructor asserted `submeshIndex < submeshCount` — i.e. 0 < 0. The
                // recovery path for a missing asset was itself the crash. MissingOptionalAsset-
                // DegradesInsteadOfAsserting below pins the same contract without needing an
                // un-fetched asset to be present, since a clone that HAS run the fetcher never
                // reaches this branch.
                if (!metadata.FilePath.empty() && !fs::exists(tempRoot / metadata.FilePath))
                {
                    (void)AssetManager::GetAsset<MeshSource>(vm.m_MeshSource);
                    continue;
                }

                if (!AssetManager::GetAsset<MeshSource>(vm.m_MeshSource))
                {
                    std::ostringstream reason;
                    reason << "VirtualMeshComponent handle " << static_cast<u64>(vm.m_MeshSource) << " ("
                           << metadata.FilePath.generic_string()
                           << ") resolves to no loadable MeshSource — the entity renders NOTHING, silently";
                    failures.push_back({ path.generic_string(), reason.str() });
                }
            }

            // Drop the scene, retaining only the manager's shared assets. This
            // test renders no frames, so PerSceneCleanup still drains deferred
            // deletion of objects that no scene or cached asset holds. The final
            // AssetManagerShutdown drains the shared assets' deletions too.
            scene.Reset();
        }

        // Loading every scene cooks each model's embedded textures into Assets/cache/ and
        // registers them. None of that may reach the registry FILE (#1664): it is tracked, the
        // cache is git-ignored, and a registry committed with those entries fails on a fresh
        // clone only. So after the whole sweep and a save, the file holds the entries that were
        // checked in and nothing else. (Entry ORDER is the asset-registry-no-cache hook's check.)
        ASSERT_TRUE(assetManager->SerializeAssetRegistry());
        sizet cacheArtefacts = 0;
        for (const AssetHandle handle : assetManager->GetAllAssetsWithType(AssetType::Texture2D))
        {
            cacheArtefacts += AssetRegistry::IsCacheArtefact(assetManager->GetMetadata(handle).FilePath) ? 1u : 0u;
        }
        ::testing::Test::RecordProperty("olo_cache_artefacts_registered", std::to_string(cacheArtefacts));
        std::printf("[asset-scene-load] %zu cache artefact(s) registered in memory after the sweep\n", cacheArtefacts);
        {
            // On the raw bytes: Deserialize drops cache entries, so comparing parsed registries
            // cannot see one that was written.
            const std::optional<std::vector<fs::path>> writtenPaths =
                RawRegistryEntryPaths(ReadAllBytes(tempRoot / "AssetRegistry.oar"));
            ASSERT_TRUE(writtenPaths.has_value()) << "the saved AssetRegistry.oar is not a whole v3 registry";
            for (const fs::path& written : *writtenPaths)
            {
                EXPECT_FALSE(AssetRegistry::IsCacheArtefact(written))
                    << "a cache artefact was written to AssetRegistry.oar: " << written.generic_string();
            }

            const fs::path beforePath = TempDir("registry-before") / "AssetRegistry.oar";
            std::ofstream(beforePath, std::ios::binary)
                .write(registryBefore.data(), static_cast<std::streamsize>(registryBefore.size()));
            AssetRegistry before;
            AssetRegistry after;
            ASSERT_TRUE(before.Deserialize(beforePath));
            ASSERT_TRUE(after.Deserialize(tempRoot / "AssetRegistry.oar"));
            std::ostringstream diff;
            const fs::path realProject = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";
            for (const AssetMetadata& m : after.GetAllAssets())
            {
                // A key starting ".." names a file outside the project: the scenes import engine
                // textures (OloEditor/assets/...). The checked-in registry has them as
                // "../assets/...", relative to the real SandboxProject; this staged copy lives
                // elsewhere, so the same file gets another key here. Such an addition is not
                // counted only when, keyed from the real project, the file is already registered.
                // An engine texture the checked-in registry lacks is a real addition.
                const bool escapesTheProject = !m.FilePath.empty() && *m.FilePath.begin() == "..";
                const bool stagingDuplicate =
                    escapesTheProject &&
                    before.Exists(EditorAssetManager::MakeRegistryKey(tempRoot / m.FilePath, realProject));
                if (const AssetMetadata old = before.GetMetadata(m.Handle); !old.IsValid() && !stagingDuplicate)
                    diff << "\n  added   " << static_cast<u64>(m.Handle) << " " << m.FilePath.generic_string();
                else if (old.IsValid() && (old.FilePath != m.FilePath || old.Type != m.Type))
                    diff << "\n  changed " << static_cast<u64>(m.Handle) << " " << old.FilePath.generic_string()
                         << " -> " << m.FilePath.generic_string();
            }
            for (const AssetMetadata& m : before.GetAllAssets())
            {
                if (!after.Exists(m.Handle))
                    diff << "\n  removed " << static_cast<u64>(m.Handle) << " " << m.FilePath.generic_string();
            }
            EXPECT_TRUE(diff.str().empty())
                << "loading the sandbox scenes changed the entries of AssetRegistry.oar; the checked-in registry "
                   "would be dirty after any editor session:"
                << diff.str();
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

        ::testing::Test::RecordProperty("olo_scene_count", std::to_string(scenes.size()));
        EXPECT_GE(scenes.size(), 1u);
    }

    // The "a missing opt-in asset degrades, it does not assert" contract (issue #694),
    // pinned without depending on an un-fetched asset actually being absent — on a clone
    // that has run scripts\Fetch-Assets.ps1 the scene-loop branch above never executes,
    // so this is the test that always runs.
    //
    // Two written contracts promise this behaviour and both used to be false:
    //   * scripts/Fetch-Assets.ps1's .DESCRIPTION — "must degrade gracefully when it is
    //     missing ... never fail".
    //   * VirtualGeometryStress.olo's header — "the dragons simply do not resolve and you
    //     get an empty hall. It will not crash."
    //
    // Needs no GL context: nothing here calls MeshSource::Build().
    TEST(AssetSceneLoad, MissingOptionalAssetDegradesInsteadOfAsserting)
    {
        // 1. A MeshSource that exists but carries no submesh. This is not a contrived
        //    input: MeshSource's (vertices, indices) constructor does not create one, so
        //    every in-memory MeshSource starts out like this. The old Mesh ctor asserted
        //    `submeshIndex < submeshCount`, which is 0 < 0 here — the perfectly ordinary
        //    submeshIndex = 0 aborted.
        auto emptySource = Ref<MeshSource>::Create(std::vector<Vertex>{}, std::vector<u32>{});
        auto meshOverEmptySource = Ref<Mesh>::Create(emptySource, 0u);
        ASSERT_TRUE(meshOverEmptySource);
        EXPECT_FALSE(meshOverEmptySource->IsValid());

        // 2. A null MeshSource — what MeshSerializer::DeserializeFromAssetPack hands the
        //    ctor when a packed handle does not resolve in a shipped OloRuntime game.
        auto meshOverNullSource = Ref<Mesh>::Create(Ref<MeshSource>{}, 0u);
        ASSERT_TRUE(meshOverNullSource);
        EXPECT_FALSE(meshOverNullSource->IsValid());

        // 3. An out-of-range index over a populated source.
        auto populatedSource = Ref<MeshSource>::Create(std::vector<Vertex>(3), std::vector<u32>{ 0u, 1u, 2u });
        Submesh only;
        only.m_VertexCount = 3;
        only.m_IndexCount = 3;
        populatedSource->AddSubmesh(only);
        auto meshOutOfRange = Ref<Mesh>::Create(populatedSource, 7u);
        ASSERT_TRUE(meshOutOfRange);
        EXPECT_FALSE(meshOutOfRange->IsValid());
        EXPECT_TRUE(Ref<Mesh>::Create(populatedSource, 0u)->IsValid());

        // Every accessor must be TOTAL on a not-valid mesh, or "fail soft" just moves the
        // crash one call deeper: the whole point is that such a mesh renders as nothing.
        // Note GetVertices()/GetIndices() are MeshSource-scoped, so meshOutOfRange still
        // reports the source's geometry — it is the SUBMESH range that must come back
        // empty, and that is what every draw path reads.
        for (const auto& invalid : { meshOverEmptySource, meshOverNullSource, meshOutOfRange })
        {
            EXPECT_EQ(invalid->GetIndexCount(), 0u);
            EXPECT_EQ(invalid->GetBaseIndex(), 0u);
            EXPECT_EQ(invalid->GetRendererID(), 0u);
            EXPECT_FALSE(invalid->IsRigged());
            // The draw paths spell "nothing to draw" as `if (!mesh->GetVertexArray())`.
            EXPECT_FALSE(invalid->GetVertexArray());
            // GetSubmesh() on a not-valid mesh yields an empty range, not a read past the end.
            EXPECT_EQ(invalid->GetSubmesh().m_IndexCount, 0u);
            EXPECT_EQ(invalid->GetSubmesh().m_VertexCount, 0u);
            EXPECT_NO_THROW((void)invalid->GetBoundingBox());
        }

        // A null MeshSource has no arrays to delegate to at all, so these must be handed a
        // stable empty container rather than dereferencing null.
        EXPECT_TRUE(meshOverNullSource->GetVertices().IsEmpty());
        EXPECT_TRUE(meshOverNullSource->GetIndices().IsEmpty());
        EXPECT_TRUE(meshOverEmptySource->GetVertices().IsEmpty());
        EXPECT_TRUE(meshOverEmptySource->GetIndices().IsEmpty());

        // 4. The stand-in the asset manager substitutes for an unresolvable handle. This is
        //    the frame that actually fired for VirtualGeometryStress.olo on a fresh clone:
        //    GetAsset<MeshSource>(<un-fetched dragon>) -> ResolveAssetOrPlaceholder ->
        //    GetPlaceholderAsset(MeshSource) -> PlaceholderMesh -> Ref<Mesh>::Create over a
        //    submesh-less MeshSource. It must construct, and its cube must be drawable.
        PlaceholderAssetManager::Initialize();
        struct PlaceholderShutdown
        {
            ~PlaceholderShutdown()
            {
                PlaceholderAssetManager::Shutdown();
            }
        } placeholderShutdown;

        for (const AssetType type : { AssetType::Mesh, AssetType::StaticMesh, AssetType::MeshSource })
        {
            Ref<Asset> placeholder = PlaceholderAssetManager::GetPlaceholderAsset(type);
            ASSERT_TRUE(placeholder) << "no placeholder for asset type " << static_cast<int>(type);

            Ref<PlaceholderMesh> placeholderMesh = placeholder.As<PlaceholderMesh>();
            ASSERT_TRUE(placeholderMesh);
            ASSERT_TRUE(placeholderMesh->GetMesh());
            // Not merely "did not abort": the stand-in cube was never renderable, because
            // nothing ever gave its MeshSource a submesh to address.
            EXPECT_TRUE(placeholderMesh->GetMesh()->IsValid());
            EXPECT_GT(placeholderMesh->GetMesh()->GetIndexCount(), 0u);
        }
    }

    // -------------------------------------------------------------------------------
    // Issue #887: DecalComponent texture references were lost on a scene round-trip
    // (empty path via YAML, dropped via .scenebin) because EditorAssetManager::ImportAsset
    // resolved a project-relative scene texture path ("Assets/Textures/Foo.png") against
    // the process's current working directory instead of the project directory — silently
    // wrong whenever OloEditor's actual cwd (OloEditor/) happens to contain a same-named
    // file under its own generic assets/ tree too, which it does for every shipped
    // texture. A texture whose resolution fails this way ends up as a non-null placeholder
    // Ref<Texture2D> with an empty GetPath(), which SceneSerializer still happily writes —
    // and a component that round-tripped that empty path once already writes nothing at
    // all on the NEXT save, since the empty path fails to resolve back into a texture on
    // reload.
    //
    // This pins the fix for both the decal texture arm (the reported symptom, freshly
    // authored with the current "Assets/..." convention) and the pre-existing sprite
    // texture control (PinkCubeWithTextures.olo, a pre-existing scene whose texture must
    // resolve inside the staged project) so a regression in either direction is
    // caught. Two loads of the same scene path are checked because the reported failure is
    // a "second load" class of bug: the binary sidecar written on the first load must carry
    // the same correct path forward, not silently drop it.
    TEST(AssetSceneLoad, DecalAndSpriteTextureHandlesSurviveYAMLAndSidecar)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        OloEngine::Tests::EnsureProcessRendererInitialized();

        GLStateGuard glGuard("AssetSceneLoad.TexturePathRoundTrip", GLStateGuard::Policy::Restore);

        std::string stageError;
        const fs::path tempRoot = StageSandboxProjectIntoTemp(stageError);
        ASSERT_FALSE(tempRoot.empty())
            << "Failed to stage SandboxProject into temp dir: " << stageError;

        struct Cleanup
        {
            fs::path Dir;
            ~Cleanup()
            {
                std::error_code ec;
                for (int attempt = 0; attempt < 3; ++attempt)
                {
                    ec.clear();
                    fs::remove_all(Dir, ec);
                    std::error_code existsEc;
                    if (!fs::exists(Dir, existsEc))
                        return;
                }
                OLO_CORE_WARN("AssetSceneLoad: could not remove staging dir '{}': {}", Dir.string(), ec.message());
            }
        } cleanup{ tempRoot };

        const fs::path projectFile = FindProjectFile(tempRoot);
        ASSERT_FALSE(projectFile.empty())
            << "No .oloproj found inside staged temp project at " << tempRoot.string();

        ASSERT_TRUE(Project::Load(projectFile))
            << "Project::Load failed on staged temp project.";

        auto assetManager = Ref<EditorAssetManager>::Create();
        assetManager->Initialize(/*startFileWatcher=*/false);
        Project::SetAssetManager(assetManager);

        struct AssetManagerShutdown
        {
            Ref<EditorAssetManager> Mgr;
            ~AssetManagerShutdown()
            {
                if (Mgr)
                    Mgr->Shutdown();

                // Shutting the manager down is not the same as uninstalling it.
                // `Project` holds the active project and the asset manager in
                // two statics, and without this the process is left with a
                // SHUT-DOWN manager and a project whose directory `cleanup`
                // deletes a moment later — both still non-null.
                //
                // That corpse poisons every later test in a single-process run
                // (issue #1074). The renderer fixtures install their own temp
                // project guarded by `if (!Project::GetActive() ||
                // !Project::HasAssetManager())`; both are non-null here, so
                // they skip their own setup and adopt this one instead. Their
                // `AssetManager::AddMemoryOnlyAsset` calls then register into a
                // dead manager, `GetAsset<T>` hands back nothing, and the
                // subject of the test never renders — a visual-evidence test
                // measuring an empty frame ~2,000 tests later, with nothing
                // pointing back here. This single test accounted for 21 of the
                // 102 monolithic-run failures.
                //
                // `Unload()` drops both statics, which is the pristine state
                // every fixture is written to handle. Deliberately not "restore
                // whatever was there before": the previous value could itself
                // be a corpse, and `GetAssetManager()` asserts on null, so a
                // caller that forgets to check now fails loudly instead of
                // silently reading a dead manager.
                Project::Unload();
            }
        } assetManagerShutdown{ assetManager };

        // Load a scene twice through the SAME path (exercising both the YAML load, which
        // also writes the .scenebin sidecar, and the second call's binary-sidecar fast
        // path) and return the requested texture's GetPath() both times.
        auto loadTwiceAndGetPath = [](const fs::path& scenePath, auto&& findTexture) -> std::pair<std::string, std::string>
        {
            std::string first;
            std::string second;
            {
                auto scene = Scene::Create();
                SceneSerializer serializer(scene);
                bool ok = serializer.Deserialize(scenePath);
                EXPECT_TRUE(ok) << "First Deserialize() of " << scenePath.string() << " failed.";
                if (ok)
                {
                    Ref<Texture2D> tex = findTexture(scene);
                    first = tex ? tex->GetPath() : std::string{ "<null texture ref>" };
                }
            }
            {
                auto scene = Scene::Create();
                SceneSerializer serializer(scene);
                bool ok = serializer.Deserialize(scenePath);
                EXPECT_TRUE(ok) << "Second Deserialize() of " << scenePath.string() << " failed.";
                if (ok)
                {
                    Ref<Texture2D> tex = findTexture(scene);
                    second = tex ? tex->GetPath() : std::string{ "<null texture ref>" };
                }
            }
            return { first, second };
        };

        // -------------------- decal arm (issue #887's reported symptom) --------------------
        {
            const fs::path scenePath = tempRoot / "Assets" / "Scenes" / "DecalModeMatrixTest.olo";
            ASSERT_TRUE(fs::exists(scenePath)) << scenePath.string();

            auto findEmissiveDecalTexture = [](const Ref<Scene>& scene) -> Ref<Texture2D>
            {
                Ref<Texture2D> result;
                auto view = scene->GetAllEntitiesWith<TagComponent, DecalComponent>();
                for (auto entity : view)
                {
                    const auto& [tag, decal] = view.get<TagComponent, DecalComponent>(entity);
                    if (tag.Tag == "Decal Emissive")
                    {
                        result = decal.m_EmissiveTexture;
                        break;
                    }
                }
                return result;
            };

            auto [firstPath, secondPath] = loadTwiceAndGetPath(scenePath, findEmissiveDecalTexture);
            EXPECT_FALSE(firstPath.empty())
                << "DecalComponent::m_EmissiveTexture had an empty GetPath() on the FIRST load "
                   "(YAML) — the texture failed to resolve and a placeholder was silently substituted.";
            EXPECT_FALSE(secondPath.empty())
                << "DecalComponent::m_EmissiveTexture had an empty GetPath() on the SECOND load "
                   "(binary sidecar) — the reference was dropped on the fast path.";
            EXPECT_EQ(firstPath, secondPath)
                << "The YAML and binary-sidecar loads disagree about the decal's texture path.";
        }

        // -------------------- sprite control (pre-existing scene) --------------------
        {
            const fs::path scenePath = tempRoot / "Assets" / "Scenes" / "PinkCubeWithTextures.olo";
            ASSERT_TRUE(fs::exists(scenePath)) << scenePath.string();

            auto findFirstSpriteTexture = [](const Ref<Scene>& scene) -> Ref<Texture2D>
            {
                Ref<Texture2D> result;
                auto view = scene->GetAllEntitiesWith<SpriteRendererComponent>();
                for (auto entity : view)
                {
                    const auto& src = view.get<SpriteRendererComponent>(entity);
                    if (src.Texture)
                    {
                        result = src.Texture;
                        break;
                    }
                }
                return result;
            };

            // This scene used to carry the "SandboxProject/Assets/..." spelling,
            // which only ever resolved because OloEditor runs one directory above
            // the project — a property of the launch, not of the project. Under it
            // this very test resolved to the ORIGINAL repository texture rather than
            // to the staged copy, and registered the same file under two handles.
            // The content is project-relative now and the prefixed spelling is no
            // longer tolerated (#1496).
            //
            // So the assertion is not merely "non-empty": it is that the texture
            // resolves INSIDE the project it was loaded from. That is the property
            // the cwd fallback could not provide, and asserting only non-emptiness
            // is what let it look correct for a year.
            auto [firstPath, secondPath] = loadTwiceAndGetPath(scenePath, findFirstSpriteTexture);
            EXPECT_FALSE(firstPath.empty())
                << "SpriteRendererComponent::Texture had an empty GetPath() on the FIRST load — "
                   "the scene's project-relative texture path must resolve.";
            EXPECT_FALSE(secondPath.empty())
                << "SpriteRendererComponent::Texture had an empty GetPath() on the SECOND load.";
            EXPECT_EQ(firstPath, secondPath);

            // Project-relative, and pointing at the STAGED tree. A path that is
            // absolute, or that climbs out with "..", is the cwd resolution coming
            // back.
            const fs::path resolved = fs::path(firstPath);
            EXPECT_FALSE(resolved.is_absolute())
                << "the sprite texture resolved to an ABSOLUTE path (" << firstPath
                << ") — scene texture references must stay project-relative to be portable.";
            EXPECT_EQ(firstPath.find(".."), std::string::npos)
                << "the sprite texture resolved OUTSIDE the project (" << firstPath
                << ") — the scene texture path must resolve against the project root, not the cwd.";
            EXPECT_TRUE(fs::exists(tempRoot / resolved))
                << "the sprite texture path '" << firstPath
                << "' does not name a file inside the staged project at " << tempRoot.string();
        }
    }

    // -------------------------------------------------------------------------------
    // The committed scene references an OpenSans font from the registry.
    // The default uses the same source file, so a loaded font alone does
    // not prove that the authored reference was restored.
    TEST(AssetSceneLoad, AuthoredFontResolvesByHandle)
    {
        // Font::Create rasterises an MSDF atlas into a GPU texture.
        OLO_ENSURE_GPU_OR_SKIP();

        OloEngine::Tests::EnsureProcessRendererInitialized();

        GLStateGuard glGuard("AssetSceneLoad.SceneFontPaths", GLStateGuard::Policy::Restore);

        std::string stageError;
        const fs::path tempRoot = StageSandboxProjectIntoTemp(stageError);
        ASSERT_FALSE(tempRoot.empty())
            << "Failed to stage SandboxProject into temp dir: " << stageError;

        struct Cleanup
        {
            fs::path Dir;
            ~Cleanup()
            {
                std::error_code ec;
                for (int attempt = 0; attempt < 3; ++attempt)
                {
                    ec.clear();
                    fs::remove_all(Dir, ec);
                    std::error_code existsEc;
                    if (!fs::exists(Dir, existsEc))
                        return;
                }
                OLO_CORE_WARN("AssetSceneLoad: could not remove staging dir '{}': {}", Dir.string(), ec.message());
            }
        } cleanup{ tempRoot };

        const fs::path projectFile = FindProjectFile(tempRoot);
        ASSERT_FALSE(projectFile.empty())
            << "No .oloproj found inside staged temp project at " << tempRoot.string();
        ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed on staged temp project.";

        auto assetManager = Ref<EditorAssetManager>::Create();
        assetManager->Initialize(/*startFileWatcher=*/false);
        Project::SetAssetManager(assetManager);

        struct AssetManagerShutdown
        {
            Ref<EditorAssetManager> Mgr;
            ~AssetManagerShutdown()
            {
                if (Mgr)
                    Mgr->Shutdown();
                // See DecalAndSpriteTextureHandlesSurviveYAMLAndSidecar: a shut-down manager
                // left installed poisons every later test in the process (#1074).
                Project::Unload();
            }
        } assetManagerShutdown{ assetManager };

        // The staged project's copy is what loads; the FONT it names lives outside it.
        const fs::path scenePath = tempRoot / "Assets" / "Scenes" / "FontRenderingTest.olo";
        ASSERT_TRUE(fs::exists(scenePath)) << scenePath.string();

        auto scene = Scene::Create();
        SceneSerializer serializer(scene);
        ASSERT_TRUE(serializer.Deserialize(scenePath)) << "Deserialize() of " << scenePath.string() << " failed.";

        u32 checked = 0;
        auto view = scene->GetAllEntitiesWith<TagComponent, TextComponent>();
        for (auto entity : view)
        {
            const auto& [tag, text] = view.get<TagComponent, TextComponent>(entity);
            ASSERT_TRUE(text.FontAsset) << "entity '" << tag.Tag << "' has a null font ref";
            const auto expectedHandle = assetManager->GetAssetHandleFromFilePath(tempRoot / "../assets/fonts/opensans/OpenSans-Regular.ttf");
            ASSERT_NE(expectedHandle, AssetHandle(0));
            EXPECT_EQ(text.FontAsset->GetHandle(), expectedHandle) << tag.Tag;
            EXPECT_TRUE(text.FontAsset->IsLoaded())
                << "entity '" << tag.Tag << "': the scene's font did not load. Font::Create hands back a "
                                            "non-null, UNLOADED Font on a miss and the text falls back to the default typeface, "
                                            "so a null check would not have caught this. Resolved path: '"
                << text.FontAsset->GetPath() << "'";
            ++checked;
        }

        EXPECT_GT(checked, 0u)
            << "FontRenderingTest.olo carries no TextComponent any more — this test is asserting nothing. "
               "Point it at another scene with a registered font handle.";
    }

    // A texture imported as a project asset reports its project-relative path in
    // the platform's spelling, and the scene wrote it verbatim: the dog showcase
    // exported "AlbedoMapPath: Assets\Models\Dog\DogIrisColor.png" on Windows, a
    // name that is no file on Linux (#1533). Material maps are written with
    // forward slashes.
    TEST(AssetSceneLoad, MaterialTexturePathsAreWrittenWithForwardSlashes)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        OloEngine::Tests::EnsureProcessRendererInitialized();

        GLStateGuard glGuard("AssetSceneLoad.PortableTexturePaths", GLStateGuard::Policy::Restore);

        const fs::path relative = fs::path("Assets") / "Textures" / "Checkerboard.png";
        const fs::path file = fs::path(OLO_TEST_EDITOR_ROOT) / "SandboxProject" / relative;
        ASSERT_TRUE(fs::exists(file)) << file.string();
        // The identity TextureSerializer gives an imported texture: metadata.FilePath.string().
        const Ref<Texture2D> texture = Texture2D::Create(file.string(), true, relative.string());
        ASSERT_TRUE(texture && texture->IsLoaded());

        auto scene = Scene::Create();
        Entity eye = scene->CreateEntity("Material");
        auto& material = eye.AddComponent<MaterialComponent>().m_Material;
        material.SetAlbedoMap(texture);
        material.SetNormalMap(texture);

        const std::string yaml = SceneSerializer(scene).SerializeToYAML();
        for (const char* key : { "AlbedoMapPath", "NormalMapPath" })
        {
            EXPECT_NE(yaml.find(std::string(key) + ": Assets/Textures/Checkerboard.png"), std::string::npos)
                << key << " was not written as a forward-slash project-relative path";
        }
        EXPECT_EQ(yaml.find("Assets\\"), std::string::npos) << "a texture path was written with backslashes";
    }

    class SceneTextureGPUContract : public RendererAttachedTest
    {
      protected:
        bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
        }
    };

    TEST_F(SceneTextureGPUContract, BoundLinearTexturesRecoverAfterMalformedReimport)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GLStateGuard glGuard("AssetSceneLoad.ReimportRecovery", GLStateGuard::Policy::Restore);
        struct UnloadProject
        {
            ~UnloadProject()
            {
                Project::Unload();
            }
        } unloadProject;

        const fs::path root = TempDir("reimport-recovery");
        fs::create_directories(root / "Assets");
        ProjectConfig config;
        config.Name = "ReimportRecovery";
        config.AssetDirectory = "Assets";
        ASSERT_TRUE(Project::NewInMemory(root, config));
        auto editor = Ref<EditorAssetManager>::Create();
        editor->Initialize(false);
        Project::SetAssetManager(editor);

        const fs::path source = root / "Assets" / "MetallicRoughness.png";
        const auto write = [&](int width, int height, u8 roughness, u8 metallic)
        {
            TArray<u8> pixels;
            pixels.SetNumUninitialized(width * height * 4);
            const i32 size = pixels.Num();
            for (i32 i = 0; i < size; i += 4)
            {
                pixels[i] = 0;
                pixels[i + 1] = roughness;
                pixels[i + 2] = metallic;
                pixels[i + 3] = 255;
            }
            return stbi_write_png(source.string().c_str(), width, height, 4, pixels.GetData(), width * 4) != 0;
        };
        ASSERT_TRUE(write(128, 128, 51, 0));
        const AssetHandle handle = editor->ImportAsset(source);
        ASSERT_NE(handle, AssetHandle(0));
        const auto linearSlot = editor->GetSceneTexture(handle, false).As<Texture2D>();
        ASSERT_TRUE(linearSlot && linearSlot->IsLoaded());
        ASSERT_TRUE(editor->ReloadData(handle));
        EXPECT_FALSE(editor->IsAssetLoaded(handle)) << "reloading a live slot allocated a canonical texture";
        auto material = Ref<Material>::Create();
        auto loaded = editor->GetAssetAsync(handle);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!loaded.IsReady && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            editor->SyncWithAssetThread();
            loaded = editor->GetAssetAsync(handle);
        }
        ASSERT_TRUE(loaded.IsReady);
        const auto canonical = loaded.Ptr.As<Texture2D>();
        ASSERT_TRUE(canonical && canonical->IsLoaded());
        material->SetMetallicRoughnessMap(canonical);

        const auto expectBound = [&](u32 width, u32 height, u8 roughness, u8 metallic)
        {
            const auto bound = material->GetMetallicRoughnessMap();
            EXPECT_EQ(bound, canonical);
            for (const auto& texture : { bound, linearSlot })
            {
                ASSERT_TRUE(texture && texture->IsLoaded());
                EXPECT_EQ(texture->GetHandle(), handle);
                EXPECT_EQ(texture->GetWidth(), width);
                EXPECT_EQ(texture->GetHeight(), height);
                RHI::TextureFormatInfo format;
                ASSERT_TRUE(RenderCommand::QueryTextureFormat(texture->GetRHIHandle(), 0, format));
                EXPECT_EQ(format.Neutral, RHI::Format::RGBA8UNorm);
                std::array<f32, 4> pixel{};
                ASSERT_TRUE(RenderCommand::ReadTextureSubImage(texture->GetRHIHandle(), 0, 0, 0, 0, 1, 1, 1,
                                                               RHI::Format::RGBA32Float, sizeof(pixel), pixel.data()));
                EXPECT_NEAR(pixel[1], static_cast<f32>(roughness) / 255.0f, 1e-6f);
                EXPECT_NEAR(pixel[2], static_cast<f32>(metallic) / 255.0f, 1e-6f);
            }
        };
        ASSERT_NO_FATAL_FAILURE(expectBound(128, 128, 51, 0));

        bool retained = false;
        ASSERT_TRUE(write(96, 64, 204, 255));
        ASSERT_TRUE(editor->ReloadData(handle, retained));
        EXPECT_FALSE(retained);
        const u32 validGeneration = editor->GetAssetGeneration(handle);
        ASSERT_NO_FATAL_FAILURE(expectBound(96, 64, 204, 255));

        std::ofstream(source, std::ios::binary | std::ios::trunc) << "invalid PNG";
        EXPECT_FALSE(editor->ReloadData(handle, retained));
        EXPECT_TRUE(retained);
        EXPECT_EQ(editor->GetMetadata(handle).Status, AssetStatus::Failed);
        EXPECT_EQ(editor->GetAssetGeneration(handle), validGeneration);
        EXPECT_EQ(editor->GetAsset(handle).As<Texture2D>(), canonical);
        ASSERT_NO_FATAL_FAILURE(expectBound(96, 64, 204, 255));

        ASSERT_TRUE(write(128, 128, 51, 0));
        ASSERT_TRUE(editor->ReloadData(handle, retained));
        EXPECT_FALSE(retained);
        EXPECT_EQ(editor->GetMetadata(handle).Status, AssetStatus::Loaded);
        EXPECT_EQ(editor->GetAssetGeneration(handle), validGeneration + 1u);
        EXPECT_EQ(editor->GetAsset(handle).As<Texture2D>(), canonical);
        ASSERT_NO_FATAL_FAILURE(expectBound(128, 128, 51, 0));
    }

    TEST_F(SceneTextureGPUContract, SceneTextureAndFontHandlesResolveLooseAndPacked)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GLStateGuard glGuard("AssetSceneLoad.HandleRoundTrip", GLStateGuard::Policy::Restore);
        struct UnloadProject
        {
            ~UnloadProject()
            {
                Project::Unload();
            }
        } unloadProject;

        const fs::path root = TempDir("scene-handle-round-trip");
        fs::create_directories(root / "Assets");
        ProjectConfig config;
        config.Name = "SceneHandleRoundTrip";
        config.AssetDirectory = "Assets";
        ASSERT_TRUE(Project::NewInMemory(root, config));
        auto editor = Ref<EditorAssetManager>::Create();
        editor->Initialize(false);
        Project::SetAssetManager(editor);

        const fs::path editorRoot{ OLO_TEST_EDITOR_ROOT };
        const fs::path texturePath = root / "Assets" / "Checkerboard.png";
        const fs::path normalPath = root / "Assets" / "Checkerboard_Normal.png";
        const fs::path fontPath = root / "Assets" / "OpenSans-Light.ttf";
        // Alternating midtones exercise the actual mip filter. A constant or
        // black/white-only source can't distinguish all decode/filter failures.
        constexpr int extent = 8;
        std::array<u8, extent * extent * 4> pixels{};
        for (sizet i = 0; i < pixels.size(); i += 4)
        {
            pixels[i] = pixels[i + 1] = pixels[i + 2] = ((i / 4) & 1u) ? 192 : 64;
            pixels[i + 3] = 255;
        }
        ASSERT_NE(stbi_write_png(texturePath.string().c_str(), extent, extent, 4, pixels.data(), extent * 4), 0);
        ASSERT_TRUE(fs::copy_file(texturePath, normalPath));
        TextureImportSettings importOverride;
        importOverride.ColorSpace = TextureImportSettings::ColorSpaceChoice::Linear;
        importOverride.GenerateMips = false;
        importOverride.Format = TextureImportSettings::FormatChoice::BC7;
        ASSERT_TRUE(TextureImport::SaveForImage(normalPath.string(), importOverride));
        ASSERT_TRUE(fs::copy_file(editorRoot / "assets/fonts/opensans/OpenSans-Light.ttf", fontPath));
        const auto spritePreview = editor->ImportTexture(texturePath, true);
        const auto dataPreview = editor->ImportTexture(texturePath, false);
        ASSERT_TRUE(spritePreview && spritePreview->IsLoaded());
        ASSERT_TRUE(dataPreview && dataPreview->IsLoaded());
        EXPECT_TRUE(spritePreview->GetSpecification().SRGB);
        EXPECT_FALSE(dataPreview->GetSpecification().SRGB);
        EXPECT_NE(spritePreview, dataPreview);
        EXPECT_EQ(editor->ImportTexture(texturePath, true), spritePreview);
        EXPECT_EQ(editor->ImportTexture(texturePath, false), dataPreview);
        const auto textureHandle = spritePreview->GetHandle();
        EXPECT_EQ(dataPreview->GetHandle(), textureHandle);
        const auto normal = editor->ImportTexture(normalPath, true);
        ASSERT_TRUE(normal && normal->IsLoaded());
        EXPECT_FALSE(normal->GetSpecification().SRGB);
        EXPECT_EQ(normal->GetMipLevelCount(), 1u);
        const auto normalHandle = normal->GetHandle();
        ASSERT_NE(normalHandle, textureHandle);
        {
            AssetRegistry savedRegistry;
            ASSERT_TRUE(savedRegistry.Deserialize(root / "AssetRegistry.oar"));
            EXPECT_EQ(savedRegistry.GetHandleFromPath("Assets/Checkerboard.png"), textureHandle);
            EXPECT_EQ(savedRegistry.GetHandleFromPath("Assets/Checkerboard_Normal.png"), normalHandle);
        }
        const auto fontHandle = editor->ImportAsset(fontPath);
        ASSERT_NE(textureHandle, AssetHandle(0));
        ASSERT_NE(fontHandle, AssetHandle(0));
        const auto texture = AssetManager::GetAsset<Texture2D>(textureHandle);
        const auto font = AssetManager::GetAsset<Font>(fontHandle);
        ASSERT_TRUE(texture && texture->IsLoaded());
        EXPECT_FALSE(texture->GetSpecification().SRGB); // The old scene resolver used this canonical texture.
        ASSERT_TRUE(font && font->IsLoaded());

        const fs::path scenePath = root / "Assets" / "HandleProbe.olo";
        {
            auto scene = Scene::Create();
            auto entity = scene->CreateEntity("HandleProbe");
            entity.AddComponent<SpriteRendererComponent>().Texture = spritePreview;
            entity.AddComponent<UIImageComponent>().m_Texture = spritePreview;
            entity.AddComponent<UIPanelComponent>().m_BackgroundTexture = spritePreview;
            auto& decal = entity.AddComponent<DecalComponent>();
            decal.m_AlbedoTexture = spritePreview;
            decal.m_NormalTexture = dataPreview;
            decal.m_RMATexture = dataPreview;
            decal.m_EmissiveTexture = spritePreview;
            decal.m_Mode = DecalMode::Emissive;
            decal.m_Color = { -0.5f, 1.25f, 1.5f, -0.75f };
            entity.AddComponent<TextComponent>().FontAsset = font;
            entity.AddComponent<UITextComponent>().m_FontAsset = font;
            entity.AddComponent<UIInputFieldComponent>().m_FontAsset = font;
            auto& dropdown = entity.AddComponent<UIDropdownComponent>();
            dropdown.m_FontAsset = font;
            dropdown.m_Options.Add(UIDropdownOption{ "First" });
            dropdown.m_Options.Add(UIDropdownOption{ "Second" });
            dropdown.m_SelectedIndex = 1;
            scene->CreateEntity("ExplicitOverride").AddComponent<SpriteRendererComponent>().Texture = normal;
            ASSERT_TRUE(SceneSerializer(scene).Serialize(scenePath));
        }
        const auto sceneHandle = editor->ImportAsset(scenePath);
        ASSERT_NE(sceneHandle, AssetHandle(0));

        // Stage the migrated scene itself with its real asset handles. Only its
        // four referenced texture/font sources need to travel with this probe.
        const auto sandbox = editorRoot / "SandboxProject";
        const auto driftSources = root / "Assets" / "DriftMenu";
        fs::create_directories(driftSources);
        const std::array<fs::path, 4> driftAssets = {
            "Assets/Textures/DriftMenuBackground-v2.png",
            "../assets/fonts/opensans/OpenSans-Light.ttf",
            "../assets/fonts/opensans/OpenSans-Regular.ttf",
            "../assets/fonts/opensans/OpenSans-SemiBold.ttf",
        };
        AssetRegistry authoredRegistry;
        ASSERT_TRUE(authoredRegistry.Deserialize(sandbox / "AssetRegistry.oar"));
        for (const auto& relative : driftAssets)
        {
            const auto handle = authoredRegistry.GetHandleFromPath(relative);
            auto metadata = authoredRegistry.GetMetadata(handle);
            ASSERT_TRUE(metadata.IsValid()) << relative;
            const auto staged = driftSources / relative.filename();
            ASSERT_TRUE(fs::copy_file(sandbox / relative, staged)) << relative;
            metadata.FilePath = staged.lexically_relative(root);
            ASSERT_TRUE(editor->SetMetadata(handle, metadata)) << relative;
        }
        const auto driftFile = driftSources / "DriftMenu.olo";
        ASSERT_TRUE(fs::copy_file(sandbox / "Assets/Scenes/DriftMenu.olo", driftFile));
        const auto driftHandle = editor->ImportAsset(driftFile);
        ASSERT_NE(driftHandle, AssetHandle(0));
        ASSERT_TRUE(editor->SerializeAssetRegistry());

        const auto readMip = [](const Ref<Texture2D>& texture, u32 mip, TArray64<u8>& bytes)
        {
            ASSERT_TRUE(texture && texture->GetMipLevelCount() > mip);
            ASSERT_TRUE(texture->GetData(bytes, mip));
            ASSERT_GE(bytes.Num(), 4);
        };
        std::array<TArray64<u8>, 2> previewMips;
        ASSERT_NO_FATAL_FAILURE(readMip(dataPreview, 2, previewMips[0]));
        ASSERT_NO_FATAL_FAILURE(readMip(spritePreview, 2, previewMips[1]));
        EXPECT_NEAR(previewMips[0][0], 128, 1);
        EXPECT_NEAR(previewMips[1][0], 146, 2);
        auto edited = pixels;
        for (sizet i = 0; i < edited.size(); i += 4)
            edited[i] = 96;
        ASSERT_NE(stbi_write_png(texturePath.string().c_str(), extent, extent, 4, edited.data(), extent * 4), 0);
        ASSERT_TRUE(editor->ReloadData(textureHandle));
        for (const auto& variant : { dataPreview, spritePreview })
        {
            TArray64<u8> bytes;
            ASSERT_NO_FATAL_FAILURE(readMip(variant, 2, bytes));
            EXPECT_NEAR(bytes[0], 96, 1);
        }
        ASSERT_NE(stbi_write_png(texturePath.string().c_str(), extent, extent, 4, pixels.data(), extent * 4), 0);
        ASSERT_TRUE(editor->ReloadData(textureHandle));

        // Close the producing manager. A new manager's first request is
        // linear, the reverse of the actual inspector drop order above.
        editor->Shutdown();
        editor.Reset();
        editor = Ref<EditorAssetManager>::Create();
        editor->Initialize(false);
        Project::SetAssetManager(editor);
        const auto reopenedData = editor->GetSceneTexture(textureHandle, false).As<Texture2D>();
        ASSERT_TRUE(reopenedData && !reopenedData->GetSpecification().SRGB);
        EXPECT_NE(reopenedData, dataPreview);

        const auto assertDriftAssets = [&](const Ref<Scene>& scene)
        {
            ASSERT_TRUE(scene);
            auto backdrop = scene->FindEntityByName("Menu Backdrop");
            ASSERT_TRUE(backdrop && backdrop.HasComponent<UIImageComponent>());
            const auto& image = backdrop.GetComponent<UIImageComponent>().m_Texture;
            ASSERT_TRUE(image && image->IsLoaded());
            EXPECT_EQ(image->GetHandle(), authoredRegistry.GetHandleFromPath(driftAssets[0]));
            constexpr std::array names{ "Drift Title", "Menu Help", "Drift Subtitle" };
            for (sizet i = 0; i < names.size(); ++i)
            {
                SCOPED_TRACE(names[i]);
                auto text = scene->FindEntityByName(names[i]);
                ASSERT_TRUE(text && text.HasComponent<UITextComponent>());
                const auto& textFont = text.GetComponent<UITextComponent>().m_FontAsset;
                ASSERT_TRUE(textFont && textFont->IsLoaded());
                EXPECT_EQ(textFont->GetHandle(), authoredRegistry.GetHandleFromPath(driftAssets[i + 1]));
            }
        };

        const auto assertHandles = [&](const Ref<Scene>& scene)
        {
            ASSERT_TRUE(scene);
            Entity entity = scene->FindEntityByName("HandleProbe");
            ASSERT_TRUE(entity);
            ASSERT_TRUE((entity.HasComponent<SpriteRendererComponent>() && entity.HasComponent<UIImageComponent>() &&
                         entity.HasComponent<UIPanelComponent>() && entity.HasComponent<DecalComponent>() &&
                         entity.HasComponent<TextComponent>() && entity.HasComponent<UITextComponent>() &&
                         entity.HasComponent<UIInputFieldComponent>() && entity.HasComponent<UIDropdownComponent>()));
            const auto checkTexture = [&](const Ref<Texture2D>& actual, bool srgb)
            {
                ASSERT_TRUE(actual && actual->IsLoaded());
                EXPECT_EQ(actual->GetHandle(), textureHandle);
                EXPECT_EQ(actual->GetSpecification().SRGB, srgb);
                RHI::TextureFormatInfo format;
                ASSERT_TRUE(RenderCommand::QueryTextureFormat(actual->GetRHIHandle(), 2, format));
                EXPECT_EQ(format.Neutral, srgb ? RHI::Format::RGBA8SRGB : RHI::Format::RGBA8UNorm);
                TArray64<u8> base;
                ASSERT_NO_FATAL_FAILURE(readMip(actual, 0, base));
                ASSERT_GE(base.Num(), 8);
                EXPECT_EQ(base[0], 64);
                EXPECT_EQ(base[4], 192);
                const f32 midtone = static_cast<f32>(base[0]) / 255.0f;
                EXPECT_NEAR(srgb ? ColorTransfer::SrgbToLinear(midtone) : midtone, srgb ? 0.05127f : 0.25098f, 0.00001f);
                TArray64<u8> bytes;
                ASSERT_NO_FATAL_FAILURE(readMip(actual, 2, bytes));
                ASSERT_EQ(bytes.Num(), previewMips[srgb].Num());
                EXPECT_EQ(std::memcmp(bytes.GetData(), previewMips[srgb].GetData(), bytes.Num()), 0);
            };
            const auto checkFont = [&](const Ref<Font>& actual)
            {
                ASSERT_TRUE(actual && actual->IsLoaded());
                EXPECT_EQ(actual->GetHandle(), fontHandle);
            };
            const auto sprite = entity.GetComponent<SpriteRendererComponent>().Texture;
            checkTexture(sprite, true);
            EXPECT_EQ(entity.GetComponent<UIImageComponent>().m_Texture, sprite);
            EXPECT_EQ(entity.GetComponent<UIPanelComponent>().m_BackgroundTexture, sprite);
            const auto& decal = entity.GetComponent<DecalComponent>();
            checkTexture(decal.m_AlbedoTexture, true);
            checkTexture(decal.m_NormalTexture, false);
            checkTexture(decal.m_RMATexture, false);
            checkTexture(decal.m_EmissiveTexture, true);
            EXPECT_EQ(decal.m_NormalTexture, decal.m_RMATexture);
            EXPECT_EQ(decal.m_AlbedoTexture, decal.m_EmissiveTexture);
            auto explicitOverride = scene->FindEntityByName("ExplicitOverride");
            ASSERT_TRUE(explicitOverride && explicitOverride.HasComponent<SpriteRendererComponent>());
            const auto& overridden = explicitOverride.GetComponent<SpriteRendererComponent>().Texture;
            ASSERT_TRUE(overridden && overridden->IsLoaded());
            EXPECT_EQ(overridden->GetHandle(), normalHandle);
            EXPECT_FALSE(overridden->GetSpecification().SRGB);
            EXPECT_EQ(overridden->GetMipLevelCount(), 1u);
            EXPECT_EQ(decal.m_Mode, DecalMode::Emissive);
            EXPECT_FLOAT_EQ(decal.m_Color.r, 0.0f);
            EXPECT_FLOAT_EQ(decal.m_Color.g, 1.0f);
            EXPECT_FLOAT_EQ(decal.m_Color.b, 1.0f);
            EXPECT_FLOAT_EQ(decal.m_Color.a, 0.0f);
            checkFont(entity.GetComponent<TextComponent>().FontAsset);
            checkFont(entity.GetComponent<UITextComponent>().m_FontAsset);
            checkFont(entity.GetComponent<UIInputFieldComponent>().m_FontAsset);
            const auto& dropdown = entity.GetComponent<UIDropdownComponent>();
            checkFont(dropdown.m_FontAsset);
            ASSERT_EQ(dropdown.m_Options.Num(), 2);
            EXPECT_EQ(dropdown.m_Options[0].m_Label.ToStdString(), "First");
            EXPECT_EQ(dropdown.m_Options[1].m_Label.ToStdString(), "Second");
            EXPECT_EQ(dropdown.m_SelectedIndex, 1);
        };

        {
            SCOPED_TRACE("Loose scene YAML");
            auto scene = AssetManager::GetAsset<Scene>(sceneHandle);
            ASSERT_NO_FATAL_FAILURE(assertHandles(scene));
            scene->FindEntityByName("HandleProbe").GetComponent<SpriteRendererComponent>().TilingFactor = 2.0f;
            SaveGameComponentSerializer::RegisterAll();
            const auto save = SaveGameSerializer::CaptureSceneState(*scene);
            ASSERT_FALSE(save.empty());
            auto restored = Scene::Create();
            ASSERT_TRUE(SceneSerializer(restored).Deserialize(scenePath));
            ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, save));
            ASSERT_NO_FATAL_FAILURE(assertHandles(restored));
            EXPECT_FLOAT_EQ(restored->FindEntityByName("HandleProbe").GetComponent<SpriteRendererComponent>().TilingFactor, 2.0f);
            ASSERT_NO_FATAL_FAILURE(assertDriftAssets(AssetManager::GetAsset<Scene>(driftHandle)));
        }
        {
            SCOPED_TRACE("Binary scene sidecar");
            ASSERT_TRUE(fs::is_regular_file(fs::path(scenePath).concat(".scenebin")));
            const auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(scenePath));
            ASSERT_NO_FATAL_FAILURE(assertHandles(scene));
        }

        // The saved scene is the pack snapshot. The active editor cache can
        // still hold a different reference which must not drive pack contents.
        const auto cachedScene = AssetManager::GetAsset<Scene>(sceneHandle);
        ASSERT_TRUE(cachedScene);
        cachedScene->FindEntityByName("HandleProbe").GetComponent<SpriteRendererComponent>().Texture = normal;

        AssetPackBuilder::BuildSettings settings;
        settings.m_OutputPath = root / "SceneHandles.olopack";
        settings.m_CompressAssets = true;
        settings.m_IncludeScriptModule = false;
        std::atomic<f32> progress{ 0.0f };
        {
            AssetRegistry sceneOnly;
            ASSERT_TRUE(sceneOnly.AddAsset(editor->GetAssetMetadata(sceneHandle)));
            auto incomplete = settings;
            incomplete.m_OutputPath = root / "IncompleteScene.olopack";
            const auto rejected = AssetPackBuilder::BuildFromRegistry(sceneOnly, incomplete, progress);
            EXPECT_FALSE(rejected.m_Success);
            EXPECT_NE(rejected.m_ErrorMessage.find("missing from the pack input registry"), std::string::npos);
            EXPECT_FALSE(fs::exists(incomplete.m_OutputPath));
        }
        const auto built = AssetPackBuilder::BuildFromActiveProject(settings, progress);
        ASSERT_TRUE(built.m_Success) << built.m_ErrorMessage;
        EXPECT_EQ(built.m_FailedAssetCount, 0u);
        {
            const auto record = std::ranges::find(built.m_Records, normalHandle, &AssetPackBuilder::AssetRecord::Handle);
            ASSERT_NE(record, built.m_Records.end());
            ASSERT_TRUE(record->TextureFormat);
            EXPECT_EQ(*record->TextureFormat, static_cast<u32>(ImageFormat::BC7));
            EXPECT_FALSE(record->TextureSRGB);
        }
        const auto rawPack = root / "UncompressedSceneHandles.olopack";
        settings.m_OutputPath = rawPack;
        settings.m_CompressAssets = false;
        const auto rawBuilt = AssetPackBuilder::BuildFromActiveProject(settings, progress);
        ASSERT_TRUE(rawBuilt.m_Success) << rawBuilt.m_ErrorMessage;
        EXPECT_EQ(rawBuilt.m_FailedAssetCount, 0u);
        const auto textureOnlyPack = root / "TextureOnly.olopack";
        {
            AssetRegistry textureOnly;
            ASSERT_TRUE(textureOnly.AddAsset(editor->GetAssetMetadata(textureHandle)));
            settings.m_OutputPath = textureOnlyPack;
            settings.m_CompressAssets = true;
            const auto textureOnlyBuilt = AssetPackBuilder::BuildFromRegistry(textureOnly, settings, progress);
            ASSERT_TRUE(textureOnlyBuilt.m_Success) << textureOnlyBuilt.m_ErrorMessage;
        }
        editor->Shutdown();
        editor.Reset();
        ASSERT_TRUE(fs::remove(texturePath));
        ASSERT_TRUE(fs::remove(normalPath));
        ASSERT_TRUE(fs::remove(TextureImport::SidecarPathFor(normalPath.string())));
        ASSERT_TRUE(fs::remove(fontPath));
        ASSERT_TRUE(fs::remove(scenePath));
        ASSERT_TRUE(fs::remove(driftFile));
        for (const auto& relative : driftAssets)
            ASSERT_TRUE(fs::remove(driftSources / relative.filename())) << relative;

        for (const auto& pack : { built.m_OutputPath, rawPack })
        {
            SCOPED_TRACE(pack.string());
            auto runtime = Ref<RuntimeAssetManager>::Create(false);
            Project::SetAssetManager(runtime);
            ASSERT_TRUE(runtime->LoadAssetPack(textureOnlyPack));
            const auto unflagged = runtime->GetSceneTexture(textureHandle, true).As<Texture2D>();
            ASSERT_TRUE(unflagged && unflagged->IsLoaded());
            EXPECT_FALSE(unflagged->GetSpecification().SRGB);
            ASSERT_TRUE(runtime->LoadAssetPack(pack));
            const auto variant = runtime->GetSceneTexture(textureHandle, pack == built.m_OutputPath).As<Texture2D>();
            ASSERT_TRUE(variant && variant->IsLoaded());
            EXPECT_NE(variant, unflagged);
            const auto estimated = runtime->GetAssetByteSize(textureHandle).GetBytes();
            ASSERT_TRUE(estimated);
            EXPECT_EQ(runtime->GetStreamingReport().Resident.EstimateBytes, *estimated);
            ASSERT_NO_FATAL_FAILURE(assertHandles(AssetManager::GetAsset<Scene>(sceneHandle)));
            ASSERT_NO_FATAL_FAILURE(assertDriftAssets(AssetManager::GetAsset<Scene>(driftHandle)));
            runtime->UnloadAssetPack(pack);
            const auto afterUnmount = runtime->GetSceneTexture(textureHandle, true).As<Texture2D>();
            ASSERT_TRUE(afterUnmount && afterUnmount->IsLoaded());
            EXPECT_FALSE(afterUnmount->GetSpecification().SRGB);
        }

        // An unavailable flagged pack still owns its slot source in the mounted
        // index. A canonical record in another pack must not hide the error.
        auto runtime = Ref<RuntimeAssetManager>::Create(false);
        Project::SetAssetManager(runtime);
        ASSERT_TRUE(runtime->LoadAssetPack(textureOnlyPack));
        ASSERT_TRUE(runtime->LoadAssetPack(built.m_OutputPath));
        const auto offline = root / "OfflineSceneHandles.olopack";
        fs::rename(built.m_OutputPath, offline);
        EXPECT_FALSE(runtime->GetSceneTexture(textureHandle, true));
        fs::rename(offline, built.m_OutputPath);
        EXPECT_FALSE(runtime->GetSceneTexture(textureHandle, true)); // Negative cache.
        runtime->UnloadAssetPack(built.m_OutputPath);
        ASSERT_TRUE(runtime->LoadAssetPack(built.m_OutputPath));
        const auto repaired = runtime->GetSceneTexture(textureHandle, true).As<Texture2D>();
        ASSERT_TRUE(repaired && repaired->IsLoaded());
        EXPECT_TRUE(repaired->GetSpecification().SRGB);
    }

    // Caching a failed texture resolution would lose the authored handle.
    // The scene with an empty texture slot is the control and writes a sidecar.
    TEST(AssetSceneLoad, AFailedTextureLoadWritesNoBinarySidecar)
    {
        // A failed load still builds a texture object (the raw load's, or the
        // placeholder), so this needs the GL context its neighbours need.
        OLO_ENSURE_GPU_OR_SKIP();

        OloEngine::Tests::EnsureProcessRendererInitialized();

        GLStateGuard glGuard("AssetSceneLoad.FailedTextureNoSidecar", GLStateGuard::Policy::Restore);

        const fs::path project = OloEngine::Tests::TempDir("sidecar");
        fs::create_directories(project / "Assets" / "Scenes");
        {
            std::ofstream out(project / "Sidecar.oloproj");
            out << "Project:\n"
                   "  Name: Sidecar\n"
                   "  StartScene: \"\"\n"
                   "  AssetDirectory: \"Assets\"\n"
                   "  ScriptModulePath: \"\"\n";
        }
        ASSERT_TRUE(Project::Load(project / "Sidecar.oloproj"));
        // Unloaded, not restored, as in DecalAndSpriteTextureHandlesSurviveYAMLAndSidecar:
        // a previous project can be a shut-down one, and adopting it poisons
        // every later test in the process (#1074).
        struct UnloadProject
        {
            ~UnloadProject()
            {
                Project::Unload();
            }
        } unloadProject;
        auto manager = Ref<EditorAssetManager>::Create();
        manager->Initialize(false);
        Project::SetAssetManager(manager);

        const auto writeSprite = [&project](const char* name, const char* textureHandleLine)
        {
            const auto path = project / "Assets" / "Scenes" / name;
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "Scene: Sprite\n"
                << "Version: " << SceneSerializer::CurrentVersion << "\n"
                << "Entities:\n"
                   "  - Entity: 4242\n"
                   "    TagComponent:\n"
                   "      Tag: Sprite\n"
                   "    TransformComponent:\n"
                   "      Translation: [0.0, 0.0, 0.0]\n"
                   "      Rotation: [0.0, 0.0, 0.0]\n"
                   "      Scale: [1.0, 1.0, 1.0]\n"
                   "    SpriteRendererComponent:\n"
                   "      Color: [1.0, 1.0, 1.0, 1.0]\n"
                << textureHandleLine;
            return path;
        };

        const auto missing = writeSprite("Missing.olo", "      TextureHandle: 424242\n");
        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(missing)) << "a missing texture does not fail the load";
        }
        EXPECT_FALSE(fs::exists(fs::path(missing).concat(".scenebin")))
            << "a sidecar written after a failed texture load serves the scene without it from then on";

        const auto control = writeSprite("Control.olo", "");
        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(control));
        }
        EXPECT_TRUE(fs::exists(fs::path(control).concat(".scenebin")))
            << "the same sprite with no texture reference does write a sidecar";
    }

} // namespace OloEngine::Tests
