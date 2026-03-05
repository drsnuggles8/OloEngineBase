#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"

#include <cstring>
#include <glm/glm.hpp>
#include <thread>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// Basic Write-Read Round-Trip
// =============================================================================

TEST(FrameDataBuffer, BoneMatrixWriteReadRoundTrip)
{
    FrameDataBuffer buffer(128, 128);
    buffer.Reset();

    constexpr u32 boneCount = 4;
    u32 offset = buffer.AllocateBoneMatrices(boneCount);
    ASSERT_NE(offset, UINT32_MAX) << "Failed to allocate bone matrices";

    // Write known data
    glm::mat4 bones[boneCount];
    for (u32 i = 0; i < boneCount; ++i)
    {
        bones[i] = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i), 0.0f, 0.0f));
    }
    buffer.WriteBoneMatrices(offset, bones, boneCount);

    // Read back and verify
    const glm::mat4* readBack = buffer.GetBoneMatrixPtr(offset);
    ASSERT_NE(readBack, nullptr);
    for (u32 i = 0; i < boneCount; ++i)
    {
        EXPECT_EQ(readBack[i], bones[i]) << "Bone matrix " << i << " mismatch";
        ValidateTransform(readBack[i]);
    }
}

TEST(FrameDataBuffer, TransformWriteReadRoundTrip)
{
    FrameDataBuffer buffer(128, 128);
    buffer.Reset();

    constexpr u32 transformCount = 8;
    u32 offset = buffer.AllocateTransforms(transformCount);
    ASSERT_NE(offset, UINT32_MAX) << "Failed to allocate transforms";

    glm::mat4 transforms[transformCount];
    for (u32 i = 0; i < transformCount; ++i)
    {
        transforms[i] = glm::scale(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i + 1)));
    }
    buffer.WriteTransforms(offset, transforms, transformCount);

    const glm::mat4* readBack = buffer.GetTransformPtr(offset);
    ASSERT_NE(readBack, nullptr);
    for (u32 i = 0; i < transformCount; ++i)
    {
        EXPECT_EQ(readBack[i], transforms[i]) << "Transform " << i << " mismatch";
    }
}

// =============================================================================
// Multiple Allocations Are Non-Overlapping
// =============================================================================

TEST(FrameDataBuffer, MultipleAllocationsNonOverlapping)
{
    FrameDataBuffer buffer(256, 256);
    buffer.Reset();

    u32 offset1 = buffer.AllocateBoneMatrices(10);
    u32 offset2 = buffer.AllocateBoneMatrices(10);
    u32 offset3 = buffer.AllocateBoneMatrices(10);

    ASSERT_NE(offset1, UINT32_MAX);
    ASSERT_NE(offset2, UINT32_MAX);
    ASSERT_NE(offset3, UINT32_MAX);

    // Offsets must not overlap
    EXPECT_NE(offset1, offset2);
    EXPECT_NE(offset2, offset3);
    EXPECT_NE(offset1, offset3);

    // Each allocation should be at least 10 apart
    EXPECT_GE(offset2, offset1 + 10);
    EXPECT_GE(offset3, offset2 + 10);
}

// =============================================================================
// Reset Clears State
// =============================================================================

TEST(FrameDataBuffer, ResetClearsState)
{
    FrameDataBuffer buffer(128, 128);

    // Allocate some data
    buffer.AllocateBoneMatrices(10);
    buffer.AllocateTransforms(10);
    EXPECT_GT(buffer.GetBoneMatrixCount(), 0u);
    EXPECT_GT(buffer.GetTransformCount(), 0u);

    // Reset
    buffer.Reset();
    EXPECT_EQ(buffer.GetBoneMatrixCount(), 0u);
    EXPECT_EQ(buffer.GetTransformCount(), 0u);
}

// =============================================================================
// Allocation Failure When Exceeding Capacity
// =============================================================================

TEST(FrameDataBuffer, AllocationFailsWhenFull)
{
    FrameDataBuffer buffer(10, 10); // Very small capacity
    buffer.Reset();

    // Allocate all capacity
    u32 offset = buffer.AllocateBoneMatrices(10);
    EXPECT_NE(offset, UINT32_MAX);

    // Next allocation should fail
    u32 overflowOffset = buffer.AllocateBoneMatrices(1);
    EXPECT_EQ(overflowOffset, UINT32_MAX);
}

// =============================================================================
// No Capacity Creep Over Many Reset Cycles
// =============================================================================

TEST(FrameDataBuffer, NoCapacityCreepOverResetCycles)
{
    FrameDataBuffer buffer(256, 256);

    sizet initialBoneCap = buffer.GetBoneMatrixCapacity();
    sizet initialTransformCap = buffer.GetTransformCapacity();

    for (int frame = 0; frame < 100; ++frame)
    {
        buffer.Reset();
        buffer.AllocateBoneMatrices(50);
        buffer.AllocateTransforms(50);
    }

    // Capacity should not have grown
    EXPECT_EQ(buffer.GetBoneMatrixCapacity(), initialBoneCap)
        << "Bone capacity grew after " << 100 << " frames";
    EXPECT_EQ(buffer.GetTransformCapacity(), initialTransformCap)
        << "Transform capacity grew after " << 100 << " frames";
}

// =============================================================================
// Statistics Track Correctly
// =============================================================================

TEST(FrameDataBuffer, StatisticsTrack)
{
    FrameDataBuffer buffer(256, 256);
    buffer.Reset();

    EXPECT_EQ(buffer.GetBoneMatrixCount(), 0u);
    EXPECT_EQ(buffer.GetTransformCount(), 0u);

    buffer.AllocateBoneMatrices(5);
    EXPECT_EQ(buffer.GetBoneMatrixCount(), 5u);

    buffer.AllocateBoneMatrices(3);
    EXPECT_EQ(buffer.GetBoneMatrixCount(), 8u);

    buffer.AllocateTransforms(10);
    EXPECT_EQ(buffer.GetTransformCount(), 10u);
}

// =============================================================================
// Worker Scratch Buffer Isolation (Parallel API)
// =============================================================================

TEST(FrameDataBuffer, WorkerScratchIsolation)
{
    FrameDataBuffer buffer(1024, 1024);
    buffer.Reset();
    buffer.PrepareForParallelSubmission();

    // Worker 0 writes bones
    u32 w0Offset = buffer.AllocateBoneMatricesParallel(0, 4);
    glm::mat4 w0Bones[4];
    for (u32 i = 0; i < 4; ++i)
        w0Bones[i] = glm::translate(glm::mat4(1.0f), glm::vec3(100.0f + static_cast<f32>(i), 0.0f, 0.0f));
    buffer.WriteBoneMatricesParallel(0, w0Offset, w0Bones, 4);

    // Worker 1 writes different bones
    u32 w1Offset = buffer.AllocateBoneMatricesParallel(1, 4);
    glm::mat4 w1Bones[4];
    for (u32 i = 0; i < 4; ++i)
        w1Bones[i] = glm::translate(glm::mat4(1.0f), glm::vec3(200.0f + static_cast<f32>(i), 0.0f, 0.0f));
    buffer.WriteBoneMatricesParallel(1, w1Offset, w1Bones, 4);

    // Merge
    buffer.MergeScratchBuffers();

    // Verify each worker's data is intact after merge
    u32 globalOffset0 = buffer.GetGlobalBoneOffset(0, w0Offset);
    u32 globalOffset1 = buffer.GetGlobalBoneOffset(1, w1Offset);

    const glm::mat4* readBack0 = buffer.GetBoneMatrixPtr(globalOffset0);
    const glm::mat4* readBack1 = buffer.GetBoneMatrixPtr(globalOffset1);

    ASSERT_NE(readBack0, nullptr);
    ASSERT_NE(readBack1, nullptr);

    for (u32 i = 0; i < 4; ++i)
    {
        EXPECT_EQ(readBack0[i], w0Bones[i]) << "Worker 0 bone " << i << " corrupted";
        EXPECT_EQ(readBack1[i], w1Bones[i]) << "Worker 1 bone " << i << " corrupted";
    }
}

// =============================================================================
// Parallel Submission Prepare-Merge Cycle
// =============================================================================

TEST(FrameDataBuffer, PrepareAndMergeCycle)
{
    FrameDataBuffer buffer(256, 256);

    for (int frame = 0; frame < 10; ++frame)
    {
        buffer.Reset();
        buffer.PrepareForParallelSubmission();

        // Simulate 3 workers
        for (u32 w = 0; w < 3; ++w)
        {
            u32 offset = buffer.AllocateBoneMatricesParallel(w, 2);
            glm::mat4 data[2] = { glm::mat4(1.0f), glm::mat4(2.0f) };
            buffer.WriteBoneMatricesParallel(w, offset, data, 2);
        }

        ASSERT_NO_FATAL_FAILURE(buffer.MergeScratchBuffers())
            << "MergeScratchBuffers crashed on frame " << frame;

        // After merge, we should have 6 bone matrices total
        EXPECT_EQ(buffer.GetBoneMatrixCount(), 6u)
            << "Frame " << frame << ": expected 6 bone matrices after merge";
    }
}

// =============================================================================
// RenderState Table Tests
// =============================================================================

TEST(FrameDataBuffer, RenderStateTableAllocateReturnsValidIndex)
{
    FrameDataBuffer buffer(16, 16);
    buffer.Reset();

    PODRenderState state{};
    u16 index = buffer.AllocateRenderState(state);
    EXPECT_NE(index, INVALID_RENDER_STATE_INDEX);
    EXPECT_EQ(index, 0u);
    EXPECT_EQ(buffer.GetRenderStateCount(), 1u);
}

TEST(FrameDataBuffer, RenderStateTableDeduplicatesIdenticalStates)
{
    FrameDataBuffer buffer(16, 16);
    buffer.Reset();

    PODRenderState state{};
    u16 first = buffer.AllocateRenderState(state);
    u16 second = buffer.AllocateRenderState(state);

    EXPECT_EQ(first, second) << "Identical states must return same index";
    EXPECT_EQ(buffer.GetRenderStateCount(), 1u) << "Only one unique state should exist";
}

TEST(FrameDataBuffer, RenderStateTableDifferentStatesGetDifferentIndices)
{
    FrameDataBuffer buffer(16, 16);
    buffer.Reset();

    PODRenderState opaque{};
    opaque.depthTestEnabled = true;
    opaque.blendEnabled = false;

    PODRenderState transparent{};
    transparent.depthTestEnabled = true;
    transparent.blendEnabled = true;
    transparent.blendSrcFactor = GL_SRC_ALPHA;
    transparent.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;

    u16 opaqueIdx = buffer.AllocateRenderState(opaque);
    u16 transIdx = buffer.AllocateRenderState(transparent);

    EXPECT_NE(opaqueIdx, transIdx) << "Different states must get different indices";
    EXPECT_EQ(buffer.GetRenderStateCount(), 2u);
}

TEST(FrameDataBuffer, RenderStateTableRoundTrip)
{
    FrameDataBuffer buffer(16, 16);
    buffer.Reset();

    PODRenderState state{};
    state.blendEnabled = true;
    state.blendSrcFactor = GL_ONE;
    state.blendDstFactor = GL_ZERO;
    state.depthTestEnabled = false;
    state.cullingEnabled = true;
    state.cullFace = GL_FRONT;
    state.polygonMode = GL_LINE;

    u16 index = buffer.AllocateRenderState(state);
    const PODRenderState& retrieved = buffer.GetRenderState(index);

    EXPECT_EQ(std::memcmp(&state, &retrieved, sizeof(PODRenderState)), 0)
        << "Retrieved state must match original byte-for-byte";
}

TEST(FrameDataBuffer, RenderStateTableResetsEachFrame)
{
    FrameDataBuffer buffer(16, 16);
    buffer.Reset();

    PODRenderState state{};
    state.blendEnabled = true;
    buffer.AllocateRenderState(state);
    EXPECT_EQ(buffer.GetRenderStateCount(), 1u);

    // Simulate new frame
    buffer.Reset();
    EXPECT_EQ(buffer.GetRenderStateCount(), 0u) << "Table must be empty after Reset()";

    // Re-allocate — should get index 0 again
    u16 index = buffer.AllocateRenderState(state);
    EXPECT_EQ(index, 0u);
    EXPECT_EQ(buffer.GetRenderStateCount(), 1u);
}

TEST(FrameDataBuffer, RenderStateTableMultipleUniqueStates)
{
    FrameDataBuffer buffer(16, 16);
    buffer.Reset();

    // Allocate 10 unique states
    for (u16 i = 0; i < 10; ++i)
    {
        PODRenderState state{};
        state.lineWidth = static_cast<f32>(i + 1);
        u16 index = buffer.AllocateRenderState(state);
        EXPECT_EQ(index, i) << "State " << i << " should get sequential index";
    }
    EXPECT_EQ(buffer.GetRenderStateCount(), 10u);

    // Verify dedup: re-allocate same states
    for (u16 i = 0; i < 10; ++i)
    {
        PODRenderState state{};
        state.lineWidth = static_cast<f32>(i + 1);
        u16 index = buffer.AllocateRenderState(state);
        EXPECT_EQ(index, i) << "Re-allocated state " << i << " should match original index";
    }
    EXPECT_EQ(buffer.GetRenderStateCount(), 10u) << "No new states should be added";
}
