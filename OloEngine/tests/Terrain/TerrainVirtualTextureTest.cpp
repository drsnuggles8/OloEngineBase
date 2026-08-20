// OLO_TEST_LAYER: unit
// =============================================================================
// TerrainVirtualTextureTest.cpp
//
// The CPU half of terrain virtual texturing (issue #715, slice 1): the packings
// that are MIRRORED IN GLSL, the feedback reduction that runs on a Task worker,
// and the page-cache residency behaviour built on #704's GPUPagedCache.
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
        return config;
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

    const u32 word = VTPackFeedback(1234u, 2345u, 5u);
    EXPECT_TRUE(VTFeedbackWasWritten(word));
    EXPECT_EQ(word & 0xFFFu, 1234u);
    EXPECT_EQ((word >> 12u) & 0xFFFu, 2345u);
    EXPECT_EQ((word >> 24u) & 0xFu, 5u);

    // The high bit must survive a maximal payload — it is the ONLY thing that
    // distinguishes "this pixel asked for page (0,0) at mip 0" from "nothing
    // wrote here", and the analyzer skips on it.
    EXPECT_TRUE(VTFeedbackWasWritten(VTPackFeedback(4095u, 4095u, 15u)));
    EXPECT_TRUE(VTFeedbackWasWritten(VTPackFeedback(0u, 0u, 0u)));
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
// Feedback analysis — the part that runs on a Task worker.
// -----------------------------------------------------------------------------

TEST(TerrainVirtualTexture, AnalyzerPinsTheCoarsestPageEvenWithNoFeedbackAtAll)
{
    // Without this the fallback chain has no terminator, and a frame in which
    // the terrain was entirely off-screen would evict the one page every future
    // lookup depends on.
    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze({}, 8u);

    ASSERT_EQ(analyzer.GetRequests().size(), 1u);
    EXPECT_EQ(analyzer.GetRequests()[0].m_PageKey, VTMakePageKey(8u, 0u, 0u));
    EXPECT_EQ(analyzer.GetWrittenTexelCount(), 0u);
}

TEST(TerrainVirtualTexture, AnalyzerRequestsEachWantedPageAndItsParent)
{
    // The parent is what a lookup falls back to while the fine page is still
    // being composited. Not requesting it leaves a hole in the fallback chain
    // exactly where the camera is pointing, which is the page pop this design
    // exists to avoid.
    constexpr u32 kMaxMip = 6;
    const std::vector<u32> feedback{ VTPackFeedback(64u, 32u, 2u) };

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, kMaxMip);

    const auto& requests = analyzer.GetRequests();
    EXPECT_EQ(analyzer.GetWrittenTexelCount(), 1u);

    const auto has = [&requests](u32 key)
    { return std::ranges::any_of(requests, [key](const VTPageRequest& r)
                                 { return r.m_PageKey == key; }); };

    EXPECT_TRUE(has(VTMakePageKey(2u, 64u >> 2u, 32u >> 2u))) << "the page the pixel asked for";
    EXPECT_TRUE(has(VTMakePageKey(3u, 64u >> 3u, 32u >> 3u))) << "its parent, for the fallback";
    EXPECT_TRUE(has(VTMakePageKey(kMaxMip, 0u, 0u))) << "the pinned coarsest page";
}

TEST(TerrainVirtualTexture, AnalyzerDeduplicatesAndCountsRepeatedRequests)
{
    constexpr u32 kMaxMip = 6;
    std::vector<u32> feedback;
    feedback.reserve(40);
    for (i32 i = 0; i < 30; ++i)
    {
        feedback.push_back(VTPackFeedback(16u, 16u, 1u)); // same page, 30 pixels
    }
    for (i32 i = 0; i < 10; ++i)
    {
        feedback.push_back(VTPackFeedback(200u, 200u, 1u)); // a different page, 10 pixels
    }
    feedback.push_back(0u); // an unwritten texel

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, kMaxMip);

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
    constexpr u32 kMaxMip = 6;
    std::vector<u32> feedback;
    feedback.push_back(VTPackFeedback(0u, 0u, 0u));
    for (i32 i = 0; i < 5; ++i)
    {
        feedback.push_back(VTPackFeedback(128u, 128u, 0u));
    }

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, kMaxMip);

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
    // buffer) can name a mip past the current chain. Clamping keeps it a
    // coarse-but-valid request instead of a key the page cache would map and the
    // indirection write would stamp outside the texture.
    const std::vector<u32> feedback{ VTPackFeedback(4000u, 4000u, 15u) };

    VTFeedbackAnalyzer analyzer;
    analyzer.Analyze(feedback, 4u);

    for (const auto& request : analyzer.GetRequests())
    {
        EXPECT_LE(VTPageKeyMip(request.m_PageKey), 4u);
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
