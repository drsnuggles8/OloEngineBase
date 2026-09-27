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
//   Ess(mu_o, r)     = E[ G2/G1 ]               over Heitz-2018 VNDF-sampled
//   Schlick(mu_o, r) = E[ G2/G1 (1 - v.h)^5 ]   half vectors,
//
// the estimator identity f*cos/pdf == F * (G2/G1), with F == 1 for Ess and the
// Schlick grazing factor for the second moment (issue #1479's diffuse coupling
// needs the lobe's albedo for any F0: F0 (Ess - Schlick) + Schlick). A below-horizon
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

    // The two moments of the single-scattering lobe one table entry stores.
    struct Moments
    {
        f64 Ess = 0.0;     // E[G2/G1]:               the albedo with F == 1
        f64 Schlick = 0.0; // E[G2/G1 (1 - v.h)^5]:   its Schlick-weighted part
    };

    // Ess(mu, r) and the Schlick moment with the ENGINE'S OWN sampler, weight
    // and Fresnel. Ess mirrors ClosureV2Test::EstimateEss exactly — that test
    // is this function's pin. The Schlick moment is the same estimator with
    // the engine's FresnelSchlick at F0 = 0, i.e. (1 - v.h)^5, as the extra
    // factor, so the single-scatter albedo for any F0 is
    //   E_ss(mu, F0) = F0 Ess + (1 - F0) Schlick = F0 (Ess - Schlick) + Schlick.
    [[nodiscard]] Moments EstimateMoments(f32 mu, f32 roughness, u32 sampleCount) noexcept
    {
        const f32 clamped = ClosureV2Roughness(roughness);
        const f32 alpha = clamped * clamped;
        const f32 sinO = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
        const glm::vec3 wo(sinO, 0.0f, mu);
        const f32 lambdaV = GgxSmithLambda(wo.z, alpha);

        f64 ess = 0.0;
        f64 schlick = 0.0;
        for (u32 i = 0; i < sampleCount; ++i)
        {
            const glm::vec2 xi = Hammersley(i, sampleCount);
            const glm::vec3 h = SampleGGXVNDFTangent(wo, alpha, alpha, xi);
            const glm::vec3 wi = glm::reflect(-wo, h);
            if (wi.z <= 0.0f)
                continue; // scores zero; the division below still counts it

            const f32 lambdaL = GgxSmithLambda(wi.z, alpha);
            const f32 weight = (1.0f + lambdaV) / (1.0f + lambdaV + lambdaL);
            const f32 grazing = FresnelSchlick(std::max(glm::dot(wo, h), 0.0f), glm::vec3(0.0f)).x;
            ess += static_cast<f64>(weight);
            schlick += static_cast<f64>(weight * grazing);
        }
        return { ess / static_cast<f64>(sampleCount), schlick / static_cast<f64>(sampleCount) };
    }

    // The cosine-weighted hemispherical averages of both moments, normalised —
    // 2 * integral_0^1 M(mu, r) mu dmu, by midpoint quadrature over mu. The
    // nodes sit at (q + 0.5) / N >= 1/128, clear of the mu floor below.
    [[nodiscard]] Moments EstimateAverages(f32 roughness, u32 quadraturePoints, u32 sampleCount) noexcept
    {
        Moments acc;
        for (u32 q = 0; q < quadraturePoints; ++q)
        {
            const f32 mu = (static_cast<f32>(q) + 0.5f) / static_cast<f32>(quadraturePoints);
            const Moments m = EstimateMoments(mu, roughness, sampleCount);
            acc.Ess += 2.0 * m.Ess * static_cast<f64>(mu);
            acc.Schlick += 2.0 * m.Schlick * static_cast<f64>(mu);
        }
        return { acc.Ess / static_cast<f64>(quadraturePoints), acc.Schlick / static_cast<f64>(quadraturePoints) };
    }

    // -------------------------------------------------------------------------
    // The grid (issue #1478)
    // -------------------------------------------------------------------------
    //
    // NODE-CENTRED ON A SQUARE-ROOT AXIS, in both mu and roughness: node j of
    // N sits at value (j / (N - 1))^2, so the lookup coordinate of a value x is
    // sqrt(x) (N - 1). Both endpoints are nodes, so no lookup clamps or
    // extrapolates anywhere in [0, 1]^2.
    //
    // Node-centred because the cell-centred grid before it clamped a quarter
    // cell short of mu = 0, mu = 1 and r = 1, which read the white furnace
    // 3.8 % short at r = 1 (#1478). Square-root spaced because 1 - Ess is not
    // smooth on a linear mu axis: it is 0 at mu = 0 and peaks at mu ~ alpha
    // before falling, so at low roughness the whole feature sits inside the
    // first linear cell. Measured against the independent oracle over the whole
    // domain (worst |white furnace - 1|): linear j/15 nodes 4.2 % for mu >= 0.05
    // and 10 % below it — no better than the cell-centred table — against 0.8 %
    // and 2.4 % for these nodes at the same 16 x 16 size.
    //
    // The node values come from the engine's GgxEnergyNodeValue, the inverse of
    // the GgxEnergyTableCoordinate the lookups use, so the bake and the lookup
    // cannot disagree about where a node sits.
    //
    // mu = 0 cannot be estimated as such: the estimator's weight G2/G1 needs
    // Lambda(mu_v), which is infinite there. It is baked at kMuFloor, the
    // cosine floor GgxSmithLambda already applies, and the lookup places that
    // value at coordinate 0. The limit it stands in for is 1 - Ess -> 0 (at a
    // grazing view every visible facet reflects above the horizon and
    // G2/G1 -> 1); the loss at 1e-4 is 0.012 at the smoothest rows and below
    // 1e-3 from roughness 0.2 up, so the node sits within the table's own
    // interpolation error of that limit.
    constexpr f32 kMuFloor = 1.0e-4f;

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

    // One entry per word: 1 - Ess in the LOW half, the Schlick moment in the
    // HIGH half — matching unpackHalf2x16's (low, high) = (x, y) return on
    // both sides, so one decode yields both terms of one grid node.
    [[nodiscard]] std::vector<u32> PackInterleaved(const std::vector<f64>& low, const std::vector<f64>& high)
    {
        std::vector<u32> words(low.size(), 0u);
        for (sizet i = 0; i < low.size(); ++i)
            words[i] = static_cast<u32>(ToHalfRoundToNearestEven(low[i])) |
                       (static_cast<u32>(ToHalfRoundToNearestEven(high[i])) << 16);
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

    // The lookup-axis prose both files carry — one source, so the two headers
    // cannot describe the grid differently.
    [[nodiscard]] std::string GridConventions(const Options& o, std::string_view lambdaName)
    {
        std::ostringstream out;
        out << "//   * NODE-CENTRED, SQUARE-ROOT SPACED (issue #1478): node j of " << o.Grid << " sits at\n"
            << "//     (j / " << (o.Grid - 1u) << ")^2 on both axes, so a value x has lookup coordinate\n"
            << "//     sqrt(x) * " << (o.Grid - 1u) << ". Both endpoints are nodes: the bilinear lookup never\n"
            << "//     clamps and never extrapolates. Entry index = row (roughness) * " << o.Grid << " + column (mu).\n"
            << "//   * mu = 0 is baked at mu = 1e-4, the cosine floor " << lambdaName << " applies\n"
            << "//     (the estimator's G2/G1 needs a finite Lambda(mu_v)); the true limit there\n"
            << "//     is 1 - Ess -> 0, and the node sits within the table's resolution of it.\n";
        return out.str();
    }

    [[nodiscard]] std::string EmitHeader(const Options& o, const std::vector<u32>& table, const std::vector<u32>& avg)
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
               "// GGX SINGLE-SCATTER ENERGY TABLES — GENERATED, DO NOT HAND-EDIT\n"
               "// =============================================================================\n"
               "//\n"
               "// Emitted by tools/OloGgxEnergyTableGen (issue #998) alongside its GLSL twin\n"
               "// OloEditor/assets/shaders/include/PBRClosureV2Energy.glsl — the SAME packed\n"
               "// words, hex for hex, consumed by the ClosureV2 closure on both sides of the\n"
               "// CPU/GPU parity boundary: its Kulla-Conty multiple-scattering compensation and\n"
               "// its energy-conserving diffuse coupling (ADR 0016 §4).\n"
               "//\n"
               "// Each grid node (mu, r) stores two moments of the SINGLE-scattering GGX\n"
               "// specular lobe, estimated over Heitz-2018 VNDF-sampled half vectors:\n"
               "//\n"
               "//   x: 1 - Ess(mu, r),  Ess     = E[ G2/G1 ]               (the albedo, F == 1)\n"
               "//   y: Schlick(mu, r),  Schlick = E[ G2/G1 (1 - v.h)^5 ]   (its grazing part)\n"
               "//\n"
               "// (the estimator identity f*cos/pdf == F * (G2/G1); see the VNDF block in\n"
               "// PBRCommon.glsl). Together they give the lobe's albedo for ANY Schlick F0,\n"
               "//\n"
               "//   E_ss(mu, F0) = F0 (Ess - Schlick) + Schlick,\n"
               "//\n"
               "// which is what the diffuse coupling subtracts. The LOSS form of Ess is stored\n"
               "// because the compensation consumes (1 - Ess) directly and the near-mirror rows\n"
               "// are ~1e-5, where \"1.0f minus a stored 0.99999f\" would shred float precision.\n"
               "// The averages row stores both moments cosine-averaged over mu,\n"
               "// 2 int M(mu) mu dmu: (1 - E_avg, Schlick_avg).\n"
               "//\n"
               "// STORAGE IS PACKED — one node per u32, as two IEEE-754 halfs — and the\n"
               "// packing is LOAD-BEARING on the GPU side: a plain `const float[256]` in the\n"
               "// GLSL twin passed glslc but failed NVIDIA's GL linker with \"C5025: lvalue in\n"
               "// assignment too complex\" once the lookups were inlined at PBR_MultiLight.glsl's\n"
               "// three lighting call sites (SPIRV-Cross materialises a dynamically-indexed\n"
               "// constant array as a local temporary per site). Both languages therefore\n"
               "// carry the packed words and decode them identically (glm::unpackHalf2x16\n"
               "// here, unpackHalf2x16 in GLSL), so the two sides evaluate the SAME quantized\n"
               "// values. Half quantization costs at most 2^-12 = 2.44e-4 absolute on any\n"
               "// entry (half an ulp of the [0.5, 1) binade) — the generator audits that bound\n"
               "// and refuses to emit a table exceeding it.\n"
               "//\n"
               "// Conventions (must match the v2 closure on both sides):\n"
               "//   * alpha = clamp(r, kMinRoughness, 1)^2 — the v2 perceptual clamp, so each\n"
               "//     row is exactly the albedo of the lobe the v2 sampler samples. Lookups\n"
               "//     take AUTHORED roughness; the rows bake the clamp in.\n"
            << GridConventions(o, "GgxSmithLambda")
            << "//\n"
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
               "// FresnelSchlick / ClosureV2Roughness out of ReferenceBRDF.h, which is what\n"
               "// makes generator-vs-engine estimator drift structurally impossible.\n"
               "// ClosureV2Test recomputes entries with that same sampler and fails if either\n"
               "// copy rots, and parses the GLSL twin so the two files cannot drift apart.\n"
               "// =============================================================================\n"
               "\n"
               "namespace OloEngine::PathTracing\n"
               "{\n"
               "\n"
               "    inline constexpr u32 kGgxEnergyTableSize = "
            << o.Grid
            << ";\n"
               "\n"
               "    // One grid node per word: half(1 - Ess) | half(Schlick) << 16.\n"
               "    // Entry index i = row * "
            << o.Grid << " + column; word = kGgxEnergyPacked[i].\n";
        out << EmitCppArray(table, "kGgxEnergyPacked", 8u);
        out << "\n"
               "    // The averages row, one roughness node per word:\n"
               "    // half(1 - E_avg) | half(Schlick_avg) << 16.\n";
        out << EmitCppArray(avg, "kGgxEnergyAvgPacked", 8u);
        out << "\n"
               "    // Decode one grid node (i in [0, "
            << (o.Grid * o.Grid - 1u)
            << "]): x = 1 - Ess, y = Schlick.\n"
               "    // GLSL twin: ggxEnergyEntry in PBRClosureV2Energy.glsl.\n"
               "    [[nodiscard]] inline glm::vec2 GgxEnergyEntry(u32 i) noexcept\n"
               "    {\n"
               "        return glm::unpackHalf2x16(kGgxEnergyPacked[i]);\n"
               "    }\n"
               "\n"
               "    // Decode one averages node (i in [0, "
            << (o.Grid - 1u)
            << "]): x = 1 - E_avg, y = Schlick_avg.\n"
               "    // GLSL twin: ggxEnergyAvgEntry in PBRClosureV2Energy.glsl.\n"
               "    [[nodiscard]] inline glm::vec2 GgxEnergyAvgEntry(u32 i) noexcept\n"
               "    {\n"
               "        return glm::unpackHalf2x16(kGgxEnergyAvgPacked[i]);\n"
               "    }\n"
               "\n"
               "} // namespace OloEngine::PathTracing\n";
        return out.str();
    }

    [[nodiscard]] std::string EmitGlsl(const Options& o, const std::vector<u32>& table, const std::vector<u32>& avg)
    {
        const sizet tableVectors = table.size() / 4u;
        const sizet avgVectors = avg.size() / 4u;
        std::ostringstream out;
        out << "// =============================================================================\n"
               "// GGX SINGLE-SCATTER ENERGY TABLES — GENERATED, DO NOT HAND-EDIT\n"
               "// =============================================================================\n"
               "//\n"
               "// Emitted by tools/OloGgxEnergyTableGen (issue #998). The command line that\n"
               "// reproduces this file, and the reason regeneration is a tool run rather than a\n"
               "// prose recipe, live in the REGENERATION block of its C++ twin\n"
               "// OloEngine/src/OloEngine/Renderer/PathTracing/GgxEnergyTables.h and in\n"
               "// ADR 0016 §6.\n"
               "//\n"
               "// Data for PBR closure v2's Kulla-Conty multiple-scattering energy compensation\n"
               "// (Kulla & Conty, \"Revisiting Physically Based Shading at Imageworks\", 2017)\n"
               "// and its energy-conserving diffuse coupling (issue #1479).\n"
               "//\n"
               "// Each grid node (mu, r) stores two moments of the SINGLE-scattering GGX\n"
               "// specular lobe, over Heitz-2018 VNDF-sampled half vectors:\n"
               "//\n"
               "//   x: 1 - Ess(mu, r),  Ess     = E[ G2/G1 ]              (the albedo, F == 1)\n"
               "//   y: Schlick(mu, r),  Schlick = E[ G2/G1 (1 - v.h)^5 ]  (its grazing part)\n"
               "//\n"
               "// the exact estimator identity f*cos/pdf == F * (G2/G1) (see the VNDF block in\n"
               "// PBRCommon.glsl). Together they give the lobe's albedo for any Schlick F0,\n"
               "// E_ss(mu, F0) = F0 (Ess - Schlick) + Schlick. The LOSS form of Ess is stored\n"
               "// because the compensation consumes (1 - Ess) directly and the near-mirror\n"
               "// rows are ~1e-5, where \"1.0 minus a stored 0.99999\" would shred float\n"
               "// precision. The averages row holds (1 - E_avg, Schlick_avg), each moment\n"
               "// cosine-averaged over mu.\n"
               "//\n"
               "// STORAGE IS PACKED, AND THE PACKING IS LOAD-BEARING. One node per uint (two\n"
               "// IEEE-754 half floats), four uints per uvec4 — "
            << (tableVectors + avgVectors) << " uvec4 constants for "
            << 2u * (o.Grid * o.Grid + o.Grid)
            << "\n"
               "// scalars. A plain `const float[256]` here LINKED FINE through glslc but\n"
               "// FAILED AT RUNTIME on NVIDIA GL (\"error C5025: lvalue in assignment too\n"
               "// complex\"): SPIRV-Cross materialises a dynamically-indexed constant array as\n"
               "// a function-local temporary copy, and once the lookups were inlined at the\n"
               "// three lighting call sites of a large shader (PBR_MultiLight.glsl) the\n"
               "// driver's complexity limit tripped — while single-call-site probe shaders\n"
               "// compiled the very same array without complaint. Packing keeps the emitted\n"
               "// element count at an eighth of the scalar count. See glsl-shaders.md §12.\n"
               "//\n"
               "// Half precision costs at most 2^-12 = 2.44e-4 absolute on any entry — the\n"
               "// generator audits that bound and refuses to emit a table exceeding it.\n"
               "//\n"
               "// Conventions (must match the v2 closure on both sides of the parity boundary):\n"
               "//   * alpha = clamp(r, MIN_ROUGHNESS, 1)^2 — the v2 perceptual clamp, so each\n"
               "//     row is exactly the albedo of the lobe the v2 sampler samples. Lookups\n"
               "//     take AUTHORED roughness; the rows bake the clamp in.\n"
            << GridConventions(o, "ggxSmithLambda")
            << "//   * Estimator: "
            << o.Samples
            << " deterministic Hammersley points per node; the averages use\n"
               "//     a "
            << o.AvgPoints << "-point midpoint quadrature over mu at " << o.AvgSamples
            << " points per evaluation.\n"
               "//\n"
               "// The C++ twin is OloEngine/src/OloEngine/Renderer/PathTracing/GgxEnergyTables.h\n"
               "// — the SAME packed words, decoded with glm::unpackHalf2x16, so the two sides\n"
               "// evaluate identical quantized values. ClosureV2Test pins both files against\n"
               "// the estimator and against each other; the GPU parity probe covers the full\n"
               "// closure.\n"
               "// =============================================================================\n"
               "#ifndef PBR_CLOSURE_V2_ENERGY_GLSL\n"
               "#define PBR_CLOSURE_V2_ENERGY_GLSL\n"
               "\n"
               "#define OLO_GGX_ENERGY_TABLE_SIZE "
            << o.Grid
            << "\n"
               "\n"
               "// One grid node per word: half(1 - Ess) | half(Schlick) << 16.\n"
               "// Entry index i = row * "
            << o.Grid
            << " + column; word = kGgxEnergyPacked[i >> 2][i & 3].\n";
        out << EmitGlslArray(table, "kGgxEnergyPacked");
        out << "\n"
               "// The averages row, one roughness node per word:\n"
               "// half(1 - E_avg) | half(Schlick_avg) << 16.\n";
        out << EmitGlslArray(avg, "kGgxEnergyAvgPacked");
        out << "\n"
               "// Decode one grid node (i in [0, "
            << (o.Grid * o.Grid - 1u)
            << "]): x = 1 - Ess, y = Schlick.\n"
               "vec2 ggxEnergyEntry(int i)\n"
               "{\n"
               "    return unpackHalf2x16(kGgxEnergyPacked[i >> 2][i & 3]);\n"
               "}\n"
               "\n"
               "// Decode one averages node (i in [0, "
            << (o.Grid - 1u)
            << "]): x = 1 - E_avg, y = Schlick_avg.\n"
               "vec2 ggxEnergyAvgEntry(int i)\n"
               "{\n"
               "    return unpackHalf2x16(kGgxEnergyAvgPacked[i >> 2][i & 3]);\n"
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
                     "  --grid N           table edge length, a multiple of 4 (default 16; also needs\n"
                     "                     ClosureV2Test's twin-drift pin updated, which hardcodes the sizes)\n"
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

    // The GLSL twin packs four nodes per uvec4 and its decode helpers shift
    // by 2, so a grid whose averages row does not divide into whole uvec4s would
    // emit a table the shader cannot index. Refuse rather than emit something
    // subtly wrong — this is the flag a future widening reaches for first.
    if (options.Grid % 4u != 0u)
    {
        std::cerr << "error: --grid must be a multiple of 4 (the GLSL twin packs 4 nodes per uvec4); got "
                  << options.Grid << "\n";
        return 2;
    }

    if (options.HeaderPath.empty())
        options.HeaderPath = options.RepoRoot / "OloEngine/src/OloEngine/Renderer/PathTracing/GgxEnergyTables.h";
    if (options.GlslPath.empty())
        options.GlslPath = options.RepoRoot / "OloEditor/assets/shaders/include/PBRClosureV2Energy.glsl";

    // ---- bake ---------------------------------------------------------------
    const sizet nodes = static_cast<sizet>(options.Grid) * options.Grid;
    std::vector<f64> loss(nodes);
    std::vector<f64> schlick(nodes);
    for (u32 row = 0; row < options.Grid; ++row)
    {
        const f32 roughness = GgxEnergyNodeValue(row, options.Grid);
        for (u32 col = 0; col < options.Grid; ++col)
        {
            const f32 mu = std::max(GgxEnergyNodeValue(col, options.Grid), kMuFloor);
            const Moments m = EstimateMoments(mu, roughness, options.Samples);
            loss[static_cast<sizet>(row) * options.Grid + col] = 1.0 - m.Ess;
            schlick[static_cast<sizet>(row) * options.Grid + col] = m.Schlick;
        }
    }

    std::vector<f64> lossAvg(options.Grid);
    std::vector<f64> schlickAvg(options.Grid);
    for (u32 row = 0; row < options.Grid; ++row)
    {
        const Moments m =
            EstimateAverages(GgxEnergyNodeValue(row, options.Grid), options.AvgPoints, options.AvgSamples);
        lossAvg[row] = 1.0 - m.Ess;
        schlickAvg[row] = m.Schlick;
    }

    const std::vector<u32> tableWords = PackInterleaved(loss, schlick);
    const std::vector<u32> avgWords = PackInterleaved(lossAvg, schlickAvg);

    // ---- audit the quantization claim the emitted comments make -------------
    f64 worstError = 0.0;
    const auto audit = [&worstError](const std::vector<f64>& values)
    {
        for (const f64 v : values)
            worstError = std::max(worstError, std::abs(v - FromHalf(ToHalfRoundToNearestEven(v))));
    };
    audit(loss);
    audit(schlick);
    audit(lossAvg);
    audit(schlickAvg);
    // Half an ulp of the [0.5, 1) binade: every stored value is in [0, 1).
    constexpr f64 kQuantizationBudget = 1.0 / 4096.0;
    std::cout << "half quantization: worst absolute error " << worstError << " (budget " << kQuantizationBudget
              << ")\n";
    if (worstError > kQuantizationBudget)
    {
        std::cerr << "error: quantization error exceeds the budget the generated comments claim.\n"
                     "       Either the grid moved into a regime half cannot hold, or the claim needs revising.\n";
        return 1;
    }

    const std::string header = EmitHeader(options, tableWords, avgWords);
    const std::string glsl = EmitGlsl(options, tableWords, avgWords);

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
