// OLO_TEST_LAYER: L1
//
// Pure-math contracts for the budgeted shadow-atlas prioritisation (issue
// #435): the screen-influence score (ShadowAtlas::ComputeScore) and the
// rank-tiered shelf packer (ShadowAtlas::Allocate) that replaced the old
// first-come 4-spot / 4-point shadow slots. Everything here is deterministic
// CPU math — no GL context.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Shadow/ShadowAtlas.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <random>
#include <set>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // A camera at the origin looking down -Z; generous frustum so in-frustum
    // placement is easy to reason about.
    Frustum MakeTestFrustum()
    {
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
        Frustum frustum(proj * view);
        return frustum;
    }

    ShadowAtlas::Candidate MakeCandidate(ShadowAtlas::CasterType type, f32 score)
    {
        ShadowAtlas::Candidate c;
        c.Type = type;
        c.Score = score;
        return c;
    }

    // True when [aX, aX+aSize) x [aY, aY+aSize) overlaps the second rect.
    bool RectsOverlap(const ShadowAtlas::TileRect& a, const ShadowAtlas::TileRect& b)
    {
        return a.X < b.X + b.Size && b.X < a.X + a.Size &&
               a.Y < b.Y + b.Size && b.Y < a.Y + a.Size;
    }
} // namespace

// =============================================================================
// Scoring
// =============================================================================

TEST(ShadowAtlasScore, CloserLightScoresHigher)
{
    const auto frustum = MakeTestFrustum();
    const glm::vec3 cameraPos(0.0f);

    const f32 nearScore = ShadowAtlas::ComputeScore({ 0, 0, -5 }, 10.0f, 1.0f, cameraPos, frustum);
    const f32 farScore = ShadowAtlas::ComputeScore({ 0, 0, -50 }, 10.0f, 1.0f, cameraPos, frustum);
    EXPECT_GT(nearScore, farScore);
    EXPECT_GT(farScore, 0.0f);
}

TEST(ShadowAtlasScore, LargerRangeScoresHigher)
{
    const auto frustum = MakeTestFrustum();
    const glm::vec3 cameraPos(0.0f);

    const f32 bigScore = ShadowAtlas::ComputeScore({ 0, 0, -30 }, 20.0f, 1.0f, cameraPos, frustum);
    const f32 smallScore = ShadowAtlas::ComputeScore({ 0, 0, -30 }, 5.0f, 1.0f, cameraPos, frustum);
    EXPECT_GT(bigScore, smallScore);
}

TEST(ShadowAtlasScore, BrighterLightScoresHigher)
{
    const auto frustum = MakeTestFrustum();
    const glm::vec3 cameraPos(0.0f);

    const f32 brightScore = ShadowAtlas::ComputeScore({ 0, 0, -30 }, 10.0f, 4.0f, cameraPos, frustum);
    const f32 dimScore = ShadowAtlas::ComputeScore({ 0, 0, -30 }, 10.0f, 1.0f, cameraPos, frustum);
    EXPECT_GT(brightScore, dimScore);
}

TEST(ShadowAtlasScore, LightOutsideFrustumScoresZero)
{
    const auto frustum = MakeTestFrustum();
    const glm::vec3 cameraPos(0.0f);

    // Behind the camera (+Z), range-sphere nowhere near the frustum.
    const f32 behindScore = ShadowAtlas::ComputeScore({ 0, 0, 100 }, 5.0f, 10.0f, cameraPos, frustum);
    EXPECT_FLOAT_EQ(behindScore, 0.0f);
}

TEST(ShadowAtlasScore, LightBehindCameraButRangeReachingFrustumScoresPositive)
{
    const auto frustum = MakeTestFrustum();
    const glm::vec3 cameraPos(0.0f);

    // Centre slightly behind the camera, but the range-sphere pokes into the
    // frustum — its shadow can darken visible geometry, so it must stay a
    // candidate rather than being frustum-culled outright.
    const f32 score = ShadowAtlas::ComputeScore({ 0, 0, 2 }, 20.0f, 1.0f, cameraPos, frustum);
    EXPECT_GT(score, 0.0f);
}

TEST(ShadowAtlasScore, DegenerateInputsScoreZero)
{
    const auto frustum = MakeTestFrustum();
    EXPECT_FLOAT_EQ(ShadowAtlas::ComputeScore({ 0, 0, -10 }, 0.0f, 1.0f, glm::vec3(0.0f), frustum), 0.0f);
    EXPECT_FLOAT_EQ(ShadowAtlas::ComputeScore({ 0, 0, -10 }, 10.0f, 0.0f, glm::vec3(0.0f), frustum), 0.0f);
}

TEST(ShadowAtlasScore, ScoreIsBounded)
{
    // The ratio clamp + intensity cap bound the score so a single degenerate
    // light (camera inside a huge, bright emitter) can't produce inf/NaN and
    // can't starve every other candidate by an astronomical margin.
    const auto frustum = MakeTestFrustum();
    const f32 extreme = ShadowAtlas::ComputeScore(
        { 0, 0, -0.001f }, 1e6f, 1e9f, glm::vec3(0.0f), frustum);
    EXPECT_TRUE(std::isfinite(extreme));
    EXPECT_LE(extreme, ShadowAtlas::kMaxRangeDistanceRatio * ShadowAtlas::kMaxRangeDistanceRatio *
                               ShadowAtlas::kIntensityCap +
                           1e-3f);
}

// =============================================================================
// Tile-size tiers
// =============================================================================

TEST(ShadowAtlasPacking, TileSizeShrinksWithRank)
{
    constexpr u32 atlas = 4096;
    EXPECT_EQ(ShadowAtlas::TileSizeForRank(0, atlas), 1024u);
    EXPECT_EQ(ShadowAtlas::TileSizeForRank(1, atlas), 1024u);
    EXPECT_EQ(ShadowAtlas::TileSizeForRank(2, atlas), 512u);
    EXPECT_EQ(ShadowAtlas::TileSizeForRank(5, atlas), 512u);
    EXPECT_EQ(ShadowAtlas::TileSizeForRank(6, atlas), 256u);
    EXPECT_EQ(ShadowAtlas::TileSizeForRank(15, atlas), 256u);

    // Point lights burn six tiles, so each face gets HALF the tier size —
    // otherwise two top-rank point lights alone would claim 12 of the 16
    // possible atlas/4 tiles and space, not the entry budget, would bind.
    EXPECT_EQ(ShadowAtlas::TileSizeForRank(0, atlas, ShadowAtlas::CasterType::Point), 512u);
    EXPECT_EQ(ShadowAtlas::TileSizeForRank(2, atlas, ShadowAtlas::CasterType::Point), 256u);
    EXPECT_EQ(ShadowAtlas::TileSizeForRank(6, atlas, ShadowAtlas::CasterType::Point), 128u);
}

// =============================================================================
// Allocation
// =============================================================================

TEST(ShadowAtlasPacking, HighestScoreWinsFirstAndLargestTile)
{
    std::vector<ShadowAtlas::Candidate> candidates = {
        MakeCandidate(ShadowAtlas::CasterType::Spot, 1.0f),
        MakeCandidate(ShadowAtlas::CasterType::Spot, 5.0f),
        MakeCandidate(ShadowAtlas::CasterType::Spot, 3.0f),
    };

    const auto result = ShadowAtlas::Allocate(candidates, 4096);
    ASSERT_EQ(result.Accepted.size(), 3u);
    EXPECT_EQ(result.Accepted[0].CandidateIndex, 1u); // score 5 first
    EXPECT_EQ(result.Accepted[1].CandidateIndex, 2u); // score 3 second
    EXPECT_EQ(result.Accepted[2].CandidateIndex, 0u); // score 1 last

    // Rank 0/1 get the large tier, rank 2 the medium tier.
    EXPECT_EQ(result.EntryRects[result.Accepted[0].BaseEntry].Size, 1024u);
    EXPECT_EQ(result.EntryRects[result.Accepted[1].BaseEntry].Size, 1024u);
    EXPECT_EQ(result.EntryRects[result.Accepted[2].BaseEntry].Size, 512u);
}

TEST(ShadowAtlasPacking, PointLightGetsSixContiguousEntries)
{
    std::vector<ShadowAtlas::Candidate> candidates = {
        MakeCandidate(ShadowAtlas::CasterType::Point, 2.0f),
        MakeCandidate(ShadowAtlas::CasterType::Spot, 1.0f),
    };

    const auto result = ShadowAtlas::Allocate(candidates, 4096);
    ASSERT_EQ(result.Accepted.size(), 2u);
    EXPECT_EQ(result.Accepted[0].CandidateIndex, 0u);
    EXPECT_EQ(result.Accepted[0].EntryCount, 6u);
    EXPECT_EQ(result.Accepted[1].EntryCount, 1u);
    // Entries are consecutive: the spot's base follows the point's 6 faces.
    EXPECT_EQ(result.Accepted[0].BaseEntry, 0u);
    EXPECT_EQ(result.Accepted[1].BaseEntry, 6u);
    ASSERT_EQ(result.EntryRects.size(), 7u);
}

TEST(ShadowAtlasPacking, ZeroScoreCandidatesAreNeverAllocated)
{
    std::vector<ShadowAtlas::Candidate> candidates = {
        MakeCandidate(ShadowAtlas::CasterType::Spot, 0.0f),
        MakeCandidate(ShadowAtlas::CasterType::Spot, 2.0f),
        MakeCandidate(ShadowAtlas::CasterType::Point, 0.0f),
    };

    const auto result = ShadowAtlas::Allocate(candidates, 4096);
    ASSERT_EQ(result.Accepted.size(), 1u);
    EXPECT_EQ(result.Accepted[0].CandidateIndex, 1u);
}

TEST(ShadowAtlasPacking, TilesNeverOverlapAndStayInBounds)
{
    // A busy mixed frame: 4 point lights (24 tiles) + 8 spots.
    std::vector<ShadowAtlas::Candidate> candidates;
    for (int i = 0; i < 4; ++i)
        candidates.push_back(MakeCandidate(ShadowAtlas::CasterType::Point, 10.0f - static_cast<f32>(i)));
    for (int i = 0; i < 8; ++i)
        candidates.push_back(MakeCandidate(ShadowAtlas::CasterType::Spot, 6.0f - 0.5f * static_cast<f32>(i)));

    constexpr u32 atlasResolution = 4096;
    const auto result = ShadowAtlas::Allocate(candidates, atlasResolution);

    ASSERT_FALSE(result.EntryRects.empty());
    for (sizet a = 0; a < result.EntryRects.size(); ++a)
    {
        const auto& rect = result.EntryRects[a];
        EXPECT_GT(rect.Size, 0u);
        EXPECT_LE(rect.X + rect.Size, atlasResolution);
        EXPECT_LE(rect.Y + rect.Size, atlasResolution);
        for (sizet b = a + 1; b < result.EntryRects.size(); ++b)
        {
            EXPECT_FALSE(RectsOverlap(rect, result.EntryRects[b]))
                << "entries " << a << " and " << b << " overlap";
        }
    }
}

TEST(ShadowAtlasPacking, EntryBudgetIsRespected)
{
    // 12 point lights would need 72 entries; the 48-entry budget must cap the
    // accepted set, preferring the higher scores.
    std::vector<ShadowAtlas::Candidate> candidates;
    for (int i = 0; i < 12; ++i)
        candidates.push_back(MakeCandidate(ShadowAtlas::CasterType::Point, 12.0f - static_cast<f32>(i)));

    const auto result = ShadowAtlas::Allocate(candidates, 4096);
    u32 totalEntries = 0;
    for (const auto& a : result.Accepted)
        totalEntries += a.EntryCount;
    EXPECT_LE(totalEntries, UBOStructures::ShadowUBO::MAX_SHADOW_ATLAS_ENTRIES);
    EXPECT_EQ(result.EntryRects.size(), totalEntries);
    // 48 / 6 = 8 point lights fit.
    EXPECT_EQ(result.Accepted.size(), 8u);
    // Highest scores won.
    for (sizet i = 0; i < result.Accepted.size(); ++i)
        EXPECT_EQ(result.Accepted[i].CandidateIndex, static_cast<u32>(i));
}

TEST(ShadowAtlasPacking, LightBudgetIsRespected)
{
    std::vector<ShadowAtlas::Candidate> candidates;
    for (int i = 0; i < 30; ++i)
        candidates.push_back(MakeCandidate(ShadowAtlas::CasterType::Spot, 30.0f - static_cast<f32>(i)));

    const auto result = ShadowAtlas::Allocate(candidates, 4096);
    EXPECT_EQ(result.Accepted.size(), ShadowAtlas::kMaxShadowedLights);
}

TEST(ShadowAtlasPacking, BeatsTheOldFixedCaps)
{
    // The acceptance criterion for issue #435: more than 4 spot AND more than
    // 4 point casters shadowed simultaneously. 6 spots + 5 points = 11 lights,
    // 36 entries — all must be accepted (the old caps allowed 4 + 4).
    std::vector<ShadowAtlas::Candidate> candidates;
    for (int i = 0; i < 6; ++i)
        candidates.push_back(MakeCandidate(ShadowAtlas::CasterType::Spot, 10.0f - static_cast<f32>(i) * 0.1f));
    for (int i = 0; i < 5; ++i)
        candidates.push_back(MakeCandidate(ShadowAtlas::CasterType::Point, 5.0f - static_cast<f32>(i) * 0.1f));

    const auto result = ShadowAtlas::Allocate(candidates, 4096);
    EXPECT_EQ(result.Accepted.size(), 11u)
        << "the atlas must shadow 6 spots + 5 points simultaneously (old caps: 4 + 4)";
}

TEST(ShadowAtlasPacking, EqualScoresKeepInputOrder)
{
    std::vector<ShadowAtlas::Candidate> candidates = {
        MakeCandidate(ShadowAtlas::CasterType::Spot, 2.0f),
        MakeCandidate(ShadowAtlas::CasterType::Spot, 2.0f),
        MakeCandidate(ShadowAtlas::CasterType::Spot, 2.0f),
    };

    const auto result = ShadowAtlas::Allocate(candidates, 4096);
    ASSERT_EQ(result.Accepted.size(), 3u);
    EXPECT_EQ(result.Accepted[0].CandidateIndex, 0u);
    EXPECT_EQ(result.Accepted[1].CandidateIndex, 1u);
    EXPECT_EQ(result.Accepted[2].CandidateIndex, 2u);
}

TEST(ShadowAtlasPacking, DeterministicAcrossRuns)
{
    std::vector<ShadowAtlas::Candidate> candidates;
    for (int i = 0; i < 10; ++i)
        candidates.push_back(MakeCandidate(
            (i % 3 == 0) ? ShadowAtlas::CasterType::Point : ShadowAtlas::CasterType::Spot,
            static_cast<f32>((i * 7) % 5) + 0.5f));

    const auto a = ShadowAtlas::Allocate(candidates, 4096);
    const auto b = ShadowAtlas::Allocate(candidates, 4096);
    ASSERT_EQ(a.Accepted.size(), b.Accepted.size());
    for (sizet i = 0; i < a.Accepted.size(); ++i)
    {
        EXPECT_EQ(a.Accepted[i].CandidateIndex, b.Accepted[i].CandidateIndex);
        EXPECT_EQ(a.Accepted[i].BaseEntry, b.Accepted[i].BaseEntry);
    }
    ASSERT_EQ(a.EntryRects.size(), b.EntryRects.size());
    for (sizet i = 0; i < a.EntryRects.size(); ++i)
    {
        EXPECT_EQ(a.EntryRects[i].X, b.EntryRects[i].X);
        EXPECT_EQ(a.EntryRects[i].Y, b.EntryRects[i].Y);
        EXPECT_EQ(a.EntryRects[i].Size, b.EntryRects[i].Size);
    }
}

TEST(ShadowAtlasPacking, TileScaleOffsetRoundTrip)
{
    // Light-space UV (0,0) maps to the tile's min corner; (1,1) to its max.
    const ShadowAtlas::TileRect rect{ 1024, 3072, 512 };
    const glm::vec4 so = ShadowAtlas::TileScaleOffset(rect, 4096);

    const glm::vec2 uvMin = glm::vec2(0.0f) * glm::vec2(so.x, so.y) + glm::vec2(so.z, so.w);
    const glm::vec2 uvMax = glm::vec2(1.0f) * glm::vec2(so.x, so.y) + glm::vec2(so.z, so.w);
    EXPECT_NEAR(uvMin.x, 1024.0f / 4096.0f, 1e-6f);
    EXPECT_NEAR(uvMin.y, 3072.0f / 4096.0f, 1e-6f);
    EXPECT_NEAR(uvMax.x, (1024.0f + 512.0f) / 4096.0f, 1e-6f);
    EXPECT_NEAR(uvMax.y, (3072.0f + 512.0f) / 4096.0f, 1e-6f);
}

TEST(ShadowAtlasPacking, EmptyAndDegenerateInputs)
{
    const auto emptyResult = ShadowAtlas::Allocate({}, 4096);
    EXPECT_TRUE(emptyResult.Accepted.empty());
    EXPECT_TRUE(emptyResult.EntryRects.empty());

    std::vector<ShadowAtlas::Candidate> one = { MakeCandidate(ShadowAtlas::CasterType::Spot, 1.0f) };
    const auto zeroAtlas = ShadowAtlas::Allocate(one, 0);
    EXPECT_TRUE(zeroAtlas.Accepted.empty());
}

// =============================================================================
// PersistentAllocator (issue #718) — the stateful sibling backed by the
// shared OloEngine::AtlasAllocator. Same ranking/tiering/skip-whole-caster
// rules as the free function above; what's new here is cross-call behaviour.
// =============================================================================

namespace
{
    ShadowAtlas::Candidate MakeIdentifiedCandidate(ShadowAtlas::CasterType type, f32 score, u64 userData)
    {
        ShadowAtlas::Candidate c = MakeCandidate(type, score);
        c.UserData = userData;
        return c;
    }
} // namespace

TEST(ShadowAtlasPersistentAllocator, ReusesTheSameTileAcrossCallsForTheSameIdentity)
{
    ShadowAtlas::PersistentAllocator allocator(1024);
    std::vector<ShadowAtlas::Candidate> candidates = {
        MakeIdentifiedCandidate(ShadowAtlas::CasterType::Spot, 5.0f, 42),
    };

    const auto first = allocator.Allocate(candidates);
    ASSERT_EQ(first.Accepted.size(), 1u);
    const auto firstRect = first.EntryRects[first.Accepted[0].BaseEntry];

    // Same candidate, same score, called again — must land on the SAME tile
    // rather than a freshly repacked one.
    const auto second = allocator.Allocate(candidates);
    ASSERT_EQ(second.Accepted.size(), 1u);
    const auto secondRect = second.EntryRects[second.Accepted[0].BaseEntry];

    EXPECT_EQ(firstRect.X, secondRect.X);
    EXPECT_EQ(firstRect.Y, secondRect.Y);
    EXPECT_EQ(firstRect.Size, secondRect.Size);
}

TEST(ShadowAtlasPersistentAllocator, RankShiftAcrossATierBoundaryReallocatesAtTheNewSize)
{
    ShadowAtlas::PersistentAllocator allocator(4096);

    // Alone, this candidate ranks 0 (large tier, 1024px).
    const auto solo = allocator.Allocate(std::vector<ShadowAtlas::Candidate>{
        MakeIdentifiedCandidate(ShadowAtlas::CasterType::Spot, 5.0f, 7) });
    ASSERT_EQ(solo.Accepted.size(), 1u);
    EXPECT_EQ(solo.EntryRects[0].Size, 1024u);

    // Six brighter newcomers push it to rank 6 (small tier, 256px) — the
    // reuse check (identity match, size mismatch) must free the old tile and
    // reallocate rather than keep serving a stale 1024px rect.
    std::vector<ShadowAtlas::Candidate> crowded;
    for (int i = 0; i < 6; ++i)
        crowded.push_back(MakeIdentifiedCandidate(ShadowAtlas::CasterType::Spot, 20.0f - static_cast<f32>(i), 100u + i));
    crowded.push_back(MakeIdentifiedCandidate(ShadowAtlas::CasterType::Spot, 5.0f, 7));

    const auto result = allocator.Allocate(crowded);
    ASSERT_EQ(result.Accepted.size(), 7u);
    const auto& last = result.Accepted.back();
    EXPECT_EQ(last.CandidateIndex, 6u); // candidate 7 (UserData=7) is last by score
    EXPECT_EQ(result.EntryRects[last.BaseEntry].Size, 256u);
}

TEST(ShadowAtlasPersistentAllocator, DroppedCandidateTileIsReclaimedNotLeaked)
{
    // A tiny 64px atlas: a lone (rank-0) spot always lands in the "large"
    // tier == atlas/4 == 16px, so the atlas holds at most 16 of them
    // concurrently (4096px^2 / 256px^2). Churning through 40 single-candidate
    // calls, each a brand-new identity that is NEVER reused, only succeeds
    // throughout if every previous call's tile was actually freed rather than
    // silently leaked.
    ShadowAtlas::PersistentAllocator allocator(64);

    for (u64 id = 1; id <= 40; ++id)
    {
        const auto result = allocator.Allocate(std::vector<ShadowAtlas::Candidate>{
            MakeIdentifiedCandidate(ShadowAtlas::CasterType::Spot, 5.0f, id) });
        ASSERT_EQ(result.Accepted.size(), 1u) << "candidate " << id << " starved — a prior tile was leaked, not reclaimed";
        EXPECT_EQ(result.EntryRects[0].Size, 16u);
    }
}

TEST(ShadowAtlasPersistentAllocator, PreservesSkipWholeCasterAcrossCalls)
{
    ShadowAtlas::PersistentAllocator allocator(4096);

    std::vector<ShadowAtlas::Candidate> candidates = {
        MakeIdentifiedCandidate(ShadowAtlas::CasterType::Point, 2.0f, 1),
        MakeIdentifiedCandidate(ShadowAtlas::CasterType::Spot, 1.0f, 2),
    };

    const auto result = allocator.Allocate(candidates);
    ASSERT_EQ(result.Accepted.size(), 2u);
    EXPECT_EQ(result.Accepted[0].EntryCount, 6u); // point light: 6 contiguous entries, never partial
    EXPECT_EQ(result.Accepted[1].EntryCount, 1u);

    // Same candidates again: the point caster must keep its whole cube (never
    // a partial one) AND land on the exact same 6 tiles via reuse.
    const auto second = allocator.Allocate(candidates);
    ASSERT_EQ(second.Accepted.size(), 2u);
    EXPECT_EQ(second.Accepted[0].EntryCount, 6u);
    EXPECT_EQ(second.Accepted[1].EntryCount, 1u);
    for (u32 face = 0; face < 6; ++face)
    {
        EXPECT_EQ(result.EntryRects[result.Accepted[0].BaseEntry + face].X,
                  second.EntryRects[second.Accepted[0].BaseEntry + face].X);
        EXPECT_EQ(result.EntryRects[result.Accepted[0].BaseEntry + face].Y,
                  second.EntryRects[second.Accepted[0].BaseEntry + face].Y);
    }
}

TEST(ShadowAtlasPersistentAllocator, TilesNeverOverlapAcrossRepeatedChangingCalls)
{
    ShadowAtlas::PersistentAllocator allocator(2048);
    std::mt19937 rng(0x51DEu);
    std::uniform_real_distribution<float> scoreDist(0.1f, 10.0f);

    for (int call = 0; call < 20; ++call)
    {
        std::vector<ShadowAtlas::Candidate> candidates;
        for (int i = 0; i < 10; ++i)
        {
            // Half carry a stable identity (eligible for reuse), half don't —
            // exercises both paths every call.
            const u64 userData = (i % 2 == 0) ? static_cast<u64>(1000 + i) : 0u;
            const auto type = (i % 3 == 0) ? ShadowAtlas::CasterType::Point : ShadowAtlas::CasterType::Spot;
            candidates.push_back(MakeIdentifiedCandidate(type, scoreDist(rng), userData));
        }

        const auto result = allocator.Allocate(candidates);
        for (sizet a = 0; a < result.EntryRects.size(); ++a)
        {
            const auto& rect = result.EntryRects[a];
            EXPECT_LE(rect.X + rect.Size, 2048u);
            EXPECT_LE(rect.Y + rect.Size, 2048u);
            for (sizet b = a + 1; b < result.EntryRects.size(); ++b)
                EXPECT_FALSE(RectsOverlap(rect, result.EntryRects[b]))
                    << "call " << call << ": entries " << a << " and " << b << " overlap";
        }
    }
}

TEST(ShadowAtlasPersistentAllocator, ChangingAtlasResolutionResetsHeldTiles)
{
    ShadowAtlas::PersistentAllocator allocator(1024);
    const auto before = allocator.Allocate(std::vector<ShadowAtlas::Candidate>{
        MakeIdentifiedCandidate(ShadowAtlas::CasterType::Spot, 5.0f, 1) });
    ASSERT_EQ(before.Accepted.size(), 1u);
    EXPECT_EQ(before.EntryRects[0].Size, 256u); // 1024/4

    allocator.SetAtlasResolution(2048);
    EXPECT_EQ(allocator.GetAtlasResolution(), 2048u);

    const auto after = allocator.Allocate(std::vector<ShadowAtlas::Candidate>{
        MakeIdentifiedCandidate(ShadowAtlas::CasterType::Spot, 5.0f, 1) });
    ASSERT_EQ(after.Accepted.size(), 1u);
    EXPECT_EQ(after.EntryRects[0].Size, 512u); // 2048/4 — retiered at the new resolution

    // A no-op resize (same value) must not disturb what is already held.
    allocator.SetAtlasResolution(2048);
    const auto unchanged = allocator.Allocate(std::vector<ShadowAtlas::Candidate>{
        MakeIdentifiedCandidate(ShadowAtlas::CasterType::Spot, 5.0f, 1) });
    ASSERT_EQ(unchanged.Accepted.size(), 1u);
    EXPECT_EQ(unchanged.EntryRects[0].X, after.EntryRects[0].X);
    EXPECT_EQ(unchanged.EntryRects[0].Y, after.EntryRects[0].Y);
}
