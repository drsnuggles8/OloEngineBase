// OLO_TEST_LAYER: L8
// =============================================================================
// MaterialLabSceneEvidenceTest.cpp
//
// Numerical sanity checks for the Material-laboratory benchmark SCENE
// (issue #974, acceptance criterion: "The material laboratory has numerical
// sanity checks for the known color/roughness reference patches").
//
// Unlike its sibling MaterialLabVisualEvidenceTest.cpp (#975's analytic probe
// shader — no scene, deliberately), this test loads the COMMITTED
// OloEditor/SandboxProject/Assets/Scenes/Benchmark/MaterialLab.olo through the
// real SceneSerializer (staged into a throwaway temp project so the .scenebin
// sidecar and registry churn land in temp, not the working tree), renders it
// through the full Renderer3D pipeline, and asserts numerical properties of
// KNOWN PIXEL REGIONS. Patch/sphere screen positions are DERIVED, not
// hard-coded: the test looks entities up by tag, takes their transform
// translation, and projects it through the capture camera's view-projection —
// so a re-layout of the scene moves the sample windows with it, and a missing
// tag fails loudly instead of sampling background.
//
// Contracts (all on the tonemapped composite; the scene pins tonemap None,
// exposure 1, bloom/FXAA off, a WHITE sun — see the scene header):
//   1. The 18% grey card (Gray18) reads neutral (R~G~B) and sits strictly
//      between PatchBlack and PatchWhite; the achromatic ladder
//      black < gray18 < gray50 < white is monotonic.
//   2. The linear colour patches keep channel dominance (PatchRed reads
//      R >> G,B; likewise green / blue).
//   3. On the metallic = 1 sweep row, the mirror sphere (Sweep_M1_R0) differs
//      measurably from the rough sphere (Sweep_M1_R1), and its sample window
//      carries MORE luma variance (a mirror reflects the high-contrast
//      environment; a rough metal integrates it away).
//   4. Emissive spheres read brighter than their non-emissive control
//      (Emissive_Ref), and Emissive_Green8 keeps green dominance.
//
// Evidence PNGs (always written BEFORE any assertion, plain write — no golden
// compare; the golden layer is the benchmark capture, not this test):
//   OloEditor/assets/tests/visual/MaterialLabScene_{Overview,Patches,
//   MetalRow,EmissiveRow}.png
//
// The scene keeps its ground plane per
// docs/agent-rules/single-mesh-visual-test-lighting.md.
//
// Runs in the normal suite: SKIPs cleanly (not fails) without a GL 4.6
// context. Run from OloEditor/ so the evidence PNGs land under
// OloEditor/assets/tests/visual/ and the scene's HDRI/textures resolve.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG),
// same layer as WaterVisualEvidenceTest / DriftIslandFieldEvidenceTest.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Frozen wall clock: nothing in this scene animates, but pinning the
        // clock keeps the captures deterministic (TAA/stochastic indices read
        // frame counters, IBL bake settles identically) — same convention as
        // every other evidence capture.
        constexpr f32 kCaptureTime = 12.0f;

        // RAII mock clock — restores the real clock on every exit path,
        // including ASSERT early-returns (the WaterVisualEvidenceTest idiom).
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
        };

        // glGetTextureImage hands back rows bottom-up (GL origin); flip once so
        // row 0 is the TOP and all pixel reasoning happens in image space.
        void FlipRowsInPlace(std::vector<u8>& rgba, u32 width, u32 height)
        {
            const sizet rowBytes = static_cast<sizet>(width) * 4u;
            std::vector<u8> scratch(rowBytes);
            for (u32 y = 0; y < height / 2u; ++y)
            {
                u8* top = rgba.data() + static_cast<sizet>(y) * rowBytes;
                u8* bottom = rgba.data() + static_cast<sizet>(height - 1u - y) * rowBytes;
                std::memcpy(scratch.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, scratch.data(), rowBytes);
            }
        }

        void WriteEvidence(const std::string& name, const std::vector<u8>& rgba, u32 w, u32 h)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path out = dir / (name + ".png");
            const int wrote = ::stbi_write_png(out.string().c_str(), static_cast<int>(w),
                                               static_cast<int>(h), 4, rgba.data(),
                                               static_cast<int>(w) * 4);
            EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();
        }

        // Mean R/G/B and luma standard deviation over a clamped square window.
        struct WindowStats
        {
            f64 MeanR = 0.0;
            f64 MeanG = 0.0;
            f64 MeanB = 0.0;
            f64 LumaStdDev = 0.0;

            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * MeanR + 0.7152 * MeanG + 0.0722 * MeanB;
            }
        };

        [[nodiscard]] WindowStats SampleWindow(const std::vector<u8>& pixels, u32 cx, u32 cy, u32 half)
        {
            const u32 x0 = (cx > half) ? cx - half : 0u;
            const u32 y0 = (cy > half) ? cy - half : 0u;
            const u32 x1 = std::min(cx + half, kWidth - 1u);
            const u32 y1 = std::min(cy + half, kHeight - 1u);

            WindowStats stats;
            f64 sumR = 0.0;
            f64 sumG = 0.0;
            f64 sumB = 0.0;
            f64 sumLuma = 0.0;
            f64 sumLumaSq = 0.0;
            u64 count = 0;
            for (u32 y = y0; y <= y1; ++y)
            {
                for (u32 x = x0; x <= x1; ++x)
                {
                    const sizet idx = (static_cast<sizet>(y) * kWidth + x) * 4u;
                    const f64 r = pixels[idx + 0];
                    const f64 g = pixels[idx + 1];
                    const f64 b = pixels[idx + 2];
                    const f64 luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    sumR += r;
                    sumG += g;
                    sumB += b;
                    sumLuma += luma;
                    sumLumaSq += luma * luma;
                    ++count;
                }
            }
            if (count == 0)
                return stats;
            const f64 n = static_cast<f64>(count);
            stats.MeanR = sumR / n;
            stats.MeanG = sumG / n;
            stats.MeanB = sumB / n;
            const f64 meanLuma = sumLuma / n;
            stats.LumaStdDev = std::sqrt(std::max(0.0, sumLumaSq / n - meanLuma * meanLuma));
            return stats;
        }
    } // namespace

    class MaterialLabSceneEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // Deserialising the scene builds GPU resources (primitive meshes,
            // the HDRI). Headless, the first GL call in MeshSource::Build is a
            // null-function-pointer crash BEFORE the test body's skip can run
            // (the AssetSceneLoadTest lesson) — so gate the whole build here
            // and let the test body GTEST_SKIP over the empty scene.
            if (!RenderPropertyFixture::IsGpuAvailable())
                return;

            EnableRendering(kWidth, kHeight);

            // Stage the committed scene into a throwaway temp project.
            // Deserialize writes a .scenebin sidecar next to the scene and
            // EditorAssetManager::Initialize re-serialises the registry, so
            // pointing either at the real working tree would dirty it.
            m_TempDir = OloEngine::Tests::TempDir("materiallab");
            std::error_code ec;
            fs::create_directories(m_TempDir / "Assets" / "Scenes" / "Benchmark", ec);
            ASSERT_FALSE(ec) << "failed to create temp project dirs: " << ec.message();

            const fs::path sourceScene = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" /
                                         "Assets" / "Scenes" / "Benchmark" / "MaterialLab.olo";
            ASSERT_TRUE(fs::exists(sourceScene))
                << sourceScene.string()
                << " missing — regenerate with OloEngine/tests/scripts/generate_material_lab_scene.py";
            const fs::path stagedScene =
                m_TempDir / "Assets" / "Scenes" / "Benchmark" / "MaterialLab.olo";
            fs::copy_file(sourceScene, stagedScene, fs::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(ec) << "failed to stage MaterialLab.olo: " << ec.message();

            const fs::path projectFile = m_TempDir / "MaterialLab.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: MaterialLab\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile))
                << "Project::Load failed for " << projectFile.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            // No file watcher: the test only reads, and a watcher thread on a
            // temp dir removed at teardown is a use-after-free.
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);

            // The full "File -> Open Scene" path, into the fixture's Scene so
            // RunEditorFrames drives the loaded content directly.
            SceneSerializer serializer(GetSceneRef());
            ASSERT_TRUE(serializer.Deserialize(stagedScene))
                << "SceneSerializer::Deserialize failed for " << stagedScene.string()
                << " — see engine log";
        }

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            // Release GPU-holding asset Refs while the GL context is alive
            // (Shutdown also serialises the registry back into the temp
            // project, which must precede its removal).
            if (m_AssetManager)
                m_AssetManager->Shutdown();
            m_AssetManager.Reset();
            if (!m_TempDir.empty())
            {
                std::error_code ec;
                fs::remove_all(m_TempDir, ec);
            }
        }

        // Tag -> transform translation for every tagged entity in the loaded
        // scene. Sample positions are derived from these, never hard-coded.
        [[nodiscard]] std::unordered_map<std::string, glm::vec3> CollectTagPositions()
        {
            std::unordered_map<std::string, glm::vec3> out;
            auto view = GetScene().GetAllEntitiesWith<TagComponent, TransformComponent>();
            for (auto entity : view)
            {
                const auto& [tag, transform] = view.get<TagComponent, TransformComponent>(entity);
                out[tag.Tag] = transform.Translation;
            }
            return out;
        }

        // Pose, tick, read back, flip, write evidence. Callers MUST wrap in
        // ASSERT_NO_FATAL_FAILURE (the Drift idiom): the ASSERTs below return
        // from the helper, not the test.
        void Capture(const std::string& poseName, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels, EditorCamera& outCamera)
        {
            outCamera = EditorCamera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                     0.05f, 1000.0f);
            outCamera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // SetPose bakes the view matrix from eye + yaw/pitch; positive
            // pitch tilts the view DOWN (see WaterVisualEvidenceTest).
            outCamera.SetPose(position, yaw, pitch);

            // Frame 1 uploads/compiles lazily-built resources (IBL bake kick,
            // shadow maps); the last frame renders the settled scene.
            RunEditorFrames(outCamera, 3);

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, width, height))
                << "no composited frame for pose '" << poseName << "'";
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);
            FlipRowsInPlace(outPixels, kWidth, kHeight);

            // Evidence FIRST, before any numerical assertion, so a reviewer
            // always has the frame that produced the numbers.
            WriteEvidence("MaterialLabScene_" + poseName, outPixels, kWidth, kHeight);
        }

        // Project a world point through the capture camera into image-space
        // pixel coordinates (row 0 = top, matching the flipped buffer).
        [[nodiscard]] bool ProjectToPixel(const EditorCamera& camera, const glm::vec3& world,
                                          u32& outX, u32& outY) const
        {
            const glm::vec4 clip = camera.GetViewProjection() * glm::vec4(world, 1.0f);
            if (clip.w <= 0.0f)
                return false;
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (std::fabs(ndc.x) > 1.0f || std::fabs(ndc.y) > 1.0f)
                return false;
            outX = static_cast<u32>((ndc.x * 0.5f + 0.5f) * static_cast<f32>(kWidth - 1u));
            outY = static_cast<u32>((1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<f32>(kHeight - 1u));
            return true;
        }

        // Look a tag up, project its (slightly lifted) translation, and sample
        // a window there. ASSERTs on a missing tag or an off-screen projection
        // so a scene re-layout can never silently sample background.
        void SampleTag(const std::unordered_map<std::string, glm::vec3>& positions,
                       const EditorCamera& camera, const std::vector<u8>& pixels,
                       const std::string& tag, f32 liftY, u32 half, WindowStats& outStats)
        {
            const auto it = positions.find(tag);
            ASSERT_NE(it, positions.end())
                << "entity tag '" << tag << "' not found in MaterialLab.olo — the scene and "
                << "this test have drifted (regenerate the scene / update the tag list)";
            u32 px = 0;
            u32 py = 0;
            ASSERT_TRUE(ProjectToPixel(camera, it->second + glm::vec3(0.0f, liftY, 0.0f), px, py))
                << "tag '" << tag << "' projects outside the frame — the capture pose no longer "
                << "covers it";
            outStats = SampleWindow(pixels, px, py, half);
        }

        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_TempDir;
    };

    // -------------------------------------------------------------------------
    // Contract 1 + 2: the reference patch strip. Near-top-down pose over the
    // strip (z = 8.25) so each patch top is seen almost square-on and the
    // specular lobe contributes minimally. Also captures the whole-lab
    // Overview evidence frame with a non-black sanity check.
    // -------------------------------------------------------------------------
    TEST_F(MaterialLabSceneEvidenceTest, ReferencePatchesAreNumericallySane)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        const auto positions = CollectTagPositions();
        ASSERT_FALSE(positions.empty()) << "scene deserialised to zero tagged entities";

        // Whole-lab overview from the scene camera's own pose (evidence frame).
        {
            std::vector<u8> pixels;
            EditorCamera camera;
            ASSERT_NO_FATAL_FAILURE(Capture("Overview", { 0.0f, 9.0f, 16.0f }, 0.0f, 0.55f,
                                            pixels, camera));
            u64 lumaSum = 0;
            for (sizet i = 0; i + 3 < pixels.size(); i += 4)
                lumaSum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            const f64 meanChannel =
                static_cast<f64>(lumaSum) / (static_cast<f64>(kWidth) * kHeight * 3.0);
            EXPECT_GT(meanChannel, 5.0) << "Overview rendered (near-)black — see "
                                           "MaterialLabScene_Overview.png";
        }

        // Near-top-down over the patch strip. Pitch 1.45 rad (~83 deg down)
        // keeps 'forward' well-defined while looking almost straight down at
        // the patch tops.
        std::vector<u8> pixels;
        EditorCamera camera;
        ASSERT_NO_FATAL_FAILURE(Capture("Patches", { 0.0f, 10.0f, 8.4f }, 0.0f, 1.45f,
                                        pixels, camera));

        // Patch tops sit 0.04 above the entity translation; window half-width
        // 12 px sits well inside the ~110 px projected patch.
        constexpr f32 kPatchLift = 0.05f;
        constexpr u32 kPatchHalf = 12;

        WindowStats black;
        WindowStats gray18;
        WindowStats gray50;
        WindowStats white;
        WindowStats red;
        WindowStats green;
        WindowStats blue;
        ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "PatchBlack", kPatchLift, kPatchHalf, black));
        ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "Gray18", kPatchLift, kPatchHalf, gray18));
        ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "PatchGray50", kPatchLift, kPatchHalf, gray50));
        ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "PatchWhite", kPatchLift, kPatchHalf, white));
        ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "PatchRed", kPatchLift, kPatchHalf, red));
        ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "PatchGreen", kPatchLift, kPatchHalf, green));
        ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "PatchBlue", kPatchLift, kPatchHalf, blue));

        // --- 18% grey card: neutral. The sun is pure white by design; the
        // Newport Loft IBL carries a mild warm cast, so "a few LSB" is given
        // some slack. If this fails marginally, read the evidence PNG before
        // touching the threshold: a strongly tinted card means a tinted light
        // leaked into the scene.
        {
            const f64 spread = std::max({ std::fabs(gray18.MeanR - gray18.MeanG),
                                          std::fabs(gray18.MeanG - gray18.MeanB),
                                          std::fabs(gray18.MeanR - gray18.MeanB) });
            EXPECT_LE(spread, 16.0)
                << "Gray18 is not neutral (R=" << gray18.MeanR << " G=" << gray18.MeanG
                << " B=" << gray18.MeanB << ") — see MaterialLabScene_Patches.png";
        }

        // --- Achromatic ladder is monotonic: black < gray18 < gray50 < white.
        EXPECT_GT(gray18.Luma(), black.Luma() + 8.0)
            << "Gray18 (" << gray18.Luma() << ") is not brighter than PatchBlack ("
            << black.Luma() << ")";
        EXPECT_GT(gray50.Luma(), gray18.Luma() + 8.0)
            << "PatchGray50 (" << gray50.Luma() << ") is not brighter than Gray18 ("
            << gray18.Luma() << ")";
        EXPECT_GT(white.Luma(), gray50.Luma() + 6.0)
            << "PatchWhite (" << white.Luma() << ") is not brighter than PatchGray50 ("
            << gray50.Luma() << ")";

        // --- Channel dominance on the pure colour patches. The margins are
        // deliberately far above IBL-cast noise: a [1,0,0] rough dielectric
        // under white light cannot read anything but strongly red.
        EXPECT_GT(red.MeanR, red.MeanG + 40.0)
            << "PatchRed R=" << red.MeanR << " G=" << red.MeanG << " B=" << red.MeanB;
        EXPECT_GT(red.MeanR, red.MeanB + 40.0)
            << "PatchRed R=" << red.MeanR << " G=" << red.MeanG << " B=" << red.MeanB;
        EXPECT_GT(green.MeanG, green.MeanR + 40.0)
            << "PatchGreen R=" << green.MeanR << " G=" << green.MeanG << " B=" << green.MeanB;
        EXPECT_GT(green.MeanG, green.MeanB + 40.0)
            << "PatchGreen R=" << green.MeanR << " G=" << green.MeanG << " B=" << green.MeanB;
        EXPECT_GT(blue.MeanB, blue.MeanR + 30.0)
            << "PatchBlue R=" << blue.MeanR << " G=" << blue.MeanG << " B=" << blue.MeanB;
        EXPECT_GT(blue.MeanB, blue.MeanG + 30.0)
            << "PatchBlue R=" << blue.MeanR << " G=" << blue.MeanG << " B=" << blue.MeanB;
    }

    // -------------------------------------------------------------------------
    // Contract 3 + 4: the metallic sweep row and the emissive row.
    // -------------------------------------------------------------------------
    TEST_F(MaterialLabSceneEvidenceTest, RoughnessSweepAndEmissiveContrast)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        const auto positions = CollectTagPositions();
        ASSERT_FALSE(positions.empty()) << "scene deserialised to zero tagged entities";

        // --- Metal row (metallic = 1, z = 6.75, nearest the camera). Sphere
        // centres project ~19 px radius at this distance; half = 6 stays well
        // inside the silhouette.
        {
            std::vector<u8> pixels;
            EditorCamera camera;
            ASSERT_NO_FATAL_FAILURE(Capture("MetalRow", { 0.0f, 5.5f, 14.0f }, 0.0f, 0.60f,
                                            pixels, camera));

            constexpr u32 kSphereHalf = 6;
            WindowStats mirror;
            WindowStats rough;
            ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "Sweep_M1_R0", 0.0f, kSphereHalf, mirror));
            ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "Sweep_M1_R1", 0.0f, kSphereHalf, rough));

            // The two roughness endpoints of the metal row must be measurably
            // different materials on screen.
            const f64 meanAbsDiff = (std::fabs(mirror.MeanR - rough.MeanR) +
                                     std::fabs(mirror.MeanG - rough.MeanG) +
                                     std::fabs(mirror.MeanB - rough.MeanB)) /
                                    3.0;
            EXPECT_GT(meanAbsDiff, 10.0)
                << "mirror metal (Sweep_M1_R0: R=" << mirror.MeanR << " G=" << mirror.MeanG
                << " B=" << mirror.MeanB << ") and rough metal (Sweep_M1_R1: R=" << rough.MeanR
                << " G=" << rough.MeanG << " B=" << rough.MeanB
                << ") are near-identical — the roughness sweep is not reaching the shading. "
                << "See MaterialLabScene_MetalRow.png";

            // Specular-highlight compactness, measured as window variance: the
            // mirror sphere images the high-contrast studio environment, the
            // rough sphere integrates it into a near-flat window.
            EXPECT_GT(mirror.LumaStdDev, rough.LumaStdDev)
                << "mirror-metal window variance (" << mirror.LumaStdDev
                << ") does not exceed rough-metal (" << rough.LumaStdDev
                << ") — the mirror endpoint is not imaging the environment. "
                << "See MaterialLabScene_MetalRow.png";
        }

        // --- Emissive row (z = -9): every emissive sphere must beat the
        // non-emissive control of the same base material. Depends on the #974
        // MaterialComponent `Emissive` key reaching the lit shader's emissive
        // term — a regression there reads all five spheres identically.
        {
            std::vector<u8> pixels;
            EditorCamera camera;
            ASSERT_NO_FATAL_FAILURE(Capture("EmissiveRow", { 0.0f, 4.5f, -1.0f }, 0.0f, 0.55f,
                                            pixels, camera));

            constexpr u32 kSphereHalf = 7;
            WindowStats ref;
            WindowStats one;
            WindowStats colored;
            WindowStats green8;
            WindowStats sixteen;
            ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "Emissive_Ref", 0.0f, kSphereHalf, ref));
            ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "Emissive_1x", 0.0f, kSphereHalf, one));
            ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "Emissive_Colored", 0.0f, kSphereHalf, colored));
            ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "Emissive_Green8", 0.0f, kSphereHalf, green8));
            ASSERT_NO_FATAL_FAILURE(SampleTag(positions, camera, pixels, "Emissive_16x", 0.0f, kSphereHalf, sixteen));

            EXPECT_GT(sixteen.Luma(), ref.Luma() + 40.0)
                << "Emissive_16x (" << sixteen.Luma() << ") barely beats the non-emissive control ("
                << ref.Luma() << ") — emissive is not reaching the shader. "
                << "See MaterialLabScene_EmissiveRow.png";
            EXPECT_GT(green8.Luma(), ref.Luma() + 20.0)
                << "Emissive_Green8 (" << green8.Luma() << ") vs control (" << ref.Luma() << ")";
            EXPECT_GT(one.Luma(), ref.Luma() + 6.0)
                << "Emissive_1x (" << one.Luma() << ") vs control (" << ref.Luma() << ")";

            // Colour of the emission survives to the composite.
            EXPECT_GT(green8.MeanG, green8.MeanR + 30.0)
                << "Emissive_Green8 R=" << green8.MeanR << " G=" << green8.MeanG
                << " B=" << green8.MeanB;
            EXPECT_GT(green8.MeanG, green8.MeanB + 30.0)
                << "Emissive_Green8 R=" << green8.MeanR << " G=" << green8.MeanG
                << " B=" << green8.MeanB;
            EXPECT_GT(colored.MeanR, colored.MeanB + 20.0)
                << "Emissive_Colored ([4,2,0.5]) R=" << colored.MeanR << " G=" << colored.MeanG
                << " B=" << colored.MeanB << " — red/blue ordering lost";
        }
    }
} // namespace OloEngine::Tests
