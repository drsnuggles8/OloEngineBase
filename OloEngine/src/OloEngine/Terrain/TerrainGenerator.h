#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Terrain/TerrainLayer.h"

#include <array>
#include <vector>

namespace OloEngine
{
    class TerrainData;
    class TerrainMaterial;

    // Extra height-field shaping applied on top of the base fBm. Every field
    // defaults to an identity transform, so a TerrainHeightShaping{} produces
    // exactly the same field as the legacy single-fBm GenerateProcedural path.
    struct TerrainHeightShaping
    {
        // 0 = pure fBm (rolling hills); 1 = pure ridged multifractal (sharp
        // mountain ridges and valleys). Blended per-sample before normalization.
        f32 RidgeBlend = 0.0f;

        // Domain warp: displaces sample coordinates by a second noise field to
        // break up the grid-aligned look of plain fBm (meandering ridges/erosion
        // feel). 0 = off. In normalized [0,1] coordinate units.
        f32 WarpStrength = 0.0f;
        f32 WarpFrequency = 2.0f;

        // Terracing / stepping: quantize the normalized height into N flat steps
        // (mesa / rice-paddy look). 0 = off. Sharpness in [0,1) controls how flat
        // each plateau is (0 = linear, →1 = hard steps).
        u32 TerraceSteps = 0;
        f32 TerraceSharpness = 0.6f;

        // Height redistribution exponent applied to the normalized field.
        // >1 flattens lowlands and sharpens peaks (island / deep-valley shaping);
        // <1 raises lowlands (plateau-heavy). 1 = identity.
        f32 HeightExponent = 1.0f;

        auto operator==(const TerrainHeightShaping& o) const -> bool;
    };

    // Hydraulic-erosion knobs for the deterministic CPU generation post-pass
    // (TerrainGenerator::ApplyErosion). These mirror the GPU editor brush's
    // ErosionSettings one-for-one so the physics is identical; the only
    // difference is the CPU path runs droplets sequentially (reproducible)
    // instead of one-thread-per-droplet (racy). Defaults match the editor brush.
    struct ErosionParams
    {
        // Droplets simulated per iteration. 0 → auto: resolution² (one droplet
        // per cell), the standard density. A higher value erodes more per pass.
        u32 DropletCount = 0;
        u32 MaxDropletSteps = 64;        // Max simulation steps per droplet
        f32 Inertia = 0.05f;             // Direction inertia [0,1]
        f32 SedimentCapacity = 4.0f;     // Sediment capacity multiplier
        f32 MinSedimentCapacity = 0.01f; // Minimum capacity floor
        f32 DepositSpeed = 0.3f;         // Deposit rate [0,1]
        f32 ErodeSpeed = 0.3f;           // Erosion rate [0,1]
        f32 EvaporateSpeed = 0.01f;      // Water evaporation per step [0,1]
        f32 Gravity = 4.0f;              // Gravity constant
        f32 InitialWater = 1.0f;         // Starting water volume
        f32 InitialSpeed = 1.0f;         // Starting droplet speed
        u32 ErosionRadius = 3;           // Brush radius for erosion/deposition (texels)
    };

    // One automatic material-assignment rule: a height band crossed with a slope
    // band selects a material layer. Rules are evaluated per splatmap texel and
    // their weights summed per layer, then normalized — so overlapping rules blend.
    struct TerrainLayerRule
    {
        u32 LayerIndex = 0; // Which material layer (0..MAX_TERRAIN_LAYERS-1) this rule feeds

        // Height band (normalized [0,1] terrain height). Weight is 1 inside
        // [MinHeight, MaxHeight] and smoothly falls to 0 over HeightBlend on each side.
        f32 MinHeight = 0.0f;
        f32 MaxHeight = 1.0f;
        f32 HeightBlend = 0.08f;

        // Slope band (degrees from horizontal; 0 = flat, 90 = vertical cliff).
        f32 MinSlopeDeg = 0.0f;
        f32 MaxSlopeDeg = 90.0f;
        f32 SlopeBlend = 6.0f;

        f32 Strength = 1.0f; // Overall weight multiplier for this rule

        auto operator==(const TerrainLayerRule& o) const -> bool;
    };

    // Rule-based procedural terrain generation. Two responsibilities:
    //   1. Shape a height field (fBm + ridged + domain-warp + terrace + exponent).
    //   2. Auto-assign material layers (the splatmap) from height/slope rules so a
    //      generated terrain comes out textured + ready for slope-aware foliage,
    //      instead of a single untextured layer.
    //
    // The math helpers (Terrace, EvaluateRuleWeight, EvaluateLayerWeights,
    // PackLayerWeights, GenerateHeightField) are pure CPU and unit-tested; the
    // GenerateHeightmap / GenerateSplatmap entry points additionally touch the GPU.
    class TerrainGenerator
    {
      public:
        // Base fBm parameters (mirror the TerrainComponent procedural fields) plus
        // the extra shaping. Resolution is the heightmap edge length in texels.
        struct HeightParams
        {
            u32 Resolution = 512;
            i32 Seed = 42;
            u32 Octaves = 6;
            f32 Frequency = 3.0f;
            f32 Lacunarity = 2.0f;
            f32 Persistence = 0.45f;
            TerrainHeightShaping Shaping;

            // Optional hydraulic-erosion post-pass. 0 = off (the field is returned
            // un-eroded, exactly as before). > 0 runs that many deterministic
            // erosion iterations on the shaped field, seeded from Seed.
            i32 ErosionIterations = 0;
            ErosionParams Erosion;
        };

        // ── Height field ────────────────────────────────────────────────────

        // Pure CPU. Fills a normalized [0,1] height field (Resolution × Resolution,
        // row-major). No GPU access — safe to call headless / in unit tests. When
        // params.ErosionIterations > 0 the shaped field is run through the
        // deterministic erosion post-pass (ApplyErosion) before returning.
        static void GenerateHeightField(std::vector<f32>& outHeights, const HeightParams& params);

        // Generate the field and push it into a TerrainData (re-uploads to GPU).
        static void GenerateHeightmap(TerrainData& data, const HeightParams& params);

        // Deterministic CPU hydraulic erosion. Mutates `heights` (row-major,
        // resolution × resolution) in place: `iterations` batches of water
        // droplets carve channels and deposit sediment, then the field is
        // re-clamped to [0,1]. Reproducible for a given (seed, params) — droplets
        // run sequentially, unlike the GPU editor brush whose parallel writes
        // race. Pure CPU; safe headless / in unit tests. A no-op if iterations or
        // resolution is 0, or `heights` isn't resolution² long.
        static void ApplyErosion(std::vector<f32>& heights, u32 resolution, u32 iterations,
                                 const ErosionParams& params, i32 seed);

        // ── Material / splatmap auto-assignment ─────────────────────────────

        // Evaluate the rules across the terrain and write the two RGBA8 splatmaps
        // (layers 0-3 → splatmap 0, layers 4-7 → splatmap 1), then upload. Requires
        // a GL context (allocates the splatmap textures via InitializeCPUSplatmaps).
        // Height comes from data.GetHeightAt; slope from data.GetNormalAt.
        static void GenerateSplatmap(TerrainMaterial& material, const TerrainData& data,
                                     const std::vector<TerrainLayerRule>& rules, u32 splatmapResolution,
                                     f32 worldSizeX, f32 worldSizeZ, f32 heightScale);

        // ── Pure helpers (unit-tested) ──────────────────────────────────────

        // Map a normalized height [0,1] onto `steps` flat plateaus. Monotonic
        // non-decreasing; Terrace(0)=0, Terrace(1)=1; steps==0 returns h unchanged.
        [[nodiscard]] static f32 Terrace(f32 h, u32 steps, f32 sharpness);

        // Membership weight of a single rule at the given height/slope, in [0, Strength].
        [[nodiscard]] static f32 EvaluateRuleWeight(f32 height01, f32 slopeDeg, const TerrainLayerRule& rule);

        // Sum every rule's contribution per layer and normalize so the weights sum
        // to 1. If no rule matches, falls back to layer 0 fully on (so the terrain
        // is never left with an all-zero splat texel).
        static void EvaluateLayerWeights(f32 height01, f32 slopeDeg,
                                         const std::vector<TerrainLayerRule>& rules,
                                         std::array<f32, MAX_TERRAIN_LAYERS>& outWeights);

        // Quantize normalized layer weights into the two RGBA8 splatmap texels.
        static void PackLayerWeights(const std::array<f32, MAX_TERRAIN_LAYERS>& weights,
                                     u8 outSplat0[4], u8 outSplat1[4]);

        // ── Presets ─────────────────────────────────────────────────────────

        // A ready-to-use sand → grass → rock → snow layer set (solid colours, no
        // texture files required) and the matching height/slope rules. Together
        // they turn "enable procedural + auto material" into a textured planet
        // out of the box.
        [[nodiscard]] static std::vector<TerrainLayer> MakeDefaultLayers();
        [[nodiscard]] static std::vector<TerrainLayerRule> MakeDefaultRules();
    };
} // namespace OloEngine
