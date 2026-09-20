#include "OloEnginePCH.h"

#include "OloEngine/Scene/AnimalScheduler.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace OloEngine
{
    namespace
    {
        constexpr f32 kMinFrameBudgetUnits = 1.0f;
        constexpr f32 kMaxFrameBudgetUnits = 1.0e9f;
        constexpr f32 kMaxPoseStepPixels = 256.0f;
        constexpr u32 kMaxHoldFrames = 600u;
        constexpr u32 kMaxStarvationFrames = 600u;
        constexpr u32 kMaxBudgetSteps = 16u;
        constexpr u32 kMaxMinVisibleStrands = 1u << 24;

        [[nodiscard]] f32 SanitizeCoefficient(f32 value, f32 fallback) noexcept
        {
            // NEGATIVE IS REJECTED, ZERO IS NOT. A zero coefficient is a
            // legitimate authoring choice -- it says "this axis is free on this
            // machine", which is exactly what a census of a GPU-bound path
            // would conclude about a CPU axis -- while a negative one would
            // make coarsening INCREASE the cost and the allocation loop would
            // never terminate.
            return (std::isfinite(value) && value >= 0.0f) ? value : fallback;
        }

        // Refining is immediate, coarsening waits for the hold. The asymmetry,
        // and its justification, are GroomLod.cpp::ApplyHold's: an animal that
        // just became important and is still cheap is a picture anyone can see,
        // while one that stays expensive a few frames too long is merely
        // expensive.
        [[nodiscard]] u32 ApplyHold(u32 current, u32 requested, u32 framesStable, u32 holdFrames) noexcept
        {
            if (requested <= current)
            {
                return requested;
            }
            return framesStable >= holdFrames ? requested : current;
        }

        /// The work one axis spends at full rate, in this animal's own units.
        [[nodiscard]] f32 AxisUnitWork(const AnimalWorkItem& item, AnimalWorkAxis axis) noexcept
        {
            switch (axis)
            {
                case AnimalWorkAxis::Deformation:
                    return static_cast<f32>(item.BoneCount);
                case AnimalWorkAxis::Simulation:
                    // Priced at what the solver will INTEGRATE, not at one step.
                    // A coat at 60 Hz in a 30 Hz frame runs two substeps and
                    // costs twice as much, and a budget blind to that would be
                    // wrong by a factor that changes with the frame rate.
                    return static_cast<f32>(item.GuidePointCount) * static_cast<f32>(item.SimulationSubsteps);
                case AnimalWorkAxis::Visibility:
                    return static_cast<f32>(item.StrandCount);
                case AnimalWorkAxis::Shadow:
                    return static_cast<f32>(item.ShadowVoxelCount);
                case AnimalWorkAxis::Count:
                    break;
            }
            return 0.0f;
        }

        [[nodiscard]] f32 AxisCoefficient(const AnimalCostModel& model, AnimalWorkAxis axis) noexcept
        {
            switch (axis)
            {
                case AnimalWorkAxis::Deformation:
                    return model.DeformationPerBone;
                case AnimalWorkAxis::Simulation:
                    return model.SimulationPerGuidePoint;
                case AnimalWorkAxis::Visibility:
                    return model.VisibilityPerStrand;
                case AnimalWorkAxis::Shadow:
                    return model.ShadowPerVoxel;
                case AnimalWorkAxis::Count:
                    break;
            }
            return 0.0f;
        }

        /// Cost of one animal's one axis at `step`.
        [[nodiscard]] f32 AxisCostAtStep(const AnimalCostModel& model, const AnimalWorkItem& item, AnimalWorkAxis axis,
                                         u32 step) noexcept
        {
            return AxisCoefficient(model, axis) * AxisUnitWork(item, axis) * AnimalStepFraction(step);
        }

        /// The service order: who gives way FIRST.
        ///
        /// Four keys, every one of them deterministic, and the UUID last so two
        /// otherwise identical animals cannot tie and leave the order depending
        /// on the gather order -- which would make the allocation depend on
        /// EnTT's iteration, and therefore not reproducible.
        struct ServiceOrder
        {
            const AnimalWorkItem* Item = nullptr;
            u32 Index = 0u;
            u32 StarvedFrames = 0u;
            /// A SANITISED copy, not a read through Item. A NaN apparent size
            /// makes every `<` false in both directions, which is not a strict
            /// weak ordering and is undefined behaviour inside std::sort -- so
            /// the value the comparator sees is repaired once, here, rather
            /// than trusted from an authored transform.
            f32 PixelSize = 0.0f;
            u64 Id = 0u;
        };

        [[nodiscard]] bool GivesWayBefore(const ServiceOrder& a, const ServiceOrder& b) noexcept
        {
            // Least starved first: an animal that has been passed over rises
            // through this comparison until it outranks its competitors, which
            // IS the anti-starvation guarantee. A counter that were merely
            // reported would be a diagnostic; one that is part of the sort is a
            // mechanism.
            if (a.StarvedFrames != b.StarvedFrames)
            {
                return a.StarvedFrames < b.StarvedFrames;
            }
            // Smallest on screen gives way first: at equal fairness, the animal
            // whose degradation is least visible.
            if (!Math::BitwiseEqual(a.PixelSize, b.PixelSize))
            {
                return a.PixelSize < b.PixelSize;
            }
            return a.Id > b.Id;
        }
    } // namespace

    AnimalCostModel SanitizeAnimalCostModel(const AnimalCostModel& model) noexcept
    {
        const AnimalCostModel defaults;
        AnimalCostModel out;
        out.DeformationPerBone = SanitizeCoefficient(model.DeformationPerBone, defaults.DeformationPerBone);
        out.SimulationPerGuidePoint =
            SanitizeCoefficient(model.SimulationPerGuidePoint, defaults.SimulationPerGuidePoint);
        out.VisibilityPerStrand = SanitizeCoefficient(model.VisibilityPerStrand, defaults.VisibilityPerStrand);
        out.ShadowPerVoxel = SanitizeCoefficient(model.ShadowPerVoxel, defaults.ShadowPerVoxel);
        out.PerDrawCall = SanitizeCoefficient(model.PerDrawCall, defaults.PerDrawCall);
        return out;
    }

    AnimalBudgetPolicy SanitizeAnimalBudgetPolicy(const AnimalBudgetPolicy& policy) noexcept
    {
        const AnimalBudgetPolicy defaults;
        AnimalBudgetPolicy out;

        out.Enabled = policy.Enabled;
        out.ProtectHero = policy.ProtectHero;

        out.FrameBudgetUnits = std::isfinite(policy.FrameBudgetUnits)
                                   ? std::clamp(policy.FrameBudgetUnits, kMinFrameBudgetUnits, kMaxFrameBudgetUnits)
                                   : defaults.FrameBudgetUnits;

        // THE WEIGHTS ARE NORMALISED, NOT CLAMPED, so an author cannot hand out
        // 140% of the frame by writing four plausible-looking fractions, and
        // 2/1/1/1 and 0.4/0.2/0.2/0.2 are the same policy.
        f32 weightSum = 0.0f;
        for (sizet i = 0; i < AnimalWorkAxisCount; ++i)
        {
            const f32 w = policy.AxisWeights[i];
            out.AxisWeights[i] = (std::isfinite(w) && w >= 0.0f) ? w : 0.0f;
            weightSum += out.AxisWeights[i];
        }
        if (!(weightSum > 0.0f))
        {
            // An all-zero or wholly non-finite weight set takes an EQUAL SPLIT
            // rather than zero. A zero weight means that axis gets no allowance
            // at all -- every animal pinned at its coarsest step forever, which
            // reads as a broken renderer rather than as a bad number.
            for (sizet i = 0; i < AnimalWorkAxisCount; ++i)
            {
                out.AxisWeights[i] = 1.0f / static_cast<f32>(AnimalWorkAxisCount);
            }
        }
        else
        {
            for (sizet i = 0; i < AnimalWorkAxisCount; ++i)
            {
                out.AxisWeights[i] /= weightSum;
            }
        }

        out.MaxPoseStepPixels = std::isfinite(policy.MaxPoseStepPixels)
                                    ? std::clamp(policy.MaxPoseStepPixels, 0.0f, kMaxPoseStepPixels)
                                    : defaults.MaxPoseStepPixels;
        out.MinVisibleStrands = std::min(policy.MinVisibleStrands, kMaxMinVisibleStrands);
        out.HoldFrames = std::min(policy.HoldFrames, kMaxHoldFrames);
        out.StarvationFrames = std::min(policy.StarvationFrames, kMaxStarvationFrames);
        return out;
    }

    AnimalSchedule IdentityAnimalSchedule(UUID id) noexcept
    {
        AnimalSchedule schedule;
        schedule.Id = id;
        schedule.Outcome = AnimalBudgetOutcome::NotScheduled;
        for (sizet i = 0; i < AnimalWorkAxisCount; ++i)
        {
            schedule.Step[i] = 0u;
            schedule.Fraction[i] = 1.0f;
        }
        return schedule;
    }

    f32 EstimateAnimalCostUnits(const AnimalCostModel& rawModel, const AnimalWorkItem& item,
                                std::span<const u32> steps) noexcept
    {
        const AnimalCostModel model = SanitizeAnimalCostModel(rawModel);
        f32 total = model.PerDrawCall * static_cast<f32>(item.DrawCallCount);
        for (sizet i = 0; i < AnimalWorkAxisCount; ++i)
        {
            const u32 step = i < steps.size() ? steps[i] : 0u;
            total += AxisCostAtStep(model, item, static_cast<AnimalWorkAxis>(i), step);
        }
        return total;
    }

    u32 MaxDeformationStepForPoseBound(f32 fullRateMotionPixels, f32 maxPoseStepPixels, u32 authoredMaxStep) noexcept
    {
        const u32 cap = std::min(authoredMaxStep, kMaxBudgetSteps);
        if (!std::isfinite(maxPoseStepPixels) || maxPoseStepPixels <= 0.0f)
        {
            // REFUSING TO REDUCE THE RATE AT ALL IS THE SAFE ANSWER, because
            // the failure of guessing wrong here is visible judder -- and a
            // bound of zero is an author saying "no pose step is acceptable",
            // which is a statement, not a missing value.
            return 0u;
        }
        if (!std::isfinite(fullRateMotionPixels) || fullRateMotionPixels <= 0.0f)
        {
            // A still animal's pose does not move between updates whatever the
            // rate, so no bound binds and the authored cap is the answer.
            return cap;
        }
        if (fullRateMotionPixels > maxPoseStepPixels)
        {
            // Already over the bound at FULL rate. Reducing the rate can only
            // make it worse, so no reduction is allowed -- and the animal is
            // counted at the pose-step cap so the budget's inability to touch
            // it is visible rather than mysterious.
            return 0u;
        }
        // The largest k with motion * 2^k <= bound.
        const f32 ratio = maxPoseStepPixels / fullRateMotionPixels;
        const f32 exact = std::log2(ratio);
        if (!std::isfinite(exact) || exact <= 0.0f)
        {
            return 0u;
        }
        const f32 floored = std::floor(exact);
        if (floored >= static_cast<f32>(cap))
        {
            return cap;
        }
        return static_cast<u32>(floored);
    }

    u32 MaxVisibilityStepForStrandFloor(u32 strandCount, u32 minVisibleStrands, u32 authoredMaxStep) noexcept
    {
        const u32 cap = std::min(authoredMaxStep, kMaxBudgetSteps);
        if (minVisibleStrands == 0u || strandCount == 0u)
        {
            return cap;
        }
        if (strandCount <= minVisibleStrands)
        {
            // ALREADY BELOW THE FLOOR AT FULL RATE. A groom authored with fewer
            // strands than the floor is not a budget failure and must not be
            // forced up to it -- the floor stops the BUDGET thinning a coat to
            // nothing, it is not a minimum the asset has to meet.
            return 0u;
        }
        u32 step = 0u;
        u32 retained = strandCount;
        while (step < cap && (retained >> 1u) >= minVisibleStrands)
        {
            retained >>= 1u;
            ++step;
        }
        return step;
    }

    std::vector<AnimalSchedule> ScheduleAnimalPopulation(const AnimalBudgetPolicy& rawPolicy,
                                                        const AnimalCostModel& rawModel,
                                                        std::span<const AnimalScheduleSlot> slots,
                                                        AnimalSchedulerStats* outStats)
    {
        const AnimalBudgetPolicy policy = SanitizeAnimalBudgetPolicy(rawPolicy);
        const AnimalCostModel model = SanitizeAnimalCostModel(rawModel);

        std::vector<AnimalSchedule> results;
        results.reserve(slots.size());

        AnimalSchedulerStats stats;
        stats.FrameBudgetUnits = policy.FrameBudgetUnits;
        stats.AnimalsConsidered = static_cast<u32>(slots.size());

        if (!policy.Enabled)
        {
            // THE STATE IS RESET, not merely ignored -- GroomLod.cpp's rule and
            // its reason. Leaving the hysteresis and starvation counters where
            // they were would mean that turning the budget off and on again
            // resumed from an allocation made for a population that no longer
            // exists, and the A/B control for every capture in this issue is
            // "turn it off", which has to be the same picture every time.
            for (const AnimalScheduleSlot& slot : slots)
            {
                if (slot.State != nullptr)
                {
                    *slot.State = AnimalScheduleState{};
                }
                // PASS-THROUGH, not full rate: with the population budget off,
                // each animal runs at exactly what its own ladder asked for,
                // which is the frame #1252 produced.
                AnimalSchedule schedule = IdentityAnimalSchedule(slot.Item.Id);
                schedule.PixelSize = slot.Item.PixelSize;
                schedule.Role = slot.Item.Role;
                for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
                {
                    schedule.Step[a] = std::min(slot.Item.DesiredStep[a], kMaxBudgetSteps);
                    schedule.Fraction[a] = AnimalStepFraction(schedule.Step[a]);
                }
                schedule.EstimatedCostUnits = EstimateAnimalCostUnits(model, slot.Item, schedule.Step);
                results.push_back(schedule);
            }
            if (outStats != nullptr)
            {
                stats.AnimalsConsidered = 0u;
                *outStats = stats;
            }
            return results;
        }

        const sizet count = slots.size();

        // ── 1. Every animal starts at its desired step, clamped by its cap ──
        //
        // min, NOT max. When a floor makes MaxStep COARSER than DesiredStep the
        // floor wins, and that is deliberate: it is how a distance ladder is
        // stopped from thinning a visible coat below MinVisibleStrands all on
        // its own, with no budget pressure involved at all. "Invisible distant
        // coats" is a failure the ladder can produce unaided.
        std::vector<std::array<u32, AnimalWorkAxisCount>> allocated(count);
        for (sizet i = 0; i < count; ++i)
        {
            for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
            {
                const u32 desired = std::min(slots[i].Item.DesiredStep[a], kMaxBudgetSteps);
                const u32 cap = std::min(slots[i].Item.MaxStep[a], kMaxBudgetSteps);
                allocated[i][a] = std::min(desired, cap);
            }
        }

        // ── 2. Price the frame, and take the unreachable cost off the top ──
        //
        // Draw calls do not scale with any step, so a budget that pretended they
        // were schedulable would coarsen every axis to nothing and still not
        // fit. Subtracting them first is what makes the axis allowances honest
        // -- and it is the mechanism by which criterion 2's instruction ("do
        // not assume draw calls are the limit") can actually come out either
        // way: if they dominated, every axis allowance would be zero and
        // BudgetExceeded would fire on the first frame.
        f32 drawCallCost = 0.0f;
        u32 drawCalls = 0u;
        for (const AnimalScheduleSlot& slot : slots)
        {
            drawCallCost += model.PerDrawCall * static_cast<f32>(slot.Item.DrawCallCount);
            drawCalls += slot.Item.DrawCallCount;
            stats.ConsideredByRole[static_cast<sizet>(slot.Item.Role)] += 1u;
        }
        stats.DrawCallCostUnits = drawCallCost;
        stats.DrawCalls = drawCalls;

        const f32 schedulable = std::max(0.0f, policy.FrameBudgetUnits - drawCallCost);

        std::array<f32, AnimalWorkAxisCount> axisBudget{};
        std::array<f32, AnimalWorkAxisCount> axisCost{};
        // PER AXIS, not one flag read by everybody. The axes have separate
        // allowances, so "the budget could not be met" is a statement about ONE
        // of them -- and marking an animal CapHeld on its shadow axis because
        // the SIMULATION axis overflowed would attribute the pressure to the
        // wrong place, which is the one thing the outcome enum exists to get
        // right.
        std::array<bool, AnimalWorkAxisCount> axisExceeded{};
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            axisBudget[a] = schedulable * policy.AxisWeights[a];
            stats.AxisBudgetUnits[a] = axisBudget[a];

            f32 desiredCost = 0.0f;
            f32 startCost = 0.0f;
            for (sizet i = 0; i < count; ++i)
            {
                const AnimalWorkAxis axis = static_cast<AnimalWorkAxis>(a);
                desiredCost += AxisCostAtStep(model, slots[i].Item, axis,
                                              std::min(slots[i].Item.DesiredStep[a], kMaxBudgetSteps));
                startCost += AxisCostAtStep(model, slots[i].Item, axis, allocated[i][a]);
            }
            stats.DesiredCostUnits[a] = desiredCost;
            axisCost[a] = startCost;
        }

        // ── 3. The service order, computed once per axis ──────────────────
        //
        // The keys -- role, starvation, apparent size, UUID -- do not change
        // while the allocation runs, so the order is fixed for the frame and is
        // sorted once rather than re-scanned per coarsening. That is what keeps
        // this O(n log n + n * steps) instead of O(n^2 * steps) on a herd.
        for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
        {
            if (!(axisCost[a] > axisBudget[a]))
            {
                continue; // fits at the desired steps; nothing to take away
            }

            std::vector<ServiceOrder> order;
            order.reserve(count);
            for (sizet i = 0; i < count; ++i)
            {
                ServiceOrder entry;
                entry.Item = &slots[i].Item;
                entry.Index = static_cast<u32>(i);
                entry.StarvedFrames = slots[i].State != nullptr ? slots[i].State->StarvedFrames[a] : 0u;
                entry.PixelSize =
                    std::isfinite(slots[i].Item.PixelSize) ? std::max(slots[i].Item.PixelSize, 0.0f) : 0.0f;
                entry.Id = static_cast<u64>(slots[i].Item.Id);
                order.push_back(entry);
            }
            std::stable_sort(order.begin(), order.end(), GivesWayBefore);

            // ROLE GROUPS ARE EXHAUSTED IN TURN, not interleaved. Every
            // Background animal must be at its cap before a Featured one gives
            // way, which is what "preserve hero quality" means made operational
            // -- a herd that thins while the hero stays sharp, rather than a
            // population that degrades uniformly and takes the hero with it.
            for (sizet roleIndex = AnimalRoleCount; roleIndex-- > 0;)
            {
                const AnimalRole role = static_cast<AnimalRole>(roleIndex);
                if (role == AnimalRole::Hero && policy.ProtectHero)
                {
                    // Never a candidate. The budget is missed out loud instead.
                    continue;
                }
                if (!(axisCost[a] > axisBudget[a]))
                {
                    break;
                }

                // ROUND-ROBIN PASSES, one halving each, rather than driving the
                // first candidate to its cap before touching the second. Both
                // fit the budget; only this one spreads the loss, and an
                // allocation that took one background animal to a sixteenth
                // while its neighbour stayed at full rate is the within-frame
                // twin of the starvation the counters fix across frames.
                bool progressed = true;
                while (progressed && axisCost[a] > axisBudget[a])
                {
                    progressed = false;
                    for (const ServiceOrder& entry : order)
                    {
                        if (entry.Item->Role != role)
                        {
                            continue;
                        }
                        const sizet i = entry.Index;
                        const u32 cap = std::min(slots[i].Item.MaxStep[a], kMaxBudgetSteps);
                        if (allocated[i][a] >= cap)
                        {
                            continue;
                        }
                        const AnimalWorkAxis axis = static_cast<AnimalWorkAxis>(a);
                        const f32 before = AxisCostAtStep(model, slots[i].Item, axis, allocated[i][a]);
                        allocated[i][a] += 1u;
                        const f32 after = AxisCostAtStep(model, slots[i].Item, axis, allocated[i][a]);
                        axisCost[a] += (after - before);
                        progressed = true;
                        if (!(axisCost[a] > axisBudget[a]))
                        {
                            break;
                        }
                    }
                }
            }

            if (axisCost[a] > axisBudget[a])
            {
                // EVERY CANDIDATE IS AT ITS CAP AND THE AXIS STILL DOES NOT FIT.
                // Said out loud rather than absorbed: the alternative is a hero
                // quietly degraded to make a number work, which hides the one
                // thing the budget exists to protect.
                axisExceeded[a] = true;
                stats.BudgetExceeded = true;
            }
        }

        // ── 4. Hold, rate-limit, and advance every counter ────────────────
        for (sizet i = 0; i < count; ++i)
        {
            const AnimalWorkItem& item = slots[i].Item;
            AnimalScheduleState local;
            AnimalScheduleState& state = slots[i].State != nullptr ? *slots[i].State : local;

            AnimalSchedule schedule;
            schedule.Id = item.Id;
            schedule.PixelSize = item.PixelSize;
            schedule.Role = item.Role;

            bool coarsened = false;
            bool capHeld = false;
            bool held = false;

            for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
            {
                const u32 desired = std::min(item.DesiredStep[a], kMaxBudgetSteps);
                const u32 cap = std::min(item.MaxStep[a], kMaxBudgetSteps);

                // ONE HALVING PER HOLD WINDOW, and this clamp is the whole of
                // the "no abrupt motion changes" guarantee on the temporal
                // axes. Applying a three-step coarsening in one frame is a
                // factor-of-eight change in update rate between two consecutive
                // frames, which is precisely the discontinuity the criterion
                // names -- so the request is rate-limited before the hold sees
                // it, and the next window takes the next step.
                const u32 want = std::min(allocated[i][a], state.Step[a] + 1u);

                state.StableFrames[a] = (want == state.RequestedStep[a]) ? state.StableFrames[a] + 1u : 0u;
                state.RequestedStep[a] = want;

                const u32 before = state.Step[a];
                state.Step[a] = ApplyHold(state.Step[a], want, state.StableFrames[a], policy.HoldFrames);
                if (state.Step[a] != before)
                {
                    schedule.StepChanged = true;
                    stats.StepChanges += 1u;
                }
                if (want > state.Step[a])
                {
                    held = true;
                }

                if (state.Step[a] > desired)
                {
                    coarsened = true;
                }
                // AT ITS CAP WITH THE BUDGET STILL ASKING. Distinguished from a
                // plain coarsening because it is the signature of a population
                // the budget cannot serve, and telling the two apart is the
                // difference between tuning the budget and rebuilding the herd.
                if (allocated[i][a] >= cap && axisExceeded[a])
                {
                    capHeld = true;
                }

                // ── The starvation counter ────────────────────────────────
                //
                // Advanced on the step actually RUNNING, not on the one the
                // allocator picked: an animal held by hysteresis at a finer
                // step is not being starved, it is being protected, and
                // counting it as starved would make it jump the queue for work
                // it is already getting.
                state.StarvedFrames[a] = (state.Step[a] > desired) ? state.StarvedFrames[a] + 1u : 0u;
                stats.MaxStarvedFrames = std::max(stats.MaxStarvedFrames, state.StarvedFrames[a]);

                schedule.Step[a] = state.Step[a];
                schedule.Fraction[a] = AnimalStepFraction(state.Step[a]);
            }

            // The floors, counted rather than merely enforced: a reader needs to
            // know the budget WANTED to go further and was refused, because that
            // is the tell that the population has outgrown the frame.
            {
                constexpr sizet vis = static_cast<sizet>(AnimalWorkAxis::Visibility);
                const u32 visCap = std::min(item.MaxStep[vis], kMaxBudgetSteps);
                const u32 visDesired = std::min(item.DesiredStep[vis], kMaxBudgetSteps);
                // BOTH conditions, and the second is the one that makes the
                // counter mean its name: at the cap because the BUDGET pushed
                // it there. An animal whose own distance ladder happens to land
                // on the cap is not being held off the floor by anything, and
                // counting it would make this number rise with distance rather
                // than with pressure -- which is the opposite of the signal.
                if (item.Visible && item.StrandCount > 0u && policy.MinVisibleStrands > 0u &&
                    schedule.Step[vis] >= visCap && schedule.Step[vis] > visDesired)
                {
                    stats.AnimalsAtVisibilityFloor += 1u;
                }
            }
            if (item.MaxStep[static_cast<sizet>(AnimalWorkAxis::Deformation)] == 0u)
            {
                stats.AnimalsAtPoseStepCap += 1u;
            }

            schedule.Outcome = AnimalBudgetOutcome::AtDesired;
            if (capHeld)
            {
                schedule.Outcome = AnimalBudgetOutcome::CapHeld;
            }
            else if (coarsened)
            {
                schedule.Outcome = AnimalBudgetOutcome::Coarsened;
            }
            else if (held)
            {
                schedule.Outcome = AnimalBudgetOutcome::HeldByHysteresis;
            }
            state.Outcome = schedule.Outcome;

            switch (schedule.Outcome)
            {
                case AnimalBudgetOutcome::AtDesired:
                    stats.AnimalsAtDesired += 1u;
                    break;
                case AnimalBudgetOutcome::Coarsened:
                    stats.AnimalsCoarsened += 1u;
                    stats.CoarsenedByRole[static_cast<sizet>(item.Role)] += 1u;
                    break;
                case AnimalBudgetOutcome::CapHeld:
                    stats.AnimalsCapHeld += 1u;
                    stats.CoarsenedByRole[static_cast<sizet>(item.Role)] += 1u;
                    break;
                case AnimalBudgetOutcome::HeldByHysteresis:
                    stats.AnimalsHeldByHysteresis += 1u;
                    break;
                case AnimalBudgetOutcome::NotScheduled:
                case AnimalBudgetOutcome::Count:
                    break;
            }

            schedule.EstimatedCostUnits = EstimateAnimalCostUnits(model, item, schedule.Step);
            stats.EstimatedCostUnits += schedule.EstimatedCostUnits;
            for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
            {
                stats.ScheduledCostUnits[a] +=
                    AxisCostAtStep(model, item, static_cast<AnimalWorkAxis>(a), schedule.Step[a]);
            }

            results.push_back(schedule);
        }

        // The share each axis carries, which is the answer to criterion 2 in a
        // form a reader can act on. Reported over the SCHEDULED cost including
        // draw calls, so the unreachable share is visible beside the reachable
        // ones instead of being normalised away.
        const f32 total = stats.EstimatedCostUnits;
        if (total > 0.0f)
        {
            for (sizet a = 0; a < AnimalWorkAxisCount; ++a)
            {
                stats.AxisShareOfScheduled[a] = stats.ScheduledCostUnits[a] / total;
            }
        }

        if (outStats != nullptr)
        {
            *outStats = stats;
        }
        return results;
    }
} // namespace OloEngine
