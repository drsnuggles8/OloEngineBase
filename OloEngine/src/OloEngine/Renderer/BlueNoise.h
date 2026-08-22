#pragma once

// =============================================================================
// BlueNoise.h — the void-and-cluster generator behind the shared screen-space
// sampler (issue #706).
//
// GPU twin: OloEditor/assets/shaders/include/StochasticCommon.glsl, which
// samples the tile this file produces and rotates a shared Sobol' sequence by
// it. Read that header first — it explains WHY a blue-noise mask is the thing
// worth having; this file is only how the mask gets made.
//
// -----------------------------------------------------------------------------
// WHY GENERATE IT RATHER THAN SHIP A PNG
// -----------------------------------------------------------------------------
// A committed noise texture is a binary blob nobody can review, whose
// properties can only be asserted about the FILE. Generating it means the
// property tests in StochasticSamplerTest run against the same code path the
// renderer uses, so "the tile is blue" is a claim about the algorithm rather
// than about an artifact somebody produced once with a tool that is no longer
// in the repo. It costs about a tenth of a second, once, in a Debug build.
//
// The generator is deterministic — same seed, same tile, on every platform and
// at every optimisation level. That is a deliberate property, not a happy
// accident: the energy field is FIXED-POINT integer (see kEnergyScale below)
// precisely so that no argmin/argmax can be decided by a one-ULP float
// difference, and the PRNG is a self-contained SplitMix32 rather than
// std::random, whose distributions are implementation-defined. It matters
// because a golden or a perceptual A/B captured against this tile has to stay
// valid on another machine.
//
// -----------------------------------------------------------------------------
// THE ALGORITHM (Ulichney, "The void-and-cluster method for dither array
// generation", SPIE 1993)
// -----------------------------------------------------------------------------
// Blue noise is not "random with a filter applied" — you cannot low-pass a
// white pattern into a blue one, because the low frequencies you want gone are
// exactly the ones a filter cannot distinguish from signal. Void-and-cluster
// builds the pattern the other way round: it maintains an ENERGY field (each
// set pixel spreads a wrapped Gaussian) and repeatedly moves the most crowded
// pixel into the emptiest gap. The result is a set with no clusters and no
// voids at any scale — which IS the spatial statement that its spectrum has no
// low-frequency energy.
//
// Ranking then converts the binary pattern into a threshold array: remove set
// pixels tightest-cluster-first for the low half of the ranks, then fill empty
// pixels largest-void-first for the high half. Every prefix of the resulting
// ranking is itself a well-distributed set, which is the property that makes
// the array usable as a per-pixel value rather than only as a dither threshold.
//
// (Ulichney's phase 3 is stated as "tightest cluster of the COMPLEMENT". With a
// wrapped kernel the complement's energy is a constant minus this one's, so
// that is the same pixel as this field's largest void — the loop below is
// therefore written once and covers both halves. The kernel truncation makes
// the identity approximate rather than exact, which changes nothing: past the
// midpoint "largest void" remains the correct criterion on its own terms.)
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <array>
#include <cmath>
#include <limits>

namespace OloEngine::BlueNoise
{
    // 64x64, two channels. Both halves of that choice are measured.
    //
    // Small enough that generation is free: 12 ms Release / 105 ms Debug, once.
    // 128x128 costs 466 ms Release — 38x, not the 4x the pixel count predicts,
    // because the O(pixels^2) argmin/argmax scans stop fitting in cache — which
    // is ~4 s in a Debug editor startup. And it buys nothing: the two tiles are
    // spectrally identical (low/high band power 0.0002 either way, neighbour
    // correlation -0.272 against -0.284).
    //
    // Large enough that the repeat period is far outside any denoiser or TAA
    // neighbourhood.
    //
    // Mirrored by OLO_BLUE_NOISE_TILE_SIZE in StochasticCommon.glsl. The shader
    // wraps with a bitmask, so this MUST stay a power of two.
    inline constexpr u32 kTileSize = 64;
    inline constexpr u32 kTilePixels = kTileSize * kTileSize;
    inline constexpr u32 kChannels = 2;

    static_assert((kTileSize & (kTileSize - 1)) == 0, "kTileSize must be a power of two — the shader wraps with a mask");

    namespace Detail
    {
        // Ulichney's recommended spread. Wide enough that the energy field sees
        // structure several pixels out (which is what suppresses low
        // frequencies), tight enough that the truncated kernel below is honest.
        inline constexpr f32 kSigma = 1.5f;
        inline constexpr i32 kKernelRadius = 5; // ~3.3 sigma; exp(-25/4.5) = 0.004

        // A deterministic, self-contained PRNG. std::mt19937 would also be
        // reproducible, but std::uniform_int_distribution is NOT specified
        // across implementations — and this tile has to be identical on every
        // platform or a golden captured on one machine is invalid on another.
        //
        // COUNTER-BASED, WITH THE SEED MIXED INTO EVERY DRAW. The obvious
        // formulation — a state that advances by a fixed increment, seeded once —
        // has a trap that bit this generator: every seed then lands somewhere on
        // ONE cycle, so two seeds are the same sequence at different offsets. If
        // those offsets happen to be close, the two streams are nearly identical.
        //
        // That is not hypothetical. The first version seeded with
        // `seed * 0x9e3779b9` — the increment's own multiple — which put seeds 1
        // and 2 exactly ONE DRAW apart. The two blue-noise channels came out
        // 99.3% identical at the prototype stage and correlated at +0.55, so a
        // 2D sample's two components were not independent at all. Every
        // per-channel metric still looked perfect, because each channel WAS a
        // fine blue-noise field; only the cross-channel correlation saw it, and
        // StochasticSampler.TileChannelsAreIndependent is what caught it.
        //
        // XORing the avalanched seed into each counter step gives genuinely
        // independent streams instead of offsets of one.
        class SplitMix32
        {
          public:
            explicit constexpr SplitMix32(u32 seed) noexcept : m_Seed(Avalanche(seed)) {}

            [[nodiscard]] constexpr u32 Next() noexcept
            {
                m_Counter += 0x9e3779b9u;
                return Avalanche(m_Seed ^ m_Counter);
            }

            // Unbiased index in [0, bound) by rejection — a modulo would skew
            // the low indices, which on a 4096-pixel tile is a visible bias in
            // where the initial ones land.
            [[nodiscard]] constexpr u32 NextBelow(u32 bound) noexcept
            {
                const u32 limit = std::numeric_limits<u32>::max() - (std::numeric_limits<u32>::max() % bound);
                u32 value = Next();
                while (value >= limit)
                    value = Next();
                return value % bound;
            }

          private:
            [[nodiscard]] static constexpr u32 Avalanche(u32 z) noexcept
            {
                z ^= z >> 16u;
                z *= 0x21f0aaadu;
                z ^= z >> 15u;
                z *= 0x735a2d97u;
                z ^= z >> 15u;
                return z;
            }

            u32 m_Seed;
            u32 m_Counter = 0;
        };

        // THE ENERGY FIELD IS FIXED-POINT, AND THAT IS NOT AN OPTIMISATION.
        //
        // Every decision this generator makes is an argmin/argmax over the
        // energy field, so a one-ULP difference in a single kernel weight can
        // flip a near-tie, and one flipped decision changes the rest of the tile
        // completely. In floats the tile would therefore be "deterministic" only
        // for one libm at one optimisation level -- and a golden or a perceptual
        // A/B captured against it would be silently invalid anywhere else.
        // Accumulating i64 multiples of a fixed u32 weight table makes every
        // comparison exact integer arithmetic instead.
        //
        // The table is still built with std::exp, but at 2^20 scale a one-ULP
        // difference (~2^-24 relative) cannot change the rounded integer, so the
        // table is stable where the raw comparisons were not.
        inline constexpr u32 kEnergyScale = 1u << 20;

        using Energy = std::array<i64, kTilePixels>;
        using Pattern = std::array<u8, kTilePixels>;
        using Kernel = std::array<u32, static_cast<sizet>(2 * kKernelRadius + 1) * (2 * kKernelRadius + 1)>;

        [[nodiscard]] inline const Kernel& GaussianKernel()
        {
            static const Kernel s_Kernel = []
            {
                Kernel k{};
                constexpr f64 invTwoSigmaSq = 1.0 / (2.0 * static_cast<f64>(kSigma) * static_cast<f64>(kSigma));
                sizet i = 0;
                for (i32 dy = -kKernelRadius; dy <= kKernelRadius; ++dy)
                {
                    for (i32 dx = -kKernelRadius; dx <= kKernelRadius; ++dx, ++i)
                    {
                        const auto distSq = static_cast<f64>(dx * dx + dy * dy);
                        k[i] = static_cast<u32>(
                            std::llround(std::exp(-distSq * invTwoSigmaSq) * static_cast<f64>(kEnergyScale)));
                    }
                }
                return k;
            }();
            return s_Kernel;
        }

        // Add (or, with sign -1, remove) one pixel's Gaussian contribution.
        // Incremental maintenance is what keeps the whole generator linear in
        // the number of toggles rather than quadratic.
        inline void SplatKernel(Energy& energy, u32 index, i32 sign) noexcept
        {
            const auto cx = static_cast<i32>(index % kTileSize);
            const auto cy = static_cast<i32>(index / kTileSize);
            const Kernel& kernel = GaussianKernel();

            // The modulo on each axis is what makes the kernel TOROIDAL: the tile
            // repeats across the screen, so a pixel near the left edge really is a
            // neighbour of one near the right. Dropping the wrap is the classic
            // void-and-cluster bug -- the pattern comes out blue in the interior and
            // clustered along the seams, which reads on screen as a faint grid at the
            // tile period and sends you to look at the sampler instead.

            sizet k = 0;
            for (i32 dy = -kKernelRadius; dy <= kKernelRadius; ++dy)
            {
                const i32 y = (cy + dy + static_cast<i32>(kTileSize)) % static_cast<i32>(kTileSize);
                for (i32 dx = -kKernelRadius; dx <= kKernelRadius; ++dx, ++k)
                {
                    const i32 x = (cx + dx + static_cast<i32>(kTileSize)) % static_cast<i32>(kTileSize);
                    energy[static_cast<sizet>(y) * kTileSize + static_cast<sizet>(x)] +=
                        static_cast<i64>(sign) * static_cast<i64>(kernel[k]);
                }
            }
        }

        // The most crowded set pixel. Strict > means ties break on the lowest
        // index, so the result does not depend on iteration order either.
        [[nodiscard]] inline u32 FindTightestCluster(const Energy& energy, const Pattern& pattern) noexcept
        {
            u32 best = 0;
            i64 bestEnergy = std::numeric_limits<i64>::min();
            for (u32 i = 0; i < kTilePixels; ++i)
            {
                if (pattern[i] != 0u && energy[i] > bestEnergy)
                {
                    bestEnergy = energy[i];
                    best = i;
                }
            }
            return best;
        }

        // The emptiest gap among the unset pixels.
        [[nodiscard]] inline u32 FindLargestVoid(const Energy& energy, const Pattern& pattern) noexcept
        {
            u32 best = 0;
            i64 bestEnergy = std::numeric_limits<i64>::max();
            for (u32 i = 0; i < kTilePixels; ++i)
            {
                if (pattern[i] == 0u && energy[i] < bestEnergy)
                {
                    bestEnergy = energy[i];
                    best = i;
                }
            }
            return best;
        }
    } // namespace Detail

    // One channel of the tile, as ranks in [0, kTilePixels).
    using RankTile = std::array<u32, kTilePixels>;

    // @brief Generate one channel's rank array by void-and-cluster.
    //
    // Deterministic in `seed`. The returned ranks are a permutation of
    // [0, kTilePixels) — every prefix of which is a blue-noise-distributed
    // subset, which is the property the sampler relies on.
    [[nodiscard]] inline RankTile GenerateRankTile(u32 seed)
    {
        using namespace Detail;

        Pattern pattern{};
        Energy energy{};

        // --- Initial binary pattern -------------------------------------------
        // Ulichney starts from roughly a tenth set. The exact fraction barely
        // matters: the swap loop below erases the starting configuration's
        // structure entirely. What it must not be is 0 or all.
        constexpr u32 initialOnes = kTilePixels / 10;
        // Plain `seed` — SplitMix32 avalanches it internally now. Pre-multiplying
        // by the generator's own increment is exactly the collision documented
        // on the class above.
        SplitMix32 rng(seed);
        for (u32 placed = 0; placed < initialOnes;)
        {
            const u32 candidate = rng.NextBelow(kTilePixels);
            if (pattern[candidate] != 0u)
                continue;
            pattern[candidate] = 1u;
            SplatKernel(energy, candidate, 1);
            ++placed;
        }

        // --- Relax to the prototype -------------------------------------------
        // Move the tightest cluster into the largest void, repeatedly. When the
        // void that opens up IS the cluster we just removed, the pattern is a
        // fixed point and no further move can improve it.
        //
        // The iteration bound is a safety net, not the termination condition:
        // pathological float ties could otherwise cycle, and a renderer-init
        // path must not be able to hang.
        for (u32 iteration = 0; iteration < kTilePixels * 4; ++iteration)
        {
            const u32 cluster = FindTightestCluster(energy, pattern);
            pattern[cluster] = 0u;
            SplatKernel(energy, cluster, -1);

            const u32 gap = FindLargestVoid(energy, pattern);
            if (gap == cluster)
            {
                // Put it back and stop — the pattern is stable.
                pattern[cluster] = 1u;
                SplatKernel(energy, cluster, 1);
                break;
            }

            pattern[gap] = 1u;
            SplatKernel(energy, gap, 1);
        }

        const Pattern prototype = pattern;
        RankTile ranks{};

        // --- Phase 1: ranks below initialOnes ---------------------------------
        // Strip the prototype one pixel at a time, most crowded first, and give
        // each the next rank down. So the LOWEST ranks are the pixels that were
        // most isolated — the ones a small prefix of the ranking should contain.
        for (u32 remaining = initialOnes; remaining > 0; --remaining)
        {
            const u32 cluster = FindTightestCluster(energy, pattern);
            pattern[cluster] = 0u;
            SplatKernel(energy, cluster, -1);
            ranks[cluster] = remaining - 1u;
        }

        // --- Phases 2 and 3: ranks from initialOnes upward --------------------
        // Restore the prototype and grow it instead, always into the largest
        // void. See the header comment for why the two published phases collapse
        // into one loop here.
        pattern = prototype;
        energy.fill(0);
        for (u32 i = 0; i < kTilePixels; ++i)
        {
            if (pattern[i] != 0u)
                SplatKernel(energy, i, 1);
        }

        for (u32 rank = initialOnes; rank < kTilePixels; ++rank)
        {
            const u32 gap = FindLargestVoid(energy, pattern);
            pattern[gap] = 1u;
            SplatKernel(energy, gap, 1);
            ranks[gap] = rank;
        }

        return ranks;
    }

    // @brief The uploadable tile: kChannels interleaved bytes per pixel.
    //
    // Two channels of INDEPENDENT blue noise (different seeds), because the
    // shader's 2D sample takes one component from each. Deriving the second
    // from the first — fract(first * golden) and friends — collapses to a short
    // ramp over the quantised first channel and biases every sample; see
    // docs/agent-rules/glsl-shaders.md section 11, where exactly that shipped.
    using TileBytes = std::array<u8, static_cast<sizet>(kTilePixels) * kChannels>;

    // The tile the renderer uploads. Fixed seeds so the tile is a constant of
    // the engine: any golden or perceptual A/B captured against it stays valid.
    [[nodiscard]] inline TileBytes GenerateTileRG()
    {
        const RankTile r = GenerateRankTile(0x1u);
        const RankTile g = GenerateRankTile(0x2u);

        TileBytes bytes{};
        for (u32 i = 0; i < kTilePixels; ++i)
        {
            // Rank -> byte. The (rank * 255 + half) / (pixels - 1) form maps the
            // extreme ranks to the extreme bytes, so the tile actually spans
            // [0, 255] rather than stopping short — a range that does not reach
            // 1.0 biases every sample it rotates.
            constexpr u32 denom = kTilePixels - 1u;
            bytes[static_cast<sizet>(i) * kChannels + 0] = static_cast<u8>((r[i] * 255u + denom / 2u) / denom);
            bytes[static_cast<sizet>(i) * kChannels + 1] = static_cast<u8>((g[i] * 255u + denom / 2u) / denom);
        }
        return bytes;
    }

    // @brief Process-wide cached tile. Generation is deterministic and takes
    // roughly a tenth of a second in Debug, so it is computed once on first use
    // and shared by every pass that uploads it.
    //
    // Deliberately POD — no GPU handle, no Ref<T>, nothing with a destruction
    // order. A shared lazy static that owns a GL object is the shape
    // docs/agent-rules/lazy-static-release-ownership.md is about; this one
    // cannot leak a resource because it holds none. Each pass uploads its own
    // texture from these bytes (32 KB of VRAM apiece), which is also what keeps
    // the tile out of any per-frame global-bind ordering question.
    [[nodiscard]] inline const TileBytes& GetTileRG()
    {
        static const TileBytes s_Tile = GenerateTileRG();
        return s_Tile;
    }
} // namespace OloEngine::BlueNoise
