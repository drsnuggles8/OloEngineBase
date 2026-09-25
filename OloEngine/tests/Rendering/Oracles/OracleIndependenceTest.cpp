// OLO_TEST_LAYER: meta
// =============================================================================
// OracleIndependenceTest.cpp — the oracles' own contract (issue #1347).
//
// Two things the BSDF oracle tests stand on, pinned here so they cannot rot:
//
//   1. INDEPENDENCE, checked by the includes. The oracle headers may include
//      the standard library, glm, the engine's primitive typedefs and each
//      other — nothing else. An oracle that includes ReferenceBRDF.h or
//      PBRCommon's mirror can call the code it is meant to check, and then it
//      proves agreement, not correctness.
//   2. THE STATISTICS ARE RIGHT. The p-value functions against textbook
//      values, the refusal of non-IID provenance, and the Bonferroni family
//      verdict — a chi-square helper that returns 1.0 for everything would
//      make every sampling test pass.
//
// Plus the oracle's self-consistency against closed forms from the papers it
// cites ([Heitz14] eq. 2: D cos integrates to 1; the octant is pi/2 sr), so a
// transcription error in the oracle itself is caught here and not blamed on
// the engine.
// =============================================================================

#include "OloEnginePCH.h"

#include "Rendering/Oracles/BsdfOracleChecks.h"
#include "Rendering/Oracles/IndependentBsdfOracle.h"
#include "Rendering/Oracles/OracleStatistics.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests::Oracle
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] fs::path OracleDirectory()
        {
            return fs::path(OLO_TEST_EDITOR_ROOT) / ".." / "OloEngine" / "tests" / "Rendering" / "Oracles";
        }

        [[nodiscard]] std::vector<std::string> IncludesOf(const fs::path& file)
        {
            std::ifstream in(file);
            std::vector<std::string> includes;
            const std::regex include(R"(^\s*#\s*include\s*([<"][^>"]+[>"]))");
            std::string line;
            while (std::getline(in, line))
            {
                std::smatch match;
                if (std::regex_search(line, match, include))
                    includes.push_back(match[1].str());
            }
            return includes;
        }

        // <name> with no directory is the standard library; <glm/...> is glm;
        // "OloEngine/Core/Base.h" is the primitive typedefs; the oracle headers
        // may include each other — but never EngineBsdfAdapters.h, the one
        // header in the directory that wraps the renderer's functions.
        [[nodiscard]] bool IsAllowedOracleInclude(const std::string& include)
        {
            if (include.front() == '<')
                return include.find('/') == std::string::npos || include.starts_with("<glm/");
            if (include.find("EngineBsdfAdapters") != std::string::npos)
                return false;
            return include == "\"OloEngine/Core/Base.h\"" || include.starts_with("\"Rendering/Oracles/");
        }
    } // namespace

    TEST(OracleIndependenceTest, IncludesOnlyTheStandardLibraryGlmAndBaseTypes)
    {
        const struct
        {
            const char* File;
            sizet MinimumIncludes; // counted, so an unreadable file cannot pass
        } headers[] = {
            { "IndependentBsdfOracle.h", 5 },
            { "OracleStatistics.h", 6 },
            { "BsdfOracleChecks.h", 9 },
        };
        for (const auto& header : headers)
        {
            const fs::path path = OracleDirectory() / header.File;
            ASSERT_TRUE(fs::exists(path)) << "oracle header not found at " << path.string();
            const std::vector<std::string> includes = IncludesOf(path);
            EXPECT_GE(includes.size(), header.MinimumIncludes)
                << header.File << ": read " << includes.size() << " includes — is the file readable?";
            for (const std::string& include : includes)
            {
                EXPECT_TRUE(IsAllowedOracleInclude(include))
                    << header.File << " includes " << include
                    << ". An oracle may not include renderer code: it would then check the implementation "
                       "against itself (issue #1347, the independence rule in IndependentBsdfOracle.h).";
            }
        }
    }

    TEST(OracleIndependenceTest, TheIncludeRuleRejectsTheRendererMirrors)
    {
        // The negative control for the rule above: the headers an oracle must
        // never include are rejected by the same predicate.
        EXPECT_FALSE(IsAllowedOracleInclude("\"OloEngine/Renderer/PathTracing/ReferenceBRDF.h\""));
        EXPECT_FALSE(IsAllowedOracleInclude("\"OloEngine/Renderer/PathTracing/PBRClosureBSDF.h\""));
        EXPECT_FALSE(IsAllowedOracleInclude("\"OloEngine/Renderer/ReSTIR/ReservoirCore.h\""));
        EXPECT_FALSE(IsAllowedOracleInclude("<OloEngine/Renderer/PathTracing/GgxEnergyTables.h>"));
        EXPECT_FALSE(IsAllowedOracleInclude("\"Rendering/Oracles/EngineBsdfAdapters.h\""));
        EXPECT_TRUE(IsAllowedOracleInclude("\"Rendering/Oracles/OracleStatistics.h\""));
        EXPECT_TRUE(IsAllowedOracleInclude("<cmath>"));
        EXPECT_TRUE(IsAllowedOracleInclude("<glm/glm.hpp>"));
    }

    // ---- statistics against textbook values --------------------------------

    TEST(OracleStatisticsTest, ChiSquareUpperTailMatchesTheTables)
    {
        // Critical values at the 5 % and 0.1 % levels (Abramowitz & Stegun,
        // table 26.8).
        EXPECT_NEAR(ChiSquareUpperTail(3.841459, 1.0), 0.05, 1.0e-6);
        EXPECT_NEAR(ChiSquareUpperTail(18.307038, 10.0), 0.05, 1.0e-6);
        EXPECT_NEAR(ChiSquareUpperTail(124.342113, 100.0), 0.05, 1.0e-6);
        EXPECT_NEAR(ChiSquareUpperTail(29.588298, 10.0), 0.001, 1.0e-7);
        // dof 2 is exact: Q(1, x/2) = exp(-x/2).
        EXPECT_NEAR(ChiSquareUpperTail(7.0, 2.0), std::exp(-3.5), 1.0e-12);
    }

    TEST(OracleStatisticsTest, KolmogorovUpperTailMatchesTheAsymptoticTable)
    {
        // Q_KS(1.3581) = 0.05 and Q_KS(1.9496) = 0.001 (Smirnov 1948); large n
        // makes Stephens' correction negligible.
        const u64 n = 100000000ull;
        const f64 sn = std::sqrt(static_cast<f64>(n));
        EXPECT_NEAR(KolmogorovUpperTail(1.3581 / sn, n), 0.05, 2.0e-4);
        EXPECT_NEAR(KolmogorovUpperTail(1.9496 / sn, n), 0.001, 2.0e-5);
    }

    TEST(OracleStatisticsTest, NonIidProvenanceIsRefusedNotPassed)
    {
        const std::vector<u64> observed{ 100, 100, 100, 100 };
        const std::vector<f64> expected{ 0.25, 0.25, 0.25, 0.25 };
        for (SampleProvenance p : { SampleProvenance::LowDiscrepancy, SampleProvenance::ReservoirM,
                                    SampleProvenance::CorrelatedFrames })
        {
            const GoodnessOfFit fit = ChiSquareTest(observed, expected, p);
            EXPECT_FALSE(fit.Valid) << ToString(p) << " was accepted as IID evidence";
            EXPECT_NE(fit.WhyInvalid.find("refused"), std::string::npos);
        }
        EXPECT_TRUE(ChiSquareTest(observed, expected, SampleProvenance::IidPseudoRandom).Valid);

        // An invalid run fails the family instead of being skipped.
        const IndependentRuns runs{ 3 };
        const FamilyVerdict verdict = RunFamily(runs, [&](u64)
                                                { return ChiSquareTest(observed, expected, SampleProvenance::ReservoirM); });
        EXPECT_FALSE(verdict.Pass);
    }

    TEST(OracleStatisticsTest, RunsHaveDistinctSeedsAndTheFamilyHasPower)
    {
        const IndependentRuns runs{};
        for (u32 a = 0; a < runs.Count; ++a)
            for (u32 b = a + 1u; b < runs.Count; ++b)
                EXPECT_NE(runs.Seed(a), runs.Seed(b));

        // Size: a fair die passes. Power: a die with a 2 % bias on one face,
        // at 200k rolls, does not.
        auto die = [](f64 biasOnSix)
        {
            return [biasOnSix](u64 seed)
            {
                IidStream s(seed);
                std::vector<u64> observed(6, 0);
                for (int i = 0; i < 200000; ++i)
                {
                    const f64 u = s.Next();
                    const f64 six = (1.0 + biasOnSix) / 6.0;
                    const u32 face = u < six ? 5u : std::min(4u, static_cast<u32>((u - six) / ((1.0 - six) / 5.0)));
                    ++observed[face];
                }
                return ChiSquareTest(observed, std::vector<f64>(6, 1.0 / 6.0), IidStream::Provenance());
            };
        };
        EXPECT_TRUE(RunFamily(runs, die(0.0)).Pass);
        EXPECT_FALSE(RunFamily(runs, die(0.02)).Pass);
    }

    // ---- the oracle against the closed forms it cites ----------------------

    TEST(OracleSelfConsistencyTest, GgxProjectedAreaIntegratesToOne)
    {
        // [Heitz14] eq. 2 by uniform quadrature (not the NDF-CDF substitution,
        // which would assume the answer): int D(m) cos(m) dm = 1.
        for (f64 alpha : { 0.2, 0.5, 1.0 })
        {
            const Quadrature q =
                IntegrateHemisphereUniform(4096, 16, [alpha](const glm::dvec3& m)
                                           { return GgxD(m.z, alpha) * m.z; });
            EXPECT_NEAR(q.Value, 1.0, std::max(2.0e-3, 4.0 * q.ErrorEstimate)) << "alpha " << alpha;
        }
        // And the CDF is the running integral of that density: the mass at
        // theta_m <= theta, i.e. at cos(theta_m) >= c.
        const f64 alpha = 0.3;
        const f64 c = 0.8;
        const Quadrature partial = IntegrateHemisphereUniform(
            8192, 8, [&](const glm::dvec3& m)
            { return m.z >= c ? GgxD(m.z, alpha) * m.z : 0.0; });
        EXPECT_NEAR(partial.Value, GgxThetaCdf(c, alpha), 1.0e-3);
        EXPECT_NEAR(GgxCosThetaFromCdf(GgxThetaCdf(c, alpha), alpha), c, 1.0e-12);
    }

    TEST(OracleSelfConsistencyTest, SmithG1IsTheProjectedAreaRatio)
    {
        // [Heitz14] eq. 22, the definition of G1:
        //   cos(theta_v) = int G1(v) <v, m> D(m) dm  over the visible normals.
        // So int_{v.m > 0} (v.m) D(m) dm = cos(theta_v) / G1(v).
        for (f64 alpha : { 0.1, 0.4, 0.9 })
        {
            for (f64 cv : { 0.1, 0.5, 0.95 })
            {
                // Uniform nodes, not the NDF-CDF ones: against D cos the
                // integrand would carry a 1/cos(m) the equal-mass cells
                // under-sample at the horizon. Here it is bounded.
                const glm::dvec3 v = Direction(cv, 0.0);
                const Quadrature q = IntegrateHemisphereUniform(
                    8192, 512, [&](const glm::dvec3& m)
                    { return std::max(glm::dot(v, m), 0.0) * GgxD(m.z, alpha); });
                const f64 expected = cv / GgxG1(cv, alpha);
                EXPECT_NEAR(q.Value, expected, std::max(1.0e-3 * expected, 4.0 * q.ErrorEstimate))
                    << "alpha " << alpha << ", cos v " << cv << ", quadrature error estimate " << q.ErrorEstimate;
            }
        }
    }

    TEST(OracleSelfConsistencyTest, SolidAngleOfTheOctantIsHalfPi)
    {
        EXPECT_NEAR(TriangleSolidAngle({ 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }), kPi / 2.0, 1.0e-12);
        // A small patch approaches dA cos / d^2.
        const f64 h = 1.0e-3;
        const glm::dvec3 p{ 0.0, 0.0, 2.0 };
        EXPECT_NEAR(PatchSolidAngle(p, { 0, 0, 0 }, { 0, 0, 1 }, h), 4.0 * h * h / 4.0, 1.0e-12);
    }
} // namespace OloEngine::Tests::Oracle
