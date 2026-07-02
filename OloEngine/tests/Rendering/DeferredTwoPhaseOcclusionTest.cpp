// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// DeferredTwoPhaseOcclusionTest.cpp
//
// Pins the disocclusion-correctness contract that #486 adds to the Deferred
// path: the two-phase GPU HZB occlusion cull (phase 1 vs the PREVIOUS frame's
// Hi-Z, phase 2 vs THIS frame's Hi-Z) eliminates the one-frame pop that the
// legacy single-phase deferred cull showed on fast camera moves.
//
// The occlusion math itself is the shared `InstanceOcclusionCull.comp`, already
// pinned byte-for-byte by GPUOcclusionCullParityTest. This test re-uses that
// same isOccluded() model and pins the *two-phase decision* the deferred path
// now makes:
//
//   * Phase 1 tests each instance against the previous frame's HZB (reprojected
//     with the previous transform). An instance hidden last frame is REJECTED —
//     it is NOT drawn in phase 1, it goes onto the reject list.
//   * Phase 2 re-tests ONLY the reject list against this frame's HZB (current
//     transform, no frustum test). An instance that became visible this frame
//     (disocclusion) SURVIVES phase 2 and is drawn — no pop.
//   * The single-phase scheme the deferred path used before would have dropped
//     that instance for the whole frame (the pop this feature removes).
//   * An instance that is STILL hidden this frame stays rejected in both phases
//     (correctly culled — no wasted draw).
//
// Conventions match the shader: OpenGL device-Z in [0,1] (smaller = nearer),
// HZB is a MAX-reduction pyramid, phase-1 bounds reprojected with the previous
// view-projection, phase-2 bounds with the current view-projection.
//
// Classification: shaderpipe (pure CPU, no GL context).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <functional>

namespace OloEngine::Tests
{
    namespace
    {
        struct OcclusionParams
        {
            glm::mat4 ViewProjection{ 1.0f }; // the HZB's own frame view-projection
            glm::vec2 HZBSize{ 1024.0f, 1024.0f };
            glm::vec2 HZBUVFactor{ 1.0f };
            i32 MipCount = 11;
            f32 DepthBias = 0.0f;
        };

        using HZBSampler = std::function<f32(glm::ivec2 texel, int mip)>;

        // Byte-for-byte re-implementation of InstanceOcclusionCull.comp's
        // isOccluded() — identical to GPUOcclusionCullParityTest's model. Returns
        // true when it is safe to cull the sphere against `p`'s HZB.
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
                    return false;
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                const glm::vec2 uv = glm::vec2(ndc) * 0.5f + 0.5f;
                const f32 deviceZ = ndc.z * 0.5f + 0.5f;
                rectMin = glm::min(rectMin, uv);
                rectMax = glm::max(rectMax, uv);
                nearestZ = glm::min(nearestZ, deviceZ);
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

        // Outcome of the deferred two-phase decision for one instance.
        enum class DrawStage
        {
            Phase1, // drawn in phase 1 (visible last frame)
            Phase2, // rejected in phase 1, recovered in phase 2 (disocclusion)
            Culled, // rejected in phase 1 AND phase 2 (still hidden)
        };

        // Models Renderer3D::SubmitGPUCulledInstanced's deferred two-phase route:
        //   phase 1: test vs prevParams (previous frame HZB). Occluded -> reject.
        //   phase 2: re-test the reject list vs currParams (this frame HZB).
        DrawStage ClassifyTwoPhase(glm::vec3 center, f32 radius,
                                   const OcclusionParams& prevParams, const HZBSampler& prevHZB,
                                   const OcclusionParams& currParams, const HZBSampler& currHZB)
        {
            if (!IsOccluded(center, radius, prevParams, prevHZB))
                return DrawStage::Phase1; // survived phase 1 -> drawn now
            // Rejected in phase 1 -> re-test against this frame's HZB.
            return IsOccluded(center, radius, currParams, currHZB) ? DrawStage::Culled
                                                                   : DrawStage::Phase2;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The core disocclusion scenario. Last frame the camera looked at a wall that
    // hid an instance (prev HZB has the wall). This frame the camera moved so the
    // wall no longer covers it (curr HZB is empty there). The single-phase deferred
    // cull would drop the instance for one frame (a pop); two-phase recovers it in
    // phase 2.
    // -------------------------------------------------------------------------
    TEST(DeferredTwoPhaseOcclusion, DisocclusionRecoveredInPhase2)
    {
        const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };

        OcclusionParams prev;
        prev.ViewProjection = MakeViewProjection(eye);
        const f32 wallZ = DeviceZForDistance(eye, 10.0f);
        const HZBSampler prevWall = [wallZ](glm::ivec2, int)
        { return wallZ; };

        OcclusionParams curr = prev; // same camera pose for the geometry probe
        const HZBSampler currEmpty = [](glm::ivec2, int)
        { return 1.0f; }; // wall gone this frame

        const glm::vec3 center{ 0.0f, 0.0f, -25.0f }; // on-screen, was behind the wall

        // Single-phase (legacy deferred): tested only vs the previous HZB -> culled
        // this frame -> invisible -> the one-frame pop.
        EXPECT_TRUE(IsOccluded(center, 0.5f, prev, prevWall))
            << "Legacy single-phase deferred would drop the disoccluded instance (the pop #486 fixes).";

        // Two-phase deferred: phase 1 rejects it, phase 2 (vs this frame's empty
        // HZB) recovers it -> drawn this frame -> no pop.
        EXPECT_EQ(ClassifyTwoPhase(center, 0.5f, prev, prevWall, curr, currEmpty), DrawStage::Phase2)
            << "Two-phase must recover the disoccluded instance in phase 2.";
    }

    // -------------------------------------------------------------------------
    // A visible (never-occluded) instance draws in phase 1 — the two-phase route
    // must not defer geometry that was already visible last frame.
    // -------------------------------------------------------------------------
    TEST(DeferredTwoPhaseOcclusion, VisibleInstanceDrawsInPhase1)
    {
        const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };

        OcclusionParams prev;
        prev.ViewProjection = MakeViewProjection(eye);
        const f32 wallZ = DeviceZForDistance(eye, 10.0f);
        const HZBSampler prevWall = [wallZ](glm::ivec2, int)
        { return wallZ; };
        OcclusionParams curr = prev;
        const HZBSampler currWall = prevWall;

        const glm::vec3 center{ 0.0f, 0.0f, -5.0f }; // in front of the wall both frames
        EXPECT_EQ(ClassifyTwoPhase(center, 0.5f, prev, prevWall, curr, currWall), DrawStage::Phase1)
            << "An instance visible in both frames must draw in phase 1.";
    }

    // -------------------------------------------------------------------------
    // An instance hidden last frame AND this frame stays culled in both phases —
    // the recovered set must be exactly the disocclusions, not everything that was
    // rejected in phase 1 (else phase 2 would re-draw the whole occluded set).
    // -------------------------------------------------------------------------
    TEST(DeferredTwoPhaseOcclusion, PersistentlyOccludedStaysCulled)
    {
        const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };

        OcclusionParams prev;
        prev.ViewProjection = MakeViewProjection(eye);
        const f32 wallZ = DeviceZForDistance(eye, 10.0f);
        const HZBSampler wall = [wallZ](glm::ivec2, int)
        { return wallZ; };
        OcclusionParams curr = prev;

        const glm::vec3 center{ 0.0f, 0.0f, -25.0f }; // behind the wall in BOTH frames
        EXPECT_EQ(ClassifyTwoPhase(center, 0.5f, prev, wall, curr, wall), DrawStage::Culled)
            << "An instance hidden in both frames must stay culled — phase 2 recovers "
               "disocclusions only, not the entire phase-1 reject set.";
    }

    // -------------------------------------------------------------------------
    // Whole-field survivor accounting across a disocclusion: half the instances
    // become visible this frame. Two-phase must draw every one of them (phase 1 +
    // phase 2), matching the frustum-only survivor count and beating single-phase
    // (which would draw only the front half).
    // -------------------------------------------------------------------------
    TEST(DeferredTwoPhaseOcclusion, FieldRecoveryMatchesGroundTruth)
    {
        const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
        OcclusionParams prev;
        prev.ViewProjection = MakeViewProjection(eye);
        const f32 wallZ = DeviceZForDistance(eye, 15.0f);
        const HZBSampler prevWall = [wallZ](glm::ivec2, int)
        { return wallZ; };
        OcclusionParams curr = prev;
        const HZBSampler currEmpty = [](glm::ivec2, int)
        { return 1.0f; }; // disoccluded this frame

        constexpr u32 kBehind = 40; // hidden last frame, revealed this frame
        constexpr u32 kFront = 20;  // visible both frames

        u32 phase1 = 0, phase2 = 0, culled = 0;
        u32 singlePhaseDrawn = 0; // legacy: only phase-1 survivors would draw

        const auto tally = [&](glm::vec3 center)
        {
            if (!IsOccluded(center, 0.3f, prev, prevWall))
                ++singlePhaseDrawn;
            switch (ClassifyTwoPhase(center, 0.3f, prev, prevWall, curr, currEmpty))
            {
                case DrawStage::Phase1:
                    ++phase1;
                    break;
                case DrawStage::Phase2:
                    ++phase2;
                    break;
                case DrawStage::Culled:
                    ++culled;
                    break;
            }
        };

        for (u32 i = 0; i < kBehind; ++i)
        {
            const f32 x = -2.0f + 4.0f * (static_cast<f32>(i) / static_cast<f32>(kBehind));
            tally(glm::vec3{ x, 0.0f, -40.0f });
        }
        for (u32 i = 0; i < kFront; ++i)
        {
            const f32 x = -2.0f + 4.0f * (static_cast<f32>(i) / static_cast<f32>(kFront));
            tally(glm::vec3{ x, 0.0f, -6.0f });
        }

        // Two-phase draws everything on screen this frame; nothing stays culled
        // (the wall is gone this frame).
        EXPECT_EQ(phase1 + phase2, kBehind + kFront)
            << "Two-phase must draw every on-screen instance across the disocclusion.";
        EXPECT_EQ(culled, 0u) << "Nothing should remain culled once the occluder is gone.";
        EXPECT_EQ(phase2, kBehind) << "Exactly the disoccluded set is recovered in phase 2.";

        // Single-phase would have popped the whole revealed set for one frame.
        EXPECT_EQ(singlePhaseDrawn, kFront)
            << "Legacy single-phase deferred would draw only the front set — a "
            << kBehind << "-instance one-frame pop.";
        EXPECT_LT(singlePhaseDrawn, phase1 + phase2)
            << "Two-phase must draw strictly more than single-phase across the disocclusion.";
    }
} // namespace OloEngine::Tests
