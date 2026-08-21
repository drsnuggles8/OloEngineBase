#include "OloEnginePCH.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTextureTypes.h"

#include <cmath>
#include <algorithm>
#include <limits>

namespace OloEngine
{
    namespace
    {
        // Round down to a power of two, never below `floor`.
        u32 FloorPowerOfTwo(u32 value, u32 floorValue)
        {
            u32 result = floorValue;
            while ((result << 1u) <= value)
            {
                result <<= 1u;
            }
            return result;
        }

        [[nodiscard]] bool IsPowerOfTwo(u32 value)
        {
            return value != 0u && (value & (value - 1u)) == 0u;
        }
    } // namespace

    bool TerrainVirtualTextureConfig::IsValid() const
    {
        if (!IsPowerOfTwo(VirtualPagesWide) || VirtualPagesWide < 2u || VirtualPagesWide > kVTMaxPagesWide)
        {
            return false;
        }
        if (!IsPowerOfTwo(PageTexels) || PageTexels < 8u || PageTexels > 1024u)
        {
            return false;
        }
        // At least one, and that is not a style preference: the physical cache is
        // sampled with a LINEAR filter, so a tap at a page edge reads a texel
        // outside the page's interior. The border is the only thing that makes
        // that texel the true neighbouring content instead of whichever page
        // happens to occupy the adjacent tile.
        if (BorderTexels == 0u || BorderTexels > PageTexels / 4u)
        {
            return false;
        }
        if (CacheTilesWide < 2u || CacheTilesWide > kVTMaxCacheTilesWide)
        {
            return false;
        }
        if (MaxTileBakesPerFrame == 0u)
        {
            return false;
        }
        if (!IsPowerOfTwo(FeedbackDownscale) || FeedbackDownscale < 2u || FeedbackDownscale > 32u)
        {
            return false;
        }
        // The one cross-field rule: the whole chain has to reach 1x1, and the
        // mip index has to fit the 4 bits both packings give it.
        return MipCount() <= kVTMaxMipCount;
    }

    bool TerrainVirtualTextureConfig::Sanitize()
    {
        const TerrainVirtualTextureConfig before = *this;

        VirtualPagesWide = FloorPowerOfTwo(std::clamp(VirtualPagesWide, 2u, kVTMaxPagesWide), 2u);
        PageTexels = FloorPowerOfTwo(std::clamp(PageTexels, 8u, 1024u), 8u);
        BorderTexels = std::clamp(BorderTexels, 1u, PageTexels / 4u);
        CacheTilesWide = std::clamp(CacheTilesWide, 2u, kVTMaxCacheTilesWide);
        MaxTileBakesPerFrame = std::max(MaxTileBakesPerFrame, 1u);
        FeedbackDownscale = FloorPowerOfTwo(std::clamp(FeedbackDownscale, 2u, 32u), 2u);

        // MipCount() grows with VirtualPagesWide, so the 4-bit mip field caps
        // the page count rather than the other way round: 2^15 pages would need
        // 16 mips, one past what the key can name.
        while (MipCount() > kVTMaxMipCount)
        {
            VirtualPagesWide >>= 1u;
        }

        return *this == before;
    }

    void VTFeedbackAnalyzer::Clear()
    {
        m_Requests.clear();
        m_Slots.clear();
        m_WrittenTexels = 0;
    }

    void VTFeedbackAnalyzer::AddRequest(u32 pageKey, u32 count)
    {
        const u32 mask = static_cast<u32>(m_Slots.size()) - 1u;
        u32 slot = (pageKey * 2654435761u) & mask;
        while (true)
        {
            const u32 stored = m_Slots[slot];
            if (stored == std::numeric_limits<u32>::max())
            {
                m_Slots[slot] = static_cast<u32>(m_Requests.size());
                m_Requests.push_back(VTPageRequest{ pageKey, count });
                return;
            }
            if (m_Requests[stored].m_PageKey == pageKey)
            {
                // Saturating: the pinned coarsest page is inserted at the
                // maximum on purpose, and wrapping it to a small number would
                // make it an eviction candidate.
                u32& existing = m_Requests[stored].m_Count;
                existing = (existing > std::numeric_limits<u32>::max() - count)
                               ? std::numeric_limits<u32>::max()
                               : existing + count;
                return;
            }
            slot = (slot + 1u) & mask;
        }
    }

    void VTFeedbackAnalyzer::Analyze(std::span<const u32> feedback, u32 maxMip)
    {
        OLO_PROFILE_FUNCTION();

        m_Requests.clear();
        m_WrittenTexels = 0;

        // Two entries per written texel (the page and its parent), plus the
        // pinned coarsest page, at load factor 0.5. Sized once from the input
        // so AddRequest never has to grow mid-probe.
        sizet capacity = 64;
        const sizet wanted = (feedback.size() * 2u + 1u) * 2u;
        while (capacity < wanted)
        {
            capacity <<= 1u;
        }
        m_Slots.assign(capacity, std::numeric_limits<u32>::max());
        m_Requests.reserve(feedback.size());

        const u32 clampedMaxMip = std::min(maxMip, kVTMaxMipCount - 1u);

        // Pin the coarsest page FIRST, before any camera-driven request, so it
        // is present even when the terrain drew nothing this frame.
        AddRequest(VTMakePageKey(clampedMaxMip, 0u, 0u), std::numeric_limits<u32>::max());

        for (const u32 word : feedback)
        {
            if (!VTFeedbackWasWritten(word))
            {
                continue;
            }
            ++m_WrittenTexels;

            const u32 mip0X = word & 0xFFFu;
            const u32 mip0Y = (word >> 12u) & 0xFFFu;
            const u32 mip = std::min((word >> 24u) & 0xFu, clampedMaxMip);

            AddRequest(VTMakePageKey(mip, mip0X >> mip, mip0Y >> mip), 1u);

            // The parent, so the fallback the shader will read while this page
            // is still being composited is itself resident.
            if (mip < clampedMaxMip)
            {
                const u32 parentMip = mip + 1u;
                AddRequest(VTMakePageKey(parentMip, mip0X >> parentMip, mip0Y >> parentMip), 1u);
            }
        }

        std::ranges::sort(m_Requests,
                          [](const VTPageRequest& a, const VTPageRequest& b)
                          {
                              const u32 mipA = VTPageKeyMip(a.m_PageKey);
                              const u32 mipB = VTPageKeyMip(b.m_PageKey);
                              if (mipA != mipB)
                              {
                                  return mipA > mipB; // coarsest first
                              }
                              if (a.m_Count != b.m_Count)
                              {
                                  return a.m_Count > b.m_Count;
                              }
                              // Total order, so the request list (and therefore
                              // which pages a truncated bake budget reaches) is
                              // reproducible for a given feedback buffer.
                              return a.m_PageKey < b.m_PageKey;
                          });
    }

    glm::vec4 VTPageUVRect(const TerrainVirtualTextureConfig& config, u32 mip, u32 pageX, u32 pageY)
    {
        const f32 span = 1.0f / static_cast<f32>(config.VirtualPagesWide >> mip);
        const glm::vec2 uvMin(static_cast<f32>(pageX) * span, static_cast<f32>(pageY) * span);
        return glm::vec4(uvMin, uvMin + glm::vec2(span));
    }

    glm::vec4 VTPageUVRectWithBorder(const TerrainVirtualTextureConfig& config, u32 mip, u32 pageX, u32 pageY)
    {
        const glm::vec4 rect = VTPageUVRect(config, mip, pageX, pageY);
        const f32 span = rect.z - rect.x;
        // The border is `BorderTexels` of the SAME density as the page's
        // interior, so it is that fraction of the page's UV span — not of the
        // tile's, which would make the interior shrink as the border grows.
        const f32 grow = span * (static_cast<f32>(config.BorderTexels) / static_cast<f32>(config.PageTexels));
        return glm::vec4(rect.x - grow, rect.y - grow, rect.z + grow, rect.w + grow);
    }

    glm::vec2 VTVirtualToPhysicalUV(const TerrainVirtualTextureConfig& config, glm::vec2 virtualUV,
                                    const VTIndirectionTexel& texel)
    {
        // The resident page's mip, which is >= the mip the lookup sampled — that
        // difference IS the fallback, and it is expressed entirely by evaluating
        // the page-local UV at the RESIDENT mip's grid rather than the sampled
        // one.
        const f32 pagesAtMip = static_cast<f32>(config.VirtualPagesWide >> texel.m_Mip);
        const glm::vec2 scaled = virtualUV * pagesAtMip;
        glm::vec2 local = scaled - glm::floor(scaled);

        const f32 tileTexels = static_cast<f32>(config.TileTexels());
        const f32 cacheTexels = static_cast<f32>(config.CacheTexels());
        const glm::vec2 tileOrigin(static_cast<f32>(texel.m_TileX) * tileTexels,
                                   static_cast<f32>(texel.m_TileY) * tileTexels);
        const glm::vec2 inTile = glm::vec2(static_cast<f32>(config.BorderTexels)) +
                                 local * static_cast<f32>(config.PageTexels);
        return (tileOrigin + inTile) / cacheTexels;
    }

    f32 VTComputeMip(glm::vec2 ddx, glm::vec2 ddy)
    {
        const f32 lenSq = std::max(glm::dot(ddx, ddx), glm::dot(ddy, ddy));
        // 0.5 * log2(lenSq) == log2(length). Guarded because a fully degenerate
        // derivative (a pixel whose UV does not move) would otherwise be -inf
        // and clamp() would propagate the NaN rather than absorb it.
        return (lenSq > 0.0f) ? 0.5f * std::log2(lenSq) : 0.0f;
    }

    // ── VTIndirectionDelta ───────────────────────────────────────────────────

    namespace
    {
        using Rect = VTIndirectionDelta::Rect;

        [[nodiscard]] Rect UnionRect(const Rect& a, const Rect& b)
        {
            if (a.IsEmpty())
            {
                return b;
            }
            if (b.IsEmpty())
            {
                return a;
            }
            const u32 x0 = std::min(a.m_X, b.m_X);
            const u32 y0 = std::min(a.m_Y, b.m_Y);
            const u32 x1 = std::max(a.m_X + a.m_Width, b.m_X + b.m_Width);
            const u32 y1 = std::max(a.m_Y + a.m_Height, b.m_Y + b.m_Height);
            return Rect{ x0, y0, x1 - x0, y1 - y0 };
        }
    } // namespace

    void VTIndirectionDelta::Reset(const TerrainVirtualTextureConfig& config)
    {
        const u32 mipCount = config.MipCount();
        m_PagesWide = config.VirtualPagesWide;

        m_PerMip.resize(mipCount);
        for (auto& list : m_PerMip)
        {
            list.clear();
        }
        m_Index.clear();
        m_Combined.clear();
        m_MipOffsets.assign(static_cast<sizet>(mipCount) + 1u, 0u);
        m_ChangeBounds.assign(mipCount, Rect{});
        m_FillRects.assign(mipCount, Rect{});
        m_Size = 0u;
        m_FullRebuild = false;
    }

    void VTIndirectionDelta::Map(u32 mip, u32 pageX, u32 pageY, u32 tileX, u32 tileY)
    {
        Write(mip, pageX, pageY, VTPackIndirection(tileX, tileY, mip, /*direct*/ true));
    }

    void VTIndirectionDelta::Unmap(u32 mip, u32 pageX, u32 pageY)
    {
        // Zeroes rather than "whatever the coarser level says": byte-for-byte
        // what slice 1's clear kernel left behind, which is what makes the two
        // paths agree even at the COARSEST level, where there is no finer-level
        // fill pass afterwards to repair the texel.
        Write(mip, pageX, pageY, VTPackIndirection(0u, 0u, 0u, /*direct*/ false));
    }

    void VTIndirectionDelta::MarkEverythingDirty()
    {
        m_FullRebuild = true;
    }

    void VTIndirectionDelta::Write(u32 mip, u32 pageX, u32 pageY, u32 packed)
    {
        // A page key can outlive the config that produced it — Configure() can
        // shrink the map while a page from the old shape is still in the cache.
        // Dropping the update is right: the whole map is rebuilt on a reconfigure
        // anyway.
        if (mip >= m_PerMip.size())
        {
            return;
        }
        const u32 side = m_PagesWide >> mip;
        if (pageX >= side || pageY >= side)
        {
            OLO_CORE_ASSERT(false, "VTIndirectionDelta: page ({}, {}) is outside mip {} ({} wide)", pageX, pageY, mip,
                            side);
            return;
        }

        VTIndirectionUpdate update;
        update.m_TexelCoord = (pageY << 16u) | pageX;
        update.m_Packed = packed;

        // One entry per texel. Two updates to the same texel in one dispatch race
        // — see rule 3 in the class comment.
        const u32 key = VTMakePageKey(mip, pageX, pageY);
        auto& list = m_PerMip[mip];
        if (const auto it = m_Index.find(key); it != m_Index.end())
        {
            list[it->second] = update;
        }
        else
        {
            m_Index.emplace(key, static_cast<u32>(list.size()));
            list.push_back(update);
            ++m_Size;
        }

        // Idempotent on the rewrite path: a texel written twice is dirty once.
        m_ChangeBounds[mip] = UnionRect(m_ChangeBounds[mip], Rect{ pageX, pageY, 1u, 1u });
    }

    void VTIndirectionDelta::Finalize()
    {
        const u32 mipCount = MipCount();
        if (mipCount == 0u)
        {
            return;
        }

        m_Combined.clear();
        m_Combined.reserve(m_Size);
        for (u32 mip = 0; mip < mipCount; ++mip)
        {
            m_MipOffsets[mip] = static_cast<u32>(m_Combined.size());
            m_Combined.insert(m_Combined.end(), m_PerMip[mip].begin(), m_PerMip[mip].end());
        }
        m_MipOffsets[mipCount] = static_cast<u32>(m_Combined.size());

        if (m_FullRebuild)
        {
            for (u32 mip = 0; mip < mipCount; ++mip)
            {
                const u32 side = m_PagesWide >> mip;
                m_FillRects[mip] = Rect{ 0u, 0u, side, side };
            }
            return;
        }

        // Coarse to fine. The texels whose INHERITED value can have changed at
        // level l are the descendants of everything that changed at level l+1 and
        // above — which is level l+1's rect, doubled — united with whatever
        // changed at level l itself. Walking it this way costs one pass and needs
        // no per-entry subtree enumeration; the recurrence is what keeps a change
        // at a coarse mip from being missed three levels down.
        Rect running{};
        for (u32 level = mipCount; level-- > 0;)
        {
            Rect rect{ running.m_X << 1u, running.m_Y << 1u, running.m_Width << 1u, running.m_Height << 1u };
            rect = UnionRect(rect, m_ChangeBounds[level]);
            m_FillRects[level] = rect;
            running = rect;
        }
    }
} // namespace OloEngine
