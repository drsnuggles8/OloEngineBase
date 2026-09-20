#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomLodVisualEvidenceTest — issue #1252, on real pixels.
//
// Writes OloEditor/assets/tests/visual/GroomLod[Off]_GL_<Path>[_<Distance>].png
//
// The filename carries the {backend} x {path} cell it covers, including the
// backend even though it is always GL here: a reader counting files then cannot
// mistake a complete set of OpenGL captures for a complete verification matrix.
// Every Vulkan cell is live-only — these fixtures need a real GL 4.6 context and
// skip without one — and is evidenced in the PR body.
//
// WHAT A STILL FRAME CANNOT SHOW, AND WHY THIS FILE IS SHAPED AROUND IT.
//
//   * A LOD that preserves density is, at any ONE distance, a picture of a
//     coat. So every capture here is a PAIR — the same camera, the same scene,
//     with GroomLodComponent's switch the only thing moved — and what is
//     asserted is the relation between the two: far fewer strands drawn, and
//     the same amount of coat on screen.
//
//   * The compensation's A/B needs a third arm. "The coat still covers the same
//     area" is not a result unless the arm WITHOUT the compensation covers
//     less, so the cap is dropped to 1.0 (which makes the compensation the
//     identity) and the same frame is taken again.
//
//   * Hysteresis does not exist in a still frame AT ALL. The last two cases
//     drive the camera: one oscillating across the hand-over every frame, one
//     sweeping in and out slowly, and what is asserted is the COUNT of
//     representation changes the pass reports. Criterion 2 says "measured in
//     motion", and a transition count is what that means as a number.
//
// The CPU comparison (GroomLodComparisonTest) is what decided the
// representations; this file is what ties its numbers to pixels the GPU
// actually produced.
//
// Classification: L8 / golden image (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomLod.h"
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        /// Pixels that are not the clear colour. The coat is much brighter than
        /// the background, so this counts COAT — and it is the quantity every
        /// density claim in this file is made about.
        ///
        /// THE FLOOR IS THE ONE THE NEIGHBOURING GROOM EVIDENCE TESTS USE, on
        /// purpose: three tests that disagree about what "coat" means are three
        /// tests nobody can compare.
        [[nodiscard]] u32 CountCoatPixels(const std::vector<u8>& frame)
        {
            constexpr int kCoatFloor = 110;
            u32 count = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                if (frame[i] > kCoatFloor && frame[i + 1] > kCoatFloor)
                {
                    ++count;
                }
            }
            return count;
        }

        [[nodiscard]] f64 MeanLuminance(const std::vector<u8>& frame)
        {
            if (frame.empty())
            {
                return 0.0;
            }
            f64 sum = 0.0;
            u32 pixels = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                sum += (0.2126 * frame[i] + 0.7152 * frame[i + 1] + 0.0722 * frame[i + 2]) / 255.0;
                ++pixels;
            }
            return pixels > 0 ? sum / pixels : 0.0;
        }
    } // namespace

    class GroomLodVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        Entity m_GroomEntity;
        Ref<GroomAsset> m_Groom;
        AssetHandle m_GroomHandle = 0;
        u32 m_CardCount = 0;

        // A coat dense enough that thinning it is visible and coarse enough to
        // stay inside a CI case's budget. 12 000 strands at 6 points is 60 000
        // segments, which is the order a real pelt's drawn budget sits at.
        static constexpr u32 kStrands = 12000;
        static constexpr u32 kPoints = 6;
        static constexpr f32 kRadius = 1.0f;
        static constexpr f32 kLength = 0.55f;
        // 5 cm on a 1-unit body: a quill rather than a hair, DELIBERATELY, and
        // #1246's rule 1 is the reason. A real hair is sub-pixel at every
        // framing, and the OpaqueRibbon tier this file pins for determinism
        // discards any fragment whose widened alpha falls under the 0.5 cutoff
        // — i.e. any strand under half a pixel of half width. The coat's bounds
        // radius here is 2.68 units, so at the far capture its projection is
        // 16.8 px per unit and a 2 cm strand is 0.17 px of half width: alpha
        // 0.34, every fragment discarded, and the LOD-OFF control frame comes
        // back with TWO coat pixels in it. That is what the first two runs of
        // this fixture measured, and it would have made every ratio below a
        // ratio of noise.
        //
        // Authored thick enough to clear the cutoff across the whole capture
        // range instead, so what these cases vary is the LOD and not the
        // composition mode. Sub-pixel behaviour is #1246's subject and is
        // measured there and in GroomLodComparisonTest's analytic model, which
        // does not have a cutoff to fall under.
        static constexpr f32 kStrandWidth = 0.05f;

        // The hand-over threshold this file drives the camera across. Chosen so
        // both sides are reachable with a camera that stays in front of the
        // coat at this field of view.
        static constexpr f32 kCardPixelSize = 220.0f;

        void BuildScene() override
        {
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                std::error_code ec;
                const fs::path projectDir = TempDir("project");
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: GroomLodEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false);
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            auto& rendererSettings = Renderer3D::GetRendererSettings();
            rendererSettings.EditorDebugDrawsEnabled = true;
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 4.0f);
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
            }

            // A PRIMARY RUNTIME CAMERA, far behind the capture poses. Load-
            // bearing for the same two reasons GroomSimulationVisualEvidenceTest
            // gives: Scene::RenderRuntime returns without rendering when nothing
            // carries a primary CameraComponent, so PublishGroomStrandRequests
            // would never run and the LOD would never advance; and a camera
            // entity sitting where the capture camera sits puts a gizmo in the
            // middle of every frame.
            {
                Entity cameraEntity = scene.CreateEntity("RuntimeCamera");
                cameraEntity.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.4f, 60.0f);
                auto& cameraComponent = cameraEntity.AddComponent<CameraComponent>();
                cameraComponent.Primary = true;
                cameraComponent.Camera.SetViewportSize(kWidth, kHeight);
            }

            InstallCoat();
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
        }

        void InstallCoat()
        {
            Scene& scene = GetScene();
            m_Groom = BuildCoat();
            ASSERT_TRUE(m_Groom);

            // THE COOKED CARD LEVEL. Built here rather than loaded, so the case
            // needs no Alembic and no asset on disk — but through the REAL
            // builder, so what is drawn at card range is the bytes the cook
            // writes and not a test-only stand-in.
            GroomCardSettings cardSettings;
            cardSettings.CellSize = 0.02f;
            cardSettings.PointsPerCard = 6;
            cardSettings.SourcePixelSize = kCardPixelSize;
            GroomLodLevel level;
            GroomCardBuildStats cardStats;
            std::string reason;
            ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*m_Groom, cardSettings, level, reason, &cardStats)) << reason;
            m_CardCount = level.GetCurveCount();
            ASSERT_TRUE(GroomLodBuilder::AttachLodLevels(*m_Groom, { std::move(level) }, reason)) << reason;
            ASSERT_NE(m_Groom->FindLodLevel(GroomRepresentation::Card), nullptr);
            std::printf("[groom-lod-evidence] cooked %u cards from %u strands (mean cluster %.1f)\n", m_CardCount,
                        cardStats.CurvesConsidered, static_cast<f64>(cardStats.MeanCluster));

            m_GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(m_Groom);
            ASSERT_NE(static_cast<u64>(m_GroomHandle), 0u);

            m_GroomEntity = scene.CreateEntity("Groom");
            auto& groomComponent = m_GroomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_GroomHandle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = kStrands;
            // OpaqueRibbon, not the stochastic default: a stochastic coat is a
            // different set of pixels every frame, and this file counts pixels
            // across arms of an A/B. The composition mode is #1246's subject and
            // holding it fixed is what makes these numbers about the LOD.
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);

            auto& lod = m_GroomEntity.AddComponent<GroomLodComponent>();
            lod.m_Enabled = true;
            lod.m_CardPixelSize = kCardPixelSize;
            lod.m_MeshPixelSize = 8.0f;
            lod.m_Hysteresis = 0.15f;
            lod.m_HoldFrames = 4;
            lod.m_VisibilityFullPixelSize = 512.0f;
            lod.m_VisibilitySteps = 4;
            lod.m_SimulationFullPixelSize = 384.0f;
            lod.m_ShadowFullPixelSize = 512.0f;
            lod.m_MaxWidthCompensation = 8.0f;
        }

        // Root UVs on the UNIT CHART, which is what a real pelt has and what
        // the card cook needs: the clump-cell addressing clamps a UV to +/-16,
        // so a groom whose chart runs past that has every strand beyond the
        // clamp land in one cell. GroomLodBuilder refuses such a groom by name;
        // this fixture is what a groom that passes looks like.
        static Ref<GroomAsset> BuildCoat()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr f32 kGoldenAngle = 2.39996323f;
            constexpr f32 kTwoPi = 6.28318530718f;

            for (u32 s = 0; s < kStrands; ++s)
            {
                const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(kStrands);
                const f32 cosTheta = 1.0f - t;
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - (cosTheta * cosTheta)));
                const f32 phi = kGoldenAngle * static_cast<f32>(s);
                const glm::vec3 normal(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                const glm::vec3 root = normal * kRadius;

                std::vector<glm::vec3> points;
                std::vector<f32> widths;
                points.reserve(kPoints);
                widths.reserve(kPoints);
                for (u32 p = 0; p < kPoints; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(kPoints - 1u);
                    glm::vec3 point = root + (normal * (kLength * along));
                    point.y -= kLength * 0.55f * along * along;
                    points.push_back(point);
                    widths.push_back(kStrandWidth * (1.0f - (0.8f * along)));
                }

                const f32 wrappedU = phi / kTwoPi;
                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { wrappedU - std::floor(wrappedU), t };
                input.GroupId = group;
                input.IsGuide = (s % 16u) == 0;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("LodCoat");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        [[nodiscard]] GroomLodComponent& Lod()
        {
            return m_GroomEntity.GetComponent<GroomLodComponent>();
        }

        [[nodiscard]] const GroomRenderStats& PassStats() const
        {
            if (const auto* pass = Renderer3D::GetGroomRenderPass())
            {
                return pass->GetStats();
            }
            static const GroomRenderStats kEmpty{};
            return kEmpty;
        }

        /// The eye distance at which the coat projects to `pixelSize` pixels of
        /// the frame height, for the capture camera's 60-degree vertical FOV.
        /// Expressed this way so every pose in this file is stated in the SAME
        /// units GroomLodComponent's thresholds are in — a pose in metres would
        /// have to be re-derived by whoever reads the evidence.
        [[nodiscard]] f32 EyeDistanceFor(f32 pixelSize) const
        {
            const f32 radius = glm::length(m_Groom->GetBoundsMax() - m_Groom->GetBoundsMin()) * 0.5f;
            const f32 cotHalfFov = 1.0f / std::tan(glm::radians(60.0f) * 0.5f);
            return (2.0f * radius) * cotHalfFov * (static_cast<f32>(kHeight) * 0.5f) / std::max(pixelSize, 1.0f);
        }

        void Capture(const std::string& saveAs, f32 pixelSize, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 0.35f, EyeDistanceFor(pixelSize)), 0.0f, 0.05f);

            // Several frames: the LOD advances once per frame and REFINING is
            // immediate while COARSENING waits for the hold, so one frame at a
            // new distance would capture whichever tier the previous pose left
            // behind. Six is past the fixture's hold of four.
            RunEditorFrames(camera, 6);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            }
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << saveAs << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            {
                const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = outPixels.data() + (static_cast<sizet>(y) * rowBytes);
                    u8* bot = outPixels.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            if (!saveAs.empty())
            {
                WriteEvidence(saveAs, outPixels, kWidth, kHeight);
            }
        }

        // EVIDENCE, not SSIM goldens. What is asserted is the contracts; the
        // PNGs exist so a reviewer can look at what the numbers describe.
        static void WriteEvidence(const std::string& name, const std::vector<u8>& pixels, u32 width, u32 height)
        {
            // "assets/...", NOT "OloEditor/assets/...", and that is not a typo:
            // renderer initialisation leaves the process's working directory in
            // OloEditor/, so the repo-relative form lands in
            // OloEditor/OloEditor/ — where the write SUCCEEDS, the test passes,
            // and the evidence is somewhere nobody looks. This file's first run
            // did exactly that. Every neighbouring evidence test writes the
            // same relative path for the same reason.
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path file = dir / (name + ".png");
            stbi_write_png(file.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4, pixels.data(),
                           static_cast<int>(width * 4u));
        }
    };

    // =========================================================================
    // Criterion 1: the hand-over preserves apparent density, on every path
    // =========================================================================

    TEST_F(GroomLodVisualEvidenceTest, TheCardTierKeepsTheCoatWhileDrawingAFractionOfIt)
    {
        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const std::array<PathCase, 3> paths = { {
            { "Forward", RenderingPath::Forward },
            { "ForwardPlus", RenderingPath::ForwardPlus },
            { "Deferred", RenderingPath::Deferred },
        } };

        // Comfortably past the hand-over, so the LOD-on arm is genuinely on
        // cards rather than on a thinned strand tier — and comfortably above
        // the size at which this coat's strands fall under the alpha cutoff,
        // so the CONTROL frame has a coat in it. See kStrandWidth.
        constexpr f32 kFarPixelSize = 120.0f;

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();

            // OFF FIRST: the control cannot be contaminated by the arm under
            // test, and the two use the SAME camera — the trap
            // backend-ab-needs-an-identical-camera names.
            Lod().m_Enabled = false;
            std::vector<u8> without;
            Capture(std::string("GroomLodOff_GL_") + pathCase.Name, kFarPixelSize, without);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            const u32 strandsWithout = PassStats().StrandsDrawn;
            const u32 coatWithout = CountCoatPixels(without);

            Lod().m_Enabled = true;
            std::vector<u8> with;
            Capture(std::string("GroomLod_GL_") + pathCase.Name, kFarPixelSize, with);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            const GroomRenderStats& stats = PassStats();
            const u32 strandsWith = stats.StrandsDrawn;
            const u32 coatWith = CountCoatPixels(with);

            std::printf("[groom-lod-evidence] %-11s  off: %6u strands / %6u coat px   on: %6u strands / %6u coat "
                        "px   tier %s\n",
                        pathCase.Name, strandsWithout, coatWithout, strandsWith, coatWith,
                        stats.Lod.ByRepresentation[static_cast<sizet>(GroomRepresentation::Card)] > 0u ? "Card"
                                                                                                       : "Strand");

            // A frame with nothing in it passes every ratio below trivially, so
            // the framing is checked before anything is concluded from it.
            EXPECT_GT(MeanLuminance(with), 0.02) << pathCase.Name << ": the frame is (near-)black";
            ASSERT_GT(coatWithout, 2000u) << pathCase.Name << ": the control frame has almost no coat in it, so "
                                                              "this case decides nothing";

            // 1. THE SAVING IS REAL. Cards plus the visibility budget together
            //    should be a large fraction less geometry.
            EXPECT_LT(strandsWith, strandsWithout / 4u)
                << pathCase.Name << ": the LOD drew nearly as much as the control, so it did nothing";

            // 2. THE COAT IS STILL THERE. This is criterion 1's "apparent
            //    density is preserved" on real pixels: a quarter of the
            //    geometry, within a quarter of the coverage.
            const f64 coverageRatio = static_cast<f64>(coatWith) / static_cast<f64>(coatWithout);
            std::printf("[groom-lod-evidence] %-11s  coverage ratio %.3f\n", pathCase.Name, coverageRatio);
            EXPECT_GT(coverageRatio, 0.75) << pathCase.Name << ": the coat lost a quarter of its apparent density "
                                                               "at the hand-over";
            EXPECT_LT(coverageRatio, 1.30) << pathCase.Name << ": the coat GAINED density, which means the "
                                                               "compensation is over-applied";

            // 3. THE PASS SAYS WHICH TIER IT DREW. A coat that looks right for
            //    the wrong reason — still on strands, merely thinned — would
            //    pass 1 and 2 and is a different feature.
            EXPECT_EQ(stats.Lod.ByRepresentation[static_cast<sizet>(GroomRepresentation::Card)], 1u)
                << pathCase.Name << ": the coat was not on the card tier at " << kFarPixelSize << " px";
            EXPECT_GT(stats.Lod.BytesByRepresentation[static_cast<sizet>(GroomRepresentation::Card)], 0u)
                << pathCase.Name << ": no memory was reported against the card tier";
        }

        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
    }

    // =========================================================================
    // Criterion 2: the width compensation is what keeps the density
    // =========================================================================

    TEST_F(GroomLodVisualEvidenceTest, WithoutTheWidthCompensationTheThinnedCoatVisiblyThins)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        // INSIDE the card threshold, so both arms are on the STRAND tier and
        // the only thing that differs is the compensation. Measuring this past
        // the hand-over would confound the compensation with the card cook's
        // own summed-width claim, and the two are separate mechanisms.
        constexpr f32 kPixelSize = 300.0f;

        Lod().m_Enabled = true;
        // A visibility curve that bites hard at this size, so there is
        // something for the compensation to compensate for.
        Lod().m_VisibilityFullPixelSize = 2400.0f;
        Lod().m_VisibilitySteps = 3;

        Lod().m_MaxWidthCompensation = 1.0f; // the identity: no compensation
        std::vector<u8> uncompensated;
        Capture("GroomLodNoCompensation_GL_Forward", kPixelSize, uncompensated);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 strandsUncompensated = PassStats().StrandsDrawn;
        const u32 coatUncompensated = CountCoatPixels(uncompensated);

        Lod().m_MaxWidthCompensation = 8.0f;
        std::vector<u8> compensated;
        Capture("GroomLodCompensated_GL_Forward", kPixelSize, compensated);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const GroomRenderStats& stats = PassStats();
        const u32 strandsCompensated = stats.StrandsDrawn;
        const u32 coatCompensated = CountCoatPixels(compensated);

        std::printf("[groom-lod-evidence] compensation off: %u strands / %u coat px   on: %u strands / %u coat px "
                    "(widest %.2fx)\n",
                    strandsUncompensated, coatUncompensated, strandsCompensated, coatCompensated,
                    static_cast<f64>(stats.Lod.MaxWidthCompensation));

        // The two arms must draw the SAME geometry — the compensation is a UBO
        // value, not a rebuild. If they differ, something else moved and the
        // comparison is not about the compensation at all.
        ASSERT_EQ(strandsUncompensated, strandsCompensated)
            << "the two arms drew different amounts of geometry, so this is not a compensation A/B";
        ASSERT_GT(coatUncompensated, 1000u) << "the uncompensated frame has almost no coat in it";

        EXPECT_GT(stats.Lod.MaxWidthCompensation, 1.5f)
            << "the budget did not thin the coat at this size, so there was nothing to compensate";
        EXPECT_GT(coatCompensated, coatUncompensated)
            << "widening the survivors did not put any coat back on screen";
        // A real, visible difference rather than a few pixels of dither.
        EXPECT_GT(static_cast<f64>(coatCompensated) / static_cast<f64>(coatUncompensated), 1.15)
            << "the compensation changed the picture by less than a tone-map wobble";
    }

    // =========================================================================
    // Criterion 2: hysteresis, which only exists in motion
    // =========================================================================

    TEST_F(GroomLodVisualEvidenceTest, ACameraOscillatingAcrossTheHandOverNeverChangesRepresentation)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        Lod().m_Enabled = true;

        // Alternating every frame across the threshold, through the REAL
        // pipeline. A camera that alternates never accumulates the consecutive
        // stable frames a coarsening needs, so it never gets one — and that is
        // the bound GroomLodContractTest proves in arithmetic and this case
        // confirms end to end, counters and all.
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));

        u32 totalChanges = 0;
        for (u32 frame = 0; frame < 40u; ++frame)
        {
            const f32 pixelSize = (frame % 2u == 0u) ? kCardPixelSize * 2.0f : kCardPixelSize * 0.5f;
            camera.SetPose(glm::vec3(0.0f, 0.35f, EyeDistanceFor(pixelSize)), 0.0f, 0.05f);
            RunEditorFrames(camera, 1);
            totalChanges += PassStats().Lod.RepresentationChanges;
        }

        std::printf("[groom-lod-evidence] 40 alternating frames across the hand-over: %u representation changes\n",
                    totalChanges);
        EXPECT_EQ(totalChanges, 0u) << "a two-frame oscillation across the threshold changed the representation, "
                                       "which is the thrashing criterion 2 forbids";
    }

    TEST_F(GroomLodVisualEvidenceTest, ASlowSweepCrossesOnceInEachDirectionAndSaysSo)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        Lod().m_Enabled = true;

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));

        // Out and back, slowly enough that the hold elapses on each side. The
        // expected answer is exactly TWO transitions — one out, one back — and
        // both the floor and the ceiling matter: zero would mean the hand-over
        // never happened, and more than two would mean it happened repeatedly
        // on one crossing.
        const auto sweep = [&](f32 from, f32 to, u32 steps, const char* label, const char* saveAs)
        {
            u32 changes = 0;
            for (u32 i = 0; i <= steps; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
                const f32 pixelSize = from + (to - from) * t;
                camera.SetPose(glm::vec3(0.0f, 0.35f, EyeDistanceFor(pixelSize)), 0.0f, 0.05f);
                RunEditorFrames(camera, 1);
                changes += PassStats().Lod.RepresentationChanges;
            }
            std::printf("[groom-lod-evidence] sweep %-8s %.0f -> %.0f px in %u steps: %u changes\n", label,
                        static_cast<f64>(from), static_cast<f64>(to), steps, changes);
            std::vector<u8> pixels;
            Capture(saveAs, to, pixels);
            return changes;
        };

        const u32 outward = sweep(kCardPixelSize * 2.0f, kCardPixelSize * 0.4f, 40u, "outward",
                                  "GroomLod_GL_Forward_Far");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        EXPECT_EQ(outward, 1u) << "walking away from the coat crossed the hand-over " << outward
                               << " times instead of once";
        EXPECT_EQ(PassStats().Lod.ByRepresentation[static_cast<sizet>(GroomRepresentation::Card)], 1u)
            << "the coat did not end the outward sweep on cards";

        const u32 inward =
            sweep(kCardPixelSize * 0.4f, kCardPixelSize * 2.0f, 40u, "inward", "GroomLod_GL_Forward_Near");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        EXPECT_EQ(inward, 1u) << "walking back crossed the hand-over " << inward << " times instead of once";
        EXPECT_EQ(PassStats().Lod.ByRepresentation[static_cast<sizet>(GroomRepresentation::Strand)], 1u)
            << "the coat did not come back to strands";
    }

    // =========================================================================
    // Criterion 3: the three budgets scale down independently
    // =========================================================================

    TEST_F(GroomLodVisualEvidenceTest, TheShadowBudgetMovesWithoutMovingTheVisibilityBudget)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        Lod().m_Enabled = true;

        // Inside the card threshold, so the representation is fixed and the
        // only thing varying is one budget curve.
        constexpr f32 kPixelSize = 300.0f;

        // A visibility curve that does NOT bite at this size, and a shadow
        // curve that does. Then the reverse. If the two were coupled — one
        // scalar, or one shared hysteresis counter — the strand count would
        // move when only the shadow curve did.
        Lod().m_VisibilityFullPixelSize = 256.0f; // 300 px is above it: step 0
        Lod().m_ShadowFullPixelSize = 4096.0f;    // 300 px is far below it
        Lod().m_ShadowSteps = 4;
        std::vector<u8> pixels;
        Capture("", kPixelSize, pixels);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 strandsShadowCoarse = PassStats().StrandsDrawn;

        Lod().m_ShadowFullPixelSize = 256.0f; // now the shadow curve is at step 0 too
        Capture("", kPixelSize, pixels);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 strandsShadowFine = PassStats().StrandsDrawn;

        std::printf("[groom-lod-evidence] shadow curve coarse: %u strands, fine: %u strands\n", strandsShadowCoarse,
                    strandsShadowFine);
        EXPECT_EQ(strandsShadowCoarse, strandsShadowFine)
            << "moving the SHADOW budget changed how many strands were drawn, so the three budgets are coupled "
               "and criterion 3 is not satisfied";

        // ...and the visibility curve alone does move it, so the case above is
        // not passing because nothing moves anything.
        Lod().m_VisibilityFullPixelSize = 4096.0f;
        Lod().m_VisibilitySteps = 4;
        Capture("", kPixelSize, pixels);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 strandsVisibilityCoarse = PassStats().StrandsDrawn;
        std::printf("[groom-lod-evidence] visibility curve coarse: %u strands\n", strandsVisibilityCoarse);
        EXPECT_LT(strandsVisibilityCoarse, strandsShadowFine)
            << "moving the VISIBILITY budget changed nothing, so the previous assertion proves nothing";
    }
} // namespace OloEngine::Tests
