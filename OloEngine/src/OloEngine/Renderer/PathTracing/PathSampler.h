#pragma once

// =============================================================================
// PathSampler.h — the deterministic, stateless sample source for the offline
// reference path tracer (issue #709).
//
// WHY STATELESS MATTERS MORE THAN IT USUALLY DOES
// -----------------------------------------------
// The reference image is meant to be an ORACLE, and an oracle that is only
// reproducible up to thread scheduling is not one: a CI gate would have to
// compare it with a fuzzy tolerance, which is exactly the fuzziness the
// instrument exists to remove. So this sampler derives every random value
// purely from (pixel, sample index, dimension, seed) — it reads no shared
// state, consumes no global stream, and takes no lock. Two renders of the same
// scene at the same sample count are therefore BIT-IDENTICAL regardless of how
// many worker threads ran them, and a per-pixel result does not depend on which
// pixels were traced before it.
//
// (The other half of that guarantee lives in the integrator: samples for one
// pixel are summed in a fixed order, sample 0 first, so the floating-point
// accumulation is reproducible too. Splitting a pixel's samples across threads
// would break bit-identity even with this sampler.)
//
// THE SEQUENCE
// ------------
// Owen-scrambled Sobol' — the 2D Sobol' pair (van der Corput + the classic
// second direction-number set), nested-uniform-scrambled with the practical
// hash of Burley, "Practical Hash-based Owen Scrambling" (JCGT 2020). Each
// dimension gets its own scramble seed, so consecutive dimensions are
// decorrelated ("padded") while each individually keeps its low-discrepancy
// stratification. That buys roughly an order of magnitude fewer samples than
// plain white noise for the same variance on the Cornell-box scene, which is
// what makes a CI-affordable sample count possible at all.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine::PathTracing
{
    namespace SamplerDetail
    {
        // Bit reversal — the van der Corput / Sobol' dimension-0 generator and
        // half of the Owen scramble.
        [[nodiscard]] inline constexpr u32 ReverseBits(u32 x) noexcept
        {
            x = (x << 16u) | (x >> 16u);
            x = ((x & 0x55555555u) << 1u) | ((x & 0xAAAAAAAAu) >> 1u);
            x = ((x & 0x33333333u) << 2u) | ((x & 0xCCCCCCCCu) >> 2u);
            x = ((x & 0x0F0F0F0Fu) << 4u) | ((x & 0xF0F0F0F0u) >> 4u);
            x = ((x & 0x00FF00FFu) << 8u) | ((x & 0xFF00FF00u) >> 8u);
            return x;
        }

        // Laine-Karras permutation (Burley 2020, listing 4). Only the LOW bits
        // are well mixed, which is why the nested-uniform scramble sandwiches
        // it between two bit reversals.
        [[nodiscard]] inline constexpr u32 LaineKarrasPermutation(u32 x, u32 seed) noexcept
        {
            x += seed;
            x ^= x * 0x6c50b47cu;
            x ^= x * 0xb82f1e52u;
            x ^= x * 0xc7afe638u;
            x ^= x * 0x8d22f6e6u;
            return x;
        }

        // Nested uniform (Owen) scramble of a base-2 radical-inverse code word.
        [[nodiscard]] inline constexpr u32 NestedUniformScramble(u32 x, u32 seed) noexcept
        {
            x = ReverseBits(x);
            x = LaineKarrasPermutation(x, seed);
            return ReverseBits(x);
        }

        // Sobol' dimension 0 — the van der Corput sequence in base 2.
        [[nodiscard]] inline constexpr u32 Sobol0(u32 index) noexcept
        {
            return ReverseBits(index);
        }

        // Sobol' dimension 1 — the classic second direction-number set
        // (v_k = 0x80000000 >> k with the Gray-code XOR recurrence).
        [[nodiscard]] inline constexpr u32 Sobol1(u32 index) noexcept
        {
            u32 v = 0u;
            for (u32 direction = 0x80000000u; index != 0u; index >>= 1u, direction ^= direction >> 1u)
            {
                if ((index & 1u) != 0u)
                    v ^= direction;
            }
            return v;
        }

        // A cheap, well-avalanched integer hash (Wang / Jenkins style) used to
        // turn a (seed, dimension) pair into an independent scramble key.
        [[nodiscard]] inline constexpr u32 HashCombine(u32 seed, u32 value) noexcept
        {
            u32 x = seed ^ (value * 0x9e3779b9u);
            x ^= x >> 16u;
            x *= 0x7feb352du;
            x ^= x >> 15u;
            x *= 0x846ca68bu;
            x ^= x >> 16u;
            return x;
        }

        [[nodiscard]] inline constexpr f32 ToUnitFloat(u32 x) noexcept
        {
            // 24-bit mantissa's worth of the code word, mapped into [0, 1).
            // Truncating to 24 bits (rather than scaling the full 32) keeps the
            // result strictly below 1.0 after rounding, which several call
            // sites rely on (a returned 1.0 makes sqrt(1 - xi) == 0 and can
            // send a cosine-sampled direction exactly into the surface plane).
            return static_cast<f32>(x >> 8u) * 0x1.0p-24f;
        }
    } // namespace SamplerDetail

    // -------------------------------------------------------------------------
    // PathSampler
    //
    // Construct one per (pixel, sample index); pull dimensions in a FIXED order
    // along the path. The dimension counter is the only mutable state and it is
    // local to the object.
    //
    // A path that takes a different number of dimensions on different bounces
    // (as this integrator's does — the NEE branch is skipped when the scene has
    // no samplable emitter) is fine: dimensions stay aligned across samples of
    // the SAME pixel because the branch decision is scene-level, not
    // sample-level.
    // -------------------------------------------------------------------------
    class PathSampler
    {
      public:
        // `pixelSeed` decorrelates neighbouring pixels; `sampleIndex` walks the
        // low-discrepancy sequence.
        PathSampler(u32 pixelSeed, u32 sampleIndex) noexcept
            : m_PixelSeed(pixelSeed), m_SampleIndex(sampleIndex)
        {
        }

        [[nodiscard]] f32 Get1D() noexcept
        {
            const u32 scramble = SamplerDetail::HashCombine(m_PixelSeed, m_Dimension++);
            return SamplerDetail::ToUnitFloat(
                SamplerDetail::NestedUniformScramble(SamplerDetail::Sobol0(ScrambledIndex(scramble)), scramble));
        }

        [[nodiscard]] glm::vec2 Get2D() noexcept
        {
            const u32 scrambleX = SamplerDetail::HashCombine(m_PixelSeed, m_Dimension++);
            const u32 scrambleY = SamplerDetail::HashCombine(m_PixelSeed, m_Dimension++);
            // Both components must walk the SAME (possibly index-shuffled)
            // point of the 2D Sobol' set, or the pair stops being stratified.
            const u32 index = ScrambledIndex(scrambleX);
            const f32 x = SamplerDetail::ToUnitFloat(
                SamplerDetail::NestedUniformScramble(SamplerDetail::Sobol0(index), scrambleX));
            const f32 y = SamplerDetail::ToUnitFloat(
                SamplerDetail::NestedUniformScramble(SamplerDetail::Sobol1(index), scrambleY));
            return glm::vec2(x, y);
        }

        // Number of dimensions consumed so far — diagnostics only.
        [[nodiscard]] u32 GetDimension() const noexcept
        {
            return m_Dimension;
        }

      private:
        // Owen-scrambling the sample INDEX as well as the value is what makes
        // per-pixel sequences independent while each stays a progressive
        // low-discrepancy sequence for any prefix length (Burley 2020 §5.2).
        [[nodiscard]] u32 ScrambledIndex(u32 scramble) const noexcept
        {
            return SamplerDetail::NestedUniformScramble(m_SampleIndex, scramble ^ 0x51633e2du);
        }

        u32 m_PixelSeed = 0;
        u32 m_SampleIndex = 0;
        u32 m_Dimension = 0;
    };

    // Stable per-pixel seed. Deliberately NOT the raw pixel index: a linear
    // seed makes the hash's low bits correlate along a scanline, which shows up
    // as visible horizontal structure at low sample counts.
    [[nodiscard]] inline u32 MakePixelSeed(u32 x, u32 y, u32 globalSeed) noexcept
    {
        return SamplerDetail::HashCombine(SamplerDetail::HashCombine(globalSeed, x), y * 0x2545f491u + 1u);
    }
} // namespace OloEngine::PathTracing
