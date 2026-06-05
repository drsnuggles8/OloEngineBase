#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainGenerator.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Particle/SimplexNoise.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace OloEngine
{
    // Upper bound on heightmap / splatmap edge length — guards against a
    // corrupt resolution from a save file or scene triggering a huge allocation.
    static constexpr u32 kMaxTerrainResolution = 4096u;

    namespace
    {
        // Clamped Hermite smoothstep (matches glm::smoothstep but guards the
        // degenerate edge0 == edge1 case so a zero-width band is a hard cutoff).
        [[nodiscard]] f32 SmoothStep(f32 edge0, f32 edge1, f32 x)
        {
            if (edge1 <= edge0)
                return (x < edge0) ? 0.0f : 1.0f;
            f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Membership in [lo, hi] with a soft `blend`-wide falloff on each side.
        [[nodiscard]] f32 SmoothBand(f32 x, f32 lo, f32 hi, f32 blend)
        {
            if (hi < lo)
                std::swap(lo, hi);
            if (blend <= 1e-6f)
                return (x >= lo && x <= hi) ? 1.0f : 0.0f;
            f32 rising = SmoothStep(lo - blend, lo, x);
            f32 falling = 1.0f - SmoothStep(hi, hi + blend, x);
            return std::clamp(rising * falling, 0.0f, 1.0f);
        }
    } // namespace

    auto TerrainHeightShaping::operator==(const TerrainHeightShaping& o) const -> bool
    {
        return Math::BitwiseEqual(RidgeBlend, o.RidgeBlend) && Math::BitwiseEqual(WarpStrength, o.WarpStrength) &&
               Math::BitwiseEqual(WarpFrequency, o.WarpFrequency) && TerraceSteps == o.TerraceSteps &&
               Math::BitwiseEqual(TerraceSharpness, o.TerraceSharpness) && Math::BitwiseEqual(HeightExponent, o.HeightExponent);
    }

    auto TerrainLayerRule::operator==(const TerrainLayerRule& o) const -> bool
    {
        return LayerIndex == o.LayerIndex && Math::BitwiseEqual(MinHeight, o.MinHeight) && Math::BitwiseEqual(MaxHeight, o.MaxHeight) &&
               Math::BitwiseEqual(HeightBlend, o.HeightBlend) && Math::BitwiseEqual(MinSlopeDeg, o.MinSlopeDeg) &&
               Math::BitwiseEqual(MaxSlopeDeg, o.MaxSlopeDeg) && Math::BitwiseEqual(SlopeBlend, o.SlopeBlend) &&
               Math::BitwiseEqual(Strength, o.Strength);
    }

    f32 TerrainGenerator::Terrace(f32 h, u32 steps, f32 sharpness)
    {
        if (steps == 0)
            return h;

        const f32 s = static_cast<f32>(steps);
        const f32 scaled = std::clamp(h, 0.0f, 1.0f) * s;
        const f32 base = std::floor(scaled);
        const f32 frac = scaled - base; // [0, 1) position within the step

        // sharpness → 1 squeezes the transition into an ever-narrower window
        // around the step centre, flattening the plateaus.
        const f32 k = std::clamp(sharpness, 0.0f, 0.999f);
        f32 t = (frac - 0.5f) / (1.0f - k) + 0.5f;
        t = std::clamp(t, 0.0f, 1.0f);
        const f32 stepped = t * t * (3.0f - 2.0f * t);
        return std::clamp((base + stepped) / s, 0.0f, 1.0f);
    }

    void TerrainGenerator::GenerateHeightField(std::vector<f32>& outHeights, const HeightParams& params)
    {
        OLO_PROFILE_FUNCTION();

        // Cap the resolution so a corrupt/huge value (from a save file or scene)
        // can't trigger a multi-GB allocation.
        const u32 resolution = std::clamp(params.Resolution, 2u, kMaxTerrainResolution);
        const sizet totalPixels = static_cast<sizet>(resolution) * resolution;
        outHeights.resize(totalPixels);

        const TerrainHeightShaping& sh = params.Shaping;
        // Derive bounded noise offsets from the seed. A naive `seed * k` offset
        // explodes for large seeds (the editor's "Randomize Seed" picks any i32):
        // adding the small per-sample coordinate to a ~1e8 offset loses all f32
        // precision, so every sample collapses to one simplex lattice cell and the
        // whole field goes flat. Hash the seed into a small [0, 256) range instead.
        const auto seedOffsetFor = [seed = params.Seed](u32 salt) -> f32
        {
            u32 h = static_cast<u32>(seed) * 374761393u + salt * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return static_cast<f32>(h % 256000u) * (1.0f / 1000.0f); // [0, 256)
        };
        const f32 seedOffset = seedOffsetFor(0u);
        // Distinct offsets so the two domain-warp axes decorrelate from each other
        // and from the main field.
        const f32 warpOffsetX = seedOffsetFor(1u);
        const f32 warpOffsetZ = seedOffsetFor(2u);
        const f32 ridgeBlend = std::clamp(sh.RidgeBlend, 0.0f, 1.0f);

        f32 minH = std::numeric_limits<f32>::max();
        f32 maxH = std::numeric_limits<f32>::lowest();

        for (u32 z = 0; z < resolution; ++z)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                f32 nx = static_cast<f32>(x) / static_cast<f32>(resolution);
                f32 nz = static_cast<f32>(z) / static_cast<f32>(resolution);

                // Domain warp: offset the sample position by a low-frequency noise
                // field so ridges meander instead of following the lattice.
                if (sh.WarpStrength > 1e-6f)
                {
                    f32 wx = SimplexNoise3D(nx * sh.WarpFrequency + warpOffsetX, 0.0f, nz * sh.WarpFrequency + warpOffsetX);
                    f32 wz = SimplexNoise3D(nx * sh.WarpFrequency + warpOffsetZ, 0.0f, nz * sh.WarpFrequency + warpOffsetZ);
                    nx += wx * sh.WarpStrength;
                    nz += wz * sh.WarpStrength;
                }

                f32 fbm = 0.0f;
                f32 ridged = 0.0f;
                f32 freq = params.Frequency;
                f32 amp = 1.0f;
                for (u32 o = 0; o < params.Octaves; ++o)
                {
                    const f32 n = SimplexNoise3D(nx * freq + seedOffset, 0.0f, nz * freq + seedOffset);
                    fbm += n * amp;
                    // Ridged multifractal octave: fold the noise about 0 and square
                    // it so the zero-crossings become sharp ridge lines.
                    const f32 r = 1.0f - std::fabs(n);
                    ridged += r * r * amp;
                    freq *= params.Lacunarity;
                    amp *= params.Persistence;
                }

                const f32 value = glm::mix(fbm, ridged, ridgeBlend);
                const sizet idx = static_cast<sizet>(z) * resolution + x;
                outHeights[idx] = value;
                minH = std::min(minH, value);
                maxH = std::max(maxH, value);
            }
        }

        // Normalize to [0, 1].
        if (const f32 range = maxH - minH; range > 1e-6f)
        {
            const f32 invRange = 1.0f / range;
            for (f32& h : outHeights)
                h = (h - minH) * invRange;
        }
        else
        {
            std::fill(outHeights.begin(), outHeights.end(), 0.0f);
        }

        // Post-normalization shaping (operates in [0, 1]).
        const bool applyExponent = !Math::BitwiseEqual(sh.HeightExponent, 1.0f) && sh.HeightExponent > 1e-4f;
        const bool applyTerrace = sh.TerraceSteps > 0;
        if (applyExponent || applyTerrace)
        {
            for (f32& h : outHeights)
            {
                if (applyExponent)
                    h = std::pow(std::clamp(h, 0.0f, 1.0f), sh.HeightExponent);
                if (applyTerrace)
                    h = Terrace(h, sh.TerraceSteps, sh.TerraceSharpness);
            }
        }
    }

    void TerrainGenerator::GenerateHeightmap(TerrainData& data, const HeightParams& params)
    {
        OLO_PROFILE_FUNCTION();

        const u32 resolution = std::max(params.Resolution, 2u);
        std::vector<f32> heights;
        GenerateHeightField(heights, params);
        const auto [minIt, maxIt] = std::minmax_element(heights.begin(), heights.end());
        const f32 hMin = heights.empty() ? 0.0f : *minIt;
        const f32 hMax = heights.empty() ? 0.0f : *maxIt;
        data.SetHeights(resolution, std::move(heights));

        OLO_CORE_INFO("TerrainGenerator: Generated {}x{} terrain (seed={}, octaves={}, ridge={:.2f}, warp={:.2f}, terrace={}, height=[{:.2f},{:.2f}])",
                      resolution, resolution, params.Seed, params.Octaves, params.Shaping.RidgeBlend,
                      params.Shaping.WarpStrength, params.Shaping.TerraceSteps, hMin, hMax);
    }

    f32 TerrainGenerator::EvaluateRuleWeight(f32 height01, f32 slopeDeg, const TerrainLayerRule& rule)
    {
        const f32 hw = SmoothBand(height01, rule.MinHeight, rule.MaxHeight, rule.HeightBlend);
        const f32 sw = SmoothBand(slopeDeg, rule.MinSlopeDeg, rule.MaxSlopeDeg, rule.SlopeBlend);
        return hw * sw * std::max(0.0f, rule.Strength);
    }

    void TerrainGenerator::EvaluateLayerWeights(f32 height01, f32 slopeDeg,
                                                const std::vector<TerrainLayerRule>& rules,
                                                std::array<f32, MAX_TERRAIN_LAYERS>& outWeights)
    {
        outWeights.fill(0.0f);
        for (const TerrainLayerRule& rule : rules)
        {
            if (rule.LayerIndex < MAX_TERRAIN_LAYERS)
                outWeights[rule.LayerIndex] += EvaluateRuleWeight(height01, slopeDeg, rule);
        }

        f32 sum = 0.0f;
        for (const f32 w : outWeights)
            sum += w;

        if (sum > 1e-6f)
        {
            const f32 invSum = 1.0f / sum;
            for (f32& w : outWeights)
                w *= invSum;
        }
        else
        {
            // No rule claimed this texel — keep layer 0 fully on so the terrain is
            // never rendered with an all-zero splat weight (which reads as black).
            outWeights[0] = 1.0f;
        }
    }

    void TerrainGenerator::PackLayerWeights(const std::array<f32, MAX_TERRAIN_LAYERS>& weights,
                                            u8 outSplat0[4], u8 outSplat1[4])
    {
        for (u32 i = 0; i < 4; ++i)
        {
            outSplat0[i] = static_cast<u8>(std::lround(std::clamp(weights[i], 0.0f, 1.0f) * 255.0f));
            outSplat1[i] = static_cast<u8>(std::lround(std::clamp(weights[4 + i], 0.0f, 1.0f) * 255.0f));
        }
    }

    void TerrainGenerator::GenerateSplatmap(TerrainMaterial& material, const TerrainData& data,
                                            const std::vector<TerrainLayerRule>& rules, u32 splatmapResolution,
                                            f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        if (rules.empty() || data.GetResolution() == 0)
        {
            OLO_CORE_WARN("TerrainGenerator::GenerateSplatmap - no rules or empty terrain; skipping");
            return;
        }

        const u32 res = std::clamp(splatmapResolution, 2u, kMaxTerrainResolution);
        material.InitializeCPUSplatmaps(res);

        std::vector<u8>& splat0 = material.GetSplatmapData(0);
        std::vector<u8>& splat1 = material.GetSplatmapData(1);
        if (splat0.size() != static_cast<sizet>(res) * res * 4 || splat1.size() != static_cast<sizet>(res) * res * 4)
        {
            OLO_CORE_ERROR("TerrainGenerator::GenerateSplatmap - splatmap buffers not allocated");
            return;
        }

        const f32 invRes = 1.0f / static_cast<f32>(res - 1);
        std::array<f32, MAX_TERRAIN_LAYERS> weights{};
        for (u32 z = 0; z < res; ++z)
        {
            for (u32 x = 0; x < res; ++x)
            {
                const f32 nx = static_cast<f32>(x) * invRes;
                const f32 nz = static_cast<f32>(z) * invRes;

                const f32 h01 = data.GetHeightAt(nx, nz);
                const glm::vec3 normal = data.GetNormalAt(nx, nz, worldSizeX, worldSizeZ, heightScale);
                const f32 slopeDeg = glm::degrees(std::acos(std::clamp(normal.y, -1.0f, 1.0f)));

                EvaluateLayerWeights(h01, slopeDeg, rules, weights);

                const sizet idx = (static_cast<sizet>(z) * res + x) * 4;
                PackLayerWeights(weights, &splat0[idx], &splat1[idx]);
            }
        }

        material.UploadSplatmapRegion(0, 0, 0, res, res);
        material.UploadSplatmapRegion(1, 0, 0, res, res);

        OLO_CORE_INFO("TerrainGenerator: Auto-assigned splatmap ({}x{}, {} rules)", res, res, rules.size());
    }

    std::vector<TerrainLayer> TerrainGenerator::MakeDefaultLayers()
    {
        std::vector<TerrainLayer> layers;
        layers.reserve(4);

        TerrainLayer sand;
        sand.Name = "Sand";
        sand.BaseColor = { 0.76f, 0.70f, 0.50f };
        sand.Roughness = 0.95f;
        sand.TilingScale = 16.0f;
        layers.push_back(sand);

        TerrainLayer grass;
        grass.Name = "Grass";
        grass.BaseColor = { 0.28f, 0.45f, 0.17f };
        grass.Roughness = 0.9f;
        grass.TilingScale = 20.0f;
        layers.push_back(grass);

        TerrainLayer rock;
        rock.Name = "Rock";
        rock.BaseColor = { 0.42f, 0.40f, 0.38f };
        rock.Roughness = 0.8f;
        rock.TilingScale = 12.0f;
        layers.push_back(rock);

        TerrainLayer snow;
        snow.Name = "Snow";
        snow.BaseColor = { 0.92f, 0.93f, 0.96f };
        snow.Roughness = 0.6f;
        snow.TilingScale = 18.0f;
        layers.push_back(snow);

        return layers;
    }

    std::vector<TerrainLayerRule> TerrainGenerator::MakeDefaultRules()
    {
        std::vector<TerrainLayerRule> rules;
        rules.reserve(4);

        // 0: Sand — beaches / low flats.
        rules.push_back({ 0, 0.0f, 0.12f, 0.06f, 0.0f, 35.0f, 8.0f, 1.0f });
        // 1: Grass — low-to-mid gentle slopes.
        rules.push_back({ 1, 0.08f, 0.55f, 0.14f, 0.0f, 28.0f, 8.0f, 1.0f });
        // 2: Rock — any steep slope (cliffs) at any altitude.
        rules.push_back({ 2, 0.0f, 1.0f, 0.0f, 32.0f, 90.0f, 10.0f, 1.0f });
        // 3: Snow — high altitude, not too steep.
        rules.push_back({ 3, 0.62f, 1.0f, 0.14f, 0.0f, 50.0f, 10.0f, 1.0f });

        return rules;
    }
} // namespace OloEngine
