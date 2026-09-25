#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"

#include <span>

// AlphaCoverageMips.h — a mip chain for an alpha-TESTED texture that keeps the
// fraction of texels passing the alpha test the same at every level, for EVERY
// cutoff at once (issues #1441, #1453).
//
// A box filter averages alpha, and an alpha test thresholds it. The two do not
// commute: a grass blade one texel wide at level 0 is a column of 255s beside
// columns of 0s, level 1 averages it to 128, level 2 to 64, and at a 0.5 cutoff
// level 2 discards the blade entirely. An alpha-cutout plant therefore thins out
// with distance on any backend that samples a real mip chain.
//
// The chain is HISTOGRAM-MATCHED. Each level is box-filtered from the one above
// it as usual, and then its alpha values are replaced, in rank order, by level
// 0's: the texel whose footprint held the most alpha gets level 0's largest
// alpha, and so on down. So for every threshold c the fraction of texels with
// alpha >= c equals level 0's, to within one texel of the level. A binary card
// stays binary, a soft edge keeps its ramp, and nothing depends on a cutoff:
//
//   - the COOK (TextureCompression) builds it with no material in scope, so one
//     cooked chain serves a Mask material at 0.25 and a foliage layer at 0.5;
//   - a texture SHARED between consumers with different cutoffs (pine_card at
//     0.5 / 0.3 / 0.25) is right for all of them, where a per-cutoff chain was
//     right only for the consumer that set it last.
//
// #1441 shipped Castaño's per-cutoff scale ("Computing Alpha Mipmaps", 2010)
// instead. #1453 measured both on the vegetation cards and grass.png, before and
// after BC7 encode (AlphaCoverageMipsContractTest pins the texel numbers): at the
// cutoff it was built for Castaño is as close as this chain, and at any OTHER
// cutoff it drifts by 3-10 coverage points where this chain drifts by < 0.1.
//
// Colour is filtered exactly as the GPU's own generator does (sRGB-decoded for
// an sRGB texture), so only alpha differs from the chain the texture would
// otherwise get. Pure CPU, RGBA8 only. The comparison is the shaders': a fragment
// is discarded when `alpha < cutoff`, with alpha the UNORM byte / 255.

namespace OloEngine::AlphaCoverageMips
{
    // A cutoff that marks a texture as alpha-tested: finite and in (0, 1].
    // Anything else (0, negative, NaN, above 1) returns 0, which means "not
    // alpha-tested, plain box chain".
    [[nodiscard("Store this!")]] f32 SanitizeCutoff(f32 cutoff) noexcept;

    // Fraction of `rgba`'s texels whose alpha survives `alpha < cutoff`.
    // 0 for an empty span.
    [[nodiscard("Store this!")]] f32 Coverage(std::span<const u8> rgba, f32 cutoff) noexcept;

    struct Level
    {
        u32 Width = 0;
        u32 Height = 0;
        u64 Offset = 0; // byte offset into Chain::Bytes
    };

    // Levels 1..mipLevels-1, tightly packed RGBA8, in order. Level 0 is the
    // caller's and is not repeated here.
    struct Chain
    {
        TArray64<u8> Bytes;
        TArray<Level> Levels;
    };

    // The coarsest level an alpha-tested chain keeps: its SHORTER side is at
    // least this many texels.
    //
    // No remapping can help a level too small to hold the coverage. A 1x1
    // level passes all or nothing, so an 18% grass card is either a solid
    // square or gone; 2x2 and 4x4 quantise to 1/4 and 1/16. A distant card a
    // few pixels wide selects exactly those levels, which is where #1441's
    // foliage thinned: with the full chain the far-pose screen coverage of
    // FoliageLodCoverageEvidenceTest fell from 2.78% (one level) to 0.43%.
    // Stopping the chain measured 2.13% at 8, 2.53% at 16, 2.48% at 32 and
    // 2.56% at 64.
    //
    // Coverage alone would pick 16; the LOOK picks 64. A grass tuft ten pixels
    // tall samples the level where the card is about ten texels, and at 16 or
    // 32 the blades there have merged into a solid blob with the right area
    // (FoliageInteraction_GL_*_Oblique, compared cropped and enlarged). At 64
    // the tips survive and the meadow reads as it did with one level. Past this
    // level the texture minifies from it, as a cutout without mips did from
    // level 0, with an eighth of the aliasing for a 512 texture.
    inline constexpr u32 kMinCoarsestExtent = 64;

    // How many of `levels` an alpha-tested texture keeps: through the last
    // level whose shorter side is >= kMinCoarsestExtent, and at least 1.
    [[nodiscard("Store this!")]] u32 CappedLevelCount(u32 width, u32 height, u32 levels) noexcept;

    // Whether `rgba` has coverage for a chain to preserve at `cutoff`: some
    // texels pass and some do not. A texture that passes everywhere (opaque
    // bark on a cutout material) or nowhere keeps that under any box filter,
    // so it gets neither the remap nor the level cap: its full chain costs
    // nothing in coverage and it would only alias without it.
    [[nodiscard("Store this!")]] bool HasPartialCoverage(std::span<const u8> rgba, f32 cutoff) noexcept;

    // Whether `rgba`'s alpha LOOKS like a cutout: some texels near-transparent
    // (<= 8), some near-opaque (>= 247), and at most a third in between.
    //
    // The cook's gate (TextureCompression), which has no consumer to say
    // "this is alpha-tested". Measured on the repository's textures (#1453):
    // every vegetation card and atlas has under 2% in between, the soft
    // cutouts (grass.png, the Sponza Mask PNGs, AlphaBlendLabels) 2-28%, and
    // data carried in alpha — pbr/wall/normal.png's height — 100%. A third
    // splits the two with room on both sides; a texture on the wrong side is
    // overridden by its ".oloimport" sidecar (AlphaMipChain).
    [[nodiscard("Store this!")]] bool IsCutoutAlpha(std::span<const u8> rgba) noexcept;

    // Level 0's alpha distribution resampled to `texelCount` ranks: entry r is
    // the alpha the texel ranked r-th (0 = the most alpha) must hold for a level
    // of that many texels to pass level 0's fraction at every cutoff, to the
    // nearest texel. Non-increasing. Build uses it; so does the cook, to put
    // back what the block encoder moves (TextureCompression::EncodeBC7).
    [[nodiscard("Store this!")]] TArray64<u8> RankedTargetAlpha(std::span<const u8> level0, u64 texelCount);

    // The largest difference, over every threshold, between the fraction of
    // `level`'s texels whose alpha reaches it and the fraction of `level0`'s.
    // 0 for identical distributions; one texel of `level` is the floor a
    // matched level can reach.
    [[nodiscard("Store this!")]] f32 WorstCoverageDifference(std::span<const u8> level, std::span<const u8> level0) noexcept;

    // The extent of the level below a `size` texel axis, as GL and Vulkan both
    // define it: max(1, size / 2).
    [[nodiscard("Store this!")]] constexpr u32 NextLevelSize(u32 size) noexcept
    {
        return size > 1u ? size / 2u : 1u;
    }

    // Box-filters `level0` (width * height RGBA8) down to `mipLevels` levels,
    // area-weighted so an odd extent loses no texels, colour averaged in linear
    // light when `srgb`. With `preserveCoverage` each level's alpha is then
    // histogram-matched to level 0's (see the top of this file); without it,
    // it is the plain chain.
    //
    // The ranking that decides which texels receive level 0's high alphas is
    // the footprint's exact average alpha (a float box chain from level 0, so
    // no rounding merges two footprints), ties broken by the 3x3 neighbourhood
    // of that average and then by texel order. The result is deterministic.
    //
    // Callers decide `preserveCoverage` (HasPartialCoverage at the consumer's
    // cutoff at runtime, IsCutoutAlpha in the cook) and cap `mipLevels`
    // (CappedLevelCount) themselves.
    [[nodiscard("Store this!")]] Chain Build(std::span<const u8> level0, u32 width, u32 height, u32 mipLevels, bool srgb,
                                             bool preserveCoverage);
} // namespace OloEngine::AlphaCoverageMips
