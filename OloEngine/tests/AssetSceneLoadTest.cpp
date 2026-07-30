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

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RendererTypes.h"
#include "OloEngine/Scene/Components.h"
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

} // namespace OloEngine::Tests
