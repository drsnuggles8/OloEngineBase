// OLO_TEST_LAYER: unit
//
// Contract tests for the 3D ring-buffer chunk window (issue #729): O(1),
// non-hashing lookup via ChunkRingBuffer3D, boundary crossings that load only
// the incoming plane rather than rescanning the whole volume, and correctness
// across a floating-origin rebase (docs/agent-rules/
// floating-origin-rebase-subsystems.md). Pure CPU — no GL context, no scene.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Terrain/ChunkRingBuffer3D.h"
#include "OloEngine/Terrain/VolumetricChunkWindow.h"

#include <set>
#include <unordered_set>

using namespace OloEngine;

namespace
{
    struct ChunkCoordHash
    {
        sizet operator()(const glm::ivec3& c) const
        {
            sizet h = std::hash<i32>{}(c.x);
            h = h * 31 + std::hash<i32>{}(c.y);
            h = h * 31 + std::hash<i32>{}(c.z);
            return h;
        }
    };

    // Test payload: remembers which coordinate it was loaded for, so a test
    // can assert a slot's content actually matches its coordinate rather than
    // trusting the window's own bookkeeping.
    struct TestChunk
    {
        glm::ivec3 Coord{ 0 };
        u32 LoadOrdinal = 0;
    };

    // Counts load/unload calls and records every coordinate loaded, so tests
    // can assert exactly which chunks a boundary crossing touched.
    struct LoadTracker
    {
        u32 LoadCount = 0;
        u32 UnloadCount = 0;
        std::vector<glm::ivec3> LoadedCoords;
        std::unordered_set<glm::ivec3, ChunkCoordHash> ResidentCoords;

        VolumetricChunkWindow<TestChunk>::LoadFn MakeLoadFn()
        {
            return [this](const glm::ivec3& coord)
            {
                ++LoadCount;
                LoadedCoords.push_back(coord);
                ResidentCoords.insert(coord);
                TestChunk chunk;
                chunk.Coord = coord;
                chunk.LoadOrdinal = LoadCount;
                return chunk;
            };
        }

        VolumetricChunkWindow<TestChunk>::UnloadFn MakeUnloadFn()
        {
            return [this](const glm::ivec3& coord, TestChunk& /*chunk*/)
            {
                ++UnloadCount;
                ResidentCoords.erase(coord);
            };
        }
    };

    // Enumerate every coordinate within `radius` of `centre` on all three axes.
    std::vector<glm::ivec3> CubeAround(const glm::ivec3& centre, i32 radius)
    {
        std::vector<glm::ivec3> out;
        out.reserve(static_cast<sizet>(2 * radius + 1) * (2 * radius + 1) * (2 * radius + 1));
        for (i32 y = -radius; y <= radius; ++y)
            for (i32 z = -radius; z <= radius; ++z)
                for (i32 x = -radius; x <= radius; ++x)
                    out.push_back(centre + glm::ivec3(x, y, z));
        return out;
    }
} // namespace

// ============================================================================
// ChunkRingBuffer3D — the raw toroidal container
// ============================================================================

TEST(ChunkRingBuffer3D, AtIsAConstantTimeModuloNoHashing)
{
    // Correctness proxy for "O(1), no hashing": distinct coordinates that are
    // congruent modulo the side length must resolve to the SAME storage slot,
    // and the container never does anything but arithmetic to find it.
    ChunkRingBuffer3D<i32> buffer(5); // side length 5 -> period 5 per axis

    buffer.At(glm::ivec3(0, 0, 0)) = 111;
    EXPECT_EQ(buffer.At(glm::ivec3(5, 0, 0)), 111);
    EXPECT_EQ(buffer.At(glm::ivec3(-5, 0, 0)), 111);
    EXPECT_EQ(buffer.At(glm::ivec3(10, -5, 0)), 111);
}

TEST(ChunkRingBuffer3D, NegativeCoordinatesWrapCorrectlyRatherThanTruncating)
{
    // `%` truncates toward zero in C++; a naive `x % L` on a negative x would
    // produce a negative index. Assert the Euclidean-wrap fixture directly, in
    // the same spirit as DDGIMath.NegativeLatticeCoordinatesWrapIntoTheStorageWindow.
    ChunkRingBuffer3D<i32> buffer(3);

    for (i32 i = -9; i <= 9; ++i)
    {
        buffer.At(glm::ivec3(i, 0, 0)) = i;
    }
    // The last write for each residue class wins; -9..9 covers exactly 3 full
    // periods for a side length of 3, so the final value at every coordinate
    // congruent to `c` is whatever was written last, which is `c + 9` (the
    // highest value in range with that residue).
    EXPECT_EQ(buffer.At(glm::ivec3(0, 0, 0)), 9);
    EXPECT_EQ(buffer.At(glm::ivec3(-3, 0, 0)), 9);
    EXPECT_EQ(buffer.At(glm::ivec3(1, 0, 0)), 7);
    EXPECT_EQ(buffer.At(glm::ivec3(-2, 0, 0)), 7);
}

// ============================================================================
// VolumetricChunkWindow — boundary-plane loading
// ============================================================================

TEST(VolumetricChunkWindow, FirstUpdateLoadsExactlyTheInitialWindow)
{
    LoadTracker tracker;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 2;
    config.RenderRadius = 2;
    VolumetricChunkWindow<TestChunk> window(config, tracker.MakeLoadFn(), tracker.MakeUnloadFn());

    EXPECT_TRUE(window.Update(glm::vec3(0.0f)));

    const auto expected = CubeAround(glm::ivec3(0, 0, 0), 2);
    EXPECT_EQ(tracker.LoadCount, expected.size());
    for (const auto& coord : expected)
    {
        EXPECT_NE(window.TryGetChunk(coord), nullptr) << "missing (" << coord.x << "," << coord.y << "," << coord.z
                                                      << ")";
    }
}

TEST(VolumetricChunkWindow, StayingInTheSameChunkDoesNothing)
{
    LoadTracker tracker;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 2;
    config.RenderRadius = 2;
    VolumetricChunkWindow<TestChunk> window(config, tracker.MakeLoadFn(), tracker.MakeUnloadFn());

    window.Update(glm::vec3(0.0f, 0.0f, 0.0f));
    const u32 loadsAfterFirst = tracker.LoadCount;

    // Still inside chunk (0,0,0) (chunk size 10, covering [0,10) on every
    // axis) — no boundary crossed.
    EXPECT_FALSE(window.Update(glm::vec3(3.0f, 2.0f, 4.0f)));
    EXPECT_EQ(tracker.LoadCount, loadsAfterFirst);
}

TEST(VolumetricChunkWindow, CrossingOneAxisLoadsExactlyOnePlaneAndDropsTheOpposite)
{
    LoadTracker tracker;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 2;
    config.RenderRadius = 2;
    VolumetricChunkWindow<TestChunk> window(config, tracker.MakeLoadFn(), tracker.MakeUnloadFn());

    window.Update(glm::vec3(0.0f));
    ASSERT_EQ(tracker.ResidentCoords.size(), 5u * 5u * 5u);
    const u32 loadsBeforeCross = tracker.LoadCount;

    // Cross +X into chunk (1,0,0): world x = 10 (chunk size 10).
    window.Update(glm::vec3(10.5f, 0.0f, 0.0f));

    const u32 loadsThisStep = tracker.LoadCount - loadsBeforeCross;
    // Exactly one 5x5 plane (LoadRadius=2 -> side 5) is the incoming face.
    EXPECT_EQ(loadsThisStep, 5u * 5u);

    // The window is now centred on (1,0,0): every coordinate in that cube is
    // resident, and the vacated x=-2 plane relative to the OLD centre is gone.
    const auto newWindow = CubeAround(glm::ivec3(1, 0, 0), 2);
    for (const auto& coord : newWindow)
    {
        EXPECT_NE(window.TryGetChunk(coord), nullptr);
    }
    for (i32 y = -2; y <= 2; ++y)
    {
        for (i32 z = -2; z <= 2; ++z)
        {
            EXPECT_EQ(window.TryGetChunk(glm::ivec3(-2, y, z)), nullptr);
        }
    }
    EXPECT_EQ(tracker.ResidentCoords.size(), 5u * 5u * 5u);
}

TEST(VolumetricChunkWindow, CrossingNegativeAxisLoadsThePlaneOnTheCorrectSide)
{
    LoadTracker tracker;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 1;
    config.RenderRadius = 1;
    VolumetricChunkWindow<TestChunk> window(config, tracker.MakeLoadFn(), tracker.MakeUnloadFn());

    window.Update(glm::vec3(0.0f));
    window.Update(glm::vec3(-11.0f, 0.0f, 0.0f)); // chunk (-2, 0, 0)

    EXPECT_EQ(window.GetCentreChunk(), glm::ivec3(-2, 0, 0));
    for (i32 y = -1; y <= 1; ++y)
        for (i32 z = -1; z <= 1; ++z)
            EXPECT_NE(window.TryGetChunk(glm::ivec3(-3, y, z)), nullptr);
    for (i32 y = -1; y <= 1; ++y)
        for (i32 z = -1; z <= 1; ++z)
            EXPECT_EQ(window.TryGetChunk(glm::ivec3(1, y, z)), nullptr);
}

TEST(VolumetricChunkWindow, MultiAxisCrossingInOneUpdateLeavesEveryFinalSlotCorrect)
{
    // Camera jumps diagonally by one chunk on X and Z in a single Update()
    // call (e.g. a low framerate tick) — every coordinate in the final window
    // must resolve to itself, not to stale data from the old window.
    LoadTracker tracker;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 1;
    config.RenderRadius = 1;
    VolumetricChunkWindow<TestChunk> window(config, tracker.MakeLoadFn(), tracker.MakeUnloadFn());

    window.Update(glm::vec3(0.0f));
    window.Update(glm::vec3(10.5f, 0.0f, 10.5f)); // (1,0,0) and (0,0,1) crossed together

    EXPECT_EQ(window.GetCentreChunk(), glm::ivec3(1, 0, 1));
    const auto expected = CubeAround(glm::ivec3(1, 0, 1), 1);
    for (const auto& coord : expected)
    {
        const TestChunk* chunk = window.TryGetChunk(coord);
        ASSERT_NE(chunk, nullptr) << "missing (" << coord.x << "," << coord.y << "," << coord.z << ")";
        EXPECT_EQ(chunk->Coord, coord) << "slot holds stale data for (" << coord.x << "," << coord.y << ","
                                       << coord.z << ")";
    }
}

TEST(VolumetricChunkWindow, MultiChunkJumpWithinTheWindowLoadsOnlyTheCrossedPlanes)
{
    // Reference implementation assumes at most one chunk of motion per frame;
    // this window must not silently corrupt state on a larger jump that still
    // overlaps the old window (e.g. a stutter, or a teleport within range).
    LoadTracker tracker;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 3;
    config.RenderRadius = 3;
    VolumetricChunkWindow<TestChunk> window(config, tracker.MakeLoadFn(), tracker.MakeUnloadFn());

    window.Update(glm::vec3(0.0f));
    const u32 loadsBefore = tracker.LoadCount;

    window.Update(glm::vec3(20.5f, 0.0f, 0.0f)); // moves 2 chunks on X

    EXPECT_EQ(window.GetCentreChunk(), glm::ivec3(2, 0, 0));
    // Two planes crossed on X (from x=-3.. to x=... ), never a full 7^3 rescan.
    const u32 loadsThisStep = tracker.LoadCount - loadsBefore;
    EXPECT_LT(loadsThisStep, 7u * 7u * 7u);
    EXPECT_EQ(loadsThisStep, 2u * 7u * 7u);

    const auto expected = CubeAround(glm::ivec3(2, 0, 0), 3);
    for (const auto& coord : expected)
    {
        const TestChunk* chunk = window.TryGetChunk(coord);
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(chunk->Coord, coord);
    }
}

TEST(VolumetricChunkWindow, JumpBeyondTheWindowDiameterFallsBackToARebuild)
{
    // A teleport with no overlap between old and new windows has no "incoming
    // plane" — every slot is incoming, so a full rebuild is correct, not a bug.
    LoadTracker tracker;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 1;
    config.RenderRadius = 1;
    VolumetricChunkWindow<TestChunk> window(config, tracker.MakeLoadFn(), tracker.MakeUnloadFn());

    window.Update(glm::vec3(0.0f));
    const u32 loadsBefore = tracker.LoadCount;

    window.Update(glm::vec3(1000.0f, 0.0f, 0.0f)); // chunk (100,0,0) — far beyond the 3-wide window

    EXPECT_EQ(window.GetCentreChunk(), glm::ivec3(100, 0, 0));
    EXPECT_EQ(tracker.LoadCount - loadsBefore, 3u * 3u * 3u);
    for (const auto& coord : CubeAround(glm::ivec3(100, 0, 0), 1))
    {
        EXPECT_NE(window.TryGetChunk(coord), nullptr);
    }
}

TEST(VolumetricChunkWindow, RenderRadiusIsAStrictSubsetOfLoadRadius)
{
    LoadTracker tracker;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 3;
    config.RenderRadius = 1;
    VolumetricChunkWindow<TestChunk> window(config, tracker.MakeLoadFn(), tracker.MakeUnloadFn());

    window.Update(glm::vec3(0.0f));

    std::set<std::tuple<i32, i32, i32>> rendered;
    window.ForEachRenderableChunk(
        [&](const glm::ivec3& coord, const TestChunk&)
        { rendered.insert({ coord.x, coord.y, coord.z }); });

    EXPECT_EQ(rendered.size(), 3u * 3u * 3u); // (2*RenderRadius+1)^3

    // Chunks between RenderRadius and LoadRadius are loaded (lookup succeeds)
    // but not surfaced for rendering.
    EXPECT_NE(window.TryGetChunk(glm::ivec3(3, 0, 0)), nullptr);
    EXPECT_FALSE(rendered.contains({ 3, 0, 0 }));
}

// ============================================================================
// Floating-origin rebase (#613) — anchor-relative feeding is invariant
// ============================================================================

TEST(VolumetricChunkWindow, AnchorRelativePositionIsInvariantAcrossARebase)
{
    // Mirrors the terrain-streaming fix in docs/agent-rules/
    // floating-origin-rebase-subsystems.md §4: feed the window a position
    // relative to an anchor that itself shifts under RebaseOrigin, and the
    // window never has to know a rebase happened at all.
    LoadTracker trackerA;
    LoadTracker trackerB;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 2;
    config.RenderRadius = 2;

    VolumetricChunkWindow<TestChunk> windowA(config, trackerA.MakeLoadFn(), trackerA.MakeUnloadFn());
    VolumetricChunkWindow<TestChunk> windowB(config, trackerB.MakeLoadFn(), trackerB.MakeUnloadFn());

    glm::vec3 cameraWorld(123.4f, 5.0f, -67.8f);
    glm::vec3 anchorWorld(100.0f, 0.0f, -60.0f);

    // Window A: never rebased. Window B: fed anchor-relative coordinates,
    // then both camera and anchor are shifted by the SAME delta (a rebase),
    // and updated again from the still-anchor-relative position.
    windowA.Update(cameraWorld - anchorWorld);
    windowB.Update(cameraWorld - anchorWorld);
    ASSERT_EQ(windowA.GetCentreChunk(), windowB.GetCentreChunk());
    const u32 loadsAfterFirstUpdate = trackerB.LoadCount;

    const glm::vec3 rebaseShift(500.0f, 0.0f, -250.0f);
    cameraWorld += rebaseShift;
    anchorWorld += rebaseShift; // the anchor is a root TransformComponent too

    // Camera moved a little further within the same chunk after the rebase.
    cameraWorld += glm::vec3(0.1f, 0.0f, 0.2f);

    windowB.Update(cameraWorld - anchorWorld);

    // Anchor-relative position is unchanged (within-chunk motion only), so the
    // rebase must not have triggered any load/unload activity or a teleport.
    EXPECT_EQ(trackerB.LoadCount, loadsAfterFirstUpdate);
    EXPECT_EQ(windowA.GetCentreChunk(), windowB.GetCentreChunk());
}

TEST(VolumetricChunkWindow, RawWorldRelativeFeedingWouldTeleportAcrossARebaseAsAWarning)
{
    // Negative control: proves the invariance above is doing real work. If the
    // window is (incorrectly) fed a position that resets near the origin on a
    // rebase — the render-relative convention floating origin exists for —
    // it must observe that as a teleport (full rebuild), not silently corrupt
    // state. Documents the failure mode the anchor-relative test avoids.
    LoadTracker tracker;
    VolumetricChunkWindow<TestChunk>::Config config;
    config.ChunkWorldSize = 10.0f;
    config.LoadRadius = 1;
    config.RenderRadius = 1;
    VolumetricChunkWindow<TestChunk> window(config, tracker.MakeLoadFn(), tracker.MakeUnloadFn());

    window.Update(glm::vec3(505.0f, 0.0f, 0.0f)); // far from the origin, pre-rebase
    const u32 loadsBefore = tracker.LoadCount;

    // A rebase resets the render-relative camera position close to the
    // origin — fed naively (without the anchor-relative correction), that's
    // indistinguishable from a teleport to this window.
    window.Update(glm::vec3(5.0f, 0.0f, 0.0f));

    EXPECT_GT(tracker.LoadCount - loadsBefore, 0u);
    EXPECT_EQ(window.GetCentreChunk(), glm::ivec3(0, 0, 0));
}

TEST(VolumetricChunkWindow, WorldToChunkFloorsRatherThanTruncatesAtNegativeCoordinates)
{
    EXPECT_EQ(VolumetricChunkWindow<TestChunk>::WorldToChunk(glm::vec3(-0.1f, 0.0f, 0.0f), 10.0f),
              glm::ivec3(-1, 0, 0));
    EXPECT_EQ(VolumetricChunkWindow<TestChunk>::WorldToChunk(glm::vec3(-10.0f, 0.0f, 0.0f), 10.0f),
              glm::ivec3(-1, 0, 0));
    EXPECT_EQ(VolumetricChunkWindow<TestChunk>::WorldToChunk(glm::vec3(9.9f, 0.0f, 0.0f), 10.0f),
              glm::ivec3(0, 0, 0));
}
