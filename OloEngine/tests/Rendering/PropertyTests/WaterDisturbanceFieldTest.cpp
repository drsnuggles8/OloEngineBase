#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1

// =============================================================================
// WaterDisturbanceFieldTest — L1 property tests for the boat-wake foam field's
// ENCODING CONTRACT (issue #967).
//
// The contract itself lives in Renderer/Water/WaterDisturbanceField.h, and is
// mirrored by include/WaterDisturbanceCommon.glsl and
// compute/WaterDisturbance_Update.comp. Every defect in the scheme is a WRONG
// ADDRESS, and every one of them renders: a mirrored field, a half-texel offset
// and a wake that never fades all produce a picture, so none of them shows up
// as an error. These pin the addressing, the decay recurrence and the bounded
// queues headlessly, so they gate CI rather than only a GPU-equipped run.
//
// Three of these are deliberately NEGATIVE-CONTROLLED — they assert first that
// their fixture is in the regime they exist to test (a negative lattice
// coordinate; an R8-quantised value; a full queue), because each would
// otherwise pass trivially on a fixture that had drifted into the easy case.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Physics3D/BoatWakeSystem.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceField.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    namespace WD = OloEngine::WaterDisturbance;

    // f32/i32/u32 are GLOBAL typedefs (Core/Base.h declares them after the
    // OloEngine namespace closes), so there is nothing to import here.

    /// Round-trip a value through R8's unsigned-normalized quantisation, the way
    /// a GL_R8 image store would.
    [[nodiscard]] f32 QuantiseR8(f32 v)
    {
        return std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f) / 255.0f;
    }

    /// Round-trip a value through a 16-bit float's mantissa, the way the RG16F
    /// field actually stores it. Not a full half-float emulation — it models the
    /// only property under test, the relative quantum near the value.
    [[nodiscard]] f32 QuantiseHalf(f32 v)
    {
        if (!(v > 0.0f))
            return 0.0f;
        const i32 exponent = static_cast<i32>(std::floor(std::log2(v)));
        const f32 quantum = std::exp2(static_cast<f32>(exponent - 10));
        return std::round(v / quantum) * quantum;
    }
} // namespace

// -----------------------------------------------------------------------------
// 1. Toroidal addressing
// -----------------------------------------------------------------------------

TEST(WaterDisturbanceFieldTest, StorageCoordinatesAreAlwaysInsideTheTexture)
{
    // Includes coordinates well into the negative — the case a bare `%` gets
    // wrong. -1 maps to kResolution-1, not to -1.
    for (const i32 lattice : { -100000, -1025, -513, -512, -511, -1, 0, 1, 511, 512, 513, 100000 })
    {
        const glm::ivec2 storage = WD::StorageForLattice(glm::ivec2(lattice, -lattice));
        EXPECT_GE(storage.x, 0) << "lattice " << lattice;
        EXPECT_LT(storage.x, WD::kResolution) << "lattice " << lattice;
        EXPECT_GE(storage.y, 0) << "lattice " << lattice;
        EXPECT_LT(storage.y, WD::kResolution) << "lattice " << lattice;
    }
}

TEST(WaterDisturbanceFieldTest, NegativeWorldCoordinatesRoundTripThroughTheWindow)
{
    // A boat sailing into -X/-Z is not an edge case in Drift, it is half the
    // map. Build the fixture there deliberately.
    const glm::vec2 centre(-1730.25f, -880.75f);
    const glm::ivec2 latticeMin = WD::LatticeMinForCentre(centre);

    // NEGATIVE CONTROL: assert the fixture is actually in the negative regime
    // before testing it, so this cannot degenerate into re-testing the positive
    // case if the constants ever change.
    ASSERT_LT(latticeMin.x, 0);
    ASSERT_LT(latticeMin.y, 0);

    for (i32 dx = 0; dx < WD::kResolution; dx += 37)
    {
        for (i32 dy = 0; dy < WD::kResolution; dy += 41)
        {
            const glm::ivec2 lattice = latticeMin + glm::ivec2(dx, dy);
            const glm::ivec2 storage = WD::StorageForLattice(lattice);
            EXPECT_EQ(WD::LatticeForStorage(storage, latticeMin), lattice);
        }
    }
}

TEST(WaterDisturbanceFieldTest, WorldToLatticeUsesFloorNotTruncation)
{
    // Truncation folds [-texel, 0) onto texel 0, giving that one row twice the
    // footprint and shifting every negative texel by one. The two samples
    // either side of the origin must land in DIFFERENT texels.
    const glm::ivec2 justBelow = WD::LatticeForWorld(glm::vec2(-0.01f, -0.01f));
    const glm::ivec2 justAbove = WD::LatticeForWorld(glm::vec2(0.01f, 0.01f));
    EXPECT_EQ(justBelow, glm::ivec2(-1, -1));
    EXPECT_EQ(justAbove, glm::ivec2(0, 0));
}

TEST(WaterDisturbanceFieldTest, OneTexelOfMotionInvalidatesExactlyOneColumn)
{
    // The property that makes the toroidal scheme worth having: a one-texel
    // shift must reassign one column and leave everything else valid at its
    // existing address. If this ever reports kResolution^2, the addressing has
    // collapsed into "re-centre everything", and the wake will drag sideways
    // with the camera.
    const glm::ivec2 before = WD::LatticeMinForCentre(glm::vec2(0.0f, 0.0f));
    const glm::ivec2 after = before + glm::ivec2(1, 0);

    i32 newlyExposed = 0;
    for (i32 sx = 0; sx < WD::kResolution; ++sx)
    {
        for (i32 sy = 0; sy < WD::kResolution; ++sy)
        {
            const glm::ivec2 lattice = WD::LatticeForStorage(glm::ivec2(sx, sy), after);
            if (!WD::WindowContains(lattice, before))
                ++newlyExposed;
        }
    }
    EXPECT_EQ(newlyExposed, WD::kResolution) << "a one-texel shift must expose exactly one column";
}

TEST(WaterDisturbanceFieldTest, AWindowThatJumpsClearOfItselfExposesEverything)
{
    // The complementary case, and the one that matters for a teleport or an
    // origin rebase: nothing may be carried over, because nothing overlaps.
    const glm::ivec2 before = WD::LatticeMinForCentre(glm::vec2(0.0f, 0.0f));
    const glm::ivec2 after = before + glm::ivec2(WD::kResolution, WD::kResolution);

    i32 carried = 0;
    for (i32 sx = 0; sx < WD::kResolution; sx += 13)
    {
        for (i32 sy = 0; sy < WD::kResolution; sy += 13)
        {
            const glm::ivec2 lattice = WD::LatticeForStorage(glm::ivec2(sx, sy), after);
            if (WD::WindowContains(lattice, before))
                ++carried;
        }
    }
    EXPECT_EQ(carried, 0);
}

// -----------------------------------------------------------------------------
// 2. The half-texel sampling convention
// -----------------------------------------------------------------------------

TEST(WaterDisturbanceFieldTest, TexelCentresLandOnTexelCentresInUvSpace)
{
    // The contract claims uv = worldXZ * invExtent needs NO correction term:
    // the centre of lattice texel `a` must land at uv (s + 0.5)/N modulo 1,
    // where s is its storage coordinate. Half a texel of error here puts the
    // wake visibly off the hull, and looks like a physics offset.
    for (const i32 lx : { -1027, -512, -3, 0, 5, 512, 1031 })
    {
        const glm::ivec2 lattice(lx, lx + 7);
        const glm::vec2 world = WD::WorldForLatticeCentre(lattice);
        const glm::vec2 uv = WD::FieldUV(world);
        const glm::ivec2 storage = WD::StorageForLattice(lattice);

        // Fold uv into [0,1) the way a REPEAT sampler does.
        const f32 fracX = uv.x - std::floor(uv.x);
        const f32 fracY = uv.y - std::floor(uv.y);
        const f32 expectX = (static_cast<f32>(storage.x) + 0.5f) / static_cast<f32>(WD::kResolution);
        const f32 expectY = (static_cast<f32>(storage.y) + 0.5f) / static_cast<f32>(WD::kResolution);
        EXPECT_NEAR(fracX, expectX, 1.0e-5f) << "lattice.x " << lattice.x;
        EXPECT_NEAR(fracY, expectY, 1.0e-5f) << "lattice.y " << lattice.y;
    }
}

TEST(WaterDisturbanceFieldTest, EdgeFadeIsOneAtTheCentreAndZeroAtTheBoundary)
{
    const glm::ivec2 latticeMin = WD::LatticeMinForCentre(glm::vec2(120.0f, -340.0f));
    const glm::vec2 centre = WD::WindowCentreWorld(latticeMin);

    EXPECT_NEAR(WD::EdgeFade(centre, centre), 1.0f, 1.0e-5f);

    // At the boundary (half an extent away on either axis) the torus seam puts
    // unrelated content one texel over — the fade must have reached zero before
    // anything can sample across it.
    const f32 half = WD::kFieldExtentMetres * 0.5f;
    EXPECT_NEAR(WD::EdgeFade(centre + glm::vec2(half, 0.0f), centre), 0.0f, 1.0e-5f);
    EXPECT_NEAR(WD::EdgeFade(centre + glm::vec2(0.0f, -half), centre), 0.0f, 1.0e-5f);

    // And it is monotonically decreasing outward, so there is no band where the
    // field brightens as it approaches the seam.
    f32 previous = 1.0f;
    for (f32 t = 0.0f; t <= half; t += half / 64.0f)
    {
        const f32 fade = WD::EdgeFade(centre + glm::vec2(t, 0.0f), centre);
        EXPECT_LE(fade, previous + 1.0e-5f) << "at " << t << " m from the centre";
        previous = fade;
    }
}

// -----------------------------------------------------------------------------
// 3. Decay
// -----------------------------------------------------------------------------

TEST(WaterDisturbanceFieldTest, DecayIsFrameRateIndependent)
{
    // One 1/30 s step must leave the same value as two 1/60 s steps. A per-frame
    // constant subtraction would fail this, and the symptom — a wake that fades
    // faster on a faster machine — is a gameplay difference, not a visual one.
    constexpr f32 halfLife = 6.0f;
    const f32 oneBigStep = WD::DecayFactor(halfLife, 1.0f / 30.0f);
    const f32 twoSmall = WD::DecayFactor(halfLife, 1.0f / 60.0f) * WD::DecayFactor(halfLife, 1.0f / 60.0f);
    EXPECT_NEAR(oneBigStep, twoSmall, 1.0e-6f);
}

TEST(WaterDisturbanceFieldTest, DecayHalvesOverExactlyOneHalfLife)
{
    EXPECT_NEAR(WD::DecayFactor(4.0f, 4.0f), 0.5f, 1.0e-6f);
    EXPECT_NEAR(WD::DecayFactor(4.0f, 8.0f), 0.25f, 1.0e-6f);
}

TEST(WaterDisturbanceFieldTest, DegenerateDecayInputsFailSafe)
{
    // A non-finite or non-positive half-life clears rather than freezes: a
    // frozen field is a permanent white smear nobody can explain.
    EXPECT_FLOAT_EQ(WD::DecayFactor(0.0f, 0.016f), 0.0f);
    EXPECT_FLOAT_EQ(WD::DecayFactor(-1.0f, 0.016f), 0.0f);
    EXPECT_FLOAT_EQ(WD::DecayFactor(std::numeric_limits<f32>::quiet_NaN(), 0.016f), 0.0f);
    // A zero/negative dt is "no time passed", which must not decay anything.
    EXPECT_FLOAT_EQ(WD::DecayFactor(6.0f, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(WD::DecayFactor(6.0f, -0.5f), 1.0f);
}

TEST(WaterDisturbanceFieldTest, DecayStallsUnderR8QuantisationButNotUnder16BitFloat)
{
    // THE reason the field is not an R8 (WaterDisturbanceField.h §4). At a 6 s
    // half-life and 60 Hz the per-frame step at 0.5 is ~0.00096, which is
    // smaller than half of R8's 1/255 quantum — so the decayed value rounds
    // straight back to where it started and the wake never fades. Every unit
    // test of the decay MATH passes while this happens, which is why the
    // storage format needs its own test.
    constexpr f32 halfLife = 6.0f;
    constexpr f32 dt = 1.0f / 60.0f;
    const f32 factor = WD::DecayFactor(halfLife, dt);

    // NEGATIVE CONTROL: the regime only exists while the per-frame step is
    // below R8's rounding threshold. Assert that before drawing any conclusion
    // from the stall — if a future default moved the half-life down far enough,
    // R8 would decay fine and this test would be asserting nothing.
    const f32 perFrameStep = 0.5f - 0.5f * factor;
    ASSERT_LT(perFrameStep, 0.5f / 255.0f) << "fixture is no longer in the R8-stall regime";

    f32 r8 = 0.5f;
    f32 half = 0.5f;
    f32 exact = 0.5f;                         // the same recurrence with no quantisation at all
    for (int frame = 0; frame < 600; ++frame) // 10 seconds — 1.67 half-lives
    {
        r8 = QuantiseR8(r8 * factor);
        half = QuantiseHalf(half * factor);
        exact *= factor;
    }

    // What the decay is SUPPOSED to do over this span: 0.5 -> ~0.157.
    ASSERT_LT(exact, 0.17f);

    // R8 settles onto the nearest representable level (0.5 is not one — 0.5*255
    // is 127.5) and then stops, because every subsequent step is smaller than
    // half a quantum and rounds back to the level it is already on. So it loses
    // ONE quantum, once, and never moves again: 0.4% instead of 69%.
    EXPECT_NEAR(r8, 0.5f, 1.5f / 255.0f)
        << "R8 is expected to STALL within one quantum of where it started. If it now decays, "
           "re-derive the format choice in WaterDisturbanceField.h §4";
    EXPECT_GT(r8, exact + 0.2f) << "R8 must be nowhere near the correct decayed value";

    // 16-bit float has ~8x the relative precision at this magnitude, so the same
    // step is several quanta and the decay proceeds.
    EXPECT_LT(half, 0.35f) << "16-bit float storage must actually decay past one half-life";
    EXPECT_NEAR(half, exact, 0.01f) << "16-bit float storage must track the exact decay closely";
}

// -----------------------------------------------------------------------------
// 4. Splats
// -----------------------------------------------------------------------------

TEST(WaterDisturbanceFieldTest, SplatWeightPeaksAtTheCentreAndVanishesAtTheRadius)
{
    const glm::vec2 p(10.0f, -4.0f);
    EXPECT_NEAR(WD::SplatWeight(p, p, p, 2.0f, 1.0f), 1.0f, 1.0e-5f);
    EXPECT_FLOAT_EQ(WD::SplatWeight(p + glm::vec2(2.0f, 0.0f), p, p, 2.0f, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(WD::SplatWeight(p + glm::vec2(5.0f, 0.0f), p, p, 2.0f, 1.0f), 0.0f);
    EXPECT_GT(WD::SplatWeight(p + glm::vec2(1.0f, 0.0f), p, p, 2.0f, 1.0f), 0.0f);
}

TEST(WaterDisturbanceFieldTest, TheCapsuleClosesTheGapADiscWouldLeave)
{
    // The reason a splat is a swept capsule rather than a per-frame disc: at
    // speed, consecutive frames' discs do not overlap, and the gap persists for
    // the whole decay tail. Model one 0.25 m-radius hull moving 3 m in a frame.
    const glm::vec2 from(0.0f, 0.0f);
    const glm::vec2 to(3.0f, 0.0f);
    const glm::vec2 midpoint(1.5f, 0.0f);
    constexpr f32 radius = 0.25f;

    // A pair of point splats at the ends leaves the midpoint untouched...
    EXPECT_FLOAT_EQ(WD::SplatWeight(midpoint, from, from, radius, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(WD::SplatWeight(midpoint, to, to, radius, 1.0f), 0.0f);
    // ...and the capsule covers it at full strength.
    EXPECT_NEAR(WD::SplatWeight(midpoint, from, to, radius, 1.0f), 1.0f, 1.0e-5f);
    // Still bounded perpendicular to the sweep — a capsule, not a slab.
    EXPECT_FLOAT_EQ(WD::SplatWeight(midpoint + glm::vec2(0.0f, radius), from, to, radius, 1.0f), 0.0f);
    // And bounded along it: past the far cap there is nothing.
    EXPECT_FLOAT_EQ(WD::SplatWeight(to + glm::vec2(radius, 0.0f), from, to, radius, 1.0f), 0.0f);
}

TEST(WaterDisturbanceFieldTest, CombiningSplatsSaturatesRatherThanAccumulating)
{
    // `max`, not `+=`. Re-stamping the same splat must be IDEMPOTENT, otherwise
    // the foam a boat lays down scales with frame rate and a boat holding
    // station ramps to full white and stays there.
    f32 value = 0.0f;
    for (int i = 0; i < 100; ++i)
        value = WD::CombineSplat(value, 0.4f);
    EXPECT_FLOAT_EQ(value, 0.4f);

    // A stronger splat wins; a weaker one leaves an existing stronger value alone.
    EXPECT_FLOAT_EQ(WD::CombineSplat(0.4f, 0.9f), 0.9f);
    EXPECT_FLOAT_EQ(WD::CombineSplat(0.9f, 0.4f), 0.9f);
    // And the result is bounded whatever it is handed.
    EXPECT_FLOAT_EQ(WD::CombineSplat(0.9f, 5.0f), 1.0f);
}

// -----------------------------------------------------------------------------
// 5. The bounded splat queue
// -----------------------------------------------------------------------------
//
// SubmitSplat is deliberately usable with no GL context and before Init(), so
// the bound and the rejection rules are testable headlessly. Every gtest case
// runs in its own process, so the process-wide queue is not shared between
// these; Reset() at the top is belt-and-braces, not load-bearing.

TEST(WaterDisturbanceFieldTest, TheSplatQueueIsBoundedAndCountsWhatItDrops)
{
    using OloEngine::WaterDisturbanceSplat;
    using OloEngine::WaterDisturbanceSystem;

    WaterDisturbanceSystem::Reset();

    constexpr u32 overshoot = 40;
    u32 accepted = 0;
    for (u32 i = 0; i < WD::kMaxSplatsPerFrame + overshoot; ++i)
    {
        WaterDisturbanceSplat splat;
        splat.m_From = glm::vec2(static_cast<f32>(i), 0.0f);
        splat.m_To = splat.m_From;
        splat.m_Radius = 1.0f;
        splat.m_Strength = 0.5f;
        if (WaterDisturbanceSystem::SubmitSplat(splat))
            ++accepted;
    }

    // NEGATIVE CONTROL: the test is only meaningful if the queue actually
    // overflowed. A cap raised past the overshoot would otherwise make the
    // drop assertions vacuous.
    ASSERT_GT(WaterDisturbanceSystem::GetDroppedSplatCount(), 0u);

    EXPECT_EQ(accepted, WD::kMaxSplatsPerFrame);
    EXPECT_EQ(WaterDisturbanceSystem::GetQueuedSplatCount(), WD::kMaxSplatsPerFrame);
    EXPECT_EQ(WaterDisturbanceSystem::GetDroppedSplatCount(), overshoot);

    WaterDisturbanceSystem::Reset();
    EXPECT_EQ(WaterDisturbanceSystem::GetQueuedSplatCount(), 0u);
    EXPECT_EQ(WaterDisturbanceSystem::GetDroppedSplatCount(), 0u);
}

TEST(WaterDisturbanceFieldTest, TheSplatQueueRejectsNonFiniteAndDegenerateSubmissions)
{
    using OloEngine::WaterDisturbanceSplat;
    using OloEngine::WaterDisturbanceSystem;

    WaterDisturbanceSystem::Reset();

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();

    WaterDisturbanceSplat base;
    base.m_From = glm::vec2(1.0f, 2.0f);
    base.m_To = glm::vec2(1.0f, 2.0f);
    base.m_Radius = 1.0f;
    base.m_Strength = 0.5f;
    base.m_Softness = 1.5f;

    auto rejects = [&base](auto mutate)
    {
        WaterDisturbanceSplat s = base;
        mutate(s);
        return !OloEngine::WaterDisturbanceSystem::SubmitSplat(s);
    };

    // This is the ONE validation boundary in the whole feature — it is what
    // lets SplatWeight stay a literal mirror of its GLSL twin instead of each
    // side inventing its own fallback for garbage.
    EXPECT_TRUE(rejects([&](WaterDisturbanceSplat& s)
                        { s.m_From.x = nan; }));
    EXPECT_TRUE(rejects([&](WaterDisturbanceSplat& s)
                        { s.m_To.y = inf; }));
    EXPECT_TRUE(rejects([&](WaterDisturbanceSplat& s)
                        { s.m_Radius = nan; }));
    EXPECT_TRUE(rejects([&](WaterDisturbanceSplat& s)
                        { s.m_Strength = inf; }));
    EXPECT_TRUE(rejects([&](WaterDisturbanceSplat& s)
                        { s.m_Softness = nan; }));
    // Degenerate but finite: contributes nothing, so it must not occupy a slot.
    EXPECT_TRUE(rejects([&](WaterDisturbanceSplat& s)
                        { s.m_Radius = 0.0f; }));
    EXPECT_TRUE(rejects([&](WaterDisturbanceSplat& s)
                        { s.m_Strength = 0.0f; }));

    EXPECT_EQ(WaterDisturbanceSystem::GetQueuedSplatCount(), 0u)
        << "a rejected splat must not consume a queue slot";
    // A rejection is not a DROP — the queue never filled up, so reporting these
    // as drops would make the "incomplete wake" signal useless.
    EXPECT_EQ(WaterDisturbanceSystem::GetDroppedSplatCount(), 0u);

    EXPECT_TRUE(WaterDisturbanceSystem::SubmitSplat(base)) << "a valid splat must still be accepted";
    EXPECT_EQ(WaterDisturbanceSystem::GetQueuedSplatCount(), 1u);

    WaterDisturbanceSystem::Reset();
}

TEST(WaterDisturbanceFieldTest, ShaderParamsReportDisabledUntilTheFieldHasBeenWritten)
{
    using OloEngine::WaterDisturbanceSystem;

    WaterDisturbanceSystem::Reset();
    // Never initialised in a headless test, and never updated — the field has
    // no content, so the sampling side must be told so. w <= 0 IS the disabled
    // state; a non-zero intensity here would put an uninitialised texture on
    // screen.
    EXPECT_LE(WaterDisturbanceSystem::GetShaderParams().w, 0.0f);

    // The fade params stay well-formed regardless, because smoothstep is
    // undefined for edge0 >= edge1 and the shader evaluates it unconditionally.
    const glm::vec4 params2 = WaterDisturbanceSystem::GetShaderParams2();
    EXPECT_LT(params2.x, params2.y);
    EXPECT_FLOAT_EQ(params2.z, WD::kEdgeFadeStart);
}

// -----------------------------------------------------------------------------
// 6. Bounded-trail eviction
// -----------------------------------------------------------------------------

TEST(WaterDisturbanceFieldTest, TheHullTrailIsBoundedAndEvictsOldestFirst)
{
    using OloEngine::BoatWakeSample;
    using OloEngine::BoatWakeTrail;

    BoatWakeTrail trail;
    EXPECT_EQ(trail.Count(), 0u);

    constexpr u32 overshoot = 50;
    for (u32 i = 0; i < BoatWakeTrail::kCapacity + overshoot; ++i)
    {
        BoatWakeSample s;
        s.m_WorldXZ = glm::vec2(static_cast<f32>(i), 0.0f);
        s.m_TimeSeconds = static_cast<f32>(i);
        s.m_Valid = true;
        trail.Push(s);
    }

    // NEGATIVE CONTROL: only meaningful if the ring actually overflowed.
    ASSERT_GT(trail.EvictedCount(), 0u);

    EXPECT_EQ(trail.Count(), BoatWakeTrail::kCapacity) << "the ring must never grow past its capacity";
    EXPECT_EQ(trail.EvictedCount(), overshoot);

    // Newest first, and the OLDEST samples are the ones that went — a ring that
    // dropped the newest would freeze the wake at the boat's starting point.
    const u32 newestIndex = BoatWakeTrail::kCapacity + overshoot - 1u;
    EXPECT_FLOAT_EQ(trail.At(0).m_WorldXZ.x, static_cast<f32>(newestIndex));
    EXPECT_FLOAT_EQ(trail.At(BoatWakeTrail::kCapacity - 1u).m_WorldXZ.x,
                    static_cast<f32>(newestIndex - (BoatWakeTrail::kCapacity - 1u)));
}

TEST(WaterDisturbanceFieldTest, TrailLookupPastTheEndReportsInvalidRatherThanWrapping)
{
    using OloEngine::BoatWakeSample;
    using OloEngine::BoatWakeTrail;

    BoatWakeTrail trail;
    BoatWakeSample s;
    s.m_WorldXZ = glm::vec2(7.0f, 0.0f);
    s.m_TimeSeconds = 1.0f;
    s.m_Valid = true;
    trail.Push(s);

    EXPECT_TRUE(trail.At(0).m_Valid);
    // A ring that silently wrapped would hand back the NEWEST sample dressed as
    // an old one, and the wake's V-arms would collapse onto the hull.
    EXPECT_FALSE(trail.At(1).m_Valid);
    EXPECT_FALSE(trail.At(BoatWakeTrail::kCapacity).m_Valid);
    EXPECT_FALSE(trail.At(BoatWakeTrail::kCapacity * 3u).m_Valid);
}

TEST(WaterDisturbanceFieldTest, TheVArmsSampleAnAgeRangeSoTheirOffsetGrows)
{
    using OloEngine::BoatWakeSample;
    using OloEngine::BoatWakeSystem;
    using OloEngine::BoatWakeTrail;

    // THE pin for the defect that shipped in review: the arms were laid at a
    // SINGLE age (`AtAge(now, kArmAgeSeconds)`), and because AtAge returns the
    // newest sample at least that old, the age — and therefore the lateral
    // offset — was the same every frame. The "diverging V" was two parallel
    // lines at a fixed offset. It rendered perfectly, so nothing failed.
    //
    // What must hold is that the sampled ages SPREAD, so the offsets computed
    // from them spread too. Asserting the offsets strictly increase is what a
    // single-age implementation cannot satisfy.

    // A boat running dead straight at a constant 8 m/s (above the full-speed
    // gate) for 4 s at 100 Hz.
    constexpr f32 kDt = 0.01f;
    constexpr f32 kSpeed = 8.0f;
    BoatWakeTrail trail;
    for (int i = 0; i <= 400; ++i)
    {
        BoatWakeSample s;
        s.m_TimeSeconds = static_cast<f32>(i) * kDt;
        s.m_WorldXZ = glm::vec2(0.0f, s.m_TimeSeconds * kSpeed);
        s.m_ForwardXZ = glm::vec2(0.0f, 1.0f);
        s.m_ForwardSpeed = kSpeed;
        s.m_Valid = true;
        trail.Push(s);
    }
    const f32 now = 4.0f;

    // The ring must actually reach back across the whole arm range, or the
    // outer segments are never laid at all.
    ASSERT_GE(static_cast<f32>(BoatWakeTrail::kCapacity) * kDt, BoatWakeSystem::kArmAgeMaxSeconds)
        << "BoatWakeTrail::kCapacity no longer spans kArmAgeMaxSeconds at 100 Hz";

    constexpr f32 kBeam = 2.4f;
    f32 previousAge = -1.0f;
    f32 previousOffset = -1.0f;
    u32 laid = 0;
    for (u32 i = 0; i < BoatWakeSystem::kArmAgeSamples; ++i)
    {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(BoatWakeSystem::kArmAgeSamples - 1u);
        const f32 wantAge = BoatWakeSystem::kArmAgeMinSeconds +
                            t * (BoatWakeSystem::kArmAgeMaxSeconds - BoatWakeSystem::kArmAgeMinSeconds);
        const BoatWakeSample s = trail.AtAge(now, wantAge);
        ASSERT_TRUE(s.m_Valid) << "no history at age " << wantAge;

        const f32 age = now - s.m_TimeSeconds;
        // Mirrors BoatWakeSystem's own offset expression; gate is 1 at 8 m/s.
        const f32 offset = kBeam * 0.5f + BoatWakeSystem::kArmSpreadMetresPerSecond * age * 1.0f;

        EXPECT_GT(age, previousAge) << "sample " << i << " is not older than the one before it";
        EXPECT_GT(offset, previousOffset) << "arm offset did not grow at sample " << i;
        previousAge = age;
        previousOffset = offset;
        ++laid;
    }

    // NEGATIVE CONTROL: a single-sample configuration would trivially satisfy
    // "offsets increase" with nothing to compare against.
    ASSERT_GE(laid, 2u) << "fewer than two arm samples — the growth assertions are vacuous";

    // And the spread is worth having: the outermost arm must sit meaningfully
    // wider than the innermost, or the V is visually a pair of parallel lines
    // even though the offsets technically differ.
    const f32 innerOffset = kBeam * 0.5f +
                            BoatWakeSystem::kArmSpreadMetresPerSecond * BoatWakeSystem::kArmAgeMinSeconds;
    const f32 outerOffset = kBeam * 0.5f +
                            BoatWakeSystem::kArmSpreadMetresPerSecond * BoatWakeSystem::kArmAgeMaxSeconds;
    EXPECT_GT(outerOffset - innerOffset, 1.0f)
        << "the V spreads by less than a metre across its whole age range";
}

TEST(WaterDisturbanceFieldTest, TrailAgeLookupPicksTheNewestSampleOldEnough)
{
    using OloEngine::BoatWakeSample;
    using OloEngine::BoatWakeTrail;

    BoatWakeTrail trail;
    for (int i = 0; i <= 100; ++i) // 0.00 s .. 1.00 s at 100 Hz
    {
        BoatWakeSample s;
        s.m_WorldXZ = glm::vec2(static_cast<f32>(i), 0.0f);
        s.m_TimeSeconds = static_cast<f32>(i) * 0.01f;
        s.m_Valid = true;
        trail.Push(s);
    }

    const BoatWakeSample found = trail.AtAge(1.0f, 0.55f);
    ASSERT_TRUE(found.m_Valid);
    // Newest sample at least 0.55 s old: t = 0.45 s, i.e. sample 45.
    EXPECT_NEAR(found.m_TimeSeconds, 0.45f, 1.0e-4f);
    EXPECT_NEAR(found.m_WorldXZ.x, 45.0f, 1.0e-3f);

    // Reaching back further than the history goes reports invalid rather than
    // clamping to the oldest — the arms then simply are not laid this frame,
    // which is correct for a boat that just started moving.
    EXPECT_FALSE(trail.AtAge(1.0f, 10.0f).m_Valid);

    trail.Clear();
    EXPECT_EQ(trail.Count(), 0u);
    EXPECT_FALSE(trail.At(0).m_Valid);
}
