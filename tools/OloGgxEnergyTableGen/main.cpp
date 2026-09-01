// =============================================================================
// OloGgxEnergyTableGen — the baker for the GGX single-scatter energy tables
// (issue #998).
//
// WHAT IT PRODUCES
// ----------------
// One artefact in two languages, byte-identical in its data words:
//
//   OloEngine/src/OloEngine/Renderer/PathTracing/GgxEnergyTables.h
//   OloEditor/assets/shaders/include/PBRClosureV2Energy.glsl
//
// Both files are emitted IN FULL — prose header, guards, arrays and decode
// helpers — never a paste-me fragment. That is the whole point: the tables are
// marked GENERATED, and before this tool existed the only way to regenerate
// them was to re-derive the estimator from a comment and hand-splice hex into
// two files that must stay word-for-word identical. See ADR 0016 §6.
//
// WHY IT IS A C++ TOOL AND NOT A PYTHON SCRIPT
// --------------------------------------------
// Issue #998 offered both homes. A script next to generate_test_catalogue.py
// needs no build wiring, but it has to RE-IMPLEMENT the estimator — and a
// re-implementation is a second mirror of the VNDF sampler that nothing keeps
// in step with the engine's. This tool instead calls SampleGGXVNDFTangent /
// GgxSmithLambda / ClosureV2Roughness out of ReferenceBRDF.h directly, so
// generator-vs-engine estimator drift is structurally impossible rather than
// merely tested for. ReferenceBRDF.h is itself pinned against the compiled
// GLSL by ReferenceBRDFGpuParityTest, so the chain runs
// generator -> C++ reference -> shader with a guard on every link.
//
// The cost is one small CMake target that links glm and spdlog's interface
// (Core/Base.h pulls Log.h in). It is test-only in spirit, the way
// OloHeaderTool is build-only; neither ships.
//
// THE ESTIMATOR
// -------------
//   Ess(mu_o, r) = E[ G2/G1 ]  over Heitz-2018 VNDF-sampled half vectors,
//
// the estimator identity f*cos/pdf == F * (G2/G1) with F == 1. A below-horizon
// reflection scores zero but STILL divides by the sample count — that is what
// makes the near-mirror rows non-zero at all, and getting it wrong changes the
// low-roughness rows by orders of magnitude. ClosureV2Test.cpp's EstimateEss
// is the same estimator and it is the referee: a generator whose output fails
// EnergyTablesMatchTheirOwnEstimator is wrong by definition.
//
// USAGE
// -----
//   OloGgxEnergyTableGen [--repo-root DIR] [--header PATH] [--glsl PATH]
//                        [--grid N] [--samples N]
//                        [--avg-points N] [--avg-samples N]
//                        [--check] [--stdout]
//
// Defaults reproduce the committed tables: --grid 16 --samples 4096
// --avg-points 64 --avg-samples 2048. Raising the sample counts is a flag
// rather than an edit; changing --grid additionally needs ClosureV2Test's
// twin-drift pin updated, since it hardcodes the table size, the packed-array
// lengths and the word counts.
//
// WHAT `--check` IS, AND IS NOT
// -----------------------------
// It recomputes and diffs against the files on disk without writing, exiting 1
// on any difference. That is a BYTE-REPRODUCTION check — "are the committed
// files exactly what THIS build emits?" — and NOT a correctness check. The bake
// is floating-point, so a different compiler, optimisation level or libm can
// legitimately move a low bit and fail it; issue #998 hit exactly that, with an
// f64 estimator and an f32 one disagreeing on 6 of 128 words. So `--check` is a
// meaningful gate only for the toolchain that baked the tables, and wiring it
// into CI across heterogeneous runners would produce a flaky red.
//
// The toolchain-independent correctness gate already exists and belongs in the
// test suite, not here: ClosureV2.EnergyTablesMatchTheirOwnEstimator recomputes
// entries against the engine's own sampler at 2e-3, and
// ClosureV2.EnergyTablesGlslTwinCarriesTheSameNumbers pins the two files to each
// other exactly. Adding a tolerance mode here would duplicate the first and
// blunt this tool's one job.
// =============================================================================

#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::PathTracing;

    struct Options
    {
        std::filesystem::path RepoRoot = ".";
        std::filesystem::path HeaderPath;
        std::filesystem::path GlslPath;
        u32 Grid = 16;
        u32 Samples = 4096;
        u32 AvgPoints = 64;
        u32 AvgSamples = 2048;
        bool Check = false;
        bool ToStdout = false;
    };

    // -------------------------------------------------------------------------
    // Estimator
    // -------------------------------------------------------------------------

    // Deterministic stratified 2D point set — the same 32-bit-reverse
    // construction ReferenceBRDFTest and ClosureV2Test use, reusing the
    // engine's own bit reversal and unit-float mapping so "deterministic
    // Hammersley" means one thing across the repo rather than three.
    [[nodiscard]] glm::vec2 Hammersley(u32 i, u32 n) noexcept
    {
        return glm::vec2(static_cast<f32>(i) / static_cast<f32>(n),
                         SamplerDetail::ToUnitFloat(SamplerDetail::ReverseBits(i)));
    }

    // Ess(mu, r) with the ENGINE'S OWN sampler and weight. Mirrors
    // ClosureV2Test::EstimateEss exactly — that test is this function's pin.
    [[nodiscard]] f64 EstimateEss(f32 mu, f32 roughness, u32 sampleCount) noexcept
    {
        const f32 clamped = ClosureV2Roughness(roughness);
        const f32 alpha = clamped * clamped;
        const f32 sinO = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
        const glm::vec3 wo(sinO, 0.0f, mu);
        const f32 lambdaV = GgxSmithLambda(wo.z, alpha);

        f64 sum = 0.0;
        for (u32 i = 0; i < sampleCount; ++i)
        {
            const glm::vec2 xi = Hammersley(i, sampleCount);
            const glm::vec3 h = SampleGGXVNDFTangent(wo, alpha, alpha, xi);
            const glm::vec3 wi = glm::reflect(-wo, h);
            if (wi.z <= 0.0f)
                continue; // scores zero; the division below still counts it

            const f32 lambdaL = GgxSmithLambda(wi.z, alpha);
            sum += static_cast<f64>((1.0f + lambdaV) / (1.0f + lambdaV + lambdaL));
        }
        return sum / static_cast<f64>(sampleCount);
    }

    // E_avg(r): the cosine-weighted hemispherical average of Ess, normalised —
    // 2 * integral_0^1 Ess(mu, r) mu dmu, by midpoint quadrature over mu.
    [[nodiscard]] f64 EstimateEavg(f32 roughness, u32 quadraturePoints, u32 sampleCount) noexcept
    {
        f64 acc = 0.0;
        for (u32 q = 0; q < quadraturePoints; ++q)
        {
            const f32 mu = (static_cast<f32>(q) + 0.5f) / static_cast<f32>(quadraturePoints);
            acc += 2.0 * EstimateEss(mu, roughness, sampleCount) * static_cast<f64>(mu);
        }
        return acc / static_cast<f64>(quadraturePoints);
    }

    // -------------------------------------------------------------------------
    // IEEE-754 binary16 packing, round-to-nearest-even
    // -------------------------------------------------------------------------
    //
    // Written out rather than delegated to glm::packHalf2x16 for two reasons:
    // the input is f64, so there is no intermediate f32 rounding step to
    // double-round through; and the tie rule is the one property every emitted
    // word hinges on, so it should be readable here rather than inferred from a
    // dependency. The result is verified against glm::unpackHalf2x16 by the
    // audit below and by ClosureV2Test, which reads the words back through glm.
    [[nodiscard]] u16 ToHalfRoundToNearestEven(f64 value) noexcept
    {
        u32 sign = 0u;
        if (value < 0.0)
        {
            sign = 0x8000u;
            value = -value;
        }
        if (!(value > 0.0)) // also catches NaN, which has no business here
            return static_cast<u16>(sign);

        // Smallest exponent whose binade contains `value`, floored at the
        // subnormal binade so the quantum below is right for both cases.
        i32 exponent = -14;
        while (exponent < 15 && value >= std::ldexp(1.0, exponent + 1))
            ++exponent;

        const f64 quantum = std::ldexp(1.0, exponent - 10);
        // The division is exact (the quantum is a power of two), so nearbyint's
        // default FE_TONEAREST gives true round-to-nearest-even on the ratio.
        f64 scaled = std::nearbyint(value / quantum);
        if (scaled >= 2048.0) // rounded up out of its binade
        {
            ++exponent;
            if (exponent > 15)
                return static_cast<u16>(sign | 0x7c00u); // +/-inf
            scaled = 1024.0;
        }

        const auto mantissa = static_cast<u32>(scaled);
        if (exponent == -14 && mantissa < 1024u)
            return static_cast<u16>(sign | mantissa); // subnormal
        return static_cast<u16>(sign | (static_cast<u32>(exponent + 15) << 10) | (mantissa - 1024u));
    }

    // Decode back, to audit the quantization claim the emitted comments make.
    [[nodiscard]] f64 FromHalf(u16 h) noexcept
    {
        const u32 exponent = (h >> 10) & 0x1fu;
        const u32 mantissa = h & 0x3ffu;
        const f64 magnitude = (exponent == 0u)
                                  ? std::ldexp(static_cast<f64>(mantissa), -24)
                                  : std::ldexp(static_cast<f64>(mantissa + 1024u), static_cast<i32>(exponent) - 25);
        return ((h & 0x8000u) != 0u) ? -magnitude : magnitude;
    }

    // Even entry in the LOW half, odd in the HIGH half — matching
    // unpackHalf2x16's (low, high) return on both sides.
    [[nodiscard]] std::vector<u32> PackPairs(const std::vector<f64>& values)
    {
        std::vector<u32> words(values.size() / 2u, 0u);
        for (sizet i = 0; i < values.size(); ++i)
        {
            const u32 half = ToHalfRoundToNearestEven(values[i]);
            if ((i & 1u) == 0u)
                words[i >> 1] = half;
            else
                words[i >> 1] |= (half << 16);
        }
        return words;
    }

    [[nodiscard]] std::string HexWord(u32 w)
    {
        char buffer[16] = {};
        std::snprintf(buffer, sizeof(buffer), "0x%08xu", w);
        return buffer;
    }

    // -------------------------------------------------------------------------
    // Emitters
    // -------------------------------------------------------------------------

    [[nodiscard]] std::string EmitCppArray(const std::vector<u32>& words, std::string_view name, sizet perLine)
    {
        std::ostringstream out;
        out << "    inline constexpr std::array<u32, " << words.size() << "> " << name << " = {\n";
        for (sizet i = 0; i < words.size(); i += perLine)
        {
            out << "       ";
            const sizet end = std::min<sizet>(i + perLine, words.size());
            for (sizet j = i; j < end; ++j)
                out << ' ' << HexWord(words[j]) << ((j + 1u < words.size()) ? "," : "");
            out << '\n';
        }
        out << "    };\n";
        return out.str();
    }

    [[nodiscard]] std::string EmitGlslArray(const std::vector<u32>& words, std::string_view name)
    {
        const sizet vectors = words.size() / 4u;
        std::ostringstream out;
        out << "const uvec4 " << name << "[" << vectors << "] = uvec4[" << vectors << "](\n";
        for (sizet v = 0; v < vectors; ++v)
        {
            out << "    uvec4(";
            for (sizet j = 0; j < 4u; ++j)
                out << HexWord(words[v * 4u + j]) << ((j < 3u) ? ", " : "");
            out << ")" << ((v + 1u < vectors) ? "," : "") << '\n';
        }
        out << ");\n";
        return out.str();
    }

    // The command line that reproduces these files, for the provenance comment.
    [[nodiscard]] std::string ReproCommand(const Options& o)
    {
        std::ostringstream out;
        out << "OloGgxEnergyTableGen[.exe] --grid " << o.Grid << " --samples " << o.Samples << " --avg-points "
            << o.AvgPoints << " --avg-samples " << o.AvgSamples;
        return out.str();
    }

    [[nodiscard]] std::string EmitHeader(const Options& o, const std::vector<u32>& loss, const std::vector<u32>& lossAvg)
    {
        std::ostringstream out;
        out << "#pragma once\n"
               "\n"
               "#include \"OloEngine/Core/Base.h\"\n"
               "\n"
               "#include <glm/gtc/packing.hpp>\n"
               "\n"
               "#include <array>\n"
               "\n"
               "// =============================================================================\n"
               "// GGX SINGLE-SCATTER ENERGY-LOSS TABLES — GENERATED, DO NOT HAND-EDIT\n"
               "// =============================================================================\n"
               "//\n"
               "// Emitted by tools/OloGgxEnergyTableGen (issue #998) alongside its GLSL twin\n"
               "// OloEditor/assets/shaders/include/PBRClosureV2Energy.glsl — the SAME packed\n"
               "// words, hex for hex, consumed by the ClosureV2 closure's Kulla-Conty\n"
               "// multiple-scattering energy compensation on both sides of the CPU/GPU parity\n"
               "// boundary.\n"
               "//\n"
               "// The tables store 1 - Ess(mu, r), where Ess is the directional albedo of the\n"
               "// SINGLE-scattering GGX specular lobe with F == 1:\n"
               "//\n"
               "//   Ess(mu_o, r) = E[ G2/G1 ]  over Heitz-2018 VNDF-sampled half vectors\n"
               "//\n"
               "// (the estimator identity f*cos/pdf == F * (G2/G1); see the VNDF block in\n"
               "// PBRCommon.glsl). The LOSS form is stored because the compensation term\n"
               "// consumes (1 - Ess) directly and the near-mirror rows are ~1e-5, where\n"
               "// \"1.0f minus a stored 0.99999f\" would shred float precision.\n"
               "//\n"
               "// STORAGE IS PACKED — two IEEE-754 halfs per u32, and the packing is\n"
               "// LOAD-BEARING on the GPU side: a plain `const float["
            << (o.Grid * o.Grid)
            << "]` in the GLSL twin\n"
               "// passed glslc but failed NVIDIA's GL linker with \"C5025: lvalue in assignment\n"
               "// too complex\" once the lookups were inlined at PBR_MultiLight.glsl's three\n"
               "// lighting call sites (SPIRV-Cross materialises a dynamically-indexed constant\n"
               "// array as a local temporary per site). Both languages therefore carry the\n"
               "// packed words and decode them identically (glm::unpackHalf2x16 here,\n"
               "// unpackHalf2x16 in GLSL), so the two sides evaluate the SAME quantized\n"
               "// values. Half quantization costs at most 2.3e-4 absolute on any entry — the\n"
               "// generator audits that bound and refuses to emit a table exceeding it — an\n"
               "// order of magnitude under every consuming tolerance.\n"
               "//\n"
               "// Conventions (must match the v2 closure on both sides):\n"
               "//   * alpha = clamp(r, kMinRoughness, 1)^2 — the v2 perceptual clamp, so each\n"
               "//     row is exactly the albedo of the lobe the v2 sampler samples.\n"
               "//   * Cell-centered grid: mu_j = (j + 0.5)/"
            << o.Grid
            << " across a row (a u32 packs the\n"
               "//     even column in its LOW half, the odd column in its HIGH half),\n"
               "//     r_k = (k + 0.5)/"
            << o.Grid
            << " down rows; bilinear lookup clamps at the edges.\n"
               "//\n"
               "// REGENERATION IS A TOOL RUN, NOT A RECIPE (ADR 0016 §6):\n"
               "//\n"
               "//   cmake --build <build-dir> --target OloGgxEnergyTableGen\n"
               "//   <build-dir>/tools/OloGgxEnergyTableGen/<Config>/"
            << ReproCommand(o)
            << "\n"
               "//\n"
               "// run from the repository root. The <Config> segment exists only under\n"
               "// multi-config generators — both this repo's trees are multi-config, so it is\n"
               "// Debug/ or Release/ there; drop it for a single-config tree, and drop the\n"
               "// .exe suffix off Windows.\n"
               "//\n"
               "// It overwrites BOTH files in place, so a clean `git diff` afterwards is the\n"
               "// reproduction proof; `--check` diffs without writing. The flag values above\n"
               "// are the ones these tables were baked with. Raising the SAMPLE COUNTS is a\n"
               "// flag rather than an edit; changing --grid is NOT, because ClosureV2Test's\n"
               "// twin-drift pin hardcodes the table size, the packed-array lengths and the\n"
               "// word counts — a non-default grid has to update those expectations (or derive\n"
               "// them from kGgxEnergyTableSize) in the same change, or that pin fails.\n"
               "// The tool calls the engine's own SampleGGXVNDFTangent / GgxSmithLambda /\n"
               "// ClosureV2Roughness out of ReferenceBRDF.h, which is what makes\n"
               "// generator-vs-engine estimator drift structurally impossible. ClosureV2Test\n"
               "// recomputes entries with that same sampler and fails if either copy rots, and\n"
               "// parses the GLSL twin so the two files cannot drift apart.\n"
               "// =============================================================================\n"
               "\n"
               "namespace OloEngine::PathTracing\n"
               "{\n"
               "\n"
               "    inline constexpr u32 kGgxEnergyTableSize = "
            << o.Grid
            << ";\n"
               "\n"
               "    // 1 - Ess(mu, r), half-packed. Linear entry index i = row * "
            << o.Grid
            << " + column;\n"
               "    // word = kGgxEnergyLossPacked[i >> 1], low/high half selected by i & 1.\n";
        out << EmitCppArray(loss, "kGgxEnergyLossPacked", 8u);
        out << "\n"
               "    // 1 - E_avg(r), half-packed, indexed by the same cell-centered roughness rows.\n";
        out << EmitCppArray(lossAvg, "kGgxEnergyLossAvgPacked", 8u);
        out << "\n"
               "    // Decode one linear entry of the loss table (i in [0, "
            << (o.Grid * o.Grid - 1u)
            << "]).\n"
               "    // GLSL twin: ggxEnergyLossEntry in PBRClosureV2Energy.glsl.\n"
               "    [[nodiscard]] inline f32 GgxEnergyLossEntry(u32 i) noexcept\n"
               "    {\n"
               "        const glm::vec2 pair = glm::unpackHalf2x16(kGgxEnergyLossPacked[i >> 1]);\n"
               "        return ((i & 1u) == 0u) ? pair.x : pair.y;\n"
               "    }\n"
               "\n"
               "    // Decode one entry of the averaged-loss row (i in [0, "
            << (o.Grid - 1u)
            << "]).\n"
               "    // GLSL twin: ggxEnergyLossAvgEntry in PBRClosureV2Energy.glsl.\n"
               "    [[nodiscard]] inline f32 GgxEnergyLossAvgEntry(u32 i) noexcept\n"
               "    {\n"
               "        const glm::vec2 pair = glm::unpackHalf2x16(kGgxEnergyLossAvgPacked[i >> 1]);\n"
               "        return ((i & 1u) == 0u) ? pair.x : pair.y;\n"
               "    }\n"
               "\n"
               "} // namespace OloEngine::PathTracing\n";
        return out.str();
    }

    [[nodiscard]] std::string EmitGlsl(const Options& o, const std::vector<u32>& loss, const std::vector<u32>& lossAvg)
    {
        const sizet lossVectors = loss.size() / 4u;
        const sizet avgVectors = lossAvg.size() / 4u;
        std::ostringstream out;
        out << "// =============================================================================\n"
               "// GGX SINGLE-SCATTER ENERGY-LOSS TABLES — GENERATED, DO NOT HAND-EDIT\n"
               "// =============================================================================\n"
               "//\n"
               "// Emitted by tools/OloGgxEnergyTableGen (issue #998). The command line that\n"
               "// reproduces this file, and the reason regeneration is a tool run rather than a\n"
               "// prose recipe, live in the REGENERATION block of its C++ twin\n"
               "// OloEngine/src/OloEngine/Renderer/PathTracing/GgxEnergyTables.h and in\n"
               "// ADR 0016 §6.\n"
               "//\n"
               "// Data for PBR closure v2's Kulla-Conty multiple-scattering energy compensation\n"
               "// (Kulla & Conty, \"Revisiting Physically Based Shading at Imageworks\", 2017).\n"
               "//\n"
               "// The tables store 1 - Ess(mu, r): the fraction of energy the SINGLE-scattering\n"
               "// GGX specular lobe loses to inter-facet shadowing, where\n"
               "//\n"
               "//   Ess(mu_o, r) = E[ G2/G1 ]  over Heitz-2018 VNDF-sampled half vectors,\n"
               "//\n"
               "// which is the exact estimator identity f*cos/pdf == F * (G2/G1) with F == 1\n"
               "// (see the VNDF block in PBRCommon.glsl). The LOSS form is stored rather than\n"
               "// Ess itself because the compensation term consumes (1 - Ess) directly and the\n"
               "// near-mirror rows are ~1e-5, where \"1.0 minus a stored 0.99999\" would shred\n"
               "// float precision.\n"
               "//\n"
               "// STORAGE IS PACKED, AND THE PACKING IS LOAD-BEARING. Two IEEE-754 half floats\n"
               "// per uint, four uints per uvec4 — "
            << (lossVectors + avgVectors) << " uvec4 constants instead of " << (o.Grid * o.Grid + o.Grid)
            << " floats.\n"
               "// A plain `const float["
            << (o.Grid * o.Grid)
            << "]` here LINKED FINE through glslc but FAILED AT\n"
               "// RUNTIME on NVIDIA GL (\"error C5025: lvalue in assignment too complex\"):\n"
               "// SPIRV-Cross materialises a dynamically-indexed constant array as a\n"
               "// function-local temporary copy, and once the lookups were inlined at the\n"
               "// three lighting call sites of a large shader (PBR_MultiLight.glsl) the\n"
               "// driver's complexity limit tripped — while single-call-site probe shaders\n"
               "// compiled the very same array without complaint. Packing cuts the emitted\n"
               "// assignment count ~8x, far below the cliff. See glsl-shaders.md §12.\n"
               "//\n"
               "// Half precision costs at most 2.3e-4 absolute on any entry — the generator\n"
               "// audits that bound and refuses to emit a table exceeding it — an order of\n"
               "// magnitude under every consuming tolerance; entries below the compensation\n"
               "// gate (lossAvg < 1e-4 returns 0) don't matter at all.\n"
               "//\n"
               "// Conventions (must match the v2 closure on both sides of the parity boundary):\n"
               "//   * alpha = clamp(r, MIN_ROUGHNESS, 1)^2 — the v2 perceptual clamp, so each\n"
               "//     row is exactly the albedo of the lobe the v2 sampler samples.\n"
               "//   * Cell-centered grid: mu_j = (j + 0.5)/"
            << o.Grid
            << " across a row (a uint packs the\n"
               "//     even column in its LOW half and the odd column in its HIGH half, matching\n"
               "//     unpackHalf2x16's (low, high) return), r_k = (k + 0.5)/"
            << o.Grid
            << " down rows;\n"
               "//     bilinear lookup clamps at the edges and never extrapolates.\n"
               "//   * Estimator: "
            << o.Samples
            << " deterministic Hammersley points per entry; E_avg uses a\n"
               "//     "
            << o.AvgPoints << "-point midpoint quadrature over mu at " << o.AvgSamples
            << " points per evaluation.\n"
               "//\n"
               "// The C++ twin is OloEngine/src/OloEngine/Renderer/PathTracing/GgxEnergyTables.h\n"
               "// — the SAME packed words, decoded with glm::unpackHalf2x16, so the two sides\n"
               "// evaluate identical quantized values. ClosureV2Test pins both files against\n"
               "// the estimator and against each other; the GPU parity probe covers the full\n"
               "// compensated closure.\n"
               "// =============================================================================\n"
               "#ifndef PBR_CLOSURE_V2_ENERGY_GLSL\n"
               "#define PBR_CLOSURE_V2_ENERGY_GLSL\n"
               "\n"
               "#define OLO_GGX_ENERGY_TABLE_SIZE "
            << o.Grid
            << "\n"
               "\n"
               "// 1 - Ess(mu, r), half-packed. Linear entry index i = row * "
            << o.Grid
            << " + column;\n"
               "// word = kGgxEnergyLossPacked[i >> 3][(i >> 1) & 3], low/high half by i & 1.\n";
        out << EmitGlslArray(loss, "kGgxEnergyLossPacked");
        out << "\n"
               "// 1 - E_avg(r), half-packed, indexed by the same cell-centered roughness rows.\n";
        out << EmitGlslArray(lossAvg, "kGgxEnergyLossAvgPacked");
        out << "\n"
               "// Decode one linear entry of the loss table (i in [0, "
            << (o.Grid * o.Grid - 1u)
            << "]).\n"
               "float ggxEnergyLossEntry(int i)\n"
               "{\n"
               "    uint word = kGgxEnergyLossPacked[i >> 3][(i >> 1) & 3];\n"
               "    vec2 pair = unpackHalf2x16(word);\n"
               "    return ((i & 1) == 0) ? pair.x : pair.y;\n"
               "}\n"
               "\n"
               "// Decode one entry of the averaged-loss row (i in [0, "
            << (o.Grid - 1u)
            << "]).\n"
               "float ggxEnergyLossAvgEntry(int i)\n"
               "{\n"
               "    uint word = kGgxEnergyLossAvgPacked[i >> 3][(i >> 1) & 3];\n"
               "    vec2 pair = unpackHalf2x16(word);\n"
               "    return ((i & 1) == 0) ? pair.x : pair.y;\n"
               "}\n"
               "\n"
               "#endif // PBR_CLOSURE_V2_ENERGY_GLSL\n";
        return out.str();
    }

    // -------------------------------------------------------------------------
    // Plumbing
    // -------------------------------------------------------------------------

    [[nodiscard]] bool ParseU32(std::string_view text, u32& out)
    {
        const auto* first = text.data();
        const auto* last = text.data() + text.size();
        const auto result = std::from_chars(first, last, out);
        return result.ec == std::errc{} && result.ptr == last && out > 0u;
    }

    // BOTH streams below are deliberately TEXT mode, and the pairing is what
    // makes it correct — do not "fix" either one to std::ios::binary.
    //
    // The emitted string uses \n throughout. Text mode makes the write match
    // what `git checkout` produces on this platform (CRLF under
    // core.autocrlf=true, LF elsewhere), and makes the read collapse that back
    // to \n, so --check compares like with like. Verified in this repo: the
    // working-tree file is CRLF while the committed blob is LF.
    //
    // Switching to binary breaks the round trip rather than tightening it: the
    // write would put LF in a Windows working tree that git renders as CRLF
    // everywhere else, and the read would then hand --check a CRLF string to
    // compare against an LF expectation — a spurious DIFFERS on every fresh
    // Windows checkout. Newline translation here is the feature, not a leak.
    [[nodiscard]] bool WriteFile(const std::filesystem::path& path, const std::string& contents)
    {
        std::ofstream file(path);
        if (!file)
            return false;
        file << contents;
        return static_cast<bool>(file);
    }

    [[nodiscard]] bool ReadFile(const std::filesystem::path& path, std::string& out)
    {
        std::ifstream file(path);
        if (!file)
            return false;
        std::ostringstream buffer;
        buffer << file.rdbuf();
        out = buffer.str();
        return true;
    }

    void PrintUsage()
    {
        std::cout << "OloGgxEnergyTableGen - bakes the GGX energy tables into both language twins.\n"
                     "\n"
                     "  --repo-root DIR    repository root (default: the current directory)\n"
                     "  --header PATH      override the C++ output path\n"
                     "  --glsl PATH        override the GLSL output path\n"
                     "  --grid N           table edge length (default 16; also needs ClosureV2Test's\n"
                     "                     twin-drift pin updated, which hardcodes the sizes)\n"
                     "  --samples N        VNDF samples per entry (default 4096)\n"
                     "  --avg-points N     E_avg midpoint quadrature points (default 64)\n"
                     "  --avg-samples N    VNDF samples per quadrature point (default 2048)\n"
                     "  --check            recompute and diff against disk; write nothing\n"
                     "  --stdout           print both files instead of writing them\n";
    }
} // namespace

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        const auto next = [&](std::string_view what) -> std::string_view
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: " << what << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return 0;
        }
        else if (arg == "--repo-root")
            options.RepoRoot = next(arg);
        else if (arg == "--header")
            options.HeaderPath = next(arg);
        else if (arg == "--glsl")
            options.GlslPath = next(arg);
        else if (arg == "--check")
            options.Check = true;
        else if (arg == "--stdout")
            options.ToStdout = true;
        else if (arg == "--grid" || arg == "--samples" || arg == "--avg-points" || arg == "--avg-samples")
        {
            const std::string_view value = next(arg);
            u32 parsed = 0u;
            if (!ParseU32(value, parsed))
            {
                std::cerr << "error: " << arg << " expects a positive integer, got '" << value << "'\n";
                return 2;
            }
            if (arg == "--grid")
                options.Grid = parsed;
            else if (arg == "--samples")
                options.Samples = parsed;
            else if (arg == "--avg-points")
                options.AvgPoints = parsed;
            else
                options.AvgSamples = parsed;
        }
        else
        {
            std::cerr << "error: unknown argument '" << arg << "'\n";
            PrintUsage();
            return 2;
        }
    }

    // The GLSL twin packs eight entries per uvec4 and its decode helpers shift
    // by 3, so a grid whose rows do not divide into whole uvec4s would emit a
    // table the shader cannot index. Refuse rather than emit something subtly
    // wrong — this is the flag a future widening reaches for first.
    if (options.Grid % 8u != 0u)
    {
        std::cerr << "error: --grid must be a multiple of 8 (the GLSL twin packs 8 entries per uvec4); got "
                  << options.Grid << "\n";
        return 2;
    }

    if (options.HeaderPath.empty())
        options.HeaderPath = options.RepoRoot / "OloEngine/src/OloEngine/Renderer/PathTracing/GgxEnergyTables.h";
    if (options.GlslPath.empty())
        options.GlslPath = options.RepoRoot / "OloEditor/assets/shaders/include/PBRClosureV2Energy.glsl";

    // ---- bake ---------------------------------------------------------------
    std::vector<f64> loss(static_cast<sizet>(options.Grid) * options.Grid);
    for (u32 row = 0; row < options.Grid; ++row)
    {
        const f32 roughness = (static_cast<f32>(row) + 0.5f) / static_cast<f32>(options.Grid);
        for (u32 col = 0; col < options.Grid; ++col)
        {
            const f32 mu = (static_cast<f32>(col) + 0.5f) / static_cast<f32>(options.Grid);
            loss[static_cast<sizet>(row) * options.Grid + col] = 1.0 - EstimateEss(mu, roughness, options.Samples);
        }
    }

    std::vector<f64> lossAvg(options.Grid);
    for (u32 row = 0; row < options.Grid; ++row)
    {
        const f32 roughness = (static_cast<f32>(row) + 0.5f) / static_cast<f32>(options.Grid);
        lossAvg[row] = 1.0 - EstimateEavg(roughness, options.AvgPoints, options.AvgSamples);
    }

    const std::vector<u32> lossWords = PackPairs(loss);
    const std::vector<u32> lossAvgWords = PackPairs(lossAvg);

    // ---- audit the quantization claim the emitted comments make -------------
    f64 worstError = 0.0;
    const auto audit = [&worstError](const std::vector<f64>& values)
    {
        for (const f64 v : values)
            worstError = std::max(worstError, std::abs(v - FromHalf(ToHalfRoundToNearestEven(v))));
    };
    audit(loss);
    audit(lossAvg);
    constexpr f64 kQuantizationBudget = 2.3e-4;
    std::cout << "half quantization: worst absolute error " << worstError << " (budget " << kQuantizationBudget
              << ")\n";
    if (worstError > kQuantizationBudget)
    {
        std::cerr << "error: quantization error exceeds the budget the generated comments claim.\n"
                     "       Either the grid moved into a regime half cannot hold, or the claim needs revising.\n";
        return 1;
    }

    const std::string header = EmitHeader(options, lossWords, lossAvgWords);
    const std::string glsl = EmitGlsl(options, lossWords, lossAvgWords);

    if (options.ToStdout)
    {
        std::cout << header << "\n----8<----\n"
                  << glsl;
        return 0;
    }

    if (options.Check)
    {
        // Byte-exact on purpose — see "WHAT `--check` IS, AND IS NOT" at the top
        // of this file. The comparison is only meaningful for the toolchain that
        // baked the tables; the toolchain-independent gate is
        // ClosureV2.EnergyTablesMatchTheirOwnEstimator, at 2e-3.
        int differences = 0;
        const auto compare = [&differences](const std::filesystem::path& path, const std::string& expected)
        {
            std::string actual;
            if (!ReadFile(path, actual))
            {
                std::cerr << "error: cannot read " << path.string() << "\n";
                ++differences;
                return;
            }
            if (actual != expected)
            {
                std::cerr << "DIFFERS: " << path.string() << "\n";
                ++differences;
            }
            else
                std::cout << "ok: " << path.string() << "\n";
        };
        compare(options.HeaderPath, header);
        compare(options.GlslPath, glsl);
        return (differences == 0) ? 0 : 1;
    }

    if (!WriteFile(options.HeaderPath, header))
    {
        std::cerr << "error: cannot write " << options.HeaderPath.string() << "\n";
        return 1;
    }
    if (!WriteFile(options.GlslPath, glsl))
    {
        std::cerr << "error: cannot write " << options.GlslPath.string() << "\n";
        return 1;
    }

    std::cout << "wrote " << options.HeaderPath.string() << "\n"
              << "wrote " << options.GlslPath.string() << "\n";
    return 0;
}
