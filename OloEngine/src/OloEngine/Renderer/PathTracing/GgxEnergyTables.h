#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/gtc/packing.hpp>

#include <array>

// =============================================================================
// GGX SINGLE-SCATTER ENERGY TABLES — GENERATED, DO NOT HAND-EDIT
// =============================================================================
//
// Emitted by tools/OloGgxEnergyTableGen (issue #998) alongside its GLSL twin
// OloEditor/assets/shaders/include/PBRClosureV2Energy.glsl — the SAME packed
// words, hex for hex, consumed by the ClosureV2 closure on both sides of the
// CPU/GPU parity boundary: its Kulla-Conty multiple-scattering compensation and
// its energy-conserving diffuse coupling (ADR 0016 §4).
//
// Each grid node (mu, r) stores two moments of the SINGLE-scattering GGX
// specular lobe, estimated over Heitz-2018 VNDF-sampled half vectors:
//
//   x: 1 - Ess(mu, r),  Ess     = E[ G2/G1 ]               (the albedo, F == 1)
//   y: Schlick(mu, r),  Schlick = E[ G2/G1 (1 - v.h)^5 ]   (its grazing part)
//
// (the estimator identity f*cos/pdf == F * (G2/G1); see the VNDF block in
// PBRCommon.glsl). Together they give the lobe's albedo for ANY Schlick F0,
//
//   E_ss(mu, F0) = F0 (Ess - Schlick) + Schlick,
//
// which is what the diffuse coupling subtracts. The LOSS form of Ess is stored
// because the compensation consumes (1 - Ess) directly and the near-mirror rows
// are ~1e-5, where "1.0f minus a stored 0.99999f" would shred float precision.
// The averages row stores both moments cosine-averaged over mu,
// 2 int M(mu) mu dmu: (1 - E_avg, Schlick_avg).
//
// STORAGE IS PACKED — one node per u32, as two IEEE-754 halfs — and the
// packing is LOAD-BEARING on the GPU side: a plain `const float[256]` in the
// GLSL twin passed glslc but failed NVIDIA's GL linker with "C5025: lvalue in
// assignment too complex" once the lookups were inlined at PBR_MultiLight.glsl's
// three lighting call sites (SPIRV-Cross materialises a dynamically-indexed
// constant array as a local temporary per site). Both languages therefore
// carry the packed words and decode them identically (glm::unpackHalf2x16
// here, unpackHalf2x16 in GLSL), so the two sides evaluate the SAME quantized
// values. Half quantization costs at most 2^-12 = 2.44e-4 absolute on any
// entry (half an ulp of the [0.5, 1) binade) — the generator audits that bound
// and refuses to emit a table exceeding it.
//
// Conventions (must match the v2 closure on both sides):
//   * alpha = clamp(r, kMinRoughness, 1)^2 — the v2 perceptual clamp, so each
//     row is exactly the albedo of the lobe the v2 sampler samples. Lookups
//     take AUTHORED roughness; the rows bake the clamp in.
//   * NODE-CENTRED, SQUARE-ROOT SPACED (issue #1478): node j of 16 sits at
//     (j / 15)^2 on both axes, so a value x has lookup coordinate
//     sqrt(x) * 15. Both endpoints are nodes: the bilinear lookup never
//     clamps and never extrapolates. Entry index = row (roughness) * 16 + column (mu).
//   * mu = 0 is baked at mu = 1e-4, the cosine floor GgxSmithLambda applies
//     (the estimator's G2/G1 needs a finite Lambda(mu_v)); the true limit there
//     is 1 - Ess -> 0, and the node sits within the table's resolution of it.
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
// FresnelSchlick / ClosureV2Roughness out of ReferenceBRDF.h, which is what
// makes generator-vs-engine estimator drift structurally impossible.
// ClosureV2Test recomputes entries with that same sampler and fails if either
// copy rots, and parses the GLSL twin so the two files cannot drift apart.
// =============================================================================

namespace OloEngine::PathTracing
{

    inline constexpr u32 kGgxEnergyTableSize = 16;

    // One grid node per word: half(1 - Ess) | half(Schlick) << 16.
    // Entry index i = row * 16 + column; word = kGgxEnergyPacked[i].
    inline constexpr std::array<u32, 256> kGgxEnergyPacked = {
        0x3b972260u, 0x3b452bcdu, 0x3b421c99u, 0x3a831170u, 0x39870834u, 0x3870035fu, 0x36b00199u, 0x34af00d8u,
        0x3201007au, 0x2edf0048u, 0x2ac6002cu, 0x2567001au, 0x1e320010u, 0x13d40008u, 0x02560004u, 0x00000000u,
        0x3b972260u, 0x3b452bcdu, 0x3b421c99u, 0x3a831170u, 0x39870834u, 0x3870035fu, 0x36b00199u, 0x34af00d8u,
        0x3201007au, 0x2edf0048u, 0x2ac6002cu, 0x2567001au, 0x1e320010u, 0x13d40008u, 0x02560004u, 0x00000000u,
        0x3b972260u, 0x3b452bcdu, 0x3b421c99u, 0x3a831170u, 0x39870834u, 0x3870035fu, 0x36b00199u, 0x34af00d8u,
        0x3201007au, 0x2edf0048u, 0x2ac6002cu, 0x2567001au, 0x1e320010u, 0x13d40008u, 0x02560004u, 0x00000000u,
        0x3b972260u, 0x3b452bcdu, 0x3b421c99u, 0x3a831170u, 0x39870834u, 0x3870035fu, 0x36b00199u, 0x34af00d8u,
        0x3201007au, 0x2edf0048u, 0x2ac6002cu, 0x2567001au, 0x1e320010u, 0x13d40008u, 0x02560004u, 0x00000000u,
        0x3b311c1cu, 0x3a952ebdu, 0x3ad62994u, 0x3a6420c0u, 0x397b19cfu, 0x386b1298u, 0x36ac100bu, 0x34ae084eu,
        0x320004ceu, 0x2edf02d4u, 0x2ac701b5u, 0x25690108u, 0x1e36009bu, 0x13df0053u, 0x025e0022u, 0x00000000u,
        0x3a8016b8u, 0x3a322c17u, 0x39f52ea8u, 0x39e42a54u, 0x3946249cu, 0x38531f01u, 0x36961a8fu, 0x34a51654u,
        0x31fa11e0u, 0x2ede102fu, 0x2ace0992u, 0x2575065bu, 0x1e4f03b3u, 0x141101fcu, 0x02a400d4u, 0x00020006u,
        0x399b1285u, 0x39782863u, 0x392d2e3fu, 0x38ff2e7eu, 0x38af2beau, 0x380527f6u, 0x364b2414u, 0x34832051u,
        0x31e11d05u, 0x2ed31a94u, 0x2ad31897u, 0x258e15d8u, 0x1e921424u, 0x146f1332u, 0x03d210bbu, 0x000b1022u,
        0x38980f45u, 0x3886250du, 0x38552c52u, 0x38172ecdu, 0x37a42eb2u, 0x36d02ce8u, 0x35982a36u, 0x342b2784u,
        0x319824a3u, 0x2ea82216u, 0x2ad71ff9u, 0x25bb1dcbu, 0x1f1c1c64u, 0x154e1b38u, 0x064519b4u, 0x001d18bdu,
        0x37290cbbu, 0x37132281u, 0x36d329d8u, 0x366d2d73u, 0x35eb2f1au, 0x354c2f21u, 0x347e2df5u, 0x33042c71u,
        0x30f22a47u, 0x2e2d285bu, 0x2aa6262fu, 0x25e62481u, 0x1fef22dbu, 0x16f02161u, 0x09d12068u, 0x004f1f7du,
        0x35550b8au, 0x354420eau, 0x35152851u, 0x34c82c3fu, 0x34612e47u, 0x33c72f97u, 0x32a22fd7u, 0x31542f2fu,
        0x2fde2e0du, 0x2d352cd6u, 0x2a002b8bu, 0x25c529e4u, 0x205628a0u, 0x18812772u, 0x0d1f2625u, 0x00b12534u,
        0x33a50b8du, 0x338a2079u, 0x3341277bu, 0x32cb2b51u, 0x322e2d9cu, 0x31732f5fu, 0x30a0304cu, 0x2f7b308bu,
        0x2dab3070u, 0x2bdb3016u, 0x28d82f34u, 0x251a2e2eu, 0x20542d3bu, 0x19592c6cu, 0x0fd02b8au, 0x01512a82u,
        0x315a0c86u, 0x314420d5u, 0x310927adu, 0x30ad2b56u, 0x30372d9cu, 0x2f5b2f89u, 0x2e31309eu, 0x2cfd3142u,
        0x2b9c31a3u, 0x296831c0u, 0x26fb31a1u, 0x23ec3154u, 0x1f8030eeu, 0x1969307cu, 0x10e3300bu, 0x021e2f47u,
        0x2f770df6u, 0x2f4f21d8u, 0x2eee287du, 0x2e5a2c34u, 0x2da62e59u, 0x2cdd3042u, 0x2c0a314au, 0x2a713234u,
        0x28e432f3u, 0x26fd3380u, 0x249f33d8u, 0x217e33fcu, 0x1d9f33f3u, 0x189433c4u, 0x10f1337bu, 0x02d63320u,
        0x2d4c100du, 0x2d272372u, 0x2cd229a0u, 0x2c572d31u, 0x2b942fc5u, 0x2a663132u, 0x29373276u, 0x281733a4u,
        0x2621345bu, 0x245a34d1u, 0x21c73535u, 0x1f043583u, 0x1b8435beu, 0x16a335e5u, 0x101a35fau, 0x031f35feu,
        0x2bce117fu, 0x2b8524ccu, 0x2ae72b34u, 0x2a112e96u, 0x292a30e6u, 0x28403286u, 0x26c2340eu, 0x252c34ceu,
        0x23963581u, 0x214d3624u, 0x1efa36b8u, 0x1c3f373au, 0x18a437adu, 0x144b3809u, 0x0dd33833u, 0x02d83857u,
        0x2a0f1359u, 0x29c32627u, 0x29252c97u, 0x2862302au, 0x2737322bu, 0x25be3417u, 0x246b3512u, 0x228f35ffu,
        0x20ad36dcu, 0x1e6237a7u, 0x1c1f3831u, 0x18f73885u, 0x157138d1u, 0x11253916u, 0x0b683954u, 0x0234398bu
    };

    // The averages row, one roughness node per word:
    // half(1 - E_avg) | half(Schlick_avg) << 16.
    inline constexpr std::array<u32, 16> kGgxEnergyAvgPacked = {
        0x2a1800beu, 0x2a1800beu, 0x2a1800beu, 0x2a1800beu, 0x2a130694u, 0x2a001021u, 0x29cb18fdu, 0x295f1fdbu,
        0x28b524fau, 0x27b1296eu, 0x25d82d46u, 0x242630a5u, 0x21923379u, 0x1f2f3584u, 0x1c86377fu, 0x19a538bau
    };

    // Decode one grid node (i in [0, 255]): x = 1 - Ess, y = Schlick.
    // GLSL twin: ggxEnergyEntry in PBRClosureV2Energy.glsl.
    [[nodiscard]] inline glm::vec2 GgxEnergyEntry(u32 i) noexcept
    {
        return glm::unpackHalf2x16(kGgxEnergyPacked[i]);
    }

    // Decode one averages node (i in [0, 15]): x = 1 - E_avg, y = Schlick_avg.
    // GLSL twin: ggxEnergyAvgEntry in PBRClosureV2Energy.glsl.
    [[nodiscard]] inline glm::vec2 GgxEnergyAvgEntry(u32 i) noexcept
    {
        return glm::unpackHalf2x16(kGgxEnergyAvgPacked[i]);
    }

} // namespace OloEngine::PathTracing
