// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GPUOcclusionCullParityTest.cpp
//
// Pins the Hi-Z occlusion math in
// `assets/shaders/compute/InstanceOcclusionCull.comp` (#431). That shader runs
// the *same* six-plane frustum test as InstanceFrustumCull.comp (already pinned
// by GPUFrustumCullParityTest) and then adds a depth-pyramid occlusion reject:
// an instance whose screen-space bounding rectangle is entirely behind the
// nearest recorded occluder is dropped before the indirect draw.
//
// There is no engine-side CPU occlusion path to compare against (unlike the
// frustum parity test), so this re-implements the shader's `isOccluded()`
// helper byte-for-byte in C++ and asserts the *decisions* it must make on
// hand-built occluder / occludee scenarios. The load-bearing invariants:
//
//   * an instance fully behind a nearer occluder, on-screen, is culled;
//   * an instance in front of the occluder always survives (it is visible);
//   * every ambiguous case (off-screen rectangle, behind camera, no occluder /
//     far-plane depth) survives — a false cull punches a visible hole, so the
//     test pins the conservative bias toward keeping the instance;
//   * occlusion produces a *measurable* survivor reduction vs frustum-only.
//
// Conventions match the shader: OpenGL device-Z in [0,1] (smaller = nearer),
// HZB is a MAX-reduction pyramid (coarse texel = farthest nearest-surface
// depth), bounds reprojected with the previous frame's view-projection.
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
        // Inputs mirroring the occlusion uniforms of InstanceOcclusionCull.comp.
        struct OcclusionParams
        {
            glm::mat4 PrevViewProjection{ 1.0f };
            glm::vec2 HZBSize{ 1024.0f, 1024.0f }; // power-of-2 texels
            glm::vec2 HZBUVFactor{ 1.0f };         // viewport / HZB size
            i32 MipCount = 11;
            f32 DepthBias = 0.0f;
        };

        // A synthetic HZB sampler: given an HZB-space UV and a mip, return the
        // device-Z occluder depth. Re-implementations below treat it exactly as
        // `textureLod(u_HZB, uv, mip).r` does in the shader.
        using HZBSampler = std::function<f32(glm::vec2 hzbUV, f32 mip)>;

        // Byte-for-byte re-implementation of InstanceOcclusionCull.comp's
        // isOccluded(). Returns true when it is safe to cull the sphere.
        bool IsOccluded_GLSLEquivalent(glm::vec3 worldCenter, f32 worldRadius,
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
                const glm::vec4 clip = p.PrevViewProjection * glm::vec4(corner, 1.0f);
                if (clip.w <= 0.0f)
                    return false; // straddles / behind camera.
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

            const glm::vec2 hzbMin = rectMin * p.HZBUVFactor;
            const glm::vec2 hzbMax = rectMax * p.HZBUVFactor;

            const glm::vec2 sizeTexels = (hzbMax - hzbMin) * p.HZBSize;
            const f32 maxSide = glm::max(sizeTexels.x, sizeTexels.y);
            f32 mip = glm::ceil(glm::log2(glm::max(maxSide, 1.0f)));
            mip = glm::clamp(mip, 0.0f, static_cast<f32>(p.MipCount - 1));

            f32 occluderZ = sampleHZB(glm::vec2(hzbMin.x, hzbMin.y), mip);
            occluderZ = glm::max(occluderZ, sampleHZB(glm::vec2(hzbMax.x, hzbMin.y), mip));
            occluderZ = glm::max(occluderZ, sampleHZB(glm::vec2(hzbMin.x, hzbMax.y), mip));
            occluderZ = glm::max(occluderZ, sampleHZB(glm::vec2(hzbMax.x, hzbMax.y), mip));

            return nearestZ > occluderZ + p.DepthBias;
        }

        // Standard editor-ish camera, near 0.1 / far 100.
        glm::mat4 MakeViewProjection(glm::vec3 eye = glm::vec3(0.0f, 0.0f, 0.0f))
        {
            const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
            const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            return projection * view;
        }

        // Device-Z of a point straight ahead at view-space distance `dist`
        // through the same projection — used to place occluder walls and probe
        // instances at known relative depths.
        f32 DeviceZForDistance(f32 dist)
        {
            const glm::mat4 vp = MakeViewProjection();
            const glm::vec4 clip = vp * glm::vec4(0.0f, 0.0f, -dist, 1.0f);
            return (clip.z / clip.w) * 0.5f + 0.5f;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // A full-screen occluder wall at a fixed device-Z. Anything entirely behind
    // it (and fully on-screen) must be culled; anything in front must survive.
    // -------------------------------------------------------------------------
    TEST(GPUOcclusionCullParity, BehindWallCulledInFrontSurvives)
    {
        OcclusionParams p;
        p.PrevViewProjection = MakeViewProjection();

        const f32 wallZ = DeviceZForDistance(10.0f); // occluder 10 units ahead
        const HZBSampler flatWall = [wallZ](glm::vec2, f32)
        { return wallZ; };

        // Small sphere directly ahead, well behind the wall (25 units): occluded.
        {
            const glm::vec3 center{ 0.0f, 0.0f, -25.0f };
            EXPECT_TRUE(IsOccluded_GLSLEquivalent(center, 0.5f, p, flatWall))
                << "A small sphere fully behind the occluder wall must be culled.";
        }

        // Same lateral position but in FRONT of the wall (5 units): visible.
        {
            const glm::vec3 center{ 0.0f, 0.0f, -5.0f };
            EXPECT_FALSE(IsOccluded_GLSLEquivalent(center, 0.5f, p, flatWall))
                << "A sphere in front of the occluder must never be culled.";
        }

        // Straddling the wall (radius spans it): nearest point is in front, so
        // it must survive — culling it would clip a visible object.
        {
            const glm::vec3 center{ 0.0f, 0.0f, -10.0f };
            EXPECT_FALSE(IsOccluded_GLSLEquivalent(center, 3.0f, p, flatWall))
                << "A sphere straddling the occluder must survive (nearest point visible).";
        }
    }

    // -------------------------------------------------------------------------
    // Empty depth (far plane = 1.0 everywhere): nothing can occlude, so every
    // instance survives regardless of distance.
    // -------------------------------------------------------------------------
    TEST(GPUOcclusionCullParity, NoOccluderNeverCulls)
    {
        OcclusionParams p;
        p.PrevViewProjection = MakeViewProjection();
        const HZBSampler emptyDepth = [](glm::vec2, f32)
        { return 1.0f; };

        for (f32 dist : { 2.0f, 10.0f, 50.0f, 95.0f })
        {
            const glm::vec3 center{ 0.0f, 0.0f, -dist };
            EXPECT_FALSE(IsOccluded_GLSLEquivalent(center, 0.5f, p, emptyDepth))
                << "Far-plane (empty) depth must never occlude; dist=" << dist;
        }
    }

    // -------------------------------------------------------------------------
    // Conservative safety: an instance whose reprojected rectangle leaves the
    // screen, or sits behind the camera, must survive even when "behind" a wall.
    // -------------------------------------------------------------------------
    TEST(GPUOcclusionCullParity, OffScreenAndBehindCameraSurvive)
    {
        OcclusionParams p;
        p.PrevViewProjection = MakeViewProjection();
        const f32 wallZ = DeviceZForDistance(10.0f);
        const HZBSampler flatWall = [wallZ](glm::vec2, f32)
        { return wallZ; };

        // Far off to the side: rectangle exits the screen -> keep.
        {
            const glm::vec3 center{ 40.0f, 0.0f, -25.0f };
            EXPECT_FALSE(IsOccluded_GLSLEquivalent(center, 0.5f, p, flatWall))
                << "A partially/fully off-screen rectangle must not be culled.";
        }

        // Behind the camera (positive Z in view space): some corner has w<=0.
        {
            const glm::vec3 center{ 0.0f, 0.0f, 5.0f };
            EXPECT_FALSE(IsOccluded_GLSLEquivalent(center, 1.0f, p, flatWall))
                << "A sphere behind the camera must not be culled.";
        }
    }

    // -------------------------------------------------------------------------
    // A positive depth bias must make culling strictly harder (more
    // conservative): a sphere that is occluded at bias 0 can be kept once the
    // bias exceeds its margin behind the wall.
    // -------------------------------------------------------------------------
    TEST(GPUOcclusionCullParity, DepthBiasMakesCullingMoreConservative)
    {
        OcclusionParams p;
        p.PrevViewProjection = MakeViewProjection();

        const f32 wallZ = DeviceZForDistance(10.0f);
        const HZBSampler flatWall = [wallZ](glm::vec2, f32)
        { return wallZ; };

        const glm::vec3 center{ 0.0f, 0.0f, -12.0f };                // just behind the wall
        const f32 nearestMargin = DeviceZForDistance(11.5f) - wallZ; // rough slack past wall

        p.DepthBias = 0.0f;
        const bool culledNoBias = IsOccluded_GLSLEquivalent(center, 0.5f, p, flatWall);
        EXPECT_TRUE(culledNoBias) << "Sphere just behind the wall should cull at zero bias.";

        // A bias larger than the device-Z margin keeps it.
        p.DepthBias = glm::max(nearestMargin, 0.0f) + 0.01f;
        const bool culledBigBias = IsOccluded_GLSLEquivalent(center, 0.5f, p, flatWall);
        EXPECT_FALSE(culledBigBias) << "A large depth bias must spare a borderline instance.";
    }

    // -------------------------------------------------------------------------
    // End-to-end survivor reduction: a field of instances split between "behind
    // a wall" and "in front" must, under occlusion, keep exactly the front
    // ones — a measurable draw reduction over frustum-only (which keeps all).
    // -------------------------------------------------------------------------
    TEST(GPUOcclusionCullParity, MeasurableSurvivorReduction)
    {
        OcclusionParams p;
        p.PrevViewProjection = MakeViewProjection();
        const f32 wallZ = DeviceZForDistance(15.0f);
        const HZBSampler flatWall = [wallZ](glm::vec2, f32)
        { return wallZ; };

        // 50 instances behind the wall in a tight on-screen column, 20 in front.
        constexpr u32 kBehind = 50;
        constexpr u32 kFront = 20;
        u32 survivors = 0;
        for (u32 i = 0; i < kBehind; ++i)
        {
            const f32 x = -2.0f + 4.0f * (static_cast<f32>(i) / static_cast<f32>(kBehind));
            const glm::vec3 center{ x, 0.0f, -40.0f };
            if (!IsOccluded_GLSLEquivalent(center, 0.3f, p, flatWall))
                ++survivors;
        }
        for (u32 i = 0; i < kFront; ++i)
        {
            const f32 x = -2.0f + 4.0f * (static_cast<f32>(i) / static_cast<f32>(kFront));
            const glm::vec3 center{ x, 0.0f, -6.0f };
            if (!IsOccluded_GLSLEquivalent(center, 0.3f, p, flatWall))
                ++survivors;
        }

        // Frustum-only would keep all 70; occlusion keeps only the 20 in front.
        EXPECT_EQ(survivors, kFront)
            << "Occlusion must cull all instances hidden behind the wall and keep the front ones.";
        EXPECT_LT(survivors, kBehind + kFront)
            << "Occlusion must produce a measurable reduction vs frustum-only.";
    }
} // namespace OloEngine::Tests
