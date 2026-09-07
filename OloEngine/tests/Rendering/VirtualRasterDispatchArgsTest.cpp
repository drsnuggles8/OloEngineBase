// OLO_TEST_LAYER: cullinglod
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "VirtualRasterDispatchArgsMirror.h"

#include <algorithm>
#include <vector>

// =============================================================================
// Indirect dispatch arguments for the virtual-geometry software raster — CPU
// contract tests (issue #1048).
//
// The rule under test ships in
// `OloEditor/assets/shaders/include/VirtualRasterDispatchArgs.glsl`
// (`OloVirtualRasterDispatchArgs`) and is written to the work list's header by
// `compute/VirtualRasterArgs.comp`, which the pass then reads with
// `DispatchComputeIndirect`.
//
// Why this is worth a contract test at all: the count is GPU-written, so the
// CPU never sees the grid it dispatched. A grid that is too SMALL drops
// clusters with no error raised anywhere — no GL error, no validation-layer
// message, nothing but slightly-missing geometry. The single property that
// prevents it is
//
//     gx * gy >= count
//
// read together with the raster's own index recovery,
//
//     swRecordIndex = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x
//
// which is why the mirror carries both halves and every test below checks them
// against each other rather than against a hard-coded table.
//
// The GPU half of this contract — that the SHIPPED GLSL agrees with this mirror
// — is ShaderUnitVirtualRasterArgsTest in ShaderUnitTests.cpp.
// =============================================================================

namespace
{
    namespace Args = OloEngine::Tests::VirtualRasterDispatchArgs;

    // Grid axis cap in GL, and in every Vulkan maxComputeWorkGroupCount we
    // target. Exceeding it is a dispatch that does not happen.
    constexpr u32 kGridAxisCap = 65535u;

    // The counts worth checking exhaustively-ish: every boundary of the 4096
    // split, plus the sizes real frames produce. #1048 measured 3,165 software
    // records against 2,834,448 clusters on VirtualGeometryStress, so the
    // interesting range spans both sides of kMaxGroupsX.
    std::vector<u32> InterestingCounts()
    {
        std::vector<u32> counts{ 0u, 1u, 2u, 63u, 64u, 65u, 3165u, 9632u };
        for (u32 const base : { Args::kMaxGroupsX, Args::kMaxGroupsX * 2u, Args::kMaxGroupsX * 7u })
        {
            for (u32 const delta : { 2u, 1u, 0u })
                counts.push_back(base - delta);
            counts.push_back(base + 1u);
            counts.push_back(base + 2u);
        }
        // The real upper bound: the SW list capacity is the frame's total
        // cluster count, and the stress scene's is nearly three million.
        counts.push_back(2834448u);
        return counts;
    }
} // namespace

// The property the whole scheme rests on. A grid smaller than the count is
// silently dropped geometry; this is the only place that can catch it.
TEST(VirtualRasterDispatchArgsTest, GridCoversEveryRecord)
{
    for (u32 const count : InterestingCounts())
    {
        const auto args = Args::FromCount(count);
        const u64 covered = static_cast<u64>(args.X) * static_cast<u64>(args.Y);
        EXPECT_GE(covered, static_cast<u64>(count))
            << "count " << count << " -> grid " << args.X << "x" << args.Y
            << " covers only " << covered << " records: the raster would silently skip the rest";
    }
}

// The surplus is what the raster's own `swRecordIndex >= Count` guard
// early-outs on. It must stay bounded by one x-row, or the overdispatch this
// issue removed creeps back in.
TEST(VirtualRasterDispatchArgsTest, SurplusIsAtMostOneRowOfGroups)
{
    for (u32 const count : InterestingCounts())
    {
        const auto args = Args::FromCount(count);
        const u64 covered = static_cast<u64>(args.X) * static_cast<u64>(args.Y);
        const u64 surplus = covered - count;
        EXPECT_LT(surplus, static_cast<u64>(std::max(args.X, 1u)))
            << "count " << count << " -> grid " << args.X << "x" << args.Y
            << " wastes " << surplus << " groups, more than one x-row";
    }
}

// The inverse. Walking the grid the way VirtualClusterRaster.comp does must
// reach every record index in [0, count) exactly once — this is what makes
// "covers" above mean "rasterizes" rather than merely "launches enough
// threads".
TEST(VirtualRasterDispatchArgsTest, GridWalkReachesEveryRecordExactlyOnce)
{
    // Bounded so the walk stays cheap; the flattening is periodic in gx, so
    // the counts either side of kMaxGroupsX exercise every distinct shape.
    for (u32 const count : { 0u, 1u, 2u, 64u, 3165u, Args::kMaxGroupsX - 1u, Args::kMaxGroupsX,
                             Args::kMaxGroupsX + 1u, Args::kMaxGroupsX * 2u + 3u })
    {
        const auto args = Args::FromCount(count);
        std::vector<u8> seen(count, 0u);
        for (u32 y = 0; y < args.Y; ++y)
        {
            for (u32 x = 0; x < args.X; ++x)
            {
                const u32 record = Args::RecordIndexOf(x, y, args.X);
                if (record < count)
                {
                    EXPECT_EQ(seen[record], 0u) << "record " << record << " reached twice (count " << count << ")";
                    seen[record] = 1u;
                }
            }
        }
        for (u32 i = 0; i < count; ++i)
            EXPECT_EQ(seen[i], 1u) << "record " << i << " never reached (count " << count << ")";
    }
}

// Both axes must stay dispatchable. gx is capped by construction; gy is the
// one that grows, and at the stress scene's capacity it is still far under.
TEST(VirtualRasterDispatchArgsTest, BothAxesStayUnderTheGridCap)
{
    for (u32 const count : InterestingCounts())
    {
        const auto args = Args::FromCount(count);
        EXPECT_LE(args.X, kGridAxisCap) << "count " << count;
        EXPECT_LE(args.Y, kGridAxisCap) << "count " << count;
        EXPECT_EQ(args.Z, 1u) << "count " << count;
    }
}

// An empty work list must dispatch nothing at all. Before this was indirect
// the equivalent CPU expression had exactly this bug: groupsX = 0 made
// groupsY = (0 + 0 - 1) / 1 = 4294967295, a four-billion-group dispatch on
// an empty frame (the braced-guard comment in VirtualGeometryPass.cpp is the
// scar). The GLSL guards the divisor instead, and this pins it.
TEST(VirtualRasterDispatchArgsTest, EmptyListDispatchesNothing)
{
    const auto args = Args::FromCount(0u);
    EXPECT_EQ(args.X, 0u);
    EXPECT_EQ(args.Y, 0u);
    EXPECT_EQ(args.Z, 1u);
    EXPECT_EQ(static_cast<u64>(args.X) * static_cast<u64>(args.Y), 0u);
}

// A 1D dispatch for everything that fits, so the common frame keeps the
// simplest possible grid and the surplus is exactly zero.
TEST(VirtualRasterDispatchArgsTest, ShortListsAreExactAndOneDimensional)
{
    // Strictly <= kMaxGroupsX: past that the split is 2D by construction and
    // the exactness below is not the contract (9632, the wider shot in #1048,
    // is deliberately NOT here — it needs three rows).
    for (u32 const count : { 1u, 2u, 64u, 3165u, Args::kMaxGroupsX })
    {
        const auto args = Args::FromCount(count);
        EXPECT_EQ(args.X, count) << "count " << count;
        EXPECT_EQ(args.Y, 1u) << "count " << count;
        EXPECT_EQ(static_cast<u64>(args.X) * static_cast<u64>(args.Y), static_cast<u64>(count));
    }
}
