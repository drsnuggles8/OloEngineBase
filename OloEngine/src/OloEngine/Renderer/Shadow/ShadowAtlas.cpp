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
} // namespace OloEngine::ShadowAtlas
