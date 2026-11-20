// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include "OloEngine/Containers/Array.h"
#include <gtest/gtest.h>

#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainLayer.h"

#include <glm/glm.hpp>
#include <stb_image/stb_image.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

// =============================================================================
// The six Drift islands' maps (issues #942, #1721).
//
// Each island in Drift.olo is a height map with two splatmaps and a macro map,
// written by tools/drift-islands. This reads the shipped scene and the shipped
// maps and pins what the art review (#1721) found false of the islands they
// replaced, as numbers rather than as a picture:
//
//   * the tile's border is the seabed floor, so its edge is never a wall;
//   * the coast is a beach or a rock platform, not the 45-60 degree ramp a
//     radial mask leaves at the waterline;
//   * the landfall ring the weather director derives from the scene holds the
//     island, so a leg begins offshore and not on the beach;
//   * every texel's layer weights are a whole, and more than one material
//     shows above the water (#942: one layer once took 77-93% of an island);
//   * the ground's albedo is physical, and no two islands share a palette.
//
// Reads the scene YAML directly, NOT through SceneSerializer (which builds GPU
// resources for the whole scene), and decodes the maps on the CPU, so retuning
// the generator and this test cannot drift and no GL context is needed.
// Classification: unit.
// =============================================================================

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr u32 kIslandCount = 6;

        struct Island
        {
            std::string Name;
            f32 WorldSize = 0.0f;
            f32 HeightScale = 0.0f;
            f32 SeaLevel01 = 0.0f; // the sea's height as a fraction of HeightScale
            f32 LandfallRadius = 0.0f;
            u32 Resolution = 0;
            TArray<f32> Heights;                  // [0, 1], row-major
            std::array<std::vector<u8>, 2> Splat; // RGBA8, Resolution^2 texels each
            std::vector<u8> Macro;                // RGBA8
            std::vector<glm::vec3> LayerColours;  // each layer's mean linear albedo
            std::vector<std::string> LayerNames;

            [[nodiscard]] f32 MetresAboveSea(sizet texel) const
            {
                return (Heights[static_cast<TArray<f32>::SizeType>(texel)] - SeaLevel01) * HeightScale;
            }
            [[nodiscard]] std::array<f32, MAX_TERRAIN_LAYERS> Weights(sizet texel) const
            {
                std::array<f32, MAX_TERRAIN_LAYERS> w{};
                for (u32 i = 0; i < MAX_TERRAIN_LAYERS; ++i)
                    w[i] = static_cast<f32>(Splat[i / 4][texel * 4 + (i % 4)]);
                return w;
            }
        };

        [[nodiscard]] fs::path ProjectDir()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";
        }

        [[nodiscard]] bool LoadRGBA(const fs::path& file, std::vector<u8>& out, u32& outSize)
        {
            int w = 0;
            int h = 0;
            int channels = 0;
            stbi_set_flip_vertically_on_load_thread(0);
            stbi_uc* pixels = stbi_load(file.string().c_str(), &w, &h, &channels, 4);
            if (!pixels)
                return false;
            const bool square = (w == h) && w > 0;
            if (square)
            {
                outSize = static_cast<u32>(w);
                out.assign(pixels, pixels + static_cast<sizet>(w) * static_cast<sizet>(h) * 4);
            }
            stbi_image_free(pixels);
            return square;
        }

        // Every "Island - <name>" terrain, with its maps decoded. A map that is
        // missing or malformed is reported through `problems`, not skipped: an
        // island that silently dropped out would pass every loop below.
        [[nodiscard]] std::vector<Island> LoadIslands(std::vector<std::string>& problems)
        {
            std::vector<Island> islands;
            const fs::path scenePath = ProjectDir() / "Assets" / "Scenes" / "Drift.olo";
            if (!fs::exists(scenePath))
            {
                problems.push_back("Drift scene not found at " + scenePath.generic_string());
                return islands;
            }
            const YAML::Node root = YAML::LoadFile(scenePath.generic_string());
            for (const auto& entity : root["Entities"])
            {
                const YAML::Node tag = entity["TagComponent"];
                const YAML::Node terrain = entity["TerrainComponent"];
                const std::string name = tag ? tag["Tag"].as<std::string>("") : std::string{};
                if (!terrain || !name.starts_with("Island - "))
                    continue;

                Island isl;
                isl.Name = name;
                isl.WorldSize = terrain["WorldSizeX"].as<f32>(0.0f);
                isl.HeightScale = terrain["HeightScale"].as<f32>(0.0f);
                if (isl.WorldSize <= 0.0f || isl.HeightScale <= 0.0f ||
                    std::abs(terrain["WorldSizeZ"].as<f32>(0.0f) - isl.WorldSize) > 1.0e-3f)
                {
                    problems.push_back(name + ": the tile is not a square with a positive height scale");
                    continue;
                }
                if (terrain["ProceduralEnabled"].as<bool>(false) || terrain["AutoMaterial"].as<bool>(false))
                    problems.push_back(name + ": still a procedural / auto-material terrain; the maps would be ignored");

                f32 translationY = 0.0f;
                if (const YAML::Node xf = entity["TransformComponent"]; xf && xf["Translation"] && xf["Translation"].size() == 3)
                    translationY = xf["Translation"][1].as<f32>(0.0f);
                isl.SeaLevel01 = -translationY / isl.HeightScale;
                // DriftWeatherDirector.lua's own formula for the coast.
                isl.LandfallRadius = (terrain["ShapingIslandFalloffRadius"].as<f32>(0.0f) + 0.5f) * 0.5f * isl.WorldSize;

                const auto mapPath = [&](const char* key)
                { return ProjectDir() / terrain[key].as<std::string>("<unset>"); };

                const fs::path heightFile = mapPath("HeightmapPath");
                if (!TerrainData::DecodeHeightmapFile(heightFile.string(), isl.Heights, isl.Resolution))
                {
                    problems.push_back(name + ": height map did not decode: " + heightFile.generic_string());
                    continue;
                }
                bool mapsOk = true;
                const std::array<std::pair<const char*, std::vector<u8>*>, 3> rgba = { {
                    { "SplatmapPath0", &isl.Splat[0] },
                    { "SplatmapPath1", &isl.Splat[1] },
                    { "MacroMapPath", &isl.Macro },
                } };
                for (const auto& [key, pixels] : rgba)
                {
                    u32 size = 0;
                    if (!LoadRGBA(mapPath(key), *pixels, size) || size != isl.Resolution)
                    {
                        problems.push_back(name + ": " + key + " is missing or is not the height map's " +
                                           std::to_string(isl.Resolution) + " px square: " + mapPath(key).generic_string());
                        mapsOk = false;
                    }
                }
                if (!mapsOk)
                    continue;

                for (const auto& layer : terrain["Layers"])
                {
                    isl.LayerNames.push_back(layer["Name"].as<std::string>("<unnamed>"));
                    glm::vec3 c{ 0.5f };
                    if (const YAML::Node bc = layer["BaseColor"]; bc && bc.size() == 3)
                        c = { bc[0].as<f32>(0.5f), bc[1].as<f32>(0.5f), bc[2].as<f32>(0.5f) };
                    isl.LayerColours.push_back(c);
                    for (const char* key : { "AlbedoPath", "NormalPath", "ARMPath" })
                    {
                        if (!fs::exists(ProjectDir() / layer[key].as<std::string>("<unset>")))
                            problems.push_back(name + ": layer '" + isl.LayerNames.back() + "' " + key + " names no file");
                    }
                }
                if (isl.LayerColours.size() != MAX_TERRAIN_LAYERS)
                    problems.push_back(name + ": expected the eight shared library layers");

                islands.push_back(std::move(isl));
            }
            if (islands.size() != kIslandCount)
                problems.push_back("expected " + std::to_string(kIslandCount) + " islands, loaded " + std::to_string(islands.size()));
            return islands;
        }

        [[nodiscard]] std::string Join(const std::vector<std::string>& lines)
        {
            std::ostringstream oss;
            for (const auto& line : lines)
                oss << "\n    " << line;
            return oss.str();
        }

        // The fixture decodes six 1024 px islands once for the whole suite.
        class DriftIslandSplatCoverage : public ::testing::Test
        {
          protected:
            static void SetUpTestSuite()
            {
                s_Islands = LoadIslands(s_Problems);
            }
            static void TearDownTestSuite()
            {
                s_Islands.clear();
                s_Problems.clear();
            }
            void SetUp() override
            {
                ASSERT_TRUE(s_Problems.empty()) << "Drift's island maps did not load:" << Join(s_Problems);
            }

            static inline std::vector<Island> s_Islands;
            static inline std::vector<std::string> s_Problems;
        };
    } // namespace

    // docs/agent-rules/terrain-tile-meets-ocean.md: a tile edge that is not the
    // floor is a wall standing in the sea. The generator drives every border
    // texel to exactly 0; the summit must fit under 1 or the map clipped it flat.
    TEST_F(DriftIslandSplatCoverage, TheTileBorderIsTheSeabedFloorAndTheSummitFits)
    {
        for (const Island& isl : s_Islands)
        {
            const u32 n = isl.Resolution;
            f32 borderMax = 0.0f;
            for (u32 i = 0; i < n; ++i)
            {
                borderMax = std::max({ borderMax, isl.Heights[i], isl.Heights[(n - 1) * n + i], isl.Heights[i * n],
                                       isl.Heights[i * n + (n - 1)] });
            }
            EXPECT_LE(borderMax, 0.0f) << isl.Name << ": a border texel stands " << borderMax * isl.HeightScale
                                       << " m above the tile floor";

            const f32 summit = *std::ranges::max_element(isl.Heights);
            EXPECT_LT(summit, 1.0f) << isl.Name << ": the summit reaches the top of the height range, so it was clipped";
            EXPECT_GT((summit - isl.SeaLevel01) * isl.HeightScale, 3.0f) << isl.Name << " barely clears the sea";
        }
    }

    // The review measured the old coasts at 45-60 degrees at the waterline (a
    // radial mask multiplying the whole height range down); a beach is 5-10.
    // The median, because rock islands keep cliffs along part of their shore.
    TEST_F(DriftIslandSplatCoverage, TheCoastIsAShoreNotARamp)
    {
        constexpr f32 kMaxMedianSlopeDeg = 15.0f;
        for (const Island& isl : s_Islands)
        {
            const u32 n = isl.Resolution;
            const f32 inv = 1.0f / static_cast<f32>(n - 1);
            std::vector<f32> slopes;
            // Every third texel: 100k samples of the shore band is plenty.
            for (u32 z = 1; z + 1 < n; z += 3)
            {
                for (u32 x = 1; x + 1 < n; x += 3)
                {
                    const f32 above = isl.MetresAboveSea(static_cast<sizet>(z) * n + x);
                    if (above <= 0.0f || above >= 1.5f)
                        continue;
                    slopes.push_back(TerrainData::SampleSlopeDegrees(isl.Heights, n, static_cast<f32>(x) * inv,
                                                                     static_cast<f32>(z) * inv, isl.WorldSize,
                                                                     isl.WorldSize, isl.HeightScale));
                }
            }
            ASSERT_GT(slopes.size(), 100u) << isl.Name << " has no shore band";
            const auto mid = slopes.begin() + static_cast<std::ptrdiff_t>(slopes.size() / 2);
            std::ranges::nth_element(slopes, mid);
            EXPECT_LE(*mid, kMaxMedianSlopeDeg)
                << isl.Name << ": the median slope within 1.5 m above the waterline is " << *mid << " degrees";
        }
    }

    // DriftWeatherDirector.lua arms a leg 45 m outside
    //   coast = (IslandFalloffRadius + 0.5) * 0.5 * WorldSizeX
    // from the tile centre. The scene stamper writes the radius from the map's
    // farthest land, so a coast that moved without a re-stamp shows here: land
    // outside the ring, or a ring far outside the land.
    TEST_F(DriftIslandSplatCoverage, TheLandfallRingHoldsTheIsland)
    {
        for (const Island& isl : s_Islands)
        {
            const u32 n = isl.Resolution;
            const f32 inv = 1.0f / static_cast<f32>(n - 1);
            u32 land = 0;
            u32 outside = 0;
            f32 farthest = 0.0f;
            for (u32 z = 0; z < n; ++z)
            {
                for (u32 x = 0; x < n; ++x)
                {
                    if (isl.MetresAboveSea(static_cast<sizet>(z) * n + x) <= 0.0f)
                        continue;
                    const f32 dx = (static_cast<f32>(x) * inv - 0.5f) * isl.WorldSize;
                    const f32 dz = (static_cast<f32>(z) * inv - 0.5f) * isl.WorldSize;
                    const f32 d = std::sqrt(dx * dx + dz * dz);
                    ++land;
                    outside += (d > isl.LandfallRadius) ? 1u : 0u;
                    farthest = std::max(farthest, d);
                }
            }
            ASSERT_GT(land, 0u) << isl.Name << " is entirely under water";
            // The stamper takes the 99.5th percentile, so an outlying skerry does
            // not push the whole ring out.
            EXPECT_LE(static_cast<f32>(outside) / static_cast<f32>(land), 0.01f)
                << isl.Name << ": " << outside << " of " << land << " land texels lie outside the "
                << isl.LandfallRadius << " m landfall ring (farthest land " << farthest << " m)";
            EXPECT_GE(farthest, 0.85f * isl.LandfallRadius)
                << isl.Name << ": the landfall ring (" << isl.LandfallRadius << " m) is far outside the land ("
                << farthest << " m); re-stamp the scene";
            EXPECT_LE(isl.LandfallRadius, 0.5f * isl.WorldSize) << isl.Name << ": the ring leaves the tile";
        }
    }

    // A texel no layer claims is not empty: the shader's height blend then gives
    // every layer a share and the ground is the average of all eight textures.
    // And #942's state, one layer at 77-93% with the others at nothing, must
    // not come back: above the water at least two materials show.
    TEST_F(DriftIslandSplatCoverage, EveryTexelIsClaimedAndMoreThanOneMaterialShows)
    {
        constexpr f32 kVisibleShare = 4.0f;
        for (const Island& isl : s_Islands)
        {
            const sizet texels = static_cast<sizet>(isl.Resolution) * isl.Resolution;
            std::array<u32, MAX_TERRAIN_LAYERS> dominant{};
            u32 land = 0;
            u32 unclaimed = 0;
            for (sizet t = 0; t < texels; ++t)
            {
                const auto w = isl.Weights(t);
                f32 sum = 0.0f;
                u32 best = 0;
                for (u32 i = 0; i < MAX_TERRAIN_LAYERS; ++i)
                {
                    sum += w[i];
                    if (w[i] > w[best])
                        best = i;
                }
                // 8 channels rounded separately: a whole is 255 +- 4.
                unclaimed += (std::abs(sum - 255.0f) > 4.0f) ? 1u : 0u;
                if (isl.MetresAboveSea(t) > 0.0f)
                {
                    ++land;
                    ++dominant[best];
                }
            }
            EXPECT_EQ(unclaimed, 0u) << isl.Name << ": texels whose eight layer weights do not sum to one";

            u32 visible = 0;
            std::ostringstream shares;
            for (u32 i = 0; i < MAX_TERRAIN_LAYERS; ++i)
            {
                const f32 share = 100.0f * static_cast<f32>(dominant[i]) / static_cast<f32>(std::max(land, 1u));
                visible += (share >= kVisibleShare) ? 1u : 0u;
                shares << "\n      " << isl.LayerNames[i] << ": " << share << "%";
            }
            EXPECT_GE(visible, 2u) << isl.Name << ": one material covers the island above the water:" << shares.str();
        }
    }

    // The review's palette finding: layer colours had been picked as display
    // colours and consumed as linear albedo, so sand was brighter than snow and
    // grass was mint. And two pairs of islands shared one palette outright.
    // The ground's colour is the layer blend times the macro map's tint, so
    // that product, averaged over the land, is what this bounds.
    TEST_F(DriftIslandSplatCoverage, GroundAlbedoIsPhysicalAndNoTwoIslandsShareAPalette)
    {
        // Linear luminance: wet basalt is about 0.05, grass 0.1, dry coral sand
        // 0.45-0.55, fresh snow 0.9.
        constexpr f32 kMinLuminance = 0.03f;
        constexpr f32 kMaxLuminance = 0.55f;
        constexpr f32 kMinSeparation = 0.03f;

        std::vector<glm::vec3> means;
        for (const Island& isl : s_Islands)
        {
            const sizet texels = static_cast<sizet>(isl.Resolution) * isl.Resolution;
            glm::dvec3 sum{ 0.0 };
            u32 land = 0;
            for (sizet t = 0; t < texels; t += 3)
            {
                if (isl.MetresAboveSea(t) <= 0.0f)
                    continue;
                const auto w = isl.Weights(t);
                glm::vec3 blend{ 0.0f };
                f32 total = 0.0f;
                for (u32 i = 0; i < MAX_TERRAIN_LAYERS; ++i)
                {
                    blend += isl.LayerColours[i] * w[i];
                    total += w[i];
                }
                blend /= std::max(total, 1.0f);
                // The macro map's RGB is a multiplier with 128 as neutral.
                const glm::vec3 tint = glm::vec3(isl.Macro[t * 4], isl.Macro[t * 4 + 1], isl.Macro[t * 4 + 2]) / 128.0f;
                sum += glm::dvec3(blend * tint);
                ++land;
            }
            ASSERT_GT(land, 0u);
            const glm::vec3 mean = glm::vec3(sum / static_cast<f64>(land));
            const f32 luminance = glm::dot(mean, glm::vec3(0.2126f, 0.7152f, 0.0722f));
            EXPECT_GE(luminance, kMinLuminance) << isl.Name << " is darker than wet basalt";
            EXPECT_LE(luminance, kMaxLuminance) << isl.Name << " is brighter than dry coral sand; a display colour used as albedo?";
            means.push_back(mean);
        }
        for (sizet i = 0; i < means.size(); ++i)
        {
            for (sizet j = i + 1; j < means.size(); ++j)
            {
                EXPECT_GE(glm::length(means[i] - means[j]), kMinSeparation)
                    << s_Islands[i].Name << " and " << s_Islands[j].Name << " have the same mean ground colour";
            }
        }
    }
} // namespace OloEngine::Tests
