#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/AtlasAllocator.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <array>
#include <span>
#include <vector>

namespace OloEngine
{
    // @brief Priority ranking + tile packing for the budgeted local-light
    // shadow atlas (issue #435).
    //
    // Replaces the old first-come 4-spot / 4-point shadow slot assignment:
    // every shadow-casting spot / point / sphere-area light becomes a
    // CANDIDATE, candidates are scored by estimated screen influence, and the
    // highest-scoring ones are packed into the atlas until the entry / light /
    // space budgets run out. A spot light occupies one square tile (one atlas
    // ENTRY); a point / sphere-area light occupies six (its cube faces).
    //
    // Everything here is pure CPU math with deterministic results —
    // exhaustively covered by ShadowAtlasPackingTest.cpp.
    namespace ShadowAtlas
    {
        // Maximum number of shadowed local LIGHTS per frame (entries are the
        // finer-grained budget: ShadowUBO::MAX_SHADOW_ATLAS_ENTRIES).
        inline constexpr u32 kMaxShadowedLights = 16;

        // Tile-size tiers by rank (fractions of the atlas resolution):
        // ranks 0-1 get atlas/4 tiles, ranks 2-5 atlas/8, the rest atlas/16.
        // At the default 4096 atlas that is 1024 / 512 / 256 px tiles.
        inline constexpr u32 kLargeTileDivisor = 4;
        inline constexpr u32 kMediumTileDivisor = 8;
        inline constexpr u32 kSmallTileDivisor = 16;
        inline constexpr u32 kLargeTileRanks = 2;  // ranks [0, 2) -> large
        inline constexpr u32 kMediumTileRanks = 6; // ranks [2, 6) -> medium

        enum class CasterType : u8
        {
            Spot, // 1 atlas entry
            Point // 6 atlas entries (cube faces; sphere-area lights use this too)
        };

        struct Candidate
        {
            CasterType Type = CasterType::Spot;
            f32 Score = 0.0f;
            // Opaque caller payload (Scene uses it to find the light again
            // when patching shadow indices after allocation). Wide enough to
            // carry a light entity's UUID unchanged — PersistentAllocator
            // uses it as the stable identity that lets a caster keep its tile
            // across frames, so a truncated/aliased id would show up as
            // unrelated lights spuriously "reusing" each other's tiles.
            u64 UserData = 0;
        };

        // A square tile within the atlas, in pixels.
        struct TileRect
        {
            u32 X = 0;
            u32 Y = 0;
            u32 Size = 0;
        };

        struct Allocation
        {
            u32 CandidateIndex = 0; // index into the input candidate span
            u32 BaseEntry = 0;      // first entry in the flat entry-rect list
            u32 EntryCount = 0;     // 1 for spot, 6 for point
        };

        struct Result
        {
            std::vector<Allocation> Accepted; // in descending-score order
            std::vector<TileRect> EntryRects; // indexed by Allocation::BaseEntry + face
        };

        // Estimated screen influence of a local light. Deliberately simple and
        // fully deterministic:
        //   - a light whose range-sphere lies entirely outside the camera
        //     frustum scores 0 (its shadow cannot touch a visible fragment
        //     beyond fringe cases),
        //   - otherwise score = intensity-weighted squared angular size:
        //     min(range / distance, kMaxRangeDistanceRatio)^2 * min(intensity, kIntensityCap).
        // Closer lights, larger ranges, and brighter lights win.
        inline constexpr f32 kMaxRangeDistanceRatio = 4.0f;
        inline constexpr f32 kIntensityCap = 8.0f;

        inline f32 ComputeScore(const glm::vec3& lightPos, f32 range, f32 intensity,
                                const glm::vec3& cameraPos, const Frustum& cameraFrustum)
        {
            if (range <= 0.0f || intensity <= 0.0f)
                return 0.0f;
            if (!cameraFrustum.IsSphereVisible(lightPos, range))
                return 0.0f;

            const f32 distance = glm::length(lightPos - cameraPos);
            const f32 ratio = std::min(range / std::max(distance, 1e-3f), kMaxRangeDistanceRatio);
            return ratio * ratio * std::min(intensity, kIntensityCap);
        }

        // Tile size for a light of the given priority rank. Point lights
        // (6 face tiles) get HALF the tier resolution per face — six
        // full-tier faces would exhaust the atlas AREA long before the entry
        // budget binds (two top-rank point lights alone would claim 12 of the
        // 16 possible atlas/4 tiles), and a cube face covers a 90° frustum
        // slice, so half resolution per face is comparable texel density to a
        // typical spot cone anyway.
        inline u32 TileSizeForRank(u32 rank, u32 atlasResolution, CasterType type = CasterType::Spot)
        {
            const u32 divisor = (rank < kLargeTileRanks)    ? kLargeTileDivisor
                                : (rank < kMediumTileRanks) ? kMediumTileDivisor
                                                            : kSmallTileDivisor;
            const u32 tileSize = atlasResolution / divisor;
            return (type == CasterType::Point) ? tileSize / 2 : tileSize;
        }

        // Rank candidates by score (descending; ties keep input order) and
        // shelf-pack their tiles into the atlas. Candidates with score <= 0
        // are never allocated. A candidate whose tiles do not all fit is
        // skipped whole (a point light never gets a partial cube), but later
        // (smaller-tiled) candidates may still fit.
        Result Allocate(std::span<const Candidate> candidates,
                        u32 atlasResolution,
                        u32 maxEntries = UBOStructures::ShadowUBO::MAX_SHADOW_ATLAS_ENTRIES,
                        u32 maxLights = kMaxShadowedLights);

        // Stateful sibling of Allocate() (issue #718): backs the SAME
        // ranking/tiering rules with a persistent OloEngine::AtlasAllocator
        // instead of a call-local shelf packer, so a caster that keeps
        // ranking into the accepted set keeps its EXISTING tile instead of
        // the whole atlas being repacked from nothing every frame. A caster
        // is recognised across frames by Candidate::UserData (the caller's
        // stable identity, e.g. a light entity's UUID) — a candidate whose
        // UserData is 0 is always treated as new (no cross-frame identity to
        // match against).
        //
        // Preserves Allocate()'s "skip-whole-caster" invariant: if any of a
        // candidate's required tiles cannot be placed (existing OR fresh),
        // the whole candidate is skipped and any tiles it did manage to
        // acquire this attempt are released, exactly as the shelf packer
        // never hands out a partial cube. A caster no longer present as a
        // candidate at all (removed, stopped casting) has its held tile(s)
        // freed EAGERLY, before ranking runs, so a legitimate allocation
        // ranked ahead of it this same call isn't blocked by space that
        // caster no longer needs. A caster that IS still a candidate but
        // whose rank crossed a tier boundary (a new brighter light pushing
        // it from large to medium, say) also frees its old tile eagerly, the
        // moment the size mismatch is detected — otherwise it would briefly
        // hold both its old AND new tile at once, and on a near-full atlas
        // that doubled peak could make its own reallocation fail.
        //
        // One case still lags a call: a caster that IS a candidate this call
        // but doesn't make the accepted set (out-ranked, over budget) only
        // frees on the trailing sweep — whether it would be accepted is only
        // known after the ranking loop runs, so its tile becomes available
        // NEXT call, not this one. Under near-capacity atlas conditions this
        // can reject a candidate this call that a from-scratch repack (the
        // free function above) would have placed. Given the atlas is sized
        // with generous headroom relative to the entry/light budgets
        // specifically so this stays rare, that trade was made deliberately
        // rather than adding a two-pass "which rejections would free enough
        // space to accept something else" solver.
        //
        // Deliberately NOT a drop-in replacement for the free function above:
        // Allocate() stays a pure, single-call function (and the packing
        // surface ShadowAtlasPackingTest exercises) precisely because it has
        // no persistent state to reuse — this class is what production code
        // (Scene's per-frame shadow setup, via ShadowMap) calls instead.
        class PersistentAllocator
        {
          public:
            PersistentAllocator() = default;
            explicit PersistentAllocator(u32 atlasResolution);

            // Rebuilds from scratch (dropping every held tile) when the
            // resolution actually changes; a no-op otherwise. Tile
            // coordinates are meaningless across a resolution change, so
            // there is nothing worth preserving.
            void SetAtlasResolution(u32 atlasResolution);
            [[nodiscard]] u32 GetAtlasResolution() const
            {
                return m_AtlasResolution;
            }

            [[nodiscard]] Result Allocate(std::span<const Candidate> candidates,
                                          u32 maxEntries = UBOStructures::ShadowUBO::MAX_SHADOW_ATLAS_ENTRIES,
                                          u32 maxLights = kMaxShadowedLights);

          private:
            struct LiveSlot
            {
                u64 UserData = 0;
                CasterType Type = CasterType::Spot;
                u32 TileSize = 0;
                u32 EntryCount = 0;
                std::array<u32, 6> Nodes{};
            };

            void FreeSlot(LiveSlot& slot);

            u32 m_AtlasResolution = 0;
            OloEngine::AtlasAllocator m_Allocator;
            std::vector<LiveSlot> m_Live;
        };

        // UV scale/offset of a tile within the atlas — the per-entry value the
        // shader uses to remap light-space UV into the atlas.
        inline glm::vec4 TileScaleOffset(const TileRect& rect, u32 atlasResolution)
        {
            const f32 inv = 1.0f / static_cast<f32>(atlasResolution);
            return {
                static_cast<f32>(rect.Size) * inv,
                static_cast<f32>(rect.Size) * inv,
                static_cast<f32>(rect.X) * inv,
                static_cast<f32>(rect.Y) * inv,
            };
        }
    } // namespace ShadowAtlas
} // namespace OloEngine
