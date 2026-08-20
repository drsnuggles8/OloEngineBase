#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Shadow/ShadowAtlas.h"

#include <algorithm>
#include <array>
#include <numeric>

namespace OloEngine::ShadowAtlas
{
    namespace
    {
        // Simple shelf packer over the square atlas. Because tile sizes are
        // non-increasing with rank (TileSizeForRank), each shelf fills with
        // equal-or-smaller squares and the packing stays dense without a
        // general rectangle packer.
        struct ShelfPacker
        {
            struct Shelf
            {
                u32 Y = 0;
                u32 Height = 0;
                u32 XCursor = 0;
            };

            u32 AtlasResolution = 0;
            u32 YCursor = 0;
            std::vector<Shelf> Shelves;

            explicit ShelfPacker(u32 atlasResolution)
                : AtlasResolution(atlasResolution)
            {
            }

            bool TryPlace(u32 size, TileRect& outRect)
            {
                if (size == 0 || size > AtlasResolution)
                    return false;

                // Reuse the first shelf tall enough with horizontal room.
                for (auto& shelf : Shelves)
                {
                    if (shelf.Height >= size && AtlasResolution - shelf.XCursor >= size)
                    {
                        outRect = { shelf.XCursor, shelf.Y, size };
                        shelf.XCursor += size;
                        return true;
                    }
                }

                // Open a new shelf.
                if (AtlasResolution - YCursor >= size)
                {
                    Shelf shelf;
                    shelf.Y = YCursor;
                    shelf.Height = size;
                    shelf.XCursor = size;
                    YCursor += size;
                    Shelves.push_back(shelf);
                    outRect = { 0, shelf.Y, size };
                    return true;
                }

                return false;
            }
        };
    } // namespace

    Result Allocate(std::span<const Candidate> candidates,
                    u32 atlasResolution,
                    u32 maxEntries,
                    u32 maxLights)
    {
        OLO_PROFILE_FUNCTION();

        Result result;
        if (atlasResolution == 0 || maxEntries == 0 || maxLights == 0 || candidates.empty())
            return result;

        // Rank by score descending; stable so equal scores keep scene
        // iteration order (deterministic frame-to-frame for a static scene).
        std::vector<u32> order(candidates.size());
        std::iota(order.begin(), order.end(), 0u);
        std::ranges::stable_sort(order, [&](u32 a, u32 b)
                                 { return candidates[a].Score > candidates[b].Score; });

        ShelfPacker packer(atlasResolution);
        u32 entriesUsed = 0;

        for (const u32 candidateIndex : order)
        {
            const auto& candidate = candidates[candidateIndex];
            if (candidate.Score <= 0.0f)
                break; // sorted: everything after is unscored too

            if (result.Accepted.size() >= maxLights)
                break;

            const u32 entryCount = (candidate.Type == CasterType::Point) ? 6u : 1u;
            if (entriesUsed + entryCount > maxEntries)
                continue; // a cheaper (1-entry) candidate later may still fit

            const u32 rank = static_cast<u32>(result.Accepted.size());
            const u32 tileSize = TileSizeForRank(rank, atlasResolution, candidate.Type);

            // Attempt to place every tile; roll back the packer wholesale if
            // any face fails so a point light never gets a partial cube.
            const ShelfPacker packerBackup = packer;
            std::array<TileRect, 6> rects{};
            bool placedAll = true;
            for (u32 i = 0; i < entryCount; ++i)
            {
                if (!packer.TryPlace(tileSize, rects[i]))
                {
                    placedAll = false;
                    break;
                }
            }
            if (!placedAll)
            {
                packer = packerBackup;
                continue; // smaller-tiled candidates may still fit
            }

            Allocation allocation;
            allocation.CandidateIndex = candidateIndex;
            allocation.BaseEntry = static_cast<u32>(result.EntryRects.size());
            allocation.EntryCount = entryCount;
            result.Accepted.push_back(allocation);
            for (u32 i = 0; i < entryCount; ++i)
                result.EntryRects.push_back(rects[i]);
            entriesUsed += entryCount;
        }

        return result;
    }

    // =========================================================================
    // PersistentAllocator (issue #718)
    // =========================================================================

    namespace
    {
        // Smallest tile TileSizeForRank can ever hand out: a point caster at
        // or beyond kMediumTileRanks always gets atlasResolution/kSmallTileDivisor/2
        // (there is no tier past "small"), regardless of how high the rank
        // climbs. Sizing the persistent AtlasAllocator's granularity to this
        // keeps its node table tiny (a few thousand nodes at most) instead of
        // defaulting to a 1px floor.
        u32 MinTileSizeFor(u32 atlasResolution)
        {
            const u32 minSize = atlasResolution / (kSmallTileDivisor * 2u);
            return (minSize == 0) ? 1u : minSize;
        }
    } // namespace

    PersistentAllocator::PersistentAllocator(u32 atlasResolution)
    {
        SetAtlasResolution(atlasResolution);
    }

    void PersistentAllocator::SetAtlasResolution(u32 atlasResolution)
    {
        if (atlasResolution == m_AtlasResolution)
            return;

        // TileSizeForRank's divisors (4/8/16, halved again for point casters)
        // already assume a power-of-two atlas; AtlasAllocator degrades that
        // same assumption to a silent zero-capacity allocator by design (a
        // generic, asset/user-resolution-safe default — see its header). At
        // THIS layer the assumption is a known subsystem invariant, not a
        // caller's problem to discover the hard way as "every shadow this
        // frame starved, no error" — so make a violation loud instead.
        if (atlasResolution != 0 && (atlasResolution & (atlasResolution - 1u)) != 0u)
        {
            OLO_CORE_ERROR("ShadowAtlas::PersistentAllocator: atlas resolution {} is not a power of two — "
                           "every shadow candidate will fail to allocate a tile this session",
                           atlasResolution);
        }

        m_AtlasResolution = atlasResolution;
        m_Allocator = OloEngine::AtlasAllocator(atlasResolution, MinTileSizeFor(atlasResolution));
        m_Live.clear();
    }

    void PersistentAllocator::FreeSlot(LiveSlot& slot)
    {
        for (u32 i = 0; i < slot.EntryCount; ++i)
            m_Allocator.Free(slot.Nodes[i]);
    }

    Result PersistentAllocator::Allocate(std::span<const Candidate> candidates, u32 maxEntries, u32 maxLights)
    {
        OLO_PROFILE_FUNCTION();

        Result result;

        // Start from last call's held tiles and end holding exactly this
        // call's accepted set: swapping (rather than mutating m_Live in
        // place while iterating) means a slot only ever leaves m_Live by
        // being reused into the NEW m_Live or freed in the trailing sweep —
        // never both, and never a slot this SAME call just added. That last
        // case is why this can't be "iterate m_Live, mark kept-by-UserData,
        // free the rest": a UserData==0 candidate (no cross-frame identity)
        // would look "not kept" the instant it was inserted and get freed
        // before Allocate() even returned, letting a later candidate in the
        // same call double-allocate its just-vacated tile.
        std::vector<LiveSlot> previousLive;
        previousLive.swap(m_Live);

        if (m_AtlasResolution == 0 || maxEntries == 0 || maxLights == 0 || candidates.empty())
        {
            for (auto& slot : previousLive)
                FreeSlot(slot);
            return result;
        }

        // Free a held tile EAGERLY, before ranking runs, when its caster
        // isn't even a candidate this call (stopped casting, entity
        // removed) — the common case, and worth not deferring to the
        // trailing sweep: a candidate ranked earlier in THIS call could
        // otherwise be rejected by tight atlas space that a caster gone
        // this call was still holding. This does NOT close the narrower gap
        // where a caster IS still a candidate but doesn't make the accepted
        // set (out-ranked, over budget) — whether that happens is only known
        // after the ranking loop below runs, so that case still frees on the
        // trailing sweep and its tile becomes available next call, not this
        // one. Any persistent allocator that can't see its own future
        // accept/reject decisions in advance has that same one-call lag.
        std::erase_if(previousLive, [&](LiveSlot& slot)
                      {
                          if (slot.UserData == 0)
                              return false; // no identity to match against candidates
                          const bool stillCandidate = std::ranges::any_of(
                              candidates, [&](const Candidate& c) { return c.UserData == slot.UserData; });
                          if (stillCandidate)
                              return false;
                          FreeSlot(slot);
                          return true; });

        std::vector<u32> order(candidates.size());
        std::iota(order.begin(), order.end(), 0u);
        std::ranges::stable_sort(order, [&](u32 a, u32 b)
                                 { return candidates[a].Score > candidates[b].Score; });

        u32 entriesUsed = 0;

        for (const u32 candidateIndex : order)
        {
            const auto& candidate = candidates[candidateIndex];
            if (candidate.Score <= 0.0f)
                break; // sorted: everything after is unscored too

            if (result.Accepted.size() >= maxLights)
                break;

            const u32 entryCount = (candidate.Type == CasterType::Point) ? 6u : 1u;
            if (entriesUsed + entryCount > maxEntries)
                continue; // a cheaper (1-entry) candidate later may still fit

            const u32 rank = static_cast<u32>(result.Accepted.size());
            const u32 tileSize = TileSizeForRank(rank, m_AtlasResolution, candidate.Type);

            // Reuse a held tile set only when identity, type AND size all
            // still match — a rank shift that crosses a tier boundary (a new
            // brighter light pushing this one from large to medium, say)
            // must free and reallocate at the new size like any other change.
            auto prevIt = (candidate.UserData != 0)
                              ? std::ranges::find_if(previousLive, [&](const LiveSlot& s)
                                                     { return s.UserData == candidate.UserData; })
                              : previousLive.end();
            const bool reusable = prevIt != previousLive.end() &&
                                  prevIt->Type == candidate.Type && prevIt->TileSize == tileSize;

            if (prevIt != previousLive.end() && !reusable)
            {
                // Identity matched but type/size didn't: this slot can never
                // be reused by anything else this call (identities are
                // unique), so free it NOW rather than waiting for the
                // trailing sweep. Otherwise the caster would briefly hold its
                // old tile AND attempt to acquire a new one at the same
                // time, and on a near-full atlas that doubled peak could make
                // its own reallocation below fail.
                FreeSlot(*prevIt);
                previousLive.erase(prevIt);
                prevIt = previousLive.end();
            }

            std::array<u32, 6> nodes{};
            bool placed = true;

            if (reusable)
            {
                nodes = prevIt->Nodes;
            }
            else
            {
                u32 acquired = 0;
                for (; acquired < entryCount; ++acquired)
                {
                    const u32 node = m_Allocator.Allocate(tileSize);
                    if (node == OloEngine::AtlasAllocator::kInvalidNode)
                        break;
                    nodes[acquired] = node;
                }
                if (acquired < entryCount)
                {
                    // Never hand out a partial cube — release what we got and
                    // let a smaller-tiled candidate try instead.
                    for (u32 i = 0; i < acquired; ++i)
                        m_Allocator.Free(nodes[i]);
                    placed = false;
                }
            }

            if (!placed)
                continue;

            Allocation allocation;
            allocation.CandidateIndex = candidateIndex;
            allocation.BaseEntry = static_cast<u32>(result.EntryRects.size());
            allocation.EntryCount = entryCount;
            result.Accepted.push_back(allocation);
            for (u32 i = 0; i < entryCount; ++i)
            {
                const auto region = m_Allocator.GetRegion(nodes[i]);
                result.EntryRects.push_back({ region.X, region.Y, region.Size });
            }
            entriesUsed += entryCount;

            LiveSlot slot;
            slot.UserData = candidate.UserData;
            slot.Type = candidate.Type;
            slot.TileSize = tileSize;
            slot.EntryCount = entryCount;
            slot.Nodes = nodes;
            m_Live.push_back(slot);

            if (reusable)
                previousLive.erase(prevIt); // consumed — the trailing sweep must not free it
        }

        // Whatever is left in previousLive was held last call but not reused
        // this call (starved, stopped casting, removed, or size/type
        // changed) — free it back to the allocator.
        for (auto& slot : previousLive)
            FreeSlot(slot);

        return result;
    }
} // namespace OloEngine::ShadowAtlas
