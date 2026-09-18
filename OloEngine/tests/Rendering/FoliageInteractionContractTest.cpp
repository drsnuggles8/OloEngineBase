// OLO_TEST_LAYER: L1
//
// The CPU half of issue #1238. Everything here is a property of the FIELD and
// of the bound it promises — no GPU, no scene, no renderer — so a regression
// shows up as an arithmetic failure with a number in it rather than as a frame
// that looks slightly wrong.
//
// The load-bearing test is FrameRateIndependence: it is the one that would fail
// if anyone ever replaced the analytic spring with a per-frame lerp, which is
// the specific failure the second acceptance criterion names.
#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include "OloEngine/Terrain/Foliage/FoliageInstanceRegistry.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        FoliageInteractionSource MakeSource(u64 id, const glm::vec3& position)
        {
            FoliageInteractionSource source;
            source.Id = id;
            source.Position = position;
            source.Radius = 2.0f;
            source.Height = 2.0f;
            source.Strength = 1.0f;
            source.Falloff = 2.0f;
            source.RecoverySeconds = 0.5f;
            return source;
        }

        void Step(const std::vector<FoliageInteractionSource>& sources, f32 dt)
        {
            FoliageInteractionField::Update(std::span<const FoliageInteractionSource>(sources), dt);
        }

        // Sum of |push| over every live slot — the field's total "how bent is
        // the world right now", which is what every teardown assertion reads.
        f32 TotalPush()
        {
            const auto gpu = FoliageInteractionField::GetGPUData();
            f32 total = 0.0f;
            for (u32 i = 0; i < static_cast<u32>(gpu.Params.x); ++i)
                total += std::abs(gpu.Slots[i].Push.x) + std::abs(gpu.Slots[i].Push.y) +
                         std::abs(gpu.Slots[i].Push.z);
            return total;
        }

        struct FieldReset
        {
            FieldReset()
            {
                FoliageInteractionField::Reset();
            }
            ~FieldReset()
            {
                FoliageInteractionField::Reset();
            }
        };
    } // namespace

    // THE contract. A relaxation expressed in seconds must land in the same
    // place however finely the clock is sliced — including across a spike,
    // which is where a per-frame lerp both diverges AND rings.
    TEST(FoliageInteractionContract, FrameRateIndependenceHoldsAcrossCadencesAndASpike)
    {
        const glm::vec3 target(0.0f);
        constexpr f32 omega = 4.0f;    // recovery time constant of 0.25 s
        constexpr f32 duration = 0.5f; // half a second of recovery, however it is diced

        const auto integrate = [&](std::span<const f32> steps)
        {
            FoliageSpringState state;
            state.Value = glm::vec3(0.8f, -0.3f, 1.0f);
            state.Velocity = glm::vec3(0.1f, 0.0f, -0.2f);
            for (const f32 dt : steps)
                FoliageSpringStep(state, target, omega, dt);
            return state;
        };

        const std::array<f32, 1> one{ duration };
        const auto reference = integrate(one);

        for (const u32 slices : { 2u, 12u, 120u, 1200u })
        {
            const std::vector<f32> steps(slices, duration / static_cast<f32>(slices));
            const auto got = integrate(steps);
            SCOPED_TRACE(::testing::Message() << "slices=" << slices);
            EXPECT_NEAR(got.Value.x, reference.Value.x, 2e-4f);
            EXPECT_NEAR(got.Value.y, reference.Value.y, 2e-4f);
            EXPECT_NEAR(got.Value.z, reference.Value.z, 2e-4f);
            EXPECT_NEAR(got.Velocity.z, reference.Velocity.z, 2e-3f);
        }

        // A spike in the middle changes nothing — the exactness is per step,
        // not per schedule. 0.4 + 0.02 x 5 is the same half second.
        const std::vector<f32> spiky{ 0.4f, 0.02f, 0.02f, 0.02f, 0.02f, 0.02f };
        const auto spiked = integrate(spiky);
        EXPECT_NEAR(spiked.Value.z, reference.Value.z, 2e-4f);
    }

    TEST(FoliageInteractionContract, RecoveryNeverOvershootsOrOscillates)
    {
        // Critical damping has a real repeated root, so a release from rest is
        // monotone. Asserting MONOTONE rather than "small" is the point: an
        // under-damped spring passes a magnitude bound and still visibly rings.
        constexpr f32 omega = 3.0f;
        // A fixed DURATION, not a fixed step count — six seconds at this rate
        // is e^-18 (times the 1 + 18 the critically damped form carries), which
        // is far below the convergence bound below. Counting steps instead
        // would only test the 240 Hz arm for a quarter of the time it tests the
        // 12 Hz one, and the assertion would then measure the cadence rather
        // than the spring.
        constexpr f32 duration = 6.0f;
        for (const f32 dt : { 1.0f / 240.0f, 1.0f / 60.0f, 1.0f / 12.0f })
        {
            FoliageSpringState state;
            state.Value = glm::vec3(0.0f, 0.0f, 1.0f);
            f32 previous = state.Value.z;
            const auto steps = static_cast<u32>(std::lround(duration / dt));
            for (u32 i = 0; i < steps; ++i)
            {
                FoliageSpringStep(state, glm::vec3(0.0f), omega, dt);
                SCOPED_TRACE(::testing::Message() << "dt=" << dt << " step=" << i);
                ASSERT_LE(state.Value.z, previous + 1e-6f) << "recovery grew — the spring rang";
                ASSERT_GE(state.Value.z, -1e-6f) << "recovery crossed zero — the spring overshot";
                previous = state.Value.z;
            }
            EXPECT_LT(state.Value.z, 1e-4f) << "did not converge after " << duration << " s at dt=" << dt;
        }
    }

    TEST(FoliageInteractionContract, NonFiniteStepsAndRatesLeaveTheSpringUntouched)
    {
        constexpr f32 nan = std::numeric_limits<f32>::quiet_NaN();
        constexpr f32 inf = std::numeric_limits<f32>::infinity();
        for (const f32 dt : { nan, inf, -1.0f, 0.0f })
        {
            FoliageSpringState state;
            state.Value = glm::vec3(0.5f);
            FoliageSpringStep(state, glm::vec3(0.0f), 2.0f, dt);
            EXPECT_TRUE(Math::BitwiseEqual(state.Value, glm::vec3(0.5f))) << "dt=" << dt;
        }
        for (const f32 omega : { nan, -3.0f, 0.0f })
        {
            FoliageSpringState state;
            state.Value = glm::vec3(0.5f);
            FoliageSpringStep(state, glm::vec3(0.0f), omega, 0.01f);
            EXPECT_TRUE(Math::BitwiseEqual(state.Value, glm::vec3(0.5f))) << "omega=" << omega;
        }
    }

    // The bound the culler rests on, stated as the shader states it: the summed
    // push is clamped to FoliageInteractionMaximumDisplacement(response), so the
    // padded AABB contains the bent plant for ANY influence count.
    TEST(FoliageInteractionContract, PaddedBoundsContainTheClampedPush)
    {
        for (const f32 response : { 0.0f, 0.25f, 1.0f, 4.0f })
        {
            const f32 bound = FoliageInteractionMaximumDisplacement(response);
            EXPECT_LE(bound, response * kFoliageInteractionMaxStrength + 1e-5f);

            FoliageBoundsProfile profile;
            profile.m_InteractionDisplacement = bound;
            const auto box = FoliageInstanceBounds({ 5.0f, 1.0f, -2.0f }, 2.0f, 3.0f, profile);
            // A blade tip at the top of its rest bound, pushed the full bound
            // horizontally, must still be inside the box.
            EXPECT_LE(5.0f + 1.0f + bound, box.Max.x + 1e-5f);
            EXPECT_GE(-2.0f - 1.0f - bound, box.Min.z - 1e-5f);
        }
        EXPECT_FLOAT_EQ(FoliageInteractionMaximumDisplacement(std::numeric_limits<f32>::quiet_NaN()), 0.0f);
    }

    TEST(FoliageInteractionContract, AnEmptyFieldPublishesExactlyNothing)
    {
        FieldReset guard;
        Step({}, 1.0f / 60.0f);
        EXPECT_EQ(FoliageInteractionField::GetActiveCount(), 0u);
        const auto gpu = FoliageInteractionField::GetGPUData();
        EXPECT_FLOAT_EQ(gpu.Params.x, 0.0f);
        for (const auto& slot : gpu.Slots)
            EXPECT_TRUE(Math::BitwiseEqual(slot.Push, glm::vec4(0.0f)));
    }

    TEST(FoliageInteractionContract, RemovingASourceRetiresItsSlotInFiniteTime)
    {
        FieldReset guard;
        const std::vector<FoliageInteractionSource> sources{ MakeSource(7, { 0.0f, 0.0f, 0.0f }) };
        for (u32 i = 0; i < 30; ++i)
            Step(sources, 1.0f / 60.0f);
        ASSERT_EQ(FoliageInteractionField::GetActiveCount(), 1u);
        ASSERT_GT(TotalPush(), 0.1f);

        // The actor is gone. Not "fades below a threshold" — RETIRED, so the
        // count returns to zero and the shader's contribution is exactly 0.
        u32 frames = 0;
        while (FoliageInteractionField::GetActiveCount() > 0 && frames < 2000)
        {
            Step({}, 1.0f / 60.0f);
            ++frames;
        }
        EXPECT_EQ(FoliageInteractionField::GetActiveCount(), 0u);
        EXPECT_LT(frames, 2000u) << "the field never let go";
        EXPECT_FLOAT_EQ(TotalPush(), 0.0f);
    }

    TEST(FoliageInteractionContract, ResetLeavesNoResidualAfterATeleport)
    {
        FieldReset guard;
        std::vector<FoliageInteractionSource> sources{ MakeSource(11, { 0.0f, 0.0f, 0.0f }) };
        for (u32 i = 0; i < 30; ++i)
            Step(sources, 1.0f / 60.0f);

        // A teleport sheds the old influence rather than dragging it: the grass
        // that was stood in recovers where it is, and the destination gets a
        // fresh influence with no history (so nothing streaks between them).
        sources[0].Position = glm::vec3(500.0f, 0.0f, 500.0f);
        Step(sources, 1.0f / 60.0f);
        EXPECT_GE(FoliageInteractionField::GetActiveCount(), 2u) << "the teleport dragged its influence";
        const auto gpu = FoliageInteractionField::GetGPUData();
        for (u32 i = 0; i < static_cast<u32>(gpu.Params.x); ++i)
        {
            SCOPED_TRACE(::testing::Message() << "slot=" << i);
            EXPECT_TRUE(Math::BitwiseEqual(glm::vec3(gpu.Slots[i].Center), glm::vec3(gpu.Slots[i].PrevCenter)) ||
                        glm::length(glm::vec3(gpu.Slots[i].Center) - glm::vec3(gpu.Slots[i].PrevCenter)) < 1.0f)
                << "an influence reprojects across the whole jump";
        }

        // A region unload / scene change drops everything at once, including
        // residuals whose absolute centres no longer mean anything.
        FoliageInteractionField::Reset();
        EXPECT_EQ(FoliageInteractionField::GetActiveCount(), 0u);
        EXPECT_FLOAT_EQ(TotalPush(), 0.0f);
    }

    TEST(FoliageInteractionContract, TheFieldIsBoundedByItsSlotCountAndRejectsHostileInput)
    {
        FieldReset guard;
        std::vector<FoliageInteractionSource> sources;
        for (u64 id = 1; id <= kFoliageInteractionSlots * 4; ++id)
            sources.push_back(MakeSource(id, { static_cast<f32>(id) * 10.0f, 0.0f, 0.0f }));
        for (u32 i = 0; i < 10; ++i)
            Step(sources, 1.0f / 60.0f);
        EXPECT_LE(FoliageInteractionField::GetActiveCount(), kFoliageInteractionSlots);

        FoliageInteractionField::Reset();
        constexpr f32 nan = std::numeric_limits<f32>::quiet_NaN();
        auto hostile = MakeSource(1, { nan, nan, nan });
        std::vector<FoliageInteractionSource> poison{ hostile };
        Step(poison, 1.0f / 60.0f);
        EXPECT_EQ(FoliageInteractionField::GetActiveCount(), 0u) << "a NaN position reached the field";

        poison[0] = MakeSource(2, { 0.0f, 0.0f, 0.0f });
        poison[0].Radius = nan;
        Step(poison, 1.0f / 60.0f);
        EXPECT_EQ(FoliageInteractionField::GetActiveCount(), 0u) << "a NaN radius reached the field";

        // A strength far past the cap is SATURATED, not rejected — it is a
        // continuous quantity, and the cap is what the bound is derived from.
        poison[0] = MakeSource(3, { 0.0f, 0.0f, 0.0f });
        poison[0].Strength = 1e9f;
        for (u32 i = 0; i < 60; ++i)
            Step(poison, 1.0f / 60.0f);
        const auto gpu = FoliageInteractionField::GetGPUData();
        ASSERT_GE(gpu.Params.x, 1.0f);
        EXPECT_LE(std::abs(gpu.Slots[0].Push.z), kFoliageInteractionMaxStrength + 1e-4f);
    }

    // The ray-traced vegetation cache derives its proxy age limit from
    // GetMaximumBendRate, so a rate that read zero for the first frame of an
    // influence would let it serve a snapshot taken just before the grass
    // started moving. It cannot: Update steps a slot in the same call that
    // allocates it, so the attack velocity is already there.
    TEST(FoliageInteractionContract, ANewInfluenceReportsItsAttackRateImmediately)
    {
        FieldReset guard;
        EXPECT_FLOAT_EQ(FoliageInteractionField::GetMaximumBendRate(), 0.0f) << "an empty field has no rate";

        const std::vector<FoliageInteractionSource> sources{ MakeSource(21, { 0.0f, 0.0f, 0.0f }) };
        Step(sources, 1.0f / 60.0f); // the very first frame this actor exists
        EXPECT_GT(FoliageInteractionField::GetMaximumBendRate(), 0.5f)
            << "a brand-new influence reported a near-zero bend rate on its first frame";

        // And it returns to zero once the field lets go, so a still scene does
        // not pay for refits it does not need.
        for (u32 i = 0; i < 2000 && FoliageInteractionField::GetActiveCount() > 0; ++i)
            Step({}, 1.0f / 60.0f);
        EXPECT_FLOAT_EQ(FoliageInteractionField::GetMaximumBendRate(), 0.0f);
    }

    TEST(FoliageInteractionContract, AWalkingActorShedsATrailThatRecoversBehindIt)
    {
        FieldReset guard;
        std::vector<FoliageInteractionSource> sources{ MakeSource(5, { 0.0f, 0.0f, 0.0f }) };
        sources[0].TrailSpacing = 0.5f;
        sources[0].RecoverySeconds = 0.4f;

        // Walk two metres in twenty frames at 60 Hz — 6 m/s, well past the
        // reference speed, so the directional lean is saturated.
        for (u32 i = 0; i < 20; ++i)
        {
            sources[0].Position.x += 0.1f;
            Step(sources, 1.0f / 60.0f);
        }
        EXPECT_GT(FoliageInteractionField::GetActiveCount(), 1u) << "no trail was shed";

        const auto gpu = FoliageInteractionField::GetGPUData();
        bool leaning = false;
        for (u32 i = 0; i < static_cast<u32>(gpu.Params.x); ++i)
            if (std::abs(gpu.Slots[i].Push.x) > 0.05f)
                leaning = true;
        EXPECT_TRUE(leaning) << "a running actor produced no directional push";

        // And the trail is transient like everything else here.
        u32 frames = 0;
        while (FoliageInteractionField::GetActiveCount() > 0 && frames < 2000)
        {
            Step({}, 1.0f / 60.0f);
            ++frames;
        }
        EXPECT_EQ(FoliageInteractionField::GetActiveCount(), 0u);
    }
} // namespace OloEngine::Tests
