#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1

// =============================================================================
// WaterFoamAdvectionTest — L1 property tests for the foam-advection contract
// (issue #1034, §2.2).
//
// The contract lives in Renderer/Water/WaterFoam.h and is mirrored by
// include/WaterFoamCommon.glsl and compute/WaterDisturbance_Update.comp.
//
// The acceptance criterion — "whitecaps DRIFT rather than pulsing in place" —
// is a multi-frame claim, and every way of getting it wrong still renders. So
// the load-bearing test here runs the ACTUAL recurrence (backtrace, bilinear
// read, decay, deposit) over a simulated field for many steps and asserts the
// foam's centre of mass MOVED, by roughly the distance the velocity says it
// should have. A single-frame assertion that "the advection ran" would pass on
// a field that stands perfectly still.
//
// Three of these are negative-controlled, because each would otherwise pass
// trivially:
//   * the drift test first asserts the field is non-empty and, with zero
//     velocity, does NOT move — so a test that always reports movement, or one
//     whose fixture is empty, fails;
//   * the toroidal test first asserts the tap it is about really is outside the
//     window, so it cannot degenerate into checking an in-window tap;
//   * the "calm sea deposits nothing" test first shows a folding sea deposits
//     something.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceField.h"
#include "OloEngine/Renderer/Water/WaterFoam.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace WF = OloEngine::WaterFoam;
    namespace WD = OloEngine::WaterDisturbance;
    namespace fs = std::filesystem;

    /// Half-width, in lattice texels, of the sub-window the simulation walks.
    ///
    /// The FIELD is the full 512^2 window — the addressing under test is the
    /// real one — but stepping all 262144 texels 40 times in a Debug build is
    /// minutes of CI time for nothing. The patch is 6 m across and travels 3 m,
    /// so a +/-32 m box around it is where every non-zero texel is and stays;
    /// anything advecting out of it would have to cross 26 m of empty water
    /// first.
    constexpr i32 kSimHalfExtent = 64;

    /// A CPU stand-in for the .g channel of the disturbance texture: one
    /// kResolution^2 toroidal window, addressed exactly as the compute pass
    /// addresses its storage.

    class FoamField
    {
      public:
        FoamField()
            : m_Texels(static_cast<sizet>(WD::kResolution) * WD::kResolution, 0.0f)
        {
        }

        [[nodiscard]] f32 Load(i32 x, i32 y) const
        {
            return m_Texels[static_cast<sizet>(y) * WD::kResolution + x];
        }
        void Store(i32 x, i32 y, f32 v)
        {
            m_Texels[static_cast<sizet>(y) * WD::kResolution + x] = v;
        }

        /// Deposit a disc of foam of `radius` metres centred at world XZ.
        void Splash(glm::vec2 centre, f32 radius, f32 value)
        {
            ForEachSimTexel(
                [&](glm::ivec2 lattice, glm::ivec2 storage)
                {
                    const glm::vec2 world = WD::WorldForLatticeCentre(lattice);
                    if (glm::length(world - centre) <= radius)
                        Store(storage.x, storage.y, value);
                });
        }

        /// One step of the recurrence the compute pass runs. Deliberately in
        /// the shader's order: backtrace, bilinear read of the PREVIOUS field,
        /// decay.
        [[nodiscard]] FoamField Step(glm::vec2 velocity, f32 decay, f32 deltaSeconds,
                                     glm::ivec2 latticeMin) const
        {
            FoamField next;
            ForEachSimTexel(
                [&](glm::ivec2 lattice, glm::ivec2 storage)
                {
                    const glm::vec2 world = WD::WorldForLatticeCentre(lattice);
                    const glm::vec2 source = WF::Backtrace(world, velocity, deltaSeconds);
                    const f32 advected = WF::SampleBilinear(
                        source, latticeMin, [this](i32 x, i32 y)
                        { return Load(x, y); });
                    next.Store(storage.x, storage.y, advected * decay);
                });
            return next;
        }

        /// Foam-weighted centre of mass in world XZ, and the total mass.
        [[nodiscard]] glm::vec2 CentreOfMass(f32& outMass) const
        {
            glm::vec2 weighted(0.0f);
            f32 mass = 0.0f;
            ForEachSimTexel(
                [&](glm::ivec2 lattice, glm::ivec2 storage)
                {
                    const f32 v = Load(storage.x, storage.y);
                    if (v <= 0.0f)
                        return;
                    weighted += WD::WorldForLatticeCentre(lattice) * v;
                    mass += v;
                });
            outMass = mass;
            return (mass > 0.0f) ? (weighted / mass) : glm::vec2(0.0f);
        }

      private:
        /// Walk the simulated sub-window, handing each visit both the ABSOLUTE
        /// lattice coordinate and the toroidal STORAGE coordinate it lives at —
        /// so the storage mapping under test is exercised on every texel rather
        /// than assumed.
        template<typename Visit>
        static void ForEachSimTexel(Visit&& visit)
        {
            for (i32 lz = -kSimHalfExtent; lz <= kSimHalfExtent; ++lz)
            {
                for (i32 lx = -kSimHalfExtent; lx <= kSimHalfExtent; ++lx)
                {
                    const glm::ivec2 lattice{ lx, lz };
                    visit(lattice, WD::StorageForLattice(lattice));
                }
            }
        }

        std::vector<f32> m_Texels;
    };

    [[nodiscard]] std::string ReadFoamGlsl()
    {
        const fs::path path = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "include" /
                              "WaterFoamCommon.glsl";
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // 1. The whole point: foam DRIFTS
    // -------------------------------------------------------------------------

    TEST(WaterFoamAdvectionTest, FoamTravelsWithTheAdvectingVelocity)
    {
        constexpr f32 kDt = 1.0f / 20.0f;
        constexpr i32 kSteps = 40; // two seconds
        const glm::vec2 velocity{ 1.5f, -0.75f };
        const glm::ivec2 latticeMin = WD::LatticeMinForCentre({ 0.0f, 0.0f });

        FoamField field;
        field.Splash({ 0.0f, 0.0f }, 6.0f, 1.0f);

        f32 startMass = 0.0f;
        const glm::vec2 start = field.CentreOfMass(startMass);
        // Negative control part one: the fixture must actually contain foam, or
        // "it moved" is a statement about an empty field.
        ASSERT_GT(startMass, 0.0f) << "the fixture deposited no foam at all";

        // Negative control part two: with ZERO velocity the SAME recurrence must
        // NOT move the patch. Without this, a test that reported movement for
        // any reason at all — including a bug that smeared the field toward the
        // origin — would still pass.
        {
            FoamField still = field;
            for (i32 i = 0; i < kSteps; ++i)
                still = still.Step({ 0.0f, 0.0f }, 1.0f, kDt, latticeMin);
            f32 stillMass = 0.0f;
            const glm::vec2 stillCentre = still.CentreOfMass(stillMass);
            ASSERT_GT(stillMass, 0.0f);
            EXPECT_NEAR(stillCentre.x, start.x, 0.05f) << "a zero velocity moved the foam";
            EXPECT_NEAR(stillCentre.y, start.y, 0.05f) << "a zero velocity moved the foam";
        }

        FoamField moving = field;
        for (i32 i = 0; i < kSteps; ++i)
            moving = moving.Step(velocity, 1.0f, kDt, latticeMin);

        f32 endMass = 0.0f;
        const glm::vec2 end = moving.CentreOfMass(endMass);
        ASSERT_GT(endMass, 0.0f) << "advection destroyed the whole field";

        const glm::vec2 expected = velocity * (kDt * static_cast<f32>(kSteps));
        const glm::vec2 actual = end - start;

        // Semi-Lagrangian advection is diffusive, so the patch spreads; its
        // CENTRE, though, travels at the velocity. A tenth of a texel per
        // second of slack over two seconds is generous and still an order of
        // magnitude tighter than "it moved at all".
        EXPECT_NEAR(actual.x, expected.x, 0.15f);
        EXPECT_NEAR(actual.y, expected.y, 0.15f);
        EXPECT_GT(glm::length(actual), 1.0f) << "the patch did not visibly move";
    }

    TEST(WaterFoamAdvectionTest, DecayReducesTheFieldOverTime)
    {
        constexpr f32 kDt = 1.0f / 20.0f;
        const glm::ivec2 latticeMin = WD::LatticeMinForCentre({ 0.0f, 0.0f });
        const f32 decay = WD::DecayFactor(3.5f, kDt);

        FoamField field;
        field.Splash({ 0.0f, 0.0f }, 5.0f, 1.0f);
        f32 mass0 = 0.0f;
        (void)field.CentreOfMass(mass0);

        // 3.5 s half-life, so 3.5 s of stepping should roughly halve it. The
        // advection is lossless in the interior, so any drop beyond diffusion
        // at the patch edge is the decay.
        const i32 steps = static_cast<i32>(3.5f / kDt);
        for (i32 i = 0; i < steps; ++i)
            field = field.Step({ 0.3f, 0.0f }, decay, kDt, latticeMin);

        f32 mass1 = 0.0f;
        (void)field.CentreOfMass(mass1);
        ASSERT_GT(mass0, 0.0f);
        const f32 ratio = mass1 / mass0;
        EXPECT_LT(ratio, 0.62f) << "the foam did not decay by a half-life over a half-life";
        EXPECT_GT(ratio, 0.38f) << "the foam decayed far faster than its half-life";
    }

    // -------------------------------------------------------------------------
    // 2. The toroidal addressing
    // -------------------------------------------------------------------------

    TEST(WaterFoamAdvectionTest, TapsOutsideTheWindowContributeNothingRatherThanWrapping)
    {
        const glm::ivec2 latticeMin = WD::LatticeMinForCentre({ 0.0f, 0.0f });

        // A world position a hair OUTSIDE the window's lower corner. Negative
        // control: assert the fixture really is in that regime, or this test
        // silently becomes a check on an ordinary in-window tap.
        const glm::vec2 justOutside =
            WD::WorldForLatticeCentre(latticeMin) - glm::vec2(WD::kTexelSizeMetres * 1.5f);
        const glm::ivec2 outsideLattice = WD::LatticeForWorld(justOutside);
        ASSERT_FALSE(WD::WindowContains(outsideLattice, latticeMin))
            << "the fixture position is inside the window — this test proves nothing";

        const WF::BilinearTaps taps = WF::PrevTaps(justOutside, latticeMin);
        for (i32 i = 0; i < 4; ++i)
        {
            EXPECT_FLOAT_EQ(taps.m_Weight[i], 0.0f)
                << "tap " << i << " outside the window carried weight — the torus seam is being "
                << "advected in, which renders as a ghost of the foam trailing the camera";
        }

        // ...and an interior position's taps DO sum to one, so the walk is not
        // simply returning nothing everywhere.
        f32 sum = 0.0f;
        const WF::BilinearTaps inside = WF::PrevTaps({ 3.3f, -7.7f }, latticeMin);
        for (i32 i = 0; i < 4; ++i)
            sum += inside.m_Weight[i];
        EXPECT_NEAR(sum, 1.0f, 1.0e-5f);
    }

    TEST(WaterFoamAdvectionTest, BilinearTapsUseTheHalfTexelConvention)
    {
        const glm::ivec2 latticeMin = WD::LatticeMinForCentre({ 0.0f, 0.0f });

        // Sampling EXACTLY at a texel centre must land entirely on that texel.
        // Dropping the -0.5 in PrevTaps shifts every advection step by half a
        // texel, which accumulates into a drift indistinguishable from a wrong
        // wind direction — and which no single-frame check can see.
        const glm::ivec2 lattice{ 7, -13 };
        const glm::vec2 centre = WD::WorldForLatticeCentre(lattice);
        const glm::ivec2 storage = WD::StorageForLattice(lattice);

        const WF::BilinearTaps taps = WF::PrevTaps(centre, latticeMin);
        f32 weightOnTarget = 0.0f;
        for (i32 i = 0; i < 4; ++i)
        {
            if (taps.m_Storage[i].x == storage.x && taps.m_Storage[i].y == storage.y)
                weightOnTarget += taps.m_Weight[i];
        }
        EXPECT_NEAR(weightOnTarget, 1.0f, 1.0e-4f)
            << "a sample at a texel centre did not land on that texel";
    }

    // -------------------------------------------------------------------------
    // 3. The velocity
    // -------------------------------------------------------------------------

    TEST(WaterFoamAdvectionTest, SurfaceVelocityCombinesOrbitalAndDrift)
    {
        // Orbital = d(displacement)/dt, drift added on top.
        const glm::vec2 v = WF::SurfaceVelocity({ 0.2f, 0.0f }, { 0.1f, 0.0f }, { 0.0f, 0.5f }, 0.1f);
        EXPECT_NEAR(v.x, 1.0f, 1.0e-4f);
        EXPECT_NEAR(v.y, 0.5f, 1.0e-4f);
    }

    TEST(WaterFoamAdvectionTest, ZeroOrNonFiniteDtDropsTheOrbitalTermRatherThanDividingByIt)
    {
        // A paused editor ticks at dt 0. Dividing there yields inf/NaN, and a
        // NaN velocity backtraces to a NaN position, which zeroes the texel
        // permanently through the max() in the deposit step — a field that
        // slowly develops dead spots and never recovers.
        for (const f32 dt : { 0.0f, -1.0f, std::numeric_limits<f32>::quiet_NaN() })
        {
            const glm::vec2 v = WF::SurfaceVelocity({ 5.0f, 5.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f }, dt);
            EXPECT_TRUE(std::isfinite(v.x) && std::isfinite(v.y)) << "dt = " << dt;
            EXPECT_NEAR(v.x, 1.0f, 1.0e-4f) << "the drift should survive; only the orbital drops";
            EXPECT_NEAR(v.y, 0.0f, 1.0e-4f);
        }
    }

    TEST(WaterFoamAdvectionTest, VelocityIsClampedSoOneStalledFrameCannotTeleportTheField)
    {
        // The orbital term divides by the frame's dt, so a single long frame —
        // or the frame right after a window jump, where the stored displacement
        // belongs to a different patch of sea — produces an arbitrarily large
        // velocity. Backtracing that far samples somewhere unrelated.
        const glm::vec2 v =
            WF::SurfaceVelocity({ 100.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, 1.0f / 60.0f);
        EXPECT_NEAR(glm::length(v), WF::kMaxVelocityMetresPerSecond, 1.0e-3f);
        // Direction preserved — a component-wise clamp would bend the flow.
        EXPECT_GT(v.x, 0.0f);
        EXPECT_NEAR(v.y, 0.0f, 1.0e-4f);
    }

    TEST(WaterFoamAdvectionTest, BacktraceIsAnIdentityWithNoStep)
    {
        const glm::vec2 p{ 12.5f, -3.25f };
        for (const f32 dt : { 0.0f, -1.0f })
        {
            const glm::vec2 out = WF::Backtrace(p, { 4.0f, 4.0f }, dt);
            EXPECT_FLOAT_EQ(out.x, p.x) << "dt = " << dt;
            EXPECT_FLOAT_EQ(out.y, p.y) << "dt = " << dt;
        }
    }

    // -------------------------------------------------------------------------
    // 4. The shared deposit criterion
    // -------------------------------------------------------------------------

    TEST(WaterFoamAdvectionTest, ACalmSeaDepositsNothing)
    {
        // Negative control first: a folding sea DOES deposit, so the assertion
        // below is about the fold and not about the function always returning
        // zero.
        ASSERT_GT(WF::DepositFromFold(0.9f, 0.25f), 0.0f);

        // saturate(1 - J) is exactly 0 on water that is not folding.
        EXPECT_FLOAT_EQ(WF::DepositFromFold(0.0f, 0.25f), 0.0f);
        EXPECT_FLOAT_EQ(WF::DepositFromFold(0.25f, 0.25f), 0.0f);
        EXPECT_FLOAT_EQ(WF::DepositFromFold(0.1f, 0.25f), 0.0f);
    }

    TEST(WaterFoamAdvectionTest, DepositIsARampNotAStep)
    {
        // A step makes the foam boundary an exact iso-contour of the Jacobian,
        // which on a smooth field is a smooth curve and reads as a drawn
        // outline rather than as foam.
        //
        // Sampled as fractions of the ramp's OWN span rather than at fixed fold
        // values, so this keeps testing the ramp if kFoamSaturationFold is
        // re-measured. The first version used 0.4/0.6/0.8 against a ramp that
        // ran to 1.0; once the upper end became the measured 0.35 all three
        // saturated and the monotonicity assertion would have compared three
        // identical ones.
        constexpr f32 lo = 0.15f;
        const f32 span = WF::kFoamSaturationFold - lo;
        ASSERT_GT(span, 0.0f);
        const f32 a = WF::DepositFromFold(lo + 0.25f * span, lo);
        const f32 b = WF::DepositFromFold(lo + 0.50f * span, lo);
        const f32 c = WF::DepositFromFold(lo + 0.75f * span, lo);
        EXPECT_GT(a, 0.0f);
        EXPECT_GT(b, a);
        EXPECT_GT(c, b);
        EXPECT_FLOAT_EQ(WF::DepositFromFold(WF::kFoamSaturationFold, lo), 1.0f);
        EXPECT_FLOAT_EQ(WF::DepositFromFold(1.0f, lo), 1.0f)
            << "a fold beyond saturation must stay clamped, not overshoot";
    }

    TEST(WaterFoamAdvectionTest, DepositRejectsNonFiniteInput)
    {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_FLOAT_EQ(WF::DepositFromFold(nan, 0.25f), 0.0f);
        EXPECT_FLOAT_EQ(WF::DepositFromFold(0.9f, nan), 0.0f);
    }

    TEST(WaterFoamAdvectionTest, CombineTakesTheMaxSoDepositIsNotFrameRateDependent)
    {
        // An accumulate would make the same sea foam twice as heavily at 120 Hz
        // as at 60 — the identical defect WaterDisturbance::CombineSplat's max
        // exists to avoid on the wake side.
        EXPECT_FLOAT_EQ(WF::CombineDeposit(0.4f, 0.7f), 0.7f);
        EXPECT_FLOAT_EQ(WF::CombineDeposit(0.7f, 0.4f), 0.7f);
        EXPECT_FLOAT_EQ(WF::CombineDeposit(0.9f, 0.9f), 0.9f);
        EXPECT_FLOAT_EQ(WF::CombineDeposit(2.0f, 3.0f), 1.0f) << "the result must stay in [0, 1]";
    }

    // -------------------------------------------------------------------------
    // 5. Layout and the GLSL twin
    // -------------------------------------------------------------------------

    TEST(WaterFoamAdvectionTest, DisturbanceUboLayoutMatchesTheShaderBlock)
    {
        using UBO = UBOStructures::WaterDisturbanceUBO;
        // The GLSL block spells these offsets out in comments; a C++ member
        // inserted without moving the shader's is a silent misread of every
        // field after it, not an error.
        EXPECT_EQ(offsetof(UBO, LatticeMin), 0u);
        EXPECT_EQ(offsetof(UBO, PrevLatticeMin), 8u);
        EXPECT_EQ(offsetof(UBO, TexelSize), 16u);
        EXPECT_EQ(offsetof(UBO, DecayFactor), 20u);
        EXPECT_EQ(offsetof(UBO, Resolution), 24u);
        EXPECT_EQ(offsetof(UBO, SplatCount), 28u);
        EXPECT_EQ(offsetof(UBO, ResetAll), 32u);
        EXPECT_EQ(offsetof(UBO, FoamEnabled), 36u);
        EXPECT_EQ(offsetof(UBO, FoamDecayFactor), 40u);
        EXPECT_EQ(offsetof(UBO, DeltaSeconds), 44u);
        EXPECT_EQ(offsetof(UBO, FoamDepositThreshold), 48u);
        EXPECT_EQ(offsetof(UBO, FoamDrift), 56u);
        EXPECT_EQ(offsetof(UBO, FoamFFTParams), 64u);
        EXPECT_EQ(offsetof(UBO, FoamFFTCascadeParams), 80u);
        EXPECT_EQ(offsetof(UBO, Splats), 96u);
        EXPECT_EQ(sizeof(UBO) % 16u, 0u) << "std140 requires a 16-byte multiple";
    }

    TEST(WaterFoamAdvectionTest, GlslTwinCarriesTheSameContractConstants)
    {
        const std::string glsl = ReadFoamGlsl();
        ASSERT_FALSE(glsl.empty()) << "could not read include/WaterFoamCommon.glsl";

        const sizet at = glsl.find("WATER_FOAM_MAX_VELOCITY = ");
        ASSERT_NE(at, std::string::npos);
        const sizet valueStart = at + std::string("WATER_FOAM_MAX_VELOCITY = ").size();
        const sizet valueEnd = glsl.find(';', valueStart);
        ASSERT_NE(valueEnd, std::string::npos);
        EXPECT_NEAR(std::stof(glsl.substr(valueStart, valueEnd - valueStart)),
                    WF::kMaxVelocityMetresPerSecond, 1.0e-6f)
            << "the GLSL velocity clamp drifted from WaterFoam.h";

        // The deposit ramp's upper end. A drift here scales every foam value on
        // the GPU only — the field still fills, still advects, and is simply
        // the wrong brightness, which no CPU test and no single frame can see.
        const sizet satAt = glsl.find("WATER_FOAM_SATURATION_FOLD = ");
        ASSERT_NE(satAt, std::string::npos)
            << "WaterFoamCommon.glsl is missing WATER_FOAM_SATURATION_FOLD";
        const sizet satStart = satAt + std::string("WATER_FOAM_SATURATION_FOLD = ").size();
        const sizet satEnd = glsl.find(';', satStart);
        ASSERT_NE(satEnd, std::string::npos);
        EXPECT_NEAR(std::stof(glsl.substr(satStart, satEnd - satStart)), WF::kFoamSaturationFold,
                    1.0e-6f)
            << "the GLSL deposit saturation drifted from WaterFoam.h";

        // The half-texel convention, as text. It is one edit away from being
        // dropped, it does not error, and the resulting per-frame half-texel
        // shift is invisible in any single frame.
        EXPECT_NE(glsl.find("absoluteWorldXZ / texelSize - 0.5"), std::string::npos)
            << "waterFoamPrevTaps lost its half-texel offset";

        // And the out-of-window guard, asserted as an ordering so it survives a
        // reformat but not a semantic change.
        const sizet fnAt = glsl.find("WaterFoamTaps waterFoamPrevTaps(");
        ASSERT_NE(fnAt, std::string::npos);
        const sizet guardAt = glsl.find("waterDisturbanceWindowContains", fnAt);
        const sizet fetchAt = glsl.find("taps.Weight[index] = wx * wz", fnAt);
        ASSERT_NE(guardAt, std::string::npos)
            << "waterFoamPrevTaps no longer tests the window at all — it is wrapping the torus "
               "seam into the advection";
        ASSERT_NE(fetchAt, std::string::npos);
        EXPECT_LT(guardAt, fetchAt);
    }
} // namespace OloEngine::Tests
