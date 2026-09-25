#pragma once

// =============================================================================
// OracleStatistics.h — the statistical conventions every sampling oracle in
// this directory follows (issue #1347, acceptance criterion 4).
// =============================================================================
//
// THE CONVENTIONS, stated once:
//
// 1. Goodness-of-fit tests (Pearson chi-square, Kolmogorov-Smirnov) assume
//    INDEPENDENT, IDENTICALLY DISTRIBUTED draws. Only a pseudo-random stream
//    satisfies that. Three sample sources in this engine do not, and a test
//    must never feed them to a goodness-of-fit statistic:
//      * low-discrepancy sequences (Hammersley, Sobol/Owen — PathSampler):
//        stratified by design, so they fit ANY smooth density "too well" and
//        the p-value means nothing. Use them for quadrature, never for a test
//        of a distribution.
//      * a reservoir's M (ReSTIR): M counts candidates that were resampled,
//        temporally and spatially reused and clamped. It is a confidence
//        weight, not an independent sample count, and its candidates are
//        correlated across pixels and frames.
//      * consecutive frames of one pixel: temporal reuse and accumulation
//        make them correlated, so N frames are not N samples.
//    `SampleProvenance` makes the rule executable: every goodness-of-fit entry
//    point takes one and refuses anything but IidPseudoRandom.
//
// 2. INDEPENDENT RUNS. A distribution is tested in `IndependentRuns::Count`
//    runs, each with its own seed derived from a base seed by SplitMix64, and
//    each with a fresh generator — no state carries between runs. The seeds
//    are FIXED, so the suite is deterministic; what the significance level
//    states is how likely a CORRECT sampler would have been rejected by seeds
//    chosen before looking.
//
// 3. SIGNIFICANCE. One check is one family: it fails if ANY of its runs has
//    p < FamilyAlpha / Count (Bonferroni), so a correct sampler fails a
//    check with probability at most FamilyAlpha = 1e-3. The mutation tests
//    (BsdfMutationDetectionTest) are the other half: they show the same check,
//    at the same sample size, rejects a wrong sampler — a test that cannot
//    fail is not evidence.
//
// 4. BINNING. Chi-square bins are pooled until every expected count is at
//    least `kMinimumExpectedCount` (5, Cochran's rule); the degrees of freedom
//    are (pooled bins - 1), with no parameters estimated from the data.
//
// 5. QUADRATURE ERROR is reported beside every integral (see
//    IndependentBsdfOracle.h `Quadrature`): a tolerance is max(stated floor,
//    k * error estimate), never a bare constant.
//
// Header-only; includes nothing from the renderer, same rule as the oracle.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace OloEngine::Tests::Oracle
{
    // ---- 1. provenance -----------------------------------------------------

    enum class SampleProvenance : u8
    {
        IidPseudoRandom,  // std::mt19937_64 with a SplitMix64-derived seed: the only valid input
        LowDiscrepancy,   // Hammersley / Sobol / Owen: quadrature only
        ReservoirM,       // a ReSTIR confidence weight, not a sample count
        CorrelatedFrames, // frames of one pixel under temporal reuse
    };

    [[nodiscard]] constexpr bool IsValidForGoodnessOfFit(SampleProvenance provenance)
    {
        return provenance == SampleProvenance::IidPseudoRandom;
    }

    [[nodiscard]] constexpr const char* ToString(SampleProvenance provenance)
    {
        switch (provenance)
        {
            case SampleProvenance::IidPseudoRandom:
                return "IID pseudo-random";
            case SampleProvenance::LowDiscrepancy:
                return "low-discrepancy (stratified, not IID)";
            case SampleProvenance::ReservoirM:
                return "reservoir M (a confidence weight, not a sample count)";
            case SampleProvenance::CorrelatedFrames:
                return "correlated frames (temporal reuse)";
        }
        return "unknown";
    }

    // ---- 2. independent runs -----------------------------------------------

    // SplitMix64 (Steele, Lea, Flood, OOPSLA 2014): the standard seed expander.
    [[nodiscard]] constexpr u64 SplitMix64(u64 x)
    {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    struct IndependentRuns
    {
        u32 Count = 5;
        u64 BaseSeed = 0x1347'0000'0000'0001ull;
        f64 FamilyAlpha = 1.0e-3;

        [[nodiscard]] u64 Seed(u32 run) const
        {
            return SplitMix64(BaseSeed ^ SplitMix64(static_cast<u64>(run) + 1u));
        }
        [[nodiscard]] f64 PerRunAlpha() const
        {
            return FamilyAlpha / static_cast<f64>(Count);
        }
    };

    // A fresh IID stream for one run.
    class IidStream
    {
      public:
        explicit IidStream(u64 seed) : m_Engine(seed) {}

        [[nodiscard]] f64 Next()
        {
            return m_Uniform(m_Engine);
        }
        [[nodiscard]] static constexpr SampleProvenance Provenance()
        {
            return SampleProvenance::IidPseudoRandom;
        }

      private:
        std::mt19937_64 m_Engine;
        std::uniform_real_distribution<f64> m_Uniform{ 0.0, 1.0 };
    };

    // ---- special functions --------------------------------------------------

    // Regularised upper incomplete gamma Q(a, x) = Gamma(a, x) / Gamma(a):
    // the series for x < a + 1, Lentz's continued fraction otherwise
    // (Press et al., Numerical Recipes 3e, §6.2).
    [[nodiscard]] inline f64 RegularizedGammaQ(f64 a, f64 x)
    {
        if (x <= 0.0)
            return 1.0;
        const f64 logPrefactor = -x + a * std::log(x) - std::lgamma(a);
        if (x < a + 1.0)
        {
            f64 term = 1.0 / a;
            f64 sum = term;
            for (int n = 1; n < 10000; ++n)
            {
                term *= x / (a + n);
                sum += term;
                if (std::abs(term) < std::abs(sum) * 1.0e-15)
                    break;
            }
            return std::clamp(1.0 - sum * std::exp(logPrefactor), 0.0, 1.0);
        }
        constexpr f64 tiny = 1.0e-300;
        f64 b = x + 1.0 - a;
        f64 c = 1.0 / tiny;
        f64 d = 1.0 / b;
        f64 h = d;
        for (int i = 1; i < 10000; ++i)
        {
            const f64 an = -i * (i - a);
            b += 2.0;
            d = an * d + b;
            if (std::abs(d) < tiny)
                d = tiny;
            c = b + an / c;
            if (std::abs(c) < tiny)
                c = tiny;
            d = 1.0 / d;
            const f64 delta = d * c;
            h *= delta;
            if (std::abs(delta - 1.0) < 1.0e-15)
                break;
        }
        return std::clamp(std::exp(logPrefactor) * h, 0.0, 1.0);
    }

    // P(X >= statistic) for X ~ chi-square with `dof` degrees of freedom.
    [[nodiscard]] inline f64 ChiSquareUpperTail(f64 statistic, f64 dof)
    {
        return RegularizedGammaQ(0.5 * dof, 0.5 * statistic);
    }

    // Asymptotic Kolmogorov distribution with Stephens' small-sample
    // correction: p = Q_KS((sqrt(n) + 0.12 + 0.11 / sqrt(n)) D),
    // Q_KS(lambda) = 2 sum_{k>=1} (-1)^(k-1) exp(-2 k^2 lambda^2)
    // (Numerical Recipes 3e, §14.3.3).
    [[nodiscard]] inline f64 KolmogorovUpperTail(f64 d, u64 n)
    {
        const f64 sn = std::sqrt(static_cast<f64>(n));
        const f64 lambda = (sn + 0.12 + 0.11 / sn) * d;
        if (lambda < 1.0e-3)
            return 1.0;
        f64 sum = 0.0;
        f64 sign = 1.0;
        for (int k = 1; k <= 200; ++k)
        {
            const f64 term = sign * std::exp(-2.0 * k * k * lambda * lambda);
            sum += term;
            if (std::abs(term) < 1.0e-16)
                break;
            sign = -sign;
        }
        return std::clamp(2.0 * sum, 0.0, 1.0);
    }

    // ---- 4. goodness of fit -------------------------------------------------

    inline constexpr f64 kMinimumExpectedCount = 5.0;

    struct GoodnessOfFit
    {
        bool Valid = false;     // false: refused (provenance) or degenerate input
        std::string WhyInvalid; // set when !Valid
        f64 Statistic = 0.0;
        f64 DegreesOfFreedom = 0.0;
        f64 PValue = 0.0;
        u32 PooledBins = 0;
    };

    // Pearson chi-square of observed counts against expected PROBABILITIES
    // (which must sum to 1 over the bins — including any "rejected" bin the
    // caller defines). Adjacent bins are pooled, in the given order, until
    // each pooled expected count reaches kMinimumExpectedCount; a trailing
    // under-filled pool merges into the previous one.
    [[nodiscard]] inline GoodnessOfFit ChiSquareTest(const std::vector<u64>& observed,
                                                     const std::vector<f64>& expectedProbability,
                                                     SampleProvenance provenance)
    {
        GoodnessOfFit result;
        if (!IsValidForGoodnessOfFit(provenance))
        {
            result.WhyInvalid = std::string("chi-square refused: samples are ") + ToString(provenance);
            return result;
        }
        if (observed.size() != expectedProbability.size() || observed.empty())
        {
            result.WhyInvalid = "observed and expected bin counts differ";
            return result;
        }
        u64 n = 0;
        f64 probabilitySum = 0.0;
        for (sizet i = 0; i < observed.size(); ++i)
        {
            n += observed[i];
            probabilitySum += expectedProbability[i];
        }
        if (n == 0 || std::abs(probabilitySum - 1.0) > 1.0e-3)
        {
            result.WhyInvalid = "expected probabilities sum to " + std::to_string(probabilitySum) + ", not 1";
            return result;
        }

        std::vector<f64> pooledExpected;
        std::vector<f64> pooledObserved;
        f64 accE = 0.0;
        f64 accO = 0.0;
        for (sizet i = 0; i < observed.size(); ++i)
        {
            accE += expectedProbability[i] * static_cast<f64>(n);
            accO += static_cast<f64>(observed[i]);
            if (accE >= kMinimumExpectedCount)
            {
                pooledExpected.push_back(accE);
                pooledObserved.push_back(accO);
                accE = 0.0;
                accO = 0.0;
            }
        }
        if (accE > 0.0 || accO > 0.0)
        {
            if (pooledExpected.empty())
            {
                pooledExpected.push_back(accE);
                pooledObserved.push_back(accO);
            }
            else
            {
                pooledExpected.back() += accE;
                pooledObserved.back() += accO;
            }
        }
        if (pooledExpected.size() < 2)
        {
            result.WhyInvalid = "fewer than two bins after pooling";
            return result;
        }

        f64 chi2 = 0.0;
        for (sizet i = 0; i < pooledExpected.size(); ++i)
        {
            const f64 diff = pooledObserved[i] - pooledExpected[i];
            chi2 += diff * diff / pooledExpected[i];
        }
        result.Valid = true;
        result.Statistic = chi2;
        result.DegreesOfFreedom = static_cast<f64>(pooledExpected.size() - 1);
        result.PValue = ChiSquareUpperTail(chi2, result.DegreesOfFreedom);
        result.PooledBins = static_cast<u32>(pooledExpected.size());
        return result;
    }

    // One-sample KS test of `samples` against a continuous CDF. Sorts a copy.
    template<typename Cdf>
    [[nodiscard]] GoodnessOfFit KolmogorovSmirnovTest(std::vector<f64> samples, Cdf&& cdf, SampleProvenance provenance)
    {
        GoodnessOfFit result;
        if (!IsValidForGoodnessOfFit(provenance))
        {
            result.WhyInvalid = std::string("KS refused: samples are ") + ToString(provenance);
            return result;
        }
        if (samples.empty())
        {
            result.WhyInvalid = "no samples";
            return result;
        }
        std::ranges::sort(samples);
        const auto n = static_cast<f64>(samples.size());
        f64 d = 0.0;
        for (sizet i = 0; i < samples.size(); ++i)
        {
            const f64 f = cdf(samples[i]);
            d = std::max(d, std::max(f - static_cast<f64>(i) / n, static_cast<f64>(i + 1) / n - f));
        }
        result.Valid = true;
        result.Statistic = d;
        result.PValue = KolmogorovUpperTail(d, samples.size());
        return result;
    }

    // ---- 3. the family verdict ---------------------------------------------

    struct FamilyVerdict
    {
        bool Pass = true;
        f64 SmallestPValue = 1.0;
        std::string Detail; // one line per run
    };

    // Run `oneRun(seed) -> GoodnessOfFit` once per independent run and apply the
    // Bonferroni rule. An invalid run fails the family: a refused or degenerate
    // test is never silently counted as a pass.
    template<typename OneRun>
    [[nodiscard]] FamilyVerdict RunFamily(const IndependentRuns& runs, OneRun&& oneRun)
    {
        FamilyVerdict verdict;
        for (u32 r = 0; r < runs.Count; ++r)
        {
            const GoodnessOfFit fit = oneRun(runs.Seed(r));
            if (!fit.Valid)
            {
                verdict.Pass = false;
                verdict.Detail += "run " + std::to_string(r) + ": INVALID (" + fit.WhyInvalid + ")\n";
                continue;
            }
            verdict.SmallestPValue = std::min(verdict.SmallestPValue, fit.PValue);
            const bool runPasses = fit.PValue >= runs.PerRunAlpha();
            verdict.Pass = verdict.Pass && runPasses;
            verdict.Detail += "run " + std::to_string(r) + ": statistic " + std::to_string(fit.Statistic) + ", dof " +
                              std::to_string(fit.DegreesOfFreedom) + ", p " + std::to_string(fit.PValue) +
                              (runPasses ? "" : "  <-- below alpha/runs") + "\n";
        }
        return verdict;
    }
} // namespace OloEngine::Tests::Oracle
