#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// AnimalSchedulerContractTest — issue #1258, the population budget's contracts.
//
// The census (AnimalSchedulingCensusTest) decides WHAT the work costs. This
// file pins the rules that a single frame cannot show, because every one of the
// three failure modes the issue names is a property of a SEQUENCE:
//
//   * starvation is "below desired for too many CONSECUTIVE frames";
//   * an abrupt motion change is a step that moved too far BETWEEN frames;
//   * an invisible coat is one the budget thinned past a floor, which only
//     happens once the population is over-subscribed.
//
// WHY THESE CASES RUN A POPULATION FOR MANY FRAMES. A fairness rule asserted on
// one frame is not asserted at all: every allocator passes "the hero got full
// rate once". The cases below run a fixed population for a bounded number of
// frames and assert on the WHOLE history — the maximum starvation streak, the
// largest single-frame step change, the minimum strand count any visible coat
// was ever built with. That is the only shape in which "does not starve" is a
// statement with a truth value.
//
// AND THE SWEEPS ARE SWEEPS, NOT SAMPLES. GroomLodContractTest's rule: a
// monotonicity or a bound checked at three hand-picked points is a coincidence
// detector. Where the claim is "for every step" or "for every population size",
// the loop covers the range.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Scene/AnimalScheduler.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <ranges>
#include <unordered_map>
#include <vector>

using namespace OloEngine;

namespace fs = std::filesystem;

namespace
{
    constexpr sizet kDeform = static_cast<sizet>(AnimalWorkAxis::Deformation);
    constexpr sizet kSim = static_cast<sizet>(AnimalWorkAxis::Simulation);
    constexpr sizet kVis = static_cast<sizet>(AnimalWorkAxis::Visibility);
    constexpr sizet kShadow = static_cast<sizet>(AnimalWorkAxis::Shadow);

    // One animal, priced like the long-coated reference: a real bone count, a
    // real guide count and a real strand count, so the budgets that bind in
    // these cases are the ones that bind in the scene.
    [[nodiscard]] AnimalWorkItem MakeAnimal(u64 id, AnimalRole role, f32 pixelSize, u32 strands = 12000u)
    {
        AnimalWorkItem item;
        item.Id = UUID{ id };
        item.Role = role;
        item.PixelSize = pixelSize;
        item.BoneCount = 64u;
        item.GuidePointCount = 1200u;
        item.StrandCount = strands;
        item.ShadowVoxelCount = 64u * 64u * 64u;
        item.DrawCallCount = 2u;
        item.SimulationSubsteps = 1u;
        item.Visible = true;
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            item.DesiredStep[a] = 0u;
            item.MaxStep[a] = 4u;
        }
        return item;
    }

    /// Drives a population for `frames` frames against one policy, keeping the
    /// per-animal state across frames the way Scene does.
    class PopulationRun
    {
      public:
        PopulationRun(std::vector<AnimalWorkItem> items, AnimalBudgetPolicy policy)
            : m_Items(std::move(items)), m_Policy(policy)
        {
            for (const AnimalWorkItem& item : m_Items)
            {
                m_States[static_cast<u64>(item.Id)] = AnimalScheduleState{};
            }
        }

        TArray<AnimalSchedule> Step(AnimalSchedulerStats* outStats = nullptr)
        {
            std::vector<AnimalScheduleSlot> slots;
            slots.reserve(m_Items.size());
            for (const AnimalWorkItem& item : m_Items)
            {
                AnimalScheduleSlot slot;
                slot.Item = item;
                slot.State = &m_States[static_cast<u64>(item.Id)];
                slots.push_back(slot);
            }
            return ScheduleAnimalPopulation(m_Policy, AnimalCostModel{}, slots, outStats);
        }

        [[nodiscard]] AnimalScheduleState& StateOf(u64 id)
        {
            return m_States[id];
        }
        [[nodiscard]] std::vector<AnimalWorkItem>& Items()
        {
            return m_Items;
        }

      private:
        std::vector<AnimalWorkItem> m_Items;
        AnimalBudgetPolicy m_Policy;
        std::unordered_map<u64, AnimalScheduleState> m_States;
    };

    /// A population large enough that the default budget cannot serve it.
    [[nodiscard]] std::vector<AnimalWorkItem> OverSubscribedHerd(u32 count, bool withHero = true)
    {
        std::vector<AnimalWorkItem> items;
        items.reserve(count);
        if (withHero)
        {
            items.push_back(MakeAnimal(1u, AnimalRole::Hero, 480.0f));
        }
        for (u32 i = 0; i < count; ++i)
        {
            // Distinct apparent sizes so the tie-break is exercised but not
            // relied on, and distinct ids so the order is total.
            items.push_back(MakeAnimal(100u + i, AnimalRole::Background, 120.0f + static_cast<f32>(i)));
        }
        return items;
    }

    /// Everything on `line` that the compiler would see, with `//` comments and
    /// string literals removed. Enough for a source scan over one known file:
    /// it is not a C++ lexer, it just refuses to let a token inside a comment
    /// or a log message stand in for the statement the scan is looking for.
    [[nodiscard]] std::string StripNonCode(std::string_view line)
    {
        std::string code;
        code.reserve(line.size());
        bool inString = false;
        for (sizet i = 0; i < line.size(); ++i)
        {
            const char c = line[i];
            if (!inString && c == '/' && i + 1 < line.size() && line[i + 1] == '/')
            {
                break;
            }
            if (c == '"' && (i == 0 || line[i - 1] != '\\'))
            {
                inString = !inString;
                continue;
            }
            if (!inString)
            {
                code.push_back(c);
            }
        }
        return code;
    }

    [[nodiscard]] AnimalBudgetPolicy TightPolicy()
    {
        AnimalBudgetPolicy policy;
        policy.Enabled = true;
        policy.ProtectHero = true;
        policy.FrameBudgetUnits = 20000.0f;
        policy.HoldFrames = 0u; // the hold is asserted separately; here it must not mask the allocation
        policy.StarvationFrames = 8u;
        return policy;
    }
} // namespace

// -----------------------------------------------------------------------------
// The disabled path is the A/B control, and it must be the old picture exactly
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerPolicy, DisabledPassesEveryAnimalThroughAtItsOwnDesiredStep)
{
    AnimalBudgetPolicy policy; // Enabled defaults to false
    std::vector<AnimalWorkItem> items = OverSubscribedHerd(40u);
    // Give the herd a distance-derived step so "pass-through" is distinguishable
    // from "full rate" — the two differ exactly when the ladder already coarsened.
    for (AnimalWorkItem& item : items)
    {
        item.DesiredStep[kVis] = 2u;
    }

    PopulationRun run(items, policy);
    const TArray<AnimalSchedule> schedules = run.Step();

    ASSERT_EQ(static_cast<sizet>(schedules.Num()), items.size());
    for (const AnimalSchedule& schedule : schedules)
    {
        EXPECT_EQ(schedule.Outcome, AnimalBudgetOutcome::NotScheduled);
        EXPECT_EQ(schedule.Step[kVis], 2u) << "the population budget is off, so each animal keeps its own ladder's step";
        EXPECT_EQ(schedule.Step[kDeform], 0u);
    }
}

TEST(AnimalSchedulerPolicy, DisablingResetsTheStateSoTheControlIsTheSamePictureEveryTime)
{
    // The A/B control for every capture in this issue is "turn the budget off".
    // If the counters survived the toggle, the control frame would depend on
    // how long the budget had been on before it, which is not a control.
    PopulationRun run(OverSubscribedHerd(60u), TightPolicy());
    for (u32 frame = 0; frame < 20u; ++frame)
    {
        run.Step();
    }
    ASSERT_GT(run.StateOf(100u).StarvedFrames[kVis] + run.StateOf(100u).Step[kVis], 0u)
        << "the pressure case must actually have coarsened something, or this asserts nothing";

    AnimalBudgetPolicy off = TightPolicy();
    off.Enabled = false;
    PopulationRun offRun(OverSubscribedHerd(60u), off);
    offRun.StateOf(100u).Step[kVis] = 3u;
    offRun.StateOf(100u).StarvedFrames[kVis] = 7u;
    offRun.Step();

    EXPECT_EQ(offRun.StateOf(100u).Step[kVis], 0u);
    EXPECT_EQ(offRun.StateOf(100u).StarvedFrames[kVis], 0u);
}

// -----------------------------------------------------------------------------
// Failure mode 1 — starvation
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerStarvation, NoAnimalIsPassedOverWhileAPeerSitsAtItsDesiredStep)
{
    // THE FAIRNESS BOUND IS RELATIVE, AND SAYING SO PRECISELY IS THE POINT OF
    // THIS CASE. When the whole population has to be coarsened there is nobody
    // to swap with, so an absolute "no animal is below its desired step for
    // more than N frames" is a promise NO allocator can keep at any budget
    // small enough to matter — the first version of this test asserted exactly
    // that and failed at 200 frames against a correct scheduler.
    //
    // What the service order actually guarantees is that the loss ROTATES: an
    // animal held below its desired step while a peer of the same role sits AT
    // its desired step is being passed over, and the starvation counter puts it
    // at the front of the queue until it is not. That is the property below,
    // and it is the one the policy field's own documentation states.
    AnimalBudgetPolicy policy = TightPolicy();
    policy.StarvationFrames = 8u;

    // A budget that can serve SOME of the herd at full rate but not all of it,
    // which is the only condition under which "passed over while a peer was
    // served" is even a meaningful accusation.
    policy.FrameBudgetUnits = 30000.0f;

    PopulationRun run(OverSubscribedHerd(48u), policy);

    std::unordered_map<u64, std::array<u32, AnimalWorkAxisCount>> streak;
    u32 framesWhereSomeonePeerWasServed = 0u;
    u32 worstStreakWhileAPeerWasServed = 0u;

    for (u32 frame = 0; frame < 200u; ++frame)
    {
        const TArray<AnimalSchedule> schedules = run.Step();

        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            bool anyPeerAtDesired = false;
            for (const AnimalSchedule& schedule : schedules)
            {
                if (schedule.Role == AnimalRole::Background && schedule.Step[a] == 0u)
                {
                    anyPeerAtDesired = true;
                    break;
                }
            }

            for (const AnimalSchedule& schedule : schedules)
            {
                if (schedule.Role != AnimalRole::Background)
                {
                    continue;
                }
                const u64 id = static_cast<u64>(schedule.Id);
                // DesiredStep is 0 for every animal in this fixture, so "below
                // desired" is "step > 0".
                streak[id][a] = (schedule.Step[a] > 0u) ? streak[id][a] + 1u : 0u;

                if (anyPeerAtDesired)
                {
                    worstStreakWhileAPeerWasServed = std::max(worstStreakWhileAPeerWasServed, streak[id][a]);
                    EXPECT_LE(streak[id][a], policy.StarvationFrames)
                        << "frame " << frame << " axis " << ToString(static_cast<AnimalWorkAxis>(a)) << " animal "
                        << id
                        << ": held below its desired step for longer than the fairness bound while a peer of the "
                           "same role sat at its own — the starvation counter is not reaching the service order";
                }
            }
            if (anyPeerAtDesired)
            {
                ++framesWhereSomeonePeerWasServed;
            }
        }
    }

    // THE CASE MUST ACTUALLY HAVE BEEN IN THE INTERESTING REGIME. Without this
    // the assertion above is vacuous whenever the budget happened to serve
    // everybody or nobody, and a vacuous fairness test is worse than none.
    EXPECT_GT(framesWhereSomeonePeerWasServed, 0u)
        << "no frame had a served peer alongside a coarsened one, so nothing was ever 'passed over' and the "
           "fairness assertion checked nothing";
    EXPECT_GT(worstStreakWhileAPeerWasServed, 0u) << "nothing was coarsened at all while a peer was served";
}

TEST(AnimalSchedulerStarvation, TheWorstStreakCounterReportsPressureRatherThanUnfairness)
{
    // The counter's meaning, pinned so the panel's reading of it stays honest.
    // Under a budget that cannot serve ANYBODY at their desired step, every
    // animal is below desired every frame and MaxStarvedFrames grows without
    // bound — correctly. It is a measure of PRESSURE, not of unfairness, and a
    // reader who takes a large value as a fairness bug will go looking for a
    // scheduler defect that is not there.
    AnimalBudgetPolicy policy = TightPolicy();
    policy.FrameBudgetUnits = 500.0f; // hopeless for this herd

    PopulationRun run(OverSubscribedHerd(64u), policy);
    AnimalSchedulerStats stats;
    for (u32 frame = 0; frame < 40u; ++frame)
    {
        run.Step(&stats);
    }

    EXPECT_GT(stats.MaxStarvedFrames, policy.StarvationFrames)
        << "a population nobody can serve should show a large streak, and that is not a bug";
    EXPECT_TRUE(stats.BudgetExceeded) << "and it should be accompanied by the flag that explains it";
}

TEST(AnimalSchedulerStarvation, TheLongestStarvedAnimalIsServedBeforeALessStarvedOne)
{
    // The mechanism, isolated from the sequence: two identical background
    // animals, one already starved. The un-starved one must give way.
    std::vector<AnimalWorkItem> items;
    items.push_back(MakeAnimal(10u, AnimalRole::Background, 200.0f));
    items.push_back(MakeAnimal(11u, AnimalRole::Background, 200.0f));

    AnimalBudgetPolicy policy = TightPolicy();
    // A budget that can afford exactly one of the two at full visibility.
    policy.FrameBudgetUnits = 400.0f;

    PopulationRun run(items, policy);
    run.StateOf(10u).StarvedFrames[kVis] = 5u;
    run.StateOf(11u).StarvedFrames[kVis] = 0u;

    const TArray<AnimalSchedule> schedules = run.Step();
    ASSERT_EQ(static_cast<sizet>(schedules.Num()), 2u);

    const AnimalSchedule& starved = schedules[0].Id == UUID{ 10u } ? schedules[0] : schedules[1];
    const AnimalSchedule& fresh = schedules[0].Id == UUID{ 10u } ? schedules[1] : schedules[0];

    EXPECT_LE(starved.Step[kVis], fresh.Step[kVis])
        << "the animal that has been starved longer must not be the one coarsened again";
}

// -----------------------------------------------------------------------------
// Failure mode 2 — abrupt motion changes
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerStability, AStepNeverCoarsensByMoreThanOneHalvingInOneFrame)
{
    // A three-step coarsening applied in one frame is a factor-of-eight change
    // in update rate between two consecutive frames, which is exactly the
    // discontinuity the criterion names. The rate limit is asserted over a
    // SEQUENCE and over every axis, because the obvious implementation
    // (`state.Step = allocated`) passes every steady-state check.
    AnimalBudgetPolicy policy = TightPolicy();
    policy.HoldFrames = 0u; // no hold at all: the rate limit must hold on its own

    PopulationRun run(OverSubscribedHerd(64u), policy);

    std::unordered_map<u64, std::array<u32, AnimalWorkAxisCount>> previous;
    u32 largestJump = 0u;
    for (u32 frame = 0; frame < 60u; ++frame)
    {
        for (const AnimalSchedule& schedule : run.Step())
        {
            const u64 id = static_cast<u64>(schedule.Id);
            if (const auto it = previous.find(id); it != previous.end())
            {
                for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
                {
                    if (schedule.Step[a] > it->second[a])
                    {
                        largestJump = std::max(largestJump, schedule.Step[a] - it->second[a]);
                    }
                }
            }
            previous[id] = schedule.Step;
        }
    }

    EXPECT_GT(largestJump, 0u) << "nothing coarsened at all, so the bound is vacuous";
    EXPECT_EQ(largestJump, 1u) << "a step coarsened by more than one halving in a single frame";
}

TEST(AnimalSchedulerStability, TheHoldBoundsHowOftenAStepMayCoarsen)
{
    // The second anti-thrash mechanism, stated as the failure it prevents: with
    // a hold of H, a coarsening needs H consecutive frames of being asked for,
    // so N frames of sustained pressure produce at most N / (H + 1) steps.
    constexpr u32 kHold = 4u;
    constexpr u32 kFrames = 60u;

    AnimalBudgetPolicy policy = TightPolicy();
    policy.HoldFrames = kHold;

    PopulationRun run(OverSubscribedHerd(64u), policy);
    u32 previousStep = 0u;
    u32 coarsenings = 0u;
    for (u32 frame = 0; frame < kFrames; ++frame)
    {
        for (const AnimalSchedule& schedule : run.Step())
        {
            if (static_cast<u64>(schedule.Id) != 100u)
            {
                continue;
            }
            if (schedule.Step[kVis] > previousStep)
            {
                ++coarsenings;
            }
            previousStep = schedule.Step[kVis];
        }
    }

    EXPECT_LE(coarsenings, kFrames / (kHold + 1u))
        << "a coarsening was taken more often than the hold window allows";
}

TEST(AnimalSchedulerPoseBound, TheDeformationCapIsAScreenSpaceBoundAndNotADistance)
{
    // Swept, not sampled. The claim is "for every motion, the chosen step keeps
    // the pose displacement under the bound", and a bound checked at three
    // hand-picked motions is a coincidence detector.
    constexpr f32 kBound = 1.0f;
    for (u32 i = 1; i <= 200u; ++i)
    {
        const f32 motion = static_cast<f32>(i) * 0.01f; // 0.01 .. 2.0 px per full-rate frame
        const u32 step = MaxDeformationStepForPoseBound(motion, kBound, 6u);
        const f32 displacement = motion * static_cast<f32>(1u << step);

        if (motion > kBound)
        {
            // ALREADY OVER THE BOUND AT FULL RATE, so there is no step that
            // satisfies it and the honest answer is to refuse any reduction.
            // The bound is a constraint on what the BUDGET may do, not a
            // promise the scheduler can keep about an animal that was already
            // moving faster than a pixel a frame — asserting it here would be
            // asserting something no implementation could deliver.
            EXPECT_EQ(step, 0u) << "motion=" << motion << ": reducing the rate can only make this worse";
            continue;
        }

        EXPECT_LE(displacement, kBound + 1e-4f)
            << "motion=" << motion << " step=" << step << ": the chosen rate steps the pose past the bound";

        if (step < 6u)
        {
            // And it is the LARGEST such step: one more must break the bound,
            // or the cap is leaving free performance on the table at every
            // distance and the whole axis under-delivers silently.
            const f32 nextDisplacement = motion * static_cast<f32>(1u << (step + 1u));
            EXPECT_GT(nextDisplacement, kBound) << "motion=" << motion << ": a coarser step would still have fit";
        }
    }
}

TEST(AnimalSchedulerPoseBound, AnAnimalAlreadyOverTheBoundAtFullRateIsNeverReduced)
{
    // Reducing the rate can only make an over-budget pose step worse, so the
    // honest answer is to refuse — and to be counted, so the budget's inability
    // to touch this animal is visible rather than mysterious.
    EXPECT_EQ(MaxDeformationStepForPoseBound(4.0f, 1.0f, 6u), 0u);
    EXPECT_EQ(MaxDeformationStepForPoseBound(1.0001f, 1.0f, 6u), 0u);
    // A still animal's pose does not move whatever the rate, so nothing binds.
    EXPECT_EQ(MaxDeformationStepForPoseBound(0.0f, 1.0f, 6u), 6u);
    // A bound of zero is an author saying "no pose step is acceptable".
    EXPECT_EQ(MaxDeformationStepForPoseBound(0.5f, 0.0f, 6u), 0u);
    // Non-finite input takes the safe answer, not the permissive one.
    EXPECT_EQ(MaxDeformationStepForPoseBound(0.5f, std::numeric_limits<f32>::quiet_NaN(), 6u), 0u);
}

// -----------------------------------------------------------------------------
// Failure mode 3 — invisible distant coats
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerFloor, NoVisibleCoatIsEverBuiltWithFewerStrandsThanTheFloor)
{
    constexpr u32 kFloor = 256u;
    constexpr u32 kStrands = 12000u;

    AnimalBudgetPolicy policy = TightPolicy();
    policy.MinVisibleStrands = kFloor;
    // A budget far below what the herd needs, so the allocator will coarsen
    // everything it is permitted to coarsen. The floor is the only thing
    // standing between this population and a field of bald animals.
    policy.FrameBudgetUnits = 1000.0f;

    std::vector<AnimalWorkItem> items = OverSubscribedHerd(80u);
    for (AnimalWorkItem& item : items)
    {
        item.StrandCount = kStrands;
        item.MaxStep[kVis] = MaxVisibilityStepForStrandFloor(kStrands, kFloor, 8u);
    }

    PopulationRun run(items, policy);

    u32 fewestStrands = kStrands;
    for (u32 frame = 0; frame < 120u; ++frame)
    {
        for (const AnimalSchedule& schedule : run.Step())
        {
            const u32 built = static_cast<u32>(static_cast<f32>(kStrands) * schedule.Fraction[kVis]);
            fewestStrands = std::min(fewestStrands, built);
        }
    }

    EXPECT_LT(fewestStrands, kStrands) << "nothing was thinned at all, so the floor is vacuous here";
    EXPECT_GE(fewestStrands, kFloor)
        << "a visible coat was thinned below the floor: the budget reached past MinVisibleStrands";
}

TEST(AnimalSchedulerFloor, TheFloorCounterSeesTheLadderBeingRefusedAndNotOnlyTheBudget)
{
    // THE ROUTE THAT MATTERS MOST WAS THE ONE THE COUNTER MISSED. An earlier
    // version only counted an animal as "at the visibility floor" when the
    // BUDGET had pushed it to its cap (step > desired). But the headline
    // guarantee — "invisible distant coats" — fires on the OTHER route: the
    // coat's own distance ladder asks to thin past MinVisibleStrands and the
    // floor refuses, which happens with no budget pressure at all.
    //
    // Found on the live population scene through olo_groom_budget_stats: 18 of
    // 41 animals were held by the floor and the counter read 0, while the
    // axis totals showed the SCHEDULED cost above the DESIRED cost — which is
    // only possible when something refused to coarsen.
    constexpr u32 kStrands = 1000u;
    constexpr u32 kFloor = 256u;

    AnimalBudgetPolicy policy = TightPolicy();
    policy.MinVisibleStrands = kFloor;
    policy.FrameBudgetUnits = 1.0e8f; // no pressure whatsoever

    AnimalWorkItem item = MakeAnimal(7u, AnimalRole::Background, 6.0f, kStrands);
    item.DesiredStep[kVis] = 6u; // the ladder wants a sixty-fourth: 15 strands
    item.MaxStep[kVis] = MaxVisibilityStepForStrandFloor(kStrands, kFloor, 8u);
    ASSERT_LT(item.MaxStep[kVis], item.DesiredStep[kVis]) << "the fixture must actually put the floor below the ladder";

    PopulationRun run({ item }, policy);
    AnimalSchedulerStats stats;
    const TArray<AnimalSchedule> schedules = run.Step(&stats);

    ASSERT_EQ(static_cast<sizet>(schedules.Num()), 1u);
    EXPECT_EQ(stats.AnimalsCoarsened, 0u) << "there is no budget pressure here at all";
    EXPECT_EQ(stats.AnimalsAtVisibilityFloor, 1u)
        << "the floor refused the distance ladder and the counter did not notice — which is the exact blind spot "
           "that made a live scene report 0 while 18 of its 41 coats were being held";

    // And the tell that exposed it: refusing to coarsen leaves the SCHEDULED
    // cost above the DESIRED cost, which nothing else in the scheduler can do.
    EXPECT_GT(stats.ScheduledCostUnits[kVis], stats.DesiredCostUnits[kVis]);
}

TEST(AnimalSchedulerFloor, AnAnimalHeldByItsAuthoredCapIsNotCountedAgainstTheFloor)
{
    // THE OTHER HALF OF THE COUNTER'S CONTRACT, and the half a `<=` made
    // vacuous. `AnimalsAtVisibilityFloor` answers "how many coats is
    // MinVisibleStrands holding back", so an animal pinned by its author's
    // m_MaxVisibilitySteps must not appear in it — that number would move when
    // somebody edited an asset, not when the floor bound anything.
    //
    // The guard that excluded it compared the item's combined cap against the
    // floor-only cap with `<=`. Both come out of the same monotonic halving
    // loop and differ only in the bound given to it, so `<=` is true for every
    // animal alive and the filter never fired.
    constexpr u32 kStrands = 1000u;
    constexpr u32 kFloor = 100u;
    constexpr u32 kAuthoredMax = 2u;

    AnimalBudgetPolicy policy = TightPolicy();
    policy.MinVisibleStrands = kFloor;
    // Hard pressure, so the allocator coarsens this animal all the way to
    // whichever cap binds first. That is the branch the counter reads.
    policy.FrameBudgetUnits = 1.0f;

    AnimalWorkItem item = MakeAnimal(11u, AnimalRole::Background, 4.0f, kStrands);
    item.DesiredStep[kVis] = 0u; // the ladder is asking for full rate, so nothing is refused
    item.MaxStep[kVis] = MaxVisibilityStepForStrandFloor(kStrands, kFloor, kAuthoredMax);

    // The fixture is only meaningful while the AUTHOR is strictly the tighter
    // of the two caps: 1000 strands over a floor of 100 affords three halvings
    // (1000 -> 500 -> 250 -> 125), and the author allowed two.
    const u32 floorOnlyCap = MaxVisibilityStepForStrandFloor(kStrands, kFloor, 16u);
    ASSERT_EQ(floorOnlyCap, 3u);
    ASSERT_EQ(item.MaxStep[kVis], kAuthoredMax);
    ASSERT_LT(item.MaxStep[kVis], floorOnlyCap) << "the author must be the binding cap or this case proves nothing";

    // Several frames: the allocator moves a step at a time so the picture does
    // not jump, so "pushed all the way to its cap" is a settled state, not a
    // first-frame one.
    PopulationRun run({ item }, policy);
    AnimalSchedulerStats stats;
    TArray<AnimalSchedule> schedules;
    for (u32 frame = 0; frame < 16u; ++frame)
    {
        stats = AnimalSchedulerStats{};
        schedules = run.Step(&stats);
    }

    ASSERT_EQ(static_cast<sizet>(schedules.Num()), 1u);
    ASSERT_EQ(schedules[0].Step[kVis], kAuthoredMax)
        << "the budget did not actually drive this animal to its cap, so the counter's branch never ran";
    EXPECT_EQ(stats.AnimalsAtVisibilityFloor, 0u)
        << "an animal stopped by its authored m_MaxVisibilitySteps was counted as held by MinVisibleStrands: the "
           "floor still permits "
        << floorOnlyCap << " halvings here and only the author refuses the third";

    // And the control, on the same fixture: lift the authored cap to the
    // floor's own answer and the animal IS floor-held, so the counter must see
    // it. Without this the case above would also pass on a counter wired to 0.
    AnimalWorkItem floorHeld = item;
    floorHeld.MaxStep[kVis] = MaxVisibilityStepForStrandFloor(kStrands, kFloor, 16u);
    PopulationRun floorRun({ floorHeld }, policy);
    AnimalSchedulerStats floorStats;
    TArray<AnimalSchedule> floorSchedules;
    for (u32 frame = 0; frame < 16u; ++frame)
    {
        floorStats = AnimalSchedulerStats{};
        floorSchedules = floorRun.Step(&floorStats);
    }

    ASSERT_EQ(static_cast<sizet>(floorSchedules.Num()), 1u);
    ASSERT_EQ(floorSchedules[0].Step[kVis], floorOnlyCap);
    EXPECT_EQ(floorStats.AnimalsAtVisibilityFloor, 1u)
        << "the same pressure against the floor's own cap must still be counted, or the fix above has simply "
           "switched the counter off";
}

TEST(AnimalSchedulerFloor, ACoatAuthoredBelowTheFloorIsNotForcedUpToIt)
{
    // The floor stops the BUDGET thinning a coat to nothing. It is not a
    // minimum the asset has to meet, and treating it as one would silently
    // refuse to LOD a deliberately sparse groom — whiskers, for instance.
    EXPECT_EQ(MaxVisibilityStepForStrandFloor(100u, 256u, 8u), 0u);
    EXPECT_EQ(MaxVisibilityStepForStrandFloor(256u, 256u, 8u), 0u);
    // And the ordinary case: 12000 strands over a 256 floor affords 5 halvings
    // (12000 -> 375), not 6 (-> 187).
    EXPECT_EQ(MaxVisibilityStepForStrandFloor(12000u, 256u, 8u), 5u);
    // The authored cap still wins when it is tighter.
    EXPECT_EQ(MaxVisibilityStepForStrandFloor(12000u, 256u, 2u), 2u);
    // A floor of zero is "no floor".
    EXPECT_EQ(MaxVisibilityStepForStrandFloor(12000u, 0u, 4u), 4u);
}

TEST(AnimalSchedulerFloor, TheDistanceLadderAloneCannotThinAVisibleCoatPastTheFloor)
{
    // "Invisible distant coats" is a failure the per-entity ladder can produce
    // unaided, with no budget pressure at all — which is why the cap is applied
    // as a min() against the DESIRED step and not only against the allocated
    // one. A population well inside its budget still gets the floor.
    AnimalBudgetPolicy policy = TightPolicy();
    policy.FrameBudgetUnits = 1.0e8f; // no pressure whatsoever
    policy.MinVisibleStrands = 256u;

    AnimalWorkItem item = MakeAnimal(7u, AnimalRole::Background, 4.0f, 1000u);
    item.DesiredStep[kVis] = 6u; // the ladder wants a sixty-fourth: 15 strands
    item.MaxStep[kVis] = MaxVisibilityStepForStrandFloor(1000u, 256u, 8u);

    PopulationRun run({ item }, policy);
    const TArray<AnimalSchedule> schedules = run.Step();

    ASSERT_EQ(static_cast<sizet>(schedules.Num()), 1u);
    EXPECT_EQ(schedules[0].Step[kVis], 1u) << "1000 strands over a 256 floor affords exactly one halving";
    EXPECT_GE(static_cast<u32>(1000.0f * schedules[0].Fraction[kVis]), 256u);
}

// -----------------------------------------------------------------------------
// Hero preservation
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerHero, TheHeroIsNeverCoarsenedWhileProtectHeroIsSet)
{
    AnimalBudgetPolicy policy = TightPolicy();
    policy.ProtectHero = true;
    policy.FrameBudgetUnits = 500.0f; // hopeless: the herd cannot fit at any step

    PopulationRun run(OverSubscribedHerd(100u), policy);

    for (u32 frame = 0; frame < 120u; ++frame)
    {
        AnimalSchedulerStats stats;
        for (const AnimalSchedule& schedule : run.Step(&stats))
        {
            if (schedule.Role != AnimalRole::Hero)
            {
                continue;
            }
            for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
            {
                EXPECT_EQ(schedule.Step[a], 0u)
                    << "frame " << frame << " axis " << ToString(static_cast<AnimalWorkAxis>(a))
                    << ": the hero gave way while a protection rule said it must not";
            }
        }
        EXPECT_EQ(stats.CoarsenedByRole[static_cast<sizet>(AnimalRole::Hero)], 0u);
    }
}

TEST(AnimalSchedulerHero, AnUnservableBudgetIsReportedRatherThanAbsorbedByTheHero)
{
    // The memory this encodes: a path that cannot do its job says so loudly.
    // Quietly degrading the hero to make the number fit would hide the one
    // thing the budget exists to protect, and the frame would merely look
    // slightly wrong with every counter green.
    AnimalBudgetPolicy policy = TightPolicy();
    policy.ProtectHero = true;
    policy.FrameBudgetUnits = 500.0f;

    PopulationRun run(OverSubscribedHerd(100u), policy);
    AnimalSchedulerStats stats;
    run.Step(&stats);

    EXPECT_TRUE(stats.BudgetExceeded) << "the budget could not be met and did not say so";

    // ASSERTED PER AXIS, not on the frame total, and the distinction is not
    // pedantry. The axes have separate allowances, so BudgetExceeded means at
    // least ONE of them could not be served — a frame whose simulation axis
    // overflowed while its shadow axis sat half empty can be over on an axis
    // and under in total. Asserting the total would be asserting something the
    // flag does not claim, and it would pass here only by accident of this
    // population being over on every axis at once.
    bool anyAxisOver = false;
    for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
    {
        anyAxisOver = anyAxisOver || (stats.ScheduledCostUnits[a] > stats.AxisBudgetUnits[a]);
    }
    EXPECT_TRUE(anyAxisOver)
        << "BudgetExceeded must mean an axis really is over its allowance, not merely that a candidate list emptied";
}

TEST(AnimalSchedulerHero, BackgroundAnimalsAreExhaustedBeforeAFeaturedOneGivesWay)
{
    // The role ladder, stated as the failure it prevents: a population that
    // degraded uniformly would take the featured animals down with the herd
    // while background animals still had halvings left to give.
    std::vector<AnimalWorkItem> items;
    items.push_back(MakeAnimal(1u, AnimalRole::Featured, 300.0f));
    for (u32 i = 0; i < 30u; ++i)
    {
        items.push_back(MakeAnimal(100u + i, AnimalRole::Background, 300.0f));
    }

    AnimalBudgetPolicy policy = TightPolicy();
    policy.FrameBudgetUnits = 12000.0f;

    PopulationRun run(items, policy);
    const TArray<AnimalSchedule> schedules = run.Step();

    const auto featured = std::find_if(schedules.begin(), schedules.end(),
                                       [](const AnimalSchedule& s)
                                       { return s.Role == AnimalRole::Featured; });
    ASSERT_NE(featured, schedules.end());

    if (featured->Step[kVis] > 0u)
    {
        for (const AnimalSchedule& schedule : schedules)
        {
            if (schedule.Role != AnimalRole::Background)
            {
                continue;
            }
            EXPECT_EQ(schedule.Step[kVis], 4u)
                << "a featured animal gave way while a background animal still had halvings left";
        }
    }
    else
    {
        SUCCEED() << "the herd alone absorbed the shortfall, which is the stronger outcome";
    }
}

// -----------------------------------------------------------------------------
// Independence of the axes (the #1252 rule, at population scale)
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerAxes, AnOverSubscribedAxisDoesNotCoarsenAnAxisThatFits)
{
    // Four axes and four allowances, so an expensive simulation cannot eat the
    // allowance that keeps the population's silhouette. One shared pool makes
    // the axes compete and the winner is whichever the cost model happens to
    // price highest — a tuning accident, not a decision anybody made.
    std::vector<AnimalWorkItem> items = OverSubscribedHerd(40u);
    for (AnimalWorkItem& item : items)
    {
        item.GuidePointCount = 40000u; // simulation wildly over-subscribed
        item.StrandCount = 10u;        // visibility trivially affordable
        item.ShadowVoxelCount = 1u;
        item.BoneCount = 1u;
    }

    AnimalBudgetPolicy policy = TightPolicy();
    PopulationRun run(items, policy);

    AnimalSchedulerStats stats;
    const TArray<AnimalSchedule> schedules = run.Step(&stats);

    bool anySimulationCoarsened = false;
    for (const AnimalSchedule& schedule : schedules)
    {
        if (schedule.Role == AnimalRole::Hero)
        {
            continue;
        }
        anySimulationCoarsened = anySimulationCoarsened || schedule.Step[kSim] > 0u;
        EXPECT_EQ(schedule.Step[kVis], 0u) << "the visibility axis fits comfortably and must not have been touched";
        EXPECT_EQ(schedule.Step[kShadow], 0u) << "the shadow axis fits comfortably and must not have been touched";
    }
    EXPECT_TRUE(anySimulationCoarsened) << "the simulation axis was meant to be over-subscribed here";
}

// -----------------------------------------------------------------------------
// Determinism — criterion 1 rests on it
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerDeterminism, TheSameFrameSchedulesIdenticallyWhateverTheGatherOrder)
{
    // The service order's UUID tie-break exists so the allocation cannot depend
    // on EnTT's iteration order, which is a function of allocation history
    // rather than of the scene. Without it, loading the same scene twice could
    // schedule it differently and every capture downstream would be noise.
    std::vector<AnimalWorkItem> forward = OverSubscribedHerd(50u);
    std::vector<AnimalWorkItem> reversed(forward.rbegin(), forward.rend());

    AnimalBudgetPolicy policy = TightPolicy();
    PopulationRun a(forward, policy);
    PopulationRun b(reversed, policy);

    for (u32 frame = 0; frame < 30u; ++frame)
    {
        TArray<AnimalSchedule> sa = a.Step();
        TArray<AnimalSchedule> sb = b.Step();
        ASSERT_EQ(static_cast<sizet>(sa.Num()), static_cast<sizet>(sb.Num()));

        const auto byId = [](const AnimalSchedule& l, const AnimalSchedule& r)
        { return static_cast<u64>(l.Id) < static_cast<u64>(r.Id); };
        std::sort(sa.begin(), sa.end(), byId);
        std::sort(sb.begin(), sb.end(), byId);

        for (sizet i = 0; i < static_cast<sizet>(sa.Num()); ++i)
        {
            EXPECT_TRUE(sa[i] == sb[i]) << "frame " << frame << " animal " << static_cast<u64>(sa[i].Id)
                                        << ": the schedule depended on the order the animals were gathered in";
        }
    }
}

TEST(AnimalSchedulerDeterminism, RepeatedRunsOfTheSamePopulationAreBitIdentical)
{
    AnimalBudgetPolicy policy = TightPolicy();
    PopulationRun a(OverSubscribedHerd(50u), policy);
    PopulationRun b(OverSubscribedHerd(50u), policy);

    for (u32 frame = 0; frame < 40u; ++frame)
    {
        const TArray<AnimalSchedule> sa = a.Step();
        const TArray<AnimalSchedule> sb = b.Step();
        ASSERT_EQ(static_cast<sizet>(sa.Num()), static_cast<sizet>(sb.Num()));
        for (sizet i = 0; i < static_cast<sizet>(sa.Num()); ++i)
        {
            EXPECT_TRUE(sa[i] == sb[i]) << "frame " << frame << " index " << i;
        }
    }
}

// -----------------------------------------------------------------------------
// Sanitisation — every number here arrives from YAML, a save game or MCP
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerSanitize, AxisWeightsAreNormalisedRatherThanClamped)
{
    AnimalBudgetPolicy policy;
    policy.AxisWeights = { 2.0f, 1.0f, 1.0f, 1.0f };
    const AnimalBudgetPolicy a = SanitizeAnimalBudgetPolicy(policy);

    policy.AxisWeights = { 0.4f, 0.2f, 0.2f, 0.2f };
    const AnimalBudgetPolicy b = SanitizeAnimalBudgetPolicy(policy);

    for (sizet i = 0; i < AnimalWorkAxisCount; ++i)
    {
        EXPECT_NEAR(a.AxisWeights[i], b.AxisWeights[i], 1e-6f) << "axis " << i;
    }
    const f32 sum = std::accumulate(a.AxisWeights.begin(), a.AxisWeights.end(), 0.0f);
    EXPECT_NEAR(sum, 1.0f, 1e-6f) << "an author must not be able to hand out more than one frame";
}

TEST(AnimalSchedulerSanitize, AnAllZeroWeightSetTakesAnEqualSplitAndNotZero)
{
    // A zero allowance on every axis pins every animal at its coarsest step
    // forever, which reads as a broken renderer rather than as a bad number.
    AnimalBudgetPolicy policy;
    policy.AxisWeights = { 0.0f, 0.0f, 0.0f, 0.0f };
    const AnimalBudgetPolicy out = SanitizeAnimalBudgetPolicy(policy);
    for (sizet i = 0; i < AnimalWorkAxisCount; ++i)
    {
        EXPECT_NEAR(out.AxisWeights[i], 0.25f, 1e-6f);
    }
}

TEST(AnimalSchedulerSanitize, NonFiniteFieldsTakeTheirDefaults)
{
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    const AnimalBudgetPolicy defaults;

    AnimalBudgetPolicy policy;
    policy.FrameBudgetUnits = nan;
    policy.MaxPoseStepPixels = inf;
    policy.AxisWeights = { nan, inf, -1.0f, 1.0f };
    const AnimalBudgetPolicy out = SanitizeAnimalBudgetPolicy(policy);

    EXPECT_TRUE(std::isfinite(out.FrameBudgetUnits));
    EXPECT_FLOAT_EQ(out.FrameBudgetUnits, defaults.FrameBudgetUnits);
    EXPECT_TRUE(std::isfinite(out.MaxPoseStepPixels));
    for (sizet i = 0; i < AnimalWorkAxisCount; ++i)
    {
        EXPECT_TRUE(std::isfinite(out.AxisWeights[i])) << "axis " << i;
        EXPECT_GE(out.AxisWeights[i], 0.0f) << "axis " << i;
    }
}

TEST(AnimalSchedulerSanitize, ANegativeCostCoefficientTakesItsDefaultButZeroIsKept)
{
    // Zero is a legitimate authoring answer — "this axis is free on this
    // machine" is exactly what a census of a GPU-bound path concludes about a
    // CPU axis. Negative would make coarsening INCREASE the cost and the
    // allocation loop would never terminate.
    AnimalCostModel model;
    model.DeformationPerBone = -1.0f;
    model.VisibilityPerStrand = 0.0f;
    const AnimalCostModel out = SanitizeAnimalCostModel(model);

    EXPECT_FLOAT_EQ(out.DeformationPerBone, AnimalCostModel{}.DeformationPerBone);
    EXPECT_FLOAT_EQ(out.VisibilityPerStrand, 0.0f);
}

TEST(AnimalSchedulerSanitize, ANonFinitePixelSizeCannotBreakTheServiceOrder)
{
    // A NaN apparent size makes every `<` false in both directions, which is not
    // a strict weak ordering: std::sort on it is undefined behaviour, not a
    // wrong answer. The repair happens once, where the comparator reads.
    std::vector<AnimalWorkItem> items = OverSubscribedHerd(20u);
    items[3].PixelSize = std::numeric_limits<f32>::quiet_NaN();
    items[7].PixelSize = -std::numeric_limits<f32>::infinity();

    PopulationRun run(items, TightPolicy());
    const TArray<AnimalSchedule> schedules = run.Step();
    EXPECT_EQ(static_cast<sizet>(schedules.Num()), items.size());
}

// -----------------------------------------------------------------------------
// The cost model, and criterion 2
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerCost, DrawCallCostIsChargedButNeverScaledByAnyStep)
{
    // This is criterion 2 made into a mechanism rather than a claim: draw calls
    // are priced beside the schedulable axes, and no step reduces them. A
    // budget that coarsens everything to nothing still pays this — so if draw
    // calls dominated, scheduling could not help and the answer would have to
    // be batching (#1031), which is exactly what the issue tells us not to
    // assume either way.
    AnimalWorkItem item = MakeAnimal(1u, AnimalRole::Background, 100.0f);
    const AnimalCostModel model;

    const std::array<u32, AnimalWorkAxisCount> full{ 0u, 0u, 0u, 0u };
    const std::array<u32, AnimalWorkAxisCount> starved{ 20u, 20u, 20u, 20u };

    const f32 fullCost = EstimateAnimalCostUnits(model, item, full);
    const f32 starvedCost = EstimateAnimalCostUnits(model, item, starved);
    const f32 drawCost = model.PerDrawCall * static_cast<f32>(item.DrawCallCount);

    EXPECT_LT(starvedCost, fullCost);
    EXPECT_GE(starvedCost, drawCost) << "the unreachable cost must survive every halving";
    EXPECT_NEAR(starvedCost, drawCost, drawCost * 0.05f)
        << "at twenty halvings essentially nothing but the draw calls should remain";
}

TEST(AnimalSchedulerCost, TheReportedAxisSharesSumToOneWithTheDrawCallShare)
{
    PopulationRun run(OverSubscribedHerd(30u), TightPolicy());
    AnimalSchedulerStats stats;
    run.Step(&stats);

    ASSERT_GT(stats.EstimatedCostUnits, 0.0f);
    f32 share = 0.0f;
    for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
    {
        share += stats.AxisShareOfScheduled[a];
    }
    share += stats.DrawCallCostUnits / stats.EstimatedCostUnits;
    EXPECT_NEAR(share, 1.0f, 1e-3f) << "the census's shares must account for the whole frame or they mislead";
}

TEST(AnimalSchedulerCost, CoarseningNeverIncreasesTheEstimatedCost)
{
    // Monotone by sweep, not by sample. This is the property the allocation
    // loop's termination rests on: a coarsening that could raise the cost would
    // let the while-loop run forever.
    AnimalWorkItem item = MakeAnimal(1u, AnimalRole::Background, 100.0f);
    const AnimalCostModel model;

    f32 previous = std::numeric_limits<f32>::infinity();
    for (u32 step = 0; step <= 16u; ++step)
    {
        std::array<u32, AnimalWorkAxisCount> steps{};
        steps.fill(step);
        const f32 cost = EstimateAnimalCostUnits(model, item, steps);
        EXPECT_LE(cost, previous + 1e-4f) << "step " << step;
        previous = cost;
    }
}

// -----------------------------------------------------------------------------
// The decision carries the floors, so a downstream ladder cannot walk past them
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerFloor, TheScheduleCarriesTheCapSoAConsumerCanClampAgainstIt)
{
    // WHY THE DECISION CARRIES MaxStep AT ALL. A consumer that combines this
    // schedule with another ladder — Scene does exactly that, folding the
    // budget into the groom's own #1252 answer — must clamp the combination.
    // Spending `max(myLadder, scheduled)` and stopping there lets the OTHER
    // ladder walk straight past MinVisibleStrands, and every assertion inside
    // this file still passes because the scheduler never saw that number.
    //
    // This case is the seam: the cap the consumer needs is ON the decision, and
    // it is the floor-derived one rather than the authored one.
    constexpr u32 kStrands = 1000u;
    constexpr u32 kFloor = 256u;

    AnimalBudgetPolicy policy = TightPolicy();
    policy.MinVisibleStrands = kFloor;
    policy.FrameBudgetUnits = 1.0e8f; // no pressure: the cap must not depend on it

    AnimalWorkItem item = MakeAnimal(7u, AnimalRole::Background, 40.0f, kStrands);
    item.MaxStep[kVis] = MaxVisibilityStepForStrandFloor(kStrands, kFloor, 8u);

    PopulationRun run({ item }, policy);
    const TArray<AnimalSchedule> schedules = run.Step();

    ASSERT_EQ(static_cast<sizet>(schedules.Num()), 1u);
    // 1000 strands over a 256 floor affords exactly one halving.
    EXPECT_EQ(schedules[0].MaxStep[kVis], 1u)
        << "the decision does not carry the floor-derived cap, so a consumer combining it with another ladder has "
           "nothing to clamp against";
    EXPECT_GE(static_cast<u32>(static_cast<f32>(kStrands) * AnimalStepFraction(schedules[0].MaxStep[kVis])), kFloor);

    // And the clamp a consumer performs with it does the job: a downstream
    // ladder asking for a sixty-fourth is held at the floor.
    constexpr u32 kGreedyLadderStep = 6u;
    const u32 combined = std::min(std::max(kGreedyLadderStep, schedules[0].Step[kVis]), schedules[0].MaxStep[kVis]);
    EXPECT_EQ(combined, 1u);
    EXPECT_GE(static_cast<u32>(static_cast<f32>(kStrands) * AnimalStepFraction(combined)), kFloor);
}

// -----------------------------------------------------------------------------
// StarvationFrames is a bound the allocator obeys, not a number it stores
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerStarvation, AnAnimalAtTheStarvationBoundIsPassedOverWhileAPeerIsStillEligible)
{
    // The knob has to DO something. Two identical background animals, one
    // already at the bound, a budget that can only afford one of them at full
    // rate: the one at the bound must be excluded from the candidate set and
    // the other must give way, however the apparent-size tie-break would have
    // ordered them.
    std::vector<AnimalWorkItem> items;
    items.push_back(MakeAnimal(10u, AnimalRole::Background, 200.0f));
    items.push_back(MakeAnimal(11u, AnimalRole::Background, 200.0f));

    AnimalBudgetPolicy policy = TightPolicy();
    policy.StarvationFrames = 4u;
    // A BUDGET THE ELIGIBLE PEER CAN ABSORB ON ITS OWN. At 400 units the
    // visibility allowance is 93 and two 12 000-strand coats cost 324, which
    // one peer cannot close even at its cap — so the allocator correctly drops
    // the starvation rule and coarsens the starved animal too, and the case
    // would be asserting that the bound is unenforceable rather than that it is
    // ignored. At 1000 the allowance is 243 and one halving of the peer (162 +
    // 81 = 243) closes it exactly.
    policy.FrameBudgetUnits = 1000.0f;

    PopulationRun run(items, policy);
    run.StateOf(10u).StarvedFrames[kVis] = 4u; // at the bound
    run.StateOf(11u).StarvedFrames[kVis] = 0u;

    const TArray<AnimalSchedule> schedules = run.Step();
    ASSERT_EQ(static_cast<sizet>(schedules.Num()), 2u);

    const AnimalSchedule& atBound = schedules[0].Id == UUID{ 10u } ? schedules[0] : schedules[1];
    const AnimalSchedule& fresh = schedules[0].Id == UUID{ 10u } ? schedules[1] : schedules[0];

    EXPECT_EQ(atBound.Step[kVis], 0u)
        << "an animal at StarvationFrames was coarsened again while an eligible peer was available: the bound is "
           "being stored and not enforced";
    EXPECT_GT(fresh.Step[kVis], 0u) << "the eligible peer should have absorbed the shortfall";
}

TEST(AnimalSchedulerStarvation, WhenEveryCandidateIsAtTheBoundTheRuleIsDroppedRatherThanStalling)
{
    // The other half, and it is the one that would deadlock if it were missing.
    // With nobody left to swap with, refusing to coarsen anyone would leave the
    // axis permanently over budget and the frame permanently wrong — so the
    // relative bound is dropped for that pass rather than turned into a
    // guarantee it cannot keep.
    std::vector<AnimalWorkItem> items;
    items.push_back(MakeAnimal(10u, AnimalRole::Background, 200.0f));
    items.push_back(MakeAnimal(11u, AnimalRole::Background, 200.0f));

    AnimalBudgetPolicy policy = TightPolicy();
    policy.StarvationFrames = 4u;
    policy.FrameBudgetUnits = 400.0f;

    PopulationRun run(items, policy);
    run.StateOf(10u).StarvedFrames[kVis] = 9u;
    run.StateOf(11u).StarvedFrames[kVis] = 9u;

    AnimalSchedulerStats stats;
    const TArray<AnimalSchedule> schedules = run.Step(&stats);

    const bool anythingCoarsened =
        std::ranges::any_of(schedules, [](const AnimalSchedule& s)
                            { return s.Step[kVis] > 0u; });
    EXPECT_TRUE(anythingCoarsened)
        << "every candidate was over the starvation bound and the allocator refused to coarsen any of them, so the "
           "axis stays over budget forever";
}

// -----------------------------------------------------------------------------
// The pose clock is advanced at EVERY animation site — a source scan
// -----------------------------------------------------------------------------

TEST(AnimalSchedulerStability, AnimalPoseTickIsAdvancedAtEveryAnimationSite)
{
    // A SOURCE SCAN, for SkeletalDeformationContract's reason: a clock wired
    // into only some entry points is invisible in every test that drives the
    // others, and this exact bug has now happened twice.
    //
    // ShouldPoseAnimalThisFrame's gate runs inside the per-entity animation
    // loop. There are two such loops in Scene.cpp — the runtime one in
    // UpdateAnimation and the preview one in OnUpdateEditor — and the tick must
    // advance immediately above BOTH. Advancing it in UpdateAnimation alone
    // froze the clock in edit mode: every animal whose UUID-derived phase did
    // not satisfy the congruence was skipped on every frame forever, while the
    // scheduler reported it scheduled and nothing logged.
    //
    // Scanning the SOURCE rather than the behaviour because the failure is a
    // missing call at a site no unit test reaches: a behavioural test would
    // have to drive OnUpdateEditor with a real clip and a real skeleton, which
    // is the visual-evidence fixture's job, and it would still only cover the
    // sites somebody remembered to drive.
    const fs::path repoRoot = fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
    const fs::path path = repoRoot / "OloEngine/src/OloEngine/Scene/Scene.cpp";

    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "could not read " << path.string();
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);)
    {
        lines.push_back(line);
    }
    ASSERT_FALSE(lines.empty());

    // Every line that opens a per-entity animation loop over the
    // (AnimationStateComponent, SkeletonComponent) group.
    constexpr std::string_view kLoop = "m_Registry.group<AnimationStateComponent, SkeletonComponent>()";
    constexpr std::string_view kTick = "++m_AnimalPoseTick;";

    u32 sites = 0;
    for (sizet i = 0; i < lines.size(); ++i)
    {
        if (lines[i].find(kLoop) == std::string::npos)
        {
            continue;
        }
        ++sites;

        // The increment sits just above, allowing for the comment block that
        // explains why — the same tolerance SkeletalDeformationContract uses.
        //
        // AND THE MATCH HAS TO BE CODE. A scan that accepts any occurrence of
        // the text accepts one inside the very comment block that explains the
        // tick, or inside a log string naming it, and a site could then lose
        // its increment while this still passed. StripNonCode removes line
        // comments and string literals before the search, so what is left is
        // executable or nothing.
        const sizet lo = i >= 24u ? i - 24u : 0u;
        bool advanced = false;
        for (sizet j = lo; j < i && !advanced; ++j)
        {
            advanced = StripNonCode(lines[j]).find(kTick) != std::string::npos;
        }

        EXPECT_TRUE(advanced)
            << "Scene.cpp:" << (i + 1)
            << " opens an animation loop whose body evaluates ShouldPoseAnimalThisFrame, with no nearby "
               "'++m_AnimalPoseTick;'. The deformation stagger's clock must advance at EVERY animation site: left "
               "out of one, the gate there sees a frozen tick and every animal whose phase does not satisfy the "
               "congruence is skipped forever, in that mode only, silently.";
    }

    // The two sites are thousands of lines apart, so no single increment can
    // satisfy both windows — each EXPECT above is its own site's assertion.
    //
    // A guard nobody reaches is the failure this scan exists to catch, so it
    // also has to fail when the sites themselves move or are renamed.
    EXPECT_GE(sites, 2u)
        << "fewer than two animation loops found in Scene.cpp — either a site was removed, or this scan stopped "
           "matching and is now passing vacuously";
}

TEST(AnimalSchedulerStability, TheSourceScanSieveRejectsCommentsAndStrings)
{
    // The scan above is only as strong as StripNonCode, so break it on purpose:
    // if the sieve ever let a comment through, the per-site assertion would go
    // green on a file that had lost its increment, and nothing else in the
    // suite would notice.
    EXPECT_NE(StripNonCode("            ++m_AnimalPoseTick;").find("++m_AnimalPoseTick;"), std::string::npos)
        << "the executable statement must survive the sieve, or the scan can never pass";

    EXPECT_EQ(StripNonCode("            // ++m_AnimalPoseTick; advances the clock").find("++m_AnimalPoseTick;"),
              std::string::npos)
        << "a line comment satisfied the scan";
    EXPECT_EQ(StripNonCode("        OLO_CORE_TRACE(\"++m_AnimalPoseTick;\");").find("++m_AnimalPoseTick;"),
              std::string::npos)
        << "a string literal satisfied the scan";
    EXPECT_EQ(StripNonCode("        Foo(); // ++m_AnimalPoseTick;").find("++m_AnimalPoseTick;"), std::string::npos)
        << "a trailing comment after real code satisfied the scan";

    // Code before a comment is still code.
    EXPECT_NE(StripNonCode("            ++m_AnimalPoseTick; // why").find("++m_AnimalPoseTick;"), std::string::npos);
}
