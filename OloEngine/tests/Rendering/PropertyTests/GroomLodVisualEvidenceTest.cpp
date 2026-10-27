#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomLodVisualEvidenceTest — issue #1252, on real pixels.
//
// Writes OloEditor/assets/tests/visual/GroomLod[Off]_GL_<Path>[_<Distance>].png
// and GroomDetailBias{High,Low}_GL_<Path>.png (#1616), and the cross-fade film
// strips GroomLod{CrossFade,HalvingFade}[Off|NoTAA]_GL_<Path>_Dolly.png
// (#1612, #1628)
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
#include "OloEngine/Groom/GroomStreaming.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/LightProbeBaker.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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
            // behind. Six is past the fixture's hold of four -- and the
            // cross-fade's window on top (#1612), so a capture is the tier the
            // coat SETTLED on rather than a frame of both through the dither.
            RunEditorFrames(camera, 6u + Renderer3D::GetRendererSettings().GroomLodCrossFadeFrames);

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

            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, outPixels);
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

        // ── The dolly through the hand-over (#1612) ─────────────────────────
        //
        // WHAT IS MEASURED: the coat's light on screen, frame to frame, while
        // the camera walks slowly out through the card hand-over. A hard
        // switch is one frame whose step stands far above every other; a
        // cross-fade spreads the same total change over its window.
        //
        // THE DOLLY'S OWN TREND IS DIVIDED OUT. Walking away shrinks the coat,
        // so its summed light falls about as fast as its projected AREA every
        // frame -- at this pace a step comparable to the pop itself. The
        // signal is therefore the coat's light per unit of projected area
        // (summed |L - background| over the apparent size squared), which a
        // coat that keeps its look holds roughly constant through the walk.
        struct DollyResult
        {
            std::vector<f64> Signal;  // per frame, the area-normalised coat light
            std::vector<u32> Changes; // per frame, representation changes the pass reported
            std::vector<u32> Fading;  // per frame, coats the pass reported cross-fading
            u32 HandOverFrame = 0;    // the frame the representation changed (0: none)
            u32 HandOvers = 0;
            u32 FadeFrames = 0;     // frames the pass reported the coat cross-fading
            std::vector<f64> GpuMs; // per frame, GroomPass GPU ms (negative: no valid sample)
            u32 Refused = 0;        // hand-overs that hard-switched for want of a slot
            f64 MaxStep = 0.0;      // largest |relative frame-to-frame step|, whole walk
            f64 MaxStepNearHandOver = 0.0;
            f64 NoiseFloor = 0.0;  // median |step| away from the hand-over and the halvings
            f64 TotalChange = 0.0; // mean after the hand-over over mean before, minus 1
            // The visibility budget's halvings (#1628): the coat's step per
            // frame, the frames it moved on, and the largest |step| from two
            // frames before each to the end of its window.
            std::vector<u32> VisibilitySteps;
            std::vector<u32> HalvingFrames;
            std::vector<f64> MaxStepNearHalving;
        };

        static constexpr u32 kDollyFrames = 192;
        static constexpr f32 kDollyFrom = kCardPixelSize * 1.5f;
        static constexpr f32 kDollyTo = kCardPixelSize * 0.55f;

        [[nodiscard]] DollyResult Dolly(const std::string& saveAs, const std::string& halvingSaveAs = "")
        {
            DollyResult result;
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            const auto sizeAt = [](u32 frame)
            {
                const f32 t = static_cast<f32>(frame) / static_cast<f32>(kDollyFrames - 1u);
                return kDollyFrom * std::pow(kDollyTo / kDollyFrom, t);
            };

            // A fresh ladder and a converged history at the start pose: the
            // component off resets the state, and the resolve settles before
            // anything is measured.
            Lod().m_Enabled = false;
            camera.SetPose(glm::vec3(0.0f, 0.35f, EyeDistanceFor(sizeAt(0))), 0.0f, 0.05f);
            RunEditorFrames(camera, 2);
            Lod().m_Enabled = true;
            Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::SceneReset);
            RunEditorFrames(camera, 24);

            // The film strip's tiles: the coat's neighbourhood, flipped top-down,
            // kept per frame only when the strip is written. The whole readback
            // per frame would hold ~700 MB for one walk.
            constexpr u32 kTile = 200;
            std::vector<u8> pixels;
            std::vector<std::vector<u8>> strip;
            for (u32 frame = 0; frame < kDollyFrames; ++frame)
            {
                const f32 pixelSize = sizeAt(frame);
                camera.SetPose(glm::vec3(0.0f, 0.35f, EyeDistanceFor(pixelSize)), 0.0f, 0.05f);
                RunEditorFrames(camera, 1);
                const GroomRenderStats& stats = PassStats();
                result.Changes.push_back(stats.Lod.RepresentationChanges);
                result.Fading.push_back(stats.Lod.CrossFading);
                result.FadeFrames += stats.Lod.CrossFading > 0u ? 1u : 0u;
                result.Refused += stats.Lod.CrossFadesRefused;
                f64 gpuMs = -1.0;
                for (const auto& timing : GPUPassTimerPool::GetInstance().GetLastFrameTimings().Passes)
                {
                    if (timing.Name == "GroomPass" && timing.IsValid())
                    {
                        gpuMs = timing.Sample.GpuMs;
                    }
                }
                result.GpuMs.push_back(gpuMs);
                if (stats.Lod.RepresentationChanges > 0u)
                {
                    ++result.HandOvers;
                    result.HandOverFrame = frame;
                }
                const GroomLodState* lodState = GetScene().FindGroomLodState(m_GroomEntity.GetUUID());
                result.VisibilitySteps.push_back(lodState != nullptr ? lodState->VisibilityStep : 0u);
                if (frame > 0u && result.VisibilitySteps[frame] != result.VisibilitySteps[frame - 1u])
                {
                    result.HalvingFrames.push_back(frame);
                }

                auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
                if (!fb)
                {
                    fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
                }
                EXPECT_TRUE(fb);
                if (!fb)
                {
                    return result;
                }
                ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, pixels);
                // The clear colour from a corner the coat never reaches.
                const f64 background = (0.2126 * pixels[0] + 0.7152 * pixels[1] + 0.0722 * pixels[2]) / 255.0;
                f64 light = 0.0;
                for (sizet i = 0; i + 3 < pixels.size(); i += 4)
                {
                    const f64 l = (0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2]) / 255.0;
                    light += std::abs(l - background);
                }
                result.Signal.push_back(light / (static_cast<f64>(pixelSize) * static_cast<f64>(pixelSize)));
                if (!saveAs.empty() || !halvingSaveAs.empty())
                {
                    std::vector<u8>& tile = strip.emplace_back(static_cast<sizet>(kTile) * kTile * 4u);
                    for (u32 y = 0; y < kTile; ++y)
                    {
                        // Bottom-up readback: flip into the tile's rows.
                        const u32 sy = (kHeight / 2u) + (kTile / 2u) - 1u - y;
                        const u32 sx = (kWidth / 2u) - (kTile / 2u);
                        std::memcpy(tile.data() + static_cast<sizet>(y) * kTile * 4u,
                                    pixels.data() + ((static_cast<sizet>(sy) * kWidth) + sx) * 4u, kTile * 4u);
                    }
                }
            }

            // Steps, the noise floor away from the hand-over, and the total
            // change across it.
            std::vector<f64> steps(result.Signal.size(), 0.0);
            std::vector<f64> quiet;
            result.MaxStepNearHalving.assign(result.HalvingFrames.size(), 0.0);
            const auto nearEvent = [](sizet f, u32 event)
            {
                const i64 from = static_cast<i64>(f) - static_cast<i64>(event);
                return from >= -2 && from <= 24;
            };
            for (sizet f = 1; f < result.Signal.size(); ++f)
            {
                steps[f] = std::abs(result.Signal[f] / std::max(result.Signal[f - 1], 1.0e-9) - 1.0);
                result.MaxStep = std::max(result.MaxStep, steps[f]);
                bool quietFrame = true;
                if (result.HandOvers > 0u && nearEvent(f, result.HandOverFrame))
                {
                    result.MaxStepNearHandOver = std::max(result.MaxStepNearHandOver, steps[f]);
                    quietFrame = false;
                }
                for (sizet h = 0; h < result.HalvingFrames.size(); ++h)
                {
                    if (nearEvent(f, result.HalvingFrames[h]))
                    {
                        result.MaxStepNearHalving[h] = std::max(result.MaxStepNearHalving[h], steps[f]);
                        quietFrame = false;
                    }
                }
                if (quietFrame)
                {
                    quiet.push_back(steps[f]);
                }
            }
            if (!quiet.empty())
            {
                std::nth_element(quiet.begin(), quiet.begin() + static_cast<std::ptrdiff_t>(quiet.size() / 2), quiet.end());
                result.NoiseFloor = quiet[quiet.size() / 2];
            }
            if (result.HandOvers > 0u && result.HandOverFrame >= 10u && result.HandOverFrame + 34u < kDollyFrames)
            {
                f64 before = 0.0;
                f64 after = 0.0;
                for (u32 k = 0; k < 8u; ++k)
                {
                    before += result.Signal[result.HandOverFrame - 8u + k];
                    after += result.Signal[result.HandOverFrame + 26u + k];
                }
                result.TotalChange = after / std::max(before, 1.0e-9) - 1.0;
            }

            // The film strips: the coat's neighbourhood on eight frames from
            // just before the hand-over, and the first halving, through the
            // cross-fade window.
            const auto writeStrip = [&](const std::string& name, u32 event)
            {
                constexpr u32 kTiles = 8;
                std::vector<u8> film(static_cast<sizet>(kTile) * kTiles * kTile * 4u, 0u);
                for (u32 t = 0; t < kTiles; ++t)
                {
                    const u32 frame = std::min<u32>(event - 1u + t * 3u, kDollyFrames - 1u);
                    const std::vector<u8>& src = strip[frame];
                    for (u32 y = 0; y < kTile; ++y)
                    {
                        std::memcpy(film.data() + ((static_cast<sizet>(y) * kTile * kTiles) + t * kTile) * 4u,
                                    src.data() + static_cast<sizet>(y) * kTile * 4u, kTile * 4u);
                    }
                }
                WriteEvidence(name, film, kTile * kTiles, kTile);
            };
            if (!saveAs.empty() && result.HandOvers > 0u)
            {
                writeStrip(saveAs, result.HandOverFrame);
            }
            if (!halvingSaveAs.empty() && !result.HalvingFrames.empty())
            {
                writeStrip(halvingSaveAs, result.HalvingFrames.front());
            }
            return result;
        }

        static void PrintDolly(const char* label, const DollyResult& r)
        {
            std::printf("[groom-crossfade] %-30s hand-overs %u at frame %u   fading %u frames   refused %u   max "
                        "step %.4f   near hand-over %.4f   noise floor %.4f   total change %+.4f\n",
                        label, r.HandOvers, r.HandOverFrame, r.FadeFrames, r.Refused, r.MaxStep,
                        r.MaxStepNearHandOver, r.NoiseFloor, r.TotalChange);
            // THE COST OF A TRANSITION FRAME (#1612, #1628 acceptance):
            // GroomPass GPU ms over a window against the tier before it and the
            // tier after it. The timer pool reports a frame or two late, so the
            // window's first two frames and the frames just after are left out.
            const auto mean = [&r](u32 from, u32 to)
            {
                f64 sum = 0.0;
                u32 n = 0;
                for (u32 f = from; f < std::min<u32>(to, static_cast<u32>(r.GpuMs.size())); ++f)
                {
                    if (r.GpuMs[f] >= 0.0)
                    {
                        sum += r.GpuMs[f];
                        ++n;
                    }
                }
                return n > 0u ? sum / n : -1.0;
            };
            // The frames a window opened on `frame` drew both tiers.
            const auto fadeRun = [&r](u32 frame)
            {
                u32 n = 0;
                while (frame + n < r.Fading.size() && r.Fading[frame + n] > 0u)
                {
                    ++n;
                }
                return n;
            };
            for (sizet h = 0; h < r.HalvingFrames.size(); ++h)
            {
                const u32 frame = r.HalvingFrames[h];
                std::printf("[groom-crossfade] %-30s halving %u -> %u at frame %u   max step near it %.4f", label,
                            r.VisibilitySteps[frame - 1u], r.VisibilitySteps[frame], frame, r.MaxStepNearHalving[h]);
                if (frame >= 12u)
                {
                    std::printf("   GroomPass GPU ms: before %.3f   window %.3f   after %.3f", mean(frame - 12u, frame),
                                mean(frame + 2u, frame + std::max(fadeRun(frame), 2u)), mean(frame + 26u, frame + 38u));
                }
                std::printf("\n");
            }
            if (r.HandOvers > 0u && r.HandOverFrame >= 14u && r.HandOverFrame + 40u < r.GpuMs.size())
            {
                const u32 h = r.HandOverFrame;
                std::printf("[groom-crossfade] %-30s GroomPass GPU ms: strands %.3f   window %.3f   cards %.3f\n",
                            label, mean(h - 12u, h), mean(h + 2u, h + std::max(fadeRun(h), 2u)), mean(h + 26u, h + 38u));
            }
            if (r.HandOvers > 0u)
            {
                std::printf("[groom-crossfade] %-30s signal around the hand-over:", label);
                const u32 first = r.HandOverFrame >= 3u ? r.HandOverFrame - 3u : 0u;
                for (u32 f = first; f < std::min<u32>(first + 22u, static_cast<u32>(r.Signal.size())); ++f)
                {
                    std::printf(" %.4f%s", r.Signal[f] / r.Signal[first], r.Fading[f] > 0u ? "*" : "");
                }
                std::printf("\n");
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
    // The global detail bias (#1616): the hand-over moves with the preset
    // =========================================================================

    TEST_F(GroomLodVisualEvidenceTest, TheDetailBiasMovesTheHandOverOnEveryPath)
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

        // 300 px: above the fixture's 220 px card edge, below it divided by
        // Low's 0.5 (440 px). At 1 the coat is on strands, at 0.5 on cards. The
        // pair is the same camera, the bias the only thing moved.
        constexpr f32 kPixelSize = 300.0f;
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        const f32 savedBias = settings.GroomDetailBias;
        const auto onCards = [this]
        { return PassStats().Lod.ByRepresentation[static_cast<sizet>(GroomRepresentation::Card)] == 1u; };
        // EACH ARM FROM A FRESH LADDER. The hysteresis holds whatever tier the
        // previous arm left, and this pose sits inside the 15% dead band, so
        // without the reset the second arm reads the first arm's tier. Turning
        // the component off resets the state (ADisabledPolicyIsTheIdentity...).
        const auto freshLadder = [this]
        {
            Lod().m_Enabled = false;
            std::vector<u8> scratch;
            Capture("", kPixelSize, scratch);
            Lod().m_Enabled = true;
        };

        for (const PathCase& pathCase : paths)
        {
            settings.Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();
            Lod().m_Enabled = true;

            settings.GroomDetailBias = 1.0f;
            freshLadder();
            std::vector<u8> asAuthored;
            Capture(std::string("GroomDetailBiasHigh_GL_") + pathCase.Name, kPixelSize, asAuthored);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            const bool authoredOnCards = onCards();
            const u32 strandsAuthored = PassStats().StrandsDrawn;

            settings.GroomDetailBias = 0.5f;
            freshLadder();
            std::vector<u8> low;
            Capture(std::string("GroomDetailBiasLow_GL_") + pathCase.Name, kPixelSize, low);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            const bool lowOnCards = onCards();
            const u32 strandsLow = PassStats().StrandsDrawn;
            const f64 coverage =
                static_cast<f64>(CountCoatPixels(low)) / std::max(1.0, static_cast<f64>(CountCoatPixels(asAuthored)));

            std::printf("[groom-detail-bias] %-11s  bias 1: %s, %6u strands   bias 0.5: %s, %6u strands   "
                        "coverage %.3f\n",
                        pathCase.Name, authoredOnCards ? "Card" : "Strand", strandsAuthored,
                        lowOnCards ? "Card" : "Strand", strandsLow, coverage);
            EXPECT_GT(MeanLuminance(asAuthored), 0.02) << pathCase.Name << ": the frame is (near-)black";
            EXPECT_FALSE(authoredOnCards) << pathCase.Name << ": at bias 1 a 300 px coat left its strands";
            EXPECT_TRUE(lowOnCards) << pathCase.Name << ": at bias 0.5 the 440 px card edge did not hand over";
            EXPECT_LT(strandsLow, strandsAuthored / 2u) << pathCase.Name << ": the low preset saved nothing";
            // The hand-over still preserves the coat, at a size the card was
            // not cooked for: the bias is a cost trade, not a bald coat.
            EXPECT_GT(coverage, 0.75) << pathCase.Name;
            EXPECT_LT(coverage, 1.30) << pathCase.Name;
        }

        settings.GroomDetailBias = savedBias;
        settings.Path = RenderingPath::Forward;
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
        const u32 strandsCompensated = PassStats().StrandsDrawn;
        const f32 widestCompensation = PassStats().Lod.MaxWidthCompensation;
        const u32 coatCompensated = CountCoatPixels(compensated);

        // THE REFERENCE: every strand at the same size, the LOD off. What the
        // compensation is for is putting the thinned coat back where the full
        // coat is (#1558): above it is as wrong as below it -- the summed width
        // drawn at alpha covered more of a pixel than the strands it replaced.
        Lod().m_Enabled = false;
        std::vector<u8> every;
        Capture("GroomLodEveryStrand_GL_Forward", kPixelSize, every);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        const u32 strandsEvery = PassStats().StrandsDrawn;
        const u32 coatEvery = CountCoatPixels(every);
        Lod().m_Enabled = true;

        std::printf("[groom-lod-evidence] compensation off: %u strands / %u coat px   on: %u strands / %u coat px "
                    "(widest %.2fx)   every strand: %u strands / %u coat px\n",
                    strandsUncompensated, coatUncompensated, strandsCompensated, coatCompensated,
                    static_cast<f64>(widestCompensation), strandsEvery, coatEvery);

        // The two arms must draw the SAME geometry — the compensation is a UBO
        // value, not a rebuild. If they differ, something else moved and the
        // comparison is not about the compensation at all.
        ASSERT_EQ(strandsUncompensated, strandsCompensated)
            << "the two arms drew different amounts of geometry, so this is not a compensation A/B";
        ASSERT_GT(coatUncompensated, 1000u) << "the uncompensated frame has almost no coat in it";

        EXPECT_GT(widestCompensation, 1.5f)
            << "the budget did not thin the coat at this size, so there was nothing to compensate";
        EXPECT_GT(coatCompensated, coatUncompensated)
            << "widening the survivors did not put any coat back on screen";
        // AGAINST THE FULL COAT (#1558), not against the thinned one: the
        // compensation closes a real share of the coverage the thinning took
        // away, and does not overshoot the coat it stands in for. (A fixed
        // "1.15x the thinned coat" bar predated the reference arm, and it
        // demanded an overshoot whenever thinning took less than 15%.)
        ASSERT_GT(coatEvery, coatUncompensated) << "thinning took no coverage away, so there is nothing to put back";
        const f64 gapThinned = static_cast<f64>(coatEvery) - static_cast<f64>(coatUncompensated);
        const f64 gapCompensated = std::abs(static_cast<f64>(coatEvery) - static_cast<f64>(coatCompensated));
        EXPECT_LT(gapCompensated, 0.7 * gapThinned)
            << "the compensation put back less than a third of the coverage the thinning removed";
        EXPECT_LE(static_cast<f64>(coatCompensated), 1.05 * static_cast<f64>(coatEvery))
            << "the compensated coat covers more than the full coat it stands in for";
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
    // #1612: the hand-over in motion
    // =========================================================================

    TEST_F(GroomLodVisualEvidenceTest, ADollyThroughTheHandOverSpreadsTheStepAcrossTheWindow)
    {
        // The coat as a real coat is drawn: stochastic composition under TAA,
        // the configuration whose resolve the cross-fade's dither relies on.
        auto& post = Renderer3D::GetPostProcessSettings();
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        const bool savedTaa = post.TAAEnabled;
        const u32 savedFrames = settings.GroomLodCrossFadeFrames;
        const u32 savedCap = settings.GroomLodMaxCrossFades;
        post.TAAEnabled = true;
        m_GroomEntity.GetComponent<GroomComponent>().m_CompositionMode =
            static_cast<u8>(GroomCompositionMode::StochasticAlpha);
        const u32 window = savedFrames > 0u ? savedFrames : 12u;

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

        for (const PathCase& pathCase : paths)
        {
            settings.Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();
            const std::string cell = std::string("_GL_") + pathCase.Name + "_Dolly";

            // OFF: the hard switch every hand-over was before #1612, the A/B
            // control, on the same walk.
            settings.GroomLodCrossFadeFrames = 0u;
            const DollyResult hard = Dolly("GroomLodCrossFadeOff" + cell, "GroomLodHalvingFadeOff" + cell);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            PrintDolly((std::string(pathCase.Name) + " hard switch").c_str(), hard);

            settings.GroomLodCrossFadeFrames = window;
            settings.GroomLodMaxCrossFades = 4u;
            const DollyResult faded = Dolly("GroomLodCrossFade" + cell, "GroomLodHalvingFade" + cell);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            PrintDolly((std::string(pathCase.Name) + " cross-fade").c_str(), faded);

            ASSERT_EQ(hard.HandOvers, 1u) << pathCase.Name << ": the walk must cross the card hand-over once";
            ASSERT_EQ(faded.HandOvers, 1u) << pathCase.Name;
            // The walk's halvings (#1628): one on the strands before the
            // hand-over and one on the cards after it, on the same frames in
            // both arms, since the fade does not touch the ladder.
            ASSERT_GE(hard.HalvingFrames.size(), 1u) << pathCase.Name << ": the walk crosses no visibility halving";
            ASSERT_EQ(faded.HalvingFrames, hard.HalvingFrames) << pathCase.Name << ": the fade moved the ladder";
            EXPECT_EQ(hard.FadeFrames, 0u) << pathCase.Name << ": a zero-length window faded";
            // One window per tier change, none overlapping on this walk.
            EXPECT_EQ(faded.FadeFrames, window * static_cast<u32>(1u + faded.HalvingFrames.size()))
                << pathCase.Name << ": the coat did not fade for exactly a window at the hand-over and each halving";

            // THE CLAIM: the step at the hand-over is spread. The hard switch
            // lands its whole change in one frame; the fade's largest step must
            // be well under half of it.
            EXPECT_LT(faded.MaxStepNearHandOver, hard.MaxStepNearHandOver * 0.5)
                << pathCase.Name << ": the cross-fade did not spread the hand-over's step";
            // ...and so is the step at each halving (#1628 acceptance: under
            // half of the hard halving's, on every path).
            for (sizet h = 0; h < faded.HalvingFrames.size(); ++h)
            {
                EXPECT_LT(faded.MaxStepNearHalving[h], hard.MaxStepNearHalving[h] * 0.5)
                    << pathCase.Name << ": the halving at frame " << faded.HalvingFrames[h]
                    << " was not spread across its window";
            }
            // ...without changing where the walk arrives: the fade hides the
            // step, it does not change either tier (#1608 is the energy).
            EXPECT_NEAR(faded.TotalChange, hard.TotalChange, 0.02)
                << pathCase.Name << ": the faded walk arrived somewhere else";
        }

        // MSAA 4 ON DEFERRED (#1628's conditional cell): the G-buffer is
        // multisampled and resolved before the grooms draw, so the masks see
        // the same single-sample target; the walk must spread its steps
        // exactly as without it.
        {
            settings.Path = RenderingPath::Deferred;
            // Restored on every exit, an ASSERT's included: a later test must
            // not inherit a multisampled G-buffer.
            struct SampleRestore
            {
                RendererSettings& Settings;
                u32 Samples;
                ~SampleRestore()
                {
                    Settings.Deferred.MSAASampleCount = Samples;
                    Renderer3D::ApplyRendererSettings();
                }
            } restoreSamples{ settings, settings.Deferred.MSAASampleCount };
            settings.Deferred.MSAASampleCount = 4u;
            Renderer3D::ApplyRendererSettings();
            settings.GroomLodCrossFadeFrames = 0u;
            const DollyResult hard = Dolly("", "GroomLodHalvingFadeOff_GL_Deferred_Msaa4_Dolly");
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            PrintDolly("Deferred MSAA 4 hard switch", hard);
            settings.GroomLodCrossFadeFrames = window;
            const DollyResult faded = Dolly("", "GroomLodHalvingFade_GL_Deferred_Msaa4_Dolly");
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            PrintDolly("Deferred MSAA 4 cross-fade", faded);
            ASSERT_EQ(faded.HandOvers, 1u);
            ASSERT_GE(hard.HalvingFrames.size(), 1u);
            ASSERT_EQ(faded.HalvingFrames, hard.HalvingFrames);
            EXPECT_LT(faded.MaxStepNearHandOver, hard.MaxStepNearHandOver * 0.5) << "MSAA 4: the hand-over";
            for (sizet h = 0; h < faded.HalvingFrames.size(); ++h)
            {
                EXPECT_LT(faded.MaxStepNearHalving[h], hard.MaxStepNearHalving[h] * 0.5)
                    << "MSAA 4: the halving at frame " << faded.HalvingFrames[h];
            }
        }

        // TAA OFF: the screen door is visible as noise, but the mask still
        // covers each pixel once, so the coat neither doubles nor holes. Its
        // light through each window -- the hand-over's and every halving's --
        // stays between the two tiers' within the per-frame noise of an
        // unresolved stochastic coat.
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        post.TAAEnabled = false;
        settings.GroomLodCrossFadeFrames = window;
        const DollyResult noTaa =
            Dolly("GroomLodCrossFadeNoTAA_GL_Forward_Dolly", "GroomLodHalvingFadeNoTAA_GL_Forward_Dolly");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        PrintDolly("Forward cross-fade, no TAA", noTaa);
        ASSERT_EQ(noTaa.HandOvers, 1u);
        ASSERT_GE(noTaa.HalvingFrames.size(), 1u);
        EXPECT_EQ(noTaa.FadeFrames, window * static_cast<u32>(1u + noTaa.HalvingFrames.size()));
        std::vector<u32> windows = noTaa.HalvingFrames;
        windows.push_back(noTaa.HandOverFrame);
        for (const u32 h : windows)
        {
            ASSERT_GE(h, 4u);
            ASSERT_LT(h + window + 4u, static_cast<u32>(noTaa.Signal.size()));
            const f64 before = noTaa.Signal[h - 2u];
            const f64 after = noTaa.Signal[h + window + 2u];
            const f64 slack = std::max(0.02, 4.0 * noTaa.NoiseFloor) * std::max(before, after);
            for (u32 f = h; f < h + window; ++f)
            {
                EXPECT_GT(noTaa.Signal[f], std::min(before, after) - slack)
                    << "a hole at frame " << f - h << " of the window opened on frame " << h;
                EXPECT_LT(noTaa.Signal[f], std::max(before, after) + slack)
                    << "a doubled coat at frame " << f - h << " of the window opened on frame " << h;
            }
        }

        // NO SLOT: the hand-over and every halving hard-switch, as before, and
        // say so by name.
        post.TAAEnabled = true;
        settings.GroomLodMaxCrossFades = 0u;
        const DollyResult capped = Dolly("");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        PrintDolly("Forward, cross-fade budget 0", capped);
        EXPECT_EQ(capped.FadeFrames, 0u);
        EXPECT_EQ(capped.Refused, static_cast<u32>(1u + capped.HalvingFrames.size()))
            << "a hard switch was not reported as CrossFadeBudgetSpent";

        settings.GroomLodCrossFadeFrames = savedFrames;
        settings.GroomLodMaxCrossFades = savedCap;
        m_GroomEntity.GetComponent<GroomComponent>().m_CompositionMode =
            static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
        post.TAAEnabled = savedTaa;
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
    }

    TEST_F(GroomLodVisualEvidenceTest, ASecondViewInOneFrameAdvancesNeitherTheLadderNorTheWindow)
    {
        // A probe bake renders the scene six times between two frames, from a
        // camera of its own. Each render publishes the grooms; the LOD's
        // hysteresis and the cross-fade window advance on the frame's first
        // publish only, so the main view's window still lasts its full length
        // and the bake's distance never moves the main view's ladder.
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        const u32 savedFrames = settings.GroomLodCrossFadeFrames;
        const u32 savedCap = settings.GroomLodMaxCrossFades;
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        const u32 window = 12u;
        settings.GroomLodCrossFadeFrames = window;
        settings.GroomLodMaxCrossFades = 4u;

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        // A real light-probe bake: six cube faces, each a full RenderScene3D
        // with the probe's own camera, none of them a frame. Close enough that
        // its 256 px, 90-degree faces see the coat at ~360 px -- strands, while
        // the main view walks out to cards -- so a publish from the bake that
        // decided for itself would ask for strands mid hand-over.
        Ref<Scene> scene = GetSceneRef();
        const glm::vec3 probe(0.0f, 0.35f, EyeDistanceFor(kCardPixelSize * 8.0f));
        const auto renderOtherViews = [&]()
        {
            GLStateGuard guard("ASecondViewInOneFrame", GLStateGuard::Policy::Restore);
            (void)LightProbeBaker::BakeProbeAtPosition(scene, probe, 256u);
        };

        Lod().m_Enabled = false;
        camera.SetPose(glm::vec3(0.0f, 0.35f, EyeDistanceFor(kCardPixelSize * 2.0f)), 0.0f, 0.05f);
        RunEditorFrames(camera, 2);
        Lod().m_Enabled = true;
        RunEditorFrames(camera, 4);

        // The ladder after a frame with six more views is the ladder that
        // frame's own publish left.
        const UUID coat = m_GroomEntity.GetUUID();
        ASSERT_NE(GetScene().FindGroomLodState(coat), nullptr);
        const GroomLodState settled = *GetScene().FindGroomLodState(coat);
        renderOtherViews();
        EXPECT_EQ(*GetScene().FindGroomLodState(coat), settled)
            << "a second view in the same frame advanced the coat's LOD hysteresis";

        // A halving, six other views after every frame (#1628): its window is
        // advanced by the frame like a hand-over's, and the bake draws the
        // frame's mix. Doubling the visibility curve moves the ladder down on
        // the strands (two rungs at this pose, in one change).
        {
            // Past the hold first: four frames in, the ladder is still on its
            // way to its step.
            RunEditorFrames(camera, 30);
            const f32 savedFull = Lod().m_VisibilityFullPixelSize;
            const u32 stepBefore = GetScene().FindGroomLodState(coat)->VisibilityStep;
            Lod().m_VisibilityFullPixelSize = savedFull * 2.0f;
            u32 halvingFade = 0;
            u32 halvingDisagreed = 0;
            for (u32 frame = 0; frame < 30u; ++frame)
            {
                RunEditorFrames(camera, 1);
                const u32 mainFading = PassStats().Lod.CrossFading;
                halvingFade += mainFading > 0u ? 1u : 0u;
                renderOtherViews();
                halvingDisagreed += PassStats().Lod.CrossFading != mainFading ? 1u : 0u;
            }
            std::printf("[groom-crossfade] a halving with a probe bake after every frame: step %u -> %u, fading %u "
                        "frames\n",
                        stepBefore, GetScene().FindGroomLodState(coat)->VisibilityStep, halvingFade);
            EXPECT_GT(GetScene().FindGroomLodState(coat)->VisibilityStep, stepBefore)
                << "the main view's ladder did not move";
            EXPECT_EQ(GetScene().FindGroomLodState(coat)->Representation, GroomRepresentation::Strand);
            EXPECT_EQ(halvingFade, window) << "the halving's window did not last its length with other views in each frame";
            EXPECT_EQ(halvingDisagreed, 0u) << "the probe bake drew a different mix of the halving than its frame";
            // Back on the authored curve, and the walk below starts as it did
            // before this block: a fresh ladder four frames in.
            Lod().m_VisibilityFullPixelSize = savedFull;
            Lod().m_Enabled = false;
            RunEditorFrames(camera, 2);
            Lod().m_Enabled = true;
            RunEditorFrames(camera, 4);
        }

        // Out to cards, six other views after every frame.
        camera.SetPose(glm::vec3(0.0f, 0.35f, EyeDistanceFor(kCardPixelSize * 0.4f)), 0.0f, 0.05f);
        u32 handOvers = 0;
        u32 fadeFrames = 0;
        u32 bakeDisagreed = 0;
        for (u32 frame = 0; frame < 60u; ++frame)
        {
            RunEditorFrames(camera, 1);
            const GroomRenderStats& stats = PassStats();
            handOvers += stats.Lod.RepresentationChanges;
            fadeFrames += stats.Lod.CrossFading > 0u ? 1u : 0u;
            const u32 mainFading = stats.Lod.CrossFading;
            const u32 mainChanges = stats.Lod.RepresentationChanges;
            renderOtherViews();
            // The bake's last face (-Z) looks at the coat: it draws the frame's
            // mix, not one of its own. A bake that decided for itself would
            // open a window on every frame it wanted strands.
            if (PassStats().Lod.CrossFading != mainFading || PassStats().Lod.RepresentationChanges != mainChanges)
            {
                ++bakeDisagreed;
            }
        }
        std::printf("[groom-crossfade] a probe bake after every frame: hand-overs %u, fading %u frames (window %u)\n",
                    handOvers, fadeFrames, window);
        EXPECT_EQ(handOvers, 1u) << "the main view's walk did not hand over exactly once";
        EXPECT_EQ(fadeFrames, window) << "the window did not last its length with other views in each frame";
        EXPECT_EQ(bakeDisagreed, 0u) << "the probe bake drew a different mix of the coat than the frame it ran in";
        ASSERT_NE(GetScene().FindGroomLodState(coat), nullptr);
        EXPECT_EQ(GetScene().FindGroomLodState(coat)->Representation, GroomRepresentation::Card);

        settings.GroomLodCrossFadeFrames = savedFrames;
        settings.GroomLodMaxCrossFades = savedCap;
    }

    TEST_F(GroomLodVisualEvidenceTest, ACoatOutOfViewTakesNoCrossFadeSlot)
    {
        // Two coats hand over on the same frame, with one cross-fade slot: the
        // fixture's coat in view, and a twin behind the camera at the same
        // distance, which casts no shadow and so cannot reach the view at all.
        // The twin draws nothing, so it must not take the slot the visible coat
        // needs. The hand-over is driven by the detail bias at a fixed camera,
        // as in TheCoatsShadowFadesWithItsColour: 300 px is strands at bias 1,
        // cards at 0.5.
        ASSERT_TRUE(Renderer3D::IsFrustumCullingEnabled()) << "without the view cull every coat is drawn";
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        const u32 window = 12u;
        settings.GroomLodCrossFadeFrames = window;
        settings.GroomLodMaxCrossFades = 1u;

        const f32 eye = EyeDistanceFor(300.0f);
        Entity twin = GetScene().DuplicateEntity(m_GroomEntity);
        twin.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 2.0f * eye);
        auto& twinShadow = twin.AddOrReplaceComponent<GroomSceneShadowComponent>();
        twinShadow.m_CastShadows = false;
        twinShadow.m_ReceiveShadows = false;

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose(glm::vec3(0.0f, 0.35f, eye), 0.0f, 0.05f);
        settings.GroomDetailBias = 1.0f;
        Lod().m_Enabled = false;
        RunEditorFrames(camera, 2);
        Lod().m_Enabled = true;
        RunEditorFrames(camera, 24);
        ASSERT_EQ(PassStats().GroomsSubmitted, 1u) << "the twin behind the camera was drawn, so this decides nothing";
        ASSERT_NE(GetScene().FindGroomLodState(twin.GetUUID()), nullptr)
            << "the twin's LOD does not advance out of view, so it cannot hand over beside the visible coat";

        settings.GroomDetailBias = 0.5f;
        u32 handOvers = 0;
        u32 fadeFrames = 0;
        u32 refused = 0;
        for (u32 frame = 0; frame < 24u; ++frame)
        {
            RunEditorFrames(camera, 1);
            handOvers += PassStats().Lod.RepresentationChanges;
            fadeFrames += PassStats().Lod.CrossFading > 0u ? 1u : 0u;
            refused += PassStats().Lod.CrossFadesRefused;
        }
        std::printf("[groom-crossfade] one slot, a twin out of view: hand-overs %u, fading %u frames, refused %u\n",
                    handOvers, fadeFrames, refused);
        ASSERT_EQ(handOvers, 1u) << "the visible coat did not hand over";
        ASSERT_NE(GetScene().FindGroomLodState(twin.GetUUID()), nullptr);
        EXPECT_EQ(GetScene().FindGroomLodState(twin.GetUUID())->Representation, GroomRepresentation::Card)
            << "the twin did not hand over beside it, so the slot was never contested";
        EXPECT_EQ(refused, 0u) << "the visible coat was refused its fade";
        EXPECT_EQ(fadeFrames, window) << "the visible coat did not fade for its window";

        GetScene().DestroyEntity(twin);
    }

    TEST_F(GroomLodVisualEvidenceTest, AFadingCoatThatLeavesTheViewGivesItsSlotToAVisibleOne)
    {
        // One cross-fade slot. A twin in view hands over first and is fading;
        // on the very frame the fixture's coat hands over, the twin leaves
        // every view. Its window closes, and the slot it held goes to the coat
        // that is still drawn -- on THAT frame, not the next.
        //
        // Sizes by the detail bias, 15% hysteresis kept well clear: the coat
        // at 500 px and the twin at 300 px are strands at bias 1; at 0.55 (the
        // edge at 400 px) only the twin hands over; at 0.35 (629 px) the coat.
        ASSERT_TRUE(Renderer3D::IsFrustumCullingEnabled()) << "without the view cull every coat is drawn";
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        const u32 window = 12u;
        settings.GroomLodCrossFadeFrames = window;
        settings.GroomLodMaxCrossFades = 1u;

        const f32 eye = EyeDistanceFor(500.0f);
        const f32 twinEye = EyeDistanceFor(300.0f);
        // Behind the coat, in view; later behind the camera, at the same
        // distance, so its tier does not move when it leaves.
        Entity twin = GetScene().DuplicateEntity(m_GroomEntity);
        twin.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, eye - twinEye);
        auto& twinShadow = twin.AddOrReplaceComponent<GroomSceneShadowComponent>();
        twinShadow.m_CastShadows = false;
        twinShadow.m_ReceiveShadows = false;

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose(glm::vec3(0.0f, 0.35f, eye), 0.0f, 0.05f);
        settings.GroomDetailBias = 1.0f;
        Lod().m_Enabled = false;
        RunEditorFrames(camera, 2);
        Lod().m_Enabled = true;
        RunEditorFrames(camera, 24);
        ASSERT_EQ(PassStats().GroomsSubmitted, 2u) << "both coats must be in view to start";

        const UUID coat = m_GroomEntity.GetUUID();
        const auto state = [&](UUID id)
        { return GetScene().FindGroomLodState(id); };
        ASSERT_NE(state(coat), nullptr);
        ASSERT_NE(state(twin.GetUUID()), nullptr);
        ASSERT_EQ(state(coat)->Representation, GroomRepresentation::Strand);
        ASSERT_EQ(state(twin.GetUUID())->Representation, GroomRepresentation::Strand);

        // The twin hands over and fades; the coat stays.
        settings.GroomDetailBias = 0.55f;
        for (u32 frame = 0; frame < 12u && state(twin.GetUUID())->Representation != GroomRepresentation::Card; ++frame)
        {
            RunEditorFrames(camera, 1);
        }
        ASSERT_EQ(state(twin.GetUUID())->Representation, GroomRepresentation::Card) << "the twin did not hand over";
        ASSERT_EQ(state(coat)->Representation, GroomRepresentation::Strand) << "the coat handed over with the twin";
        RunEditorFrames(camera, 1);
        ASSERT_EQ(PassStats().Lod.CrossFading, 1u) << "the twin is not fading, so it holds no slot to give back";

        // The coat's hand-over, and the twin out of view on exactly its frame:
        // the frame on which the coat's coarser request completes its hold.
        settings.GroomDetailBias = 0.35f;
        const u32 hold = Lod().m_HoldFrames;
        bool moved = false;
        for (u32 frame = 0; frame < 12u && !moved; ++frame)
        {
            const GroomLodState before = *state(coat);
            if (before.RequestedRepresentation == GroomRepresentation::Card &&
                before.RepresentationStableFrames + 1u >= hold)
            {
                twin.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, eye + twinEye);
                moved = true;
            }
            RunEditorFrames(camera, 1);
        }
        ASSERT_TRUE(moved) << "the coat's request never neared the end of its hold";
        const GroomRenderStats& stats = PassStats();
        std::printf("[groom-crossfade] the twin leaves the view as the coat hands over: drawn %u, hand-overs %u, "
                    "fading %u, refused %u\n",
                    stats.GroomsSubmitted, stats.Lod.RepresentationChanges, stats.Lod.CrossFading,
                    stats.Lod.CrossFadesRefused);
        ASSERT_EQ(stats.GroomsSubmitted, 1u) << "the twin was still drawn after it left the view";
        ASSERT_EQ(stats.Lod.RepresentationChanges, 1u) << "the coat did not hand over on the twin's last frame";
        EXPECT_EQ(stats.Lod.CrossFadesRefused, 0u) << "the slot the departed twin held was not given to the coat";
        EXPECT_EQ(stats.Lod.CrossFading, 1u) << "the coat hard-switched on the frame the twin left";

        GetScene().DestroyEntity(twin);
    }

    TEST_F(GroomLodVisualEvidenceTest, ACoatBackInViewFadesFromTheTierItWasLastDrawnOn)
    {
        // A coat's ladder keeps moving out of view. Here the coat is drawn on
        // strands close up, then reaches the cards' fourth halving while the
        // camera looks away from far off; the camera comes back, nearer, and
        // the ladder refines to the cards' second step on the frame the coat
        // is in view again. Nothing on screen ever showed the fourth. The coat
        // fades in from the strands the viewer last saw (#1628): settled on
        // its out-of-view tier, it faded from cards it never drew; dropped,
        // it switched hard.
        ASSERT_TRUE(Renderer3D::IsFrustumCullingEnabled()) << "without the view cull every coat is drawn";
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        settings.GroomLodCrossFadeFrames = 12u;
        settings.GroomLodMaxCrossFades = 4u;
        // Its shadow must not reach the view either, or it is never out of it.
        auto& routing = m_GroomEntity.AddOrReplaceComponent<GroomSceneShadowComponent>();
        routing.m_CastShadows = false;
        routing.m_ReceiveShadows = false;
        // A twin where the far camera looks, so the pass still runs -- and
        // reports -- on the frames the coat is out of view: a pass with
        // nothing to draw keeps its last frame's counters.
        const f32 farEye = EyeDistanceFor(80.0f);
        Entity twin = GetScene().DuplicateEntity(m_GroomEntity);
        twin.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, farEye + 30.0f);

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        const UUID coat = m_GroomEntity.GetUUID();
        camera.SetPose(glm::vec3(0.0f, 0.35f, EyeDistanceFor(300.0f)), 0.0f, 0.05f);
        Lod().m_Enabled = false;
        RunEditorFrames(camera, 2);
        Lod().m_Enabled = true;
        RunEditorFrames(camera, 30);
        ASSERT_EQ(PassStats().GroomsSubmitted, 1u);

        // Far off and looking away: cards, a coarse step, nothing drawn.
        camera.SetPose(glm::vec3(0.0f, 0.35f, farEye), 3.14159265f, 0.05f);
        RunEditorFrames(camera, 30);
        ASSERT_EQ(PassStats().GroomsSubmitted, 1u) << "the coat is still in view, or the twin is not";
        ASSERT_NE(GetScene().FindGroomLodState(coat), nullptr);
        const GroomLodState away = *GetScene().FindGroomLodState(coat);
        ASSERT_EQ(away.Representation, GroomRepresentation::Card);

        // Back, nearer: still cards, a finer step from the first frame.
        camera.SetPose(glm::vec3(0.0f, 0.35f, EyeDistanceFor(260.0f)), 0.0f, 0.05f);
        RunEditorFrames(camera, 1);
        const GroomLodState back = *GetScene().FindGroomLodState(coat);
        const u32 fadingOnReturn = PassStats().Lod.CrossFading;
        // The outgoing copy is a strand tier: the strands drawn this frame are
        // that copy's, the primary being cards.
        const u32 strandsOnReturn =
            PassStats().Lod.StrandsByRepresentation[static_cast<sizet>(GroomRepresentation::Strand)];
        u32 fadingFrames = fadingOnReturn > 0u ? 1u : 0u;
        for (u32 frame = 0; frame < 16u; ++frame)
        {
            RunEditorFrames(camera, 1);
            fadingFrames += PassStats().Lod.CrossFading > 0u ? 1u : 0u;
        }
        std::printf("[groom-crossfade] back in view: away on %s step %u, back on %s step %u, fading %u on the "
                    "frame with %u strand-tier strands, %u fading frames\n",
                    away.Representation == GroomRepresentation::Card ? "cards" : "strands", away.VisibilityStep,
                    back.Representation == GroomRepresentation::Card ? "cards" : "strands", back.VisibilityStep,
                    fadingOnReturn, strandsOnReturn, fadingFrames);
        ASSERT_EQ(back.Representation, GroomRepresentation::Card);
        ASSERT_LT(back.VisibilityStep, away.VisibilityStep) << "the ladder did not refine on the return frame";
        EXPECT_EQ(fadingOnReturn, 1u) << "the coat switched hard from the strands the viewer last saw";
        EXPECT_GT(strandsOnReturn, 0u) << "the coat faded in from a tier other than the strands it was last drawn on";
        EXPECT_EQ(fadingFrames, 12u);

        GetScene().DestroyEntity(twin);
        m_GroomEntity.RemoveComponent<GroomSceneShadowComponent>();
    }

    TEST_F(GroomLodVisualEvidenceTest, AFadeWhoseTwoTiersStreamToOneBuildGivesItsSlotBack)
    {
        // Under a representation budget too small for any detail, every coat
        // draws its streaming floor -- this fixture's 2500 cooked cards -- and
        // a halving's two tiers resolve to that same build, so Scene drops the
        // outgoing copy: there is nothing to fade between. The window must go
        // with it. Left open, it kept its slot for its whole length while the
        // coat drew one tier, and with one slot a twin's hand-over two frames
        // later was refused.
        ASSERT_TRUE(Renderer3D::IsFrustumCullingEnabled()) << "without the view cull every coat is drawn";
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        settings.GroomLodCrossFadeFrames = 12u;
        settings.GroomLodMaxCrossFades = 1u;
        StreamingSettings& streaming = GetScene().GetStreamingSettings();
        const StreamingSettings savedStreaming = streaming;
        streaming.RepresentationResidentMegabytes = 0.001f;

        // The twin beside the coat, both in view, the twin a little farther
        // and still on strands.
        const f32 eye = EyeDistanceFor(300.0f);
        Entity twin = GetScene().DuplicateEntity(m_GroomEntity);
        twin.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 0.0f, 0.0f);
        auto& twinShadow = twin.AddOrReplaceComponent<GroomSceneShadowComponent>();
        twinShadow.m_CastShadows = false;
        twinShadow.m_ReceiveShadows = false;

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose(glm::vec3(2.5f, 0.35f, eye), 0.0f, 0.05f);
        Lod().m_Enabled = false;
        RunEditorFrames(camera, 2);
        Lod().m_Enabled = true;
        RunEditorFrames(camera, 48);
        const auto state = [&](UUID id)
        { return GetScene().FindGroomLodState(id); };
        const UUID coat = m_GroomEntity.GetUUID();
        ASSERT_EQ(PassStats().GroomsSubmitted, 2u) << "both coats must be in view";
        ASSERT_NE(state(coat), nullptr);
        ASSERT_NE(state(twin.GetUUID()), nullptr);
        ASSERT_EQ(state(twin.GetUUID())->Representation, GroomRepresentation::Strand);
        const FGroomStreamingStats& streamed = Renderer3D::GetGroomRenderPass()->GetStreamingStats();
        ASSERT_GE(streamed.FallbackDraws, 2u) << "the coats are not on their streaming floor";
        ASSERT_EQ(streamed.DetailDraws, 0u) << "a coat drew its detail, so no halving lands on the floor";

        // The coat halves; two frames later the twin hands over.
        const f32 savedFull = Lod().m_VisibilityFullPixelSize;
        const u32 stepBefore = state(coat)->VisibilityStep;
        Lod().m_VisibilityFullPixelSize = savedFull * 2.0f;
        u32 refused = 0;
        for (u32 frame = 0; frame < 24u; ++frame)
        {
            if (frame == 2u)
            {
                twin.GetComponent<GroomLodComponent>().m_CardPixelSize = 4096.0f;
            }
            RunEditorFrames(camera, 1);
            refused += PassStats().Lod.CrossFadesRefused;
        }
        std::printf("[groom-crossfade] a halving on the streaming floor, then a twin's hand-over, one slot: coat step "
                    "%u -> %u, twin on %s, refused %u\n",
                    stepBefore, state(coat)->VisibilityStep,
                    state(twin.GetUUID())->Representation == GroomRepresentation::Card ? "cards" : "strands", refused);
        EXPECT_GT(state(coat)->VisibilityStep, stepBefore) << "the coat did not halve";
        EXPECT_EQ(state(twin.GetUUID())->Representation, GroomRepresentation::Card) << "the twin did not hand over";
        EXPECT_EQ(refused, 0u) << "a window that drew one tier kept the slot the twin's hand-over needed";

        Lod().m_VisibilityFullPixelSize = savedFull;
        streaming = savedStreaming;
        GetScene().DestroyEntity(twin);
    }

    TEST_F(GroomLodVisualEvidenceTest, TheCoatsShadowFadesWithItsColour)
    {
        // #1612: a fading coat's two tiers cast through the masks they are
        // drawn with. Without them both tiers would cast whole for the window
        // and the ground under the coat would go darker than under either
        // tier -- a shadow that pulses at every hand-over. The hand-over is
        // driven by the detail bias (#1616) at a FIXED camera, so the ground,
        // the light and the view are the same picture on every frame and the
        // only thing that changes is the tier.
        Scene& scene = GetScene();
        auto& post = Renderer3D::GetPostProcessSettings();
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        const bool savedTaa = post.TAAEnabled;
        const f32 savedBias = settings.GroomDetailBias;
        const u32 savedFrames = settings.GroomLodCrossFadeFrames;
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        post.TAAEnabled = true;
        m_GroomEntity.GetComponent<GroomComponent>().m_CompositionMode =
            static_cast<u8>(GroomCompositionMode::StochasticAlpha);

        // A pale ground just under the coat, and a coat that casts onto it but
        // does not receive, so the coat's own pixels do not move with the cast.
        Entity ground = scene.CreateEntity("Ground");
        {
            auto& tc = ground.GetComponent<TransformComponent>();
            tc.Translation = glm::vec3(0.0f, -1.45f, 0.0f);
            tc.Scale = glm::vec3(20.0f, 1.0f, 20.0f);
            auto& mc = ground.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Plane;
            if (Ref<Mesh> mesh = MeshPrimitives::CreatePlane())
            {
                mc.m_MeshSource = mesh->GetMeshSource();
            }
            ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.62f, 1.0f));
        }
        auto& routing = m_GroomEntity.AddComponent<GroomSceneShadowComponent>();
        routing.m_ReceiveShadows = false;
        routing.m_CastShadows = true;

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        // 300 px: strands at bias 1, cards at 0.5 (the 220 px edge becomes 440).
        camera.SetPose(glm::vec3(0.0f, 1.6f, EyeDistanceFor(300.0f)), 0.0f, 0.3f);

        std::vector<u8> pixels;
        const auto readLuma = [&](std::vector<f64>& luma)
        {
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            ASSERT_TRUE(fb);
            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, pixels);
            luma.resize(static_cast<sizet>(kWidth) * kHeight);
            for (sizet i = 0; i < luma.size(); ++i)
            {
                luma[i] = (0.2126 * pixels[i * 4u] + 0.7152 * pixels[i * 4u + 1u] + 0.0722 * pixels[i * 4u + 2u]) / 255.0;
            }
        };
        // A settled, resolved frame on one tier.
        const auto settled = [&](f32 bias, bool cast, std::vector<f64>& luma)
        {
            settings.GroomDetailBias = bias;
            routing.m_CastShadows = cast;
            Lod().m_Enabled = false;
            RunEditorFrames(camera, 2);
            Lod().m_Enabled = true;
            RunEditorFrames(camera, 48);
            readLuma(luma);
        };

        std::vector<f64> strandCast, strandBare, cardCast, cardBare;
        settled(1.0f, true, strandCast);
        settled(1.0f, false, strandBare);
        settled(0.5f, true, cardCast);
        settled(0.5f, false, cardBare);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        ASSERT_EQ(PassStats().Lod.ByRepresentation[static_cast<sizet>(GroomRepresentation::Card)], 1u);

        // THE SHADOW'S PIXELS: the ground that either tier's cast darkens, and
        // that is not coat in either tier (the two silhouettes differ).
        std::vector<u8> mask(strandCast.size(), 0u);
        u32 maskPixels = 0;
        for (sizet i = 0; i < mask.size(); ++i)
        {
            const bool shadowed = (strandBare[i] - strandCast[i]) > 0.03 || (cardBare[i] - cardCast[i]) > 0.03;
            const bool sameGround = std::abs(strandBare[i] - cardBare[i]) < 0.02;
            if (shadowed && sameGround)
            {
                mask[i] = 1u;
                ++maskPixels;
            }
        }
        const auto shadowLight = [&](const std::vector<f64>& luma)
        {
            f64 sum = 0.0;
            for (sizet i = 0; i < luma.size(); ++i)
            {
                sum += mask[i] ? luma[i] : 0.0;
            }
            return sum / std::max<u32>(maskPixels, 1u);
        };
        const f64 underStrands = shadowLight(strandCast);
        const f64 underCards = shadowLight(cardCast);
        const f64 bare = shadowLight(strandBare);
        std::printf("[groom-crossfade-shadow] %u shadow px   ground light: bare %.4f   under strands %.4f   under cards "
                    "%.4f\n",
                    maskPixels, bare, underStrands, underCards);
        ASSERT_GT(maskPixels, 500u) << "the coat casts no measurable shadow onto the ground, so this decides nothing";
        ASSERT_LT(underStrands, bare - 0.05) << "the strands' shadow is too faint to measure a fade on";

        // THE HAND-OVER, frame by frame, at the same pose: strands settled,
        // then the bias that hands over.
        const auto handOver = [&](u32 window, const char* label, const std::string& saveAs)
        {
            settings.GroomLodCrossFadeFrames = window;
            std::vector<f64> luma;
            settled(1.0f, true, luma);
            settings.GroomDetailBias = 0.5f;
            std::vector<f64> series;
            for (u32 f = 0; f < 24u; ++f)
            {
                RunEditorFrames(camera, 1);
                readLuma(luma);
                series.push_back(shadowLight(luma));
                if (f == window / 2u && !saveAs.empty())
                {
                    std::vector<u8> flipped(pixels.size());
                    const sizet row = static_cast<sizet>(kWidth) * 4u;
                    for (u32 y = 0; y < kHeight; ++y)
                    {
                        std::memcpy(flipped.data() + y * row, pixels.data() + (kHeight - 1u - y) * row, row);
                    }
                    WriteEvidence(saveAs, flipped, kWidth, kHeight);
                }
            }
            std::printf("[groom-crossfade-shadow] %-12s ground light under the coat:", label);
            for (const f64 v : series)
            {
                std::printf(" %.4f", v);
            }
            std::printf("\n");
            return series;
        };
        const std::vector<f64> hard = handOver(0u, "hard switch", "");
        const std::vector<f64> faded = handOver(12u, "cross-fade", "GroomLodCrossFadeShadow_GL_Forward");

        // THE SHADOW'S STEP IS SPREAD WITH THE COLOUR'S. What two casters
        // without the mask do on THIS coat is not a darker shadow -- the cards'
        // shadow lies inside the strands', so the pair casts exactly the
        // strands' shadow -- but a shadow that holds the outgoing tier for the
        // whole window and then steps at its END, a pop moved twelve frames
        // rather than removed. Measured with the caster discard taken out of
        // include/GroomStrandDepthFragment.glsl: the largest step was 0.86 of the hard
        // switch's, against 0.31 masked. So the claim is the step, as for the
        // colour: the fade's largest frame-to-frame step in the ground's light
        // is under half the hard switch's.
        const auto largestStep = [](const std::vector<f64>& series)
        {
            f64 largest = 0.0;
            for (sizet f = 1; f < series.size(); ++f)
            {
                largest = std::max(largest, std::abs(series[f] - series[f - 1]));
            }
            return largest;
        };
        const f64 hardStep = largestStep(hard);
        const f64 fadedStep = largestStep(faded);
        std::printf("[groom-crossfade-shadow] largest step: hard switch %.4f   cross-fade %.4f (%.2f of it)\n", hardStep,
                    fadedStep, fadedStep / std::max(hardStep, 1.0e-9));
        EXPECT_GT(hardStep, 4.0 * 0.003) << "the hard switch's shadow step is too small to judge a spread against";
        EXPECT_LT(fadedStep, 0.5 * hardStep) << "the fading coat's shadow did not fade with its colour";
        // And it lands where the cards' shadow is, without a hole or a doubled
        // shadow on the way: every frame between the two tiers' shadows, with a
        // little room for the resolve.
        const f64 lo = std::min(underStrands, underCards) - 0.003;
        const f64 hi = std::max(underStrands, underCards) + 0.003;
        for (sizet f = 0; f < faded.size(); ++f)
        {
            EXPECT_GT(faded[f], lo) << "frame " << f << ": the ground under the fading coat went darker than "
                                                        "under either tier";
            EXPECT_LT(faded[f], hi) << "frame " << f << ": a hole in the fading coat's shadow";
        }

        // A HALVING CASTS THROUGH THE MASKS TOO (#1628). The same pose, the
        // coat on strands throughout; doubling the visibility curve's full
        // size moves the ladder exactly one rung, so the only change is the
        // strand budget. Its outgoing copy is a strand tier like the primary,
        // through the same caster programs, and a missing caster mask shows as
        // it did for the hand-over: the shadow holds and steps at the end.
        const f32 savedFull = Lod().m_VisibilityFullPixelSize;
        const auto halving = [&](u32 window, const char* label, const std::string& saveAs)
        {
            settings.GroomLodCrossFadeFrames = window;
            Lod().m_VisibilityFullPixelSize = savedFull;
            std::vector<f64> luma;
            settled(1.0f, true, luma);
            const u32 stepBefore = GetScene().FindGroomLodState(m_GroomEntity.GetUUID())->VisibilityStep;
            Lod().m_VisibilityFullPixelSize = savedFull * 2.0f;
            std::vector<f64> series;
            u32 fadeFrames = 0;
            for (u32 f = 0; f < 24u; ++f)
            {
                RunEditorFrames(camera, 1);
                fadeFrames += PassStats().Lod.CrossFading > 0u ? 1u : 0u;
                readLuma(luma);
                series.push_back(shadowLight(luma));
                if (f == window / 2u && !saveAs.empty())
                {
                    std::vector<u8> flipped(pixels.size());
                    const sizet row = static_cast<sizet>(kWidth) * 4u;
                    for (u32 y = 0; y < kHeight; ++y)
                    {
                        std::memcpy(flipped.data() + y * row, pixels.data() + (kHeight - 1u - y) * row, row);
                    }
                    WriteEvidence(saveAs, flipped, kWidth, kHeight);
                }
            }
            const u32 stepAfter = GetScene().FindGroomLodState(m_GroomEntity.GetUUID())->VisibilityStep;
            std::printf("[groom-crossfade-shadow] %-12s halving %u -> %u, fading %u frames, ground light:", label,
                        stepBefore, stepAfter, fadeFrames);
            for (const f64 v : series)
            {
                std::printf(" %.4f", v);
            }
            std::printf("\n");
            EXPECT_EQ(stepAfter, stepBefore + 1u) << label << ": doubling the curve did not move the ladder one rung";
            EXPECT_EQ(PassStats().Lod.ByRepresentation[static_cast<sizet>(GroomRepresentation::Strand)], 1u)
                << label << ": the coat left the strands, so this is not a halving alone";
            EXPECT_EQ(fadeFrames, window) << label;
            return series;
        };
        const std::vector<f64> hardHalving = halving(0u, "hard halving", "");
        const std::vector<f64> fadedHalving = halving(12u, "faded halving", "GroomLodHalvingFadeShadow_GL_Forward");
        Lod().m_VisibilityFullPixelSize = savedFull;
        // The width compensation keeps a halving's coverage, so its shadow
        // step is a fifth of the hand-over's: judged against the series' own
        // quiet frames (the hold's, before the ladder moves) rather than the
        // hand-over's floor.
        const auto quietStep = [](const std::vector<f64>& series)
        {
            f64 largest = 0.0;
            for (sizet f = 1; f < std::min<sizet>(series.size(), 4u); ++f)
            {
                largest = std::max(largest, std::abs(series[f] - series[f - 1]));
            }
            return largest;
        };
        const f64 hardHalvingStep = largestStep(hardHalving);
        const f64 fadedHalvingStep = largestStep(fadedHalving);
        const f64 halvingNoise = std::max({ quietStep(hardHalving), quietStep(fadedHalving), 1.0e-4 });
        std::printf("[groom-crossfade-shadow] halving largest step: hard %.4f   cross-fade %.4f (%.2f of it)   quiet "
                    "frames %.4f\n",
                    hardHalvingStep, fadedHalvingStep, fadedHalvingStep / std::max(hardHalvingStep, 1.0e-9),
                    halvingNoise);
        EXPECT_GT(hardHalvingStep, 10.0 * halvingNoise)
            << "the hard halving's shadow step is too small to judge a spread against";
        EXPECT_LT(fadedHalvingStep, 0.5 * hardHalvingStep) << "the halving coat's shadow did not fade with its colour";

        m_GroomEntity.RemoveComponent<GroomSceneShadowComponent>();
        scene.DestroyEntity(ground);
        settings.GroomDetailBias = savedBias;
        settings.GroomLodCrossFadeFrames = savedFrames;
        post.TAAEnabled = savedTaa;
        m_GroomEntity.GetComponent<GroomComponent>().m_CompositionMode =
            static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
    }

    TEST_F(GroomLodVisualEvidenceTest, AHalvingsShadowFadesWithItsColourUnderASpotLight)
    {
        // #1628, the atlas cell: a spot light casts the coat into the
        // local-light shadow atlas, and a halving's two tiers must cast there
        // through their masks as they do into the cascades. The sun casts
        // nothing here, so the shadow on the ground is the atlas's alone.
        //
        // TAA OFF, OPAQUE COAT: under TAA the atlas shadow on the ground
        // swings 0.003 in an eight-frame cycle with the camera jitter, which
        // is as large as the halving's whole step, so neither arm could be
        // told from the other. Without it the ground is steady and the
        // window's dither averages over thousands of shadow texels.
        Scene& scene = GetScene();
        auto& post = Renderer3D::GetPostProcessSettings();
        RendererSettings& settings = Renderer3D::GetRendererSettings();
        // Restored on every exit: TAA is a post-process setting, which the
        // fixture does not reset between tests.
        struct TaaRestore
        {
            bool& Flag;
            bool Saved;
            ~TaaRestore()
            {
                Flag = Saved;
            }
        } restoreTaa{ post.TAAEnabled, post.TAAEnabled };
        const u32 savedFrames = settings.GroomLodCrossFadeFrames;
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        post.TAAEnabled = false;
        settings.GroomDetailBias = 1.0f;

        // The sun is taken out, not just told not to cast: the cascades still
        // drew the coat with its m_CastShadows off.
        std::vector<std::pair<Entity, DirectionalLightComponent>> suns;
        for (const auto e : scene.GetAllEntitiesWith<DirectionalLightComponent>())
        {
            Entity sun{ e, &scene };
            suns.emplace_back(sun, sun.GetComponent<DirectionalLightComponent>());
        }
        for (auto& [sun, light] : suns)
        {
            sun.RemoveComponent<DirectionalLightComponent>();
        }
        Entity ground = scene.CreateEntity("Ground");
        {
            auto& tc = ground.GetComponent<TransformComponent>();
            tc.Translation = glm::vec3(0.0f, -1.45f, 0.0f);
            tc.Scale = glm::vec3(20.0f, 1.0f, 20.0f);
            auto& mc = ground.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Plane;
            if (Ref<Mesh> mesh = MeshPrimitives::CreatePlane())
            {
                mc.m_MeshSource = mesh->GetMeshSource();
            }
            ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.62f, 1.0f));
        }
        Entity lamp = scene.CreateEntity("Lamp");
        {
            const glm::vec3 position(1.5f, 5.0f, 1.5f);
            lamp.GetComponent<TransformComponent>().Translation = position;
            auto& spot = lamp.AddComponent<SpotLightComponent>();
            spot.m_Direction = glm::normalize(-position);
            spot.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
            spot.m_Intensity = 80.0f;
            spot.m_Range = 14.0f;
            spot.m_InnerCutoff = 30.0f;
            spot.m_OuterCutoff = 36.0f;
            spot.m_CastShadows = true;
        }
        auto& routing = m_GroomEntity.AddComponent<GroomSceneShadowComponent>();
        routing.m_ReceiveShadows = false;
        routing.m_CastShadows = true;

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose(glm::vec3(0.0f, 1.6f, EyeDistanceFor(300.0f)), 0.0f, 0.3f);

        std::vector<u8> pixels;
        const auto readLuma = [&](std::vector<f64>& luma)
        {
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            ASSERT_TRUE(fb);
            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, pixels);
            luma.resize(static_cast<sizet>(kWidth) * kHeight);
            for (sizet i = 0; i < luma.size(); ++i)
            {
                luma[i] = (0.2126 * pixels[i * 4u] + 0.7152 * pixels[i * 4u + 1u] + 0.0722 * pixels[i * 4u + 2u]) / 255.0;
            }
        };
        const f32 savedFull = Lod().m_VisibilityFullPixelSize;
        const auto settled = [&](bool cast, std::vector<f64>& luma)
        {
            routing.m_CastShadows = cast;
            Lod().m_VisibilityFullPixelSize = savedFull;
            Lod().m_Enabled = false;
            RunEditorFrames(camera, 2);
            Lod().m_Enabled = true;
            RunEditorFrames(camera, 48);
            readLuma(luma);
        };
        std::vector<f64> cast, bare;
        settled(false, bare);
        settled(true, cast);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        ASSERT_GT(PassStats().SceneShadow.AtlasDraws, 0u) << "the coat cast nothing into the atlas";
        std::vector<u8> mask(cast.size(), 0u);
        u32 maskPixels = 0;
        f64 bareLight = 0.0;
        f64 castLight = 0.0;
        for (sizet i = 0; i < mask.size(); ++i)
        {
            if (bare[i] - cast[i] > 0.03)
            {
                mask[i] = 1u;
                ++maskPixels;
                bareLight += bare[i];
                castLight += cast[i];
            }
        }
        const auto shadowLight = [&](const std::vector<f64>& luma)
        {
            f64 sum = 0.0;
            for (sizet i = 0; i < luma.size(); ++i)
            {
                sum += mask[i] ? luma[i] : 0.0;
            }
            return sum / std::max<u32>(maskPixels, 1u);
        };
        std::printf("[groom-crossfade-atlas] %u shadow px   ground light: bare %.4f   under the coat %.4f\n", maskPixels,
                    bareLight / std::max<u32>(maskPixels, 1u), castLight / std::max<u32>(maskPixels, 1u));
        ASSERT_GT(maskPixels, 500u) << "the coat casts no measurable atlas shadow onto the ground";

        const auto halving = [&](u32 window, const char* label, const std::string& saveAs)
        {
            settings.GroomLodCrossFadeFrames = window;
            std::vector<f64> luma;
            settled(true, luma);
            const u32 stepBefore = GetScene().FindGroomLodState(m_GroomEntity.GetUUID())->VisibilityStep;
            Lod().m_VisibilityFullPixelSize = savedFull * 2.0f;
            std::vector<f64> series;
            for (u32 f = 0; f < 24u; ++f)
            {
                RunEditorFrames(camera, 1);
                readLuma(luma);
                series.push_back(shadowLight(luma));
                if (f == window / 2u && !saveAs.empty())
                {
                    std::vector<u8> flipped(pixels.size());
                    const sizet row = static_cast<sizet>(kWidth) * 4u;
                    for (u32 y = 0; y < kHeight; ++y)
                    {
                        std::memcpy(flipped.data() + y * row, pixels.data() + (kHeight - 1u - y) * row, row);
                    }
                    WriteEvidence(saveAs, flipped, kWidth, kHeight);
                }
            }
            std::printf("[groom-crossfade-atlas] %-13s halving %u -> %u, ground light:", label, stepBefore,
                        GetScene().FindGroomLodState(m_GroomEntity.GetUUID())->VisibilityStep);
            for (const f64 v : series)
            {
                std::printf(" %.4f", v);
            }
            std::printf("\n");
            EXPECT_GT(GetScene().FindGroomLodState(m_GroomEntity.GetUUID())->VisibilityStep, stepBefore) << label;
            return series;
        };
        const std::vector<f64> hard = halving(0u, "hard halving", "GroomLodHalvingFadeShadowOff_GL_Forward_Atlas");
        const std::vector<f64> faded = halving(12u, "faded halving", "GroomLodHalvingFadeShadow_GL_Forward_Atlas");
        const auto largestStep = [](const std::vector<f64>& series, sizet to)
        {
            f64 largest = 0.0;
            for (sizet f = 1; f < std::min(series.size(), to); ++f)
            {
                largest = std::max(largest, std::abs(series[f] - series[f - 1]));
            }
            return largest;
        };
        // The hold's frames, before the ladder moves, are the series' noise:
        // bit-identical frames without TAA, so the floor is the readback's.
        const f64 noise = std::max({ largestStep(hard, 4u), largestStep(faded, 4u), 1.0e-5 });
        const f64 hardStep = largestStep(hard, hard.size());
        const f64 fadedStep = largestStep(faded, faded.size());
        std::printf("[groom-crossfade-atlas] halving largest step: hard %.4f   cross-fade %.4f (%.2f of it)   quiet "
                    "frames %.4f\n",
                    hardStep, fadedStep, fadedStep / std::max(hardStep, 1.0e-9), noise);
        EXPECT_GT(hardStep, 10.0 * noise) << "the hard halving's atlas shadow step is too small to judge";
        EXPECT_LT(fadedStep, 0.5 * hardStep) << "the halving coat's atlas shadow did not fade with its colour";

        Lod().m_VisibilityFullPixelSize = savedFull;
        m_GroomEntity.RemoveComponent<GroomSceneShadowComponent>();
        scene.DestroyEntity(lamp);
        scene.DestroyEntity(ground);
        for (auto& [sun, light] : suns)
        {
            sun.AddComponent<DirectionalLightComponent>(light);
        }
        settings.GroomLodCrossFadeFrames = savedFrames;
    }

    // =========================================================================
    // Criterion 3: the three budgets scale down independently
    // =========================================================================

    TEST_F(GroomLodVisualEvidenceTest, TheFirstSteadyFrameAfterAFadeRedrawsTheCoatsVirtualShadow)
    {
        // A fading tier casts a dithered silhouette into the VSM's cached
        // pages. The frame after the window closes draws the incoming tier
        // whole, and its pages must be redrawn: on a coat that neither deforms
        // nor moves, nothing else would. The outgoing tier's departure only
        // redraws ITS footprint, and each tier's comes from its own geometry.
        auto& shadowMap = Renderer3D::GetShadowMap();
        const auto setVsm = [&shadowMap](bool enabled)
        {
            ShadowSettings vsmSettings = shadowMap.GetSettings();
            vsmSettings.VSM.Enabled = enabled;
            shadowMap.SetSettings(vsmSettings);
            return shadowMap.IsVirtualShadowMapActive() == enabled;
        };
        struct VsmRestore
        {
            std::function<bool(bool)> Set;
            ~VsmRestore()
            {
                (void)Set(false);
            }
        } restore{ setVsm };
        if (!setVsm(true))
        {
            GTEST_SKIP() << "the Virtual Shadow Map could not start on this device (VirtualShadowMap::Init refused)";
        }

        RendererSettings& settings = Renderer3D::GetRendererSettings();
        const f32 savedBias = settings.GroomDetailBias;
        const u32 savedFrames = settings.GroomLodCrossFadeFrames;
        const u32 savedCap = settings.GroomLodMaxCrossFades;
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        const u32 window = 12u;
        settings.GroomLodCrossFadeFrames = window;
        settings.GroomLodMaxCrossFades = 4u;
        auto& routing = m_GroomEntity.AddComponent<GroomSceneShadowComponent>();
        routing.m_ReceiveShadows = false;
        routing.m_CastShadows = true;

        // The hand-over by the detail bias at a fixed camera, as in
        // TheCoatsShadowFadesWithItsColour: 300 px is strands at bias 1 and
        // cards at 0.5.
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 4000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose(glm::vec3(0.0f, 1.6f, EyeDistanceFor(300.0f)), 0.0f, 0.3f);
        // A fade, from a settled coat at bias 1, and the frames after it.
        // `change` moves the tier: the bias hands the coat over, a doubled
        // visibility curve halves its strand budget (#1628).
        const f32 savedFull = Lod().m_VisibilityFullPixelSize;
        const auto fadeEnd = [&](const char* label, const std::function<void()>& change)
        {
            settings.GroomDetailBias = 1.0f;
            Lod().m_VisibilityFullPixelSize = savedFull;
            Lod().m_Enabled = false;
            RunEditorFrames(camera, 2);
            Lod().m_Enabled = true;
            RunEditorFrames(camera, 48);
            ASSERT_TRUE(PassStats().SceneShadow.VirtualShadowMapActive)
                << "this is the VSM cell; the frame ran the cascades";
            ASSERT_EQ(PassStats().SceneShadow.VirtualShadowInvalidations, 0u)
                << "a steady coat invalidates its VSM pages every frame, so this cannot see a fade's end";

            change();
            std::vector<u32> fading;
            std::vector<u32> invalidations;
            for (u32 frame = 0; frame < 24u; ++frame)
            {
                RunEditorFrames(camera, 1);
                fading.push_back(PassStats().Lod.CrossFading);
                invalidations.push_back(PassStats().SceneShadow.VirtualShadowInvalidations);
            }
            std::printf("[groom-crossfade-vsm] %-9s fading/invalidations per frame:", label);
            for (sizet f = 0; f < fading.size(); ++f)
            {
                std::printf(" %u/%u", fading[f], invalidations[f]);
            }
            std::printf("\n");

            const auto lastFade = std::find_if(fading.rbegin(), fading.rend(), [](u32 n)
                                               { return n > 0u; });
            ASSERT_NE(lastFade, fading.rend()) << label << ": the coat did not change tier through a fade";
            const sizet firstSteady = static_cast<sizet>(fading.rend() - lastFade);
            ASSERT_LT(firstSteady + 1u, fading.size());
            // The first steady frame redraws the outgoing tier's departed
            // footprint AND the incoming tier's own, which last frame was
            // dithered; the frame after it redraws nothing.
            EXPECT_EQ(invalidations[firstSteady], 2u)
                << label << ": the first frame after the fade did not redraw the incoming tier's pages";
            EXPECT_EQ(invalidations[firstSteady + 1u], 0u) << label << ": a steady coat kept invalidating after its fade";
        };
        fadeEnd("hand-over", [&]
                { settings.GroomDetailBias = 0.5f; });
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        fadeEnd("halving", [&]
                { Lod().m_VisibilityFullPixelSize = savedFull * 2.0f; });
        Lod().m_VisibilityFullPixelSize = savedFull;

        m_GroomEntity.RemoveComponent<GroomSceneShadowComponent>();
        settings.GroomDetailBias = savedBias;
        settings.GroomLodCrossFadeFrames = savedFrames;
        settings.GroomLodMaxCrossFades = savedCap;
    }

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
