// =============================================================================
// WaterProjectedGridVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the projected water grid (issue #1035,
// water-ocean.md §4.1), from the two camera angles the issue's acceptance
// criteria name: a LOW GRAZING angle and a HIGH OVERHEAD one.
//
// It captures four frames — both angles, both grids — into
//   OloEditor/assets/tests/visual/WaterProjGrid_<Grid>_<Angle>.png
// so the A/B is IN the evidence rather than spread across two runs. That
// matters here more than usual: the whole claim is "equal or better visual
// fidelity at equal or lower vertex count", and half of it is a statement about
// two pictures, not about one.
//
// The grazing angle is the interesting one and it is where the world-space grid
// is worst: WaterGeometryLodProfileTest measures 80-94% of its surviving
// triangles landing sub-pixel there. Sub-pixel geometry does not look better,
// it looks NOISIER — it is the surface sampled below the raster's Nyquist rate,
// which is the same failure docs/agent-rules/water-shading-nyquist.md describes
// for normals. So the projected arm is expected to read as calmer toward the
// horizon and no worse near the camera, and the assertions check exactly that
// pair rather than a single "looks right".
//
// The committed PNGs are RMSE goldens like every other file in that directory:
// a normal run COMPARES and writes nothing, `--olo-golden-rebase` rewrites them
// after a deliberate visual change. Run from OloEditor/ so assets resolve.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <stb_image.h>
#include <stb_image_write.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;

        // Frozen wall clock, so the wave phase is identical on every run and the
        // committed PNGs can be golden references at all.
        constexpr f32 kCaptureTime = 8.0f;

        // Same threshold the sibling water evidence tests use.
        constexpr f64 kGoldenRmseThreshold = 8.0;

        // The world-space grid this scene would ship with, and the screen-space
        // grid that replaces it — WaterShowcase.olo's own numbers. 256x144 is
        // deliberately denser than the viewport needs: part of the grid is laid
        // out OUTSIDE the frame so a wave crest can lift water into the bottom
        // edge, and at a low eye height that skirt is most of the rows.
        constexpr u32 kWorldGridResolution = 512;
        constexpr u32 kProjectedGridX = 256;
        constexpr u32 kProjectedGridY = 144;

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::max();
            f64 sumSq = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                for (i32 c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        /// Mean RGB over a horizontal band of the frame, rows [y0, y1) with row 0
        /// at the TOP (the pixels handed in are already flipped upright).
        [[nodiscard]] glm::vec3 BandMean(const std::vector<u8>& pixels, u32 y0, u32 y1)
        {
            glm::dvec3 sum(0.0);
            sizet count = 0;
            for (u32 y = y0; y < y1 && y < kHeight; ++y)
            {
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4u;
                    sum += glm::dvec3(pixels[i], pixels[i + 1], pixels[i + 2]);
                    ++count;
                }
            }
            return count ? glm::vec3(sum / static_cast<f64>(count)) : glm::vec3(0.0f);
        }

        /// Mean absolute luminance difference between horizontally adjacent
        /// pixels, over rows [y0, y1). This is the noise measure the grazing-angle
        /// claim rests on: geometry sampled below the raster rate shows up as
        /// high-frequency speckle, and speckle is exactly what this counts.
        [[nodiscard]] f64 BandHorizontalDetail(const std::vector<u8>& pixels, u32 y0, u32 y1)
        {
            f64 sum = 0.0;
            sizet count = 0;
            const auto luma = [&pixels](sizet i)
            {
                return 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
            };
            for (u32 y = y0; y < y1 && y < kHeight; ++y)
            {
                for (u32 x = 1; x < kWidth; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4u;
                    sum += std::abs(luma(i) - luma(i - 4u));
                    ++count;
                }
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }
    } // namespace

    class WaterProjectedGridVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 60.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.75f, -0.5f));
                dl.m_Color = glm::vec3(1.0f, 0.96f, 0.9f);
                dl.m_Intensity = 2.0f;
            }

            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = true;
                env.m_IBLIntensity = 0.3f;
            }

            // A kilometre of open sea, which is the scale at which the two grids
            // differ: at 200 m the world grid never gets far enough away to
            // foreshorten into nothing. WaterShowcase.olo's wave parameters, so
            // the captures describe a surface the engine actually ships.
            {
                Entity ocean = scene.CreateEntity("Ocean");
                m_OceanEntity = ocean;
                auto& wc = ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 1000.0f;
                wc.m_WorldSizeZ = 1000.0f;
                wc.m_GridResolutionX = kWorldGridResolution;
                wc.m_GridResolutionZ = kWorldGridResolution;
                wc.m_WaveAmplitude = 0.5f;
                wc.m_WaveFrequency = 1.0f;
                wc.m_WaveDir0 = { 1.0f, 0.3f };
                wc.m_WaveSteepness0 = 0.3f;
                wc.m_Wavelength0 = 30.0f;
                wc.m_WaveDir1 = { -0.4f, 0.9f };
                wc.m_WaveSteepness1 = 0.25f;
                wc.m_Wavelength1 = 50.0f;
                wc.m_TessellationEnabled = true;
                wc.m_TessellationFactor = 8.0f;
                wc.m_TessMinDistance = 10.0f;
                wc.m_TessMaxDistance = 200.0f;
                wc.m_RenderFromBelow = true;
            }

            // A seafloor far enough down to be out of sight, in a colour that
            // could not arise from water — if it shows, the surface has holes,
            // which is the failure mode a tess level below 1 produces.
            {
                Entity floorEntity = scene.CreateEntity("Seafloor");
                auto& tc = floorEntity.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -40.0f, 0.0f };
                tc.Scale = { 300.0f, 1.0f, 300.0f };
                auto& mc = floorEntity.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> mesh = MeshPrimitives::CreatePlane())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = floorEntity.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
            }

            // A pillar breaking the surface, so the two arms can be compared on
            // something with a hard silhouette rather than on open water alone.
            {
                Entity pillar = scene.CreateEntity("Pillar");
                auto& tc = pillar.GetComponent<TransformComponent>();
                tc.Translation = { 6.0f, -8.0f, -18.0f };
                tc.Scale = { 2.0f, 24.0f, 2.0f };
                auto& mc = pillar.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = pillar.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.62f, 1.0f));
            }
        }

        /// Switch the ocean between the two grids. The resolution moves with the
        /// toggle because it changes meaning: quads across the WORLD before,
        /// quads across the VIEWPORT after.
        void UseProjectedGrid(bool projected)
        {
            auto& wc = m_OceanEntity.GetComponent<WaterComponent>();
            wc.m_ProjectedGridEnabled = projected;
            wc.m_GridResolutionX = projected ? kProjectedGridX : kWorldGridResolution;
            wc.m_GridResolutionZ = projected ? kProjectedGridY : kWorldGridResolution;
            wc.m_NeedsRebuild = true;
        }

        void Capture(const std::string& name, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << name << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip so row 0 is the top of the frame for
            // both the PNG and the band sampling below.
            {
                const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = outPixels.data() + static_cast<sizet>(y) * rowBytes;
                    u8* bot = outPixels.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes;
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("WaterProjGrid_" + name + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string() << "': " << ec.message();
                const i32 wrote = ::stbi_write_png(path.c_str(), static_cast<i32>(kWidth),
                                                   static_cast<i32>(kHeight), 4, outPixels.data(),
                                                   static_cast<i32>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed to write golden '" << path << "'";
                return;
            }

            i32 gw = 0;
            i32 gh = 0;
            i32 gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr)
                << "Missing golden '" << path << "' — rerun with --olo-golden-rebase to create it.";
            const bool sizeMatches = (gw == static_cast<i32>(kWidth) && gh == static_cast<i32>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<sizet>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh << ", expected "
                                     << kWidth << "x" << kHeight << " — rerun with --olo-golden-rebase.";

            const f64 rmse = Rgba8Rmse(outPixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "'" << name << "' diverged from golden (RMSE " << rmse << " > " << kGoldenRmseThreshold
                << "). If this is an intended visual change, rerun with --olo-golden-rebase to update " << path;
        }

        Entity m_OceanEntity;
    };

    // The two poses #1035 names. Yaw/pitch are radians into EditorCamera::SetPose.
    TEST_F(WaterProjectedGridVisualEvidenceTest, CaptureBothGridsFromGrazingAndOverheadAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

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
        } scopedMockTime(kCaptureTime);

        // POSITIVE pitch tilts the view DOWN — EditorCamera::SetPose says so and
        // the sign is easy to get backwards: with it negated, the "overhead"
        // capture is a frame of pure sky and still writes a perfectly valid PNG.
        //
        // Low grazing: eye 3 m above the sea, looking very slightly down, so the
        // horizon sits in frame and the far water foreshortens to nothing.
        const glm::vec3 grazingEye(0.0f, 3.0f, 120.0f);
        constexpr f32 kGrazingYaw = 0.0f;
        constexpr f32 kGrazingPitch = 0.04f;
        // High overhead: steeply down, where nothing is foreshortened.
        const glm::vec3 overheadEye(0.0f, 150.0f, 120.0f);
        constexpr f32 kOverheadYaw = 0.0f;
        constexpr f32 kOverheadPitch = 0.85f;

        std::vector<u8> worldGrazing;
        std::vector<u8> worldOverhead;
        std::vector<u8> projectedGrazing;
        std::vector<u8> projectedOverhead;

        UseProjectedGrid(false);
        Capture("WorldGrid_Grazing", grazingEye, kGrazingYaw, kGrazingPitch, worldGrazing);
        Capture("WorldGrid_Overhead", overheadEye, kOverheadYaw, kOverheadPitch, worldOverhead);

        UseProjectedGrid(true);
        Capture("Projected_Grazing", grazingEye, kGrazingYaw, kGrazingPitch, projectedGrazing);
        Capture("Projected_Overhead", overheadEye, kOverheadYaw, kOverheadPitch, projectedOverhead);

        ASSERT_FALSE(worldGrazing.empty());
        ASSERT_FALSE(projectedGrazing.empty());
        ASSERT_FALSE(worldOverhead.empty());
        ASSERT_FALSE(projectedOverhead.empty());

        // ---- it is still the same sea ---------------------------------------
        //
        // The near foreground is where both grids are dense and where the two
        // must agree: a projected grid that changed the water's colour or its
        // lighting would be a different surface, not a cheaper one.
        const u32 nearBandTop = kHeight * 3u / 4u;
        for (const auto& pair : { std::pair{ &worldGrazing, &projectedGrazing },
                                  std::pair{ &worldOverhead, &projectedOverhead } })
        {
            const glm::vec3 worldMean = BandMean(*pair.first, nearBandTop, kHeight);
            const glm::vec3 projectedMean = BandMean(*pair.second, nearBandTop, kHeight);
            EXPECT_NEAR(projectedMean.r, worldMean.r, 24.0f) << "near-field red drifted";
            EXPECT_NEAR(projectedMean.g, worldMean.g, 24.0f) << "near-field green drifted";
            EXPECT_NEAR(projectedMean.b, worldMean.b, 24.0f) << "near-field blue drifted";
            EXPECT_GT(projectedMean.b, projectedMean.r)
                << "the near foreground must still read as water (blue/teal dominant)";
        }

        // ---- and the surface is still opaque --------------------------------
        //
        // The seafloor is magenta and 40 m down. Any red-dominant near band means
        // the water has holes in it — which is precisely what a tess level below
        // 1 produces, and the regression the projected grid's constant level 1
        // has to avoid reintroducing.
        for (const auto* frame : { &projectedGrazing, &projectedOverhead })
        {
            const glm::vec3 mean = BandMean(*frame, nearBandTop, kHeight);
            EXPECT_LT(mean.r, mean.b)
                << "magenta seafloor is showing through the projected surface (r " << mean.r
                << " vs b " << mean.b << ")";
        }

        // ---- and the horizon is calmer, not noisier -------------------------
        //
        // The band just below the horizon is where the world grid draws its
        // sub-pixel triangles. Sampling a surface below the raster rate produces
        // speckle, so the projected arm — whose triangles are pixels-wide
        // everywhere — should show no MORE horizontal detail there. This is the
        // "equal or better visual fidelity" half of the acceptance criterion,
        // measured rather than asserted from a screenshot.
        const u32 horizonTop = kHeight * 40u / 100u;
        const u32 horizonBottom = kHeight * 55u / 100u;
        const f64 worldDetail = BandHorizontalDetail(worldGrazing, horizonTop, horizonBottom);
        const f64 projectedDetail = BandHorizontalDetail(projectedGrazing, horizonTop, horizonBottom);
        std::cout << "[  PROFILE ] grazing horizon band, mean |dLuma/dx|: world grid " << worldDetail
                  << ", projected grid " << projectedDetail << std::endl;
        EXPECT_LE(projectedDetail, worldDetail * 1.15 + 0.5)
            << "the projected grid added high-frequency noise at the horizon, which is the "
               "artefact it exists to remove";
    }
} // namespace OloEngine::Tests
