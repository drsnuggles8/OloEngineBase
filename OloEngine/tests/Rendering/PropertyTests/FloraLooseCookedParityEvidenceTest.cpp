// =============================================================================
// FloraLooseCookedParityEvidenceTest.cpp — the {loose, cooked} cell of epic
// #1224's criterion 3 ("stable across ... streaming"), issue #1392.
//
// Loads the committed Scenes/FoliageMeadowToWoodland.olo two ways and compares
// what the flora actually drew:
//
//   LOOSE   the editor's way: Sandbox.oloproj mounted, the working directory
//           OloEditor/, the scene read from the project.
//   COOKED  the shipped game's way: a game directory laid out by the real
//           GameBuildPipeline::StageRuntimeContent, a project mounted at it
//           exactly as OloRuntime's MountGameProject does, the game directory
//           as the working directory, and a RuntimeAssetManager with NO pack.
//           The scene holds no asset handles, so an empty pack is stricter than
//           the real one, not weaker: nothing the flora draws may come from it.
//
// ── Why every assertion here is about a SILENT downgrade ───────────────────
//
// Every flora asset has a designed fallback and each renders something
// plausible. A MeshPath that does not resolve draws the flat card; the impostor
// is baked from that mesh, so it is never baked either; a mesh part whose .mtl
// texture is missing draws the CARD's albedo (FoliageRenderer's part fallback);
// a leaf map that will not open shades with the authored constant. None of it
// crashes. Before this file's fix, GameBuildPipeline shipped no .obj or .mtl at
// all (only the pack, the scenes and loose PNGs), so every one of those
// fallbacks fired in a packaged game and it said so only in the log.
//
// So the census compares, per layer, off the REAL draw stream and the
// renderer's own record of what each surface sampled:
//   1. plants placed, draw entries, instances submitted;
//   2. an IsAuthoredMesh draw exists where the layer asks for one;
//   3. the impostor atlas exists and rides the draw where UseImpostor is set;
//   4. each authored leaf map rides the draw (a failed load drops the handle);
//   plus the file each card / mesh part / impostor-bake surface sampled and
//   whether it decoded (FoliageRenderer::GetAlphaCoverage), which is what sees
//   the .mtl fallback that (2) cannot.
//   5. pixels at a near (authored mesh) and a far (impostor) pose, on every GL
//      rendering path, against a loose-vs-loose control for the noise floor.
//
// Each assertion has a planted negative control against the cooked game
// (PlantedDowngradesTurnTheMatchingAssertionRed): a check that cannot fail
// proves nothing here.
//
// Writes
//   OloEditor/assets/tests/visual/FloraLoose_GL_<Path>_<Pose>.png
//   OloEditor/assets/tests/visual/FloraCooked_GL_<Path>_<Pose>.png
//   OloEditor/assets/tests/visual/FloraCookedNoPineMesh_GL_Deferred_<Pose>.png
// for <Path> in {Forward, ForwardPlus, Deferred} and <Pose> in {Near, Far}.
// Vulkan is not reachable from a headless fixture; those cells are a live
// OloRuntime session, recorded in the PR.
//
// Runs in the normal suite and SKIPs cleanly without a GL 4.6 context.
//
// OLO_TEST_LAYER: L8
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
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

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;
        // Wind and the interaction spring are functions of the scene clock; a
        // frozen clock is what makes two loads of one scene pixel-comparable.
        constexpr f32 kCaptureTime = 4.0f;
        // Frames per capture. The first generates the flora and bakes the
        // impostors; the rest let temporal history settle to THIS arm's frame.
        constexpr u32 kFramesPerCapture = 6;
        // The fraction of pixels cooked may differ from loose by when the
        // loose-vs-loose control differs by nothing. The planted missing mesh
        // must clear twice this, so the pixel check is shown able to fail.
        constexpr f64 kPixelAllowance = 0.001;

        // Asset-directory-relative, so it names the scene in both the project
        // and the game directory's Scenes/ copy.
        const fs::path kSceneAssetPath = fs::path("Scenes") / "FoliageMeadowToWoodland.olo";

        [[nodiscard]] fs::path EditorRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT };
        }

        [[nodiscard]] fs::path SandboxProjectFile()
        {
            return EditorRoot() / "SandboxProject" / "Sandbox.oloproj";
        }

        // Same threshold as FloraTraversalEvidenceTest's coverage figures.
        [[nodiscard]] f64 DifferingFraction(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 1.0;
            sizet differing = 0;
            sizet total = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                ++total;
                const int dr = std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (dr + dg + db > 24)
                    ++differing;
            }
            return total == 0 ? 1.0 : static_cast<f64>(differing) / static_cast<f64>(total);
        }

        [[nodiscard]] const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
            }
            return "Unknown";
        }

        void WriteEvidencePng(const std::string& name, const std::vector<u8>& bottomUp)
        {
            std::vector<u8> flipped(bottomUp); // the composite reads back bottom-up
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* a = flipped.data() + static_cast<sizet>(y) * rowBytes;
                u8* b = flipped.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes;
                std::vector<u8> tmp(a, a + rowBytes);
                std::memcpy(a, b, rowBytes);
                std::memcpy(b, tmp.data(), rowBytes);
            }
            // Absolute: the cooked arm runs with the game directory as the cwd.
            const fs::path dir = EditorRoot() / "assets" / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ::stbi_write_png((dir / name).string().c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             flipped.data(), static_cast<int>(rowBytes));
        }

        // ── The census ──────────────────────────────────────────────────────

        struct LayerCensus
        {
            std::string Name;
            u32 Placed = 0;
            u32 DrawEntries = 0;
            u32 Instances = 0;
            bool WantsMesh = false;
            bool DrawsMesh = false;
            bool WantsImpostor = false;
            bool ImpostorBound = false;
            std::array<bool, 3> WantsLeafMap{};
            std::array<bool, 3> LeafMapBound{};
            // Draws that carry no albedo texture. A card whose AlbedoPath does
            // not resolve draws white; the pines' card draws in the band between
            // the mesh and the impostor, where no surface census looks.
            bool WantsAlbedo = false;
            u32 DrawsWithoutAlbedo = 0;
            // "<surface> <- <file sampled>", plus " [undecoded]" when the
            // texture would not decode. Sorted.
            std::vector<std::string> Surfaces;
        };
        using Census = std::vector<LayerCensus>;

        constexpr std::array<const char*, 3> kLeafMapNames{ "normal", "roughness", "thickness" };

        [[nodiscard]] Census TakeCensus(const FoliageComponent& foliage)
        {
            Census census;
            const FoliageRenderer& renderer = *foliage.m_Renderer;
            const auto draws = renderer.GetActiveLayerDrawInfo();
            census.resize(static_cast<sizet>(foliage.m_Layers.Num()));

            for (u32 i = 0; i < static_cast<u32>(census.size()); ++i)
            {
                const FoliageLayer& layer = foliage.m_Layers[static_cast<i32>(i)];
                LayerCensus& c = census[i];
                c.Name = layer.Name.ToStdString();
                c.WantsMesh = layer.Enabled && layer.UseAuthoredMesh && !layer.MeshPath.IsEmpty();
                c.WantsImpostor = layer.Enabled && layer.UseImpostor && !layer.MeshPath.IsEmpty();
                c.WantsLeafMap = { layer.Enabled && !layer.NormalMapPath.IsEmpty(),
                                   layer.Enabled && !layer.RoughnessMapPath.IsEmpty(),
                                   layer.Enabled && !layer.ThicknessMapPath.IsEmpty() };
                c.WantsAlbedo = layer.Enabled && !layer.AlbedoPath.IsEmpty();
                c.ImpostorBound = renderer.GetImpostorAtlas(i) != nullptr;

                for (const auto& entry : renderer.GetAlphaCoverage(i))
                {
                    const std::string file = fs::path(entry.Texture.ToStdString()).filename().string();
                    c.Surfaces.push_back(entry.Surface.ToStdString() + " <- " + (file.empty() ? "(none)" : file) +
                                         (entry.Measured ? "" : " [undecoded]"));
                }
                std::ranges::sort(c.Surfaces);
            }

            for (const auto& record : renderer.GetInstanceRegistry().GetRecords())
            {
                if (record.m_LayerIndex < census.size())
                    ++census[record.m_LayerIndex].Placed;
            }

            std::vector<bool> impostorOnDraw(census.size(), false);
            for (const auto& draw : draws)
            {
                if (draw.LayerIndex >= census.size())
                    continue;
                LayerCensus& c = census[draw.LayerIndex];
                ++c.DrawEntries;
                c.Instances = std::max(c.Instances, draw.InstanceCount);
                c.DrawsMesh = c.DrawsMesh || draw.IsAuthoredMesh;
                if (draw.UseImpostor && draw.ImpostorAlbedoAtlasID.IsValid())
                    impostorOnDraw[draw.LayerIndex] = true;
                c.LeafMapBound[0] = c.LeafMapBound[0] || draw.LeafNormalTextureID.IsValid();
                c.LeafMapBound[1] = c.LeafMapBound[1] || draw.LeafRoughnessTextureID.IsValid();
                c.LeafMapBound[2] = c.LeafMapBound[2] || draw.LeafThicknessTextureID.IsValid();
                if (!draw.AlbedoTextureID.IsValid())
                    ++c.DrawsWithoutAlbedo;
            }
            // Baked AND riding the draw: an atlas the draw does not carry is
            // the same downgrade as no atlas.
            for (sizet i = 0; i < census.size(); ++i)
                census[i].ImpostorBound = census[i].ImpostorBound && impostorOnDraw[i];
            return census;
        }

        // What the scene authored and the arm did NOT deliver. Run on the loose
        // arm: if loose itself is downgraded, parity with it proves nothing.
        [[nodiscard]] std::vector<std::string> AuthoredButNotDelivered(const Census& census)
        {
            std::vector<std::string> out;
            for (const auto& c : census)
            {
                const std::string layer = "layer '" + c.Name + "': ";
                if (c.Placed > 0 && (c.DrawEntries == 0 || c.Instances == 0))
                    out.push_back(layer + "placed " + std::to_string(c.Placed) + " plants and draws none");
                if (c.WantsMesh && !c.DrawsMesh)
                    out.push_back(layer + "asks for its authored mesh and no draw carries one");
                if (c.WantsImpostor && !c.ImpostorBound)
                    out.push_back(layer + "asks for an impostor and no atlas is bound");
                if (c.WantsAlbedo && c.DrawsWithoutAlbedo > 0)
                    out.push_back(layer + "authors an albedo and " + std::to_string(c.DrawsWithoutAlbedo) +
                                  " draw(s) carry none");
                for (sizet m = 0; m < kLeafMapNames.size(); ++m)
                {
                    if (c.WantsLeafMap[m] && !c.LeafMapBound[m])
                        out.push_back(layer + "authors a " + kLeafMapNames[m] + " map and no draw carries it");
                }
                for (const auto& surface : c.Surfaces)
                {
                    if (surface.ends_with("[undecoded]"))
                        out.push_back(layer + "surface did not decode: " + surface);
                }
            }
            return out;
        }

        // Every way `cooked` differs from `loose`. Empty is parity.
        [[nodiscard]] std::vector<std::string> CompareCensus(const Census& loose, const Census& cooked)
        {
            std::vector<std::string> out;
            if (loose.size() != cooked.size())
            {
                out.push_back("layer count " + std::to_string(loose.size()) + " loose vs " +
                              std::to_string(cooked.size()) + " cooked");
                return out;
            }
            const auto differ = [&out](const LayerCensus& l, const std::string& what, const std::string& a,
                                       const std::string& b)
            {
                if (a != b)
                    out.push_back("layer '" + l.Name + "': " + what + " " + a + " loose vs " + b + " cooked");
            };
            const auto yesNo = [](bool v) { return std::string(v ? "yes" : "no"); };
            for (sizet i = 0; i < loose.size(); ++i)
            {
                const LayerCensus& l = loose[i];
                const LayerCensus& c = cooked[i];
                differ(l, "plants placed", std::to_string(l.Placed), std::to_string(c.Placed));
                differ(l, "draw entries", std::to_string(l.DrawEntries), std::to_string(c.DrawEntries));
                differ(l, "instances submitted", std::to_string(l.Instances), std::to_string(c.Instances));
                differ(l, "authored mesh drawn", yesNo(l.DrawsMesh), yesNo(c.DrawsMesh));
                differ(l, "impostor atlas bound", yesNo(l.ImpostorBound), yesNo(c.ImpostorBound));
                differ(l, "draws without albedo", std::to_string(l.DrawsWithoutAlbedo),
                       std::to_string(c.DrawsWithoutAlbedo));
                for (sizet m = 0; m < kLeafMapNames.size(); ++m)
                    differ(l, std::string(kLeafMapNames[m]) + " map bound", yesNo(l.LeafMapBound[m]),
                           yesNo(c.LeafMapBound[m]));
                std::string ls;
                std::string cs;
                for (const auto& s : l.Surfaces)
                    ls += "\n      " + s;
                for (const auto& s : c.Surfaces)
                    cs += "\n      " + s;
                differ(l, "surfaces sampled", ls, cs);
            }
            return out;
        }

        [[nodiscard]] std::string Join(const std::vector<std::string>& lines)
        {
            std::string out;
            for (const auto& line : lines)
                out += "\n  " + line;
            return out;
        }

        struct CameraPose
        {
            std::string Name;
            glm::vec3 Eye{ 0.0f };
            f32 Yaw = 0.0f;
            f32 Pitch = 0.0f;
        };

        // Every renderer-global the test moves, put back on every exit path
        // (docs/agent-rules/cross-test-renderer-state.md).
        struct ScopedRenderPath
        {
            ScopedRenderPath() : m_Path(Renderer3D::GetRendererSettings().Path) {}
            ~ScopedRenderPath()
            {
                Set(m_Path);
            }
            ScopedRenderPath(const ScopedRenderPath&) = delete;
            auto operator=(const ScopedRenderPath&) -> ScopedRenderPath& = delete;
            static void Set(RenderingPath path)
            {
                Renderer3D::GetRendererSettings().Path = path;
                Renderer3D::ApplyRendererSettings();
            }
            RenderingPath m_Path;
        };

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
            ScopedMockTime(const ScopedMockTime&) = delete;
            auto operator=(const ScopedMockTime&) -> ScopedMockTime& = delete;
        };

        // Moves a staged game file out of the way for one planted downgrade
        // and puts it back on every exit path.
        struct ScopedHiddenFile
        {
            explicit ScopedHiddenFile(fs::path path) : m_Path(std::move(path)), m_Hidden(m_Path)
            {
                m_Hidden += ".planted-missing";
                std::error_code ec;
                fs::rename(m_Path, m_Hidden, ec);
                m_Ok = !ec;
            }
            ~ScopedHiddenFile()
            {
                if (m_Ok)
                {
                    std::error_code ec;
                    fs::rename(m_Hidden, m_Path, ec);
                }
            }
            ScopedHiddenFile(const ScopedHiddenFile&) = delete;
            auto operator=(const ScopedHiddenFile&) -> ScopedHiddenFile& = delete;
            fs::path m_Path;
            fs::path m_Hidden;
            bool m_Ok = false;
        };
    } // namespace

    class FloraLooseCookedParityEvidenceTest : public RendererAttachedTest
    {
      protected:
        enum class Source
        {
            Loose,
            Cooked
        };

        // One loaded scene and where its content resolves from.
        struct Arm
        {
            Source From = Source::Loose;
            Ref<Scene> SceneRef;
            Entity Foliage;
        };

        void BuildScene() override
        {
            if (!RenderPropertyFixture::IsGpuAvailable())
                return;
            // The fixture's own scene stays empty; every arm owns its scene and
            // is driven through RunEditorFramesOn. This sizes the render graph.
            EnableRendering(kWidth, kHeight);
        }

        void SetUp() override
        {
            m_PreviousProject = Project::GetActive();
            m_PreviousAssetManager = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            std::error_code ec;
            m_PreviousCwd = fs::current_path(ec);
            RendererAttachedTest::SetUp();
        }

        void TearDown() override
        {
            m_Arms.clear();
            RendererAttachedTest::TearDown();
            if (m_CookedAssetManager)
                m_CookedAssetManager->Shutdown();
            m_CookedAssetManager.Reset();
            RestoreContext();
        }

        // ── Where content resolves from ────────────────────────────────────

        void RestoreContext()
        {
            std::error_code ec;
            if (!m_PreviousCwd.empty())
                fs::current_path(m_PreviousCwd, ec);
            if (m_PreviousProject)
                Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
            else
                Project::Unload();
            if (m_PreviousAssetManager)
                Project::SetAssetManager(m_PreviousAssetManager);
        }

        // The editor: the real project file, the editor working directory.
        void EnterLoose()
        {
            std::error_code ec;
            fs::current_path(EditorRoot(), ec);
            ASSERT_FALSE(ec) << "cannot enter the editor root: " << ec.message();
            // Unload first: Load replaces only the project, and the cooked
            // arm's RuntimeAssetManager must not serve the loose one.
            Project::Unload();
            ASSERT_TRUE(Project::Load(SandboxProjectFile())) << SandboxProjectFile().string();
            if (m_PreviousAssetManager)
                Project::SetAssetManager(m_PreviousAssetManager);
        }

        // OloRuntime: MountGameProject's in-memory project at the game
        // directory, which is also the working directory, and a
        // RuntimeAssetManager with no pack loaded.
        void EnterCooked()
        {
            ProjectConfig config;
            config.Name = "FloraParityGame";
            config.AssetDirectory = "Assets";
            Project::NewInMemory(m_GameRoot, config);
            if (!m_CookedAssetManager)
                m_CookedAssetManager = Ref<RuntimeAssetManager>::Create(/*autoLoadDefaultPack=*/false);
            Project::SetAssetManager(m_CookedAssetManager);
            std::error_code ec;
            fs::current_path(m_GameRoot, ec);
            ASSERT_FALSE(ec) << "cannot enter the game directory: " << ec.message();
        }

        void Enter(Source from)
        {
            if (from == Source::Loose)
                EnterLoose();
            else
                EnterCooked();
        }

        // Lays the game directory out once per process through the pipeline's
        // own staging, from the editor's context.
        void StageCookedGame()
        {
            static fs::path s_Staged;
            if (!s_Staged.empty() && fs::exists(s_Staged / kSceneAssetPath.parent_path()))
            {
                m_GameRoot = s_Staged;
                return;
            }

            std::error_code ec;
            ASSERT_TRUE(fs::equivalent(fs::current_path(ec), EditorRoot(), ec))
                << "the suite does not run from OloEditor/, which the pipeline's engine-resource copy assumes";
            ASSERT_NO_FATAL_FAILURE(EnterLoose());

            const fs::path root = TempRoot() / "FloraParityGame";
            fs::remove_all(root, ec);
            fs::create_directories(root, ec);
            ASSERT_FALSE(ec) << ec.message();

            std::string error;
            ASSERT_TRUE(GameBuildPipeline::StageRuntimeContent(root, error)) << error;
            RestoreContext();

            s_Staged = root;
            m_GameRoot = root;
        }

        // CopySceneFiles keeps the asset-relative path under Scenes/, so the
        // shipped copy is Scenes/Scenes/<name>; OloRuntime's FindStartScene
        // finds it there.
        [[nodiscard]] fs::path CookedScenePath() const
        {
            return m_GameRoot / "Scenes" / kSceneAssetPath;
        }

        // A fresh copy of the committed scene outside the working tree, so the
        // .scenebin sidecar Deserialize writes lands in temp. Where the .olo
        // sits does not change where its content resolves: that is the
        // project's and the working directory's business.
        [[nodiscard]] fs::path FreshLooseSceneCopy(const std::string& tag)
        {
            const fs::path source = EditorRoot() / "SandboxProject" / "Assets" / kSceneAssetPath;
            const fs::path copy = TempDir("loose-" + tag) / kSceneAssetPath.filename();
            std::error_code ec;
            fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec);
            EXPECT_FALSE(ec) << "cannot copy " << source.string() << ": " << ec.message();
            return copy;
        }

        // Deserialize and tick until the flora exists. The arm's content root
        // must be current for the whole tick: generation is what opens the
        // meshes, textures and leaf maps and bakes the impostors.
        [[nodiscard]] Arm* LoadArm(Source from, const fs::path& sceneFile)
        {
            Enter(from);
            if (::testing::Test::HasFatalFailure())
                return nullptr;

            Arm arm;
            arm.From = from;
            arm.SceneRef = Scene::Create();
            SceneSerializer serializer(arm.SceneRef);
            EXPECT_TRUE(serializer.Deserialize(sceneFile)) << sceneFile.string();

            for (auto entity : arm.SceneRef->GetAllEntitiesWith<FoliageComponent>())
                arm.Foliage = Entity{ entity, arm.SceneRef.get() };
            if (!arm.Foliage)
            {
                ADD_FAILURE() << sceneFile.string() << " has no FoliageComponent";
                return nullptr;
            }

            arm.SceneRef->SetIs3DModeEnabled(true);
            arm.SceneRef->OnViewportResize(kWidth, kHeight);
            arm.SceneRef->SetRenderingEnabled(true);

            // Anywhere will do: generation covers the whole terrain.
            EditorCamera camera = MakeCamera(CameraPose{ "warm-up", glm::vec3(128.0f, 60.0f, 300.0f), 0.0f, 0.3f });
            RunEditorFramesOn(*arm.SceneRef, camera, 2);

            const auto& foliage = arm.Foliage.GetComponent<FoliageComponent>();
            if (!foliage.m_Renderer || foliage.m_Renderer->GetTotalInstanceCount() == 0u)
            {
                ADD_FAILURE() << sceneFile.string() << " generated no flora at all";
                return nullptr;
            }
            m_Arms.push_back(std::move(arm));
            return &m_Arms.back();
        }

        [[nodiscard]] static EditorCamera MakeCamera(const CameraPose& pose)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(pose.Eye, pose.Yaw, pose.Pitch);
            return camera;
        }

        // The same frame sequence for every arm, so what differs between two
        // captures is the content and not the history.
        void Capture(Arm& arm, const CameraPose& pose, std::vector<u8>& out, bool foliageEnabled = true)
        {
            Enter(arm.From);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            auto& foliage = arm.Foliage.GetComponent<FoliageComponent>();
            foliage.m_Enabled = foliageEnabled;
            Renderer3D::ResetFrameSequences();
            RunEditorFramesOn(*arm.SceneRef, MakeCamera(pose), kFramesPerCapture);
            foliage.m_Enabled = true;

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(out, width, height)) << "no composite for pose " << pose.Name;
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);
        }

        // Near: inside the pines' authored-mesh range. Far: past their
        // impostor hand-over, looking at the same stand. Derived from where the
        // pines actually are, so a re-seeded terrain moves the poses with it.
        [[nodiscard]] std::optional<std::array<CameraPose, 2>> DerivePoses(const Arm& arm, u32 pineLayer) const
        {
            const auto& foliage = arm.Foliage.GetComponent<FoliageComponent>();
            const auto& records = foliage.m_Renderer->GetInstanceRegistry().GetRecords();

            // The fullest 16 m bucket of pines, not the mean: a band over
            // procedural terrain is routinely multi-modal.
            constexpr u32 kGrid = 16u;
            constexpr f32 kWorld = 256.0f;
            std::vector<u32> counts(kGrid * kGrid, 0u);
            std::vector<glm::dvec3> sums(kGrid * kGrid, glm::dvec3(0.0));
            for (const auto& record : records)
            {
                if (record.m_LayerIndex != pineLayer)
                    continue;
                const i32 cx = static_cast<i32>(record.m_Position.x / (kWorld / kGrid));
                const i32 cz = static_cast<i32>(record.m_Position.z / (kWorld / kGrid));
                if (cx < 0 || cz < 0 || cx >= static_cast<i32>(kGrid) || cz >= static_cast<i32>(kGrid))
                    continue;
                const sizet bucket = static_cast<sizet>(cz) * kGrid + static_cast<sizet>(cx);
                ++counts[bucket];
                sums[bucket] += glm::dvec3(record.m_Position);
            }
            const auto best = std::ranges::max_element(counts);
            if (best == counts.end() || *best == 0u)
                return std::nullopt;
            const sizet bucket = static_cast<sizet>(std::distance(counts.begin(), best));
            const glm::vec3 stand = glm::vec3(sums[bucket] / static_cast<f64>(*best));

            // Back away from the stand toward the terrain's middle, so the near
            // pose stands on the terrain and the far pose looks across it.
            glm::vec3 away = glm::vec3(kWorld * 0.5f, 0.0f, kWorld * 0.5f) - stand;
            away.y = 0.0f;
            away = glm::length(away) > 1.0f ? glm::normalize(away) : glm::vec3(0.0f, 0.0f, 1.0f);

            const auto look = [&](const std::string& name, f32 distance, f32 height)
            {
                const glm::vec3 target = stand + glm::vec3(0.0f, 4.0f, 0.0f);
                const glm::vec3 eye = stand + away * distance + glm::vec3(0.0f, height, 0.0f);
                const glm::vec3 toTarget = target - eye;
                const f32 horizontal = std::max(glm::length(glm::vec2(toTarget.x, toTarget.z)), 1e-3f);
                CameraPose pose;
                pose.Name = name;
                pose.Eye = eye;
                pose.Yaw = std::atan2(-toTarget.x, -toTarget.z); // EditorCamera: yaw 0 looks along -Z
                pose.Pitch = std::atan2(-toTarget.y, horizontal); // positive pitch looks DOWN
                return pose;
            };
            // 30 m is well inside the pines' 52-70 m mesh band; 160 m is past
            // the 110 m + 30 m impostor hand-over, and no further, because the
            // scene has only a few dozen pines and every metre shrinks them.
            return std::array<CameraPose, 2>{ look("Near", 30.0f, 6.0f), look("Far", 160.0f, 35.0f) };
        }

        [[nodiscard]] static std::optional<u32> ImpostorLayer(const FoliageComponent& foliage)
        {
            for (i32 i = 0; i < foliage.m_Layers.Num(); ++i)
            {
                if (foliage.m_Layers[i].Enabled && foliage.m_Layers[i].UseImpostor)
                    return static_cast<u32>(i);
            }
            return std::nullopt;
        }

        static void LogCensus(const char* label, const Census& census)
        {
            for (const auto& c : census)
            {
                GTEST_LOG_(INFO) << label << " '" << c.Name << "': " << c.Placed << " placed, " << c.DrawEntries
                                 << " draw entries, " << c.Instances << " instances; mesh "
                                 << (c.DrawsMesh ? "yes" : "no") << ", impostor " << (c.ImpostorBound ? "yes" : "no")
                                 << ", leaf maps " << c.LeafMapBound[0] << c.LeafMapBound[1] << c.LeafMapBound[2]
                                 << ", " << c.Surfaces.size() << " surfaces";
            }
        }

        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssetManager;
        Ref<RuntimeAssetManager> m_CookedAssetManager;
        fs::path m_PreviousCwd;
        fs::path m_GameRoot;
        // A deque, because LoadArm hands out Arm* and a later push_back must
        // not move the arms already loaded.
        std::deque<Arm> m_Arms;
    };

    // ── Items 1-4: the census, loose vs cooked, first and second launch ─────

    TEST_F(FloraLooseCookedParityEvidenceTest, CookedGameDrawsEveryFloraAssetTheLooseProjectDraws)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedRenderPath restorePath;
        ScopedRenderPath::Set(RenderingPath::Deferred);
        const ScopedMockTime mockTime(kCaptureTime);

        ASSERT_NO_FATAL_FAILURE(StageCookedGame());

        Arm* loose = LoadArm(Source::Loose, FreshLooseSceneCopy("census"));
        ASSERT_NE(loose, nullptr);
        const Census looseCensus = TakeCensus(loose->Foliage.GetComponent<FoliageComponent>());
        LogCensus("loose", looseCensus);

        // The baseline has to be the scene as authored, or matching it is
        // vacuous: a loose project with a broken MeshPath and a cooked game
        // with the same broken MeshPath agree perfectly.
        const auto looseGaps = AuthoredButNotDelivered(looseCensus);
        ASSERT_TRUE(looseGaps.empty()) << "the LOOSE scene is itself downgraded:" << Join(looseGaps);
        ASSERT_TRUE(std::ranges::any_of(looseCensus, [](const LayerCensus& c) { return c.WantsMesh; }))
            << "no layer asks for an authored mesh — item 2 would pass vacuously";
        ASSERT_TRUE(std::ranges::any_of(looseCensus, [](const LayerCensus& c) { return c.WantsImpostor; }))
            << "no layer asks for an impostor — item 3 would pass vacuously";
        ASSERT_TRUE(std::ranges::any_of(looseCensus, [](const LayerCensus& c)
                                        { return c.WantsLeafMap[0] && c.WantsLeafMap[1] && c.WantsLeafMap[2]; }))
            << "no layer authors all three leaf maps — item 4 would pass vacuously";

        // First launch of the shipped game: the scene is parsed from YAML and
        // a .scenebin sidecar is written beside it.
        std::error_code ec;
        fs::path sidecar = CookedScenePath();
        sidecar += ".scenebin"; // SceneSerializer names it <scene>.olo.scenebin
        fs::remove(sidecar, ec);
        Arm* cooked = LoadArm(Source::Cooked, CookedScenePath());
        ASSERT_NE(cooked, nullptr);
        const Census cookedCensus = TakeCensus(cooked->Foliage.GetComponent<FoliageComponent>());
        LogCensus("cooked", cookedCensus);
        const auto firstLaunch = CompareCensus(looseCensus, cookedCensus);
        EXPECT_TRUE(firstLaunch.empty()) << "the cooked game (first launch, YAML) differs from loose:"
                                         << Join(firstLaunch);

        // Second launch reads the sidecar instead: a different deserializer
        // over the same content, and the one every launch after the first uses.
        ASSERT_TRUE(fs::is_regular_file(sidecar, ec))
            << "the first cooked load wrote no " << sidecar.filename().string()
            << " — the second-launch arm would re-test the YAML path";
        Arm* relaunch = LoadArm(Source::Cooked, CookedScenePath());
        ASSERT_NE(relaunch, nullptr);
        const auto secondLaunch =
            CompareCensus(looseCensus, TakeCensus(relaunch->Foliage.GetComponent<FoliageComponent>()));
        EXPECT_TRUE(secondLaunch.empty()) << "the cooked game (second launch, .scenebin) differs from loose:"
                                          << Join(secondLaunch);
    }

    // ── Item 5: pixels, near and far, on every GL rendering path ────────────

    TEST_F(FloraLooseCookedParityEvidenceTest, CookedFloraMatchesLoosePixelsOnEveryGLPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedRenderPath restorePath;
        ScopedRenderPath::Set(RenderingPath::Deferred);
        const ScopedMockTime mockTime(kCaptureTime);

        ASSERT_NO_FATAL_FAILURE(StageCookedGame());
        Arm* loose = LoadArm(Source::Loose, FreshLooseSceneCopy("pixels"));
        ASSERT_NE(loose, nullptr);
        // The control: a second, independent load of the same loose scene. What
        // it differs from the first by is the noise floor; cooked may differ
        // from loose by no more than that.
        Arm* control = LoadArm(Source::Loose, FreshLooseSceneCopy("pixels-control"));
        ASSERT_NE(control, nullptr);
        Arm* cooked = LoadArm(Source::Cooked, CookedScenePath());
        ASSERT_NE(cooked, nullptr);
        // Drawn with its flora switched off, for the coverage baseline. Its own
        // arm, because switching an arm's flora off changes its later frames.
        Arm* bare = LoadArm(Source::Loose, FreshLooseSceneCopy("pixels-bare"));
        ASSERT_NE(bare, nullptr);

        const auto pineLayer = ImpostorLayer(loose->Foliage.GetComponent<FoliageComponent>());
        ASSERT_TRUE(pineLayer.has_value()) << "no impostor layer to frame";
        const auto poses = DerivePoses(*loose, *pineLayer);
        ASSERT_TRUE(poses.has_value()) << "the impostor layer placed no plant on the terrain";

        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            ScopedRenderPath::Set(path);
            for (const auto& pose : *poses)
            {
                SCOPED_TRACE(std::string(PathName(path)) + " / " + pose.Name);
                std::vector<u8> looseFrame;
                std::vector<u8> controlFrame;
                std::vector<u8> cookedFrame;
                std::vector<u8> bareFrame;
                // The three compared arms see the same camera history, frame
                // for frame. The flora's LOD hysteresis remembers the previous
                // frame's distance, so an arm with a different history draws a
                // different frame: taking the foliage-off baseline on the loose
                // arm itself put it 6-8% of the frame away from the control and
                // the cooked arm, which agreed with each other exactly.
                ASSERT_NO_FATAL_FAILURE(Capture(*loose, pose, looseFrame));
                ASSERT_NO_FATAL_FAILURE(Capture(*control, pose, controlFrame));
                ASSERT_NO_FATAL_FAILURE(Capture(*cooked, pose, cookedFrame));
                ASSERT_NO_FATAL_FAILURE(Capture(*bare, pose, bareFrame, /*foliageEnabled=*/false));

                const std::string cell = std::string("_GL_") + PathName(path) + "_" + pose.Name + ".png";
                WriteEvidencePng("FloraLoose" + cell, looseFrame);
                WriteEvidencePng("FloraCooked" + cell, cookedFrame);

                const f64 coverage = DifferingFraction(looseFrame, bareFrame);
                const f64 noise = DifferingFraction(looseFrame, controlFrame);
                const f64 drift = DifferingFraction(looseFrame, cookedFrame);
                GTEST_LOG_(INFO) << PathName(path) << " " << pose.Name << ": flora covers " << coverage * 100.0
                                 << "% of the frame; loose-vs-loose " << noise * 100.0 << "%, loose-vs-cooked "
                                 << drift * 100.0 << "%";

                EXPECT_GT(coverage, 0.02) << "the flora covers almost none of the frame — a parity of empty frames";
                // The floor is the control's own spread, with a small absolute
                // allowance for a control that happens to land on zero.
                EXPECT_LE(noise, kPixelAllowance)
                    << "two loads of the same loose scene draw differently — the control has no floor to measure "
                       "cooked against";
                EXPECT_LE(drift, std::max(3.0 * noise, kPixelAllowance))
                    << "cooked flora draws visibly differently from loose at this pose";
            }
        }
    }

    // ── Every assertion above, planted against the cooked game ──────────────

    TEST_F(FloraLooseCookedParityEvidenceTest, PlantedDowngradesTurnTheMatchingAssertionRed)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedRenderPath restorePath;
        ScopedRenderPath::Set(RenderingPath::Deferred);
        const ScopedMockTime mockTime(kCaptureTime);

        ASSERT_NO_FATAL_FAILURE(StageCookedGame());
        Arm* loose = LoadArm(Source::Loose, FreshLooseSceneCopy("planted"));
        ASSERT_NE(loose, nullptr);
        const Census looseCensus = TakeCensus(loose->Foliage.GetComponent<FoliageComponent>());
        const auto pineLayer = ImpostorLayer(loose->Foliage.GetComponent<FoliageComponent>());
        ASSERT_TRUE(pineLayer.has_value());
        const auto poses = DerivePoses(*loose, *pineLayer);
        ASSERT_TRUE(poses.has_value());
        const std::string pineName = looseCensus[*pineLayer].Name;

        // The comparator alone: each field flipped by itself must be reported
        // by itself. This is what shows the impostor check is not merely the
        // mesh check again — the two move together in every real downgrade,
        // because the atlas is baked from the mesh.
        {
            const auto flipOne = [&](const std::function<void(LayerCensus&)>& flip)
            {
                Census mutated = looseCensus;
                flip(mutated[*pineLayer]);
                return CompareCensus(looseCensus, mutated);
            };
            const auto reportsOnly = [](const std::vector<std::string>& found, const std::string& needle)
            {
                return found.size() == 1u && found.front().find(needle) != std::string::npos;
            };
            EXPECT_TRUE(reportsOnly(flipOne([](LayerCensus& c) { c.Placed += 1; }), "plants placed"));
            EXPECT_TRUE(reportsOnly(flipOne([](LayerCensus& c) { c.DrawEntries += 1; }), "draw entries"));
            EXPECT_TRUE(reportsOnly(flipOne([](LayerCensus& c) { c.Instances += 1; }), "instances submitted"));
            EXPECT_TRUE(reportsOnly(flipOne([](LayerCensus& c) { c.DrawsMesh = !c.DrawsMesh; }), "authored mesh"));
            EXPECT_TRUE(reportsOnly(flipOne([](LayerCensus& c) { c.DrawsWithoutAlbedo += 1; }), "without albedo"));
            EXPECT_TRUE(reportsOnly(flipOne([](LayerCensus& c) { c.ImpostorBound = !c.ImpostorBound; }),
                                    "impostor atlas"));
            for (sizet m = 0; m < kLeafMapNames.size(); ++m)
                EXPECT_TRUE(reportsOnly(flipOne([m](LayerCensus& c) { c.LeafMapBound[m] = !c.LeafMapBound[m]; }),
                                        std::string(kLeafMapNames[m]) + " map"));
            EXPECT_TRUE(reportsOnly(flipOne([](LayerCensus& c) { c.Surfaces.emplace_back("x <- y.png"); }),
                                    "surfaces sampled"));
        }

        // Each downgrade hides one staged file, loads the cooked game fresh,
        // and must be reported with the named finding. A warm mesh cache would
        // serve the .mtl's materials without opening it; a fresh install has
        // none, so the cache is cleared before every planted load.
        struct Plant
        {
            const char* What;
            fs::path File; // game-root relative
            std::vector<std::string> MustReport;
        };
        const fs::path pine = fs::path("Assets") / "Models" / "Vegetation" / "pine";
        const std::vector<Plant> plants{
            { "the pine's .obj", pine / "pine.obj", { "'" + pineName + "': authored mesh", "'" + pineName + "': impostor atlas" } },
            { "the pine's .mtl", pine / "pine.mtl", { "'" + pineName + "': surfaces sampled" } },
            // The pines draw their card only between the mesh and the impostor,
            // and the surface census does not list it: only the per-draw albedo
            // check sees this one.
            { "the pine's card albedo", pine / "Textures" / "pine_card.png", { "'" + pineName + "': draws without albedo" } },
            { "the leaf roughness map", fs::path("assets") / "textures" / "leaf_roughness.png", { "roughness map bound" } },
        };

        for (const auto& plant : plants)
        {
            SCOPED_TRACE(std::string("planted: missing ") + plant.What);
            std::error_code ec;
            fs::remove_all(m_GameRoot / "Assets" / "cache", ec);
            ASSERT_TRUE(fs::is_regular_file(m_GameRoot / plant.File, ec))
                << plant.File.generic_string() << " is not in the staged game — the plant would hide nothing";

            const ScopedHiddenFile hidden(m_GameRoot / plant.File);
            ASSERT_TRUE(hidden.m_Ok) << "could not hide " << plant.File.generic_string();
            Arm* damaged = LoadArm(Source::Cooked, CookedScenePath());
            ASSERT_NE(damaged, nullptr);
            const auto found = CompareCensus(looseCensus, TakeCensus(damaged->Foliage.GetComponent<FoliageComponent>()));
            GTEST_LOG_(INFO) << "missing " << plant.What << " reported as:" << Join(found);
            for (const auto& needle : plant.MustReport)
            {
                EXPECT_TRUE(std::ranges::any_of(found, [&](const std::string& line)
                                                { return line.find(needle) != std::string::npos; }))
                    << "missing " << plant.What << " was not reported as \"" << needle << "\"; found:" << Join(found);
            }

            // The pixel check has to see the worst of them, near and far.
            if (plant.File.filename() == "pine.obj")
            {
                for (const auto& pose : *poses)
                {
                    std::vector<u8> looseFrame;
                    std::vector<u8> damagedFrame;
                    ASSERT_NO_FATAL_FAILURE(Capture(*loose, pose, looseFrame));
                    ASSERT_NO_FATAL_FAILURE(Capture(*damaged, pose, damagedFrame));
                    WriteEvidencePng("FloraCookedNoPineMesh_GL_Deferred_" + pose.Name + ".png", damagedFrame);
                    const f64 drift = DifferingFraction(looseFrame, damagedFrame);
                    GTEST_LOG_(INFO) << "no pine mesh, " << pose.Name << ": loose-vs-cooked " << drift * 100.0 << "%";
                    // Twice the pixel test's allowance: a missing mesh must be
                    // outside anything that test accepts.
                    EXPECT_GT(drift, 2.0 * kPixelAllowance) <<"a cooked game with no pine mesh draws like the loose one at "
                                           << pose.Name << " — the pixel check cannot see this downgrade";
                }
            }
            m_Arms.pop_back(); // the damaged arm; `loose` stays loaded
        }
    }
} // namespace OloEngine::Tests
