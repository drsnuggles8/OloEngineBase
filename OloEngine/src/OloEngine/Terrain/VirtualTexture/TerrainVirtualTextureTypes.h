#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/AlignmentTemplates.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OloEngine
{
    // @brief The CPU half of terrain virtual texturing (issue #715): every
    // packing, every coordinate transform, the feedback reduction, and the
    // adaptive image sizing/remap policy — with no GPU dependency at all.
    //
    // Deliberately separated from TerrainVirtualTexture (which owns the GL
    // resources) for two reasons. The packings are MIRRORED IN GLSL — the same
    // uint layouts are unpacked by include/TerrainVirtualTexture.glsl and the
    // three compute kernels — and a mirror that only exists on the GPU can only
    // be checked by looking at pixels. And the feedback reduction is the part
    // that runs on a Task worker, where touching a renderer object would be a
    // bug rather than a slowdown.
    //
    // Everything here is unit-tested headlessly by
    // OloEngine/tests/Terrain/TerrainVirtualTextureTest.cpp.

    // ── Limits, all of them dictated by a packing rather than chosen ──────────

    // Virtual page coordinates get 12 bits each in both the feedback word and
    // the page key, so the virtual ATLAS is at most 4096 pages on a side.
    inline constexpr u32 kVTMaxPagesWide = 4096u;
    // Mip gets 4 bits in the same two words.
    inline constexpr u32 kVTMaxMipCount = 16u;
    // The indirection texel stores the physical tile coordinate in two RGBA8
    // bytes, so the physical cache is at most 256 tiles on a side.
    inline constexpr u32 kVTMaxCacheTilesWide = 256u;
    // The per-sector table rides the TerrainParams UBO as a fixed array (the
    // UBO namespace has no free binding to spend on an SSBO — see
    // ShaderBindingLayout), so the sector grid is capped at compile time.
    // 8x8 = 64 entries x 2 vec4s = 2 KB of UBO, and 64 per-sector pinned
    // pages still leave three quarters of the default 256-tile cache free.
    inline constexpr u32 kVTMaxSectorsWide = 8u;
    inline constexpr u32 kVTMaxSectorCount = kVTMaxSectorsWide * kVTMaxSectorsWide;

    // @brief How a terrain's virtual texture is shaped. Sizes are texels unless
    // the name says pages/tiles.
    struct TerrainVirtualTextureConfig
    {
        // Pages across the virtual ATLAS at mip 0. Power of two — the
        // indirection texture's mip chain halves it each level, and the
        // shader's `virtualUV * (pagesWide >> mip)` only lands on page
        // boundaries when it is. With AdaptiveEnabled this is an ADDRESS
        // SPACE the per-sector images are allocated from, not the terrain's
        // texel density; without it the whole atlas is one fixed image over
        // the terrain (slices 1-2 exactly).
        u32 VirtualPagesWide = 256u;
        // ── Adaptive (slice 3): variable-size virtual images ──
        //
        // The terrain is cut into SectorsWide^2 equal sectors; each sector
        // owns a square power-of-two image allocated from the atlas, sized by
        // what the feedback loop reports the camera actually resolves. The
        // whole slices-1-2 machinery (feedback words, page keys, indirection
        // map, delta publish) keeps operating in atlas page space unchanged;
        // adaptivity only changes how a terrain UV finds its virtual UV.
        bool AdaptiveEnabled = true;
        // Power of two in [1, kVTMaxSectorsWide]. 1 sector spanning the whole
        // atlas at a pinned size IS the fixed grid, through the same code.
        u32 SectorsWide = 8u;
        // Per-image size bounds, pages, powers of two. The min is also the
        // atlas allocator's granularity: AtlasAllocator caps its level count
        // at 12, so VirtualPagesWide / MinImagePagesWide must stay <= 2048 or
        // the allocator silently constructs with zero capacity.
        u32 MinImagePagesWide = 1u;
        u32 MaxImagePagesWide = 64u;
        // Blend two virtual mips per sample (4 cache taps instead of 2) so a
        // page-mip transition is a cross-fade rather than a density step.
        bool TrilinearEnabled = true;
        // ── Compression (slice 4): BC7 cache tiles ──
        //
        // Tiles bake into an RGBA8 scratch, a compute pass packs BC7 blocks
        // into an RGBA32UI staging image, and a block-compatible copy lands
        // them in the BC7 cache — 1 byte per texel per layer instead of 4.
        bool CompressedCache = true;
        // Unique texels per page edge (the part that is NOT border).
        u32 PageTexels = 128u;
        // Texels of duplicated neighbourhood on each side of a cache tile. This
        // is what lets the cache be sampled with a linear filter without
        // bleeding a neighbouring page's content across the seam — the whole
        // reason a VT cache tile is bigger than the page it holds.
        u32 BorderTexels = 4u;
        // Physical cache is CacheTilesWide^2 tiles of (PageTexels + 2*Border).
        u32 CacheTilesWide = 16u;
        // Ceiling on tiles composited per frame. The bake is the expensive half
        // of the loop, so this is the knob that trades convergence speed for
        // frame-time stability.
        u32 MaxTileBakesPerFrame = 8u;
        // Feedback target is the viewport divided by this on each axis. Must be
        // a power of two: the screen-position hash that decides which pixel of
        // each block writes this frame masks with (n - 1).
        u32 FeedbackDownscale = 8u;

        [[nodiscard]] u32 TileTexels() const
        {
            return PageTexels + 2u * BorderTexels;
        }
        [[nodiscard]] u32 CacheTexels() const
        {
            return CacheTilesWide * TileTexels();
        }
        [[nodiscard]] u32 CacheTileCount() const
        {
            return CacheTilesWide * CacheTilesWide;
        }
        // Number of indirection mips: mip `MipCount()-1` is the 1x1 level whose
        // single page covers the whole terrain. That page is what every
        // unresolved lookup ultimately falls back to, so the chain must reach
        // 1x1 exactly — a truncated chain leaves lookups with nothing to inherit.
        [[nodiscard]] u32 MipCount() const
        {
            u32 mips = 1u;
            for (u32 wide = VirtualPagesWide; wide > 1u; wide >>= 1u)
            {
                ++mips;
            }
            return mips;
        }
        [[nodiscard]] u32 MaxMip() const
        {
            return MipCount() - 1u;
        }
        // Virtual texels across the ATLAS at mip 0. With adaptivity the
        // terrain-density figure is per sector: EffectiveMaxImagePagesWide()
        // * PageTexels * EffectiveSectorsWide() texels across the terrain when
        // every sector is at maximum size.
        [[nodiscard]] u64 VirtualTexelsWide() const
        {
            return static_cast<u64>(VirtualPagesWide) * PageTexels;
        }

        // The values the runtime actually uses: with adaptivity off the grid
        // degenerates to ONE image pinned at the full atlas size, which makes
        // the fixed-grid path a config of the adaptive code rather than a
        // second code path.
        [[nodiscard]] u32 EffectiveSectorsWide() const
        {
            return AdaptiveEnabled ? SectorsWide : 1u;
        }
        [[nodiscard]] u32 EffectiveSectorCount() const
        {
            return EffectiveSectorsWide() * EffectiveSectorsWide();
        }
        [[nodiscard]] u32 EffectiveMinImagePagesWide() const
        {
            return AdaptiveEnabled ? MinImagePagesWide : VirtualPagesWide;
        }
        [[nodiscard]] u32 EffectiveMaxImagePagesWide() const
        {
            return AdaptiveEnabled ? MaxImagePagesWide : VirtualPagesWide;
        }

        [[nodiscard]] bool IsValid() const;

        // Clamp/round every field to something the packings can express. Returns
        // false when a value had to be changed, so a caller can say so once
        // rather than silently rendering a different virtual texture than the
        // scene asked for.
        bool Sanitize();

        bool operator==(const TerrainVirtualTextureConfig&) const = default;
    };

    // ── Page key: (mip, x, y) as one u32 ─────────────────────────────────────
    //
    // GLSL twin: oloVTPageKey() in include/TerrainVirtualTexture.glsl. Also the
    // GPUPagedCache ObjectID, which is why it has to be a plain integral type.
    //
    //   bits  0..11 : page x AT THIS MIP
    //   bits 12..23 : page y AT THIS MIP
    //   bits 24..27 : mip

    [[nodiscard]] constexpr u32 VTMakePageKey(u32 mip, u32 x, u32 y)
    {
        return ((mip & 0xFu) << 24u) | ((y & 0xFFFu) << 12u) | (x & 0xFFFu);
    }
    [[nodiscard]] constexpr u32 VTPageKeyX(u32 key)
    {
        return key & 0xFFFu;
    }
    [[nodiscard]] constexpr u32 VTPageKeyY(u32 key)
    {
        return (key >> 12u) & 0xFFFu;
    }
    [[nodiscard]] constexpr u32 VTPageKeyMip(u32 key)
    {
        return (key >> 24u) & 0xFu;
    }

    // ── Feedback word ────────────────────────────────────────────────────────
    //
    // Written by the terrain fragment stage, one per feedback texel per frame.
    // GLSL twin: oloVTPackFeedback() in include/TerrainVirtualTexture.glsl.
    //
    //   bits  0..11 : page x AT MIP 0 (i.e. before the >> mip)
    //   bits 12..23 : page y AT MIP 0
    //   bits 24..27 : the mip the pixel wanted, PLUS ONE
    //   bit  31     : written this frame
    //
    // Storing the MIP-0 coordinate rather than the already-shifted one is what
    // lets the analysis derive both the wanted page and its parent without ever
    // reconstructing a finer coordinate from a coarser one.
    //
    // The +1 bias reserves the field value 0 for "the pixel wanted a mip FINER
    // than the sector's image provides" (a computed mip below zero) — the
    // signal the adaptive layer grows an image on. The reference encodes this
    // and then discards it (AVTFeedbackAnalyzeJob clamps it away, with a
    // comment planning exactly this use); closing that loop is what makes the
    // image sizes feedback-driven rather than camera-distance-banded.

    inline constexpr u32 kVTFeedbackWrittenBit = 1u << 31u;

    [[nodiscard]] constexpr u32 VTPackFeedback(u32 mip0X, u32 mip0Y, i32 wantedMip)
    {
        const u32 biased = static_cast<u32>(std::clamp(wantedMip + 1, 0, 15));
        return kVTFeedbackWrittenBit | (biased << 24u) | ((mip0Y & 0xFFFu) << 12u) | (mip0X & 0xFFFu);
    }
    // -1 means "finer than the image has" — the grow signal, not a mip.
    [[nodiscard]] constexpr i32 VTFeedbackMip(u32 word)
    {
        return static_cast<i32>((word >> 24u) & 0xFu) - 1;
    }
    [[nodiscard]] constexpr bool VTFeedbackWasWritten(u32 word)
    {
        return (word & kVTFeedbackWrittenBit) != 0u;
    }

    // ── Indirection texel ────────────────────────────────────────────────────
    //
    // One RGBA8 texel per virtual page per mip:
    //   R = physical tile x, G = physical tile y
    //   B = the mip of the page actually resident there (may be COARSER than the
    //       mip being sampled — that is the whole fallback mechanism)
    //   A = 255 when this texel was written directly from a resident page at
    //       this mip, 0 when it was inherited from a coarser mip
    //
    // The fill kernel keys on A: a texel it finds at 0 is overwritten from the
    // coarser level, so an entry inherited two levels up is inherited twice
    // rather than needing a search. GLSL twin: oloVTUnpackIndirection().
    struct VTIndirectionTexel
    {
        u8 m_TileX = 0;
        u8 m_TileY = 0;
        u8 m_Mip = 0;
        u8 m_Direct = 0;

        bool operator==(const VTIndirectionTexel&) const = default;
    };

    [[nodiscard]] constexpr u32 VTPackIndirection(u32 tileX, u32 tileY, u32 mip, bool direct)
    {
        return ((direct ? 255u : 0u) << 24u) | ((mip & 0xFFu) << 16u) | ((tileY & 0xFFu) << 8u) | (tileX & 0xFFu);
    }
    [[nodiscard]] constexpr VTIndirectionTexel VTUnpackIndirection(u32 packed)
    {
        return VTIndirectionTexel{ static_cast<u8>(packed & 0xFFu), static_cast<u8>((packed >> 8u) & 0xFFu),
                                   static_cast<u8>((packed >> 16u) & 0xFFu),
                                   static_cast<u8>((packed >> 24u) & 0xFFu) };
    }

    // @brief One texel the indirection-write kernel must stamp.
    // GPU-visible; std430 twin is VTIndirectionUpdate in
    // compute/TerrainVTIndirectionWrite.comp.
    struct VTIndirectionUpdate
    {
        u32 m_TexelCoord = 0; // x | (y << 16), already at the target mip
        u32 m_Packed = 0;     // VTPackIndirection(...)
    };
    static_assert(sizeof(VTIndirectionUpdate) == 8, "VTIndirectionUpdate must match its std430 twin");

    // Physical tile index -> its (x, y) in the cache atlas. A free function
    // rather than a member of TerrainVirtualTexture because the delta below and
    // its equivalence test both need it, and this is one of the addresses that
    // is wrong-but-plausible when it is wrong.
    [[nodiscard]] constexpr glm::uvec2 VTTileCoord(const TerrainVirtualTextureConfig& config, u32 tileIndex)
    {
        return glm::uvec2(tileIndex % config.CacheTilesWide, tileIndex / config.CacheTilesWide);
    }

    // ── Adaptive virtual images (#715) ────────────────────────────
    //
    // The atlas allocator hands each sector a power-of-two square at a
    // power-of-two-ALIGNED origin (AtlasAllocator's buddy structure guarantees
    // X and Y are multiples of Size). That alignment is what lets variable-size
    // images share one indirection mip chain: at atlas mip m the image occupies
    // exactly (origin >> m, size >> m), still aligned, so no two images ever
    // share an indirection texel at any mip up to their own coarsest level —
    // and the fill kernel's parent-inheritance can never cross an image
    // boundary below that level. ABOVE its own coarsest level an image's texels
    // ARE shared with its allocator siblings, which is why the shader clamps
    // every lookup to the sector's MaxMip and why nothing is ever requested
    // coarser than it.

    // @brief One sector's allocation as the analyzer and the bake path see it.
    // A SNAPSHOT: the analysis job runs on a Task worker over feedback the GPU
    // wrote two-plus frames ago, so it judges those words against the table as
    // it was when the analysis launched; servicing re-validates every request
    // against the live table before allocating.
    struct VTSectorSnapshot
    {
        u16 m_OriginX = 0; // pages, atlas space, multiple of m_SizePages
        u16 m_OriginY = 0;
        u16 m_SizePages = 0; // power of two; 0 = unallocated

        [[nodiscard]] bool IsAllocated() const
        {
            return m_SizePages != 0u;
        }
        // The image's own 1x1 level. DERIVED, not stored: a stored copy is a
        // second source of truth for the same pyramid, and the struct is
        // aggregate-initialized in several places, so nothing would stop a
        // snapshot from carrying a level that disagrees with m_SizePages.
        // Contains()/PinKey() and VTRemapPageKey would then answer from two
        // different pyramids — the remap targeting a level Contains() rejects.
        [[nodiscard]] constexpr u32 MaxMip() const
        {
            return m_SizePages == 0u ? 0u : FloorLogTwo(static_cast<u32>(m_SizePages));
        }
        // Whether an atlas page key addresses a texel INSIDE this image, at a
        // level the image actually has. The validity check every stale
        // feedback word is filtered through: after a resize moves an image,
        // in-flight feedback still names pages of the old rect, and a request
        // that no live image owns must be dropped, not baked.
        [[nodiscard]] constexpr bool Contains(u32 pageKey) const
        {
            const u32 mip = VTPageKeyMip(pageKey);
            if (m_SizePages == 0u || mip > MaxMip())
            {
                return false;
            }
            const u32 sizeAtMip = static_cast<u32>(m_SizePages) >> mip;
            const u32 localX = VTPageKeyX(pageKey) - (static_cast<u32>(m_OriginX) >> mip);
            const u32 localY = VTPageKeyY(pageKey) - (static_cast<u32>(m_OriginY) >> mip);
            return localX < sizeAtMip && localY < sizeAtMip; // unsigned wrap catches below-origin
        }
        // The page every fallback chain inside this image terminates in. Kept
        // permanently requested at maximum priority (the per-sector equivalent
        // of slices 1-2's single pinned page).
        [[nodiscard]] constexpr u32 PinKey() const
        {
            const u32 maxMip = MaxMip();
            return VTMakePageKey(maxMip, static_cast<u32>(m_OriginX) >> maxMip,
                                 static_cast<u32>(m_OriginY) >> maxMip);
        }

        bool operator==(const VTSectorSnapshot&) const = default;
    };

    // @brief What one analysis saw of one sector — the signals the sizing
    // policy runs on. Requests carry the camera's actual resolve; nothing here
    // is a distance heuristic.
    struct VTSectorFeedback
    {
        u32 m_Requests = 0;                        // feedback texels that landed in this sector
        u32 m_UnderResolved = 0;                   // of those, how many wanted a mip finer than mip 0
        u32 m_FinestMipRequested = kVTMaxMipCount; // min wanted mip (>= 0) this analysis

        bool operator==(const VTSectorFeedback&) const = default;
    };

    // @brief Per-sector sizing state persisted across analyses. Streaks and a
    // cooldown are the hysteresis the reference left unwired (its
    // LastRegionResizeFrame is written and never read): a resize costs a
    // remap of every resident page plus indirection churn, so a sector must
    // WANT a size for several consecutive analyses before it gets it, and
    // cannot flap back for kVTResizeCooldownAnalyses afterwards.
    struct VTSectorSizingState
    {
        u32 m_GrowStreak = 0;
        u32 m_ShrinkStreak = 0;
        u32 m_IdleAnalyses = 0;
        u32 m_CooldownAnalyses = 0;
    };

    // Deliberately constants, not config: sizing knobs breed thrash bugs, and
    // these are counted in ANALYSES (one adopted feedback reduction), not
    // frames, so they are independent of frame rate and readback latency.
    inline constexpr u32 kVTResizeCooldownAnalyses = 8u;
    inline constexpr u32 kVTGrowStreakAnalyses = 3u;
    // Shrinking discards the finest level's baked pages, so it takes far more
    // evidence than growing, and only fires when the finest request sits at
    // least this many mips above the image's floor.
    inline constexpr u32 kVTShrinkStreakAnalyses = 24u;
    inline constexpr u32 kVTShrinkSlackMips = 2u;
    // A sector nothing looked at (off-screen, behind the camera) steps toward
    // the minimum so its atlas space and cache tiles go where the camera is.
    inline constexpr u32 kVTIdleShrinkAnalyses = 48u;
    // Requests below this are treated as noise for the GROW signal: one
    // grazing-angle pixel at the sector's far corner should not double the
    // image.
    inline constexpr u32 kVTGrowMinRequests = 4u;
    // A resize remaps every resident page of the image and dirties the
    // indirection map, so it is budgeted per analysis like the bake is.
    inline constexpr u32 kVTMaxResizesPerFrame = 2u;

    // @brief One analysis step of the sizing policy for one sector. Returns
    // the size the sector should have (== currentSize when nothing changes)
    // and updates the streak state. Pure, so the policy is headlessly
    // testable against synthetic feedback histories.
    //
    // canResize is the caller's per-analysis resize BUDGET, and it is a
    // parameter rather than something the caller decides after the fact
    // because this function COMMITS as it answers: returning a new size zeroes
    // the streak and arms the cooldown. A caller that asked for a decision and
    // then declined to act on it would throw that resize away AND penalise the
    // sector with a full cooldown. With canResize false the per-analysis
    // counters still tick — that is the whole point, the constants above are
    // counted in analyses and only mean something if every sector ticks at the
    // same rate — but nothing is committed and the answer is always
    // currentSize, so the sector keeps its streak and resizes once budget
    // frees up.
    [[nodiscard]] u32 VTDesiredImageSize(u32 currentSize, const VTSectorFeedback& feedback,
                                         VTSectorSizingState& state, u32 minSize, u32 maxSize, bool canResize = true);

    // @brief Where a resident page of a resized image lives in the new
    // allocation. A resize is a REMAP, not a rebake (the reference's central
    // trick): the power-of-two pyramids nest, so the page covering a given
    // world rect at a given texel density exists in both — one mip-index
    // shift plus an origin translation, with the physical tile untouched.
    // Returns std::nullopt when the page's density falls off the new pyramid
    // (the finest level of an image that shrank), in which case the caller
    // evicts it.
    [[nodiscard]] constexpr std::optional<u32> VTRemapPageKey(u32 pageKey, const VTSectorSnapshot& oldImage,
                                                              const VTSectorSnapshot& newImage)
    {
        // A remap from or to an unallocated image is meaningless — and the
        // doubling walks below would never terminate on a zero size, so this
        // guard is a hang prevention, not a nicety.
        if (!oldImage.IsAllocated() || !newImage.IsAllocated())
        {
            return std::nullopt;
        }
        const u32 mip = VTPageKeyMip(pageKey);
        // Grow doubles the size: every old level shifts one coarser in the new
        // pyramid (old mip 0 at 64 pages == new mip 1 at 128 — same page
        // count, same world rect, same density).
        const i32 deltaMip = static_cast<i32>(newImage.MaxMip()) - static_cast<i32>(oldImage.MaxMip());
        const i32 newMip = static_cast<i32>(mip) + deltaMip;
        if (newMip < 0 || newMip > static_cast<i32>(newImage.MaxMip()))
        {
            return std::nullopt;
        }
        const u32 localX = VTPageKeyX(pageKey) - (static_cast<u32>(oldImage.m_OriginX) >> mip);
        const u32 localY = VTPageKeyY(pageKey) - (static_cast<u32>(oldImage.m_OriginY) >> mip);
        const u32 newMipU = static_cast<u32>(newMip);
        return VTMakePageKey(newMipU, (static_cast<u32>(newImage.m_OriginX) >> newMipU) + localX,
                             (static_cast<u32>(newImage.m_OriginY) >> newMipU) + localY);
    }

    // @brief Terrain-UV rect of one page's CONTENT (no border), derived from
    // its owning sector. This is what the bake composites: under adaptivity a
    // page's terrain footprint depends on which sector owns it and how big
    // that sector's image currently is, so the CPU resolves it per request
    // and the bake kernel just samples the rect it is handed.
    // sectorIndex is row-major in the sector grid.
    [[nodiscard]] glm::vec4 VTPageTerrainUVRect(const TerrainVirtualTextureConfig& config,
                                                const VTSectorSnapshot& sector, u32 sectorIndex, u32 pageKey);

    // ── Incremental indirection updates (#715) ────────────────────
    //
    // @brief One frame's worth of indirection-texel CHANGES, plus the region the
    // coarse→fine propagation has to be re-run over because of them.
    //
    // The original republished the indirection map by REBUILDING it: clear every mip,
    // re-stamp every resident page, re-propagate every level — ~1.33 *
    // VirtualPagesWide^2 texel writes plus as many again in reads, on every frame
    // where residency changed at all, to express a change of typically under a
    // dozen texels. This is the reference's delta-list shape instead
    // (IndirectionMapDelta.cs / PopulateIndirectionMap.compute in PhotoTerrain,
    // MIT).
    //
    // **Three things make it equivalent to the rebuild rather than merely
    // cheaper**, and each is a way it goes quietly wrong:
    //
    //  1. **An eviction is an ENTRY, not an absence.** The rebuild expressed
    //     "page P is gone" by clearing the whole map first. With no clear pass, a
    //     page that stopped being resident has to be written as an explicit
    //     `Unmap` — a texel of zeroes, exactly what the clear kernel would have
    //     left. A delta that only ever adds mappings leaves the evicted page's
    //     texel pointing at a physical tile some other page now owns, which
    //     renders as a patch of the wrong terrain rather than as an error.
    //  2. **The propagation still has to run, over the DESCENDANTS of every
    //     change.** A texel at mip m that changed invalidates the 2^k x 2^k block
    //     below it at every finer mip m-k, because those texels may have
    //     inherited from it. `GetFillRect` walks that down: the rect at level l
    //     is (the rect at level l+1, doubled) united with the texels that changed
    //     at level l itself. Skipping it reintroduces exactly the page pop the
    //     mip chain exists to prevent — and it looks like a streaming bug, not an
    //     indirection bug.
    //  3. **One entry per texel, last write wins.** Two updates to the same texel
    //     in one dispatch race, and the loser is not predictable. The index map
    //     below is why the reference structure exists at all; its comment names
    //     the case (a page unmapped and then mapped to something else in the same
    //     frame).
    //
    // The rect union is a bounding box, not an exact set — two changes at
    // opposite corners of a level cover it entirely. That is deliberate: the
    // degenerate case costs what slice 1 cost unconditionally, so the delta path
    // is never the slower one.
    //
    // **It is also where most of the remaining cost is**, and that was measured
    // rather than assumed: at 256 pages wide a delta publish touches 17k–33k
    // texels against the rebuild's fixed 175k, because the pages *arriving* are
    // camera-local but the pages *leaving* are wherever the LRU tail happens to
    // be, and the box around both is a large fraction of the level. Tracking N
    // rectangles per level instead of one would recover that — at up to one
    // dispatch per change per level, and the publish is dispatch-bound at these
    // sizes, so it is very likely a regression. See
    // docs/agent-rules/terrain-virtual-texturing.md §4 before changing it.
    class VTIndirectionDelta
    {
      public:
        // A half-open texel rectangle at one indirection mip.
        struct Rect
        {
            u32 m_X = 0;
            u32 m_Y = 0;
            u32 m_Width = 0;
            u32 m_Height = 0;

            [[nodiscard]] bool IsEmpty() const
            {
                return m_Width == 0u || m_Height == 0u;
            }
            bool operator==(const Rect&) const = default;
        };

        // Drop everything and size the per-mip lists for `config`. Cheap enough
        // to call every frame: the vectors keep their capacity.
        void Reset(const TerrainVirtualTextureConfig& config);

        // Page (mip, pageX, pageY) now resolves DIRECTLY to cache tile
        // (tileX, tileY).
        void Map(u32 mip, u32 pageX, u32 pageY, u32 tileX, u32 tileY);

        // Page (mip, pageX, pageY) is no longer resident. Writes the same zeroed
        // texel the clear kernel would have, so the fill pass adopts a coarser
        // ancestor for it — see rule 1 above.
        void Unmap(u32 mip, u32 pageX, u32 pageY);

        // Every level is dirty everywhere: the path that reproduces slice 1's
        // rebuild. Used when the indirection texture's contents are not known
        // (freshly created storage) or when the delta outgrew its upload buffer.
        // The caller must also run the clear pass — a full rebuild is the ONLY
        // thing that needs it.
        void MarkEverythingDirty();

        [[nodiscard]] bool WantsFullRebuild() const
        {
            return m_FullRebuild;
        }

        // Flatten the per-mip lists into one mip-major array and derive the fill
        // rectangles. Call once, after the last Map/Unmap of the frame.
        void Finalize();

        [[nodiscard]] u32 MipCount() const
        {
            return static_cast<u32>(m_PerMip.size());
        }
        [[nodiscard]] u32 Size() const
        {
            return m_Size;
        }
        [[nodiscard]] bool IsEmpty() const
        {
            return m_Size == 0u;
        }

        // Valid after Finalize(). The window for `mip` is
        // [GetMipBase(mip), GetMipBase(mip) + GetMipCountAt(mip)).
        [[nodiscard]] const std::vector<VTIndirectionUpdate>& GetUpdates() const
        {
            return m_Combined;
        }
        [[nodiscard]] u32 GetMipBase(u32 mip) const
        {
            return m_MipOffsets[mip];
        }
        [[nodiscard]] u32 GetMipCountAt(u32 mip) const
        {
            return m_MipOffsets[mip + 1u] - m_MipOffsets[mip];
        }
        // The region the fill kernel must re-propagate at `mip`. Empty means
        // nothing at that level can have changed.
        [[nodiscard]] const Rect& GetFillRect(u32 mip) const
        {
            return m_FillRects[mip];
        }

      private:
        void Write(u32 mip, u32 pageX, u32 pageY, u32 packed);

        u32 m_PagesWide = 0;
        std::vector<std::vector<VTIndirectionUpdate>> m_PerMip;
        // Page key -> index within ITS mip's list. A page key carries the mip, so
        // one key can only ever appear in one of those lists. An unordered_map
        // rather than the analyzer's open-addressed vector because the delta is
        // three orders of magnitude smaller than a feedback buffer — the
        // allocation it avoids is not worth the hand-rolled probe.
        std::unordered_map<u32, u32> m_Index;
        std::vector<VTIndirectionUpdate> m_Combined;
        std::vector<u32> m_MipOffsets;
        std::vector<Rect> m_ChangeBounds; // per mip, bbox of the texels written
        std::vector<Rect> m_FillRects;    // per mip, after the coarse→fine walk
        u32 m_Size = 0;
        bool m_FullRebuild = false;
    };

    // Fold one residency change into the delta. The runtime's eviction listener
    // and its newly-mapped loop call these, and so does the equivalence test —
    // the page-key -> texel-coordinate decomposition is exactly the kind of
    // address a transcription gets subtly wrong and then proves anyway.
    inline void VTRecordEviction(VTIndirectionDelta& delta, u32 pageKey)
    {
        delta.Unmap(VTPageKeyMip(pageKey), VTPageKeyX(pageKey), VTPageKeyY(pageKey));
    }
    inline void VTRecordMapping(VTIndirectionDelta& delta, const TerrainVirtualTextureConfig& config, u32 pageKey,
                                u32 tileIndex)
    {
        const glm::uvec2 coord = VTTileCoord(config, tileIndex);
        delta.Map(VTPageKeyMip(pageKey), VTPageKeyX(pageKey), VTPageKeyY(pageKey), coord.x, coord.y);
    }

    // @brief One tile the bake kernel must composite.
    // std430 twin is VTBakeRequest in compute/TerrainVTTileBake.comp — and in
    // compute/TerrainVTCompressBC7.comp, which declares the same SSBO. Three
    // declarations, one layout.
    struct VTBakeRequest
    {
        u32 m_PageKey = 0; // VTMakePageKey(mip, x, y)
        u32 m_TileX = 0;   // destination tile in the physical cache — or the
                           // scratch slot when the compressed path stages the
                           // bake (slot i at x = i * tileTexels, y = 0)
        u32 m_TileY = 0;
        u32 m_Padding = 0;
        // Terrain-UV rect of the page CONTENT: xy = min, z = span, w unused.
        // The kernel used to derive this from a uniform grid; under adaptivity
        // the footprint depends on the owning sector's current image size, so
        // the CPU resolves it (VTPageTerrainUVRect) and the kernel just
        // samples what it is handed.
        glm::vec4 m_UVRect{ 0.0f };
    };
    static_assert(sizeof(VTBakeRequest) == 32, "VTBakeRequest must match its std430 twin");
    // The size alone does not pin the layout: std430 aligns a vec4 to 16 bytes,
    // so a member added or reordered ahead of m_UVRect can keep the struct at
    // 32 bytes while moving the rect the two kernels read at offset 16. That
    // failure is a bake sampling the wrong terrain rect — plausible pixels,
    // wrong content — so pin the offset, not just the stride.
    static_assert(offsetof(VTBakeRequest, m_UVRect) == 16,
                  "VTBakeRequest::m_UVRect must stay at std430 offset 16 — both bake kernels read it there");

    // ── Feedback analysis ────────────────────────────────────────────────────

    // @brief A page the camera asked for, with how many feedback texels asked.
    struct VTPageRequest
    {
        u32 m_PageKey = 0;
        u32 m_Count = 0;

        bool operator==(const VTPageRequest&) const = default;
    };

    // @brief Reduce a raw feedback buffer to a unique, priority-ordered page
    // request list plus per-sector sizing signals. Runs on a Task worker —
    // pure CPU, no renderer state.
    //
    // Three behaviours here are load-bearing rather than incidental:
    //
    //  - **Each written texel requests its page AND that page's parent** (the
    //    parent clamped to the owning sector's coarsest level). The parent is
    //    what a lookup falls back to while the fine page is still being
    //    composited, so not requesting it means the fallback chain has a hole
    //    exactly where the camera is looking — which shows up as the page pop
    //    this design exists to avoid.
    //  - **Every allocated sector's coarsest page is always requested**, at
    //    the highest possible count, before any camera-driven request — even
    //    on a frame where the terrain drew nothing. Every unresolved lookup
    //    inside a sector ultimately inherits from that one page; evicting it
    //    would leave the sector with nothing to fall back to.
    //  - **A word no live sector owns is dropped, and counted.** Feedback is
    //    two-plus frames stale, so after a resize moves an image the buffer
    //    still names pages of the old rect; baking those would composite
    //    terrain content into pages of whatever image now occupies that atlas
    //    space.
    //
    // Ordering is (pins first, then coarser mip first, then descending
    // count): a coarse page both covers more screen and is the prerequisite
    // for its children's fallback. Pins can no longer ride the mip sort
    // alone — a 1-page image's pin lives at atlas mip 0, the FINEST band, and
    // would otherwise be starved behind every other sector's requests.
    class VTFeedbackAnalyzer
    {
      public:
        // `feedback` is the raw ring-slot copy; `sectors` is the sector-table
        // snapshot taken when this analysis launched (EffectiveSectorCount()
        // entries, row-major); `atlasMaxMip` clamps a stale word's mip field
        // to something the current config can express at all.
        void Analyze(std::span<const u32> feedback, std::span<const VTSectorSnapshot> sectors, u32 atlasMaxMip);

        [[nodiscard]] const std::vector<VTPageRequest>& GetRequests() const
        {
            return m_Requests;
        }
        // Per-sector aggregates, index-matched to the snapshot. What the
        // sizing policy runs on.
        [[nodiscard]] const std::vector<VTSectorFeedback>& GetSectorFeedback() const
        {
            return m_SectorFeedback;
        }
        // Feedback texels that carried a written flag. Reported for inspection
        // (acceptance criterion 3) and to tell "nothing asked" apart from
        // "nothing was drawn".
        [[nodiscard]] u32 GetWrittenTexelCount() const
        {
            return m_WrittenTexels;
        }
        // Written texels whose page no live sector owned — normal for a frame
        // or two after a resize; persistent growth means an addressing bug.
        [[nodiscard]] u32 GetStaleTexelCount() const
        {
            return m_StaleTexels;
        }

        void Clear();

      private:
        void AddRequest(u32 pageKey, u32 count);
        // Owning sector of an atlas mip-0 page coordinate, or nullopt. Linear
        // scan over the snapshot with a last-hit cache: neighbouring feedback
        // texels overwhelmingly share a sector (the reference measured its
        // identical scheme at ~10x over a cold scan).
        [[nodiscard]] std::optional<u32> FindSector(std::span<const VTSectorSnapshot> sectors, u32 mip0X, u32 mip0Y);

        std::vector<VTPageRequest> m_Requests;
        // Open-addressed pageKey -> index into m_Requests. A plain vector keyed
        // by a mask of the key: the request count per frame is bounded by the
        // feedback texel count (a few thousand), so this never grows.
        std::vector<u32> m_Slots;
        std::vector<VTSectorFeedback> m_SectorFeedback;
        u32 m_LastSectorHit = 0;
        u32 m_WrittenTexels = 0;
        u32 m_StaleTexels = 0;
    };

    // ── Page residency policy ────────────────────────────────────────────────

    // @brief What one frame of servicing did.
    struct VTServiceOutcome
    {
        // (page key, physical tile index) for the pages newly mapped this frame.
        // These are exactly the tiles that need compositing.
        std::vector<std::pair<u32, u32>> m_Mapped;
        u32 m_Touched = 0;  // requested pages that were already resident
        u32 m_Deferred = 0; // requested pages left for a later frame
        // True when the camera wants more pages than the cache can hold at once.
        // Not an error — the coarse-mip fallback covers whatever misses — but it
        // is the difference between "the bake budget is too small" and "the cache
        // is too small", which look identical from the viewport.
        bool m_WorkingSetExceedsCache = false;
    };

    // @brief Turn a priority-ordered request list into page allocations.
    //
    // Templated on the cache so this stays pure CPU logic with no renderer
    // dependency: `TerrainVirtualTexture::ServiceRequests` and the headless test
    // call THE SAME function rather than the test transcribing the policy — the
    // two properties below are exactly the kind that a transcription gets subtly
    // wrong and then proves anyway.
    //
    // **Two passes. Each has one non-obvious rule, and they do DIFFERENT jobs —
    // the first version of this code conflated them and got the important one
    // wrong.**
    //
    // Pass 1 records the hits, IN REVERSE PRIORITY ORDER. `LRUPolicy::OnAccess`
    // moves to the front and the victim is the tail, so touching front-to-back
    // would leave the highest-priority page nearest the victim end. This is
    // about RETENTION AFTER THE CAMERA MOVES ON: once a page stops being
    // requested, its position decides how long it survives, and the pages that
    // mattered most should survive longest. It does NOT protect a page that is
    // still being requested — see the cap below for that.
    //
    // Pass 2 maps the misses, highest priority first, bounded by BOTH the bake
    // budget and the number of tiles this frame's request list did not touch.
    // **That second bound is what makes the pin real.** Without it, a frame
    // whose allocations outnumber the untouched tiles walks the LRU order all
    // the way round and evicts a page it touched moments earlier — including the
    // pinned coarsest one that every fallback chain terminates in. Touch order
    // cannot prevent that, because every ALLOCATION also moves to the front, so
    // enough allocations push anything to the tail.
    template<typename Cache>
    VTServiceOutcome VTServicePageRequests(Cache& cache, const std::vector<VTPageRequest>& requests,
                                           u32 cacheTileCount, u32 maxAllocations)
    {
        VTServiceOutcome outcome;
        if (requests.empty() || cacheTileCount == 0)
        {
            return outcome;
        }
        outcome.m_WorkingSetExceedsCache = requests.size() > cacheTileCount;

        for (auto it = requests.rbegin(); it != requests.rend(); ++it)
        {
            if (cache.Touch(it->m_PageKey))
            {
                ++outcome.m_Touched;
            }
        }

        const u32 evictable = (cacheTileCount > outcome.m_Touched) ? cacheTileCount - outcome.m_Touched : 0u;
        u32 budget = std::min(maxAllocations, evictable);

        for (const auto& request : requests)
        {
            if (cache.Has(request.m_PageKey))
            {
                continue;
            }
            if (budget == 0u)
            {
                // Deferred, not dropped: the request list persists across frames,
                // so a starved request is retried next frame.
                ++outcome.m_Deferred;
                continue;
            }

            typename Cache::ObjectAllocation allocation;
            if (!cache.AllocatePages(request.m_PageKey, 1u, allocation))
            {
                // The policy has nothing evictable at all. Stop rather than
                // spin — the caller reports it once.
                break;
            }
            outcome.m_Mapped.emplace_back(request.m_PageKey, allocation.m_StartPage);
            --budget;
        }
        return outcome;
    }

    // ── Coordinate math, mirrored in GLSL ────────────────────────────────────

    // The [0,1]^2 sub-rect of the ATLAS a page covers, WITHOUT its border.
    // Atlas UV, not terrain UV — under adaptivity the two coincide only for
    // the degenerate one-sector config; the terrain-side rect is
    // VTPageTerrainUVRect above.
    [[nodiscard]] glm::vec4 VTPageUVRect(const TerrainVirtualTextureConfig& config, u32 mip, u32 pageX, u32 pageY);

    // The same rect grown by the border, i.e. what the bake kernel actually
    // samples. Growing in UV (rather than clamping in texels) is what makes the
    // border hold the true neighbouring content instead of a smeared edge.
    [[nodiscard]] glm::vec4 VTPageUVRectWithBorder(const TerrainVirtualTextureConfig& config, u32 mip, u32 pageX,
                                                   u32 pageY);

    // Virtual UV -> physical cache UV, given the indirection texel that virtual
    // UV resolved to. GLSL twin: oloVTVirtualToPhysicalUV(). Exported so a
    // headless test can assert the round trip that only pixels would otherwise
    // reveal: a UV inside page P must land inside P's tile interior, never in
    // its border and never in a neighbour's tile.
    [[nodiscard]] glm::vec2 VTVirtualToPhysicalUV(const TerrainVirtualTextureConfig& config, glm::vec2 virtualUV,
                                                  const VTIndirectionTexel& texel);

    // The mip a pixel wants, from the mip-0 virtual-texel-space derivatives.
    // GLSL twin: oloVTComputeMip(). Plain isotropic log2 of the longer
    // derivative — van Waveren's anisotropic refinement is slice 3's concern,
    // and doing it here would only bias the request list, not fix a defect.
    [[nodiscard]] f32 VTComputeMip(glm::vec2 ddx, glm::vec2 ddy);
} // namespace OloEngine
