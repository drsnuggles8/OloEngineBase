// OLO_TEST_LAYER: L8
// =============================================================================
// DriftIslandFieldEvidenceTest.cpp
//
// Visual evidence (PNG) for a Drift island as the game ships it (issues #880,
// #1721): Ridgeback, built from the SAME height map, splatmaps, macro map and
// layer textures Drift.olo names, read out of the scene file so the two cannot
// drift apart.
//
// Three contracts run on the rendered frame, none of them a golden:
//
//   1. TileBorderIsSeaNotCliff. A TERRAIN TILE MEETING AN OCEAN MUST NOT END IN
//      A WALL (#880). The tile's own border points are projected through the
//      capture camera and the pixels there must read as water. The CPU half of
//      the claim (every border texel of the map is the floor) is
//      DriftIslandSplatCoverage.
//   2. TheIslandShowsItsMaterials. Seen from above, the land is not one colour:
//      a real share of it is turf-green and a real share is rock-grey. That is
//      the whole textured path at once (layer arrays, splatmaps from files, the
//      macro map) reaching the frame; with any of them missing the island is one
//      flat tone, which is what the art review (#1721) found.
//   3. Every pose renders something (mean luma), so a black frame cannot pass.
//
// Contracts 1 and 2 run on Forward, Forward+ and Deferred, in this process and
// in `OloEngine-Tests --olo-rhi=vulkan`, and write
//   OloEditor/assets/tests/visual/DriftIsland_<Backend>_<Path>_<pose>.png
//
// Six poses on the default path are also GOLDENS (OpenGL only),
//   OloEditor/assets/tests/visual/DriftIsland_<pose>.png
// the poses #880 asked for: three approach distances, the waterline, submerged,
// and straight down. A normal run compares against the committed PNGs (RMSE)
// and writes nothing; pass --olo-golden-rebase after a deliberate visual change.
//
// What this deliberately does NOT cover: the island's foliage layers.
// UpdateImpostorAtlas bakes an octahedral atlas from a mesh asset on first use,
// which is a second subsystem's cost and failure mode inside what is meant to be
// a ground-and-shoreline test. Those are verified live in the editor.
//
// READING THE CAPTURES: the pale mountains along the horizon in the above-water
// poses are the SKYBOX (assets/textures/Skybox is a coastal cubemap with a
// mountain range in it), not geometry.
//
// Classification: L8 / integration (full pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>
#include <yaml-cpp/yaml.h>

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

        constexpr f32 kSeaSize = 1600.0f;
        constexpr f32 kSeafloorY = -26.0f;
        // Out-reaches the water tile, as in the shipped scene: any smaller and a
        // grazing look through the transparency finds the plane's own edge.
        constexpr f32 kSeafloorSize = kSeaSize * 1.5f;

        // The tests run with OloEditor/ as the working directory and no active
        // project, so the scene's project-relative "Assets/..." spelling is
        // given its working-directory one.
        [[nodiscard]] std::string FromWorkingDirectory(const std::string& projectRelative)
        {
            return projectRelative.empty() ? projectRelative : "SandboxProject/" + projectRelative;
        }

        // Ridgeback as Drift.olo authors it.
        struct IslandSource
        {
            glm::vec3 Translation{ 0.0f };
            f32 WorldSize = 0.0f;
            f32 HeightScale = 0.0f;
            f32 TargetTriangleSize = 8.0f;
            f32 MorphRegion = 0.3f;
            u32 LayerResolution = 512;
            std::string Heightmap;
            std::array<std::string, 2> Splatmaps;
            std::string MacroMap;
            std::vector<TerrainLayer> Layers;

            [[nodiscard]] glm::vec2 Centre() const
            {
                return { Translation.x + WorldSize * 0.5f, Translation.z + WorldSize * 0.5f };
            }
        };

        [[nodiscard]] bool LoadRidgeback(IslandSource& out, std::string& why)
        {
            const fs::path scenePath = fs::path("SandboxProject") / "Assets" / "Scenes" / "Drift.olo";
            if (!fs::exists(scenePath))
            {
                why = "Drift.olo not found at " + scenePath.generic_string();
                return false;
            }
            const YAML::Node root = YAML::LoadFile(scenePath.generic_string());
            for (const auto& entity : root["Entities"])
            {
                const YAML::Node tag = entity["TagComponent"];
                if (!tag || tag["Tag"].as<std::string>("") != "Island - Ridgeback")
                    continue;
                const YAML::Node terrain = entity["TerrainComponent"];
                const YAML::Node translation = entity["TransformComponent"]["Translation"];
                if (!terrain || !translation || translation.size() != 3)
                    break;
                out.Translation = { translation[0].as<f32>(), translation[1].as<f32>(), translation[2].as<f32>() };
                out.WorldSize = terrain["WorldSizeX"].as<f32>(0.0f);
                out.HeightScale = terrain["HeightScale"].as<f32>(0.0f);
                out.TargetTriangleSize = terrain["TargetTriangleSize"].as<f32>(out.TargetTriangleSize);
                out.MorphRegion = terrain["MorphRegion"].as<f32>(out.MorphRegion);
                out.LayerResolution = terrain["LayerResolution"].as<u32>(out.LayerResolution);
                out.Heightmap = FromWorkingDirectory(terrain["HeightmapPath"].as<std::string>(""));
                out.Splatmaps[0] = FromWorkingDirectory(terrain["SplatmapPath0"].as<std::string>(""));
                out.Splatmaps[1] = FromWorkingDirectory(terrain["SplatmapPath1"].as<std::string>(""));
                out.MacroMap = FromWorkingDirectory(terrain["MacroMapPath"].as<std::string>(""));
                for (const auto& node : terrain["Layers"])
                {
                    TerrainLayer layer;
                    layer.Name = node["Name"].as<std::string>("Unnamed");
                    layer.AlbedoPath = FromWorkingDirectory(node["AlbedoPath"].as<std::string>(""));
                    layer.NormalPath = FromWorkingDirectory(node["NormalPath"].as<std::string>(""));
                    layer.ARMPath = FromWorkingDirectory(node["ARMPath"].as<std::string>(""));
                    layer.TilingScale = node["TilingScale"].as<f32>(layer.TilingScale);
                    layer.HeightBlendSharpness = node["HeightBlendSharpness"].as<f32>(layer.HeightBlendSharpness);
                    layer.TriplanarSharpness = node["TriplanarSharpness"].as<f32>(layer.TriplanarSharpness);
                    if (const YAML::Node bc = node["BaseColor"]; bc && bc.size() == 3)
                        layer.BaseColor = { bc[0].as<f32>(), bc[1].as<f32>(), bc[2].as<f32>() };
                    layer.Roughness = node["Roughness"].as<f32>(layer.Roughness);
                    layer.Metallic = node["Metallic"].as<f32>(layer.Metallic);
                    out.Layers.push_back(std::move(layer));
                }
                if (out.WorldSize <= 0.0f || out.HeightScale <= 0.0f || out.Heightmap.empty() || out.Layers.empty())
                {
                    why = "Island - Ridgeback is not a height-map terrain with layers any more";
                    return false;
                }
                for (const std::string& file : { out.Heightmap, out.Splatmaps[0], out.Splatmaps[1], out.MacroMap })
                {
                    if (!fs::exists(file))
                    {
                        why = "Ridgeback names a map that is not on disk: " + file;
                        return false;
                    }
                }
                return true;
            }
            why = "no 'Island - Ridgeback' terrain in Drift.olo";
            return false;
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

        // The readback hands rows back bottom-up. stbi_write_png and every
        // "upper/lower part of the frame" statement below treat row 0 as the
        // TOP, so flip once, right after readback, and reason in image space.
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
            f32 OffShore;  // metres south of the island's south shore; ignored for TopDown
            f32 EyeHeight; // metres above the sea
            f32 Pitch;     // radians; POSITIVE tilts the view down
            const char* What;
        };

        // The boat's start line is south of Ridgeback, so every approach pose
        // looks north (+Z, yaw = pi) along the island's centre line.
        //
        // DISTANCES ARE TO THE SHORE, NOT TO THE CENTRE. The south shore on the
        // centre line is 110 m from the tile centre (a Jolt ray survey of the
        // shipped map); it is a constant here because a pose derived from the
        // map would move the camera whenever the map changed and make every
        // golden diff a camera diff.
        constexpr f32 kShoreFromCentre = 110.0f;
        constexpr f32 kPi = 3.14159265358979323846f;
        constexpr sizet kTopDownPose = 5;
        const std::array<Pose, 6> kPoses = { {
            { "ApproachFar", 445.0f, 7.0f, 0.02f, "445 m off the shore: the island as a silhouette on the horizon" },
            { "ApproachMid", 175.0f, 7.0f, 0.04f, "175 m off: relief and the shore band resolve" },
            { "ApproachNear", 60.0f, 7.0f, 0.05f, "60 m off: the LOD level the boat actually arrives at" },
            { "Waterline", 22.0f, 0.5f, 0.02f, "eye at deck height, 22 m off: the shore straddling the frame" },
            { "Submerged", 22.0f, -3.0f, -0.05f, "3 m under at the same spot, looking up at the surface" },
            { "TopDown", 0.0f, 430.0f, 1.53f, "straight down on the whole tile: where a border cliff would be obvious" },
        } };

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
                default:
                    return "Other";
            }
        }

        // Freezes the clock for a capture; restores whatever it found.
        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
                : m_HadMock(Time::HasMockTime()), m_Previous(Time::GetMockTime())
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                if (m_HadMock)
                    Time::SetMockTime(m_Previous);
                else
                    Time::ClearMockTime();
            }
            ScopedMockTime(const ScopedMockTime&) = delete;
            ScopedMockTime& operator=(const ScopedMockTime&) = delete;

          private:
            bool m_HadMock;
            f32 m_Previous;
        };
    } // namespace

    // The scene both suites below render: Ridgeback, the sea, its floor, a sun
    // and the coastal skybox.
    class DriftIslandScene : public RendererAttachedTest
    {
      protected:
        void BuildIslandScene()
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);
            // No editor grid: it is drawn through the shore band, where the
            // ground lies within a metre of the grid's own plane, and a frame
            // with it in is not what the game shows.
            Renderer3D::GetRendererSettings().ShowGrid = false;
            Renderer3D::ApplyRendererSettings();

            std::string why;
            ASSERT_TRUE(LoadRidgeback(m_Source, why)) << why;

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                // Travelling +Z, i.e. AWAY from every approach camera, so the face
                // the boat sails at is the lit one: a sun over the camera's
                // shoulder, about 40 degrees up. This scene has no time-of-day
                // clock; the shipped scene drives its sun off one, so its light is
                // not this light.
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

            // Seafloor: the same 2 m of clearance under the tile base the scene
            // uses. Higher than that and this plane would slice the island's
            // underwater flank into a flat lid.
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
                // The terrain origin is a CORNER, as in the scene.
                m_Island.GetComponent<TransformComponent>().Translation = m_Source.Translation;

                auto& terrain = m_Island.AddComponent<TerrainComponent>();
                terrain.m_HeightmapPath = m_Source.Heightmap;
                terrain.m_WorldSizeX = m_Source.WorldSize;
                terrain.m_WorldSizeZ = m_Source.WorldSize;
                terrain.m_HeightScale = m_Source.HeightScale;
                // No Jolt body: nothing in this test drives physics.
                terrain.m_CollisionEnabled = false;
                terrain.m_ProceduralEnabled = false;
                terrain.m_AutoMaterial = false;
                terrain.m_TessellationEnabled = true;
                terrain.m_TargetTriangleSize = m_Source.TargetTriangleSize;
                terrain.m_MorphRegion = m_Source.MorphRegion;

                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const TerrainLayer& layer : m_Source.Layers)
                    terrain.m_Material->AddLayer(layer);
                terrain.m_Material->SetSplatmapPath(0, m_Source.Splatmaps[0]);
                terrain.m_Material->SetSplatmapPath(1, m_Source.Splatmaps[1]);
                terrain.m_Material->SetMacroMapPath(m_Source.MacroMap);
                terrain.m_Material->SetLayerResolution(m_Source.LayerResolution);
                terrain.m_MaterialNeedsRebuild = true;
            }
        }

        [[nodiscard]] glm::vec3 PosePosition(const Pose& pose) const
        {
            const glm::vec2 centre = m_Source.Centre();
            if (pose.OffShore <= 0.0f)
                return { centre.x, pose.EyeHeight, centre.y };
            return { centre.x, pose.EyeHeight, centre.y - kShoreFromCentre - pose.OffShore };
        }

        // Pose, tick, read back, flip. Callers MUST wrap this in
        // ASSERT_NO_FATAL_FAILURE: the ASSERT_ below returns from the helper,
        // not from the test.
        void Capture(const Pose& pose, std::vector<u8>& outPixels, EditorCamera& outCamera)
        {
            outCamera = EditorCamera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.2f, 4000.0f);
            outCamera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            outCamera.SetPose(PosePosition(pose), kPi, pose.Pitch);

            // The first ticks load the maps and build the chunk meshes, both
            // quadtrees and the layer arrays; the last renders the ready terrain.
            RunEditorFrames(outCamera, 6);

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, width, height)) << "no composited frame for '" << pose.Name << "'";
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);
            FlipRowsInPlace(outPixels, kWidth, kHeight);
        }

        static void ExpectNotBlack(const Pose& pose, const std::vector<u8>& pixels)
        {
            u64 lumaSum = 0;
            for (sizet i = 0; i + 3 < pixels.size(); i += 4)
                lumaSum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            const f64 meanChannel = static_cast<f64>(lumaSum) / (static_cast<f64>(kWidth) * kHeight * 3.0);
            EXPECT_GT(meanChannel, 5.0) << "Pose '" << pose.Name << "' (" << pose.What << ") rendered (near-)black";
        }

        // The #880 claim, on a rendered frame: the tile ENDS in sea, not in a
        // wall. Looking straight down, the midpoint of each border and each
        // corner is projected through the capture camera and the pixel there
        // must be blue-dominant: no ground in the palette is, and the sea is.
        // Sampled at the TILE BASE, where the border surface actually is: a
        // 24 m height error at a 207 m offset would shift the sample 11 m and
        // put it outside the tile, where the sea is open whatever the tile does.
        void ExpectTileBorderIsSea(const EditorCamera& camera, const std::vector<u8>& pixels, const std::string& evidence) const
        {
            const glm::vec2 centre = m_Source.Centre();
            const f32 sampleY = m_Source.Translation.y;
            const f32 edge = m_Source.WorldSize * 0.5f * 0.985f;
            struct BorderPoint
            {
                const char* Name;
                glm::vec3 World;
            };
            const std::array<BorderPoint, 8> border = { {
                { "+X edge", { centre.x + edge, sampleY, centre.y } },
                { "-X edge", { centre.x - edge, sampleY, centre.y } },
                { "+Z edge", { centre.x, sampleY, centre.y + edge } },
                { "-Z edge", { centre.x, sampleY, centre.y - edge } },
                { "+X+Z corner", { centre.x + edge, sampleY, centre.y + edge } },
                { "+X-Z corner", { centre.x + edge, sampleY, centre.y - edge } },
                { "-X+Z corner", { centre.x - edge, sampleY, centre.y + edge } },
                { "-X-Z corner", { centre.x - edge, sampleY, centre.y - edge } },
            } };

            const glm::mat4 viewProjection = camera.GetViewProjection();
            u32 sampled = 0;
            for (const BorderPoint& point : border)
            {
                const glm::vec4 clip = viewProjection * glm::vec4(point.World, 1.0f);
                ASSERT_GT(clip.w, 0.0f) << point.Name << " projected behind the camera";
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                if (std::fabs(ndc.x) > 1.0f || std::fabs(ndc.y) > 1.0f)
                    continue; // outside the frame at this framing
                const u32 x = static_cast<u32>((ndc.x * 0.5f + 0.5f) * static_cast<f32>(kWidth - 1u));
                // Rows are in image space (row 0 = top), so +Y in NDC is a
                // SMALLER row index.
                const u32 y = static_cast<u32>((1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<f32>(kHeight - 1u));
                const sizet idx = (static_cast<sizet>(y) * kWidth + x) * 4u;
                ASSERT_LT(idx + 2u, pixels.size()) << point.Name;
                const int r = pixels[idx + 0];
                const int b = pixels[idx + 2];
                EXPECT_GT(b, r) << "Tile border point '" << point.Name << "' at pixel (" << x << ", " << y
                                << ") reads as land (r=" << r << ", b=" << b << "), not sea. See " << evidence;
                ++sampled;
            }
            // Anti-vacuity: with the tile out of frame every point is skipped.
            EXPECT_GE(sampled, 6u) << "only " << sampled << " of 8 tile-border points were in frame";
        }

        // Contract 2. Land is every pixel that is not blue-dominant. On it, the
        // turf is green-dominant and the gneiss is near-neutral; each must hold
        // a real share. Shares are of LAND pixels, so the sea's size is not in
        // the number. Loose on purpose: the map gives turf 45% and rock 48% of
        // the land, and the bound only has to fail an island of one tone.
        static void ExpectTheIslandShowsItsMaterials(const std::vector<u8>& pixels, const std::string& evidence)
        {
            u32 land = 0;
            u32 green = 0;
            u32 grey = 0;
            for (sizet i = 0; i + 3 < pixels.size(); i += 4)
            {
                const f32 r = pixels[i];
                const f32 g = pixels[i + 1];
                const f32 b = pixels[i + 2];
                if (b > r && b > g)
                    continue; // sea
                ++land;
                const f32 top = std::max({ r, g, b, 1.0f });
                if (g > r * 1.06f && g > b * 1.06f)
                    ++green;
                else if ((top - std::min({ r, g, b })) / top < 0.16f)
                    ++grey;
            }
            ASSERT_GT(land, kWidth * kHeight / 50u) << "the island covers under 2% of the frame. See " << evidence;
            const f32 greenShare = static_cast<f32>(green) / static_cast<f32>(land);
            const f32 greyShare = static_cast<f32>(grey) / static_cast<f32>(land);
            EXPECT_GT(greenShare, 0.12f) << "turf covers " << greenShare * 100.0f << "% of the land. See " << evidence;
            EXPECT_GT(greyShare, 0.12f) << "bare rock covers " << greyShare * 100.0f << "% of the land. See " << evidence;
        }

        IslandSource m_Source;
        Entity m_Island;
    };

    // ── The goldens: six poses, default path, OpenGL ─────────────────────────

    class DriftIslandFieldEvidenceTest : public DriftIslandScene
    {
      protected:
        void BuildScene() override
        {
            BuildIslandScene();
        }

        // Golden model: rebase writes, a normal run compares and writes nothing.
        void CompareOrRebase(const std::string& poseName, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("DriftIsland_" + poseName + ".png")).string();

            if (OloEngine::Tests::Options().GoldenRebase)
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string() << "': " << ec.message();
                ::stbi_flip_vertically_on_write(0);
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                                   4, pixels.data(), static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed to write golden '" << path << "'";
                return;
            }

            int goldenWidth = 0;
            int goldenHeight = 0;
            int goldenChannels = 0;
            ::stbi_set_flip_vertically_on_load_thread(0);
            stbi_uc* golden = ::stbi_load(path.c_str(), &goldenWidth, &goldenHeight, &goldenChannels, 4);
            ASSERT_NE(golden, nullptr) << "Missing golden '" << path << "'; rerun with --olo-golden-rebase.";
            const bool sizeMatches = (goldenWidth == static_cast<int>(kWidth) && goldenHeight == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<sizet>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << goldenWidth << "x" << goldenHeight
                                     << ", expected " << kWidth << "x" << kHeight << "; rerun with --olo-golden-rebase.";

            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Pose '" << poseName << "' diverged from golden (RMSE " << rmse << " > " << kGoldenRmseThreshold
                << "). If this is an intended visual change, rerun with --olo-golden-rebase to update " << path;
        }
    };

    TEST_F(DriftIslandFieldEvidenceTest, CaptureApproachSeriesAndWaterline)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime scopedMockTime(kCaptureTime);

        for (const Pose& pose : kPoses)
        {
            std::vector<u8> pixels;
            EditorCamera camera;
            ASSERT_NO_FATAL_FAILURE(Capture(pose, pixels, camera)) << pose.Name;
            // A black frame must never pass as "matches the golden", which it
            // would if a golden were ever rebased from a broken run.
            ExpectNotBlack(pose, pixels);
            ASSERT_NO_FATAL_FAILURE(CompareOrRebase(pose.Name, pixels)) << pose.Name;
        }
    }

    // ── The contracts: every path, both backends ─────────────────────────────

    class DriftIslandPaths : public DriftIslandScene, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            Renderer3D::GetRendererSettings().Path = GetParam();
            BuildIslandScene(); // applies the renderer settings
        }

        [[nodiscard]] std::string WriteEvidence(const char* poseName, const std::vector<u8>& pixels) const
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string file =
                (dir / (TestRenderBackend::EvidenceName("DriftIsland") + PathName(GetParam()) + "_" + poseName + ".png")).string();
            ::stbi_flip_vertically_on_write(0);
            EXPECT_NE(::stbi_write_png(file.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       pixels.data(), static_cast<int>(kWidth) * 4),
                      0)
                << "could not write " << file;
            return file;
        }
    };

    TEST_P(DriftIslandPaths, TheTileEndsInSeaAndTheIslandShowsItsMaterials)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime scopedMockTime(kCaptureTime);

        const Pose& topDown = kPoses[kTopDownPose];
        ASSERT_STREQ(topDown.Name, "TopDown");
        std::vector<u8> pixels;
        EditorCamera camera;
        ASSERT_NO_FATAL_FAILURE(Capture(topDown, pixels, camera));
        const std::string evidence = WriteEvidence(topDown.Name, pixels);

        ExpectNotBlack(topDown, pixels);
        ASSERT_NO_FATAL_FAILURE(ExpectTileBorderIsSea(camera, pixels, evidence));
        ASSERT_NO_FATAL_FAILURE(ExpectTheIslandShowsItsMaterials(pixels, evidence));
    }

    // The view the boat arrives at: the shore band, the turf slope and the rock
    // above it in one frame. Evidence for each path and backend; the frame must
    // not be black.
    TEST_P(DriftIslandPaths, TheArrivalViewRenders)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime scopedMockTime(kCaptureTime);

        const Pose& arrival = kPoses[2];
        ASSERT_STREQ(arrival.Name, "ApproachNear");
        std::vector<u8> pixels;
        EditorCamera camera;
        ASSERT_NO_FATAL_FAILURE(Capture(arrival, pixels, camera));
        (void)WriteEvidence(arrival.Name, pixels);
        ExpectNotBlack(arrival, pixels);
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, DriftIslandPaths,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus,
                                               RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });
} // namespace OloEngine::Tests
