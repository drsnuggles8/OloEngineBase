// OLO_TEST_LAYER: L1
// =============================================================================
// ClosureV2Test.cpp — contract tests for PBR closure v2 (issue #975, ADR 0016).
//
// The v2 closure's whole pitch is three properties Legacy structurally cannot
// have: ONE D serving Evaluate/Sample/Pdf (alpha clamp instead of denominator
// clamp), a near-mirror lobe that narrows and brightens instead of collapsing,
// and Kulla-Conty multiple-scattering energy compensation that closes the white
// furnace. Each is pinned here, headless, with its PAIRED NEGATIVE where the
// acceptance criterion demands one ("a furnace test that fails without the
// compensation", "a near-mirror test that fails against today's code").
//
// This file is also the "ClosureV2EnergyTableTest" the generated table pair
// refers to (GgxEnergyTables.h / include/PBRClosureV2Energy.glsl): the first
// two tests are the anti-rot pin that recomputes table entries with the
// engine's own sampler, and the drift pin that parses the GLSL twin's literals
// against the C++ constants. The GPU half of the story — the compiled shader
// against the C++ twins — lives in ReferenceBRDFGpuParityTest; neither
// subsumes the other, same split as ReferenceBRDFTest.
//
// Everything below is deterministic: Hammersley point sets and midpoint
// quadrature grids, no RNG, so every tolerance is a fixed number rather than a
// flaky one.
//
// Classification: L1 (pure CPU math plus a text read of the GLSL twin; no GL).
// =============================================================================

#include "OloEnginePCH.h"

#include "Rendering/ShaderHarness.h"

#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/GgxEnergyTables.h"
#include "OloEngine/Renderer/PathTracing/PBRClosureBSDF.h"
#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;

    namespace
    {
        // Deterministic stratified 2D point set (Hammersley) — the same
        // 32-bit-reverse construction ReferenceBRDFTest uses, so every
        // estimate below is reproducible and its error is a fixed number.
        [[nodiscard]] glm::vec2 Hammersley(u32 i, u32 n)
        {
            return glm::vec2(static_cast<f32>(i) / static_cast<f32>(n),
                             SamplerDetail::ToUnitFloat(SamplerDetail::ReverseBits(i)));
        }

        // Unit direction from a polar cosine and an azimuth, z-up.
        [[nodiscard]] glm::vec3 DirectionFrom(f32 cosTheta, f32 phi)
        {
            const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            return glm::vec3(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);
        }

        // Ess(mu, r): the directional albedo of the single-scattering GGX lobe
        // with F == 1, estimated with the ENGINE'S OWN sampler and weight — the
        // estimator identity f*cos/pdf == F * (G2/G1) with the height-correlated
        // Smith G2 and the VNDF's G1 (see GgxEnergyTables.h's regeneration
        // comment and the VNDF block in PBRCommon.glsl). A below-horizon
        // reflection scores 0 but still divides by N, exactly as the table
        // generator counts it.
        [[nodiscard]] f64 EstimateEss(f32 mu, f32 roughness, u32 sampleCount)
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
                    continue; // scores zero; the division by sampleCount below still counts it

                const f32 lambdaL = GgxSmithLambda(wi.z, alpha);
                sum += static_cast<f64>((1.0f + lambdaV) / (1.0f + lambdaV + lambdaL));
            }
            return sum / static_cast<f64>(sampleCount);
        }

        // Uniform-hemisphere Monte Carlo estimate of the LEGACY directional
        // albedo — the same estimator ReferenceBRDFTest's FurnaceIntegral runs
        // (and PbrFurnaceProbe.glsl on the GPU), generalised to an off-normal
        // view direction so it can be compared against the v2 estimate at the
        // same mu_o.
        [[nodiscard]] f64 LegacyFurnaceIntegral(f32 roughness, f32 metallic, const glm::vec3& albedo, f32 muO,
                                                u32 sampleCount)
        {
            const glm::vec3 n(0.0f, 0.0f, 1.0f);
            const f32 sinO = std::sqrt(std::max(0.0f, 1.0f - muO * muO));
            const glm::vec3 v(sinO, 0.0f, muO);

            f64 sum = 0.0;
            for (u32 i = 0; i < sampleCount; ++i)
            {
                const glm::vec2 xi = Hammersley(i, sampleCount);
                const f32 cosTheta = xi.x;
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                const f32 phi = kTwoPi * xi.y;
                const glm::vec3 l(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);

                const glm::vec3 f = CookTorranceBRDF(n, v, l, albedo, metallic, roughness);
                sum += static_cast<f64>((f.x + f.y + f.z) / 3.0f) * static_cast<f64>(cosTheta) * static_cast<f64>(kTwoPi);
            }
            return sum / static_cast<f64>(sampleCount);
        }

        // Pull the `0x????????u` hex words out of a GLSL packed-table
        // declaration. The marker anchors the search at the declaration itself
        // (with its element count), so a mention of the array name in a
        // comment can never leak words into the parse; the terminator is the
        // constructor's closing `);` because uvec4(...) entries carry interior
        // parentheses of their own.
        [[nodiscard]] std::vector<u32> ParseGlslPackedArray(const std::string& src, std::string_view marker)
        {
            std::vector<u32> out;
            const sizet decl = src.find(marker);
            if (decl == std::string::npos)
                return out;
            const sizet close = src.find(");", decl);
            if (close == std::string::npos)
                return out;

            sizet cursor = decl;
            while (true)
            {
                const sizet hex = src.find("0x", cursor);
                if (hex == std::string::npos || hex >= close)
                    break;
                out.push_back(static_cast<u32>(std::stoul(src.substr(hex + 2, 8), nullptr, 16)));
                cursor = hex + 2;
            }
            return out;
        }
    } // namespace

    // =========================================================================
    // 1. The anti-rot pin for the generated table.
    //
    // Recompute a subset of kGgxEnergyLoss entries with the engine's own
    // functions (SampleGGXVNDFTangent + GgxSmithLambda), on the table's own
    // conventions: cell-centered grid, alpha = ClosureV2Roughness(r)^2, 4096
    // deterministic Hammersley samples. If someone regenerates the table with a
    // different convention (unclamped alpha, node-centered grid, Ess stored
    // instead of loss, rows and columns swapped) — or edits an entry by hand —
    // this recomputation disagrees.
    // =========================================================================
    TEST(ClosureV2, EnergyTablesMatchTheirOwnEstimator)
    {
        constexpr u32 kSamples = 4096;

        // Every 3rd row/column — {0, 3, 6, 9, 12, 15} — which also covers all
        // four corner cells (0,0), (0,15), (15,0), (15,15), the places a
        // clamping or off-by-one regeneration bug lands first.
        for (u32 row = 0; row < kGgxEnergyTableSize; row += 3)
        {
            const f32 rCell = (static_cast<f32>(row) + 0.5f) / static_cast<f32>(kGgxEnergyTableSize);
            for (u32 col = 0; col < kGgxEnergyTableSize; col += 3)
            {
                const f32 muCell = (static_cast<f32>(col) + 0.5f) / static_cast<f32>(kGgxEnergyTableSize);

                const f64 recomputed = EstimateEss(muCell, rCell, kSamples);
                const f64 stored = 1.0 - static_cast<f64>(GgxEnergyLossEntry(row * kGgxEnergyTableSize + col));

                // 2e-3 separates a rotted or wrong-convention table (a missing
                // alpha clamp, swapped axes, Ess-instead-of-loss are all off at
                // the percent level or worse) from the residual difference
                // between two 4096-sample Hammersley estimates of the integral.
                EXPECT_NEAR(recomputed, stored, 2e-3)
                    << "table row (roughness) = " << row << " (r = " << rCell << "), "
                    << "column (mu) = " << col << " (mu = " << muCell << ")\n"
                    << "kGgxEnergyLoss no longer matches what the engine's own VNDF sampler measures.\n"
                    << "Either the table was regenerated with different conventions or one side of the\n"
                    << "estimator changed — see the regeneration comment in GgxEnergyTables.h.";
            }
        }
    }

    // =========================================================================
    // 2. The GLSL twin carries the SAME numbers.
    //
    // GgxEnergyTables.h and include/PBRClosureV2Energy.glsl are one generated
    // artifact emitted into two languages (ADR 0016 §4). Nothing at runtime
    // compares them — the CPU reads one, the GPU the other — so a partial
    // regeneration (one file updated, the other forgotten) would silently split
    // the CPU/GPU compensation. Parse the GLSL as text, the same way
    // ReferenceBRDF.PbrCommonKeepsTheAlphaConventionAndTheVisibilityMultiply
    // reads PBRCommon.glsl, and demand literal-for-literal agreement.
    // =========================================================================
    TEST(ClosureV2, EnergyTablesGlslTwinCarriesTheSameNumbers)
    {
        const auto root = Tests::ShaderHarness::ResolveShaderRoot();
        ASSERT_FALSE(root.empty()) << "could not resolve the shader root";
        const std::string src = Tests::ShaderHarness::ReadWholeFile(root / "include" / "PBRClosureV2Energy.glsl");
        ASSERT_FALSE(src.empty()) << "PBRClosureV2Energy.glsl is missing or unreadable";

        // The GLSL side's size macro must agree with kGgxEnergyTableSize — the
        // bilinear lookups on both sides bake the 16 in.
        EXPECT_NE(src.find("#define OLO_GGX_ENERGY_TABLE_SIZE 16"), std::string::npos)
            << "the GLSL table-size macro changed (or moved) without this pin being updated — the "
               "C++ side still indexes a 16x16 grid";

        const std::vector<u32> loss = ParseGlslPackedArray(src, "const uvec4 kGgxEnergyLossPacked[32]");
        const std::vector<u32> lossAvg = ParseGlslPackedArray(src, "const uvec4 kGgxEnergyLossAvgPacked[2]");

        // 128 + 8 packed words is the full generated payload; a miscount means
        // the parse anchored on the wrong text or the arrays were resized on
        // one side.
        ASSERT_EQ(loss.size(), 128u) << "kGgxEnergyLossPacked in the GLSL twin no longer holds 128 words";
        ASSERT_EQ(lossAvg.size(), 8u) << "kGgxEnergyLossAvgPacked in the GLSL twin no longer holds 8 words";

        // EXACT integer equality: both sides are meant to be the SAME hex
        // words (the half-packed table is the one generated artifact emitted
        // into two languages), so any inequality is a real divergence, never
        // floating-point noise. The packing itself is load-bearing on the GPU
        // side — see PBRClosureV2Energy.glsl's header (NVIDIA C5025).
        for (u32 i = 0; i < static_cast<u32>(loss.size()); ++i)
        {
            EXPECT_EQ(loss[i], kGgxEnergyLossPacked[i])
                << "kGgxEnergyLossPacked word " << i << " differs between PBRClosureV2Energy.glsl and "
                << "GgxEnergyTables.h — the two generated files have drifted; regenerate BOTH.";
        }
        for (u32 i = 0; i < static_cast<u32>(lossAvg.size()); ++i)
        {
            EXPECT_EQ(lossAvg[i], kGgxEnergyLossAvgPacked[i])
                << "kGgxEnergyLossAvgPacked word " << i << " differs between PBRClosureV2Energy.glsl and "
                << "GgxEnergyTables.h — the two generated files have drifted; regenerate BOTH.";
        }
    }

    // =========================================================================
    // 3. The white furnace closes WITH the compensation and fails WITHOUT it —
    //    the issue-#975 acceptance criterion, with its paired negative.
    //
    // metallic = 1, albedo = white, so F0 = 1, the diffuse term is exactly zero
    // and the full ClosureV2Evaluate IS the specular lobe. Analytically (ADR
    // 0016 §4): the single-scatter integral is Ess(mu_o), the multi-scatter
    // integral at F_avg = 1 is exactly 1 - Ess(mu_o), so the sum closes to 1 —
    // up to the 16x16 table's bilinear interpolation error and the quadrature's
    // discretization error.
    //
    // The roughness grid starts at 0.4 ON PURPOSE: below that the specular peak
    // narrows toward the uniform quadrature's angular resolution and the
    // integral gets noisy — the near-mirror regime is covered by
    // NearMirrorLobeNoLongerCollapses below with VNDF sampling instead.
    // =========================================================================
    TEST(ClosureV2, FurnaceClosesWithCompensationAndFailsWithoutIt)
    {
        // 64 x 64 midpoint product grid over (cos theta, phi) — deterministic
        // uniform solid-angle quadrature of the hemisphere.
        constexpr u32 kCosSteps = 64;
        constexpr u32 kPhiSteps = 64;

        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        const glm::vec3 white(1.0f);

        for (const f32 roughness : { 0.4f, 0.6f, 0.8f, 1.0f })
        {
            const f32 rClamped = ClosureV2Roughness(roughness);
            for (const f32 muO : { 0.3f, 0.6f, 0.9f })
            {
                const f32 sinO = std::sqrt(std::max(0.0f, 1.0f - muO * muO));
                const glm::vec3 v(sinO, 0.0f, muO);

                f64 full = 0.0;
                f64 msOnly = 0.0;
                for (u32 ci = 0; ci < kCosSteps; ++ci)
                {
                    const f32 cosTheta = (static_cast<f32>(ci) + 0.5f) / static_cast<f32>(kCosSteps);
                    const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                    for (u32 pi = 0; pi < kPhiSteps; ++pi)
                    {
                        const f32 phi = kTwoPi * (static_cast<f32>(pi) + 0.5f) / static_cast<f32>(kPhiSteps);
                        const glm::vec3 l(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);

                        const glm::vec3 f = ClosureV2Evaluate(n, v, l, white, 1.0f, roughness);
                        full += static_cast<f64>((f.x + f.y + f.z) / 3.0f) * static_cast<f64>(cosTheta);

                        // The exact compensation term Evaluate added for this
                        // direction: same clamped roughness, and dot(n, v) ==
                        // muO / dot(n, l) == cosTheta exactly for these
                        // axis-aligned constructions.
                        const glm::vec3 ms = ClosureV2MultiScatter(muO, cosTheta, rClamped, white);
                        msOnly += static_cast<f64>((ms.x + ms.y + ms.z) / 3.0f) * static_cast<f64>(cosTheta);
                    }
                }

                const f64 cellWeight = static_cast<f64>(kTwoPi) / static_cast<f64>(kCosSteps * kPhiSteps);
                full *= cellWeight;
                msOnly *= cellWeight;

                // [0.95, 1.05] separates a missing/mis-scaled compensation term
                // (>= 10% low at these roughnesses, see the negative below) and
                // an energy-creating one from the ~1-2% of table interpolation
                // plus quadrature discretization error.
                EXPECT_GE(full, 0.95) << "roughness = " << roughness << ", mu_o = " << muO
                                      << " — the compensated v2 furnace no longer closes; the "
                                         "multiple-scattering term went missing or lost its scale";
                EXPECT_LE(full, 1.05) << "roughness = " << roughness << ", mu_o = " << muO
                                      << " — the v2 closure is CREATING energy";

                // PAIRED NEGATIVE: the same integral without the compensation
                // term. < 0.90 proves the term carries >= 10% of the energy
                // here — i.e. the furnace test above genuinely FAILS without
                // it, so the compensation is load-bearing, not decorative.
                if (roughness >= 0.6f)
                {
                    const f64 singleScatterOnly = full - msOnly;
                    EXPECT_LT(singleScatterOnly, 0.90)
                        << "roughness = " << roughness << ", mu_o = " << muO
                        << " — the single-scatter lobe alone already closes the furnace, which means "
                           "the compensated assertion above could not fail without the compensation. "
                           "Either the energy tables collapsed toward zero loss or this test's "
                           "subtraction no longer matches what ClosureV2Evaluate adds.";
                }
            }
        }
    }

    // =========================================================================
    // 4. The near-mirror lobe no longer collapses — the second issue-#975
    //    acceptance criterion, with the "fails against today's code" negative.
    //
    // (a) energy: v2's directional albedo at roughness 0.05 stays ~1, while
    //     the Legacy closure at the SAME configuration collapses (the docs
    //     measured 0.035 at roughness 0.1, metallic 1 — the EPSILON denominator
    //     clamp caps D over the whole lobe).
    // (b) shape: the v2 sampling-density D at the peak is a narrow bright spike
    //     even at AUTHORED roughness 0, while the Legacy D is capped at
    //     a2^2 / EPSILON. ReferenceBRDF.GgxNdfPeakIsEpsilonClampedAtLowRoughness
    //     is the sibling pin asserting that Legacy clamp is PRESERVED; this
    //     test asserts the CONTRAST — v2 must not inherit it.
    // =========================================================================
    TEST(ClosureV2, NearMirrorLobeNoLongerCollapses)
    {
        constexpr u32 kSamples = 4096;
        constexpr f32 kRoughness = 0.05f;
        constexpr f32 kMuO = 0.7f;

        // (a) v2 directional albedo via VNDF sampling. With F == 1 (white
        // metallic) the estimator f*cos/pdf collapses to E[G2/G1] == Ess, so
        // the mean weight IS the albedo.
        const f64 v2Albedo = EstimateEss(kMuO, kRoughness, kSamples);
        // 0.97 separates the near-unity v2 near-mirror albedo (~0.9995) from
        // any regression toward the Legacy collapse (~0.03); stratified noise
        // at 4096 samples is orders of magnitude below the margin.
        EXPECT_GE(v2Albedo, 0.97)
            << "the v2 near-mirror specular lobe went dark again — the exact defect ClosureV2 exists "
               "to fix (an alpha clamp must bound the lobe, never a denominator clamp)";

        // PAIRED NEGATIVE — this half FAILS against today's Legacy closure by
        // construction: same estimator family, same roughness/metallic/mu_o,
        // Legacy CookTorranceBRDF instead of the v2 closure.
        const f64 legacyAlbedo = LegacyFurnaceIntegral(kRoughness, 1.0f, glm::vec3(1.0f), kMuO, kSamples);
        // 0.2 separates the EPSILON-clamped Legacy collapse (~0.03 measured)
        // from anything resembling a preserved mirror lobe; if this fires the
        // Legacy closure changed, which the bit-identical render pins forbid.
        EXPECT_LT(legacyAlbedo, 0.2)
            << "the LEGACY closure no longer collapses at near-mirror roughness — Legacy is frozen "
               "(PathTracerCornellBoxTest pins its renders bit-for-bit), so something rewrote it";
        EXPECT_GT(v2Albedo, legacyAlbedo)
            << "v2 must carry MORE energy than the collapsed Legacy lobe at near-mirror roughness";

        // (b) peak NDF contrast at the SAME configuration (n = h = +Z).
        const f32 v2Peak = DistributionGGXSamplingDensity(1.0f, ClosureV2Roughness(0.0f));
        // 1e4 separates the true unclamped peak at the v2 roughness floor
        // (1 / (pi * 0.04^4) ~ 1.2e5) from any re-introduced value clamp.
        EXPECT_GT(v2Peak, 1.0e4f)
            << "the v2 D no longer spikes at authored roughness 0 — the alpha clamp "
               "(ClosureV2Roughness) was replaced or a denominator clamp crept back in";

        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        const glm::vec3 h(0.0f, 0.0f, 1.0f);
        const f32 legacyPeak = DistributionGGX(n, h, 0.04f);
        // 100 separates the EPSILON-capped Legacy peak (a2^2 / 1e-4 ~ 0.026)
        // from the unclamped ~1.2e5 — the same fact
        // ReferenceBRDF.GgxNdfPeakIsEpsilonClampedAtLowRoughness pins as an
        // identity; here it is the negative half of the contrast.
        EXPECT_LT(legacyPeak, 100.0f)
            << "the Legacy D lost its EPSILON clamp — that clamp is shipped behaviour and the "
               "reference must reproduce it (see ReferenceBRDF.h's fidelity notes)";
    }

    // =========================================================================
    // 5. Reciprocity: f(v, l) == f(l, v).
    //
    // Why this holds exactly (up to fp reassociation): F is evaluated at the
    // HALF vector, and for unit v, l the half vector makes equal angles with
    // both (dot(h, v) == dot(h, l)); the height-correlated V is symmetric in
    // (NdotV, NdotL); the Kulla-Conty term is symmetric by construction
    // (lossV * lossL); and the diffuse term depends on v, l only through that
    // same F(h). ADR 0016 §5 records reciprocity as a v2 design property —
    // Legacy is deliberately NOT held to it.
    // =========================================================================
    TEST(ClosureV2, ClosureV2IsReciprocal)
    {
        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        // Non-gray albedo so a per-channel asymmetry cannot hide in a gray
        // average.
        const glm::vec3 albedo(0.8f, 0.4f, 0.2f);

        struct DirPair
        {
            f32 CosV;
            f32 PhiV;
            f32 CosL;
            f32 PhiL;
        };
        // Both directions above the horizon; azimuth separations avoid the
        // near-opposite-grazing corner where kD -> 0 would turn last-ulp
        // Fresnel noise into large RELATIVE noise (that corner is covered by
        // the edge-case test below, as a finiteness claim).
        constexpr DirPair kPairs[] = {
            { 0.9f, 0.0f, 0.4f, 2.0f },
            { 0.6f, 1.0f, 0.8f, 4.0f },
            { 0.25f, 0.5f, 0.7f, 3.6f },
            { 0.5f, 5.5f, 0.5f, 2.5f },
        };

        for (const f32 roughness : { 0.1f, 0.5f, 1.0f })
        {
            for (const f32 metallic : { 0.0f, 0.5f, 1.0f })
            {
                for (const auto& pair : kPairs)
                {
                    const glm::vec3 v = DirectionFrom(pair.CosV, pair.PhiV);
                    const glm::vec3 l = DirectionFrom(pair.CosL, pair.PhiL);

                    const glm::vec3 forward = ClosureV2Evaluate(n, v, l, albedo, metallic, roughness);
                    const glm::vec3 reversed = ClosureV2Evaluate(n, l, v, albedo, metallic, roughness);

                    for (glm::length_t c = 0; c < 3; ++c)
                    {
                        // 1e-5 relative separates a genuinely one-sided term (a
                        // Fresnel moved to NdotV, an asymmetric energy lookup)
                        // from the last-ulp reassociation of swapping the
                        // arguments. The 1e-8 floor only guards the division.
                        const f32 relative = std::abs(forward[c] - reversed[c]) /
                                             std::max({ std::abs(forward[c]), std::abs(reversed[c]), 1e-8f });
                        EXPECT_LT(relative, 1e-5f)
                            << "roughness = " << roughness << ", metallic = " << metallic << ", channel " << c
                            << ": ClosureV2Evaluate(n, v, l) != ClosureV2Evaluate(n, l, v).\n"
                            << "The v2 closure is reciprocal by construction (ADR 0016) — a one-sided "
                               "term crept in.";
                    }
                }
            }
        }
    }

    // =========================================================================
    // 6. Deterministic edge cases: the closure and its density must never emit
    //    a NaN/Inf or a negative value, because one poisoned sample propagates
    //    through the integrator's running mean and ruins the whole pixel — the
    //    failure is an image artifact, never a test assertion, unless pinned
    //    here.
    // =========================================================================
    TEST(ClosureV2, EvaluateIsFiniteAndNonNegativeAtEdges)
    {
        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        const glm::vec3 albedo(0.9f, 0.8f, 0.7f);
        constexpr f32 kGrazeZ = 1.0e-4f; // nDot ~ 1e-4: inside every cosine clamp's danger zone

        const glm::vec3 vGraze = DirectionFrom(kGrazeZ, 0.3f);
        const glm::vec3 lGraze = DirectionFrom(kGrazeZ, 2.4f);
        const glm::vec3 up = n;
        const glm::vec3 tilted = DirectionFrom(0.8f, 1.3f);
        // v nearly == -l: the x/y components cancel, so h = normalize(v + l)
        // survives only on the tiny 2 * kGrazeZ z-component — the degenerate-
        // half-vector construction (v + l can never be exactly zero with both
        // directions above the horizon).
        const glm::vec3 lMirrored(-vGraze.x, -vGraze.y, vGraze.z);
        const glm::vec3 lNearOpposite = glm::normalize(glm::vec3(-vGraze.x + 1.0e-5f, -vGraze.y, vGraze.z));

        struct Config
        {
            glm::vec3 V;
            glm::vec3 L;
            const char* Name;
        };
        const Config kConfigs[] = {
            { vGraze, lGraze, "v and l both grazing" },
            { vGraze, up, "grazing v, head-on l" },
            { up, up, "v == l == n" },
            { tilted, tilted, "v == l, tilted" },
            { vGraze, lMirrored, "v ~ -l, exact azimuth mirror (h degenerate)" },
            { vGraze, lNearOpposite, "v ~ -l, perturbed (h nearly degenerate)" },
        };

        for (const f32 roughness : { 0.0f, 0.04f, 1.0f })
        {
            for (const f32 metallic : { 0.0f, 1.0f })
            {
                for (const auto& config : kConfigs)
                {
                    const glm::vec3 f = ClosureV2Evaluate(n, config.V, config.L, albedo, metallic, roughness);
                    for (glm::length_t c = 0; c < 3; ++c)
                    {
                        // Finiteness separates a division by an unguarded ~zero
                        // (the degenerate h, a grazing cosine) from the huge-
                        // but-finite values a narrow lobe legitimately produces.
                        EXPECT_TRUE(std::isfinite(f[c]))
                            << config.Name << ": roughness = " << roughness << ", metallic = " << metallic
                            << ", channel " << c << " is not finite";
                        // >= 0: a negative BRDF value would DARKEN the pixel it
                        // lands in — a mis-signed term, invisible in any energy
                        // total.
                        EXPECT_GE(f[c], 0.0f)
                            << config.Name << ": roughness = " << roughness << ", metallic = " << metallic
                            << ", channel " << c << " went negative";
                    }

                    ReferenceMaterial material;
                    material.BaseColor = albedo;
                    material.Metallic = metallic;
                    material.Roughness = roughness;
                    material.Model = PBRModel::ClosureV2;
                    const f32 pdf = BSDF::Pdf(material, n, config.V, config.L);
                    // The density the integrator divides by and MIS weights
                    // with: a NaN here corrupts every estimator downstream, a
                    // negative one flips a MIS weight's sign.
                    EXPECT_TRUE(std::isfinite(pdf))
                        << config.Name << ": roughness = " << roughness << ", metallic = " << metallic
                        << " — BSDF::Pdf is not finite";
                    EXPECT_GE(pdf, 0.0f) << config.Name << ": roughness = " << roughness
                                         << ", metallic = " << metallic << " — BSDF::Pdf went negative";
                }
            }
        }
    }

    // =========================================================================
    // 7. The Legacy dispatch is untouched, and the v2 dispatch is live.
    //
    // The #975 contract froze Legacy bit-for-bit (the Cornell-box render pins
    // depend on the float sequence not changing), so BSDF::Evaluate on a Legacy
    // material — explicit OR default-constructed, since Legacy IS the
    // constructor default per ADR 0016 §1 — must be the exact same computation
    // as a direct CookTorranceBRDF call. And a ClosureV2 material must actually
    // reach the other branch, or the whole feature is a dead enum.
    // =========================================================================
    TEST(ClosureV2, LegacyDispatchIsUntouched)
    {
        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        const glm::vec3 albedo(0.7f, 0.5f, 0.3f);
        constexpr f32 kMetallic = 0.25f;
        constexpr f32 kRoughness = 0.45f;

        ReferenceMaterial explicitLegacy;
        explicitLegacy.BaseColor = albedo;
        explicitLegacy.Metallic = kMetallic;
        explicitLegacy.Roughness = kRoughness;
        explicitLegacy.Model = PBRModel::Legacy;

        ReferenceMaterial defaulted;
        defaulted.BaseColor = albedo;
        defaulted.Metallic = kMetallic;
        defaulted.Roughness = kRoughness;
        // Model deliberately untouched: the constructor default must BE Legacy
        // (ADR 0016 §1 — flipping any default is a separate, deliberate
        // decision with a golden rebake attached, never a drive-by).

        struct DirPair
        {
            f32 CosV;
            f32 PhiV;
            f32 CosL;
            f32 PhiL;
        };
        constexpr DirPair kPairs[] = {
            { 0.9f, 0.0f, 0.4f, 2.0f },
            { 0.3f, 1.5f, 0.75f, 4.2f },
            { 0.55f, 3.0f, 0.55f, 0.7f },
        };

        for (const auto& pair : kPairs)
        {
            const glm::vec3 v = DirectionFrom(pair.CosV, pair.PhiV);
            const glm::vec3 l = DirectionFrom(pair.CosL, pair.PhiL);

            const glm::vec3 direct = CookTorranceBRDF(n, v, l, albedo, kMetallic, kRoughness);
            const glm::vec3 viaExplicit = BSDF::Evaluate(explicitLegacy, n, v, l);
            const glm::vec3 viaDefault = BSDF::Evaluate(defaulted, n, v, l);

            for (glm::length_t c = 0; c < 3; ++c)
            {
                // EXACT float equality ON PURPOSE (the other place float == is
                // the point): bit-identity — not closeness — is what the frozen
                // Legacy contract promises; ANY drift here moves the pinned
                // Cornell-box render hashes.
                EXPECT_EQ(viaExplicit[c], direct[c])
                    << "channel " << c << ": BSDF::Evaluate(Model = Legacy) is no longer the verbatim "
                    << "CookTorranceBRDF — the Legacy float sequence is pinned by "
                    << "PathTracerCornellBoxTest.RendersAreBitIdentical and must not change";
                EXPECT_EQ(viaDefault[c], direct[c])
                    << "channel " << c << ": a default-constructed ReferenceMaterial no longer shades "
                    << "Legacy — the engine-wide default silently flipped (ADR 0016 forbids that "
                    << "without a deliberate decision)";
            }
        }

        // The dispatch is LIVE: a ClosureV2 material at near-mirror roughness
        // must produce a very different value (v2's unclamped-D peak vs
        // Legacy's EPSILON-clamped one). Float != is fine here for the same
        // reason == was above; the 100x gap is the assertion with teeth.
        ReferenceMaterial v2Material;
        v2Material.BaseColor = glm::vec3(1.0f);
        v2Material.Metallic = 1.0f;
        v2Material.Roughness = 0.05f;
        v2Material.Model = PBRModel::ClosureV2;

        // Head-on (v = l = n): nDotH == 1, where the two models' D treatment
        // differs the most.
        const glm::vec3 v2Value = BSDF::Evaluate(v2Material, n, n, n);
        const glm::vec3 legacyValue = CookTorranceBRDF(n, n, n, glm::vec3(1.0f), 1.0f, 0.05f);
        EXPECT_NE(v2Value.x, legacyValue.x)
            << "a ClosureV2 material evaluates identically to Legacy — the Model dispatch in "
               "PBRClosureBSDF.h is not reaching the v2 branch";
        // 100x separates "took the v2 branch" (head-on near-mirror: ~1.3e4 vs
        // ~1.6e-2, nearly six orders apart) from any plausible same-branch
        // numeric wobble.
        EXPECT_GT(v2Value.x, 100.0f * legacyValue.x)
            << "the v2 near-mirror value no longer dwarfs the clamped Legacy one — either the "
               "dispatch is dead or the v2 lobe collapsed (see NearMirrorLobeNoLongerCollapses)";
    }
} // namespace OloEngine::Tests
