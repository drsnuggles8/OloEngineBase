#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/gtc/packing.hpp>

#include <array>

// =============================================================================
// GGX SINGLE-SCATTER ENERGY-LOSS TABLES — GENERATED, DO NOT HAND-EDIT
// =============================================================================
//
// Emitted by tools/OloGgxEnergyTableGen (issue #998) alongside its GLSL twin
// OloEditor/assets/shaders/include/PBRClosureV2Energy.glsl — the SAME packed
// words, hex for hex, consumed by the ClosureV2 closure's Kulla-Conty
// multiple-scattering energy compensation on both sides of the CPU/GPU parity
// boundary.
//
// The tables store 1 - Ess(mu, r), where Ess is the directional albedo of the
// SINGLE-scattering GGX specular lobe with F == 1:
//
//   Ess(mu_o, r) = E[ G2/G1 ]  over Heitz-2018 VNDF-sampled half vectors
//
// (the estimator identity f*cos/pdf == F * (G2/G1); see the VNDF block in
// PBRCommon.glsl). The LOSS form is stored because the compensation term
// consumes (1 - Ess) directly and the near-mirror rows are ~1e-5, where
// "1.0f minus a stored 0.99999f" would shred float precision.
//
// STORAGE IS PACKED — two IEEE-754 halfs per u32, and the packing is
// LOAD-BEARING on the GPU side: a plain `const float[256]` in the GLSL twin
// passed glslc but failed NVIDIA's GL linker with "C5025: lvalue in assignment
// too complex" once the lookups were inlined at PBR_MultiLight.glsl's three
// lighting call sites (SPIRV-Cross materialises a dynamically-indexed constant
// array as a local temporary per site). Both languages therefore carry the
// packed words and decode them identically (glm::unpackHalf2x16 here,
// unpackHalf2x16 in GLSL), so the two sides evaluate the SAME quantized
// values. Half quantization costs at most 2.3e-4 absolute on any entry — the
// generator audits that bound and refuses to emit a table exceeding it — an
// order of magnitude under every consuming tolerance.
//
// Conventions (must match the v2 closure on both sides):
//   * alpha = clamp(r, kMinRoughness, 1)^2 — the v2 perceptual clamp, so each
//     row is exactly the albedo of the lobe the v2 sampler samples.
//   * Cell-centered grid: mu_j = (j + 0.5)/16 across a row (a u32 packs the
//     even column in its LOW half, the odd column in its HIGH half),
//     r_k = (k + 0.5)/16 down rows; bilinear lookup clamps at the edges.
//
// REGENERATION IS A TOOL RUN, NOT A RECIPE (ADR 0016 §6):
//
//   cmake --build <build-dir> --target OloGgxEnergyTableGen
//   <build-dir>/tools/OloGgxEnergyTableGen/<Config>/OloGgxEnergyTableGen[.exe] --grid 16 --samples 4096 --avg-points 64 --avg-samples 2048
//
// run from the repository root. The <Config> segment exists only under
// multi-config generators — both this repo's trees are multi-config, so it is
// Debug/ or Release/ there; drop it for a single-config tree, and drop the
// .exe suffix off Windows.
//
// It overwrites BOTH files in place, so a clean `git diff` afterwards is the
// reproduction proof; `--check` diffs without writing. The flag values above
// are the ones these tables were baked with. Raising the SAMPLE COUNTS is a
// flag rather than an edit; changing --grid is NOT, because ClosureV2Test's
// twin-drift pin hardcodes the table size, the packed-array lengths and the
// word counts — a non-default grid has to update those expectations (or derive
// them from kGgxEnergyTableSize) in the same change, or that pin fails.
// The tool calls the engine's own SampleGGXVNDFTangent / GgxSmithLambda /
// ClosureV2Roughness out of ReferenceBRDF.h, which is what makes
// generator-vs-engine estimator drift structurally impossible. ClosureV2Test
// recomputes entries with that same sampler and fails if either copy rots, and
// parses the GLSL twin so the two files cannot drift apart.
// =============================================================================

namespace OloEngine::PathTracing
{

    inline constexpr u32 kGgxEnergyTableSize = 16;

    // 1 - Ess(mu, r), half-packed. Linear entry index i = row * 16 + column;
    // word = kGgxEnergyLossPacked[i >> 1], low/high half selected by i & 1.
    inline constexpr std::array<u32, 128> kGgxEnergyLossPacked = {
        0x04c5145bu, 0x00d601aeu, 0x0050007du, 0x00260036u, 0x0014001cu, 0x000a000eu, 0x00040008u, 0x00000002u,
        0x1cf6297bu, 0x1154162du, 0x08eb0f80u, 0x048f0689u, 0x02590341u, 0x013201b0u, 0x008500d1u, 0x00160048u,
        0x28e52ee5u, 0x1f9f23bfu, 0x1a8f1ca3u, 0x16be18f7u, 0x144a151du, 0x134013c9u, 0x10b112bfu, 0x10221058u,
        0x2dc12e29u, 0x27932a83u, 0x22a924cfu, 0x1f6020f0u, 0x1cf01dfcu, 0x1b9a1c50u, 0x1a3d1b07u, 0x18ef1965u,
        0x2f482ca9u, 0x2c4a2df2u, 0x287d2a26u, 0x256826deu, 0x234d245fu, 0x217b2244u, 0x206320d0u, 0x1f6f200fu,
        0x2f472b55u, 0x2ea92f9cu, 0x2c702d7bu, 0x29f72b39u, 0x283e28fcu, 0x2679275bu, 0x253925cau, 0x246c24c9u,
        0x2ee12a4bu, 0x302d3028u, 0x2ee92fbfu, 0x2d4b2e10u, 0x2c152ca4u, 0x2a802b40u, 0x295b29e1u, 0x289028ecu,
        0x2ea729eau, 0x30be3053u, 0x309b30c8u, 0x30003053u, 0x2ebc2f59u, 0x2dac2e2du, 0x2cd92d3cu, 0x2c392c83u,
        0x2ebd2a02u, 0x31373089u, 0x31a5318du, 0x316a3194u, 0x30f13131u, 0x306d30aeu, 0x2fe9302fu, 0x2f1a2f7du,
        0x2f272a78u, 0x31be30deu, 0x32a4324fu, 0x32d632cdu, 0x32a932c7u, 0x324f327fu, 0x31e6321bu, 0x317d31b1u,
        0x2fd82b2fu, 0x3266315du, 0x33af3326u, 0x34243406u, 0x343b3435u, 0x34313439u, 0x34153425u, 0x33de3403u,
        0x30622c0eu, 0x333231feu, 0x346a340eu, 0x34e534afu, 0x3529350cu, 0x3549353du, 0x354f354eu, 0x3545354cu,
        0x30f12c9au, 0x341032c0u, 0x350a349au, 0x35ae3564u, 0x361c35ebu, 0x36653645u, 0x3692367eu, 0x36ac36a1u,
        0x31932d38u, 0x3494339bu, 0x35b63533u, 0x367f3623u, 0x371236ceu, 0x377e374cu, 0x37cf37aau, 0x380637f0u,
        0x32422de5u, 0x35223444u, 0x366a35d5u, 0x375436e8u, 0x380237b2u, 0x38473827u, 0x387e3864u, 0x38aa3895u,
        0x32fd2e9cu, 0x35b734c0u, 0x3723367eu, 0x381437afu, 0x38783849u, 0x38c738a1u, 0x390738e9u, 0x393d3923u
    };

    // 1 - E_avg(r), half-packed, indexed by the same cell-centered roughness rows.
    inline constexpr std::array<u32, 8> kGgxEnergyLossAvgPacked = {
        0x0c6b00beu, 0x1ffe18a6u, 0x28a824c9u, 0x2e322bf9u, 0x321f307cu, 0x34fb33f8u, 0x370d3604u, 0x38803807u
    };

    // Decode one linear entry of the loss table (i in [0, 255]).
    // GLSL twin: ggxEnergyLossEntry in PBRClosureV2Energy.glsl.
    [[nodiscard]] inline f32 GgxEnergyLossEntry(u32 i) noexcept
    {
        const glm::vec2 pair = glm::unpackHalf2x16(kGgxEnergyLossPacked[i >> 1]);
        return ((i & 1u) == 0u) ? pair.x : pair.y;
    }

    // Decode one entry of the averaged-loss row (i in [0, 15]).
    // GLSL twin: ggxEnergyLossAvgEntry in PBRClosureV2Energy.glsl.
    [[nodiscard]] inline f32 GgxEnergyLossAvgEntry(u32 i) noexcept
    {
        const glm::vec2 pair = glm::unpackHalf2x16(kGgxEnergyLossAvgPacked[i >> 1]);
        return ((i & 1u) == 0u) ? pair.x : pair.y;
    }

} // namespace OloEngine::PathTracing
