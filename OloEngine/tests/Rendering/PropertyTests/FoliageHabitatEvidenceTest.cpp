#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// FoliageHabitatEvidenceTest — issue #1254, acceptance criteria 2 and 4.
//
// The meadow-to-woodland example, rendered. Writes
//   OloEditor/assets/tests/visual/FoliageHabitat_GL_<Path>[_<Angle>].png
// and its A/B control
//   OloEditor/assets/tests/visual/FoliageHabitatOff_GL_<Path>[_<Angle>].png
//
// The filename carries the {backend} x {path} cell it covers, deliberately
// including the backend even though it is always GL here: every Vulkan cell is
// LIVE-ONLY, because these fixtures need a real GL 4.6 context and skip without
// one, and a reader counting files must not mistake a complete set of OpenGL
// captures for a complete verification matrix. The Vulkan rows are evidenced in
// the PR body from a real editor session instead.
//
// The CONTROL is the same scene with the #1254 rules switched off — the
// pre-#1254 uniform scatter — captured from the same camera with the same
// scene graph, so a pixel difference is the habitat rules and not a different
// frame. (backend-ab-needs-an-identical-camera: the two arms differ in one
// field each and nothing else.)
//
// The contracts, which are the things that can be wrong while the picture still
// looks like grass:
//
//   1. The rules REACH THE FRAME, on all three rendering paths. A placement
//      change that only lands on one path is invisible in a single-path
//      capture, and the foliage pass has three of them (#1265 put it in the
//      G-Buffer).
//   2. Habitat gating THINS the scatter rather than replacing it: the rule-on
//      arm places strictly fewer instances than the control.
//   3. The variation is visible at BOTH scales (criterion 4) — the near and
//      landscape cameras each show a large pixel difference against their own
//      control, so "it only reads from one distance" fails here.
//   4. MSAA and a non-native resolution are cells of their own, because foliage
//      is alpha-cutout geometry whose coverage interacts with both.
//
// The COMMITTED PNGs come from an ISOLATED run of this fixture. Running it
// after other renderer tests leaves global renderer state behind
// (cross-test-renderer-state.md) and the captures come out shaded differently
// — same scene, same habitat structure, different exposure and density. That
// does not weaken the assertions: every measurement below is an A/B between
// two frames captured back to back under whatever state is live, so both arms
// share it and the difference is the habitat rules alone. Only the pictures
// are order-sensitive, and they are evidence for a human to look at, never
// compared against a baseline.
//
// Classification: L8 / visual evidence (full GL pipeline + RGBA8 readback +
// PNG). Skips cleanly without a GL 4.6 context; never DISABLED_.
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Well above tone-map dithering, well below the contrast between a
        // grass blade and the ground it stands on.
        constexpr int kPixelThreshold = 12;

        [[nodiscard]] u32 CountDifferingPixels(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            if (frame.size() != baseline.size() || frame.empty())
                return 0;
            u32 differing = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                const int dr = std::abs(static_cast<int>(frame[i]) - static_cast<int>(baseline[i]));
                const int dg = std::abs(static_cast<int>(frame[i + 1]) - static_cast<int>(baseline[i + 1]));
                const int db = std::abs(static_cast<int>(frame[i + 2]) - static_cast<int>(baseline[i + 2]));
                if (dr > kPixelThreshold || dg > kPixelThreshold || db > kPixelThreshold)
                    ++differing;
            }
            return differing;
        }

        [[nodiscard]] u32 MaxChannelDelta(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            if (frame.size() != baseline.size())
                return 0;
            int worst = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                    worst = std::max(worst,
                                     std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(baseline[i + c])));
            return static_cast<u32>(worst);
        }

        [[nodiscard]] f64 MeanLuminance(const std::vector<u8>& frame)
        {
            if (frame.empty())
                return 0.0;
            f64 sum = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                sum += (0.2126 * frame[i] + 0.7152 * frame[i + 1] + 0.0722 * frame[i + 2]) / 255.0;
                ++count;
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }

        /// Strip the #1254 rules off a layer set, leaving the pre-#1254 scatter.
        /// This is the A/B control arm: same species, same densities, same
        /// textures, uniform placement.
        [[nodiscard]] std::vector<FoliageLayer> WithoutHabitatRules(std::vector<FoliageLayer> layers)
        {
            for (auto& layer : layers)
            {
                layer.SlopeFeather = 0.0f;
                layer.UseAltitudeBand = false;
                layer.UseMoisture = false;
                layer.ExclusionSplatmapChannel = -1;
                layer.ClumpStrength = 0.0f;
                layer.ClumpScaleInfluence = 0.0f;
                layer.GroundOffset = 0.0f;
                layer.SlopeSinkFactor = 0.0f;
                layer.DecorrelatedVariation = false;
            }
            return layers;
        }
    } // namespace

    class FoliageHabitatEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            auto& rendererSettings = Renderer3D::GetRendererSettings();
            rendererSettings.EditorDebugDrawsEnabled = true;
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.45f, -0.72f, -0.35f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 4.5f;
            }

            // A landscape with real relief, so the habitat rules have something
            // to separate ON: the moisture proxy reads low flat ground as wet
            // (meadow) and high steep ground as dry (woodland fringe), and the
            // altitude the plants sit at is what the bands cut.
            m_TerrainEntity = scene.CreateEntity("Terrain");
            {
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                // The terrain parameters are the ones FoliageGenerationTest.olo
                // and FoliageGenerationEvidenceTest already use, deliberately.
                // A first pass here picked a different seed and a taller height
                // scale, which put the whole camera arc inside the auto-
                // material's SNOW band (rule 3 starts at 0.62 normalized): the
                // foliage A/B was still valid, because both arms shared it, but
                // the captures read as grass on a snowfield rather than the
                // meadow-to-woodland the issue asks to demonstrate.
                terrain.m_ProceduralSeed = 7;
                terrain.m_ProceduralResolution = 192;
                terrain.m_ProceduralOctaves = 5;
                terrain.m_ProceduralFrequency = 2.0f;
                terrain.m_HeightShaping.HeightExponent = 1.3f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                terrain.m_HeightScale = 28.0f;
                terrain.m_TessellationEnabled = false;

                terrain.m_AutoMaterial = true;
                terrain.m_SplatmapGenResolution = 256;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                    terrain.m_Material->AddLayer(layer);
                terrain.m_LayerRules = TerrainGenerator::MakeDefaultRules();
                terrain.m_MaterialNeedsRebuild = true;
                terrain.m_AutoSplatNeedsRebuild = true;

                // The five-species meadow-to-woodland mix the generator emits
                // since #1254 — this IS the reproducible example of criterion 4,
                // not a hand-authored one-off, so it cannot drift from what the
                // "Generate from Terrain Rules" button gives an author.
                m_HabitatLayers = TerrainGenerator::MakeFoliageLayersFromRules(terrain.m_LayerRules);
                m_ControlLayers = WithoutHabitatRules(m_HabitatLayers);

                auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;
                foliage.m_Layers = m_HabitatLayers;
                foliage.m_NeedsRebuild = true;
            }
        }

        void UseLayers(const std::vector<FoliageLayer>& layers)
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            foliage.m_Layers = layers;
            foliage.m_NeedsRebuild = true;
        }

        [[nodiscard]] u32 InstanceCount() const
        {
            const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            return foliage.m_Renderer ? foliage.m_Renderer->GetTotalInstanceCount() : 0u;
        }

        void Capture(const std::string& saveAs, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels, u32 width = kWidth, u32 height = kHeight)
        {
            EditorCamera camera(60.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.5f, 3000.0f);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
            camera.SetPose(position, yaw, pitch);

            // Several ticks: build the height field, the auto-splat and the
            // foliage instances, then let the frame graph settle.
            RunEditorFrames(camera, 4);

            // SceneColor, not UIComposite. The foliage pass composites into
            // SceneColor before post and the UI overlay, and this is the target
            // FoliageGenerationEvidenceTest reads for the same scene — its
            // committed capture is the reference for what this terrain's
            // auto-material is supposed to look like. Reading UIComposite here
            // washed the terrain out to near-white, which made the captures
            // useless as the issue's "meadow-to-woodland" evidence even though
            // the foliage A/B measured identically either way.
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer for '" << saveAs << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), width, height, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(width) * height * 4u);

            // GL readback is bottom-up.
            {
                const sizet rowBytes = static_cast<sizet>(width) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < height / 2u; ++y)
                {
                    u8* top = outPixels.data() + (static_cast<sizet>(y) * rowBytes);
                    u8* bot = outPixels.data() + (static_cast<sizet>(height - 1u - y) * rowBytes);
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            if (!saveAs.empty())
                WriteEvidence(saveAs, outPixels, width, height);
        }

        // EVIDENCE, not an SSIM golden: the subject is a procedural
        // distribution, and a committed per-pixel baseline of one would be a
        // flake generator the first time a driver rounds a blade differently.
        // The contracts are the assertions; the PNGs exist so a reviewer can
        // look at what they describe. So this always writes, and never compares.
        static void WriteEvidence(const std::string& name, const std::vector<u8>& pixels, u32 width, u32 height)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                               pixels.data(), static_cast<int>(width) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }

        Entity m_TerrainEntity;
        std::vector<FoliageLayer> m_HabitatLayers;
        std::vector<FoliageLayer> m_ControlLayers;
    };

    // ── Criterion 2 + 4: the rules reach the frame, on every path ───────────

    TEST_F(FoliageHabitatEvidenceTest, HabitatRulesChangeTheFrameOnEveryRenderingPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

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

        // A landscape vantage: high enough that the patch structure is the
        // subject rather than individual blades.
        const glm::vec3 eye{ 128.0f, 62.0f, 214.0f };

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();

            // Control FIRST, so the baseline cannot be contaminated.
            UseLayers(m_ControlLayers);
            std::vector<u8> without;
            Capture(std::string("FoliageHabitatOff_GL_") + pathCase.Name + "_Landscape", eye, 0.0f, 0.28f, without);
            if (::testing::Test::HasFatalFailure())
                return;
            const u32 controlInstances = InstanceCount();

            UseLayers(m_HabitatLayers);
            std::vector<u8> with;
            Capture(std::string("FoliageHabitat_GL_") + pathCase.Name + "_Landscape", eye, 0.0f, 0.28f, with);
            if (::testing::Test::HasFatalFailure())
                return;
            const u32 habitatInstances = InstanceCount();

            EXPECT_GT(MeanLuminance(with), 0.02)
                << pathCase.Name << ": the frame is (near-)black, so nothing below is evidence of anything";
            EXPECT_GT(habitatInstances, 0u) << pathCase.Name << ": the habitat rules scattered nothing at all";

            const u32 differing = CountDifferingPixels(with, without);
            const u32 maxDelta = MaxChannelDelta(with, without);
            std::printf("[foliage-habitat] path %-11s  %u px differ, max delta %u/255, %u -> %u instances\n",
                        pathCase.Name, differing, maxDelta, controlInstances, habitatInstances);

            EXPECT_GT(differing, 3000u)
                << pathCase.Name << ": switching the habitat rules on changed almost nothing, so they did not "
                                    "reach this path's foliage draw";
            EXPECT_GT(maxDelta, 30u) << pathCase.Name << ": the difference is too faint to be geometry";

            // Criterion 1's gating half: the rules THIN the scatter. If the
            // counts matched, the bands and the clump field would be decorative.
            EXPECT_LT(habitatInstances, controlInstances)
                << pathCase.Name << ": the habitat rules placed as many plants as the uniform control, so "
                                    "nothing is being gated";
        }
    }

    // ── Criterion 4: variation visible at near AND landscape scale ──────────

    TEST_F(FoliageHabitatEvidenceTest, VariationIsVisibleNearAndAtLandscapeScale)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        struct AngleCase
        {
            const char* Name;
            glm::vec3 Eye;
            f32 Yaw;
            f32 Pitch;
        };
        // Three vantages, deliberately at different SCALES rather than three
        // rotations of the same one: criterion 4 asks for variation that reads
        // both up close and from a distance, and a set of near-identical
        // framings cannot tell those apart.
        const std::array<AngleCase, 3> angles = { {
            { "Near", glm::vec3(128.0f, 19.0f, 158.0f), 0.0f, 0.04f },
            { "Oblique", glm::vec3(96.0f, 30.0f, 178.0f), -0.5f, 0.22f },
            { "Landscape", glm::vec3(128.0f, 86.0f, 250.0f), 0.0f, 0.36f },
        } };

        for (const AngleCase& angle : angles)
        {
            UseLayers(m_ControlLayers);
            std::vector<u8> without;
            Capture(std::string("FoliageHabitatOff_GL_Forward_") + angle.Name, angle.Eye, angle.Yaw, angle.Pitch,
                    without);
            if (::testing::Test::HasFatalFailure())
                return;

            UseLayers(m_HabitatLayers);
            std::vector<u8> with;
            Capture(std::string("FoliageHabitat_GL_Forward_") + angle.Name, angle.Eye, angle.Yaw, angle.Pitch, with);
            if (::testing::Test::HasFatalFailure())
                return;

            const u32 differing = CountDifferingPixels(with, without);
            std::printf("[foliage-habitat] angle %-10s  %u px differ, mean luma %.4f\n", angle.Name, differing,
                        MeanLuminance(with));

            EXPECT_GT(MeanLuminance(with), 0.02) << angle.Name << ": empty frame";
            EXPECT_GT(differing, 3000u)
                << angle.Name << ": the habitat structure is invisible at this scale, so criterion 4's "
                                 "'visible at near and landscape scales' is not met here";
        }
    }

    // ── Conditional axes: MSAA, and a non-native resolution ─────────────────

    TEST_F(FoliageHabitatEvidenceTest, HabitatStructureSurvivesMsaaAndANonNativeResolution)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 eye{ 128.0f, 30.0f, 182.0f };

        // MSAA is a cell of its own here, not a shrug: foliage is alpha-cutout
        // geometry, coverage and MSAA interact directly, and a clumping change
        // alters silhouette density — which is exactly what a coverage resolve
        // is sensitive to.
        //
        // In this engine MSAA is the DEFERRED G-Buffer's sample count
        // (DeferredSettings::MSAASampleCount), so the cell runs on the deferred
        // path; there is no forward MSAA knob to toggle.
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        const u32 samplesBefore = settings.Deferred.MSAASampleCount;

        settings.Deferred.MSAASampleCount = 1;
        Renderer3D::ApplyRendererSettings();
        UseLayers(m_HabitatLayers);
        std::vector<u8> noMsaa;
        Capture("FoliageHabitat_GL_Deferred_MsaaOff", eye, 0.0f, 0.18f, noMsaa);
        if (::testing::Test::HasFatalFailure())
            return;

        settings.Deferred.MSAASampleCount = std::min(4u, std::max(1u, Renderer3D::GetMaxMSAASamples()));
        Renderer3D::ApplyRendererSettings();
        std::vector<u8> msaa;
        Capture("FoliageHabitat_GL_Deferred_MsaaOn", eye, 0.0f, 0.18f, msaa);
        const u32 samplesUsed = settings.Deferred.MSAASampleCount;
        settings.Deferred.MSAASampleCount = samplesBefore;
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        if (::testing::Test::HasFatalFailure())
            return;
        std::printf("[foliage-habitat] MSAA cell ran at %u samples\n", samplesUsed);

        EXPECT_GT(MeanLuminance(msaa), 0.02) << "the MSAA frame is empty — the cell proves nothing";
        // The two must be RECOGNISABLY the same scene: MSAA edits edges, so the
        // mean brightness moves by a little and not by a lot. A collapse here
        // is the foliage dropping out under the resolve, which is the failure
        // this cell exists to catch.
        const f64 lumaDelta = std::abs(MeanLuminance(msaa) - MeanLuminance(noMsaa));
        std::printf("[foliage-habitat] MSAA mean-luma delta %.5f\n", lumaDelta);
        EXPECT_LT(lumaDelta, 0.05) << "the foliage did not survive the MSAA resolve intact";

        // A non-native resolution: ground cover is thin geometry and the
        // mesh/card/impostor hand-over is measured in world units against a
        // pixel budget, so a different target size is a different sampling of
        // the same distribution.
        constexpr u32 kOddWidth = 907;
        constexpr u32 kOddHeight = 611;
        ResizeRenderTarget(kOddWidth, kOddHeight);
        std::vector<u8> odd;
        Capture("FoliageHabitat_GL_Forward_NonNativeRes", eye, 0.0f, 0.18f, odd, kOddWidth, kOddHeight);
        ResizeRenderTarget(kWidth, kHeight);
        if (::testing::Test::HasFatalFailure())
            return;

        EXPECT_GT(MeanLuminance(odd), 0.02)
            << "the foliage vanished at a non-native resolution — thin geometry lost to the sampling";
        EXPECT_GT(InstanceCount(), 0u) << "the scatter itself collapsed at a non-native resolution";
    }
} // namespace OloEngine::Tests
