#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Occlusion/OcclusionState.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// OcclusionState Default Values
// =============================================================================

TEST(OcclusionState, DefaultConstruction)
{
    OcclusionState state{};
    EXPECT_EQ(state.QueryIndex, UINT32_MAX);
    EXPECT_TRUE(state.WasVisible);
    EXPECT_EQ(state.InvisibleFrameCount, 0u);
    EXPECT_EQ(state.LastTestedFrame, 0u);
}

TEST(OcclusionState, MutableFields)
{
    OcclusionState state{};
    state.QueryIndex = 42;
    state.WasVisible = false;
    state.InvisibleFrameCount = 5;
    state.LastTestedFrame = 100;

    EXPECT_EQ(state.QueryIndex, 42u);
    EXPECT_FALSE(state.WasVisible);
    EXPECT_EQ(state.InvisibleFrameCount, 5u);
    EXPECT_EQ(state.LastTestedFrame, 100u);
}

// =============================================================================
// OcclusionStateManager Fixture — isolates singleton state per test
// =============================================================================

class OcclusionStateManagerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto& mgr = OcclusionStateManager::GetInstance();
        mgr.Clear();
        mgr.SetMaxQueries(64);
    }

    void TearDown() override
    {
        OcclusionStateManager::GetInstance().Clear();
    }
};

// =============================================================================
// Basic State Management
// =============================================================================

TEST_F(OcclusionStateManagerTest, GetOrCreateNewState)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    auto& state = mgr.GetOrCreate(1001);

    // Newly created state should have defaults
    EXPECT_EQ(state.QueryIndex, UINT32_MAX);
    EXPECT_TRUE(state.WasVisible);
    EXPECT_EQ(state.InvisibleFrameCount, 0u);
}

TEST_F(OcclusionStateManagerTest, GetOrCreateReturnsSameState)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    auto& state1 = mgr.GetOrCreate(1001);
    state1.WasVisible = false;
    state1.InvisibleFrameCount = 7;

    auto& state2 = mgr.GetOrCreate(1001);
    EXPECT_FALSE(state2.WasVisible);
    EXPECT_EQ(state2.InvisibleFrameCount, 7u);
    EXPECT_EQ(&state1, &state2) << "Should return same object for same ID";
}

TEST_F(OcclusionStateManagerTest, HasReturnsFalseForUnknown)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    EXPECT_FALSE(mgr.Has(9999));
}

TEST_F(OcclusionStateManagerTest, HasReturnsTrueAfterCreate)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    mgr.GetOrCreate(42);
    EXPECT_TRUE(mgr.Has(42));
}

TEST_F(OcclusionStateManagerTest, RemoveDeletesState)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    mgr.GetOrCreate(42);
    ASSERT_TRUE(mgr.Has(42));

    mgr.Remove(42);
    EXPECT_FALSE(mgr.Has(42));
}

TEST_F(OcclusionStateManagerTest, RemoveNonExistentNoOp)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    // Should not crash
    mgr.Remove(9999);
    EXPECT_FALSE(mgr.Has(9999));
}

TEST_F(OcclusionStateManagerTest, MultipleObjects)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    auto& s1 = mgr.GetOrCreate(100);
    auto& s2 = mgr.GetOrCreate(200);
    auto& s3 = mgr.GetOrCreate(300);

    s1.WasVisible = false;
    s2.WasVisible = true;
    s3.WasVisible = false;

    EXPECT_FALSE(mgr.GetOrCreate(100).WasVisible);
    EXPECT_TRUE(mgr.GetOrCreate(200).WasVisible);
    EXPECT_FALSE(mgr.GetOrCreate(300).WasVisible);
}

// =============================================================================
// Query Index Allocation (Free-List)
// =============================================================================

TEST_F(OcclusionStateManagerTest, AllocateSequential)
{
    auto& mgr = OcclusionStateManager::GetInstance();

    u32 idx0 = mgr.AllocateQueryIndex();
    u32 idx1 = mgr.AllocateQueryIndex();
    u32 idx2 = mgr.AllocateQueryIndex();

    EXPECT_EQ(idx0, 0u);
    EXPECT_EQ(idx1, 1u);
    EXPECT_EQ(idx2, 2u);
}

TEST_F(OcclusionStateManagerTest, AllocateRespectsMaxQueries)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    mgr.Clear();
    mgr.SetMaxQueries(3);

    EXPECT_NE(mgr.AllocateQueryIndex(), UINT32_MAX);
    EXPECT_NE(mgr.AllocateQueryIndex(), UINT32_MAX);
    EXPECT_NE(mgr.AllocateQueryIndex(), UINT32_MAX);
    EXPECT_EQ(mgr.AllocateQueryIndex(), UINT32_MAX) << "Should exhaust after max";
}

TEST_F(OcclusionStateManagerTest, FreeAndReallocate)
{
    auto& mgr = OcclusionStateManager::GetInstance();

    u32 idx0 = mgr.AllocateQueryIndex();
    u32 idx1 = mgr.AllocateQueryIndex();

    mgr.FreeQueryIndex(idx0);

    u32 reused = mgr.AllocateQueryIndex();
    EXPECT_EQ(reused, idx0) << "Free-list should reuse the freed index";
}

TEST_F(OcclusionStateManagerTest, RemoveFreesQueryIndex)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    mgr.Clear();
    mgr.SetMaxQueries(2);

    // Allocate both query indices
    auto& s1 = mgr.GetOrCreate(100);
    s1.QueryIndex = mgr.AllocateQueryIndex(); // 0
    auto& s2 = mgr.GetOrCreate(200);
    s2.QueryIndex = mgr.AllocateQueryIndex(); // 1

    // Pool should be exhausted
    EXPECT_EQ(mgr.AllocateQueryIndex(), UINT32_MAX);

    // Removing s1 should free its query index
    mgr.Remove(100);
    u32 freed = mgr.AllocateQueryIndex();
    EXPECT_EQ(freed, 0u) << "Remove() should return query index to free-list";
}

TEST_F(OcclusionStateManagerTest, RemoveWithNoQueryNoFreeListCorruption)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    mgr.Clear();
    mgr.SetMaxQueries(2);

    // Create state but don't assign a query (QueryIndex = UINT32_MAX)
    mgr.GetOrCreate(100);
    mgr.Remove(100);

    // Allocation should still work normally (nothing was incorrectly freed)
    u32 idx = mgr.AllocateQueryIndex();
    EXPECT_EQ(idx, 0u);
}

// =============================================================================
// Frame Counter
// =============================================================================

TEST_F(OcclusionStateManagerTest, FrameCounterStartsAtZero)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    mgr.Clear();
    EXPECT_EQ(mgr.GetCurrentFrame(), 0u);
}

TEST_F(OcclusionStateManagerTest, BeginFrameIncrementsCounter)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    mgr.Clear();

    mgr.BeginFrame();
    EXPECT_EQ(mgr.GetCurrentFrame(), 1u);

    mgr.BeginFrame();
    EXPECT_EQ(mgr.GetCurrentFrame(), 2u);

    mgr.BeginFrame();
    EXPECT_EQ(mgr.GetCurrentFrame(), 3u);
}

TEST_F(OcclusionStateManagerTest, ClearResetsFrameCounter)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    mgr.BeginFrame();
    mgr.BeginFrame();
    ASSERT_EQ(mgr.GetCurrentFrame(), 2u);

    mgr.Clear();
    EXPECT_EQ(mgr.GetCurrentFrame(), 0u);
}

// =============================================================================
// Temporal Coherence Simulation
// =============================================================================

TEST_F(OcclusionStateManagerTest, SimulateTemporalCoherence)
{
    auto& mgr = OcclusionStateManager::GetInstance();

    // Simulate object becoming occluded over multiple frames
    auto& state = mgr.GetOrCreate(42);
    state.QueryIndex = mgr.AllocateQueryIndex();

    // Frame 1: visible
    mgr.BeginFrame();
    state.WasVisible = true;
    state.LastTestedFrame = mgr.GetCurrentFrame();
    EXPECT_TRUE(state.WasVisible);

    // Frames 2-5: occluded
    for (u32 i = 0; i < 4; ++i)
    {
        mgr.BeginFrame();
        state.WasVisible = false;
        state.InvisibleFrameCount++;
        state.LastTestedFrame = mgr.GetCurrentFrame();
    }

    EXPECT_FALSE(state.WasVisible);
    EXPECT_EQ(state.InvisibleFrameCount, 4u);
    EXPECT_EQ(state.LastTestedFrame, 5u);

    // After 4 invisible frames, temporal logic would re-test
    // (the magic number 4 comes from Renderer3D::DrawMesh)
    bool shouldRetest = (state.InvisibleFrameCount % 4 == 0);
    EXPECT_TRUE(shouldRetest);
}

TEST_F(OcclusionStateManagerTest, StressAllocFree)
{
    auto& mgr = OcclusionStateManager::GetInstance();
    mgr.Clear();
    mgr.SetMaxQueries(128);

    // Allocate all
    std::vector<u32> indices;
    for (u32 i = 0; i < 128; ++i)
    {
        u32 idx = mgr.AllocateQueryIndex();
        ASSERT_NE(idx, UINT32_MAX) << "Should succeed for index " << i;
        indices.push_back(idx);
    }

    // Pool exhausted
    EXPECT_EQ(mgr.AllocateQueryIndex(), UINT32_MAX);

    // Free all in reverse
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        mgr.FreeQueryIndex(*it);
    }

    // Re-allocate all (from free-list)
    for (u32 i = 0; i < 128; ++i)
    {
        u32 idx = mgr.AllocateQueryIndex();
        EXPECT_NE(idx, UINT32_MAX) << "Free-list should supply index " << i;
    }

    // Exhausted again
    EXPECT_EQ(mgr.AllocateQueryIndex(), UINT32_MAX);
}
