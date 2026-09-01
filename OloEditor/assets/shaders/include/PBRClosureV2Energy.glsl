// =============================================================================
// GGX SINGLE-SCATTER ENERGY-LOSS TABLES — GENERATED, DO NOT HAND-EDIT
// =============================================================================
//
// Emitted by tools/OloGgxEnergyTableGen (issue #998). The command line that
// reproduces this file, and the reason regeneration is a tool run rather than a
// prose recipe, live in the REGENERATION block of its C++ twin
// OloEngine/src/OloEngine/Renderer/PathTracing/GgxEnergyTables.h and in
// ADR 0016 §6.
//
// Data for PBR closure v2's Kulla-Conty multiple-scattering energy compensation
// (Kulla & Conty, "Revisiting Physically Based Shading at Imageworks", 2017).
//
// The tables store 1 - Ess(mu, r): the fraction of energy the SINGLE-scattering
// GGX specular lobe loses to inter-facet shadowing, where
//
//   Ess(mu_o, r) = E[ G2/G1 ]  over Heitz-2018 VNDF-sampled half vectors,
//
// which is the exact estimator identity f*cos/pdf == F * (G2/G1) with F == 1
// (see the VNDF block in PBRCommon.glsl). The LOSS form is stored rather than
// Ess itself because the compensation term consumes (1 - Ess) directly and the
// near-mirror rows are ~1e-5, where "1.0 minus a stored 0.99999" would shred
// float precision.
//
// STORAGE IS PACKED, AND THE PACKING IS LOAD-BEARING. Two IEEE-754 half floats
// per uint, four uints per uvec4 — 34 uvec4 constants instead of 272 floats.
// A plain `const float[256]` here LINKED FINE through glslc but FAILED AT
// RUNTIME on NVIDIA GL ("error C5025: lvalue in assignment too complex"):
// SPIRV-Cross materialises a dynamically-indexed constant array as a
// function-local temporary copy, and once the lookups were inlined at the
// three lighting call sites of a large shader (PBR_MultiLight.glsl) the
// driver's complexity limit tripped — while single-call-site probe shaders
// compiled the very same array without complaint. Packing cuts the emitted
// assignment count ~8x, far below the cliff. See glsl-shaders.md §12.
//
// Half precision costs at most 2.3e-4 absolute on any entry — the generator
// audits that bound and refuses to emit a table exceeding it — an order of
// magnitude under every consuming tolerance; entries below the compensation
// gate (lossAvg < 1e-4 returns 0) don't matter at all.
//
// Conventions (must match the v2 closure on both sides of the parity boundary):
//   * alpha = clamp(r, MIN_ROUGHNESS, 1)^2 — the v2 perceptual clamp, so each
//     row is exactly the albedo of the lobe the v2 sampler samples.
//   * Cell-centered grid: mu_j = (j + 0.5)/16 across a row (a uint packs the
//     even column in its LOW half and the odd column in its HIGH half, matching
//     unpackHalf2x16's (low, high) return), r_k = (k + 0.5)/16 down rows;
//     bilinear lookup clamps at the edges and never extrapolates.
//   * Estimator: 4096 deterministic Hammersley points per entry; E_avg uses a
//     64-point midpoint quadrature over mu at 2048 points per evaluation.
//
// The C++ twin is OloEngine/src/OloEngine/Renderer/PathTracing/GgxEnergyTables.h
// — the SAME packed words, decoded with glm::unpackHalf2x16, so the two sides
// evaluate identical quantized values. ClosureV2Test pins both files against
// the estimator and against each other; the GPU parity probe covers the full
// compensated closure.
// =============================================================================
#ifndef PBR_CLOSURE_V2_ENERGY_GLSL
#define PBR_CLOSURE_V2_ENERGY_GLSL

#define OLO_GGX_ENERGY_TABLE_SIZE 16

// 1 - Ess(mu, r), half-packed. Linear entry index i = row * 16 + column;
// word = kGgxEnergyLossPacked[i >> 3][(i >> 1) & 3], low/high half by i & 1.
const uvec4 kGgxEnergyLossPacked[32] = uvec4[32](
    uvec4(0x04c5145bu, 0x00d601aeu, 0x0050007du, 0x00260036u),
    uvec4(0x0014001cu, 0x000a000eu, 0x00040008u, 0x00000002u),
    uvec4(0x1cf6297bu, 0x1154162du, 0x08eb0f80u, 0x048f0689u),
    uvec4(0x02590341u, 0x013201b0u, 0x008500d1u, 0x00160048u),
    uvec4(0x28e52ee5u, 0x1f9f23bfu, 0x1a8f1ca3u, 0x16be18f7u),
    uvec4(0x144a151du, 0x134013c9u, 0x10b112bfu, 0x10221058u),
    uvec4(0x2dc12e29u, 0x27932a83u, 0x22a924cfu, 0x1f6020f0u),
    uvec4(0x1cf01dfcu, 0x1b9a1c50u, 0x1a3d1b07u, 0x18ef1965u),
    uvec4(0x2f482ca9u, 0x2c4a2df2u, 0x287d2a26u, 0x256826deu),
    uvec4(0x234d245fu, 0x217b2244u, 0x206320d0u, 0x1f6f200fu),
    uvec4(0x2f472b55u, 0x2ea92f9cu, 0x2c702d7bu, 0x29f72b39u),
    uvec4(0x283e28fcu, 0x2679275bu, 0x253925cau, 0x246c24c9u),
    uvec4(0x2ee12a4bu, 0x302d3028u, 0x2ee92fbfu, 0x2d4b2e10u),
    uvec4(0x2c152ca4u, 0x2a802b40u, 0x295b29e1u, 0x289028ecu),
    uvec4(0x2ea729eau, 0x30be3053u, 0x309b30c8u, 0x30003053u),
    uvec4(0x2ebc2f59u, 0x2dac2e2du, 0x2cd92d3cu, 0x2c392c83u),
    uvec4(0x2ebd2a02u, 0x31373089u, 0x31a5318du, 0x316a3194u),
    uvec4(0x30f13131u, 0x306d30aeu, 0x2fe9302fu, 0x2f1a2f7du),
    uvec4(0x2f272a78u, 0x31be30deu, 0x32a4324fu, 0x32d632cdu),
    uvec4(0x32a932c7u, 0x324f327fu, 0x31e6321bu, 0x317d31b1u),
    uvec4(0x2fd82b2fu, 0x3266315du, 0x33af3326u, 0x34243406u),
    uvec4(0x343b3435u, 0x34313439u, 0x34153425u, 0x33de3403u),
    uvec4(0x30622c0eu, 0x333231feu, 0x346a340eu, 0x34e534afu),
    uvec4(0x3529350cu, 0x3549353du, 0x354f354eu, 0x3545354cu),
    uvec4(0x30f12c9au, 0x341032c0u, 0x350a349au, 0x35ae3564u),
    uvec4(0x361c35ebu, 0x36653645u, 0x3692367eu, 0x36ac36a1u),
    uvec4(0x31932d38u, 0x3494339bu, 0x35b63533u, 0x367f3623u),
    uvec4(0x371236ceu, 0x377e374cu, 0x37cf37aau, 0x380637f0u),
    uvec4(0x32422de5u, 0x35223444u, 0x366a35d5u, 0x375436e8u),
    uvec4(0x380237b2u, 0x38473827u, 0x387e3864u, 0x38aa3895u),
    uvec4(0x32fd2e9cu, 0x35b734c0u, 0x3723367eu, 0x381437afu),
    uvec4(0x38783849u, 0x38c738a1u, 0x390738e9u, 0x393d3923u)
);

// 1 - E_avg(r), half-packed, indexed by the same cell-centered roughness rows.
const uvec4 kGgxEnergyLossAvgPacked[2] = uvec4[2](
    uvec4(0x0c6b00beu, 0x1ffe18a6u, 0x28a824c9u, 0x2e322bf9u),
    uvec4(0x321f307cu, 0x34fb33f8u, 0x370d3604u, 0x38803807u)
);

// Decode one linear entry of the loss table (i in [0, 255]).
float ggxEnergyLossEntry(int i)
{
    uint word = kGgxEnergyLossPacked[i >> 3][(i >> 1) & 3];
    vec2 pair = unpackHalf2x16(word);
    return ((i & 1) == 0) ? pair.x : pair.y;
}

// Decode one entry of the averaged-loss row (i in [0, 15]).
float ggxEnergyLossAvgEntry(int i)
{
    uint word = kGgxEnergyLossAvgPacked[i >> 3][(i >> 1) & 3];
    vec2 pair = unpackHalf2x16(word);
    return ((i & 1) == 0) ? pair.x : pair.y;
}

#endif // PBR_CLOSURE_V2_ENERGY_GLSL
