// OLO_TEST_LAYER: unit
// =============================================================================
// TerrainVirtualTextureTest.cpp
//
// The CPU half of terrain virtual texturing (issue #715, slices 1-4): the
// packings that are MIRRORED IN GLSL, the feedback reduction that runs on a
// Task worker, the page-cache residency behaviour built on #704's
// GPUPagedCache, and the adaptive per-sector image sizing and resize-remap
// policy slice 3 layered on top of them.
//
// Why these and not a golden image: every failure mode this feature has is a
// wrong ADDRESS, and a wrong address does not look like an error. A page key
// that packs the mip one bit off makes the terrain sample a different page — a
// plausible-looking, wrong texture. A physical-UV mapping that lands in a tile's
// border samples the neighbouring page's duplicated edge, which is CORRECT
// content in the wrong place. A fallback chain that does not terminate leaves
// one texel of the indirection map pointing at tile (0,0), which is whatever
// page happens to live there. None of those produce a black frame, and a
// screenshot diff would call all three "the terrain looks slightly different".
// The adaptive layer only raises the stakes: a resize that remaps a page to
// the wrong new key shows the right content at the wrong place or density, and
// a sizing policy that flaps re-bakes a sector every few frames — a stutter,
// not an image difference, so no golden would ever see it.
//
// The GLSL side of each mirrored packing is named in the test that pins it.
//
// Classification: unit (pure CPU, no GL context, no task system).
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/GPUCache/GPUCachePolicy.h"
#include "OloEngine/Renderer/GPUCache/GPUPagedCache.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTextureTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace OloEngine;

namespace
{
    TerrainVirtualTextureConfig MakeConfig()
    {
        TerrainVirtualTextureConfig config;
        config.VirtualPagesWide = 64;
        config.PageTexels = 32;
        config.BorderTexels = 4;
        config.CacheTilesWide = 8;
        config.MaxTileBakesPerFrame = 4;
        config.FeedbackDownscale = 8;
        // Small enough that the pinned-page floor (one page per allocated
        // sector) stays under half of this compact 64-tile cache — and of the
        // 16-tile one the delta equivalence test shrinks it to. The default
        // 8x8 grid needs a 256-tile cache and would fail IsValid() here.
        config.SectorsWide = 2;
        return config;
    }

    // The degenerate adaptive configuration: ONE image spanning the whole
    // atlas at a pinned size. This is exactly slices 1-2's fixed grid, running
    // through the adaptive code path rather than beside it — which is why the
    // pre-adaptivity analyzer tests below keep passing this instead of being
    // deleted: their survival IS the proof of that equivalence.
    VTSectorSnapshot MakeAtlasSpanningSector(u32 atlasPagesWide)
    {
        u16 maxMip = 0;
        for (u32 wide = atlasPagesWide; wide > 1u; wide >>= 1u)
        {
            ++maxMip;
        }
        VTSectorSnapshot sector;
        sector.m_SizePages = static_cast<u16>(atlasPagesWide);
        sector.m_MaxMip = maxMip;
        return sector;
    }

    // The CPU twin of what the fill kernel leaves behind: for a virtual page at
    // `mip`, the entry a lookup reads is the nearest RESIDENT ancestor. Used to
    // build indirection texels for the physical-UV tests without a GPU.
    VTIndirectionTexel ResolveWithFallback(const TerrainVirtualTextureConfig& config,
                                           const std::vector<u32>& residentKeys, u32 mip, u32 pageX, u32 pageY,
                                           bool& outFound)
    {
        for (u32 level = mip; level <= config.MaxMip(); ++level)
        {
            const u32 shift = level - mip;
            const u32 key = VTMakePageKey(level, pageX >> shift, pageY >> shift);
            const auto it = std::ranges::find(residentKeys, key);
            if (it != residentKeys.end())
            {
                const auto index = static_cast<u32>(std::distance(residentKeys.begin(), it));
                outFound = true;
                return VTIndirectionTexel{ static_cast<u8>(index % config.CacheTilesWide),
                                           static_cast<u8>(index / config.CacheTilesWide),
                                           static_cast<u8>(level), 255 };
            }
        }
        outFound = false;
        return VTIndirectionTexel{};
    }
} // namespace

// -----------------------------------------------------------------------------
// Packings. Each of these is a uint layout that also exists in GLSL; the pin is
// on the round trip, plus the bit position, because a shifted field still round
// trips within C++.
// -----------------------------------------------------------------------------

TEST(TerrainVirtualTexture, PageKeyRoundTripsAndUsesTheDocumentedBitPositions)
{
    // GLSL twin: the unpack in compute/TerrainVTTileBake.comp's main().
    for (u32 mip = 0; mip < kVTMaxMipCount; ++mip)
    {
        for (u32 x : { 0u, 1u, 1234u, 4095u })
        {
            for (u32 y : { 0u, 7u, 2048u, 4095u })
            {
                const u32 key = VTMakePageKey(mip, x, y);
                EXPECT_EQ(VTPageKeyMip(key), mip);
                EXPECT_EQ(VTPageKeyX(key), x);
                EXPECT_EQ(VTPageKeyY(key), y);
            }
        }
    }

    // The bit positions themselves, not just the round trip: the shader reads
    // `key & 0xFFF`, `(key >> 12) & 0xFFF`, `(key >> 24) & 0xF` literally.
    EXPECT_EQ(VTMakePageKey(0u, 1u, 0u), 0x1u);
    EXPECT_EQ(VTMakePageKey(0u, 0u, 1u), 0x1000u);
    EXPECT_EQ(VTMakePageKey(1u, 0u, 0u), 0x1000000u);
}

TEST(TerrainVirtualTexture, FeedbackWordCarriesMipZeroCoordinatesAndAWrittenFlag)
{
    // GLSL twin: oloVTPackFeedback() in include/TerrainVirtualTexture.glsl.
    EXPECT_FALSE(VTFeedbackWasWritten(0u));

    const u32 word = VTPackFeedback(1234u, 2345u, 5);
    EXPECT_TRUE(VTFeedbackWasWritten(word));
    EXPECT_EQ(word & 0xFFFu, 1234u);
    EXPECT_EQ((word >> 12u) & 0xFFFu, 2345u);
    // The stored field is the wanted mip PLUS ONE — value 0 is reserved for
    // the grow signal (the test below owns that contract).
    EXPECT_EQ((word >> 24u) & 0xFu, 6u);
    EXPECT_EQ(VTFeedbackMip(word), 5);

    // The high bit must survive a maximal payload — it is the ONLY thing that
    // distinguishes "this pixel asked for page (0,0) at mip 0" from "nothing
    // wrote here", and the analyzer skips on it.
    EXPECT_TRUE(VTFeedbackWasWritten(VTPackFeedback(4095u, 4095u, 14)));
    EXPECT_TRUE(VTFeedbackWasWritten(VTPackFeedback(0u, 0u, -1)));
}

TEST(TerrainVirtualTexture, TheFeedbackMipBiasReservesZeroForTheGrowSignal)
{
    // The +1 bias exists so the field can say something a plain mip cannot:
    // "this pixel resolved FINER than the sector's image provides" (a computed
    // mip below zero). That decoded -1 is the signal the adaptive layer grows
    // an image on — the reference encoded it and then clamped it away, which
    // is exactly what left its image sizes camera-distance-banded instead of
    // feedback-driven.
    for (i32 wanted = -1; wanted <= 14; ++wanted)
    {
        EXPECT_EQ(VTFeedbackMip(VTPackFeedback(7u, 9u, wanted)), wanted) << "wanted mip " << wanted;
    }

    // The grow signal occupies the raw field value 0 — the bit pattern the
    // GLSL packer writes for a below-zero computed mip.
    EXPECT_EQ((VTPackFeedback(0u, 0u, -1) >> 24u) & 0xFu, 0u);
    // A derivative can compute arbitrarily far below zero; anything under -1
    // must clamp to the grow signal rather than wrap into a huge biased value.
    EXPECT_EQ(VTFeedbackMip(VTPackFeedback(0u, 0u, -7)), -1);

    // The field saturates at its 4-bit ceiling: a wanted mip of 14 stores the
    // maximum biased value 15, and anything coarser decodes as 14 too. That
    // loss is unreachable in a valid config (a 4096-page atlas tops out at
    // mip 12) — pinned so a future widening has to update this line rather
    // than silently change the packing.
    EXPECT_EQ((VTPackFeedback(0u, 0u, 14) >> 24u) & 0xFu, 15u);
    EXPECT_EQ(VTFeedbackMip(VTPackFeedback(0u, 0u, 15)), 14);
    EXPECT_EQ(VTFeedbackMip(VTPackFeedback(0u, 0u, 100)), 14);
}

TEST(TerrainVirtualTexture, IndirectionTexelRoundTripsThroughItsRGBA8Layout)
{
    // GLSL twin: the byte extraction in compute/TerrainVTIndirectionWrite.comp
    // and oloVTLookup() in include/TerrainVirtualTexture.glsl.
    for (u32 tileX : { 0u, 1u, 200u, 255u })
    {
        for (u32 tileY : { 0u, 63u, 255u })
        {
            for (u32 mip : { 0u, 3u, 15u })
            {
                for (bool direct : { false, true })
                {
                    const VTIndirectionTexel texel = VTUnpackIndirection(VTPackIndirection(tileX, tileY, mip, direct));
                    EXPECT_EQ(texel.m_TileX, tileX);
                    EXPECT_EQ(texel.m_TileY, tileY);
                    EXPECT_EQ(texel.m_Mip, mip);
                    EXPECT_EQ(texel.m_Direct != 0, direct);
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

TEST(TerrainVirtualTexture, DefaultConfigurationIsValidAndReachesAOneByOneMip)
{
    const TerrainVirtualTextureConfig config;
    EXPECT_TRUE(config.IsValid());

    // The chain MUST reach 1x1: that page is what every unresolved lookup
    // ultimately inherits from, so a chain that stopped at 2x2 would leave four
    // quadrants with four separate terminating pages and no guarantee any of
    // them is resident.
    EXPECT_EQ(config.VirtualPagesWide >> config.MaxMip(), 1u);
    EXPECT_EQ(config.MipCount(), config.MaxMip() + 1u);

    // The density claim the feature exists for, stated as a number: 256 pages of
    // 128 texels is 32768 texels across the terrain, where the splatmap path
    // tops out at its splatmap resolution (512 by default).
    EXPECT_EQ(config.VirtualTexelsWide(), 32768u);

    // And the default must sanitize CLEAN — a default the sanitizer "repairs"
    // would make every Configure() of an untouched component report that it
    // rendered a different virtual texture than the scene asked for.
    TerrainVirtualTextureConfig sanitized;
    EXPECT_TRUE(sanitized.Sanitize());
    EXPECT_EQ(sanitized, TerrainVirtualTextureConfig{});
}

TEST(TerrainVirtualTexture, DisablingAdaptivityDegeneratesToOneAtlasSizedImage)
{
    // The fixed-grid path is a CONFIG of the adaptive code, not a second code
    // path: adaptivity off means one sector whose image is pinned at the full
    // atlas size. This is the contract every "degenerate config reproduces
    // slices 1-2" test below leans on, so it gets pinned once, here.
    TerrainVirtualTextureConfig config;
    config.AdaptiveEnabled = false;
    EXPECT_EQ(config.EffectiveSectorsWide(), 1u);
    EXPECT_EQ(config.EffectiveSectorCount(), 1u);
    EXPECT_EQ(config.EffectiveMinImagePagesWide(), config.VirtualPagesWide);
    EXPECT_EQ(config.EffectiveMaxImagePagesWide(), config.VirtualPagesWide);

    // With adaptivity on, the knobs pass straight through.
    const TerrainVirtualTextureConfig adaptive;
    EXPECT_EQ(adaptive.EffectiveSectorsWide(), adaptive.SectorsWide);
    EXPECT_EQ(adaptive.EffectiveSectorCount(), adaptive.SectorsWide * adaptive.SectorsWide);
    EXPECT_EQ(adaptive.EffectiveMinImagePagesWide(), adaptive.MinImagePagesWide);
    EXPECT_EQ(adaptive.EffectiveMaxImagePagesWide(), adaptive.MaxImagePagesWide);
}

TEST(TerrainVirtualTexture, AZeroCacheBorderIsRejectedAndRepaired)
{
    // The border is not padding. The physical cache is sampled with a LINEAR
    // filter, so a tap at a page edge reads a texel outside the page's interior;
    // the border is the only thing that makes that texel the true neighbouring
    // content instead of whichever unrelated page occupies the adjacent tile.
    // With zero border every page boundary in the terrain becomes a seam — and a
    // seam is exactly the kind of defect that reads as "the texture looks a bit
    // off" rather than as an error.
    TerrainVirtualTextureConfig config;
    config.BorderTexels = 0;
    EXPECT_FALSE(config.IsValid()) << "a zero border must not be accepted";

    EXPECT_FALSE(config.Sanitize()) << "Sanitize must report that it changed the border";
    EXPECT_GE(config.BorderTexels, 1u);
    EXPECT_TRUE(config.IsValid());

    // The upper bound still holds, and the two are independent.
    TerrainVirtualTextureConfig wide;
    wide.BorderTexels = 9999;
    EXPECT_FALSE(wide.IsValid());
    EXPECT_FALSE(wide.Sanitize());
    EXPECT_EQ(wide.BorderTexels, wide.PageTexels / 4u);
}

TEST(TerrainVirtualTexture, SanitizeRoundsToWhatThePackingsCanExpressAndReportsThatItDid)
{
    TerrainVirtualTextureConfig config;
    config.VirtualPagesWide = 300; // not a power of two
    config.PageTexels = 100;       // not a power of two
    config.BorderTexels = 999;     // past the quarter-page cap
    config.CacheTilesWide = 0;
    config.MaxTileBakesPerFrame = 0;
    config.FeedbackDownscale = 7;

    EXPECT_FALSE(config.Sanitize()) << "Sanitize must report that it changed something";
    EXPECT_TRUE(config.IsValid());
    EXPECT_EQ(config.VirtualPagesWide, 256u);
    EXPECT_EQ(config.PageTexels, 64u);
    EXPECT_EQ(config.BorderTexels, config.PageTexels / 4u);
    EXPECT_GE(config.CacheTilesWide, 2u);
    EXPECT_GE(config.MaxTileBakesPerFrame, 1u);
    EXPECT_EQ(config.FeedbackDownscale, 4u);

    // Idempotent: a sanitized config must survive a second pass unchanged, or a
    // caller that re-sanitizes on every Configure() would warn every frame.
    TerrainVirtualTextureConfig again = config;
    EXPECT_TRUE(again.Sanitize());
    EXPECT_EQ(again, config);
}

TEST(TerrainVirtualTexture, TheSectorGridMustBeAPowerOfTwoNoWiderThanItsUBOTable)
{
    // The per-sector table rides the TerrainParams UBO as a FIXED array — the
    // UBO namespace has no free binding left for an SSBO — so the grid is
    // capped at kVTMaxSectorsWide at compile time, and a non-power-of-two
    // grid would break the sector lookup's shift arithmetic in the shader.
    TerrainVirtualTextureConfig odd;
    odd.SectorsWide = 3;
    EXPECT_FALSE(odd.IsValid());
    EXPECT_FALSE(odd.Sanitize());
    EXPECT_EQ(odd.SectorsWide, 2u);
    EXPECT_TRUE(odd.IsValid());

    TerrainVirtualTextureConfig wide;
    wide.SectorsWide = 16;
    EXPECT_FALSE(wide.IsValid());
    EXPECT_FALSE(wide.Sanitize());
    EXPECT_EQ(wide.SectorsWide, kVTMaxSectorsWide);
    EXPECT_TRUE(wide.IsValid());
}

TEST(TerrainVirtualTexture, ImageSizeBoundsMustBePowersOfTwoNestedInsideTheAtlas)
{
    // An image wider than the atlas cannot be allocated at all; a min above
    // the max makes the sizing policy oscillate against an empty range. Both
    // are config-load mistakes, so both must be caught before the allocator
    // sees them.
    TerrainVirtualTextureConfig oversized;
    oversized.VirtualPagesWide = 64;
    oversized.MaxImagePagesWide = 128;
    EXPECT_FALSE(oversized.IsValid());
    EXPECT_FALSE(oversized.Sanitize());
    EXPECT_EQ(oversized.MaxImagePagesWide, oversized.VirtualPagesWide);
    EXPECT_TRUE(oversized.IsValid());

    TerrainVirtualTextureConfig crossed;
    crossed.MinImagePagesWide = 32;
    crossed.MaxImagePagesWide = 16;
    EXPECT_FALSE(crossed.IsValid());
    EXPECT_FALSE(crossed.Sanitize());
    EXPECT_LE(crossed.MinImagePagesWide, crossed.MaxImagePagesWide);
    EXPECT_TRUE(crossed.IsValid());

    TerrainVirtualTextureConfig notPow2;
    notPow2.MinImagePagesWide = 3;
    EXPECT_FALSE(notPow2.IsValid());
    EXPECT_FALSE(notPow2.Sanitize());
    EXPECT_EQ(notPow2.MinImagePagesWide, 2u);
    EXPECT_TRUE(notPow2.IsValid());
}

TEST(TerrainVirtualTexture, TheAllocatorsTwelveLevelCeilingRaisesTheMinimumImageSize)
{
    // AtlasAllocator preallocates its whole quadtree and caps the level count
    // at 12: one level past that it silently constructs with ZERO capacity
    // and every Allocate returns kInvalidNode — no assert, no log, just a
    // terrain that never gets a single image. So atlas/minImage must stay at
    // or under 2048, and Sanitize repairs it by raising the MINIMUM — the
    // atlas is the address space the scene sized deliberately.
    TerrainVirtualTextureConfig config;
    config.VirtualPagesWide = 4096;
    ASSERT_EQ(config.MinImagePagesWide, 1u) << "the default minimum is what trips the ratio";
    EXPECT_FALSE(config.IsValid());
    EXPECT_FALSE(config.Sanitize());
    EXPECT_EQ(config.MinImagePagesWide, 2u) << "bumped up to bring the ratio back to 2048";
    EXPECT_EQ(config.VirtualPagesWide, 4096u) << "the atlas itself must not be the field that gives";
    EXPECT_TRUE(config.IsValid());

    // Exactly 2048:1 is the allocator's last valid shape, not past it.
    TerrainVirtualTextureConfig edge;
    edge.VirtualPagesWide = 2048;
    EXPECT_TRUE(edge.IsValid());
}

TEST(TerrainVirtualTexture, PinnedSectorImagesMayClaimAtMostHalfTheCache)
{
    // Every allocated sector keeps its coarsest page permanently resident, so
    // the sector count is a hard floor on cache occupancy. Past half the
    // cache, servicing has too little evictable room left to stream what the
    // camera actually looks at — the terrain would converge to pins plus
    // almost nothing.
    TerrainVirtualTextureConfig config;
    config.CacheTilesWide = 8; // 64 tiles; the default 8x8 grid would pin 64 of them
    EXPECT_FALSE(config.IsValid());
    EXPECT_FALSE(config.Sanitize());
    EXPECT_EQ(config.SectorsWide, 4u);
    EXPECT_LE(config.SectorsWide * config.SectorsWide, config.CacheTileCount() / 2u);
    EXPECT_TRUE(config.IsValid());
}

TEST(TerrainVirtualTexture, ACompressedCacheRoundsAnOddBorderUpToTheNextEvenValue)
{
    // BC blocks are 4x4 texels. Tile origins in the cache atlas sit at
    // multiples of TileTexels(), so the block-compatible copy stays aligned
    // iff the tile edge is a multiple of 4 — PageTexels is a power of two
    // >= 8, which leaves the border needing to be even.
    TerrainVirtualTextureConfig config;
    config.BorderTexels = 5;
    ASSERT_TRUE(config.CompressedCache);
    EXPECT_FALSE(config.IsValid());

    // The same border is fine when the cache stays uncompressed — the rule is
    // about block alignment, not about the border itself.
    TerrainVirtualTextureConfig uncompressed = config;
    uncompressed.CompressedCache = false;
    EXPECT_TRUE(uncompressed.IsValid());

    // Sanitize rounds UP, never down: the border exists to keep linear
    // filtering inside the tile, so shrinking it is the direction that can
    // create seams.
    EXPECT_FALSE(config.Sanitize());
    EXPECT_EQ(config.BorderTexels, 6u);
    EXPECT_TRUE(config.IsValid());
}

// -----------------------------------------------------------------------------
// Coordinate math. These are the tests that would catch a tile addressed one
// border-width off, which is the failure that looks like "the terrain texture
// has faint seams" rather than like a bug.
// -----------------------------------------------------------------------------

TEST(TerrainVirtualTexture, PageUVRectsTileTheTerrainExactlyAtEveryMip)
{
    const TerrainVirtualTextureConfig config = MakeConfig();

    for (u32 mip = 0; mip <= config.MaxMip(); ++mip)
    {
        const u32 pages = config.VirtualPagesWide >> mip;
        const glm::vec4 first = VTPageUVRect(config, mip, 0, 0);
        const glm::vec4 last = VTPageUVRect(config, mip, pages - 1u, pages - 1u);

        EXPECT_NEAR(first.x, 0.0f, 1e-6f);
        EXPECT_NEAR(first.y, 0.0f, 1e-6f);
        EXPECT_NEAR(last.z, 1.0f, 1e-6f);
        EXPECT_NEAR(last.w, 1.0f, 1e-6f);

        // Adjacent pages share an edge exactly — a gap or an overlap here is
        // duplicated or missing terrain content in the bake.
        for (u32 x = 0; x + 1u < pages; ++x)
        {
            const glm::vec4 left = VTPageUVRect(config, mip, x, 0);
            const glm::vec4 right = VTPageUVRect(config, mip, x + 1u, 0);
            EXPECT_NEAR(left.z, right.x, 1e-6f);
        }
    }
}

TEST(TerrainVirtualTexture, BorderedRectExtendsThePageByExactlyTheBorderDensity)
{
    const TerrainVirtualTextureConfig config = MakeConfig();

    const glm::vec4 plain = VTPageUVRect(config, 2, 3, 5);
    const glm::vec4 bordered = VTPageUVRectWithBorder(config, 2, 3, 5);

    const f32 pageSpan = plain.z - plain.x;
    const f32 expectedGrow = pageSpan * static_cast<f32>(config.BorderTexels) / static_cast<f32>(config.PageTexels);

    EXPECT_NEAR(bordered.x, plain.x - expectedGrow, 1e-6f);
    EXPECT_NEAR(bordered.z, plain.z + expectedGrow, 1e-6f);
    // The border carries the SAME texel density as the interior; if it did not,
    // a linear tap that crosses the seam would blend two different densities and
    // the seam would be visible as a sharpness change rather than a colour one.
    const f32 borderedSpan = bordered.z - bordered.x;
    const f32 texelsAcross = static_cast<f32>(config.TileTexels());
    EXPECT_NEAR(borderedSpan / texelsAcross, pageSpan / static_cast<f32>(config.PageTexels), 1e-6f);
}

TEST(TerrainVirtualTexture, PhysicalUVLandsInsideTheOwningTileInterior)
{
    const TerrainVirtualTextureConfig config = MakeConfig();

    // A page at mip 0, mapped to an arbitrary tile.
    constexpr u32 kMip = 0;
    constexpr u32 kPageX = 5;
    constexpr u32 kPageY = 3;
    const VTIndirectionTexel texel{ 6, 2, static_cast<u8>(kMip), 255 };

    const f32 span = 1.0f / static_cast<f32>(config.VirtualPagesWide >> kMip);
    const f32 tileTexels = static_cast<f32>(config.TileTexels());
    const f32 cacheTexels = static_cast<f32>(config.CacheTexels());
    const f32 interiorMin = (static_cast<f32>(texel.m_TileX) * tileTexels + static_cast<f32>(config.BorderTexels)) /
                            cacheTexels;
    const f32 interiorMax = (static_cast<f32>(texel.m_TileX) * tileTexels + static_cast<f32>(config.BorderTexels) +
                             static_cast<f32>(config.PageTexels)) /
                            cacheTexels;

    // Sweep the page's own UV range. Every sample must land in the tile's
    // INTERIOR, never in its border and never in a neighbouring tile — landing
    // in the border reads correct-looking content belonging to the wrong page.
    for (i32 step = 0; step <= 64; ++step)
    {
        const f32 t = static_cast<f32>(step) / 64.0f;
        const glm::vec2 virtualUV((static_cast<f32>(kPageX) + t) * span, (static_cast<f32>(kPageY) + t) * span);
        const glm::vec2 physical = VTVirtualToPhysicalUV(config, virtualUV, texel);

        EXPECT_GE(physical.x, interiorMin - 1e-6f) << "step " << step;
        EXPECT_LE(physical.x, interiorMax + 1e-6f) << "step " << step;
    }

    // The two endpoints pin the mapping exactly rather than merely bounding it.
    const glm::vec2 atPageStart = VTVirtualToPhysicalUV(
        config, glm::vec2(static_cast<f32>(kPageX) * span, static_cast<f32>(kPageY) * span), texel);
    EXPECT_NEAR(atPageStart.x, interiorMin, 1e-6f);
}

TEST(TerrainVirtualTexture, ACoarserResidentPageIsAddressedAtItsOwnMip)
{
    // THE FALLBACK, as arithmetic. A lookup that wanted mip 0 but resolved to a
    // mip-2 page must read that page's correct sixteenth — if the page-local
    // coordinate were evaluated at the REQUESTED mip instead of the resident
    // one, every fine lookup would read the coarse page's top-left corner and
    // the terrain would show a repeating stamp.
    const TerrainVirtualTextureConfig config = MakeConfig();

    constexpr u32 kResidentMip = 2;
    constexpr u32 kFinePageX = 13; // inside coarse page 13 >> 2 == 3
    constexpr u32 kFinePageY = 9;  // inside coarse page 9 >> 2 == 2

    const VTIndirectionTexel texel{ 1, 1, static_cast<u8>(kResidentMip), 0 };

    const f32 fineSpan = 1.0f / static_cast<f32>(config.VirtualPagesWide);
    // Centre of the fine page.
    const glm::vec2 virtualUV((static_cast<f32>(kFinePageX) + 0.5f) * fineSpan,
                              (static_cast<f32>(kFinePageY) + 0.5f) * fineSpan);

    const glm::vec2 physical = VTVirtualToPhysicalUV(config, virtualUV, texel);

    // Where the fine page sits inside its mip-2 ancestor: 1/4 of the ancestor
    // per axis, at offset (13 & 3, 9 & 3) = (1, 1), plus the half-page centre.
    const f32 expectedLocalX = (static_cast<f32>(kFinePageX & 3u) + 0.5f) / 4.0f;
    const f32 tileTexels = static_cast<f32>(config.TileTexels());
    const f32 cacheTexels = static_cast<f32>(config.CacheTexels());
    const f32 expectedX = (static_cast<f32>(texel.m_TileX) * tileTexels + static_cast<f32>(config.BorderTexels) +
                           expectedLocalX * static_cast<f32>(config.PageTexels)) /
                          cacheTexels;
    EXPECT_NEAR(physical.x, expectedX, 1e-5f);
}

TEST(TerrainVirtualTexture, MipSelectionFollowsTexelsPerPixel)
{
    // GLSL twin: oloVTComputeMip(). One virtual texel per pixel is mip 0; each
    // doubling of the footprint is one mip coarser.
    EXPECT_NEAR(VTComputeMip(glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 1.0f)), 0.0f, 1e-5f);
    EXPECT_NEAR(VTComputeMip(glm::vec2(2.0f, 0.0f), glm::vec2(0.0f, 2.0f)), 1.0f, 1e-5f);
    EXPECT_NEAR(VTComputeMip(glm::vec2(16.0f, 0.0f), glm::vec2(0.0f, 16.0f)), 4.0f, 1e-5f);
    // Anisotropic footprints take the LONGER axis, which over-blurs rather than
    // under-sampling — the safe direction for a cache with no mips.
    EXPECT_NEAR(VTComputeMip(glm::vec2(8.0f, 0.0f), glm::vec2(0.0f, 1.0f)), 3.0f, 1e-5f);
    // A pixel whose UV does not move must not produce -inf.
    EXPECT_TRUE(std::isfinite(VTComputeMip(glm::vec2(0.0f), glm::vec2(0.0f))));
}

// -----------------------------------------------------------------------------
// Adaptive virtual images (slice 3): sector snapshots, the resize remap, and
// the per-sector terrain UV rects. Address arithmetic again, with the same
// failure grammar: an ownership test that answers wrong bakes terrain content
// into another sector's image, and a remap that lands one page off shows the
// RIGHT content in the WRONG place — both plausible frames, neither an error.
// -----------------------------------------------------------------------------

TEST(TerrainVirtualTexture, ASectorOwnsExactlyThePagesOfItsOwnPyramid)
{
    // Contains() is the validity check every stale feedback word and every
    // serviced request is filtered through: after a resize moves an image,
    // in-flight feedback still names pages of the OLD rect, and a request no
    // live image owns must be dropped, not baked.
    const VTSectorSnapshot sector{ 32, 32, 32, 5 };

    // Inside, at several levels of the pyramid.
    EXPECT_TRUE(sector.Contains(VTMakePageKey(0u, 32u, 32u)));
    EXPECT_TRUE(sector.Contains(VTMakePageKey(0u, 63u, 63u)));
    EXPECT_TRUE(sector.Contains(VTMakePageKey(2u, 8u, 8u)));
    EXPECT_TRUE(sector.Contains(VTMakePageKey(2u, 15u, 15u)));
    EXPECT_TRUE(sector.Contains(sector.PinKey()));

    // One page past the far edge, at two levels.
    EXPECT_FALSE(sector.Contains(VTMakePageKey(0u, 64u, 32u)));
    EXPECT_FALSE(sector.Contains(VTMakePageKey(0u, 32u, 64u)));
    EXPECT_FALSE(sector.Contains(VTMakePageKey(2u, 16u, 8u)));

    // BELOW the origin. The local coordinate is computed in unsigned
    // arithmetic, so a below-origin page UNDERFLOWS — the wrap must read as
    // "outside", not as a huge coordinate that happens to compare as inside.
    EXPECT_FALSE(sector.Contains(VTMakePageKey(0u, 31u, 32u)));
    EXPECT_FALSE(sector.Contains(VTMakePageKey(0u, 0u, 0u)));

    // A mip the image does not have. ABOVE its own coarsest level an image's
    // atlas texels are shared with its allocator siblings, so a coarser key
    // is another image's business even when the x/y would land inside.
    EXPECT_FALSE(sector.Contains(VTMakePageKey(6u, 0u, 0u)));

    // An unallocated sector owns nothing at all.
    const VTSectorSnapshot unallocated{};
    EXPECT_FALSE(unallocated.Contains(VTMakePageKey(0u, 0u, 0u)));
    EXPECT_FALSE(unallocated.Contains(VTMakePageKey(0u, 32u, 32u)));
}

TEST(TerrainVirtualTexture, ThePinKeyNamesTheImagesOwnCoarsestPage)
{
    // The per-sector equivalent of slices 1-2's single pinned page: the one
    // page every fallback chain inside the image terminates in. The origin is
    // always a multiple of the size (AtlasAllocator's buddy structure
    // guarantees it), so origin >> maxMip lands exactly on the coarsest
    // level's grid rather than between two of its pages.
    const VTSectorSnapshot sector{ 64, 32, 32, 5 };
    EXPECT_EQ(sector.PinKey(), VTMakePageKey(5u, 2u, 1u));
    EXPECT_TRUE(sector.Contains(sector.PinKey()));

    // A single-page image pins ITSELF, at atlas mip 0 — the finest band. This
    // is the case the analyzer's pins-first sort exists for (pinned below).
    const VTSectorSnapshot tiny{ 7, 3, 1, 0 };
    EXPECT_EQ(tiny.PinKey(), VTMakePageKey(0u, 7u, 3u));

    // The degenerate whole-atlas image pins the same page slices 1-2 pinned
    // globally.
    EXPECT_EQ(MakeAtlasSpanningSector(256u).PinKey(), VTMakePageKey(8u, 0u, 0u));
}

TEST(TerrainVirtualTexture, GrowingAnImageKeepsEveryPageOverTheSameWorldRectAndDensity)
{
    // A resize is a REMAP, not a rebake: the power-of-two pyramids nest, so
    // the page covering a given world rect at a given texel density exists in
    // both allocations. The invariant that actually matters is therefore not
    // "the coordinates shifted correctly" but "the page's terrain UV rect is
    // UNCHANGED" — that rect is what the bake composited into the physical
    // tile, and the tile is not touched by the resize.
    TerrainVirtualTextureConfig config = MakeConfig();
    config.VirtualPagesWide = 128;
    config.SectorsWide = 4;
    constexpr u32 kSectorIndex = 6;

    const VTSectorSnapshot oldImage{ 32, 0, 32, 5 };
    const VTSectorSnapshot newImage{ 0, 64, 64, 6 };

    for (u32 mip = 0; mip <= oldImage.m_MaxMip; ++mip)
    {
        const u32 pages = static_cast<u32>(oldImage.m_SizePages) >> mip;
        for (u32 ly = 0; ly < pages; ++ly)
        {
            for (u32 lx = 0; lx < pages; ++lx)
            {
                const u32 oldKey =
                    VTMakePageKey(mip, (static_cast<u32>(oldImage.m_OriginX) >> mip) + lx,
                                  (static_cast<u32>(oldImage.m_OriginY) >> mip) + ly);
                const auto newKey = VTRemapPageKey(oldKey, oldImage, newImage);
                ASSERT_TRUE(newKey.has_value())
                    << "grow must preserve every page (mip " << mip << ", local " << lx << ", " << ly << ")";
                // One mip coarser in the doubled pyramid: same page count at
                // that level, so same world rect at the same density.
                EXPECT_EQ(VTPageKeyMip(*newKey), mip + 1u);
                ASSERT_TRUE(newImage.Contains(*newKey));

                const glm::vec4 before = VTPageTerrainUVRect(config, oldImage, kSectorIndex, oldKey);
                const glm::vec4 after = VTPageTerrainUVRect(config, newImage, kSectorIndex, *newKey);
                EXPECT_NEAR(before.x, after.x, 1e-6f);
                EXPECT_NEAR(before.y, after.y, 1e-6f);
                EXPECT_NEAR(before.z, after.z, 1e-6f) << "same span == same texel density";
            }
        }
    }

    // The pin remaps to the pin: the fallback terminator survives the resize
    // as itself, so the sector is never without one.
    const auto remappedPin = VTRemapPageKey(oldImage.PinKey(), oldImage, newImage);
    ASSERT_TRUE(remappedPin.has_value());
    EXPECT_EQ(*remappedPin, newImage.PinKey());
}

TEST(TerrainVirtualTexture, ShrinkingAnImageDropsExactlyTheFinestLevel)
{
    // The halved pyramid has no level at the old mip-0 density, so those
    // pages fall off and the caller evicts them; every coarser level still
    // exists, one mip finer in the new numbering. Dropping MORE than the
    // finest level would throw away baked tiles the new image could keep —
    // paid back as re-bakes; dropping LESS would leave a key the new image
    // cannot address.
    TerrainVirtualTextureConfig config = MakeConfig();
    config.VirtualPagesWide = 128;
    config.SectorsWide = 4;
    constexpr u32 kSectorIndex = 9;

    const VTSectorSnapshot oldImage{ 0, 0, 64, 6 };
    const VTSectorSnapshot newImage{ 96, 32, 32, 5 };

    for (u32 y = 0; y < 64u; ++y)
    {
        for (u32 x = 0; x < 64u; ++x)
        {
            ASSERT_FALSE(VTRemapPageKey(VTMakePageKey(0u, x, y), oldImage, newImage).has_value())
                << "old mip-0 page (" << x << ", " << y << ") has no density slot in the shrunk pyramid";
        }
    }

    for (u32 mip = 1; mip <= oldImage.m_MaxMip; ++mip)
    {
        const u32 pages = static_cast<u32>(oldImage.m_SizePages) >> mip;
        for (u32 ly = 0; ly < pages; ++ly)
        {
            for (u32 lx = 0; lx < pages; ++lx)
            {
                const u32 oldKey = VTMakePageKey(mip, lx, ly); // origin (0,0): local == atlas
                const auto newKey = VTRemapPageKey(oldKey, oldImage, newImage);
                ASSERT_TRUE(newKey.has_value()) << "mip " << mip << " must survive the shrink";
                EXPECT_EQ(VTPageKeyMip(*newKey), mip - 1u);
                ASSERT_TRUE(newImage.Contains(*newKey));

                const glm::vec4 before = VTPageTerrainUVRect(config, oldImage, kSectorIndex, oldKey);
                const glm::vec4 after = VTPageTerrainUVRect(config, newImage, kSectorIndex, *newKey);
                EXPECT_NEAR(before.x, after.x, 1e-6f);
                EXPECT_NEAR(before.y, after.y, 1e-6f);
                EXPECT_NEAR(before.z, after.z, 1e-6f);
            }
        }
    }

    const auto remappedPin = VTRemapPageKey(oldImage.PinKey(), oldImage, newImage);
    ASSERT_TRUE(remappedPin.has_value());
    EXPECT_EQ(*remappedPin, newImage.PinKey());
}

TEST(TerrainVirtualTexture, MovingAnImageTranslatesEveryPageWithoutChangingItsLevel)
{
    // Images do not only resize — the allocator can hand a same-size image a
    // different origin (a free-then-reallocate under fragmentation). That is
    // a pure translation: mip indices unchanged, local coordinates unchanged,
    // terrain rects unchanged. A translation that touched the mip would
    // silently re-density every resident page.
    TerrainVirtualTextureConfig config = MakeConfig();
    config.VirtualPagesWide = 128;
    config.SectorsWide = 4;
    constexpr u32 kSectorIndex = 3;

    const VTSectorSnapshot oldImage{ 0, 0, 32, 5 };
    const VTSectorSnapshot newImage{ 64, 64, 32, 5 };

    for (u32 mip = 0; mip <= oldImage.m_MaxMip; ++mip)
    {
        const u32 pages = static_cast<u32>(oldImage.m_SizePages) >> mip;
        for (u32 ly = 0; ly < pages; ++ly)
        {
            for (u32 lx = 0; lx < pages; ++lx)
            {
                const u32 oldKey = VTMakePageKey(mip, lx, ly);
                const auto newKey = VTRemapPageKey(oldKey, oldImage, newImage);
                ASSERT_TRUE(newKey.has_value());
                EXPECT_EQ(VTPageKeyMip(*newKey), mip);
                EXPECT_EQ(VTPageKeyX(*newKey), (64u >> mip) + lx);
                EXPECT_EQ(VTPageKeyY(*newKey), (64u >> mip) + ly);

                const glm::vec4 before = VTPageTerrainUVRect(config, oldImage, kSectorIndex, oldKey);
                const glm::vec4 after = VTPageTerrainUVRect(config, newImage, kSectorIndex, *newKey);
                EXPECT_NEAR(before.x, after.x, 1e-6f);
                EXPECT_NEAR(before.y, after.y, 1e-6f);
                EXPECT_NEAR(before.z, after.z, 1e-6f);
            }
        }
    }

    const auto remappedPin = VTRemapPageKey(oldImage.PinKey(), oldImage, newImage);
    ASSERT_TRUE(remappedPin.has_value());
    EXPECT_EQ(*remappedPin, newImage.PinKey());
}

TEST(TerrainVirtualTexture, ADegenerateOneSectorImageReproducesTheUniformPageGrid)
{
    // With one sector spanning the whole atlas, the terrain-side rect and the
    // atlas-side rect describe the same grid — VTPageTerrainUVRect must agree
    // with slices 1-2's VTPageUVRect exactly, because the bake kernel used to
    // DERIVE the rect from that uniform grid and now samples what the CPU
    // hands it. A disagreement here is the fixed-grid path regressing the
    // moment adaptivity ships, on scenes that never asked for it.
    TerrainVirtualTextureConfig config = MakeConfig();
    config.SectorsWide = 1;
    const VTSectorSnapshot whole = MakeAtlasSpanningSector(config.VirtualPagesWide);

    struct Probe
    {
        u32 m_Mip;
        u32 m_X;
        u32 m_Y;
    };
    for (const Probe& probe : { Probe{ 0, 0, 0 }, Probe{ 0, 63, 63 }, Probe{ 2, 5, 9 }, Probe{ 3, 7, 0 },
                                Probe{ 6, 0, 0 } })
    {
        const glm::vec4 uniform = VTPageUVRect(config, probe.m_Mip, probe.m_X, probe.m_Y); // (min, max)
        const glm::vec4 terrain =
            VTPageTerrainUVRect(config, whole, 0u, VTMakePageKey(probe.m_Mip, probe.m_X, probe.m_Y)); // (min, span)
        EXPECT_NEAR(terrain.x, uniform.x, 1e-6f) << "mip " << probe.m_Mip;
        EXPECT_NEAR(terrain.y, uniform.y, 1e-6f) << "mip " << probe.m_Mip;
        EXPECT_NEAR(terrain.z, uniform.z - uniform.x, 1e-6f) << "mip " << probe.m_Mip;
    }
}

TEST(TerrainVirtualTexture, ASectorsPageRectsTileItsShareOfTheTerrainExactly)
{
    // The multi-sector counterpart of PageUVRectsTileTheTerrainExactly: at any
    // mip, a sector's pages must partition the sector's own 1/SectorsWide
    // square of terrain UV with no gap and no overlap. A gap or overlap here
    // is duplicated or missing terrain content in the bake — and unlike the
    // uniform grid, the page span now depends on the sector's CURRENT image
    // size, so this is where a resize would first show a seam.
    TerrainVirtualTextureConfig config = MakeConfig();
    config.SectorsWide = 4;
    constexpr u32 kSectorIndex = 5;                // row 1, column 1 of the 4x4 grid
    const VTSectorSnapshot sector{ 32, 0, 16, 4 }; // atlas placement is the allocator's business, not the grid's

    const f32 sectorSpan = 1.0f / 4.0f;
    const glm::vec2 sectorMin(0.25f, 0.25f);

    for (u32 mip = 0; mip <= sector.m_MaxMip; ++mip)
    {
        const u32 pages = static_cast<u32>(sector.m_SizePages) >> mip;
        const auto rectAt = [&](u32 lx, u32 ly)
        {
            return VTPageTerrainUVRect(config, sector, kSectorIndex,
                                       VTMakePageKey(mip, (32u >> mip) + lx, (0u >> mip) + ly));
        };

        // The corners pin the sector's own UV square.
        const glm::vec4 first = rectAt(0u, 0u);
        const glm::vec4 last = rectAt(pages - 1u, pages - 1u);
        EXPECT_NEAR(first.x, sectorMin.x, 1e-6f) << "mip " << mip;
        EXPECT_NEAR(first.y, sectorMin.y, 1e-6f) << "mip " << mip;
        EXPECT_NEAR(last.x + last.z, sectorMin.x + sectorSpan, 1e-6f) << "mip " << mip;
        EXPECT_NEAR(last.y + last.z, sectorMin.y + sectorSpan, 1e-6f) << "mip " << mip;

        // Adjacent pages share an edge exactly, along both axes.
        for (u32 i = 0; i + 1u < pages; ++i)
        {
            const glm::vec4 left = rectAt(i, 0u);
            const glm::vec4 right = rectAt(i + 1u, 0u);
            EXPECT_NEAR(left.x + left.z, right.x, 1e-6f) << "mip " << mip << " column " << i;
            const glm::vec4 top = rectAt(0u, i);
            const glm::vec4 bottom = rectAt(0u, i + 1u);
            EXPECT_NEAR(top.y + top.z, bottom.y, 1e-6f) << "mip " << mip << " row " << i;
        }
    }
}

// -----------------------------------------------------------------------------
// Feedback analysis — the part that runs on a Task worker.
//
// The analyzer now judges every word against a sector-table snapshot
// (slice 3). The first tests below run it in the DEGENERATE configuration —
// one image spanning the whole atlas — which reproduces slices 1-2's fixed
// grid through the same code path; keeping them adapted rather than deleted is
// what proves that equivalence. The genuinely multi-sector behaviour is pinned
// after them.
// -----------------------------------------------------------------------------

TEST(TerrainVirtualTexture, AnalyzerPinsTheCoarsestPageEvenWithNoFeedbackAtAll)
{
    // Without this the fallback chain has no terminator, and a frame in which
    // the terrain was entirely off-screen would evict the one page every future
    // lookup depends on.
    const std::vector<VTSectorSnapshot> sectors{ MakeAtlasSpanningSector(256u) };

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze({}, sectors, 8u);

    ASSERT_EQ(analyzer.GetRequests().size(), 1u);
    EXPECT_EQ(analyzer.GetRequests()[0].m_PageKey, VTMakePageKey(8u, 0u, 0u));
    // At the maximum count: the count is what the pins-first sort keys on, so
    // anything lower would let camera traffic outrank the terminator.
    EXPECT_EQ(analyzer.GetRequests()[0].m_Count, std::numeric_limits<u32>::max());
    EXPECT_EQ(analyzer.GetWrittenTexelCount(), 0u);

    // An UNALLOCATED sector pins nothing — there is no image whose chain
    // needs terminating, and pinning page (0,0,0) of a freed rect would keep
    // whatever image now lives there permanently resident.
    const std::vector<VTSectorSnapshot> freed{ VTSectorSnapshot{} };
    VTFeedbackAnalyzer empty;
    empty.Analyze({}, freed, 8u);
    EXPECT_TRUE(empty.GetRequests().empty());
}

TEST(TerrainVirtualTexture, AnalyzerRequestsEachWantedPageAndItsParent)
{
    // The parent is what a lookup falls back to while the fine page is still
    // being composited. Not requesting it leaves a hole in the fallback chain
    // exactly where the camera is pointing, which is the page pop this design
    // exists to avoid.
    constexpr u32 kMaxMip = 8;
    const std::vector<VTSectorSnapshot> sectors{ MakeAtlasSpanningSector(256u) };
    const std::vector<u32> feedback{ VTPackFeedback(64u, 32u, 2) };

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, sectors, kMaxMip);

    const auto& requests = analyzer.GetRequests();
    EXPECT_EQ(analyzer.GetWrittenTexelCount(), 1u);
    EXPECT_EQ(analyzer.GetStaleTexelCount(), 0u) << "the whole-atlas sector owns every address";

    const auto has = [&requests](u32 key)
    { return std::ranges::any_of(requests, [key](const VTPageRequest& r)
                                 { return r.m_PageKey == key; }); };

    EXPECT_TRUE(has(VTMakePageKey(2u, 64u >> 2u, 32u >> 2u))) << "the page the pixel asked for";
    EXPECT_TRUE(has(VTMakePageKey(3u, 64u >> 3u, 32u >> 3u))) << "its parent, for the fallback";
    EXPECT_TRUE(has(VTMakePageKey(kMaxMip, 0u, 0u))) << "the pinned coarsest page";
}

TEST(TerrainVirtualTexture, AnalyzerDeduplicatesAndCountsRepeatedRequests)
{
    constexpr u32 kMaxMip = 8;
    const std::vector<VTSectorSnapshot> sectors{ MakeAtlasSpanningSector(256u) };
    std::vector<u32> feedback;
    feedback.reserve(40);
    for (i32 i = 0; i < 30; ++i)
    {
        feedback.push_back(VTPackFeedback(16u, 16u, 1)); // same page, 30 pixels
    }
    for (i32 i = 0; i < 10; ++i)
    {
        feedback.push_back(VTPackFeedback(200u, 200u, 1)); // a different page, 10 pixels
    }
    feedback.push_back(0u); // an unwritten texel

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, sectors, kMaxMip);

    EXPECT_EQ(analyzer.GetWrittenTexelCount(), 40u);

    const auto& requests = analyzer.GetRequests();
    const auto find = [&requests](u32 key) -> u32
    {
        const auto it = std::ranges::find_if(requests, [key](const VTPageRequest& r)
                                             { return r.m_PageKey == key; });
        return (it == requests.end()) ? 0u : it->m_Count;
    };

    EXPECT_EQ(find(VTMakePageKey(1u, 8u, 8u)), 30u);
    EXPECT_EQ(find(VTMakePageKey(1u, 100u, 100u)), 10u);
}

TEST(TerrainVirtualTexture, RequestsAreOrderedCoarsestFirstThenByDemand)
{
    // The bake budget is spent from the front of this list, so the order IS the
    // policy: a coarse page both covers more screen and is the prerequisite for
    // its children's fallback, so spending the budget on it first converges the
    // image faster than chasing the finest requests.
    //
    // In this degenerate one-sector config the pin lives at the atlas's own
    // coarsest mip, so the pins-first band and the mip-major order coincide —
    // the multi-sector case where they DIVERGE has its own test below.
    constexpr u32 kMaxMip = 8;
    const std::vector<VTSectorSnapshot> sectors{ MakeAtlasSpanningSector(256u) };
    std::vector<u32> feedback;
    feedback.push_back(VTPackFeedback(0u, 0u, 0));
    for (i32 i = 0; i < 5; ++i)
    {
        feedback.push_back(VTPackFeedback(128u, 128u, 0));
    }

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, sectors, kMaxMip);

    const auto& requests = analyzer.GetRequests();
    ASSERT_GE(requests.size(), 2u);

    EXPECT_EQ(VTPageKeyMip(requests.front().m_PageKey), kMaxMip) << "the pinned page must sort first";
    for (sizet i = 1; i < requests.size(); ++i)
    {
        const u32 prevMip = VTPageKeyMip(requests[i - 1].m_PageKey);
        const u32 mip = VTPageKeyMip(requests[i].m_PageKey);
        EXPECT_GE(prevMip, mip) << "mips must be non-increasing";
        if (prevMip == mip)
        {
            EXPECT_GE(requests[i - 1].m_Count, requests[i].m_Count) << "within a mip, demand must be non-increasing";
        }
    }
}

TEST(TerrainVirtualTexture, AnalyzerClampsAMipTheConfigNoLongerHas)
{
    // A feedback word from a frame before a config change (or from a corrupted
    // buffer) can name a mip past the current chain. Its ADDRESS is still
    // owned — it is the mip that is bogus — so it must clamp to the coarsest
    // level the owning image has, not be dropped as stale and not become a key
    // the page cache would map and the indirection write would stamp outside
    // the texture.
    const std::vector<VTSectorSnapshot> sectors{ MakeAtlasSpanningSector(16u) };
    const std::vector<u32> feedback{ VTPackFeedback(15u, 15u, 14) };

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, sectors, 4u);

    EXPECT_EQ(analyzer.GetStaleTexelCount(), 0u) << "an owned address with a bogus mip is clamped, not dropped";
    ASSERT_FALSE(analyzer.GetRequests().empty());
    for (const auto& request : analyzer.GetRequests())
    {
        EXPECT_LE(VTPageKeyMip(request.m_PageKey), 4u);
    }
}

TEST(TerrainVirtualTexture, AnalyzerAttributesEveryWordToTheSectorThatOwnsItsAddress)
{
    // Two images side by side in the atlas, a third sector freed. The
    // per-sector aggregates are what the sizing policy runs on, so a word
    // credited to the wrong sector is a wrong image size a few analyses
    // later — and a word landing in freed atlas space must be dropped and
    // COUNTED, because baking it would composite terrain content into
    // whichever image now occupies that space, while a persistently growing
    // stale count is how an addressing bug announces itself.
    const std::vector<VTSectorSnapshot> sectors{
        VTSectorSnapshot{ 0, 0, 64, 6 },  // sector 0: a 64-page image at the atlas origin
        VTSectorSnapshot{ 64, 0, 32, 5 }, // sector 1: a 32-page neighbour
        VTSectorSnapshot{},               // sector 2: freed
    };

    std::vector<u32> feedback;
    for (i32 i = 0; i < 3; ++i)
    {
        feedback.push_back(VTPackFeedback(10u, 10u, 2)); // sector 0, an ordinary mid-mip request
    }
    for (i32 i = 0; i < 2; ++i)
    {
        feedback.push_back(VTPackFeedback(80u, 16u, -1)); // sector 1, wanted finer than its mip 0
    }
    feedback.push_back(VTPackFeedback(70u, 4u, 9));    // sector 1, coarser than its image goes
    feedback.push_back(VTPackFeedback(200u, 200u, 0)); // nobody's — the freed-space case

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, sectors, 8u);

    EXPECT_EQ(analyzer.GetWrittenTexelCount(), 7u);
    EXPECT_EQ(analyzer.GetStaleTexelCount(), 1u);

    const auto& perSector = analyzer.GetSectorFeedback();
    ASSERT_EQ(perSector.size(), sectors.size());
    EXPECT_EQ(perSector[0].m_Requests, 3u);
    EXPECT_EQ(perSector[0].m_UnderResolved, 0u);
    EXPECT_EQ(perSector[0].m_FinestMipRequested, 2u);
    EXPECT_EQ(perSector[1].m_Requests, 3u);
    EXPECT_EQ(perSector[1].m_UnderResolved, 2u) << "the biased-zero words are sector 1's grow signal";
    EXPECT_EQ(perSector[1].m_FinestMipRequested, 0u);
    EXPECT_EQ(perSector[2].m_Requests, 0u) << "the stale word must not be credited to anyone";
    EXPECT_EQ(perSector[2].m_FinestMipRequested, kVTMaxMipCount) << "an untouched sector keeps the idle sentinel";

    // The exact request list, in its sorted order. Every camera page rides
    // with its parent, and both clamps are per-OWNER: sector 1's wanted-mip-9
    // word collapses onto its own pin instead of escaping into atlas texels
    // other images own, and the mip-0 pair's parent stops at sector 1's own
    // coarsest level.
    const auto& requests = analyzer.GetRequests();
    ASSERT_EQ(requests.size(), 6u);
    EXPECT_EQ(requests[0].m_PageKey, sectors[0].PinKey());
    EXPECT_EQ(requests[0].m_Count, std::numeric_limits<u32>::max());
    EXPECT_EQ(requests[1].m_PageKey, sectors[1].PinKey());
    EXPECT_EQ(requests[1].m_Count, std::numeric_limits<u32>::max())
        << "a camera word landing on the pin must SATURATE the count, not wrap it into an eviction candidate";
    EXPECT_EQ(requests[2].m_PageKey, VTMakePageKey(3u, 1u, 1u)) << "sector 0's parent";
    EXPECT_EQ(requests[2].m_Count, 3u);
    EXPECT_EQ(requests[3].m_PageKey, VTMakePageKey(2u, 2u, 2u)) << "sector 0's wanted page";
    EXPECT_EQ(requests[3].m_Count, 3u);
    EXPECT_EQ(requests[4].m_PageKey, VTMakePageKey(1u, 40u, 8u)) << "the under-resolved pair's parent";
    EXPECT_EQ(requests[4].m_Count, 2u);
    EXPECT_EQ(requests[5].m_PageKey, VTMakePageKey(0u, 80u, 16u)) << "the finest page sector 1 actually has";
    EXPECT_EQ(requests[5].m_Count, 2u);
}

TEST(TerrainVirtualTexture, AOnePageImagesPinSortsBeforeEveryCameraRequest)
{
    // The starvation fix. A pin used to ride the coarsest-first sort alone —
    // valid while every pin lived at the atlas's own coarsest mip. Under
    // adaptivity a 1-page image's pin lives at atlas mip 0, the FINEST band,
    // and a mip-major sort would file it behind every other sector's traffic;
    // a bake budget that never reaches the tail would then starve the one
    // page that sector's every lookup falls back to. This test fails under
    // the old sort.
    const std::vector<VTSectorSnapshot> sectors{
        VTSectorSnapshot{ 0, 0, 1, 0 },   // an idle far-away sector, shrunk to one page
        VTSectorSnapshot{ 64, 0, 64, 6 }, // a busy neighbour
    };

    std::vector<u32> feedback;
    for (i32 i = 0; i < 5; ++i)
    {
        feedback.push_back(VTPackFeedback(70u, 20u, 4));
    }

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, sectors, 8u);

    const auto& requests = analyzer.GetRequests();
    ASSERT_EQ(requests.size(), 4u);
    EXPECT_EQ(requests[0].m_PageKey, sectors[1].PinKey()) << "between pins, coarsest first";
    EXPECT_EQ(requests[1].m_PageKey, sectors[0].PinKey())
        << "the one-page image's pin must outrank the neighbour's coarse camera traffic";
    EXPECT_EQ(VTPageKeyMip(requests[1].m_PageKey), 0u) << "...and it really does live in the finest band";
    EXPECT_EQ(requests[2].m_PageKey, VTMakePageKey(5u, 2u, 0u)) << "the camera page's parent";
    EXPECT_EQ(requests[3].m_PageKey, VTMakePageKey(4u, 4u, 1u)) << "the camera page itself";
}

// -----------------------------------------------------------------------------
// The image sizing policy. Deliberately tested as behaviour over SEQUENCES of
// analyses: every knob VTDesiredImageSize has exists to prevent thrash, and
// thrash is a property of a history, not of a single call. A resize costs a
// remap of every resident page plus indirection churn, so the contract is
// hysteresis — and a policy that resizes one analysis early passes every
// single-call test while stuttering in the editor. The streak/cooldown
// constants are referenced from the production header on purpose: restating
// them as literals here would let the two drift apart silently.
// -----------------------------------------------------------------------------

namespace
{
    // The camera resolved finer than the image's mip 0, in enough texels to
    // matter — the sustained form of the grow signal.
    VTSectorFeedback UnderResolvedSignal()
    {
        VTSectorFeedback feedback;
        feedback.m_Requests = 32u;
        feedback.m_UnderResolved = kVTGrowMinRequests;
        feedback.m_FinestMipRequested = 0u;
        return feedback;
    }

    // The camera looked at the sector and its current size satisfied it: the
    // finest request landed at mip 0 but nothing wanted finer. Builds no
    // streak in either direction.
    VTSectorFeedback SettledSignal()
    {
        VTSectorFeedback feedback;
        feedback.m_Requests = 32u;
        feedback.m_UnderResolved = 0u;
        feedback.m_FinestMipRequested = 0u;
        return feedback;
    }

    // Nothing came within kVTShrinkSlackMips of the finest level — the
    // sustained form of the shrink signal.
    VTSectorFeedback OverResolvedSignal()
    {
        VTSectorFeedback feedback;
        feedback.m_Requests = 32u;
        feedback.m_UnderResolved = 0u;
        feedback.m_FinestMipRequested = kVTShrinkSlackMips;
        return feedback;
    }
} // namespace

TEST(TerrainVirtualTexture, AnImageGrowsOnlyAfterASustainedUnderResolvedStreak)
{
    // One under-resolved analysis is a camera flick; kVTGrowStreakAnalyses of
    // them in a row is demand. Growing on the flick would double an image the
    // camera has already left.
    VTSectorSizingState state;
    for (u32 analysis = 1; analysis < kVTGrowStreakAnalyses; ++analysis)
    {
        EXPECT_EQ(VTDesiredImageSize(8u, UnderResolvedSignal(), state, 1u, 64u), 8u) << "analysis " << analysis;
    }
    EXPECT_EQ(VTDesiredImageSize(8u, UnderResolvedSignal(), state, 1u, 64u), 16u)
        << "the streak's closing analysis doubles the image";

    // A single noisy analysis resets the streak: fewer under-resolved texels
    // than kVTGrowMinRequests is one grazing-angle pixel at the sector's far
    // corner, not demand, and the evidence must be CONSECUTIVE.
    VTSectorSizingState interrupted;
    VTSectorFeedback noise = UnderResolvedSignal();
    noise.m_UnderResolved = kVTGrowMinRequests - 1u;
    (void)VTDesiredImageSize(8u, UnderResolvedSignal(), interrupted, 1u, 64u);
    (void)VTDesiredImageSize(8u, UnderResolvedSignal(), interrupted, 1u, 64u);
    EXPECT_EQ(VTDesiredImageSize(8u, noise, interrupted, 1u, 64u), 8u);
    for (u32 analysis = 1; analysis < kVTGrowStreakAnalyses; ++analysis)
    {
        EXPECT_EQ(VTDesiredImageSize(8u, UnderResolvedSignal(), interrupted, 1u, 64u), 8u)
            << "post-noise analysis " << analysis << " — the streak must start over";
    }
    EXPECT_EQ(VTDesiredImageSize(8u, UnderResolvedSignal(), interrupted, 1u, 64u), 16u);
}

TEST(TerrainVirtualTexture, AResizeCannotFlapAgainInsideTheCooldownWindow)
{
    // The cooldown is the second half of the hysteresis: even a signal that
    // PERSISTS may only move the size once per kVTResizeCooldownAnalyses.
    // Without it a sector two sizes short would resize on back-to-back
    // analyses — every one of them a full-image remap.
    VTSectorSizingState state;
    u32 size = 8u;
    for (u32 analysis = 0; analysis < kVTGrowStreakAnalyses; ++analysis)
    {
        size = VTDesiredImageSize(size, UnderResolvedSignal(), state, 1u, 64u);
    }
    ASSERT_EQ(size, 16u) << "the setup grow did not fire where the streak test pinned it";

    for (u32 analysis = 1; analysis < kVTResizeCooldownAnalyses; ++analysis)
    {
        EXPECT_EQ(VTDesiredImageSize(16u, UnderResolvedSignal(), state, 1u, 64u), 16u)
            << "cooldown analysis " << analysis << " must not resize";
    }
    EXPECT_EQ(VTDesiredImageSize(16u, UnderResolvedSignal(), state, 1u, 64u), 32u)
        << "sustained demand resumes the moment the cooldown expires";
}

TEST(TerrainVirtualTexture, ShrinkingTakesFarMoreEvidenceThanGrowing)
{
    // The asymmetry is deliberate: growing costs atlas space, shrinking
    // DISCARDS the finest level's baked pages — work already paid for, paid
    // again if the camera comes back. So the shrink streak is an order of
    // magnitude longer, and only counts analyses whose finest request stayed
    // kVTShrinkSlackMips above the floor.
    VTSectorSizingState state;
    for (u32 analysis = 1; analysis < kVTShrinkStreakAnalyses; ++analysis)
    {
        EXPECT_EQ(VTDesiredImageSize(32u, OverResolvedSignal(), state, 1u, 64u), 32u) << "analysis " << analysis;
    }
    EXPECT_EQ(VTDesiredImageSize(32u, OverResolvedSignal(), state, 1u, 64u), 16u);

    // One request near the finest level voids the whole accumulated case for
    // shrinking — the camera proved it still uses the resolution.
    VTSectorSizingState interrupted;
    for (u32 analysis = 0; analysis < kVTShrinkStreakAnalyses - 1u; ++analysis)
    {
        (void)VTDesiredImageSize(32u, OverResolvedSignal(), interrupted, 1u, 64u);
    }
    EXPECT_EQ(VTDesiredImageSize(32u, SettledSignal(), interrupted, 1u, 64u), 32u);
    EXPECT_EQ(VTDesiredImageSize(32u, OverResolvedSignal(), interrupted, 1u, 64u), 32u)
        << "one near-finest analysis must reset the streak, not merely pause it";
}

TEST(TerrainVirtualTexture, AnIdleSectorStepsDownOnlyAfterALongQuietStretch)
{
    // A sector nothing looked at carries no evidence in either direction, so
    // it steps toward the minimum — its atlas space and pinned tiles should
    // follow the camera — but SLOWLY: an occluded sector the camera swings
    // back to should still have most of its pyramid.
    VTSectorSizingState state;
    for (u32 analysis = 1; analysis < kVTIdleShrinkAnalyses; ++analysis)
    {
        EXPECT_EQ(VTDesiredImageSize(32u, VTSectorFeedback{}, state, 1u, 64u), 32u) << "idle analysis " << analysis;
    }
    EXPECT_EQ(VTDesiredImageSize(32u, VTSectorFeedback{}, state, 1u, 64u), 16u);

    // A single analysis that saw the sector restarts the whole quiet stretch.
    VTSectorSizingState glanced;
    for (u32 analysis = 0; analysis < kVTIdleShrinkAnalyses - 1u; ++analysis)
    {
        (void)VTDesiredImageSize(32u, VTSectorFeedback{}, glanced, 1u, 64u);
    }
    (void)VTDesiredImageSize(32u, SettledSignal(), glanced, 1u, 64u);
    for (u32 analysis = 1; analysis < kVTIdleShrinkAnalyses; ++analysis)
    {
        EXPECT_EQ(VTDesiredImageSize(32u, VTSectorFeedback{}, glanced, 1u, 64u), 32u)
            << "post-glance idle analysis " << analysis;
    }
    EXPECT_EQ(VTDesiredImageSize(32u, VTSectorFeedback{}, glanced, 1u, 64u), 16u);
}

TEST(TerrainVirtualTexture, TheSizingPolicyNeverLeavesItsConfiguredBounds)
{
    // The bounds come from the config (and ultimately from the allocator's
    // granularity and the atlas budget), so the policy respecting them is
    // what keeps a runaway signal — a camera parked against a cliff face —
    // from requesting an image the atlas cannot hold.
    VTSectorSizingState atMax;
    for (u32 analysis = 0; analysis < kVTShrinkStreakAnalyses * 2u; ++analysis)
    {
        EXPECT_EQ(VTDesiredImageSize(64u, UnderResolvedSignal(), atMax, 1u, 64u), 64u)
            << "sustained demand at the maximum must change nothing (analysis " << analysis << ")";
    }

    VTSectorSizingState atMinActive;
    for (u32 analysis = 0; analysis < kVTShrinkStreakAnalyses * 2u; ++analysis)
    {
        EXPECT_EQ(VTDesiredImageSize(1u, OverResolvedSignal(), atMinActive, 1u, 64u), 1u)
            << "over-resolve at the minimum must change nothing (analysis " << analysis << ")";
    }

    VTSectorSizingState atMinIdle;
    for (u32 analysis = 0; analysis < kVTIdleShrinkAnalyses * 2u; ++analysis)
    {
        EXPECT_EQ(VTDesiredImageSize(1u, VTSectorFeedback{}, atMinIdle, 1u, 64u), 1u)
            << "idleness at the minimum must change nothing (analysis " << analysis << ")";
    }
}

// -----------------------------------------------------------------------------
// Page cache residency — the substrate (#704) driven exactly the way
// TerrainVirtualTexture::ServiceRequests drives it. HostOnly backing, so this
// runs with no rendering device.
// -----------------------------------------------------------------------------

namespace
{
    using VTPageCache = GPUPagedCache<u32, VTBakeRequest, LRUPolicy>;

    // The tests below drive `VTServicePageRequests` — the SAME function
    // TerrainVirtualTexture::ServiceRequests calls — rather than a transcription
    // of the policy. An earlier version of this file restated the two passes
    // here, and the restatement was where the allocation cap was missing: the
    // test then proved a policy the engine did not have.
    std::vector<u32> MappedKeys(const VTServiceOutcome& outcome)
    {
        std::vector<u32> keys;
        keys.reserve(outcome.m_Mapped.size());
        for (const auto& [key, tile] : outcome.m_Mapped)
        {
            keys.push_back(key);
        }
        return keys;
    }
} // namespace

TEST(TerrainVirtualTexture, PageCacheMapsOnePhysicalTilePerPage)
{
    VTPageCache cache;
    ASSERT_TRUE(cache.Create(0, 16u, GPUCacheBacking::HostOnly));

    std::vector<VTPageRequest> requests;
    for (u32 i = 0; i < 4u; ++i)
    {
        requests.push_back(VTPageRequest{ VTMakePageKey(0u, i, 0u), 1u });
    }
    const VTServiceOutcome outcome = VTServicePageRequests(cache, requests, 16u, 8u);

    EXPECT_EQ(outcome.m_Mapped.size(), 4u);
    EXPECT_EQ(outcome.m_Touched, 0u);
    EXPECT_FALSE(outcome.m_WorkingSetExceedsCache);

    std::vector<u32> tiles;
    for (const auto& [key, tile] : outcome.m_Mapped)
    {
        VTPageCache::ObjectAllocation allocation;
        ASSERT_TRUE(cache.Find(key, allocation));
        EXPECT_EQ(allocation.m_StartPage, tile) << "the reported tile must be the one the cache allocated";
        tiles.push_back(tile);
    }
    // Distinct tiles: two pages sharing one physical tile would have the second
    // bake overwrite the first while both indirection entries still point there.
    std::ranges::sort(tiles);
    EXPECT_EQ(std::ranges::unique(tiles).begin(), tiles.end());
}

TEST(TerrainVirtualTexture, TheBakeBudgetDefersRatherThanDropsRequests)
{
    VTPageCache cache;
    ASSERT_TRUE(cache.Create(0, 32u, GPUCacheBacking::HostOnly));

    std::vector<VTPageRequest> requests;
    for (u32 i = 0; i < 10u; ++i)
    {
        requests.push_back(VTPageRequest{ VTMakePageKey(0u, i, 0u), 10u - i });
    }

    const VTServiceOutcome first = VTServicePageRequests(cache, requests, 32u, 3u);
    EXPECT_EQ(first.m_Mapped.size(), 3u);
    EXPECT_EQ(first.m_Deferred, 7u) << "the rest must be reported as deferred, not silently dropped";

    // Same request list, next frame: the three already-resident pages are hits
    // and the budget goes to the next three. Nothing was lost.
    const VTServiceOutcome second = VTServicePageRequests(cache, requests, 32u, 3u);
    EXPECT_EQ(second.m_Mapped.size(), 3u);
    EXPECT_EQ(second.m_Touched, 3u);

    const std::vector<u32> firstKeys = MappedKeys(first);
    const std::vector<u32> secondKeys = MappedKeys(second);
    for (u32 key : firstKeys)
    {
        EXPECT_TRUE(cache.Has(key));
        EXPECT_FALSE(std::ranges::contains(secondKeys, key));
    }
}

TEST(TerrainVirtualTexture, ThePinnedPageSurvivesACacheSmallerThanTheWorkingSet)
{
    // THE TWO PROPERTIES THAT KEEP THE FALLBACK CHAIN ALIVE, tested together
    // because either one alone is insufficient — which is exactly how the first
    // version of this code got it wrong.
    //
    // (1) `LRUPolicy::OnAccess` moves to the FRONT and the victim is the TAIL,
    //     so the touch pass runs in REVERSE priority order. Front-to-back would
    //     leave the pinned page nearest the victim end.
    // (2) Ordering alone is not enough: every ALLOCATION also moves to the front,
    //     so a frame that allocates more pages than the cache has untouched tiles
    //     walks the LRU order all the way round and evicts a page it touched
    //     moments earlier. The allocation cap in VTServicePageRequests is what
    //     prevents that, and this test fails without it.
    //
    // Four tiles, five wanted pages, ten frames of a moving camera: the working
    // set never fits, so something is evicted every frame. It must never be the
    // pinned page.
    constexpr u32 kTiles = 4;
    constexpr u32 kMaxMip = 6;

    VTPageCache cache;
    ASSERT_TRUE(cache.Create(0, kTiles, GPUCacheBacking::HostOnly));

    const u32 pinnedKey = VTMakePageKey(kMaxMip, 0u, 0u);

    for (u32 frame = 0; frame < 10u; ++frame)
    {
        std::vector<VTPageRequest> requests;
        requests.push_back(VTPageRequest{ pinnedKey, 0xFFFFFFFFu });
        for (u32 i = 0; i < 4u; ++i)
        {
            requests.push_back(VTPageRequest{ VTMakePageKey(0u, frame * 4u + i, 0u), 4u - i });
        }
        const VTServiceOutcome outcome = VTServicePageRequests(cache, requests, kTiles, kTiles);

        EXPECT_TRUE(outcome.m_WorkingSetExceedsCache)
            << "five wanted pages into four tiles must be reported as over-subscribed";
        ASSERT_TRUE(cache.Has(pinnedKey))
            << "the pinned coarsest page was evicted on frame " << frame
            << " — every unresolved lookup now inherits from an unmapped texel";
    }
}

TEST(TerrainVirtualTexture, ServicingNeverEvictsAPageTheSameFrameAskedFor)
{
    // The general form of the property above: whatever the request list is, a
    // page that was resident AND requested this frame is still resident after
    // servicing. That is what makes the fallback chain stable frame to frame
    // instead of merely usually stable.
    constexpr u32 kTiles = 8;

    VTPageCache cache;
    ASSERT_TRUE(cache.Create(0, kTiles, GPUCacheBacking::HostOnly));

    // Fill the cache.
    std::vector<VTPageRequest> warm;
    for (u32 i = 0; i < kTiles; ++i)
    {
        warm.push_back(VTPageRequest{ VTMakePageKey(0u, i, 0u), 1u });
    }
    (void)VTServicePageRequests(cache, warm, kTiles, kTiles);
    for (const auto& request : warm)
    {
        ASSERT_TRUE(cache.Has(request.m_PageKey));
    }

    // Now ask for half the old set plus a lot of new pages, with a budget big
    // enough to wrap the whole cache if nothing stopped it.
    std::vector<VTPageRequest> mixed;
    for (u32 i = 0; i < 4u; ++i)
    {
        mixed.push_back(VTPageRequest{ VTMakePageKey(0u, i, 0u), 100u - i });
    }
    for (u32 i = 0; i < 16u; ++i)
    {
        mixed.push_back(VTPageRequest{ VTMakePageKey(0u, 100u + i, 0u), 1u });
    }
    (void)VTServicePageRequests(cache, mixed, kTiles, kTiles * 4u);

    for (u32 i = 0; i < 4u; ++i)
    {
        EXPECT_TRUE(cache.Has(VTMakePageKey(0u, i, 0u)))
            << "page " << i << " was requested this frame and still got evicted this frame";
    }
}

TEST(TerrainVirtualTexture, HighPriorityPagesOutliveLowPriorityOnesAfterTheCameraMovesOn)
{
    // What pass 1's REVERSE order actually buys, and the only thing it buys.
    //
    // The allocation cap already guarantees a requested page survives the frame
    // that requested it. Touch order decides what happens AFTERWARDS: once the
    // camera turns away and a page stops being requested, its LRU position is
    // what decides how long it lasts. Reversed, the pages that mattered most
    // are the last to go — which is the difference between a small camera
    // wobble re-baking nothing and re-baking the pages it just left.
    //
    // Flip `rbegin()/rend()` back to `begin()/end()` in VTServicePageRequests and
    // this fails while the two cap tests keep passing. That asymmetry is the
    // whole reason this is a separate test.
    constexpr u32 kTiles = 4;

    VTPageCache cache;
    ASSERT_TRUE(cache.Create(0, kTiles, GPUCacheBacking::HostOnly));

    const u32 highest = VTMakePageKey(0u, 0u, 0u);
    const u32 lowest = VTMakePageKey(0u, 3u, 0u);

    std::vector<VTPageRequest> requests;
    for (u32 i = 0; i < kTiles; ++i)
    {
        requests.push_back(VTPageRequest{ VTMakePageKey(0u, i, 0u), 100u - i });
    }

    // Frame 1 fills the cache; frame 2 is the first that TOUCHES rather than
    // allocates, and touch order is what this test is about. (Allocation order
    // alone leaves the first-allocated page at the tail, which is why one frame
    // is not enough to establish the order.)
    (void)VTServicePageRequests(cache, requests, kTiles, kTiles);
    (void)VTServicePageRequests(cache, requests, kTiles, kTiles);

    // The camera moves somewhere else entirely: two brand new pages, nothing
    // from the old set requested.
    const std::vector<VTPageRequest> elsewhere{ VTPageRequest{ VTMakePageKey(0u, 50u, 0u), 5u },
                                                VTPageRequest{ VTMakePageKey(0u, 51u, 0u), 4u } };
    const VTServiceOutcome outcome = VTServicePageRequests(cache, elsewhere, kTiles, kTiles);
    ASSERT_EQ(outcome.m_Mapped.size(), 2u) << "both new pages should map into the two coldest tiles";

    EXPECT_TRUE(cache.Has(highest)) << "the highest-priority page was evicted before the lowest-priority one — "
                                       "the touch pass is running in priority order instead of reverse";
    EXPECT_FALSE(cache.Has(lowest)) << "the lowest-priority page should have been the first victim";
}

TEST(TerrainVirtualTexture, EvictionNotifiesTheConsumerSoTheIndirectionCanBeRebuilt)
{
    // The consumer owns the payload (the physical cache texture), so the only
    // thing that tells it a page stopped being resident is this callback. Miss it
    // and the indirection map keeps an entry pointing at a tile another page has
    // since overwritten.
    VTPageCache cache;
    ASSERT_TRUE(cache.Create(0, 2u, GPUCacheBacking::HostOnly));

    std::vector<u32> evicted;
    cache.SetEvictionListener([&evicted](const u32& victim)
                              { evicted.push_back(victim); });

    VTPageCache::ObjectAllocation allocation;
    ASSERT_TRUE(cache.AllocatePages(VTMakePageKey(0u, 0u, 0u), 1u, allocation));
    ASSERT_TRUE(cache.AllocatePages(VTMakePageKey(0u, 1u, 0u), 1u, allocation));
    ASSERT_TRUE(cache.AllocatePages(VTMakePageKey(0u, 2u, 0u), 1u, allocation));

    ASSERT_EQ(evicted.size(), 1u);
    EXPECT_EQ(evicted[0], VTMakePageKey(0u, 0u, 0u)) << "the least recently used page is the victim";
    EXPECT_FALSE(cache.Has(evicted[0]));
}

// -----------------------------------------------------------------------------
// The fallback chain as a whole. This is the property acceptance criterion 2
// rests on, expressed as arithmetic rather than as "it looked fine while I
// moved the camera".
// -----------------------------------------------------------------------------

TEST(TerrainVirtualTexture, EveryLookupResolvesOnceTheCoarsestPageIsResident)
{
    const TerrainVirtualTextureConfig config = MakeConfig();

    // The worst case the runtime can actually reach: ONLY the pinned page is
    // resident, which is what the first frame after a cache invalidate looks
    // like.
    const std::vector<u32> residentKeys{ VTMakePageKey(config.MaxMip(), 0u, 0u) };

    for (u32 mip = 0; mip <= config.MaxMip(); ++mip)
    {
        const u32 pages = config.VirtualPagesWide >> mip;
        for (u32 y = 0; y < pages; ++y)
        {
            for (u32 x = 0; x < pages; ++x)
            {
                bool found = false;
                const VTIndirectionTexel texel = ResolveWithFallback(config, residentKeys, mip, x, y, found);
                ASSERT_TRUE(found) << "no resident ancestor for page (" << x << ", " << y << ") at mip " << mip;
                EXPECT_EQ(texel.m_Mip, config.MaxMip());
            }
        }
    }
}

TEST(TerrainVirtualTexture, AFineLookupPrefersTheFinestResidentAncestor)
{
    const TerrainVirtualTextureConfig config = MakeConfig();

    // Pinned page, plus one page at mip 2 covering the top-left sixteenth.
    const std::vector<u32> residentKeys{ VTMakePageKey(config.MaxMip(), 0u, 0u), VTMakePageKey(2u, 0u, 0u) };

    bool found = false;
    // A mip-0 page inside that mip-2 page's footprint.
    const VTIndirectionTexel inside = ResolveWithFallback(config, residentKeys, 0u, 3u, 3u, found);
    ASSERT_TRUE(found);
    EXPECT_EQ(inside.m_Mip, 2u) << "must inherit from the FINEST resident ancestor, not the coarsest";

    // One outside it falls all the way back to the pinned page — the resolution
    // step between those two neighbouring pixels is exactly what "the page has
    // not arrived yet" looks like, and it is a blur boundary rather than a hole.
    const VTIndirectionTexel outside = ResolveWithFallback(config, residentKeys, 0u, 40u, 40u, found);
    ASSERT_TRUE(found);
    EXPECT_EQ(outside.m_Mip, config.MaxMip());
}

// -----------------------------------------------------------------------------
// Incremental indirection updates (slice 2). The property that matters is not
// "the delta is smaller" — it is that the delta produces THE SAME MAP as the
// rebuild it replaces. Everything below is built around that equivalence,
// because every way this can be wrong (a missing unmap, a fill rect that stops
// one level short) leaves a map that renders a plausible frame.
// -----------------------------------------------------------------------------

namespace
{
    // The CPU twin of what the three kernels leave in the indirection texture:
    // one packed u32 per texel per mip.
    //
    // The kernels themselves are three lines each and are transcribed here; what
    // is NOT transcribed is the part under test — which texels a frame changes
    // and which rectangles get re-propagated — because that comes out of the
    // production VTIndirectionDelta. `Publish` is deliberately ONE function used
    // by both paths, so the rebuild and the delta differ only in the delta they
    // are handed, exactly as they do in PublishIndirection.
    class IndirectionMapModel
    {
      public:
        explicit IndirectionMapModel(const TerrainVirtualTextureConfig& config) : m_Config(config)
        {
            m_Levels.resize(config.MipCount());
            for (u32 mip = 0; mip < config.MipCount(); ++mip)
            {
                const u32 side = config.VirtualPagesWide >> mip;
                m_Levels[mip].assign(static_cast<sizet>(side) * side, 0u);
            }
        }

        [[nodiscard]] u32 At(u32 mip, u32 x, u32 y) const
        {
            const u32 side = m_Config.VirtualPagesWide >> mip;
            return m_Levels[mip][static_cast<sizet>(y) * side + x];
        }

        void Publish(const VTIndirectionDelta& delta)
        {
            if (delta.WantsFullRebuild())
            {
                Clear();
            }
            Write(delta);
            Fill(delta);
        }

        // Deliberately available on its own so a negative control can publish a
        // deliberately-incomplete delta and prove the comparison has teeth.
        void Write(const VTIndirectionDelta& delta)
        {
            for (u32 mip = 0; mip < m_Config.MipCount(); ++mip)
            {
                const u32 base = delta.GetMipBase(mip);
                const u32 count = delta.GetMipCountAt(mip);
                const u32 side = m_Config.VirtualPagesWide >> mip;
                for (u32 i = 0; i < count; ++i)
                {
                    const VTIndirectionUpdate& update = delta.GetUpdates()[base + i];
                    const u32 x = update.m_TexelCoord & 0xFFFFu;
                    const u32 y = update.m_TexelCoord >> 16u;
                    m_Levels[mip][static_cast<sizet>(y) * side + x] = update.m_Packed;
                }
            }
        }

        // `stopAtLevel` exists for the negative control below: propagating down
        // to level 1 instead of level 0 is what "the fill rect stopped one level
        // short" looks like, and the comparison has to be able to see it.
        void Fill(const VTIndirectionDelta& delta, u32 stopAtLevel = 0u)
        {
            // Strictly top-down, exactly as the dispatch loop is: level m reads
            // level m+1 AFTER m+1 has been repaired.
            for (u32 level = m_Config.MipCount() - 1u; level-- > stopAtLevel;)
            {
                const VTIndirectionDelta::Rect& rect = delta.GetFillRect(level);
                const u32 side = m_Config.VirtualPagesWide >> level;
                for (u32 dy = 0; dy < rect.m_Height; ++dy)
                {
                    for (u32 dx = 0; dx < rect.m_Width; ++dx)
                    {
                        const u32 x = rect.m_X + dx;
                        const u32 y = rect.m_Y + dy;
                        ASSERT_LT(x, side);
                        ASSERT_LT(y, side);
                        u32& texel = m_Levels[level][static_cast<sizet>(y) * side + x];
                        if (VTUnpackIndirection(texel).m_Direct != 0u)
                        {
                            continue; // a direct mapping — the write pass owns it
                        }
                        // alpha stays 0 so the inheritance keeps propagating.
                        texel = At(level + 1u, x >> 1u, y >> 1u) & 0x00FFFFFFu;
                    }
                }
            }
        }

        void Clear()
        {
            for (auto& level : m_Levels)
            {
                std::ranges::fill(level, 0u);
            }
        }

        // Texel-for-texel, every mip. Returns the first disagreement so a failure
        // names an address rather than "the maps differ".
        [[nodiscard]] bool SameAs(const IndirectionMapModel& other, std::string& outWhere) const
        {
            for (u32 mip = 0; mip < m_Config.MipCount(); ++mip)
            {
                const u32 side = m_Config.VirtualPagesWide >> mip;
                for (u32 y = 0; y < side; ++y)
                {
                    for (u32 x = 0; x < side; ++x)
                    {
                        if (At(mip, x, y) != other.At(mip, x, y))
                        {
                            outWhere = "mip " + std::to_string(mip) + " texel (" + std::to_string(x) + ", " +
                                       std::to_string(y) + "): rebuild=" + std::to_string(At(mip, x, y)) +
                                       " delta=" + std::to_string(other.At(mip, x, y));
                            return false;
                        }
                    }
                }
            }
            return true;
        }

      private:
        TerrainVirtualTextureConfig m_Config;
        std::vector<std::vector<u32>> m_Levels;
    };

    // The rebuild path, as PublishIndirection builds it: every resident page
    // stamped, every level's fill rect full. This is the ORACLE — it is a
    // transcription on purpose, because it is the known-good behaviour slice 1
    // shipped and the thing the delta has to reproduce.
    void BuildFullRebuildDelta(VTIndirectionDelta& delta, const TerrainVirtualTextureConfig& config,
                               const std::unordered_map<u32, u32>& resident)
    {
        delta.Reset(config);
        for (const auto& [key, tile] : resident)
        {
            VTRecordMapping(delta, config, key, tile);
        }
        delta.MarkEverythingDirty();
        delta.Finalize();
    }

    // A feedback buffer for a camera looking at `centre`, at mip `mip`. Random
    // rather than swept: the fill rectangle is a BOUNDING BOX, so scattered
    // requests are the case that stresses it.
    std::vector<u32> MakeFeedback(std::mt19937& rng, const TerrainVirtualTextureConfig& config, glm::uvec2 centre,
                                  u32 span, u32 texelCount)
    {
        std::vector<u32> feedback(texelCount, 0u);
        std::uniform_int_distribution<u32> offset(0u, span);
        std::uniform_int_distribution<u32> mipPick(0u, std::min(3u, config.MaxMip()));
        for (u32 i = 0; i < texelCount; ++i)
        {
            const u32 x = std::min(centre.x + offset(rng), config.VirtualPagesWide - 1u);
            const u32 y = std::min(centre.y + offset(rng), config.VirtualPagesWide - 1u);
            feedback[i] = VTPackFeedback(x, y, mipPick(rng));
        }
        return feedback;
    }
} // namespace

TEST(TerrainVirtualTexture, AnEvictionIsWrittenAsAnExplicitAllZeroEntry)
{
    // Rule 1 of the delta: with no clear pass, "page P is gone" has to be an
    // ENTRY. A delta that only ever adds mappings leaves P's texel addressing a
    // physical tile that another page now owns.
    const TerrainVirtualTextureConfig config = MakeConfig();
    VTIndirectionDelta delta;
    delta.Reset(config);

    VTRecordEviction(delta, VTMakePageKey(2u, 5u, 7u));
    delta.Finalize();

    ASSERT_EQ(delta.Size(), 1u);
    ASSERT_EQ(delta.GetMipCountAt(2u), 1u);
    const VTIndirectionUpdate& update = delta.GetUpdates()[delta.GetMipBase(2u)];
    EXPECT_EQ(update.m_TexelCoord, (7u << 16u) | 5u);
    // Byte-for-byte what TerrainVTIndirectionClear.comp writes. Anything else —
    // "point it at the coarser page" being the tempting one — would disagree
    // with the rebuild at the coarsest level, where no fill pass follows.
    EXPECT_EQ(update.m_Packed, 0u);
}

TEST(TerrainVirtualTexture, ATexelWrittenTwiceInOneFrameKeepsOnlyTheLastWrite)
{
    // Rule 3: two updates to one texel in a single dispatch race, and the loser
    // is not predictable. Same page evicted and re-mapped within one frame.
    const TerrainVirtualTextureConfig config = MakeConfig();
    VTIndirectionDelta delta;
    delta.Reset(config);

    const u32 key = VTMakePageKey(1u, 3u, 4u);
    VTRecordEviction(delta, key);
    VTRecordMapping(delta, config, key, /*tileIndex*/ 9u);
    delta.Finalize();

    ASSERT_EQ(delta.Size(), 1u) << "one texel, one entry";
    const glm::uvec2 tile = VTTileCoord(config, 9u);
    EXPECT_EQ(delta.GetUpdates()[0].m_Packed, VTPackIndirection(tile.x, tile.y, 1u, true));
}

TEST(TerrainVirtualTexture, TheFillRectangleCoversEveryDescendantOfAChangedTexel)
{
    // Rule 2, as arithmetic. A texel that changed at mip m invalidates the whole
    // 2^k x 2^k block below it at mip m-k, because any of those texels may have
    // inherited from it. A rect that stops one level short is a page that keeps
    // pointing at a tile that has been reused — which reads as a streaming bug.
    const TerrainVirtualTextureConfig config = MakeConfig(); // 64 pages, mips 0..6
    VTIndirectionDelta delta;
    delta.Reset(config);

    constexpr u32 kMip = 4u;
    constexpr u32 kPageX = 2u;
    constexpr u32 kPageY = 1u;
    VTRecordEviction(delta, VTMakePageKey(kMip, kPageX, kPageY));
    delta.Finalize();

    for (u32 level = 0; level <= kMip; ++level)
    {
        const u32 scale = 1u << (kMip - level);
        const VTIndirectionDelta::Rect& rect = delta.GetFillRect(level);
        EXPECT_EQ(rect.m_X, kPageX * scale) << "level " << level;
        EXPECT_EQ(rect.m_Y, kPageY * scale) << "level " << level;
        EXPECT_EQ(rect.m_Width, scale) << "level " << level;
        EXPECT_EQ(rect.m_Height, scale) << "level " << level;
    }
    for (u32 level = kMip + 1u; level < config.MipCount(); ++level)
    {
        EXPECT_TRUE(delta.GetFillRect(level).IsEmpty())
            << "nothing coarser than the change can have inherited from it (level " << level << ")";
    }
}

TEST(TerrainVirtualTexture, TwoScatteredChangesUniteIntoOneBoundingRectangle)
{
    // The documented degeneration: the rect is a bounding box, not an exact set.
    // Pinned so that a future exact-set implementation has to update the claim
    // rather than silently change the cost model.
    const TerrainVirtualTextureConfig config = MakeConfig();
    VTIndirectionDelta delta;
    delta.Reset(config);

    VTRecordEviction(delta, VTMakePageKey(0u, 1u, 1u));
    VTRecordEviction(delta, VTMakePageKey(0u, 60u, 40u));
    delta.Finalize();

    const VTIndirectionDelta::Rect& rect = delta.GetFillRect(0u);
    EXPECT_EQ(rect.m_X, 1u);
    EXPECT_EQ(rect.m_Y, 1u);
    EXPECT_EQ(rect.m_Width, 60u);
    EXPECT_EQ(rect.m_Height, 40u);
}

TEST(TerrainVirtualTexture, AFullRebuildDirtiesEveryLevelEntirely)
{
    const TerrainVirtualTextureConfig config = MakeConfig();
    VTIndirectionDelta delta;
    delta.Reset(config);
    delta.MarkEverythingDirty();
    delta.Finalize();

    EXPECT_TRUE(delta.WantsFullRebuild());
    for (u32 level = 0; level < config.MipCount(); ++level)
    {
        const u32 side = config.VirtualPagesWide >> level;
        const VTIndirectionDelta::Rect& rect = delta.GetFillRect(level);
        EXPECT_EQ(rect.m_X, 0u);
        EXPECT_EQ(rect.m_Y, 0u);
        EXPECT_EQ(rect.m_Width, side) << "level " << level;
        EXPECT_EQ(rect.m_Height, side) << "level " << level;
    }
}

TEST(TerrainVirtualTexture, TheDeltaProducesTheSameMapAsAFullRebuildOverRandomTraffic)
{
    // **The guard this slice exists for.** Equivalence against the known-good
    // path over a randomised insert/evict sequence, rather than against a
    // hand-written expected map — because a hand-written expectation encodes the
    // same misunderstanding twice.
    //
    // The cache is deliberately far smaller than the working set, so evictions
    // are the common case rather than a corner one.
    TerrainVirtualTextureConfig config = MakeConfig();
    config.CacheTilesWide = 4u; // 16 tiles
    config.MaxTileBakesPerFrame = 4u;
    ASSERT_TRUE(config.IsValid());

    VTPageCache cache;
    ASSERT_TRUE(cache.Create(0, config.CacheTileCount(), GPUCacheBacking::HostOnly));

    std::unordered_map<u32, u32> resident;
    u32 evictions = 0;
    VTIndirectionDelta deltaPath;
    deltaPath.Reset(config);
    cache.SetEvictionListener(
        [&resident, &deltaPath, &evictions](const u32& victim)
        {
            ++evictions;
            resident.erase(victim);
            VTRecordEviction(deltaPath, victim);
        });

    IndirectionMapModel rebuiltMap(config);
    IndirectionMapModel deltaMap(config);
    VTIndirectionDelta rebuildDelta;

    std::mt19937 rng(0xA71C5EEDu);
    glm::uvec2 camera(0u, 0u);

    // The degenerate one-sector table. The delta/rebuild machinery operates
    // in atlas page space either way, so the whole-atlas image makes the
    // analyzer produce exactly the slices-1-2 traffic this test always drove.
    const std::vector<VTSectorSnapshot> sectors{ MakeAtlasSpanningSector(config.VirtualPagesWide) };

    for (u32 frame = 0; frame < 60u; ++frame)
    {
        // Drift the camera so pages both arrive and stop being asked for.
        camera.x = (camera.x + 3u) % (config.VirtualPagesWide - 8u);
        camera.y = (camera.y + 2u) % (config.VirtualPagesWide - 8u);

        VTFeedbackAnalyzer analyzer;
        analyzer.Analyze(MakeFeedback(rng, config, camera, 8u, 64u), sectors, config.MaxMip());
        const VTServiceOutcome outcome =
            VTServicePageRequests(cache, analyzer.GetRequests(), config.CacheTileCount(),
                                  config.MaxTileBakesPerFrame);

        for (const auto& [pageKey, tile] : outcome.m_Mapped)
        {
            resident[pageKey] = tile;
            VTRecordMapping(deltaPath, config, pageKey, tile);
        }

        // The oracle: rebuild from the resident set, every frame.
        BuildFullRebuildDelta(rebuildDelta, config, resident);
        rebuiltMap.Publish(rebuildDelta);

        // The path under test. Frame 0 rebuilds for the same reason Configure()
        // sets the flag — texture storage starts undefined, so there is no
        // "unchanged" state to be incremental against.
        if (frame == 0u)
        {
            deltaPath.Reset(config);
            for (const auto& [key, tile] : resident)
            {
                VTRecordMapping(deltaPath, config, key, tile);
            }
            deltaPath.MarkEverythingDirty();
        }
        deltaPath.Finalize();
        deltaMap.Publish(deltaPath);
        deltaPath.Reset(config);

        std::string where;
        ASSERT_TRUE(rebuiltMap.SameAs(deltaMap, where)) << "frame " << frame << ": " << where;
    }

    // Not a vacuous pass. `resident` being non-empty only proves pages were
    // MAPPED; the rule this test exists for is rule 1, and an eviction is the
    // only thing that exercises it. A config change that stopped the cache
    // overflowing would leave every assertion above passing on a delta that
    // never had to express a removal.
    EXPECT_GT(evictions, 0u) << "no page was ever evicted, so the unmap half of the delta went untested";
    EXPECT_GT(resident.size(), 0u);
    cache.SetEvictionListener(nullptr);
}

TEST(TerrainVirtualTexture, TheEquivalenceCheckFailsWhenTheDeltaOmitsItsUnmaps)
{
    // The negative control. The equivalence test above is only worth anything if
    // it can tell the two paths apart, and the specific mistake it exists to
    // catch — a delta that records mappings but not evictions — has to make it
    // fail. Slice 1's own history is the reason this is here: a pin that did not
    // hold was found by undoing a rule and watching the test stay green.
    const TerrainVirtualTextureConfig config = MakeConfig();

    std::unordered_map<u32, u32> resident;
    const u32 evictedKey = VTMakePageKey(0u, 4u, 4u);
    const u32 replacementKey = VTMakePageKey(0u, 5u, 5u);

    // Frame 1: one page resident, both maps agree.
    resident[evictedKey] = 3u;
    VTIndirectionDelta delta;
    IndirectionMapModel rebuiltMap(config);
    IndirectionMapModel deltaMap(config);

    BuildFullRebuildDelta(delta, config, resident);
    rebuiltMap.Publish(delta);
    deltaMap.Publish(delta);
    std::string where;
    ASSERT_TRUE(rebuiltMap.SameAs(deltaMap, where)) << where;

    // Frame 2: that page is evicted and its tile handed to another page. The
    // honest delta records both halves; this one records only the mapping.
    resident.erase(evictedKey);
    resident[replacementKey] = 3u;

    BuildFullRebuildDelta(delta, config, resident);
    rebuiltMap.Publish(delta);

    VTIndirectionDelta lyingDelta;
    lyingDelta.Reset(config);
    VTRecordMapping(lyingDelta, config, replacementKey, 3u);
    // ...and NOT VTRecordEviction(lyingDelta, evictedKey).
    lyingDelta.Finalize();
    deltaMap.Publish(lyingDelta);

    EXPECT_FALSE(rebuiltMap.SameAs(deltaMap, where))
        << "a delta that drops its evictions must NOT match the rebuild — if it does, the equivalence test "
           "above proves nothing";
    // And name the damage: the evicted page's texel still addresses tile 3.
    const VTIndirectionTexel stale = VTUnpackIndirection(deltaMap.At(0u, 4u, 4u));
    const glm::uvec2 tile = VTTileCoord(config, 3u);
    EXPECT_EQ(stale.m_TileX, static_cast<u8>(tile.x));
    EXPECT_EQ(stale.m_TileY, static_cast<u8>(tile.y));
    EXPECT_NE(stale.m_Direct, 0u) << "still marked direct, so the fill pass will not repair it either";
}

TEST(TerrainVirtualTexture, TheEquivalenceCheckFailsWhenThePropagationStopsOneLevelShort)
{
    // The second negative control, for rule 2. A delta list that updates only
    // the changed page and does not re-propagate its descendants is the failure
    // the handover for this slice named as most likely — and its symptom is a
    // fine page still addressing a tile that has been handed to somebody else,
    // which looks like a streaming bug rather than an indirection bug.
    //
    // "One level short" rather than "no propagation at all" on purpose: the
    // partial case is the one a bounding-box walk can produce by accident.
    const TerrainVirtualTextureConfig config = MakeConfig();

    // A coarse page resident, so mip 0 has something to inherit; plus a fine
    // page inside its footprint that is about to go away.
    std::unordered_map<u32, u32> resident;
    resident[VTMakePageKey(config.MaxMip(), 0u, 0u)] = 0u;
    const u32 finePage = VTMakePageKey(1u, 2u, 2u);
    resident[finePage] = 5u;

    VTIndirectionDelta delta;
    IndirectionMapModel honest(config);
    IndirectionMapModel truncated(config);
    BuildFullRebuildDelta(delta, config, resident);
    honest.Publish(delta);
    truncated.Publish(delta);

    std::string where;
    ASSERT_TRUE(honest.SameAs(truncated, where)) << where;
    // Its two mip-0 children currently inherit the FINE page.
    ASSERT_EQ(VTUnpackIndirection(honest.At(0u, 4u, 4u)).m_Mip, 1u);

    // Now evict the fine page. Both sides get the same, correct delta; only the
    // propagation differs.
    resident.erase(finePage);
    VTIndirectionDelta eviction;
    eviction.Reset(config);
    VTRecordEviction(eviction, finePage);
    eviction.Finalize();

    honest.Write(eviction);
    honest.Fill(eviction);
    truncated.Write(eviction);
    truncated.Fill(eviction, /*stopAtLevel*/ 1u);

    EXPECT_FALSE(honest.SameAs(truncated, where))
        << "a propagation that stops one level short must NOT match the honest one";
    EXPECT_EQ(VTUnpackIndirection(honest.At(0u, 4u, 4u)).m_Mip, config.MaxMip())
        << "the child falls back to the coarsest resident ancestor";
    EXPECT_EQ(VTUnpackIndirection(truncated.At(0u, 4u, 4u)).m_Mip, 1u)
        << "and without it, the child still names a page that is no longer resident — the silent bug";
}

TEST(TerrainVirtualTexture, TheDeltaWritesFarFewerTexelsThanTheRebuildItReplaces)
{
    // The cost claim, as arithmetic rather than as a measurement — the measured
    // GPU-ms number is in the PR, but this pins the SHAPE: a steady-state frame
    // touches a handful of texels and re-propagates their subtrees, where the
    // rebuild touched every texel of every level twice.
    TerrainVirtualTextureConfig config;
    config.VirtualPagesWide = 256u;
    config.PageTexels = 128u;
    config.BorderTexels = 4u;
    config.CacheTilesWide = 16u;
    config.MaxTileBakesPerFrame = 8u;
    config.FeedbackDownscale = 8u;
    ASSERT_TRUE(config.IsValid());

    u64 rebuildTexels = 0;
    for (u32 mip = 0; mip < config.MipCount(); ++mip)
    {
        const u64 side = config.VirtualPagesWide >> mip;
        rebuildTexels += side * side; // the clear, and again the fill
    }

    // Eight fine pages arriving around one spot: what a frame of ordinary
    // camera movement looks like.
    VTIndirectionDelta delta;
    delta.Reset(config);
    for (u32 i = 0; i < 8u; ++i)
    {
        VTRecordMapping(delta, config, VTMakePageKey(0u, 100u + i, 100u), i);
    }
    delta.Finalize();

    u64 deltaTexels = delta.Size();
    for (u32 level = 0; level + 1u < config.MipCount(); ++level)
    {
        const VTIndirectionDelta::Rect& rect = delta.GetFillRect(level);
        deltaTexels += static_cast<u64>(rect.m_Width) * rect.m_Height;
    }

    EXPECT_LT(deltaTexels * 1000u, rebuildTexels) << "delta " << deltaTexels << " vs rebuild " << rebuildTexels
                                                  << " texels (the rebuild pays this twice, clear then fill)";
}
