#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"

#include <span>

// AlphaCoverageMips.h — a mip chain for an alpha-TESTED texture that keeps the
// fraction of texels passing the alpha test the same at every level (issue #1441).
//
// A box filter averages alpha, and an alpha test thresholds it. The two do not
// commute: a grass blade one texel wide at level 0 is a column of 255s beside
// columns of 0s, level 1 averages it to 128, level 2 to 64, and at a 0.5 cutoff
// level 2 discards the blade entirely. An alpha-cutout plant therefore thins out
// with distance on any backend that samples a real mip chain — which OpenGL did
// not, for a file-loaded texture, until #1441 gave it one. Vulkan always did.
//
// The fix is Castaño's ("Computing Alpha Mipmaps", 2010): build the chain with an
// ordinary box filter, then scale each level's alpha by the one factor that makes
// its passing fraction match level 0's. Colour is filtered exactly as the GPU's
// own generator does (sRGB-decoded for an sRGB texture), so only alpha differs
// from the chain the texture would otherwise get.
//
// The cutoff is the CONSUMER's (Material::GetAlphaCutoff, FoliageLayer::AlphaCutoff),
// not the texture's, which is why this is opt-in per texture through
// Texture2D::SetAlphaCoverageCutoff rather than applied to every RGBA file: an
// alpha-BLENDED texture must keep its averaged alpha, and a cutoff chosen by
// guessing would change what a translucent surface looks like at distance.
//
// Pure CPU, RGBA8 only. The comparison is the shaders': a fragment is discarded
// when `alpha < cutoff`, with alpha the UNORM byte / 255.

namespace OloEngine::AlphaCoverageMips
{
    // A cutoff that can drive preservation: finite and in (0, 1]. Anything else
    // (0, negative, NaN, above 1) returns 0, which means "plain box chain".
    [[nodiscard("Store this!")]] f32 SanitizeCutoff(f32 cutoff) noexcept;

    // Fraction of `rgba`'s texels whose alpha survives `alpha < cutoff`.
    // 0 for an empty span.
    [[nodiscard("Store this!")]] f32 Coverage(std::span<const u8> rgba, f32 cutoff) noexcept;

    // Scales `rgba`'s alpha in place (saturating at 255) by the single factor
    // whose resulting coverage is closest to `targetCoverage`. Colour is untouched.
    // Returns the factor applied; 1 when nothing was changed.
    f32 ScaleAlphaToCoverage(std::span<u8> rgba, f32 cutoff, f32 targetCoverage) noexcept;

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
    // Rescaling alpha cannot help a level too small to hold the coverage. A
    // 1x1 level passes all or nothing, so an 18% grass card is either a solid
    // square or gone, and the nearest answer is "gone"; 2x2 and 4x4 quantise
    // to 1/4 and 1/16. A distant card a few pixels wide selects exactly those
    // levels, which is where #1441's foliage thinned: with the full chain the
    // far-pose screen coverage of FoliageLodCoverageEvidenceTest fell from
    // 2.78% (one level) to 0.43%. Stopping the chain measured 2.13% at 8,
    // 2.53% at 16, 2.48% at 32 and 2.56% at 64.
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
    // so it gets neither the rescale nor the level cap: its full chain costs
    // nothing in coverage and it would only alias without it.
    [[nodiscard("Store this!")]] bool HasPartialCoverage(std::span<const u8> rgba, f32 cutoff) noexcept;

    // The extent of the level below a `size` texel axis, as GL and Vulkan both
    // define it: max(1, size / 2).
    [[nodiscard("Store this!")]] constexpr u32 NextLevelSize(u32 size) noexcept
    {
        return size > 1u ? size / 2u : 1u;
    }

    // Box-filters `level0` (width * height RGBA8) down to `mipLevels` levels,
    // area-weighted so an odd extent loses no texels, colour averaged in linear
    // light when `srgb`. With a sanitized `cutoff` above 0 each level's alpha is
    // then rescaled to level 0's coverage; with 0 it is the plain chain.
    // Each level is filtered from the previous level BEFORE its rescale, so the
    // scale factors do not compound down the chain.
    [[nodiscard("Store this!")]] Chain Build(std::span<const u8> level0, u32 width, u32 height, u32 mipLevels, bool srgb,
                                             f32 cutoff);
} // namespace OloEngine::AlphaCoverageMips
