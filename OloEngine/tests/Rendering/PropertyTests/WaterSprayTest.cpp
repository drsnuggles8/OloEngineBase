#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1

// =============================================================================
// WaterSprayTest — L1 property tests for the crest-spray emission contract
// (issue #1034, §2.3).
//
// The acceptance criterion is "spray emits only from folding/steep crests, and
// does NOT emit on a calm sea", and the failure mode is silent: an emitter that
// sprays a flat sea renders a perfectly plausible frame — it just looks like
// the weather is wrong. So the emission path is deliberately a pure function
// over a crest-sampling callback (WaterSpray::Emit), which lets these run with
// no ocean, no GPU and no renderer, and lets the criterion be a CI gate rather
// than a screenshot.
//
// The calm-sea test is negative-controlled against a folding sea through the
// SAME fixture, so it cannot degenerate into "the emitter never emits".
//
// The other thing pinned here is that spray and foam share ONE crest detector.
// That is not a comment to be trusted: EmissionTracksTheFoamDeposit asserts the
// emitter's own threshold behaviour against WaterFoam::DepositFromFold, so a
// second detector growing here would have to reproduce that function exactly to
// pass, at which point it is the same function.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
#include "OloEngine/Renderer/Water/WaterFoam.h"
#include "OloEngine/Renderer/Water/WaterSpray.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace
{
    namespace WS = OloEngine::WaterSpray;
    namespace WF = OloEngine::WaterFoam;
    using OloEngine::GPUParticle;

    [[nodiscard]] WS::WaterSpraySettings MakeSettings()
    {
        WS::WaterSpraySettings s;
        s.m_Enabled = true;
        // Inside the deposit ramp (which now runs to the measured
        // kFoamSaturationFold), so the fold values below land ON the ramp
        // rather than all saturating at its top.
        s.m_Threshold = 0.15f;
        s.m_RatePerCell = 40.0f; // high, so a single 1/60 s step emits reliably
        s.m_RadiusMetres = 8.0f; // 9x9 cells — enough to measure, cheap in Debug
        s.m_LaunchSpeed = 3.0f;
        s.m_Lifetime = 0.9f;
        s.m_ParticleSize = 0.12f;
        s.m_WindMetresPerSecond = { 8.0f, 0.0f };
        s.m_WaterPlaneY = 0.0f;
        s.m_HeightScale = 1.0f;
        return s;
    }

    /// A sea whose fold signal is a constant. `fold == 0` is a calm sea:
    /// saturate(1 - Jacobian) is exactly zero on water that is not folding.
    [[nodiscard]] auto FlatSea(f32 fold, f32 height = 0.0f)
    {
        // Zero choppy displacement, so the emitter's spawn point is the sample
        // point and the geometry assertions below stay readable. The DISPLACED
        // case is covered by ARealStormOceanFolds..., which samples a real field.
        return [fold, height](glm::vec2) -> WS::CrestSample
        { return { fold, height, glm::vec2(0.0f) }; };
    }

    [[nodiscard]] u32 EmitCount(const WS::WaterSpraySettings& settings, f32 fold, f32 foamThreshold,
                                f32 timeSeconds, f32 dt)
    {
        const auto particles =
            WS::Emit(settings, foamThreshold, { 0.0f, 0.0f }, timeSeconds, dt, FlatSea(fold));
        return static_cast<u32>(particles.size());
    }

    /// Emissions summed over several steps, so a probabilistic emitter is
    /// measured rather than sampled once.
    [[nodiscard]] u32 EmitOverSeveralFrames(const WS::WaterSpraySettings& settings, f32 fold,
                                            f32 foamThreshold)
    {
        constexpr f32 kDt = 1.0f / 60.0f;
        u32 total = 0;
        // A dozen frames rather than a full second: the emitter is
        // probabilistic, so this has to be a SUM over several frames rather
        // than one sample, but a whole second of them buys nothing but Debug
        // runtime.
        for (i32 i = 0; i < 12; ++i)
            total += EmitCount(settings, fold, foamThreshold, 5.0f + static_cast<f32>(i) * kDt, kDt);
        return total;
    }
} // namespace

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // 1. The acceptance criterion
    // -------------------------------------------------------------------------

    TEST(WaterSprayTest, ACalmSeaEmitsNothing)
    {
        const WS::WaterSpraySettings settings = MakeSettings();

        // Negative control: the SAME fixture on a hard-folding sea must emit,
        // or the assertion below is about an emitter that never emits at all.
        ASSERT_GT(EmitOverSeveralFrames(settings, 0.30f, 0.10f), 0u)
            << "the fixture emitted nothing even on a folding sea";

        // A sea that is not folding has fold == 0 exactly. Not "few particles":
        // none.
        EXPECT_EQ(EmitOverSeveralFrames(settings, 0.0f, 0.10f), 0u);
        // ...and one that folds, but below the threshold, is still silent.
        EXPECT_EQ(EmitOverSeveralFrames(settings, 0.10f, 0.10f), 0u);
        EXPECT_EQ(EmitOverSeveralFrames(settings, settings.m_Threshold, 0.10f), 0u)
            << "a crest exactly AT the threshold deposits zero foam, so it must spray nothing";
    }

    TEST(WaterSprayTest, HarderFoldsEmitMoreSpray)
    {
        const WS::WaterSpraySettings settings = MakeSettings();
        // Both folds sit ON the ramp — above saturation they would produce the
        // same deposit and the comparison would be between two equal numbers.
        const u32 gentle = EmitOverSeveralFrames(settings, 0.20f, 0.10f);
        const u32 hard = EmitOverSeveralFrames(settings, 0.32f, 0.10f);
        EXPECT_GT(gentle, 0u);
        EXPECT_GT(hard, gentle);
    }

    TEST(WaterSprayTest, DisabledEmitsNothingWhateverTheSeaIsDoing)
    {
        WS::WaterSpraySettings settings = MakeSettings();
        ASSERT_GT(EmitOverSeveralFrames(settings, 0.30f, 0.10f), 0u);
        settings.m_Enabled = false;
        EXPECT_EQ(EmitOverSeveralFrames(settings, 0.30f, 0.10f), 0u);
    }

    // -------------------------------------------------------------------------
    // 2. One crest detector, not two
    // -------------------------------------------------------------------------

    TEST(WaterSprayTest, EmissionTracksTheFoamDeposit)
    {
        // The emitter's criterion IS WaterFoam::DepositFromFold. Walk a range of
        // fold values and require that the emitter is silent exactly where the
        // deposit is zero, and active exactly where it is not — which a second,
        // independently-written crest test could only satisfy by being the same
        // function.
        WS::WaterSpraySettings settings = MakeSettings();
        settings.m_Threshold = 0.20f;
        constexpr f32 kFoamThreshold = 0.12f;
        const f32 effective = WS::EffectiveThreshold(settings.m_Threshold, kFoamThreshold);

        i32 depositsSeen = 0;
        for (i32 i = 0; i <= 20; ++i)
        {
            const f32 fold = static_cast<f32>(i) / 20.0f;
            const f32 deposit = WF::DepositFromFold(fold, effective);
            const u32 emitted = EmitOverSeveralFrames(settings, fold, kFoamThreshold);
            if (deposit <= 0.0f)
            {
                EXPECT_EQ(emitted, 0u) << "fold " << fold << " deposits no foam but sprayed";
            }
            else
            {
                ++depositsSeen;
                EXPECT_GT(emitted, 0u) << "fold " << fold << " deposits foam but did not spray";
            }
        }
        ASSERT_GT(depositsSeen, 0) << "no fold value in the sweep deposited foam — vacuous";
    }

    TEST(WaterSprayTest, TheSprayThresholdIsClampedUpToTheFoamOne)
    {
        // Spray is the LOUDER half of the same phenomenon. A scene that
        // authored spray to fire on gentler crests than foam is deposited on
        // would show droplets flying off water that never went white, which
        // reads as a bug in the foam rather than a choice about spray.
        EXPECT_FLOAT_EQ(WS::EffectiveThreshold(0.1f, 0.5f), 0.5f);
        EXPECT_FLOAT_EQ(WS::EffectiveThreshold(0.7f, 0.25f), 0.7f);

        WS::WaterSpraySettings settings = MakeSettings();
        settings.m_Threshold = 0.1f;
        // fold 0.4 clears the authored spray threshold but not the foam one.
        EXPECT_EQ(EmitOverSeveralFrames(settings, 0.4f, 0.6f), 0u);
        EXPECT_GT(EmitOverSeveralFrames(settings, 0.9f, 0.6f), 0u);
    }

    // -------------------------------------------------------------------------
    // 3. Frame-rate independence and determinism
    // -------------------------------------------------------------------------

    TEST(WaterSprayTest, EmissionIsFrameRateIndependent)
    {
        // One second of 120 Hz must emit roughly what one second of 60 Hz does.
        // Rounding the sub-one expected count UP per frame — the obvious
        // implementation — doubles the spray on a faster machine, which is the
        // same defect the foam's max() combine exists to avoid.
        WS::WaterSpraySettings settings = MakeSettings();
        settings.m_RatePerCell = 3.0f; // deliberately well under one per frame

        auto run = [&settings](f32 dt, i32 steps)
        {
            u32 total = 0;
            for (i32 i = 0; i < steps; ++i)
                total += EmitCount(settings, 0.9f, 0.25f, 5.0f + static_cast<f32>(i) * dt, dt);
            return total;
        };

        const u32 at60 = run(1.0f / 60.0f, 60);
        const u32 at120 = run(1.0f / 120.0f, 120);
        ASSERT_GT(at60, 0u);
        ASSERT_GT(at120, 0u);
        const f32 ratio = static_cast<f32>(at120) / static_cast<f32>(at60);
        EXPECT_GT(ratio, 0.6f) << "120 Hz emitted far less spray than 60 Hz";
        EXPECT_LT(ratio, 1.7f) << "120 Hz emitted far more spray than 60 Hz";
    }

    TEST(WaterSprayTest, EmissionIsDeterministicForTheSameClock)
    {
        // A golden capture of spray is only possible if the same clock produces
        // the same particles. The emitter hashes (cell, time bucket) instead of
        // drawing from an RNG precisely so this holds.
        const WS::WaterSpraySettings settings = MakeSettings();
        const auto a = WS::Emit(settings, 0.25f, { 0.0f, 0.0f }, 12.0f, 1.0f / 60.0f, FlatSea(0.9f));
        const auto b = WS::Emit(settings, 0.25f, { 0.0f, 0.0f }, 12.0f, 1.0f / 60.0f, FlatSea(0.9f));
        ASSERT_FALSE(a.empty());
        ASSERT_EQ(a.size(), b.size());
        for (sizet i = 0; i < a.size(); ++i)
        {
            EXPECT_EQ(a[i].PositionLifetime, b[i].PositionLifetime) << "particle " << i;
            EXPECT_EQ(a[i].VelocityMaxLifetime, b[i].VelocityMaxLifetime) << "particle " << i;
        }
    }

    TEST(WaterSprayTest, SamplePointsAreWorldAnchoredNotCameraRelative)
    {
        // Moving the camera must not re-roll WHICH crests spray. A
        // camera-relative sample pattern makes the whole effect crawl as the
        // camera moves, which is a motion artefact no still frame can show.
        const glm::ivec2 cell{ 4, -9 };
        const glm::vec2 first = WS::SamplePointForCell(cell);
        const glm::vec2 second = WS::SamplePointForCell(cell);
        EXPECT_EQ(first, second);

        // And the cell a given world position falls in does not depend on where
        // the window happens to start.
        i32 axisA = 0;
        i32 axisB = 0;
        const glm::ivec2 minA = WS::CellMinForCentre({ 0.0f, 0.0f }, 20.0f, axisA);
        const glm::ivec2 minB = WS::CellMinForCentre({ 40.0f, 0.0f }, 20.0f, axisB);
        EXPECT_EQ(axisA, axisB);
        EXPECT_NE(minA, minB) << "the window did not follow the camera at all";
        // The lattice itself is anchored: the two windows' corners differ by a
        // whole number of cells, so every shared cell keeps its identity.
        EXPECT_EQ((minB.x - minA.x) * static_cast<i32>(WS::kSampleSpacingMetres),
                  static_cast<i32>(40.0f));
    }

    // -------------------------------------------------------------------------
    // 3b. The seam this file otherwise substitutes away
    // -------------------------------------------------------------------------

    TEST(WaterSprayTest, ARealStormOceanFoldsHardEnoughToClearTheDefaultThresholds)
    {
        // Every test above hands Emit() a CONSTANT fold, which is exactly the
        // substitution docs/agent-rules/substituted-seams-compound.md warns
        // about: it pins what the emitter does with a fold value and says
        // nothing about whether a real sea ever produces one. That gap is not
        // hypothetical — it is where the live pipeline emitted zero particles
        // from a 24 m/s sea while all thirteen substituted tests passed.
        //
        // So build a REAL field, on the CPU path (uploadToGpu = false, so this
        // needs no GL and gates in CI), and measure the fold the emitter will
        // actually see. The numbers in the failure messages are the point: a
        // default threshold above what any sea reaches is a feature that never
        // fires, and nothing else in this file can see that.
        Ocean::SpectrumParams sp;
        sp.m_Resolution = 128u;
        sp.m_PatchSize = 90.0f;
        sp.m_WindSpeed = 24.0f; // a storm, the regime spray exists for
        sp.m_WindDirection = { 1.0f, 0.25f };
        sp.m_Amplitude = 2.0f;
        sp.m_Choppiness = 1.6f; // the pinch that folds the surface at all
        sp.m_Seed = 1337u;
        sp.m_CascadeCount = 1u;

        Ref<Ocean::OceanFFTField> field = Ref<Ocean::OceanFFTField>::Create();
        field->Update(sp, 12.0f, /*uploadToGpu=*/false, /*useGpuCompute=*/false);
        ASSERT_GT(field->GetCascadeCount(), 0u) << "the field built no cascades";

        f32 peakFold = 0.0f;
        f64 foldSum = 0.0;
        i32 samples = 0;
        for (i32 z = 0; z < 128; ++z)
        {
            for (i32 x = 0; x < 128; ++x)
            {
                const glm::vec2 p{ static_cast<f32>(x) * 0.7f, static_cast<f32>(z) * 0.7f };
                const f32 fold = field->SampleCascades(p).Foam;
                peakFold = std::max(peakFold, fold);
                foldSum += fold;
                ++samples;
            }
        }
        const f32 meanFold = static_cast<f32>(foldSum / samples);

        // The defaults a scene gets with no tuning at all.
        const WS::WaterSpraySettings defaults;
        const WaterFoam::WaterFoamSettings foamDefaults;
        const f32 effective =
            WS::EffectiveThreshold(defaults.m_Threshold, foamDefaults.m_DepositThreshold);

        EXPECT_GT(peakFold, effective)
            << "a 24 m/s storm sea peaks at fold " << peakFold << " but the DEFAULT spray "
            << "threshold is " << effective << " — spray can never fire without retuning, which "
            << "is the state the live pipeline was in (mean fold over the patch was "
            << meanFold << ")";

        // ...and the emitter really does produce particles from that field,
        // through the same criterion, with no constant standing in for it.
        WS::WaterSpraySettings settings = MakeSettings();
        settings.m_Threshold = defaults.m_Threshold;
        // Deliberately high: only a small fraction of a storm's cells clear the
        // threshold at any instant, and this test is about whether ANY do — not
        // about the emission rate, which the tests above pin.
        settings.m_RatePerCell = 200.0f;
        settings.m_RadiusMetres = 30.0f;

        auto sampleCrest = [&field](glm::vec2 worldXZ) -> WS::CrestSample
        {
            const Ocean::OceanFFTField::SurfaceSample s = field->SampleCascades(worldXZ);
            return { s.Foam, s.Height, s.Horizontal };
        };

        u32 total = 0;
        for (i32 i = 0; i < 12; ++i)
        {
            total += static_cast<u32>(
                WS::Emit(settings, foamDefaults.m_DepositThreshold, { 0.0f, 0.0f },
                         5.0f + static_cast<f32>(i) / 60.0f, 1.0f / 60.0f, sampleCrest)
                    .size());
        }
        EXPECT_GT(total, 0u) << "no spray from a real storm sea (peak fold " << peakFold << ")";
    }

    // -------------------------------------------------------------------------
    // 4. Bounds and robustness
    // -------------------------------------------------------------------------

    TEST(WaterSprayTest, TheSampleStrideNeverSharesAFactorWithTheRowLength)
    {
        // The cell walk advances a FLAT index over a cellsPerAxis^2 square, so
        // x moves by `stride mod cellsPerAxis` each step. Share a factor and the
        // walk visits `cellsPerAxis / gcd` columns and never the rest — spray
        // then appears in world-anchored stripes with dead gaps between them,
        // which is a directional artefact rather than the even thinning the cap
        // is meant to be.
        //
        // The DEFAULT radius is what degenerated: 38 m gives 39 columns, and
        // the naive `total / cap + 1` gives stride 3 against gcd 3 — a third of
        // the columns, two thirds never sprayed. Sweeping radii is the point;
        // the bug was invisible at 10, 20, 60, 100, 200 and 400.
        //
        // Asserted on the STRIDE rather than on emitted particles: the
        // per-frame particle cap stops the walk long before it wraps, so a
        // count of sampled columns measures the cap and not this.
        i32 degenerateWithNaiveStride = 0;
        for (i32 radiusTenths = 20; radiusTenths <= 4000; radiusTenths += 7)
        {
            const f32 radius = static_cast<f32>(radiusTenths) * 0.1f;
            i32 cellsPerAxis = 0;
            (void)WS::CellMinForCentre({ 0.0f, 0.0f }, radius, cellsPerAxis);
            const i32 totalCells = cellsPerAxis * cellsPerAxis;
            const i32 stride = WS::SampleStride(totalCells, cellsPerAxis);

            EXPECT_EQ(std::gcd(stride, cellsPerAxis), 1)
                << "radius " << radius << ": stride " << stride << " shares a factor with "
                << cellsPerAxis << " columns — the sea would spray in stripes";
            EXPECT_GE(stride, 1);

            // Negative control: confirm the sweep actually contains cases the
            // naive stride would have got wrong, so this cannot pass on a range
            // where the bug never occurs.
            if (totalCells > static_cast<i32>(WS::kMaxSampleCells))
            {
                const i32 naive = totalCells / static_cast<i32>(WS::kMaxSampleCells) + 1;
                if (std::gcd(naive, cellsPerAxis) != 1)
                    ++degenerateWithNaiveStride;
            }
        }
        EXPECT_GT(degenerateWithNaiveStride, 0)
            << "no radius in the sweep degenerates under the naive stride — this test would "
               "pass even with the bug present";
    }

    TEST(WaterSprayTest, EmissionIsCappedPerFrame)
    {
        WS::WaterSpraySettings settings = MakeSettings();
        settings.m_RatePerCell = 100000.0f; // absurd on purpose
        settings.m_RadiusMetres = 200.0f;
        const auto particles =
            WS::Emit(settings, 0.25f, { 0.0f, 0.0f }, 3.0f, 1.0f / 60.0f, FlatSea(1.0f));
        EXPECT_LE(particles.size(), static_cast<sizet>(WS::kMaxEmitPerFrame));
        EXPECT_GT(particles.size(), 0u);
    }

    TEST(WaterSprayTest, NonFiniteCrestSamplesAreSkippedRatherThanEmitted)
    {
        const WS::WaterSpraySettings settings = MakeSettings();
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const auto particles = WS::Emit(settings, 0.25f, { 0.0f, 0.0f }, 3.0f, 1.0f / 60.0f,
                                        [nan](glm::vec2) -> WS::CrestSample
                                        { return { nan, nan, glm::vec2(nan) }; });
        EXPECT_TRUE(particles.empty());
    }

    TEST(WaterSprayTest, ZeroOrNonFiniteDtEmitsNothing)
    {
        const WS::WaterSpraySettings settings = MakeSettings();
        for (const f32 dt : { 0.0f, -1.0f, std::numeric_limits<f32>::quiet_NaN() })
            EXPECT_EQ(EmitCount(settings, 0.95f, 0.25f, 3.0f, dt), 0u) << "dt = " << dt;
    }

    TEST(WaterSprayTest, ParticlesLaunchUpwardFromTheCrestAndAreAlive)
    {
        WS::WaterSpraySettings settings = MakeSettings();
        settings.m_WaterPlaneY = 7.0f;
        constexpr f32 kCrestHeight = 1.25f;
        const auto particles = WS::Emit(settings, 0.25f, { 0.0f, 0.0f }, 3.0f, 1.0f / 60.0f,
                                        FlatSea(0.95f, kCrestHeight));
        ASSERT_FALSE(particles.empty());
        for (const GPUParticle& p : particles)
        {
            // Above the crest it came off, not below it and not at the plane —
            // and clear of it by the margin the band-limited CPU proxy's
            // under-reported crest height demands (kSpawnClearanceMetres).
            EXPECT_GT(p.PositionLifetime.y,
                      settings.m_WaterPlaneY + kCrestHeight + WS::kSpawnClearanceMetres)
                << "a droplet spawned inside the surface it came off";
            EXPECT_GT(p.VelocityMaxLifetime.y, 0.0f) << "spray must be thrown UP";
            EXPECT_GT(p.PositionLifetime.w, 0.0f) << "a zero-lifetime particle dies on frame 1";
            EXPECT_FLOAT_EQ(p.Misc.z, 1.0f) << "the particle was not marked alive";
            EXPECT_GT(p.Misc.x, 0.0f) << "a zero-size particle draws nothing";
        }
    }

    TEST(WaterSprayTest, SprayIsThrownDownwind)
    {
        WS::WaterSpraySettings settings = MakeSettings();
        settings.m_WindMetresPerSecond = { 12.0f, 0.0f };
        const auto particles =
            WS::Emit(settings, 0.25f, { 0.0f, 0.0f }, 3.0f, 1.0f / 60.0f, FlatSea(0.95f));
        ASSERT_FALSE(particles.empty());

        f32 meanX = 0.0f;
        for (const GPUParticle& p : particles)
            meanX += p.VelocityMaxLifetime.x;
        meanX /= static_cast<f32>(particles.size());
        EXPECT_GT(meanX, 0.0f) << "spray was not carried downwind";
    }
} // namespace OloEngine::Tests
