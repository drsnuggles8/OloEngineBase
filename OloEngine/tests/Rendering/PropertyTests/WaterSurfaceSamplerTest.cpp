#include "OloEnginePCH.h"

// =============================================================================
// WaterSurfaceSamplerTest — L1 property tests for the CPU Gerstner sampler.
//
// `OloEngine::WaterSurface` (Renderer/WaterSurface.h) is a 1:1 CPU port of
// WaterCommon.glsl :: sumGerstnerWaves. The BuoyancySystem samples it so a
// floating body tracks the *rendered* wave surface rather than a flat plane.
// These tests pin the shared invariants of that port — flatness, plane-height
// linearity, the displacement bound (shared with the renderer's frustum-cull
// margin), temporal animation and determinism — so the CPU and GPU copies can't
// silently drift apart. See docs/agent-rules/testing-architecture.md (the same
// CPU/GPU-mirror discipline as WaterRenderingTest's ComputeMaxWaveDisplacementCpu).
// =============================================================================

#include "OloEngine/Renderer/WaterSurface.h"
#include "OloEngine/Scene/Components.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace OloEngine;

namespace
{
    // Build sampler params from a WaterComponent exactly the way
    // BuoyancySystem::MakeParams does (kept in sync deliberately).
    WaterSurface::Params ParamsFrom(const WaterComponent& wc, f32 planeHeight)
    {
        WaterSurface::Params p;
        p.m_WaveDir0 = wc.PackWaveDir0();
        p.m_WaveDir1 = wc.PackWaveDir1();
        p.m_WaveFrequency = wc.m_WaveFrequency;
        p.m_WaveAmplitude = wc.m_WaveAmplitude;
        p.m_WaveSpeed = wc.m_WaveSpeed;
        p.m_PlaneHeight = planeHeight;
        return p;
    }

    // Conservative upper bound on the vertical Gerstner displacement — a copy of
    // WaterRenderingTest's `ComputeMaxWaveDisplacementCpu`, itself a port of
    // Water.glsl :: computeMaxWaveDisplacement (the TCS frustum-cull margin). That
    // test proves this bound dominates the true per-axis amplitude sum, so the
    // sampler's |dy| must never exceed it.
    f32 MaxWaveDisplacementBound(glm::vec4 waveParams, glm::vec4 waveDir0, glm::vec4 waveDir1)
    {
        constexpr f32 kTwoPi = 6.28318530f;
        const f32 freq = std::max(waveParams.w, 0.01f);
        const f32 amp = waveParams.z;
        const f32 wl0 = std::max(waveDir0.w, 0.1f) / freq;
        const f32 wl1 = std::max(waveDir1.w, 0.1f) / freq;
        const f32 a0 = waveDir0.z * wl0 / kTwoPi;
        const f32 a1 = waveDir1.z * wl1 / kTwoPi;
        const f32 avgWL = (wl0 + wl1) * 0.5f;
        const f32 avgSt = (waveDir0.z + waveDir1.z) * 0.5f;
        const f32 maxOctaveA = avgSt * avgWL / kTwoPi;
        const f32 octaveSum = maxOctaveA * 1.67f; // 0.5+0.4+0.3+0.22+0.15+0.1
        return amp * (a0 * 0.55f + a1 * 0.55f + octaveSum) * 1.5f;
    }
} // namespace

TEST(WaterSurfaceSampler, FlatWaterReturnsPlaneHeight)
{
    // Zero amplitude crushes every wave to nothing — the surface is the plane.
    WaterComponent wc{};
    wc.m_WaveAmplitude = 0.0f;
    const WaterSurface::Params p = ParamsFrom(wc, /*planeHeight=*/2.5f);

    for (f32 t : { 0.0f, 0.37f, 5.0f })
        for (glm::vec2 xz : { glm::vec2(0.0f), glm::vec2(13.0f, -7.0f), glm::vec2(-50.0f, 50.0f) })
        {
            EXPECT_NEAR(WaterSurface::SampleHeight(p, xz, t), 2.5f, 1e-4f);
            EXPECT_NEAR(WaterSurface::SampleDisplacement(p, xz, t).y, 0.0f, 1e-4f);
        }
}

TEST(WaterSurfaceSampler, ZeroSteepnessHasNoVerticalDisplacement)
{
    // Every wave amplitude is derived from steepness (a = steepness / k), and the
    // detail octaves scale by the average steepness, so steepness 0 ⇒ flat.
    WaterComponent wc{};
    wc.m_WaveSteepness0 = 0.0f;
    wc.m_WaveSteepness1 = 0.0f;
    const WaterSurface::Params p = ParamsFrom(wc, 0.0f);

    EXPECT_NEAR(WaterSurface::SampleDisplacement(p, { 4.0f, 9.0f }, 1.0f).y, 0.0f, 1e-4f);
    EXPECT_NEAR(WaterSurface::SampleHeight(p, { 4.0f, 9.0f }, 1.0f), 0.0f, 1e-4f);
}

TEST(WaterSurfaceSampler, PlaneHeightShiftsResultByExactlyDelta)
{
    // The wave field is evaluated in world XZ and only offset vertically by the
    // plane height — raising the plane must raise the surface 1:1.
    WaterComponent wc{};
    const WaterSurface::Params p0 = ParamsFrom(wc, 0.0f);
    const WaterSurface::Params p1 = ParamsFrom(wc, 10.0f);

    const glm::vec2 xz(3.0f, -2.0f);
    const f32 h0 = WaterSurface::SampleHeight(p0, xz, 0.5f);
    const f32 h1 = WaterSurface::SampleHeight(p1, xz, 0.5f);
    EXPECT_NEAR(h1 - h0, 10.0f, 1e-3f);
}

TEST(WaterSurfaceSampler, VerticalDisplacementRespectsTheRendererBound)
{
    // Tie the sampler to the renderer's frustum-cull displacement margin: the
    // sampled height must never poke past the bound the shader culls against,
    // else off-screen wave crests would pop (and buoyancy would over/undershoot).
    WaterComponent wc{};
    const WaterSurface::Params p = ParamsFrom(wc, 0.0f);
    const glm::vec4 waveParams(0.0f, wc.m_WaveSpeed, wc.m_WaveAmplitude, wc.m_WaveFrequency);
    const f32 bound = MaxWaveDisplacementBound(waveParams, wc.PackWaveDir0(), wc.PackWaveDir1());
    ASSERT_GT(bound, 0.0f);

    for (f32 t = 0.0f; t < 6.0f; t += 0.25f)
        for (i32 ix = -40; ix <= 40; ix += 8)
            for (i32 iz = -40; iz <= 40; iz += 8)
            {
                const f32 dy = WaterSurface::SampleDisplacement(p, { static_cast<f32>(ix), static_cast<f32>(iz) }, t).y;
                ASSERT_TRUE(std::isfinite(dy));
                EXPECT_LE(std::abs(dy), bound)
                    << "displacement exceeded the shader cull margin at (" << ix << "," << iz << ") t=" << t;
                // SampleHeight stays inside the same band around the plane.
                const f32 h = WaterSurface::SampleHeight(p, { static_cast<f32>(ix), static_cast<f32>(iz) }, t);
                EXPECT_LE(std::abs(h), bound + 1e-3f);
            }
}

TEST(WaterSurfaceSampler, WavesAnimateOverTimeButFreezeAtZeroSpeed)
{
    WaterComponent wc{};
    const glm::vec2 xz(2.0f, 1.0f);

    // Non-zero speed: the surface at a fixed point moves between two times.
    const WaterSurface::Params moving = ParamsFrom(wc, 0.0f);
    const f32 a = WaterSurface::SampleHeight(moving, xz, 0.0f);
    const f32 b = WaterSurface::SampleHeight(moving, xz, 1.0f);
    EXPECT_GT(std::abs(a - b), 1e-3f) << "waves did not animate with time";

    // Zero speed freezes the phase: identical across times (deterministic captures).
    WaterComponent frozenWc = wc;
    frozenWc.m_WaveSpeed = 0.0f;
    const WaterSurface::Params frozen = ParamsFrom(frozenWc, 0.0f);
    EXPECT_NEAR(WaterSurface::SampleHeight(frozen, xz, 0.0f),
                WaterSurface::SampleHeight(frozen, xz, 9.0f), 1e-5f);
}

TEST(WaterSurfaceSampler, IsDeterministic)
{
    WaterComponent wc{};
    const WaterSurface::Params p = ParamsFrom(wc, 1.0f);
    const glm::vec2 xz(7.0f, -3.0f);
    EXPECT_FLOAT_EQ(WaterSurface::SampleHeight(p, xz, 2.0f), WaterSurface::SampleHeight(p, xz, 2.0f));
    const glm::vec3 d0 = WaterSurface::SampleDisplacement(p, xz, 2.0f);
    const glm::vec3 d1 = WaterSurface::SampleDisplacement(p, xz, 2.0f);
    EXPECT_FLOAT_EQ(d0.x, d1.x);
    EXPECT_FLOAT_EQ(d0.y, d1.y);
    EXPECT_FLOAT_EQ(d0.z, d1.z);
}

TEST(WaterSurfaceSampler, HeightInversionLandsOverTheQueryColumn)
{
    // SampleHeight inverts the horizontal Gerstner shift: the base point it solves
    // for, re-displaced horizontally, must land back on the query XZ. We can't see
    // the internal base, but we can verify the height it returns is the vertical
    // displacement of *some* base whose horizontal image is the query — i.e. the
    // naive (un-inverted) read and the inverted read agree to within the
    // horizontal displacement magnitude, and the inverted read is self-consistent.
    WaterComponent wc{};
    const WaterSurface::Params p = ParamsFrom(wc, 0.0f);
    const glm::vec2 query(5.0f, 5.0f);
    const f32 t = 1.3f;

    const f32 h = WaterSurface::SampleHeight(p, query, t);
    ASSERT_TRUE(std::isfinite(h));

    // Reconstruct the base the inversion should have found and confirm it images
    // back onto the query column (sub-centimetre after the iteration converges).
    glm::vec2 base = query;
    for (int it = 0; it < 8; ++it)
    {
        const glm::vec3 d = WaterSurface::SampleDisplacement(p, base, t);
        base = query - glm::vec2(d.x, d.z);
    }
    const glm::vec3 dFinal = WaterSurface::SampleDisplacement(p, base, t);
    const glm::vec2 imaged = base + glm::vec2(dFinal.x, dFinal.z);
    EXPECT_NEAR(imaged.x, query.x, 1e-2f);
    EXPECT_NEAR(imaged.y, query.y, 1e-2f);
    // The well-converged base reproduces the same height the sampler returned.
    EXPECT_NEAR(p.m_PlaneHeight + dFinal.y, h, 5e-2f);
}
