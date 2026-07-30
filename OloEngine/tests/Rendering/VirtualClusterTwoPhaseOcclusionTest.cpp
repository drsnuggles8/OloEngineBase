// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// VirtualClusterTwoPhaseOcclusionTest.cpp
//
// Pins the two-phase occlusion contract issue #682 adds to the virtualized-
// geometry cluster cull (VirtualClusterCull.comp + VirtualGeometryPass):
//
//   * PHASE 1 tests every cut-selected cluster against the PREVIOUS frame's
//     depth pyramid, reprojected with the previous view-projection and the
//     instance's PrevTransform. That pyramid was built from last frame's FINAL
//     depth, so it already contains virtual geometry — which is the whole point:
//     the single-phase scheme tested THIS frame's pyramid, which can only ever
//     hold the classic-path occluders ScenePass drew, so a VG occluder could
//     never cull the VG behind it.
//   * A phase-1 hit is DEFERRED to a reject list, not dropped.
//   * PHASE 2 re-tests only the reject list against a pyramid rebuilt from this
//     frame's depth (opaque scene + phase-1 VG draws), with the CURRENT VP and
//     transform. Still hidden -> culled; visible -> drawn. So phase 1 is allowed
//     to be wrong in the reject direction and the scheme stays hole-free.
//
// The occlusion math itself is the same screen-rect-vs-max-pyramid test the
// instanced path uses (already pinned byte-for-byte by GPUOcclusionCullParityTest
// and DeferredTwoPhaseOcclusionTest); this file models the CLUSTER-level
// decision built on top of it, plus the command/args region arithmetic that
// keeps the two MDI replays from stepping on each other.
//
// Classification: shaderpipe (pure CPU, no GL context).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <functional>
#include <set>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        struct OcclusionParams
        {
            glm::mat4 ViewProjection{ 1.0f }; // the VP the bound pyramid was rendered with
            glm::vec2 HZBSize{ 1024.0f, 1024.0f };
            glm::vec2 HZBUVFactor{ 1.0f };
            i32 MipCount = 11;
            f32 DepthBias = 0.0f;
        };

        using HZBSampler = std::function<f32(glm::ivec2 texel, int mip)>;

        // Re-implementation of VirtualClusterCull.comp's IsOccluded(). Identical
        // in shape to InstanceOcclusionCull.comp's isOccluded() — the difference
        // #682 introduces is only WHICH view-projection is supplied
        // (u_OcclusionViewProjection always names the VP that matches the bound
        // pyramid). Returns true when it is safe to cull the sphere.
        bool IsOccluded(glm::vec3 worldCenter, f32 worldRadius,
                        const OcclusionParams& p, const HZBSampler& sampleHZB)
        {
            glm::vec2 rectMin{ 1e30f };
            glm::vec2 rectMax{ -1e30f };
            f32 nearestZ = 1e30f;
            for (i32 i = 0; i < 8; ++i)
            {
                const glm::vec3 corner = worldCenter + worldRadius * glm::vec3(
                                                                         (i & 1) != 0 ? 1.0f : -1.0f,
                                                                         (i & 2) != 0 ? 1.0f : -1.0f,
                                                                         (i & 4) != 0 ? 1.0f : -1.0f);
                const glm::vec4 clip = p.ViewProjection * glm::vec4(corner, 1.0f);
                if (clip.w <= 0.0f)
                    return false; // straddles / behind camera — cannot project safely
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                rectMin = glm::min(rectMin, glm::vec2(ndc) * 0.5f + 0.5f);
                rectMax = glm::max(rectMax, glm::vec2(ndc) * 0.5f + 0.5f);
                nearestZ = glm::min(nearestZ, ndc.z * 0.5f + 0.5f);
            }

            if (rectMin.x < 0.0f || rectMin.y < 0.0f || rectMax.x > 1.0f || rectMax.y > 1.0f)
                return false;
            if (rectMax.x <= rectMin.x || rectMax.y <= rectMin.y)
                return false;

            const glm::vec2 rectMinTexel = (rectMin * p.HZBUVFactor) * p.HZBSize;
            const glm::vec2 rectMaxTexel = (rectMax * p.HZBUVFactor) * p.HZBSize;

            const glm::vec2 sizeTexels = rectMaxTexel - rectMinTexel;
            const f32 maxSide = glm::max(sizeTexels.x, sizeTexels.y);
            const int mip = glm::clamp(static_cast<int>(glm::ceil(glm::log2(glm::max(maxSide, 1.0f)))),
                                       0, p.MipCount - 1);

            const int mipW = glm::max(static_cast<int>(p.HZBSize.x) >> mip, 1);
            const int mipH = glm::max(static_cast<int>(p.HZBSize.y) >> mip, 1);
            glm::ivec2 lo{ static_cast<int>(glm::floor(rectMinTexel.x)) >> mip,
                           static_cast<int>(glm::floor(rectMinTexel.y)) >> mip };
            glm::ivec2 hi{ static_cast<int>(glm::floor(rectMaxTexel.x)) >> mip,
                           static_cast<int>(glm::floor(rectMaxTexel.y)) >> mip };
            hi = glm::min(hi, lo + glm::ivec2(1));
            lo = glm::clamp(lo, glm::ivec2(0), glm::ivec2(mipW - 1, mipH - 1));
            hi = glm::clamp(hi, glm::ivec2(0), glm::ivec2(mipW - 1, mipH - 1));

            // Farthest occluder over the footprint = MAX of the covered texels.
            f32 occluderZ = 0.0f;
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x)
                    occluderZ = glm::max(occluderZ, sampleHZB(glm::ivec2(x, y), mip));

            return nearestZ > occluderZ + p.DepthBias;
        }

        glm::mat4 MakeViewProjection(glm::vec3 eye)
        {
            const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
            const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            return projection * view;
        }

        f32 DeviceZForDistance(glm::vec3 eye, f32 dist)
        {
            const glm::mat4 vp = MakeViewProjection(eye);
            const glm::vec4 clip = vp * glm::vec4(eye + glm::vec3(0.0f, 0.0f, -dist), 1.0f);
            return (clip.z / clip.w) * 0.5f + 0.5f;
        }

        // Where a cluster ends up in the frame.
        enum class ClusterStage
        {
            Phase1, // survived phase 1 -> drawn from the phase-1 command region
            Phase2, // rejected in phase 1, recovered in phase 2 -> phase-2 region
            Culled, // rejected by both pyramids -> never rasterized
        };

        // Models VirtualClusterCull.comp's two-phase decision for one cluster of
        // one instance. `prevTransform` / `transform` are the instance's, mirroring
        // the shader: phase 1 projects the cull sphere through PrevTransform (where
        // the cluster WAS, matching the retained pyramid), phase 2 through the
        // current Transform (matching the freshly built one).
        ClusterStage ClassifyCluster(const glm::vec4& meshLocalCullSphere,
                                     const glm::mat4& prevTransform, const glm::mat4& transform,
                                     f32 maxScale,
                                     const OcclusionParams& prevParams, const HZBSampler& prevHZB,
                                     const OcclusionParams& currParams, const HZBSampler& currHZB)
        {
            const glm::vec3 prevCenter = glm::vec3(prevTransform * glm::vec4(glm::vec3(meshLocalCullSphere), 1.0f));
            const f32 prevScale = glm::max(glm::max(glm::length(glm::vec3(prevTransform[0])),
                                                    glm::length(glm::vec3(prevTransform[1]))),
                                           glm::length(glm::vec3(prevTransform[2])));
            if (!IsOccluded(prevCenter, meshLocalCullSphere.w * prevScale, prevParams, prevHZB))
                return ClusterStage::Phase1;

            const glm::vec3 center = glm::vec3(transform * glm::vec4(glm::vec3(meshLocalCullSphere), 1.0f));
            return IsOccluded(center, meshLocalCullSphere.w * maxScale, currParams, currHZB)
                       ? ClusterStage::Culled
                       : ClusterStage::Phase2;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The feature itself: a VG occluder in LAST frame's pyramid culls the VG
    // behind it. The single-phase scheme could not, because the pyramid it tested
    // was built from ScenePass's depth — classic-path occluders only, no virtual
    // geometry — so a cluster hidden purely by other virtual geometry survived.
    // -------------------------------------------------------------------------
    TEST(VirtualClusterTwoPhaseOcclusion, ClusterHiddenByVirtualGeometryIsCulledByBothPyramids)
    {
        const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };

        OcclusionParams prev;
        prev.ViewProjection = MakeViewProjection(eye);
        OcclusionParams curr = prev;

        // The previous frame's FINAL depth: a virtual-geometry statue 10 units out.
        const f32 statueZ = DeviceZForDistance(eye, 10.0f);
        const HZBSampler prevWithVG = [statueZ](glm::ivec2, int)
        { return statueZ; };
        // The single-phase pyramid: built mid-frame from ScenePass's depth, which
        // contains no virtual geometry at all — everything reads as far.
        const HZBSampler classicOnly = [](glm::ivec2, int)
        { return 1.0f; };
        // This frame's rebuilt pyramid: the phase-1 statue draws are in it.
        const HZBSampler currWithVG = prevWithVG;

        const glm::vec4 cullSphere{ 0.0f, 0.0f, -25.0f, 0.5f }; // a statue behind the first one
        const glm::mat4 identity{ 1.0f };

        EXPECT_FALSE(IsOccluded(glm::vec3(cullSphere), cullSphere.w, curr, classicOnly))
            << "single-phase (this frame's ScenePass-only pyramid) cannot see the VG occluder — "
               "the cluster survives, which is exactly the gap #682 closes";

        EXPECT_EQ(ClassifyCluster(cullSphere, identity, identity, 1.0f, prev, prevWithVG, curr, currWithVG),
                  ClusterStage::Culled)
            << "a cluster hidden by virtual geometry must be rejected by phase 1 AND stay rejected in phase 2";
    }

    // -------------------------------------------------------------------------
    // Hole-freeness: whatever phase 1 gets wrong, phase 2 recovers. Last frame the
    // cluster was behind an occluder; this frame it is not. A pure previous-frame
    // test would drop it for a frame (a visible pop / hole in the DAG cut).
    // -------------------------------------------------------------------------
    TEST(VirtualClusterTwoPhaseOcclusion, DisocclusionRecoveredInPhase2)
    {
        const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };

        OcclusionParams prev;
        prev.ViewProjection = MakeViewProjection(eye);
        OcclusionParams curr = prev;

        const f32 wallZ = DeviceZForDistance(eye, 10.0f);
        const HZBSampler prevWall = [wallZ](glm::ivec2, int)
        { return wallZ; };
        const HZBSampler currEmpty = [](glm::ivec2, int)
        { return 1.0f; }; // the occluder moved away this frame

        const glm::vec4 cullSphere{ 0.0f, 0.0f, -25.0f, 0.5f };
        const glm::mat4 identity{ 1.0f };

        EXPECT_EQ(ClassifyCluster(cullSphere, identity, identity, 1.0f, prev, prevWall, curr, currEmpty),
                  ClusterStage::Phase2)
            << "a disoccluded cluster must come back in phase 2 — a phase-1 reject nobody re-tests is a hole";
    }

    // -------------------------------------------------------------------------
    // A camera cut is the worst case for the reprojected phase-1 test: last
    // frame's pyramid describes a completely different view. Phase 2 still has to
    // put every genuinely visible cluster back.
    // -------------------------------------------------------------------------
    TEST(VirtualClusterTwoPhaseOcclusion, CameraCutCannotLoseAVisibleCluster)
    {
        // Previous frame: camera at the origin, a wall filling the pyramid.
        OcclusionParams prev;
        prev.ViewProjection = MakeViewProjection(glm::vec3{ 0.0f, 0.0f, 0.0f });
        const f32 wallZ = DeviceZForDistance(glm::vec3{ 0.0f, 0.0f, 0.0f }, 2.0f);
        const HZBSampler prevWall = [wallZ](glm::ivec2, int)
        { return wallZ; };

        // This frame the camera teleported: a totally different VP, nothing in
        // front of the cluster any more.
        OcclusionParams curr;
        curr.ViewProjection = MakeViewProjection(glm::vec3{ 60.0f, 12.0f, 40.0f });
        const HZBSampler currEmpty = [](glm::ivec2, int)
        { return 1.0f; };

        // Sits in front of the teleported camera (it looks down -Z from the new eye).
        const glm::vec4 cullSphere{ 60.0f, 12.0f, 20.0f, 0.5f };
        const glm::mat4 identity{ 1.0f };

        EXPECT_NE(ClassifyCluster(cullSphere, identity, identity, 1.0f, prev, prevWall, curr, currEmpty),
                  ClusterStage::Culled)
            << "a camera cut must never cull a cluster outright — phase 1 may mis-reject, phase 2 decides";
    }

    // -------------------------------------------------------------------------
    // A moving instance's phase-1 bounds come from PrevTransform (where it WAS,
    // matching last frame's pyramid), not Transform. Using the current transform
    // against a previous-frame pyramid is the classic reprojection bug: it tests
    // the object at a position that frame's depth knows nothing about.
    // -------------------------------------------------------------------------
    TEST(VirtualClusterTwoPhaseOcclusion, Phase1ReprojectsThroughThePreviousTransform)
    {
        const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
        OcclusionParams prev;
        prev.ViewProjection = MakeViewProjection(eye);
        OcclusionParams curr = prev;

        // Last frame's depth: a wall covering only the LEFT half of the screen.
        const f32 wallZ = DeviceZForDistance(eye, 10.0f);
        const HZBSampler prevLeftWall = [wallZ](glm::ivec2 texel, int mip)
        {
            const int mipW = glm::max(1024 >> mip, 1);
            return texel.x < mipW / 2 ? wallZ : 1.0f;
        };
        const HZBSampler currEmpty = [](glm::ivec2, int)
        { return 1.0f; };

        // The instance was well left of centre last frame (behind the wall) and has
        // moved to the right half this frame.
        const glm::vec4 cullSphere{ 0.0f, 0.0f, 0.0f, 0.5f }; // mesh-local, at the origin
        const glm::mat4 prevTransform = glm::translate(glm::mat4(1.0f), glm::vec3(-8.0f, 0.0f, -25.0f));
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(8.0f, 0.0f, -25.0f));

        EXPECT_EQ(ClassifyCluster(cullSphere, prevTransform, transform, 1.0f, prev, prevLeftWall, curr, currEmpty),
                  ClusterStage::Phase2)
            << "phase 1 must test the PREVIOUS position against the previous pyramid (it was hidden there), "
               "and phase 2 the current one";

        // Control: had phase 1 (wrongly) used the current transform, the cluster
        // would have been on the un-walled right half and drawn in phase 1.
        EXPECT_EQ(ClassifyCluster(cullSphere, transform, transform, 1.0f, prev, prevLeftWall, curr, currEmpty),
                  ClusterStage::Phase1)
            << "test setup: the current position is NOT behind last frame's wall";
    }

    // -------------------------------------------------------------------------
    // The region arithmetic that keeps the two MDI replays apart.
    //
    // Phase 1 draws before phase 2's cull even runs, so phase 2 cannot append to
    // phase 1's command segment: the second replay would re-issue every phase-1
    // draw (double raster, doubled overdraw counters). VirtualMeshRegistry sizes
    // the command / visible / args buffers for TWO regions and the shader offsets
    // phase 2 by u_CommandSlotBase = frame cluster count and u_ArgsSlotBase =
    // frame instance count — CPU-known constants, which is what lets the second
    // glMultiDrawElementsIndirectCount name the region without a GPU readback.
    // -------------------------------------------------------------------------
    TEST(VirtualClusterTwoPhaseOcclusion, PhaseCommandRegionsAreDisjointAndInBounds)
    {
        // Three instances with 4 / 7 / 5 clusters, packed exactly as
        // VirtualMeshRegistry::PrepareFrame packs them.
        const std::vector<u32> clusterCounts{ 4u, 7u, 5u };
        std::vector<u32> commandBases;
        u32 frameClusterCount = 0;
        for (u32 count : clusterCounts)
        {
            commandBases.push_back(frameClusterCount);
            frameClusterCount += count;
        }
        const auto instanceCount = static_cast<u32>(clusterCounts.size());
        ASSERT_EQ(frameClusterCount, 16u);

        // Every slot either phase can possibly write, at the worst case where a
        // cluster lands in phase 1 for one instance and phase 2 for another.
        std::set<u32> slots;
        for (u32 phase = 0; phase < 2; ++phase)
        {
            const u32 commandSlotBase = phase == 0 ? 0u : frameClusterCount;
            for (sizet i = 0; i < clusterCounts.size(); ++i)
            {
                for (u32 local = 0; local < clusterCounts[i]; ++local)
                {
                    const u32 slot = commandSlotBase + commandBases[i] + local;
                    EXPECT_TRUE(slots.insert(slot).second)
                        << "command slot " << slot << " is claimed twice — a phase-2 draw would overwrite a "
                                                      "phase-1 command (or vice versa)";
                    EXPECT_LT(slot, frameClusterCount * 2u)
                        << "command slot " << slot << " is past the two-region buffer VirtualMeshRegistry allocates";
                }
            }
        }

        // The args parameter-buffer offsets: phase 2 reads its draw count from
        // args[instanceCount + i], which must clear phase 1's [0, n) region and
        // stay inside the doubled allocation.
        for (u32 i = 0; i < instanceCount; ++i)
        {
            const u32 phase1Offset = i * static_cast<u32>(sizeof(VirtualDrawArgs));
            const u32 phase2Offset = (instanceCount + i) * static_cast<u32>(sizeof(VirtualDrawArgs));
            EXPECT_GE(phase2Offset, instanceCount * static_cast<u32>(sizeof(VirtualDrawArgs)));
            EXPECT_NE(phase1Offset, phase2Offset);
            EXPECT_LT(phase2Offset, instanceCount * 2u * static_cast<u32>(sizeof(VirtualDrawArgs)));
            // glMultiDrawElementsIndirectCount's parameter-buffer offset must be
            // 4-byte aligned; the 16-byte VirtualDrawArgs stride guarantees it.
            EXPECT_EQ(phase2Offset % 4u, 0u);
        }
    }

    // -------------------------------------------------------------------------
    // The reject list can never overflow. Phase 1 dispatches exactly one thread
    // per cluster of each instance, so the worst case is "every cluster rejected"
    // = the frame's cluster count, which is also the bound VirtualGeometryPass
    // hands the shader as `u_RejectCapacity`.
    //
    // The load-bearing part is that the ALLOCATION actually holds that many
    // records. The buffer is a 16-byte header followed by the record array, so
    // the sizing expression has to add the header — drop it and the capacity is
    // one record short of the bound the shader trusts, which is a silent
    // out-of-bounds write on the fullest possible frame. So this re-derives the
    // record capacity from the production byte expression rather than from the
    // cluster count, which would just restate the premise.
    //
    // (The shader also bounds-checks the append and DRAWS the cluster instead of
    // dropping it if it ever failed, since a lost reject is a hole.)
    // -------------------------------------------------------------------------
    TEST(VirtualClusterTwoPhaseOcclusion, RejectListAllocationHoldsTheWorstCaseRejectCount)
    {
        // VirtualMeshRegistry::EnsureFrameBuffers, verbatim:
        //   rejectBytes = 16 + totalFrameClusterCount * sizeof(VirtualVisibleCluster)
        //   allocated   = max(rejectBytes, 1024)
        const auto allocatedBytes = [](u32 frameClusterCount) -> u32
        {
            const u32 rejectBytes = 16u + frameClusterCount * static_cast<u32>(sizeof(VirtualVisibleCluster));
            return std::max(rejectBytes, 1024u);
        };
        // Records that fit AFTER the header — what the shader may index.
        const auto recordCapacity = [&allocatedBytes](u32 frameClusterCount) -> u32
        {
            return (allocatedBytes(frameClusterCount) - 16u) / static_cast<u32>(sizeof(VirtualVisibleCluster));
        };

        // 0 and the small counts exercise the 1024-byte floor; the larger ones the
        // exact-fit path where a missing header byte-count would bite.
        for (const u32 frameClusterCount : { 0u, 1u, 16u, 63u, 64u, 65u, 4860u, 100000u })
        {
            // One phase-1 thread per cluster => this is the most rejects possible.
            const u32 worstCaseRejects = frameClusterCount;
            // VirtualGeometryPass passes exactly this as u_RejectCapacity.
            const u32 shaderCapacityBound = frameClusterCount;

            EXPECT_GE(recordCapacity(frameClusterCount), shaderCapacityBound)
                << "the reject buffer allocated for " << frameClusterCount
                << " clusters holds only " << recordCapacity(frameClusterCount)
                << " records after its 16-byte header, but the shader is told it may write "
                << shaderCapacityBound << " — the sizing expression is missing the header";
            EXPECT_LE(worstCaseRejects, recordCapacity(frameClusterCount))
                << "phase 1 can append " << worstCaseRejects << " rejects at " << frameClusterCount
                << " clusters, past the allocation's " << recordCapacity(frameClusterCount) << " records";
        }
    }
} // namespace OloEngine::Tests
