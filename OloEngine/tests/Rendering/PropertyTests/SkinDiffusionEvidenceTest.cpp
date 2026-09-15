// OLO_TEST_LAYER: L8
// =============================================================================
// SkinDiffusionEvidenceTest.cpp — the diffusion must SOFTEN THE TERMINATOR,
// BLEED RED PAST IT, and LEAVE THE HIGHLIGHT ALONE. Issue #1241.
//
// WHY THIS TEST SHAPE. The issue's title is the whole specification: diffusion
// that preserves sharp surface specular. Every one of the three claims in it can
// fail on its own, and two of the three failures produce a frame that looks
// plausible:
//
//   * a diffusion that does nothing (the kernel never reached the shader, the
//     radius came out under half a texel, the slot lane decoded wrong) renders
//     exactly the pre-#1241 frame, which is a correct-looking head;
//   * a diffusion that blurred the COMPOSITE rather than the diffuse half
//     renders a soft, waxy head that a screenshot reviewer can easily accept —
//     it is the classic failure the issue names, and no numeric CPU test
//     anywhere else in the suite would notice;
//   * a diffusion with no per-channel weighting renders a grey blur, which is
//     the difference between skin and wax and is again only visible if measured.
//
// So each claim is measured, A against B, on the SAME scene with the ONLY
// difference being the renderer's diffusion switch:
//
//   1. TERMINATOR SOFTNESS. The steepest luminance gradient across the shadow
//      terminator must DROP. That is the diffusion doing something at all.
//   2. WARM BLEED. The dark side just past the terminator must get REDDER.
//      That is the per-channel profile: red's mean free path is three times
//      blue's, so red survives a trip under the surface that blue does not.
//   3. SHARP SPECULAR. The highlight's peak luminance and its own steepest
//      gradient must NOT move. That is the claim in the issue's title, and it
//      is the one that fails if anything ever blurs the combined term.
//
// A third capture with the profile authored at TRANSPORT VERSION 0 pins ADR
// 0024's rule from the other side: with the renderer's diffusion switched fully
// on, a version-0 profile must render the #1231 frame, byte for byte in every
// measure this test takes.
//
// Evidence PNGs (written before any assertion):
//   OloEditor/assets/tests/visual/SkinDiffusion_{Forward,ForwardPlus,Deferred}.png
//   OloEditor/assets/tests/visual/SkinDiffusionOff_{Forward,ForwardPlus,Deferred}.png
//   OloEditor/assets/tests/visual/SkinDiffusion_Deferred_Oblique.png
//   OloEditor/assets/tests/visual/SkinDiffusionOff_Deferred_Oblique.png
//   OloEditor/assets/tests/visual/SkinDiffusion_VersionZero_Deferred.png
//
// Classification: L8 (full Scene pipeline on all three raster paths, RGBA8
// readback + PNG; SKIPs cleanly without a GL 4.6 context).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 384;

        // AN EXAGGERATED PROFILE, DELIBERATELY. A contract test measures whether
        // the mechanism is connected, not whether it is beautiful: at the
        // reference head's authored 1.55 mm the blur on a 1-metre sphere is a
        // fraction of a pixel, and a tolerance wide enough to see it would also
        // pass with the feature switched off. These radii put the support at a
        // few dozen millimetres, which on a unit sphere at this framing is a
        // measurable number of pixels.
        //
        // The RATIO between the channels is the authored one — red about three
        // times blue — because claim 2 is about that ratio and nothing else.
        constexpr glm::vec3 kScatterRadiusMM{ 31.0f, 16.0f, 11.0f };
        constexpr glm::vec3 kScatterColor{ 0.85f, 0.55f, 0.45f };

        [[nodiscard]] f32 Channel(const std::vector<u8>& px, std::size_t idx, u32 channel)
        {
            return static_cast<f32>(px[idx + channel]) / 255.0f;
        }

        [[nodiscard]] f32 LumaAt(const std::vector<u8>& px, std::size_t idx)
        {
            return 0.2126f * Channel(px, idx, 0) + 0.7152f * Channel(px, idx, 1) + 0.0722f * Channel(px, idx, 2);
        }

        // Red's share of the pixel's total energy. Scale-free, so it does not
        // move when one capture is a little brighter overall — only when the
        // colour balance changes, which is exactly claim 2.
        [[nodiscard]] f32 RedFractionAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = Channel(px, idx, 0);
            const f32 g = Channel(px, idx, 1);
            const f32 b = Channel(px, idx, 2);
            return r / std::max(r + g + b, 1.0e-4f);
        }

        struct Capture
        {
            std::vector<u8> Pixels;
            u32 Width = 0;
            u32 Height = 0;

            [[nodiscard]] std::size_t Index(u32 x, u32 y) const
            {
                return (static_cast<std::size_t>(y) * Width + x) * 4u;
            }
        };

        // The steepest HORIZONTAL luminance step anywhere inside a normalised
        // box, and the mean luminance over it.
        //
        // A GRADIENT, NOT A BLUR RADIUS. "Is it blurred?" has no direct
        // measurement in a frame, but "how fast does the image change here?" has
        // one, and a blur can only ever lower it. That makes claim 1 a
        // one-directional inequality and claim 3 a two-sided bound, both of
        // which survive a change of exposure, tonemapper or path.
        struct BoxStats
        {
            f32 MaxGradient = 0.0f;
            f32 MeanLuma = 0.0f;
            f32 MeanRedFraction = 0.0f;
            u64 Samples = 0;
        };

        [[nodiscard]] BoxStats MeasureBox(const Capture& capture, f32 cx, f32 cy, f32 halfW, f32 halfH)
        {
            const auto span = [](f32 centre, f32 extent, u32 size) {
                const auto lo = static_cast<i32>((centre - extent) * static_cast<f32>(size));
                const auto hi = static_cast<i32>((centre + extent) * static_cast<f32>(size));
                return std::pair<u32, u32>{ static_cast<u32>(std::clamp(lo, 1, static_cast<i32>(size) - 2)),
                                            static_cast<u32>(std::clamp(hi, 1, static_cast<i32>(size) - 2)) };
            };
            const auto [x0, x1] = span(cx, halfW, capture.Width);
            const auto [y0, y1] = span(cy, halfH, capture.Height);

            BoxStats stats{};
            f64 lumaSum = 0.0;
            f64 redSum = 0.0;
            for (u32 y = y0; y <= y1; ++y)
            {
                for (u32 x = x0; x <= x1; ++x)
                {
                    const std::size_t idx = capture.Index(x, y);
                    const f32 luma = LumaAt(capture.Pixels, idx);
                    lumaSum += static_cast<f64>(luma);
                    redSum += static_cast<f64>(RedFractionAt(capture.Pixels, idx));
                    ++stats.Samples;

                    const f32 left = LumaAt(capture.Pixels, capture.Index(x - 1u, y));
                    const f32 right = LumaAt(capture.Pixels, capture.Index(x + 1u, y));
                    stats.MaxGradient = std::max(stats.MaxGradient, std::abs(right - left));
                }
            }
            if (stats.Samples > 0)
            {
                stats.MeanLuma = static_cast<f32>(lumaSum / static_cast<f64>(stats.Samples));
                stats.MeanRedFraction = static_cast<f32>(redSum / static_cast<f64>(stats.Samples));
            }
            return stats;
        }

        [[nodiscard]] f32 PeakLuma(const Capture& capture)
        {
            f32 peak = 0.0f;
            for (u32 y = 4; y + 4 < capture.Height; ++y)
                for (u32 x = 4; x + 4 < capture.Width; ++x)
                    peak = std::max(peak, LumaAt(capture.Pixels, capture.Index(x, y)));
            return peak;
        }

        [[nodiscard]] fs::path VisualOutputPath(const std::string& name)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / (name + ".png");
        }

        // THE THREE MEASUREMENT BOXES, DERIVED FROM THE SCENE RATHER THAN FITTED
        // TO A CAPTURE. The camera is at z = 4.0 looking down -Z at a unit sphere
        // at the origin, so the sphere fills roughly the middle half of the
        // frame. The key light comes from the LEFT, so:
        //
        //   * the highlight sits left of centre,
        //   * the terminator runs roughly vertically right of centre,
        //   * the shadowed limb is further right again.
        //
        // The boxes are tall and narrow so each one samples a band of the
        // terminator rather than a point, and none of them reaches the
        // silhouette, where the bilateral guard legitimately stops the blur.
        constexpr f32 kHighlightCx = 0.375f;
        constexpr f32 kTerminatorCx = 0.570f;
        constexpr f32 kShadowedCx = 0.610f;
        constexpr f32 kBoxCy = 0.500f;
        constexpr f32 kBoxHalfW = 0.030f;
        constexpr f32 kBoxHalfH = 0.110f;
    } // namespace

    class SkinDiffusionScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            SetUpScratchProject();

            m_DiffusingProfile = MakeProfile("DiffusionProbe", SkinEvaluationModel::ScreenSpaceDiffusion);
            m_LegacyProfile = MakeProfile("LegacyProbe", SkinEvaluationModel::DiffuseSpecularSplit);

            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 4.0f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            // A hard SIDE key. The terminator is the feature under test, and a
            // light near the camera would not produce one.
            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = { 0.94f, -0.12f, -0.32f };
            dirLight.m_Color = { 1.0f, 1.0f, 1.0f };
            dirLight.m_Intensity = 2.0f;
            dirLight.m_CastShadows = false;

            const Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 48);
            Entity entity = GetScene().CreateEntity("SkinSphere");
            entity.AddComponent<MeshComponent>(sphere->GetMeshSource());
            auto& materialComp = entity.AddComponent<MaterialComponent>();
            // A pale, moderately rough dielectric. Pale so the diffuse half
            // carries enough energy for the bleed to be visible; rough enough
            // that the highlight spreads over a measurable number of pixels
            // instead of clipping to a white dot in every capture.
            materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.62f, 0.48f, 0.42f, 1.0f));
            materialComp.m_Material.SetMetallicFactor(0.0f);
            materialComp.m_Material.SetRoughnessFactor(0.34f);
            materialComp.m_Material.SetMaterialKind(MaterialKind::Skin);
            materialComp.m_Material.SetSkinProfileHandle(m_DiffusingProfile);
            m_Sphere = entity;

            EnableRendering(kSize, kSize);
        }

        void TearDown() override
        {
            // BEFORE anything that can fail. An ASSERT_* returns from the test
            // body, so a reset written there never runs and leaves a
            // renderer-wide switch armed for every later fixture in the process
            // (cross-test-renderer-state.md).
            Renderer3D::GetSkinDiffusionSettings() = SkinDiffusionSettings{};
            Renderer3D::GetPostProcessSettings().MaterialDebug = MaterialDebugView::None;
            Renderer3D::GetSkinProfileTable().Reset();

            m_AssetManager.Reset();
            Project::Unload();
            std::error_code ec;
            if (!m_ProjectDir.empty())
                fs::remove_all(m_ProjectDir, ec);
            RendererAttachedTest::TearDown();
        }

        [[nodiscard]] AssetHandle MakeProfile(const char* name, SkinEvaluationModel model)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName(name);
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = model;
            parameters.ScatterColor = kScatterColor;
            parameters.ScatterRadiusMM = kScatterRadiusMM;
            EXPECT_TRUE(profile->SetParameters(parameters))
                << name << ": the probe profile needed correcting — a bound moved";
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            EXPECT_NE(static_cast<u64>(handle), 0ULL) << name << ": AddMemoryOnlyAsset returned a zero handle";
            return handle;
        }

        void SetUpScratchProject()
        {
            std::error_code ec;
            m_ProjectDir = OloEngine::Tests::TempDir("skin-diffusion-project");
            fs::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project dir at " << m_ProjectDir.string();

            const fs::path projectFile = m_ProjectDir / "SkinDiffusion.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinDiffusion\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << projectFile.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        void SetCameraPose(const glm::vec3& translation, const glm::vec3& rotationRadians)
        {
            Entity camera = GetScene().FindEntityByName("Camera");
            ASSERT_TRUE(static_cast<bool>(camera)) << "the probe scene lost its camera";
            auto& transform = camera.GetComponent<TransformComponent>();
            transform.Translation = translation;
            transform.SetRotationEuler(rotationRadians);
        }

        // Render one capture and write its evidence PNG. `diffusionEnabled` is
        // the ONLY thing that differs between the A and B captures of a pair.
        [[nodiscard]] bool CaptureFrame(RenderingPath path, bool diffusionEnabled, const std::string& name,
                                        Capture& out)
        {
            Renderer3D::GetSkinDiffusionSettings().Enabled = diffusionEnabled;
            Renderer3D::GetSkinDiffusionSettings().Quality = SkinDiffusionQuality::High;
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            // Two frames: the first one settles the graph rebuild that the
            // path / enable change forces, the second is the one measured.
            RunFrames(2);

            if (!ReadbackComposite(out.Pixels, out.Width, out.Height))
                return false;
            if (out.Pixels.size() != static_cast<std::size_t>(out.Width) * out.Height * 4u)
                return false;

            const fs::path file = VisualOutputPath(name);
            const int wrote = ::stbi_write_png(file.string().c_str(), static_cast<int>(out.Width),
                                               static_cast<int>(out.Height), 4, out.Pixels.data(),
                                               static_cast<int>(out.Width) * 4);
            EXPECT_NE(wrote, 0) << "failed to write " << file.string();
            return true;
        }

        void ExpectDiffusionOnPath(RenderingPath path, const char* pathName)
        {
            const std::string offName = std::string("SkinDiffusionOff_") + pathName;
            const std::string onName = std::string("SkinDiffusion_") + pathName;

            Capture off;
            Capture on;
            ASSERT_TRUE(CaptureFrame(path, /*diffusionEnabled=*/false, offName, off)) << pathName << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, /*diffusionEnabled=*/true, onName, on)) << pathName << ": readback failed";
            ASSERT_EQ(off.Width, on.Width);
            ASSERT_EQ(off.Height, on.Height);

            const BoxStats offTerminator = MeasureBox(off, kTerminatorCx, kBoxCy, kBoxHalfW, kBoxHalfH);
            const BoxStats onTerminator = MeasureBox(on, kTerminatorCx, kBoxCy, kBoxHalfW, kBoxHalfH);
            const BoxStats offHighlight = MeasureBox(off, kHighlightCx, kBoxCy, kBoxHalfW, kBoxHalfH);
            const BoxStats onHighlight = MeasureBox(on, kHighlightCx, kBoxCy, kBoxHalfW, kBoxHalfH);
            const BoxStats offShadowed = MeasureBox(off, kShadowedCx, kBoxCy, kBoxHalfW, kBoxHalfH);
            const BoxStats onShadowed = MeasureBox(on, kShadowedCx, kBoxCy, kBoxHalfW, kBoxHalfH);

            // NON-VACUITY FIRST. Every assertion below is a comparison between
            // two captures, and two EMPTY captures compare equal — so the frame
            // has to be shown to contain a lit sphere before any of them means
            // anything.
            ASSERT_GT(offHighlight.MeanLuma, 0.05f)
                << pathName << ": nothing lit in the highlight box — the scene did not render; see "
                << VisualOutputPath(offName).string();
            ASSERT_GT(offTerminator.MaxGradient, 0.02f)
                << pathName << ": no terminator in the frame — the key light is not where this test thinks; see "
                << VisualOutputPath(offName).string();

            // CLAIM 1 — the terminator softens.
            EXPECT_LT(onTerminator.MaxGradient, offTerminator.MaxGradient * 0.92f)
                << pathName << ": the terminator did not soften (" << offTerminator.MaxGradient << " -> "
                << onTerminator.MaxGradient << "); see " << VisualOutputPath(onName).string();

            // CLAIM 2 — red bleeds into the shadowed side. Red's mean free path
            // is nearly three times blue's, so the light that survives the trip
            // under the surface is warm. A grey blur would move the luminance
            // and leave this ratio alone.
            EXPECT_GT(onShadowed.MeanRedFraction, offShadowed.MeanRedFraction + 0.002f)
                << pathName << ": no warm bleed past the terminator (" << offShadowed.MeanRedFraction << " -> "
                << onShadowed.MeanRedFraction << "); see " << VisualOutputPath(onName).string();

            // CLAIM 3 — THE TITLE. The specular highlight is untouched: its peak
            // does not dim and its own steepest gradient does not soften. This
            // is the assertion that fails if anything ever blurs the combined
            // term, and it is the one a screenshot cannot make for you.
            //
            // A 5% band rather than an equality: the diffuse half UNDER the
            // highlight is legitimately blurred, and it contributes to the same
            // pixels. What must not happen is the highlight itself smearing,
            // which would be a change of tens of percent.
            EXPECT_GT(onHighlight.MaxGradient, offHighlight.MaxGradient * 0.95f)
                << pathName << ": the specular highlight SOFTENED — the composite is being blurred ("
                << offHighlight.MaxGradient << " -> " << onHighlight.MaxGradient << "); see "
                << VisualOutputPath(onName).string();
            EXPECT_NEAR(PeakLuma(on), PeakLuma(off), 0.05f)
                << pathName << ": the frame's peak moved — the highlight is being spread; see "
                << VisualOutputPath(onName).string();
        }

        AssetHandle m_DiffusingProfile{};
        AssetHandle m_LegacyProfile{};
        Entity m_Sphere{};
        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_ProjectDir;
    };

    TEST_F(SkinDiffusionScene, DiffusesTheDiffuseHalfAndLeavesTheHighlightSharpOnForward)
    {
        ExpectDiffusionOnPath(RenderingPath::Forward, "Forward");
    }

    TEST_F(SkinDiffusionScene, DiffusesTheDiffuseHalfAndLeavesTheHighlightSharpOnForwardPlus)
    {
        ExpectDiffusionOnPath(RenderingPath::ForwardPlus, "ForwardPlus");
    }

    TEST_F(SkinDiffusionScene, DiffusesTheDiffuseHalfAndLeavesTheHighlightSharpOnDeferred)
    {
        ExpectDiffusionOnPath(RenderingPath::Deferred, "Deferred");
    }

    TEST_F(SkinDiffusionScene, DiffusesAtAnObliqueAngleToo)
    {
        // A GRAZING VIEW, because that is where a screen-space filter is most
        // likely to be wrong: the surface runs nearly edge-on to the camera, so
        // the same screen-space radius covers far more of the SURFACE than it
        // does head-on, and the depth guard is doing most of the work. The
        // measurement is the one claim that survives an unknown framing — the
        // terminator softens and the peak does not move.
        SetCameraPose({ 3.2f, 0.4f, 2.2f }, { -0.10f, 0.95f, 0.0f });

        Capture off;
        Capture on;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, false, "SkinDiffusionOff_Deferred_Oblique", off));
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, true, "SkinDiffusion_Deferred_Oblique", on));

        const f32 offPeak = PeakLuma(off);
        ASSERT_GT(offPeak, 0.05f) << "the oblique view shows no lit sphere; see "
                                  << VisualOutputPath("SkinDiffusionOff_Deferred_Oblique").string();
        EXPECT_NEAR(PeakLuma(on), offPeak, 0.06f)
            << "the oblique view's peak moved — the highlight is being spread; see "
            << VisualOutputPath("SkinDiffusion_Deferred_Oblique").string();

        // The whole-frame steepest gradient must not RISE: a screen-space filter
        // that ran off its guards at a grazing angle introduces edges rather
        // than removing them, which is the artefact this capture exists to
        // catch.
        const BoxStats offAll = MeasureBox(off, 0.5f, 0.5f, 0.45f, 0.45f);
        const BoxStats onAll = MeasureBox(on, 0.5f, 0.5f, 0.45f, 0.45f);
        EXPECT_LE(onAll.MaxGradient, offAll.MaxGradient * 1.02f)
            << "the oblique view got a HARDER edge than it started with (" << offAll.MaxGradient << " -> "
            << onAll.MaxGradient << "); see " << VisualOutputPath("SkinDiffusion_Deferred_Oblique").string();
    }

    TEST_F(SkinDiffusionScene, AVersionZeroProfileIsNotDiffusedWithTheRendererSwitchOn)
    {
        // ADR 0024's rule, measured: the renderer's diffusion is fully on, and a
        // profile authored against transport version 0 still renders the #1231
        // frame. If a renderer setting could restate an authored head, every
        // scene in the project would change the day this feature shipped.
        Capture withDiffusingProfile;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, true, "SkinDiffusion_Deferred_Control", withDiffusingProfile));

        auto& material = m_Sphere.GetComponent<MaterialComponent>().m_Material;
        material.SetSkinProfileHandle(m_LegacyProfile);

        Capture withLegacyProfile;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, true, "SkinDiffusion_VersionZero_Deferred",
                                 withLegacyProfile));

        Capture undiffused;
        material.SetSkinProfileHandle(m_DiffusingProfile);
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, false, "SkinDiffusionOff_Deferred_Control", undiffused));

        const BoxStats legacyTerminator = MeasureBox(withLegacyProfile, kTerminatorCx, kBoxCy, kBoxHalfW, kBoxHalfH);
        const BoxStats offTerminator = MeasureBox(undiffused, kTerminatorCx, kBoxCy, kBoxHalfW, kBoxHalfH);
        const BoxStats diffusedTerminator =
            MeasureBox(withDiffusingProfile, kTerminatorCx, kBoxCy, kBoxHalfW, kBoxHalfH);

        ASSERT_GT(offTerminator.MaxGradient, 0.02f) << "no terminator in the control frame";
        // Non-vacuity: the diffusing profile DID do something in this same
        // fixture, so "the legacy profile did nothing" is a statement about the
        // profile and not about the pass being dead.
        ASSERT_LT(diffusedTerminator.MaxGradient, offTerminator.MaxGradient * 0.92f)
            << "the diffusing profile did not diffuse — this test cannot say anything about the legacy one";

        EXPECT_NEAR(legacyTerminator.MaxGradient, offTerminator.MaxGradient, offTerminator.MaxGradient * 0.03f)
            << "a version-0 profile was diffused by the renderer switch; see "
            << VisualOutputPath("SkinDiffusion_VersionZero_Deferred").string();
    }
} // namespace OloEngine::Tests
