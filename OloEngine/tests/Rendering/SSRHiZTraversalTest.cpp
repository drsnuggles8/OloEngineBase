#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// =============================================================================
// Screen-Space Reflections — hierarchical-Z (HiZ) traversal contract tests
// (GitHub issue #284).
//
// These pin the min-depth HZB traversal added to PostProcess_SSR.glsl WITHOUT a
// GL context, so they run in headless CI (companion to ScreenSpaceReflectionMath
// Test, which pins the per-pixel math). They build a synthetic perspective depth
// buffer (a reflective floor + a floating box), a CPU min-depth pyramid from it,
// and faithful CPU mirrors of BOTH marchers:
//   * a fixed-stride linear march (the finest-level fallback / reference), and
//   * the HiZ march (coarse-cell skipping against the min pyramid, dropping to
//     the linear test + binary refine near a surface).
//
// The contract is twofold, matching the acceptance criteria of #284:
//   1. EQUIVALENCE — HiZ finds the same hit as the linear march (a HiZ skip can
//      never invent or drop a hit; the actual hit test is always against full-
//      res scene depth, the pyramid only skips provably-empty space).
//   2. ACCELERATION — HiZ reaches that hit with far fewer scene-depth fetches by
//      taking large steps through empty space.
//
// Plus the min-pyramid's defining property (each cell holds its block's nearest
// surface) and the conservativeness of the skip predicate.
//
// Classification: shaderpipe (CPU mirror of GLSL traversal; no GL).
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    constexpr float kSkyDepth = 0.999999f; // matches SKY_DEPTH in PostProcess_SSR.glsl

    // ---- GLSL helper mirrors (must match PostProcess_SSR.glsl) --------------

    glm::vec3 ViewPosFromDepth(glm::vec2 uv, float depth, const glm::mat4& invProj)
    {
        const glm::vec4 ndc(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, depth * 2.0f - 1.0f, 1.0f);
        const glm::vec4 view = invProj * ndc;
        return glm::vec3(view) / view.w;
    }

    glm::vec2 ProjectToUV(glm::vec3 viewPos, const glm::mat4& proj)
    {
        const glm::vec4 clip = proj * glm::vec4(viewPos, 1.0f);
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        return ndc * 0.5f + 0.5f;
    }

    float DeviceZFromViewPos(glm::vec3 viewPos, const glm::mat4& proj)
    {
        const glm::vec4 clip = proj * glm::vec4(viewPos, 1.0f);
        return (clip.z / clip.w) * 0.5f + 0.5f;
    }

    // ---- Synthetic scene: a reflective floor + a floating box --------------
    //
    // Built once into a discrete device-Z depth buffer (exactly like a real
    // depth attachment), so the min-pyramid and the marcher's fine test read the
    // same texels and the skip is conservative by construction.

    struct Scene
    {
        static constexpr u32 W = 128;
        static constexpr u32 H = 128;

        glm::mat4 Proj{ 1.0f };
        glm::mat4 InvProj{ 1.0f };
        std::vector<float> Depth; // W*H device-Z, 1.0 = sky

        // Min-depth pyramid (power-of-2, full mip chain), mirroring HZBGenerator.
        u32 HzbW = 0, HzbH = 0;
        glm::vec2 UVFactor{ 1.0f };
        std::vector<std::vector<float>> MinMips; // MinMips[lod][y*mipW + x]
        std::vector<u32> MipW, MipH;

        // Nearest surface (view-space) along the eye ray through `uv`, or sky.
        // Floor: plane y = -1.5; Box: AABB centred (0,-0.3,-7), half (1.5,0.8,1).
        float SurfaceDeviceZ(glm::vec2 uv) const
        {
            const glm::vec3 dir = glm::normalize(ViewPosFromDepth(uv, 0.5f, InvProj));

            float bestT = std::numeric_limits<float>::max();

            // Floor plane y = floorY.
            constexpr float floorY = -1.5f;
            if (dir.y < -1e-4f)
            {
                const float t = floorY / dir.y;
                if (t > 0.0f)
                    bestT = std::min(bestT, t);
            }

            // Box (slab method), centred at C with half-extents He.
            const glm::vec3 C(0.0f, -0.3f, -7.0f);
            const glm::vec3 He(1.5f, 0.8f, 1.0f);
            const glm::vec3 bmin = C - He;
            const glm::vec3 bmax = C + He;
            float tmin = 0.0f;
            float tmax = std::numeric_limits<float>::max();
            bool boxHit = true;
            for (int a = 0; a < 3; ++a)
            {
                if (std::abs(dir[a]) < 1e-8f)
                {
                    if (0.0f < bmin[a] || 0.0f > bmax[a]) // ray origin outside slab
                    {
                        boxHit = false;
                        break;
                    }
                }
                else
                {
                    float t1 = bmin[a] / dir[a];
                    float t2 = bmax[a] / dir[a];
                    if (t1 > t2)
                        std::swap(t1, t2);
                    tmin = std::max(tmin, t1);
                    tmax = std::min(tmax, t2);
                    if (tmin > tmax)
                    {
                        boxHit = false;
                        break;
                    }
                }
            }
            if (boxHit && tmin > 0.0f)
                bestT = std::min(bestT, tmin);

            if (bestT >= std::numeric_limits<float>::max())
                return 1.0f; // sky

            const glm::vec3 p = dir * bestT;
            if (p.z >= 0.0f)
                return 1.0f; // behind the eye → treat as sky
            return DeviceZFromViewPos(p, Proj);
        }

        // Nearest-texel scene-depth fetch (the "depth texture").
        float SampleSceneDepth(glm::vec2 uv) const
        {
            const i32 x = std::clamp(static_cast<i32>(uv.x * static_cast<float>(W)), 0, static_cast<i32>(W) - 1);
            const i32 y = std::clamp(static_cast<i32>(uv.y * static_cast<float>(H)), 0, static_cast<i32>(H) - 1);
            return Depth[static_cast<sizet>(y) * W + x];
        }

        // Min-HZB fetch (mirrors SampleMinHZB in PostProcess_SSR.glsl).
        float SampleMinHZB(glm::vec2 uv, int lod) const
        {
            const glm::vec2 hzbUV = glm::clamp(uv, glm::vec2(0.0f), glm::vec2(1.0f)) * UVFactor;
            const u32 mw = MipW[static_cast<sizet>(lod)];
            const u32 mh = MipH[static_cast<sizet>(lod)];
            const i32 cx = std::clamp(static_cast<i32>(hzbUV.x * static_cast<float>(mw)), 0, static_cast<i32>(mw) - 1);
            const i32 cy = std::clamp(static_cast<i32>(hzbUV.y * static_cast<float>(mh)), 0, static_cast<i32>(mh) - 1);
            return MinMips[static_cast<sizet>(lod)][static_cast<sizet>(cy) * mw + cx];
        }

        [[nodiscard]] int MipCount() const
        {
            return static_cast<int>(MinMips.size());
        }
    };

    u32 NextPow2(u32 v)
    {
        u32 r = 1u;
        while (r < v)
            r <<= 1u;
        return r;
    }

    Scene BuildScene()
    {
        Scene s;
        s.Proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
        s.InvProj = glm::inverse(s.Proj);

        // Fill the discrete depth buffer at texel centres.
        s.Depth.resize(static_cast<sizet>(Scene::W) * Scene::H);
        for (u32 y = 0; y < Scene::H; ++y)
        {
            for (u32 x = 0; x < Scene::W; ++x)
            {
                const glm::vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(Scene::W),
                                   (static_cast<float>(y) + 0.5f) / static_cast<float>(Scene::H));
                s.Depth[static_cast<sizet>(y) * Scene::W + x] = s.SurfaceDeviceZ(uv);
            }
        }

        // Build the power-of-2 min pyramid from the depth buffer (mip0 = scene
        // depth in the valid sub-rect, edge-clamped in the pow2 padding; coarser
        // mips min-reduce 2x2 — mirrors HZB.comp with u_ReduceOp = min).
        s.HzbW = NextPow2(Scene::W);
        s.HzbH = NextPow2(Scene::H);
        s.UVFactor = glm::vec2(static_cast<float>(Scene::W) / static_cast<float>(s.HzbW),
                               static_cast<float>(Scene::H) / static_cast<float>(s.HzbH));

        std::vector<float> mip0(static_cast<sizet>(s.HzbW) * s.HzbH);
        for (u32 y = 0; y < s.HzbH; ++y)
        {
            const u32 sy = std::min(y, Scene::H - 1u);
            for (u32 x = 0; x < s.HzbW; ++x)
            {
                const u32 sx = std::min(x, Scene::W - 1u);
                mip0[static_cast<sizet>(y) * s.HzbW + x] = s.Depth[static_cast<sizet>(sy) * Scene::W + sx];
            }
        }
        s.MinMips.push_back(std::move(mip0));
        s.MipW.push_back(s.HzbW);
        s.MipH.push_back(s.HzbH);

        u32 mw = s.HzbW, mh = s.HzbH;
        while (mw > 1u || mh > 1u)
        {
            const u32 pw = mw, ph = mh;
            mw = std::max(1u, mw / 2u);
            mh = std::max(1u, mh / 2u);
            const auto& parent = s.MinMips.back();
            std::vector<float> mip(static_cast<sizet>(mw) * mh);
            for (u32 y = 0; y < mh; ++y)
            {
                for (u32 x = 0; x < mw; ++x)
                {
                    float m = std::numeric_limits<float>::max();
                    for (u32 dy = 0; dy < 2u; ++dy)
                    {
                        for (u32 dx = 0; dx < 2u; ++dx)
                        {
                            const u32 px = std::min(2u * x + dx, pw - 1u);
                            const u32 py = std::min(2u * y + dy, ph - 1u);
                            m = std::min(m, parent[static_cast<sizet>(py) * pw + px]);
                        }
                    }
                    mip[static_cast<sizet>(y) * mw + x] = m;
                }
            }
            s.MinMips.push_back(std::move(mip));
            s.MipW.push_back(mw);
            s.MipH.push_back(mh);
        }
        return s;
    }

    struct MarchResult
    {
        bool Hit = false;
        glm::vec2 HitUV{ 0.0f };
        int SceneFetches = 0; // full-res scene-depth lookups
        int Steps = 0;        // march iterations — the "large steps through empty space" metric
    };

    constexpr float kStride = 0.15f;
    constexpr float kMaxDist = 40.0f;
    constexpr float kThickness = 0.6f;
    constexpr int kMaxSteps = 240;
    constexpr int kBinSteps = 8;

    glm::vec2 RefineCrossing(glm::vec3 lo, glm::vec3 hi, int steps, const Scene& s, int& fetches)
    {
        for (int b = 0; b < steps; ++b)
        {
            const glm::vec3 mid = (lo + hi) * 0.5f;
            const glm::vec2 muv = ProjectToUV(mid, s.Proj);
            const float md = s.SampleSceneDepth(muv);
            ++fetches;
            const glm::vec3 mpos = ViewPosFromDepth(muv, md, s.InvProj);
            const float mdl = (-mid.z) - (-mpos.z);
            if (mdl > 0.0f)
                hi = mid;
            else
                lo = mid;
        }
        return ProjectToUV(hi, s.Proj);
    }

    // Reference: plain fixed-stride linear march + binary refine (the finest-
    // level fallback). No HZB.
    MarchResult LinearMarch(glm::vec3 rayPos, glm::vec3 R, const Scene& s)
    {
        MarchResult res;
        glm::vec3 prevPos = rayPos;
        float traveled = 0.0f;
        for (int i = 0; i < kMaxSteps; ++i)
        {
            ++res.Steps;
            prevPos = rayPos;
            rayPos += R * kStride;
            traveled += kStride;
            if (traveled > kMaxDist)
                break;
            if (rayPos.z >= 0.0f)
                break;
            const glm::vec2 uv = ProjectToUV(rayPos, s.Proj);
            if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
                break;
            const float sDepth = s.SampleSceneDepth(uv);
            ++res.SceneFetches;
            if (sDepth >= kSkyDepth)
                continue;
            const glm::vec3 sPos = ViewPosFromDepth(uv, sDepth, s.InvProj);
            const float deltaL = (-rayPos.z) - (-sPos.z);
            if (deltaL > 0.0f && deltaL < kThickness)
            {
                res.HitUV = RefineCrossing(prevPos, rayPos, kBinSteps, s, res.SceneFetches);
                res.Hit = true;
                break;
            }
        }
        return res;
    }

    // Screen-space hierarchical-Z march — a faithful mirror of the DDA in
    // PostProcess_SSR.glsl, parameterised by the HZB-level cap so the test can
    // run it with full skipping (HiZ) and with skipping disabled (cap = 0, the
    // 1px linear fallback) and compare the two.
    MarchResult DDAMarch(glm::vec3 vStart, glm::vec3 R, const Scene& s, int maxLevelCap)
    {
        MarchResult res;
        const int maxLevel = std::clamp(std::min(s.MipCount() - 1, maxLevelCap), 0, 16);

        float segLen = kMaxDist;
        if (R.z > 1e-6f)
            segLen = std::min(segLen, (-1e-3f - vStart.z) / R.z);
        const glm::vec3 vEnd = vStart + R * segLen;

        const glm::vec4 hStart = s.Proj * glm::vec4(vStart, 1.0f);
        const glm::vec4 hEnd = s.Proj * glm::vec4(vEnd, 1.0f);
        if (!(hStart.w > 0.0f && hEnd.w > 0.0f))
            return res;

        const glm::vec2 uvStart = glm::vec2(hStart) / hStart.w * 0.5f + 0.5f;
        const glm::vec2 uvEnd = glm::vec2(hEnd) / hEnd.w * 0.5f + 0.5f;
        const glm::vec2 screen(static_cast<float>(Scene::W), static_cast<float>(Scene::H));
        const float pixLen = glm::length((uvEnd - uvStart) * screen);
        if (pixLen < 1.0f)
            return res;

        const float tInc = 1.0f / pixLen;
        int level = 0;
        float t = 0.0f;
        float prevT = 0.0f;

        for (int i = 0; i < kMaxSteps; ++i)
        {
            if (t >= 1.0f)
                break;
            ++res.Steps;
            const glm::vec4 clip = glm::mix(hStart, hEnd, t);
            const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
            if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
                break;

            // Cell step sized in LOCAL screen pixels (perspective-correct), so a
            // skip is exactly one cell wide and never over-skips a surface; level
            // 0 keeps the global view-uniform increment (mirror of the shader).
            const glm::vec4 clipProbe = glm::mix(hStart, hEnd, std::min(t + tInc, 1.0f));
            const glm::vec2 uvProbe = glm::vec2(clipProbe) / clipProbe.w * 0.5f + 0.5f;
            const float localPixPerT = std::max(glm::length((uvProbe - uv) * screen) / tInc, 1e-3f);
            const float cellStepT = (level == 0) ? tInc : (std::exp2(static_cast<float>(level)) / localPixPerT);
            const float tExit = std::min(t + cellStepT, 1.0f);
            const glm::vec4 clipExit = glm::mix(hStart, hEnd, tExit);
            const float rayDZ = (clip.z / clip.w) * 0.5f + 0.5f;
            const float rayDZExit = (clipExit.z / clipExit.w) * 0.5f + 0.5f;
            const float rayDZFar = std::max(rayDZ, rayDZExit);

            const float cellMin = s.SampleMinHZB(uv, level);
            if (cellMin >= kSkyDepth || rayDZFar < cellMin)
            {
                // Empty cell across its whole far edge — jump a cell, climb a mip.
                prevT = t;
                t = tExit;
                level = std::min(level + 1, maxLevel);
            }
            else if (level > 0)
            {
                level -= 1; // descend toward the surface
            }
            else
            {
                const float sDepth = s.SampleSceneDepth(uv);
                ++res.SceneFetches;
                if (sDepth < kSkyDepth)
                {
                    const glm::vec3 rayPos = glm::mix(vStart, vEnd, t);
                    const glm::vec3 sPos = ViewPosFromDepth(uv, sDepth, s.InvProj);
                    const float deltaL = (-rayPos.z) - (-sPos.z);
                    if (deltaL > 0.0f && deltaL < kThickness)
                    {
                        res.HitUV = RefineCrossing(glm::mix(vStart, vEnd, prevT), rayPos, kBinSteps, s, res.SceneFetches);
                        res.Hit = true;
                        break;
                    }
                }
                prevT = t;
                t += tInc;
            }
        }
        return res;
    }

    // Reflection rays off the floor, reconstructed exactly as the SSR shader
    // does: P from depth, N = floor normal, V = eye→fragment, R = reflect(V,N).
    struct Ray
    {
        glm::vec3 Start;
        glm::vec3 Dir;
    };

    std::vector<Ray> FloorReflectionRays(const Scene& s)
    {
        std::vector<Ray> rays;
        const glm::vec3 N(0.0f, 1.0f, 0.0f); // floor normal (view space)
        for (int gy = 0; gy < 18; ++gy)
        {
            for (int gx = 0; gx < 18; ++gx)
            {
                const glm::vec2 uv(0.18f + 0.64f * (static_cast<float>(gx) / 17.0f),
                                   0.04f + 0.40f * (static_cast<float>(gy) / 17.0f)); // lower screen = floor
                const float d = s.SampleSceneDepth(uv);
                if (d >= kSkyDepth)
                    continue;
                const glm::vec3 P = ViewPosFromDepth(uv, d, s.InvProj);
                // Only start from floor surfaces (roughly y ≈ -1.5), not the box.
                if (P.y > -1.3f)
                    continue;
                const glm::vec3 V = glm::normalize(P);
                const glm::vec3 R = glm::normalize(glm::reflect(V, N));
                const glm::vec3 start = P + N * (0.02f * -P.z); // matches shader bias
                rays.push_back({ start, R });
            }
        }
        return rays;
    }
} // namespace

// ---- Min-pyramid defining property ------------------------------------------

TEST(SSRHiZTraversal, MinPyramidStoresNearestSurfaceOfEachBlock)
{
    const Scene s = BuildScene();
    ASSERT_GE(s.MipCount(), 2);

    // For a sampling of coarse cells, the stored value must equal the minimum
    // (nearest) of the mip-0 texels the cell covers — the front-to-back
    // contract that makes HiZ skipping valid.
    for (int lod = 1; lod < s.MipCount(); ++lod)
    {
        const u32 mw = s.MipW[static_cast<sizet>(lod)];
        const u32 mh = s.MipH[static_cast<sizet>(lod)];
        const u32 scale = 1u << lod;
        for (u32 cy = 0; cy < mh; cy += std::max(1u, mh / 8u))
        {
            for (u32 cx = 0; cx < mw; cx += std::max(1u, mw / 8u))
            {
                float expected = std::numeric_limits<float>::max();
                for (u32 y = cy * scale; y < std::min((cy + 1u) * scale, s.HzbH); ++y)
                    for (u32 x = cx * scale; x < std::min((cx + 1u) * scale, s.HzbW); ++x)
                        expected = std::min(expected, s.MinMips[0][static_cast<sizet>(y) * s.HzbW + x]);

                const float stored = s.MinMips[static_cast<sizet>(lod)][static_cast<sizet>(cy) * mw + cx];
                EXPECT_NEAR(stored, expected, 1e-6f)
                    << "min pyramid lod " << lod << " cell (" << cx << "," << cy << ")";
            }
        }
    }
}

// ---- Skip predicate is conservative -----------------------------------------

TEST(SSRHiZTraversal, SkipPredicateNeverHidesACloserSurface)
{
    const Scene s = BuildScene();

    // The skip rule is: if rayDeviceZ < cellMin, the whole cell is empty. Verify
    // the contrapositive holds for the buffer: whenever a mip-0 texel in a cell
    // is nearer than the cell's stored min would allow, the stored min reflects
    // it. Equivalently, no covered texel is strictly nearer (smaller) than the
    // cell minimum — i.e. the stored min is a true lower bound on device-Z.
    for (int lod = 1; lod < s.MipCount(); ++lod)
    {
        const u32 mw = s.MipW[static_cast<sizet>(lod)];
        const u32 mh = s.MipH[static_cast<sizet>(lod)];
        const u32 scale = 1u << lod;
        for (u32 cy = 0; cy < mh; ++cy)
        {
            for (u32 cx = 0; cx < mw; ++cx)
            {
                const float stored = s.MinMips[static_cast<sizet>(lod)][static_cast<sizet>(cy) * mw + cx];
                for (u32 y = cy * scale; y < std::min((cy + 1u) * scale, s.HzbH); ++y)
                    for (u32 x = cx * scale; x < std::min((cx + 1u) * scale, s.HzbW); ++x)
                        ASSERT_GE(s.MinMips[0][static_cast<sizet>(y) * s.HzbW + x], stored - 1e-6f)
                            << "cell min is not a lower bound at lod " << lod;
            }
        }
    }
}

// ---- Equivalence: HiZ skipping doesn't change the result vs the 1px march ----

TEST(SSRHiZTraversal, HiZSkippingMatchesNoSkipMarch)
{
    const Scene s = BuildScene();
    const auto rays = FloorReflectionRays(s);
    ASSERT_GE(rays.size(), 40u) << "test scene produced too few floor reflection rays";

    u32 agree = 0;
    u32 bothHit = 0;
    for (const Ray& r : rays)
    {
        const MarchResult noSkip = DDAMarch(r.Start, r.Dir, s, 0); // exhaustive 1px march
        const MarchResult hiz = DDAMarch(r.Start, r.Dir, s, 16);   // full HiZ skipping

        if (hiz.Hit == noSkip.Hit)
            ++agree;
        if (hiz.Hit && noSkip.Hit)
        {
            ++bothHit;
            // The skip only jumps provably-empty cells, so it localises the same
            // crossing the exhaustive march finds.
            EXPECT_NEAR(hiz.HitUV.x, noSkip.HitUV.x, 0.02f) << "hitUV.x drift";
            EXPECT_NEAR(hiz.HitUV.y, noSkip.HitUV.y, 0.02f) << "hitUV.y drift";
        }
    }

    // The scene is designed so a meaningful share of floor rays reflect the box.
    EXPECT_GT(bothHit, 5u) << "no reflections of the box were found — test scene is not exercising hits";
    // HiZ skipping must not change hit-vs-miss relative to testing every pixel:
    // the min pyramid only ever skips cells the ray is provably in front of. A
    // couple of float-boundary disagreements at silhouettes are tolerated.
    EXPECT_GE(agree * 100u, rays.size() * 98u)
        << "HiZ skipping changed hit-vs-miss on " << (rays.size() - agree) << " of " << rays.size() << " rays";
}

// ---- Acceleration: HiZ takes large steps through empty space -----------------

TEST(SSRHiZTraversal, HiZTakesLargeStepsThroughEmptySpace)
{
    const Scene s = BuildScene();
    const auto rays = FloorReflectionRays(s);
    ASSERT_GE(rays.size(), 40u);

    u64 noSkipSteps = 0; // exhaustive 1px-per-step march
    u64 hizSteps = 0;    // coarse-cell skipping
    u64 hizSceneFetches = 0;
    u64 linearSceneFetches = 0; // independent fixed-stride march fetches every step
    for (const Ray& r : rays)
    {
        const MarchResult noSkip = DDAMarch(r.Start, r.Dir, s, 0);
        const MarchResult hiz = DDAMarch(r.Start, r.Dir, s, 16);
        const MarchResult linear = LinearMarch(r.Start, r.Dir, s);
        noSkipSteps += static_cast<u64>(noSkip.Steps);
        hizSteps += static_cast<u64>(hiz.Steps);
        hizSceneFetches += static_cast<u64>(hiz.SceneFetches);
        linearSceneFetches += static_cast<u64>(linear.SceneFetches);
    }

    // The defining acceptance criterion of #284: rays jump across empty space at
    // coarse mips instead of crawling pixel by pixel, so HiZ takes far fewer
    // march iterations than the exhaustive 1px march.
    EXPECT_LT(hizSteps, noSkipSteps / 2u)
        << "HiZ did not at least halve march iterations (hiz=" << hizSteps
        << " exhaustive=" << noSkipSteps << ")";

    // And against a naive fixed-stride linear march that samples full-res depth
    // every step, HiZ slashes the expensive scene-depth fetches.
    EXPECT_LT(hizSceneFetches, linearSceneFetches / 2u)
        << "HiZ did not at least halve scene-depth fetches vs the linear march (hiz="
        << hizSceneFetches << " linear=" << linearSceneFetches << ")";
}

// ---- Cross-check the marcher against an independent linear march -------------

TEST(SSRHiZTraversal, NoSkipDDACrossChecksIndependentLinearMarch)
{
    const Scene s = BuildScene();
    const auto rays = FloorReflectionRays(s);
    ASSERT_GE(rays.size(), 40u);

    // The DDA with skipping disabled is a screen-space 1px linear march. Cross-
    // check it against a wholly independent fixed-stride VIEW-space linear march
    // (a different step strategy entirely). They localise the same crossings, so
    // hit-vs-miss must agree for the vast majority (silhouette pixels excepted)
    // and a shared hit must land on the same surface.
    u32 agree = 0;
    u32 bothHit = 0;
    for (const Ray& r : rays)
    {
        const MarchResult ind = LinearMarch(r.Start, r.Dir, s);
        const MarchResult dda0 = DDAMarch(r.Start, r.Dir, s, 0);
        if (ind.Hit == dda0.Hit)
            ++agree;
        if (ind.Hit && dda0.Hit)
        {
            ++bothHit;
            EXPECT_NEAR(ind.HitUV.x, dda0.HitUV.x, 0.03f);
            EXPECT_NEAR(ind.HitUV.y, dda0.HitUV.y, 0.03f);
        }
    }

    EXPECT_GT(bothHit, 5u);
    EXPECT_GE(agree * 100u, rays.size() * 85u)
        << "the no-skip DDA and the independent linear march disagreed on "
        << (rays.size() - agree) << " of " << rays.size() << " rays";
}
