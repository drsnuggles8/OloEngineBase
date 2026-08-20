#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace OloEngine
{
    // @brief The CPU half of terrain virtual texturing (issue #715, slice 1):
    // every packing, every coordinate transform, and the feedback reduction —
    // with no GPU dependency at all.
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
    // the page key, so a virtual image is at most 4096 pages on a side.
    inline constexpr u32 kVTMaxPagesWide = 4096u;
    // Mip gets 4 bits in the same two words.
    inline constexpr u32 kVTMaxMipCount = 16u;
    // The indirection texel stores the physical tile coordinate in two RGBA8
    // bytes, so the physical cache is at most 256 tiles on a side.
    inline constexpr u32 kVTMaxCacheTilesWide = 256u;

    // @brief How a terrain's virtual texture is shaped. Sizes are texels unless
    // the name says pages/tiles.
    struct TerrainVirtualTextureConfig
    {
        // Pages across the terrain at mip 0. Power of two — the indirection
        // texture's mip chain halves it each level, and the shader's
        // `virtualUV * (pagesWide >> mip)` only lands on page boundaries when it
        // is.
        u32 VirtualPagesWide = 256u;
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
        // Virtual texels across the terrain at mip 0 — the density figure that
        // is compared against the splatmap path.
        [[nodiscard]] u64 VirtualTexelsWide() const
        {
            return static_cast<u64>(VirtualPagesWide) * PageTexels;
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
    //   bits 24..27 : the mip the pixel wanted
    //   bit  31     : written this frame
    //
    // Storing the MIP-0 coordinate rather than the already-shifted one is what
    // lets the analysis derive both the wanted page and its parent without ever
    // reconstructing a finer coordinate from a coarser one.

    inline constexpr u32 kVTFeedbackWrittenBit = 1u << 31u;

    [[nodiscard]] constexpr u32 VTPackFeedback(u32 mip0X, u32 mip0Y, u32 mip)
    {
        return kVTFeedbackWrittenBit | ((mip & 0xFu) << 24u) | ((mip0Y & 0xFFFu) << 12u) | (mip0X & 0xFFFu);
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

    // @brief One tile the bake kernel must composite.
    // std430 twin is VTBakeRequest in compute/TerrainVTTileBake.comp.
    struct VTBakeRequest
    {
        u32 m_PageKey = 0; // VTMakePageKey(mip, x, y)
        u32 m_TileX = 0;   // destination tile in the physical cache
        u32 m_TileY = 0;
        u32 m_Padding = 0;
    };
    static_assert(sizeof(VTBakeRequest) == 16, "VTBakeRequest must match its std430 twin");

    // ── Feedback analysis ────────────────────────────────────────────────────

    // @brief A page the camera asked for, with how many feedback texels asked.
    struct VTPageRequest
    {
        u32 m_PageKey = 0;
        u32 m_Count = 0;

        bool operator==(const VTPageRequest&) const = default;
    };

    // @brief Reduce a raw feedback buffer to a unique, priority-ordered page
    // request list. Runs on a Task worker — pure CPU, no renderer state.
    //
    // Two behaviours here are load-bearing rather than incidental:
    //
    //  - **Each written texel requests its page AND that page's parent.** The
    //    parent is what a lookup falls back to while the fine page is still
    //    being composited, so not requesting it means the fallback chain has a
    //    hole exactly where the camera is looking — which shows up as the page
    //    pop this design exists to avoid.
    //  - **The coarsest mip is always requested**, at the highest possible
    //    count, so it is never the LRU victim. Every unresolved lookup
    //    ultimately inherits from that one page; evicting it would leave the
    //    whole terrain with nothing to fall back to for a frame.
    //
    // Ordering is by (coarser mip first, then descending count): a coarse page
    // both covers more screen and is the prerequisite for its children's
    // fallback, so spending a limited per-frame bake budget on it first
    // converges the image faster than chasing the finest requests.
    class VTFeedbackAnalyzer
    {
      public:
        // `feedback` is the raw ring-slot copy; `maxMip` clamps the requested
        // mip (a feedback word from a stale frame can name a mip the current
        // config no longer has).
        void Analyze(std::span<const u32> feedback, u32 maxMip);

        [[nodiscard]] const std::vector<VTPageRequest>& GetRequests() const
        {
            return m_Requests;
        }
        // Feedback texels that carried a written flag. Reported for inspection
        // (acceptance criterion 3) and to tell "nothing asked" apart from
        // "nothing was drawn".
        [[nodiscard]] u32 GetWrittenTexelCount() const
        {
            return m_WrittenTexels;
        }

        void Clear();

      private:
        void AddRequest(u32 pageKey, u32 count);

        std::vector<VTPageRequest> m_Requests;
        // Open-addressed pageKey -> index into m_Requests. A plain vector keyed
        // by a mask of the key: the request count per frame is bounded by the
        // feedback texel count (a few thousand), so this never grows.
        std::vector<u32> m_Slots;
        u32 m_WrittenTexels = 0;
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

    // The [0,1]^2 sub-rect of the terrain a page covers, WITHOUT its border.
    // GLSL twin: oloVTPageUVRect().
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
