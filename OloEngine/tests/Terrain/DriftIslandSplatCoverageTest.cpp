// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainLayer.h"

#include <glm/glm.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

// =============================================================================
// Drift auto-material coverage (issue #942).
//
// Every island in Drift.olo carries AutoMaterial + four LayerRules, and every
// island rendered as one flat colour anyway. Nothing in the pipeline was broken:
// the gate fired, the rules survived deserialization, the splatmap generated and
// reached the shader, and the shader blended it faithfully. The rules themselves
// were mis-anchored, in two ways that are invisible to any test that only asks
// whether the weights VARY — they varied fine:
//
//   * The slope-gated rock rule opened at 48 deg while #880's shoreline mask
//     ramps the full height to zero across the outer flank of every island, a
//     ~50 deg slope by construction. Rock therefore claimed the flank — 77% of
//     Ridgeback, 86% of Sisters, 93% of Stacks — and normalization drove the
//     altitude-banded layers to nothing.
//   * The altitude bands were written as fractions of [0, 1], but the island
//     mask and the height exponent leave the field short of 1.0 (Mesa peaks at
//     0.800). A summit band starting at 0.844 is above the summit, so layer 3
//     drew on 0.0-3.5% of every island.
//
// So the contract worth pinning is COVERAGE, not variation: on each island's
// above-water surface, every authored layer must claim a real share and none may
// swallow the rest. That is the property a reader checks in a screenshot, and it
// is the one that was false.
//
// Coverage is necessary and not sufficient. A layer can hold 20% of the surface
// and still be invisible if its colour matches its neighbour, which is what the
// third test here guards — every island shipped a pair separated only by
// brightness, and under this scene's haze and tone curve those read as one
// material no matter what the splatmap says.
//
// Pure CPU — GenerateHeightField and the TerrainData::Sample* statics need no GL
// context (the TerrainData MEMBERS would: SetHeights uploads to the GPU). Reads
// the shipped scene directly, so retuning the scene and this test cannot drift.
// Classification: unit.
// =============================================================================

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    namespace
    {
        // Every Drift island sits at y = -24 with the sea at y = 0, so the
        // fraction of the height range that is under water is -translationY /
        // heightScale. Read from the scene rather than hardcoded: an island that
        // gets sunk deeper must re-band, and that is exactly what this measures.
        [[nodiscard]] f32 SeaLevel01(f32 translationY, f32 heightScale)
        {
            if (heightScale <= 0.0f)
                return 0.0f;
            return std::clamp(-translationY / heightScale, 0.0f, 1.0f);
        }

        struct Island
        {
            std::string Name;
            TerrainGenerator::HeightParams Params;
            std::vector<TerrainLayerRule> Rules;
            std::vector<std::string> LayerNames;
            std::vector<glm::vec3> LayerColours;
            f32 WorldSizeX = 0.0f;
            f32 WorldSizeZ = 0.0f;
            f32 HeightScale = 0.0f;
            f32 SeaLevel = 0.0f;
        };

        [[nodiscard]] fs::path DriftScenePath()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" / "Assets" / "Scenes" / "Drift.olo";
        }

        // Pull every auto-material terrain out of the scene YAML. Deliberately
        // NOT via SceneSerializer: that eagerly builds GPU resources for the
        // whole scene (meshes, textures, fonts), which would make this a
        // GL-gated test for the sake of four numbers per component.
        [[nodiscard]] std::vector<Island> LoadAutoMaterialIslands(const fs::path& scenePath)
        {
            std::vector<Island> islands;
            const YAML::Node root = YAML::LoadFile(scenePath.generic_string());
            const YAML::Node entities = root["Entities"];
            if (!entities || !entities.IsSequence())
                return islands;

            for (const auto& entity : entities)
            {
                const YAML::Node terrain = entity["TerrainComponent"];
                if (!terrain || !terrain["AutoMaterial"].as<bool>(false))
                    continue;
                if (!terrain["ProceduralEnabled"].as<bool>(false))
                    continue;
                const YAML::Node rulesNode = terrain["LayerRules"];
                if (!rulesNode || !rulesNode.IsSequence() || rulesNode.size() == 0)
                    continue;

                Island isl;
                const YAML::Node tag = entity["TagComponent"];
                isl.Name = tag ? tag["Tag"].as<std::string>("<unnamed>") : "<unnamed>";

                auto& p = isl.Params;
                p.Resolution = terrain["ProceduralResolution"].as<u32>(p.Resolution);
                p.Seed = terrain["ProceduralSeed"].as<i32>(p.Seed);
                p.Octaves = terrain["ProceduralOctaves"].as<u32>(p.Octaves);
                p.Frequency = terrain["ProceduralFrequency"].as<f32>(p.Frequency);
                p.Lacunarity = terrain["ProceduralLacunarity"].as<f32>(p.Lacunarity);
                p.Persistence = terrain["ProceduralPersistence"].as<f32>(p.Persistence);
                p.ErosionIterations = terrain["ProceduralErosionIterations"].as<i32>(p.ErosionIterations);
                p.Shaping.RidgeBlend = terrain["ShapingRidgeBlend"].as<f32>(p.Shaping.RidgeBlend);
                p.Shaping.WarpStrength = terrain["ShapingWarpStrength"].as<f32>(p.Shaping.WarpStrength);
                p.Shaping.WarpFrequency = terrain["ShapingWarpFrequency"].as<f32>(p.Shaping.WarpFrequency);
                p.Shaping.TerraceSteps = terrain["ShapingTerraceSteps"].as<u32>(p.Shaping.TerraceSteps);
                p.Shaping.TerraceSharpness = terrain["ShapingTerraceSharpness"].as<f32>(p.Shaping.TerraceSharpness);
                p.Shaping.HeightExponent = terrain["ShapingHeightExponent"].as<f32>(p.Shaping.HeightExponent);
                p.Shaping.IslandFalloff = terrain["ShapingIslandFalloff"].as<f32>(p.Shaping.IslandFalloff);
                p.Shaping.IslandFalloffRadius =
                    terrain["ShapingIslandFalloffRadius"].as<f32>(p.Shaping.IslandFalloffRadius);

                isl.WorldSizeX = terrain["WorldSizeX"].as<f32>(0.0f);
                isl.WorldSizeZ = terrain["WorldSizeZ"].as<f32>(0.0f);
                isl.HeightScale = terrain["HeightScale"].as<f32>(0.0f);

                f32 translationY = 0.0f;
                if (const YAML::Node xf = entity["TransformComponent"]; xf && xf["Translation"] &&
                                                                        xf["Translation"].size() == 3)
                {
                    translationY = xf["Translation"][1].as<f32>(0.0f);
                }
                isl.SeaLevel = SeaLevel01(translationY, isl.HeightScale);

                for (const auto& r : rulesNode)
                {
                    TerrainLayerRule rule;
                    rule.LayerIndex = r["LayerIndex"].as<u32>(rule.LayerIndex);
                    rule.MinHeight = r["MinHeight"].as<f32>(rule.MinHeight);
                    rule.MaxHeight = r["MaxHeight"].as<f32>(rule.MaxHeight);
                    rule.HeightBlend = r["HeightBlend"].as<f32>(rule.HeightBlend);
                    rule.MinSlopeDeg = r["MinSlopeDeg"].as<f32>(rule.MinSlopeDeg);
                    rule.MaxSlopeDeg = r["MaxSlopeDeg"].as<f32>(rule.MaxSlopeDeg);
                    rule.SlopeBlend = r["SlopeBlend"].as<f32>(rule.SlopeBlend);
                    rule.Strength = r["Strength"].as<f32>(rule.Strength);
                    isl.Rules.push_back(rule);
                }

                if (const YAML::Node layers = terrain["Layers"]; layers && layers.IsSequence())
                {
                    for (const auto& l : layers)
                    {
                        isl.LayerNames.push_back(l["Name"].as<std::string>("<unnamed>"));
                        glm::vec3 c{ 0.5f, 0.5f, 0.5f };
                        if (const YAML::Node bc = l["BaseColor"]; bc && bc.size() == 3)
                            c = { bc[0].as<f32>(0.5f), bc[1].as<f32>(0.5f), bc[2].as<f32>(0.5f) };
                        isl.LayerColours.push_back(c);
                    }
                }

                islands.push_back(std::move(isl));
            }
            return islands;
        }

        struct Coverage
        {
            std::array<f32, MAX_TERRAIN_LAYERS> DominantPercent{};
            u32 AboveWaterTexels = 0;
            f32 MaxHeight01 = 0.0f;
        };

        // Share of the ABOVE-WATER surface on which each layer is the winning
        // weight. Above-water is the only part anybody sees; the sea floor out to
        // the tile edge is the majority of the texels and is all one band, so
        // including it would let a flat island pass.
        [[nodiscard]] Coverage MeasureCoverage(const Island& isl)
        {
            std::vector<f32> heights;
            TerrainGenerator::GenerateHeightField(heights, isl.Params);

            const u32 res = std::max(isl.Params.Resolution, 2u);
            const f32 invRes = 1.0f / static_cast<f32>(res - 1);

            Coverage cov;
            std::array<u32, MAX_TERRAIN_LAYERS> hits{};
            hits.fill(0);
            std::array<f32, MAX_TERRAIN_LAYERS> weights{};

            for (u32 z = 0; z < res; ++z)
            {
                for (u32 x = 0; x < res; ++x)
                {
                    const f32 nx = static_cast<f32>(x) * invRes;
                    const f32 nz = static_cast<f32>(z) * invRes;
                    const f32 h01 = TerrainData::SampleHeight(heights, res, nx, nz);
                    cov.MaxHeight01 = std::max(cov.MaxHeight01, h01);
                    if (h01 < isl.SeaLevel)
                        continue;

                    const f32 slopeDeg = TerrainData::SampleSlopeDegrees(
                        heights, res, nx, nz, isl.WorldSizeX, isl.WorldSizeZ, isl.HeightScale);
                    TerrainGenerator::EvaluateLayerWeights(h01, slopeDeg, isl.Rules, weights);

                    u32 best = 0;
                    for (u32 i = 1; i < MAX_TERRAIN_LAYERS; ++i)
                        if (weights[i] > weights[best])
                            best = i;
                    ++hits[best];
                    ++cov.AboveWaterTexels;
                }
            }

            const f32 inv = (cov.AboveWaterTexels > 0)
                                ? 100.0f / static_cast<f32>(cov.AboveWaterTexels)
                                : 0.0f;
            for (u32 i = 0; i < MAX_TERRAIN_LAYERS; ++i)
                cov.DominantPercent[i] = static_cast<f32>(hits[i]) * inv;
            return cov;
        }

        [[nodiscard]] std::string DescribeCoverage(const Island& isl, const Coverage& cov)
        {
            std::ostringstream oss;
            oss << isl.Name << " — " << cov.AboveWaterTexels << " above-water texels, max height "
                << cov.MaxHeight01 << " (waterline " << isl.SeaLevel << ")\n";
            for (sizet i = 0; i < isl.LayerNames.size() && i < MAX_TERRAIN_LAYERS; ++i)
                oss << "      layer " << i << " '" << isl.LayerNames[i] << "': "
                    << cov.DominantPercent[i] << "% of the surface\n";
            return oss.str();
        }
    } // namespace

    // Every authored layer has to actually appear, and no layer may swallow the
    // island. The bounds are deliberately loose — the shipped scene lands at
    // roughly 17/36/27/20 and the worst island (Mesa, whose terracing quantizes
    // its height distribution) at 28/31/28/13, so this leaves real retuning room
    // while still failing the state that prompted the issue (a layer at 77-93%
    // with two layers at ~0%).
    TEST(DriftIslandSplatCoverage, EveryAuthoredLayerClaimsPartOfEachIsland)
    {
        const fs::path scene = DriftScenePath();
        ASSERT_TRUE(fs::exists(scene)) << "Drift scene not found at " << scene.generic_string();

        const std::vector<Island> islands = LoadAutoMaterialIslands(scene);
        ASSERT_GE(islands.size(), 1u)
            << "no auto-material procedural terrain found in Drift.olo — scene layout changed?";

        constexpr f32 kMinShare = 6.0f;
        constexpr f32 kMaxShare = 55.0f;

        for (const Island& isl : islands)
        {
            const Coverage cov = MeasureCoverage(isl);
            ASSERT_GT(cov.AboveWaterTexels, 0u) << isl.Name << " is entirely under water";

            // Only layers an authored rule actually targets are required to show.
            std::array<bool, MAX_TERRAIN_LAYERS> targeted{};
            targeted.fill(false);
            for (const TerrainLayerRule& r : isl.Rules)
                if (r.LayerIndex < MAX_TERRAIN_LAYERS)
                    targeted[r.LayerIndex] = true;

            for (u32 i = 0; i < MAX_TERRAIN_LAYERS; ++i)
            {
                if (!targeted[i])
                    continue;
                EXPECT_GE(cov.DominantPercent[i], kMinShare)
                    << "layer " << i << " is effectively invisible on this island.\n"
                    << DescribeCoverage(isl, cov)
                    << "    A rule whose band sits outside the terrain's reachable height range, or "
                       "whose slope band is shadowed by a more permissive rule, contributes nothing "
                       "once EvaluateLayerWeights normalizes. See issue #942.";
                EXPECT_LE(cov.DominantPercent[i], kMaxShare)
                    << "layer " << i << " has swallowed this island, leaving no visible banding.\n"
                    << DescribeCoverage(isl, cov)
                    << "    Usually a slope-gated rule whose MinSlopeDeg opens below the island's "
                       "median slope — the #880 shoreline mask alone contributes ~50 deg across the "
                       "whole outer flank. See issue #942.";
            }
        }
    }

    // The trap behind the second half of #942, pinned on its own so it reads as a
    // fact about the generator rather than as a scene-tuning accident: with an
    // island mask (and/or a height exponent > 1) the shaped field no longer
    // reaches 1.0, so the top of the [0,1] range is unreachable and a band
    // anchored there never fires. Nothing warns about this — the rule is valid,
    // it simply never matches a texel.
    TEST(DriftIslandSplatCoverage, IslandFalloffLeavesTheSummitShortOfFullHeight)
    {
        const fs::path scene = DriftScenePath();
        ASSERT_TRUE(fs::exists(scene)) << "Drift scene not found at " << scene.generic_string();

        const std::vector<Island> islands = LoadAutoMaterialIslands(scene);
        ASSERT_GE(islands.size(), 1u);

        bool sawShortfall = false;
        for (const Island& isl : islands)
        {
            std::vector<f32> heights;
            TerrainGenerator::GenerateHeightField(heights, isl.Params);
            const f32 maxH = *std::ranges::max_element(heights);
            ASSERT_TRUE(std::isfinite(maxH));
            EXPECT_LE(maxH, 1.0f) << isl.Name << " exceeds the normalized [0,1] contract";
            if (maxH < 0.99f)
                sawShortfall = true;

            // Whatever the shortfall is, no authored band may start above it.
            for (const TerrainLayerRule& r : isl.Rules)
            {
                EXPECT_LT(r.MinHeight, maxH)
                    << isl.Name << ": rule for layer " << r.LayerIndex << " starts at MinHeight "
                    << r.MinHeight << ", but the shaped field never exceeds " << maxH
                    << " — the band is above the summit and can never fire (issue #942).";
            }
        }

        EXPECT_TRUE(sawShortfall)
            << "no Drift island falls short of full height any more — if the island mask stopped "
               "compressing the field, the anchoring rationale in the scene comments is stale.";
    }

    // Coverage says each layer occupies a real share of the surface. It says
    // nothing about whether a viewer can TELL them apart, and on the first pass
    // of #942 that was the remaining half of "the islands look flat": every
    // island carried a pair of materials separated only by brightness — Rock vs
    // Heath 0.135 apart in RGB, Palm scrub vs Green cap 0.075, Shingle vs Lichen
    // 0.134, Dry scrub vs Sandstone 0.154. On Ridgeback that pair was 47% of the
    // surface, so correcting the splat still left the uplands reading as one mass.
    //
    // The threshold is on plain RGB distance rather than on hue, because a large
    // lightness gap really is distinguishable: Stacks pairs Shingle with Guano,
    // both neutral greys, 0.61 apart and in no danger of merging. What fails is a
    // pair that is close on BOTH axes, and distance alone catches that.
    TEST(DriftIslandSplatCoverage, EveryIslandsLayerColoursAreDistinguishable)
    {
        const fs::path scene = DriftScenePath();
        ASSERT_TRUE(fs::exists(scene)) << "Drift scene not found at " << scene.generic_string();

        const std::vector<Island> islands = LoadAutoMaterialIslands(scene);
        ASSERT_GE(islands.size(), 1u);

        // Shipped palette's worst pair is 0.223 (Mesa's Dry scrub vs Sandstone),
        // so this leaves room to re-tune a colour without tripping the guard while
        // still failing every pair the issue found.
        constexpr f32 kMinSeparation = 0.18f;

        for (const Island& isl : islands)
        {
            ASSERT_GE(isl.LayerColours.size(), 2u) << isl.Name << " has fewer than two layers";
            for (sizet i = 0; i < isl.LayerColours.size(); ++i)
            {
                for (sizet j = i + 1; j < isl.LayerColours.size(); ++j)
                {
                    const f32 d = glm::length(isl.LayerColours[i] - isl.LayerColours[j]);
                    EXPECT_GE(d, kMinSeparation)
                        << isl.Name << ": layers '" << isl.LayerNames[i] << "' and '"
                        << isl.LayerNames[j] << "' are only " << d
                        << " apart in RGB — they will read as one material under the scene's "
                           "haze and tone curve, whatever the splatmap assigns. Separate them by "
                           "hue rather than by another step of brightness (issue #942).";
                }
            }
        }
    }
} // namespace OloEngine::Tests
