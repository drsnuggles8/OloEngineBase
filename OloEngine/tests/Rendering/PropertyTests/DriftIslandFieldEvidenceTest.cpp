// OLO_TEST_LAYER: L8
// =============================================================================
// DriftIslandFieldEvidenceTest.cpp
//
// Visual evidence (PNG) for the Drift procedural island field (issue #880), and
// the one contract that decides whether the feature works at all:
//
//   A PROCEDURAL TERRAIN TILE MEETING AN OCEAN MUST NOT END IN A WALL.
//
// Every shaping knob TerrainGenerator had before #880 was uniform over the tile,
// so whatever the noise happened to leave high at the tile BORDER became a
// vertical cliff dropping into the sea. Measured on the Drift island as it
// shipped: 64.9% of its border stood above sea level. #880 added
// TerrainHeightShaping::IslandFalloff, a radial mask that drives every border
// texel to the tile's base height, and this scene mirrors Drift.olo's Ridgeback
// island (same seed / shaping / height scale, base 24 m under water) so the PNGs
// show the real thing rather than a demo.
//
// Six poses are captured to
//   OloEditor/assets/tests/visual/DriftIsland_<pose>.png
// and they are the poses the issue asks for: three approach distances, the
// waterline, submerged, and straight down.
//
// Two DRIVER-INDEPENDENT contracts run alongside the golden compare, because a
// golden only ever answers "did it change?":
//
//   1. TileBorderIsSeaNotCliff — projects the tile's own border points through
//      the capture camera's view-projection and asserts the pixels there read as
//      water, not terrain. That is the #880 claim, checked on a rendered frame
//      rather than inferred from the height field. The CPU side of the same
//      claim (border height == 0 exactly, at several resolutions) lives in
//      TerrainGeneratorTest.
//   2. Every pose renders something (mean luma), so a black frame cannot pass as
//      "matches the golden" if the golden were ever rebased black.
//
// What this test deliberately does NOT cover: the foliage/impostor layers the
// shipped scene puts on each island. UpdateImpostorAtlas bakes an octahedral
// atlas from a mesh asset on first use, which is a second subsystem's cost and
// failure mode inside what is meant to be a shoreline test. Those are verified
// live in the editor instead; ImpostorBakeEvidenceTest owns the bake itself.
//
// READING THE CAPTURES: the pale mountains along the horizon in every above-water
// pose are the SKYBOX (assets/textures/Skybox is a coastal cubemap with a mountain
// range in it), not geometry and not the seafloor plane showing past the water's
// edge. That was checked rather than assumed — the first read of these goldens
// diagnosed them as an uncovered seafloor ring and shrank the plane, which changed
// nothing because the mountains are painted on the sky.
//
// Runs in the normal suite: SKIPs cleanly (not fails) without a GL 4.6 context,
// like every other RendererAttachedTest capture. A normal run COMPARES against
// the committed PNGs (RMSE) and writes nothing, so a passing run leaves no
// tracked-file churn; pass --olo-golden-rebase to (re)write them after a
// deliberate visual change. Run from OloEditor/ so assets resolve.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Frozen wall clock: the Gerstner phase and the normal-map scroll come
        // from Time::GetTime(), so pinning it is what makes these PNGs usable as
        // golden references at all.
        constexpr f32 kCaptureTime = 12.0f;

        // Same slack as WaterVisualEvidenceTest: same-machine re-runs land at ~0,
        // and the margin absorbs cross-GPU float variance in the high-frequency
        // wave/foam detail.
        constexpr f64 kGoldenRmseThreshold = 6.0;

        // ── Ridgeback, transcribed from Drift.olo ────────────────────────────
        // Only the heightmap resolution differs (256 rather than the shipped
        // 512), and deliberately: the noise is sampled at x/resolution, so a
        // coarser grid is the SAME field read at fewer points, not a different
        // island. The border guarantee is a property of the mask, which is
        // evaluated per texel at whatever resolution is asked for — and
        // TerrainGeneratorTest sweeps resolutions for exactly that reason.
        constexpr f32 kTileSize = 420.0f;    // metres square
        constexpr f32 kHeightScale = 110.0f; // metres, 0..1 of the field maps onto this
        constexpr f32 kBaseY = -24.0f;       // tile origin: the border sits here, 24 m under
        constexpr f32 kIslandCentreX = 0.0f;
        constexpr f32 kIslandCentreZ = 280.0f;
        constexpr f32 kSeaSize = 1600.0f;
        constexpr f32 kSeafloorY = -26.0f;
        // Out-reaches the water tile, as in the shipped scene: any smaller and a
        // grazing look through the transparency finds the plane's own edge.
        constexpr f32 kSeafloorSize = kSeaSize * 1.5f;

        // Waterline as a fraction of the height range — the number every layer
        // band below is anchored on, matching the scene's own derivation.
        constexpr f32 kSeaLevel01 = -kBaseY / kHeightScale; // 0.218

        [[nodiscard]] constexpr f32 AboveWater(f32 fraction)
        {
            return kSeaLevel01 + fraction * (1.0f - kSeaLevel01);
        }

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::max();
            f64 sumSq = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        // glGetTextureImage hands back rows bottom-up (GL origin). stbi_write_png
        // and every "upper/lower part of the frame" statement below treat row 0
        // as the TOP, so flip once, right after readback, and reason in image
        // space from then on.
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

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;   // radians; 0 looks toward -Z, so pi looks toward +Z
            f32 Pitch; // radians; POSITIVE tilts the view down
            const char* What;
        };

        // The island sits at +Z from the boat's start line, so every approach pose
        // looks toward +Z (yaw = pi).
        //
        // DISTANCES ARE TO THE SHORE, NOT TO THE CENTRE, and the shore is not
        // where the falloff radius puts it. The mask starts falling at
        // `IslandFalloffRadius * tile` (126 m here) and reaches zero at the tile's
        // inscribed circle (210 m); land survives anywhere between the two that
        // the noise left high enough, so this island's coastline is at ~165 m from
        // the centre — world z = 115 on the approach axis. Estimating it from the
        // falloff radius put the first two "approach" poses 5 and 13 m INLAND,
        // which renders as a wall of rock filling the frame and is very hard to
        // read as a mis-placed camera rather than a broken island.
        constexpr f32 kShoreZ = 115.0f; // measured, not derived — see above
        constexpr f32 kPi = 3.14159265358979323846f;
        const std::array<Pose, 6> kPoses = { {
            { "ApproachFar", { 0.0f, 7.0f, kShoreZ - 445.0f }, kPi, 0.02f, "445 m off the shore — the island as a silhouette on the horizon" },
            { "ApproachMid", { 0.0f, 7.0f, kShoreZ - 175.0f }, kPi, 0.04f, "175 m off — relief and the shore band resolve" },
            { "ApproachNear", { 0.0f, 7.0f, kShoreZ - 60.0f }, kPi, 0.05f, "60 m off — the LOD level the boat actually arrives at" },
            { "Waterline", { 0.0f, 0.5f, kShoreZ - 22.0f }, kPi, 0.02f, "eye at deck height, 22 m off — the shore straddling the frame" },
            { "Submerged", { 0.0f, -3.0f, kShoreZ - 22.0f }, kPi, -0.05f, "3 m under at the same spot, looking up at the surface" },
            { "TopDown", { kIslandCentreX, 430.0f, kIslandCentreZ }, kPi, 1.53f, "straight down on the whole tile — where a border cliff would be obvious" },
        } };

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }
    } // namespace

    class DriftIslandFieldEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                // Travelling +Z, i.e. AWAY from every approach camera, so the face
                // the boat sails at is the lit one — a sun over the camera's
                // shoulder. Elevated enough (~40 deg) that the near shore is not
                // in the island's own shadow at the close poses, which is what
                // turned the first waterline capture into a black wall, and still
                // low enough to keep the relief that tells one island from
                // another. This scene has no time-of-day clock; the shipped scene
                // drives its sun off one, so its light is not this light.
                dl.m_Direction = glm::normalize(glm::vec3(-0.30f, -0.84f, 0.45f));
                dl.m_Color = glm::vec3(1.0f, 0.94f, 0.86f);
                dl.m_Intensity = 3.0f;
            }

            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = true;
                env.m_IBLIntensity = 0.35f;
            }

            // Seafloor — the same 2 m of clearance under the tile base the scene
            // uses. Higher than that and this plane would slice the island's
            // underwater flank into a flat lid, which is a different artefact
            // that looks exactly like the one being tested for.
            {
                Entity floor = scene.CreateEntity("Seafloor");
                auto& tc = floor.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, kSeafloorY, 0.0f };
                tc.Scale = { kSeafloorSize, 1.0f, kSeafloorSize };
                auto& mc = floor.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> plane = MeshPrimitives::CreatePlane())
                    mc.m_MeshSource = plane->GetMeshSource();
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.30f, 0.29f, 0.24f, 1.0f));
            }

            {
                Entity sea = scene.CreateEntity("Sea");
                auto& wc = sea.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = kSeaSize;
                wc.m_WorldSizeZ = kSeaSize;
                // 2.5 m per quad, the density the shipped scene is authored at.
                wc.m_GridResolutionX = 640;
                wc.m_GridResolutionZ = 640;
                wc.m_WaveAmplitude = 0.12f;
                wc.m_WaveFrequency = 0.55f;
                wc.m_WaterColor = glm::vec3(0.09f, 0.33f, 0.44f);
                wc.m_DeepColor = glm::vec3(0.015f, 0.075f, 0.14f);
                wc.m_RenderFromBelow = true;
                wc.m_UnderwaterFogColor = glm::vec3(0.04f, 0.18f, 0.3f);
                wc.m_UnderwaterFogDensity = 0.09f;
                wc.m_UseFFT = false;
            }

            {
                m_Island = scene.CreateEntity("Island - Ridgeback");
                auto& tc = m_Island.GetComponent<TransformComponent>();
                // The terrain origin is a CORNER, so translate by half the tile.
                tc.Translation = { kIslandCentreX - kTileSize * 0.5f, kBaseY, kIslandCentreZ - kTileSize * 0.5f };

                auto& terrain = m_Island.AddComponent<TerrainComponent>();
                terrain.m_WorldSizeX = kTileSize;
                terrain.m_WorldSizeZ = kTileSize;
                terrain.m_HeightScale = kHeightScale;
                // No Jolt body: nothing in this test drives physics, and building
                // one would be a heightfield-shape cost per run for no assertion.
                terrain.m_CollisionEnabled = false;
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 879;
                terrain.m_ProceduralResolution = 256;
                terrain.m_ProceduralOctaves = 6;
                terrain.m_ProceduralFrequency = 2.4f;
                terrain.m_ProceduralLacunarity = 2.0f;
                terrain.m_ProceduralPersistence = 0.46f;
                terrain.m_HeightShaping.RidgeBlend = 0.72f;
                terrain.m_HeightShaping.WarpStrength = 0.18f;
                terrain.m_HeightShaping.WarpFrequency = 2.0f;
                terrain.m_HeightShaping.HeightExponent = 1.15f;
                // The subject of this whole file.
                terrain.m_HeightShaping.IslandFalloff = 1.0f;
                terrain.m_HeightShaping.IslandFalloffRadius = 0.30f;

                // The GPU LOD quadtree, which is what makes the approach series
                // resolve rather than pop. No Drift terrain set this before #880.
                terrain.m_TessellationEnabled = true;
                terrain.m_TargetTriangleSize = 10.0f;

                terrain.m_AutoMaterial = true;
                terrain.m_SplatmapGenResolution = 256;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : MakeShoreLayers())
                    terrain.m_Material->AddLayer(layer);
                terrain.m_LayerRules = MakeShoreRules();
                terrain.m_MaterialNeedsRebuild = true;
                terrain.m_AutoSplatNeedsRebuild = true;
            }
        }

        // Sand / scrub / rock / heath, banded on this island's own waterline the
        // way Drift.olo bands every one of its six. Solid colours — no texture
        // assets needed, so the capture is the same with or without a cook.
        [[nodiscard]] static std::vector<TerrainLayer> MakeShoreLayers()
        {
            const auto layer = [](const char* name, glm::vec3 colour, f32 tiling, f32 roughness)
            {
                TerrainLayer l;
                l.Name = name;
                l.BaseColor = colour;
                l.TilingScale = tiling;
                l.Roughness = roughness;
                l.Metallic = 0.0f;
                return l;
            };
            return {
                layer("Sand", { 0.78f, 0.72f, 0.55f }, 22.0f, 0.95f),
                layer("Scrub", { 0.28f, 0.40f, 0.21f }, 24.0f, 0.92f),
                layer("Rock", { 0.44f, 0.41f, 0.38f }, 14.0f, 0.85f),
                layer("Heath", { 0.52f, 0.50f, 0.44f }, 18.0f, 0.80f),
            };
        }

        [[nodiscard]] static std::vector<TerrainLayerRule> MakeShoreRules()
        {
            const auto rule = [](u32 index, f32 minH, f32 maxH, f32 heightBlend, f32 minSlope, f32 maxSlope,
                                 f32 slopeBlend)
            {
                TerrainLayerRule r;
                r.LayerIndex = index;
                r.MinHeight = minH;
                r.MaxHeight = maxH;
                r.HeightBlend = heightBlend;
                r.MinSlopeDeg = minSlope;
                r.MaxSlopeDeg = maxSlope;
                r.SlopeBlend = slopeBlend;
                r.Strength = 1.0f;
                return r;
            };
            return {
                rule(0, 0.0f, AboveWater(0.10f), 0.05f, 0.0f, 40.0f, 8.0f),
                rule(1, AboveWater(0.12f), AboveWater(0.66f), 0.06f, 0.0f, 30.0f, 8.0f),
                rule(2, 0.0f, 1.0f, 0.0f, 34.0f, 90.0f, 10.0f),
                rule(3, AboveWater(0.80f), 1.0f, 0.05f, 0.0f, 45.0f, 10.0f),
            };
        }

        // Pose, tick, read back, flip. Callers MUST wrap this in
        // ASSERT_NO_FATAL_FAILURE — the ASSERT_ below returns from the helper,
        // not from the test, so a failed readback would otherwise leave
        // `outPixels` empty and let every assertion run on nothing.
        void Capture(const Pose& pose, std::vector<u8>& outPixels, EditorCamera& outCamera)
        {
            outCamera = EditorCamera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.2f, 4000.0f);
            outCamera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            outCamera.SetPose(pose.Position, pose.Yaw, pose.Pitch);

            // The first ticks build the height field, the chunk meshes, both
            // quadtrees and the auto-splat; the last renders the ready terrain.
            RunEditorFrames(outCamera, 4);

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, width, height)) << "no composited frame for '" << pose.Name << "'";
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);
            FlipRowsInPlace(outPixels, kWidth, kHeight);
        }

        // Golden model: rebase writes, a normal run compares and writes nothing.
        void CompareOrRebase(const std::string& poseName, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("DriftIsland_" + poseName + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string() << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                                   4, pixels.data(), static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed to write golden '" << path << "'";
                return;
            }

            int goldenWidth = 0;
            int goldenHeight = 0;
            int goldenChannels = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &goldenWidth, &goldenHeight, &goldenChannels, 4);
            ASSERT_NE(golden, nullptr) << "Missing golden '" << path << "' — rerun with --olo-golden-rebase.";
            const bool sizeMatches = (goldenWidth == static_cast<int>(kWidth) && goldenHeight == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<sizet>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << goldenWidth << "x" << goldenHeight
                                     << ", expected " << kWidth << "x" << kHeight << " — rerun with --olo-golden-rebase.";

            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Pose '" << poseName << "' diverged from golden (RMSE " << rmse << " > " << kGoldenRmseThreshold
                << "). If this is an intended visual change, rerun with --olo-golden-rebase to update " << path;
        }

        Entity m_Island;
    };

    // Freezes the clock for the whole run so every pose is deterministic — the
    // precondition for treating the PNGs as goldens. RAII restores the real clock
    // on every exit path, including an ASSERT early-return.
    namespace
    {
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
    } // namespace

    TEST_F(DriftIslandFieldEvidenceTest, CaptureApproachSeriesAndWaterline)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        for (const Pose& pose : kPoses)
        {
            std::vector<u8> pixels;
            EditorCamera camera;
            ASSERT_NO_FATAL_FAILURE(Capture(pose, pixels, camera)) << pose.Name;

            // A black frame must never pass as "matches the golden" — which it
            // would, silently, if a golden were ever rebased from a broken run.
            u64 lumaSum = 0;
            for (sizet i = 0; i + 3 < pixels.size(); i += 4)
                lumaSum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            const f64 meanChannel = static_cast<f64>(lumaSum) / (static_cast<f64>(kWidth) * kHeight * 3.0);
            EXPECT_GT(meanChannel, 5.0) << "Pose '" << pose.Name << "' (" << pose.What << ") rendered (near-)black";

            ASSERT_NO_FATAL_FAILURE(CompareOrRebase(pose.Name, pixels)) << pose.Name;
        }
    }

    // The #880 claim, on a rendered frame: the tile ENDS in sea, not in a wall.
    //
    // Looking straight down at the tile, project the midpoint of each of its four
    // borders (and its four corners) into screen space through the capture
    // camera's own view-projection, and sample the pixel there. With the radial
    // mask on, the surface at those points is 24 m under water, so what is drawn
    // is open sea — blue-dominant. Without the mask two thirds of that ring was
    // terrain, which is sand/rock/scrub: red at least as strong as blue.
    //
    // The blue-over-red test is what makes this driver-independent: it does not
    // care about the exact shade of either, only that no plausible terrain colour
    // in the palette above is blue-dominant and no plausible sea colour is not.
    TEST_F(DriftIslandFieldEvidenceTest, TileBorderIsSeaNotCliff)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime scopedMockTime(kCaptureTime);

        const Pose& topDown = kPoses[5];
        ASSERT_STREQ(topDown.Name, "TopDown");

        std::vector<u8> pixels;
        EditorCamera camera;
        ASSERT_NO_FATAL_FAILURE(Capture(topDown, pixels, camera));

        // Sampled at the TILE BASE, not at y = 0: that is where the masked border
        // surface actually is, and the TopDown camera is near-vertical rather than
        // vertical. A 24 m height error at a ~207 m offset shifts the projected
        // pixel radially by ~207 * 24 / 430 = 11 m, which is well past the 3 m
        // inset below — the sample would land outside the tile, where the sea is
        // open whether or not the falloff works, and the contract would pass
        // vacuously.
        const f32 sampleY = kBaseY;
        const f32 half = kTileSize * 0.5f;
        // Pulled a hair inside the tile so a sample can never land on the very
        // first texel column and be argued about; the mask is zero well before
        // this (the ramp ends on the outermost texel centre).
        const f32 edge = half * 0.985f;
        struct BorderPoint
        {
            const char* Name;
            glm::vec3 World;
        };
        const std::array<BorderPoint, 8> border = { {
            { "+X edge", { kIslandCentreX + edge, sampleY, kIslandCentreZ } },
            { "-X edge", { kIslandCentreX - edge, sampleY, kIslandCentreZ } },
            { "+Z edge", { kIslandCentreX, sampleY, kIslandCentreZ + edge } },
            { "-Z edge", { kIslandCentreX, sampleY, kIslandCentreZ - edge } },
            { "+X+Z corner", { kIslandCentreX + edge, sampleY, kIslandCentreZ + edge } },
            { "+X-Z corner", { kIslandCentreX + edge, sampleY, kIslandCentreZ - edge } },
            { "-X+Z corner", { kIslandCentreX - edge, sampleY, kIslandCentreZ + edge } },
            { "-X-Z corner", { kIslandCentreX - edge, sampleY, kIslandCentreZ - edge } },
        } };

        const glm::mat4 viewProjection = camera.GetViewProjection();
        u32 sampled = 0;
        for (const BorderPoint& point : border)
        {
            const glm::vec4 clip = viewProjection * glm::vec4(point.World, 1.0f);
            ASSERT_GT(clip.w, 0.0f) << point.Name << " projected behind the camera";
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (std::fabs(ndc.x) > 1.0f || std::fabs(ndc.y) > 1.0f)
                continue; // outside the frame at this framing — nothing to say about it

            const u32 x = static_cast<u32>((ndc.x * 0.5f + 0.5f) * static_cast<f32>(kWidth - 1u));
            // Rows were flipped to image space (row 0 = top), so +Y in NDC is UP
            // and therefore a SMALLER row index.
            const u32 y = static_cast<u32>((1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<f32>(kHeight - 1u));
            const sizet idx = (static_cast<sizet>(y) * kWidth + x) * 4u;
            ASSERT_LT(idx + 2u, pixels.size()) << point.Name;

            const int r = pixels[idx + 0];
            const int b = pixels[idx + 2];
            EXPECT_GT(b, r) << "Tile border point '" << point.Name << "' at pixel (" << x << ", " << y
                            << ") reads as land (r=" << r << ", b=" << b << "), not sea — the island falloff is "
                            << "not reaching the tile edge. See DriftIsland_TopDown.png.";
            ++sampled;
        }

        // Anti-vacuity: if the framing ever changes so the tile no longer fits in
        // frame, every point would be skipped and the loop above would assert
        // nothing at all while reporting green.
        EXPECT_GE(sampled, 6u) << "only " << sampled << " of 8 tile-border points were in frame — "
                               << "the TopDown pose no longer frames the tile, so this test proves nothing";
    }
} // namespace OloEngine::Tests
