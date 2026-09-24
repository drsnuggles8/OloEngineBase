#pragma once

// Pure JSON shaping behind olo_groom_budget_stats (issue #1258).
//
// WHY THIS TOOL EXISTS. Verifying the multi-animal budget on a live editor meant
// grepping OloEngine.log for `GroomRenderPass: built strand geometry ... (stride
// N)` lines and counting them by hand, because the pass's own counters and the
// population scheduler's decisions were not reachable from outside the process.
// That works — the strand build IS logged — but it only shows the builds that
// MISSED the geometry cache, so a steady-state frame reports nothing at all and
// the numbers you get depend on how long the session has been running.
//
// TWO BLOCKS, AND THE SPLIT IS THE POINT. `groom` is what the pass DREW this
// frame; `animalBudget` is what the scheduler DECIDED before it. A reader
// looking at a thinned coat needs both, because the same picture is produced by
// "the budget coarsened this animal" and by "the budget never saw this animal"
// — and those have completely different fixes. AnimalsConsidered == 0 next to a
// thinned coat means the coat's own distance ladder did it and the population
// budget is not involved.
//
// Every count here describes the LAST FRAME. Unlike the deformation stats
// beside it there are no session totals: the scheduler holds no rare deliberate
// events, and its one "something is wrong" signal (budgetExceeded) is a property
// of the frame you are looking at rather than of the session.

#include "MCP/McpStatsSnapshot.h"

#include <array>
#include <string>
#include <vector>

namespace OloEngine::MCP::GroomBudgetStats
{
    using Json = nlohmann::json;

    struct RepresentationRow
    {
        std::string Name;
        u32 Grooms = 0;
        u32 Strands = 0;
        u64 Bytes = 0;
    };

    struct AnimalAxisRow
    {
        std::string Name;
        f32 DesiredCostUnits = 0.0f;
        f32 ScheduledCostUnits = 0.0f;
        f32 BudgetUnits = 0.0f;
        f32 ShareOfScheduled = 0.0f;
    };

    struct Snapshot
    {
        StatsSnapshot::State State;

        // ── What the pass drew ────────────────────────────────────────
        u32 GroomsSubmitted = 0;
        u32 StrandsDrawn = 0;
        u32 SegmentsDrawn = 0;
        u32 GroomsOnSelectedTier = 0;
        u32 GroomsFellBack = 0;
        u32 RepresentationChanges = 0;
        u32 GroomsAtCompensationCap = 0;
        f32 MaxWidthCompensation = 1.0f;
        std::string DominantFallbackReason = "None";
        std::vector<RepresentationRow> Representations;

        // ── Coat self-shadowing (#1248, #1426) ───────────────────────
        u32 CoatShadowed = 0;
        u32 CoatFallback = 0;
        u32 CoatUnshadowedByChoice = 0;
        std::string CoatDominantFallbackReason = "None";
        u32 CoatRebuilds = 0;
        u32 CoatDeformedRebakes = 0;
        f32 CoatMaxDriftVoxels = 0.0f;
        u64 CoatBakeMicroseconds = 0;
        u32 CoatResolutionInForce = 0;
        u64 CoatResidentBytes = 0;

        // ── What the scheduler decided ────────────────────────────────
        bool BudgetEnabled = false;
        u32 AnimalsConsidered = 0;
        u32 AnimalsAtDesired = 0;
        u32 AnimalsCoarsened = 0;
        u32 AnimalsCapHeld = 0;
        u32 AnimalsHeldByHysteresis = 0;
        u32 HeroesConsidered = 0;
        u32 HeroesCoarsened = 0;
        u32 MaxStarvedFrames = 0;
        u32 AnimalsAtVisibilityFloor = 0;
        u32 AnimalsAtPoseStepCap = 0;
        u32 StepChanges = 0;
        u32 DrawCalls = 0;
        f32 DrawCallCostUnits = 0.0f;
        f32 EstimatedCostUnits = 0.0f;
        f32 FrameBudgetUnits = 0.0f;
        bool BudgetExceeded = false;
        std::vector<AnimalAxisRow> Axes;

        // ── The frame-time window (#1258 criterion 4) ─────────────────
        u32 FrameSamples = 0;
        f32 P50Ms = 0.0f;
        f32 P95Ms = 0.0f;
        f32 P99Ms = 0.0f;
        f32 MaxMs = 0.0f;
        f32 MeanMs = 0.0f;
        u32 OverBudgetFrames = 0;
        f32 FrameBudgetMs = 0.0f;
    };

    [[nodiscard("this builds the response; it does not send it")]] inline Json BuildReport(const Snapshot& snapshot)
    {
        Json out = StatsSnapshot::ToJson(snapshot.State);
        if (StatsSnapshot::Status(snapshot.State) != "ready")
            return out;

        Json representations = Json::array();
        for (const RepresentationRow& row : snapshot.Representations)
        {
            representations.push_back(Json{
                { "representation", row.Name },
                { "grooms", row.Grooms },
                { "strands", row.Strands },
                { "bytes", row.Bytes },
            });
        }

        out["groom"] = Json{
            { "groomsSubmitted", snapshot.GroomsSubmitted },
            { "strandsDrawn", snapshot.StrandsDrawn },
            { "segmentsDrawn", snapshot.SegmentsDrawn },
            { "groomsOnSelectedTier", snapshot.GroomsOnSelectedTier },
            { "groomsFellBack", snapshot.GroomsFellBack },
            // A counter that stays near groomsSubmitted IS thrashing — the
            // representation is changing almost every frame for almost every
            // coat, which the hold and the hysteresis exist to prevent.
            { "representationChanges", snapshot.RepresentationChanges },
            { "groomsAtCompensationCap", snapshot.GroomsAtCompensationCap },
            { "maxWidthCompensation", snapshot.MaxWidthCompensation },
            { "dominantFallbackReason", snapshot.DominantFallbackReason },
            { "byRepresentation", representations },
        };

        // Whether a coat asked for self-shadowing got it, and -- for a coat
        // bound to a moving body (#1426) -- what following the pose cost this
        // frame. `shadowed` is the live half of a Vulkan cell's evidence: the
        // headless suite cannot run on Vulkan, so this is the only place the
        // decision can be read there.
        out["coatShadow"] = Json{
            { "shadowed", snapshot.CoatShadowed },
            { "fallback", snapshot.CoatFallback },
            { "unshadowedByChoice", snapshot.CoatUnshadowedByChoice },
            { "dominantFallbackReason", snapshot.CoatDominantFallbackReason },
            { "rebuilds", snapshot.CoatRebuilds },
            { "deformedRebakes", snapshot.CoatDeformedRebakes },
            { "maxDriftVoxels", snapshot.CoatMaxDriftVoxels },
            { "bakeMicroseconds", snapshot.CoatBakeMicroseconds },
            { "resolutionInForce", snapshot.CoatResolutionInForce },
            { "residentBytes", snapshot.CoatResidentBytes },
        };

        Json axes = Json::array();
        for (const AnimalAxisRow& row : snapshot.Axes)
        {
            axes.push_back(Json{
                { "axis", row.Name },
                { "desiredCostUnits", row.DesiredCostUnits },
                { "scheduledCostUnits", row.ScheduledCostUnits },
                { "budgetUnits", row.BudgetUnits },
                { "shareOfScheduled", row.ShareOfScheduled },
            });
        }

        out["animalBudget"] = Json{
            // FIRST, because it is the question every other number here is
            // conditional on. `false` means nothing below was decided by the
            // population budget at all, and a thinned coat is its own distance
            // ladder's doing.
            { "enabled", snapshot.BudgetEnabled },
            { "animalsConsidered", snapshot.AnimalsConsidered },
            { "animalsAtDesired", snapshot.AnimalsAtDesired },
            { "animalsCoarsened", snapshot.AnimalsCoarsened },
            { "animalsCapHeld", snapshot.AnimalsCapHeld },
            { "animalsHeldByHysteresis", snapshot.AnimalsHeldByHysteresis },
            // The hero contract as a number rather than a claim: with
            // ProtectHero set, heroesCoarsened must be 0.
            { "heroesConsidered", snapshot.HeroesConsidered },
            { "heroesCoarsened", snapshot.HeroesCoarsened },
            // PRESSURE, not unfairness — it grows for every animal when nothing
            // can be served. The unfairness condition is relative (a same-role
            // peer at its desired step while this one is below it).
            { "maxStarvedFrames", snapshot.MaxStarvedFrames },
            { "animalsAtVisibilityFloor", snapshot.AnimalsAtVisibilityFloor },
            { "animalsAtPoseStepCap", snapshot.AnimalsAtPoseStepCap },
            { "stepChanges", snapshot.StepChanges },
            { "drawCalls", snapshot.DrawCalls },
            { "drawCallCostUnits", snapshot.DrawCallCostUnits },
            { "estimatedCostUnits", snapshot.EstimatedCostUnits },
            { "frameBudgetUnits", snapshot.FrameBudgetUnits },
            // Every animal that could give way is at its cap and the frame
            // still does not fit. The hero is left at full rate rather than
            // quietly softened, so this is the only way that state is visible.
            { "budgetExceeded", snapshot.BudgetExceeded },
            { "byAxis", axes },
        };

        out["frameTime"] = Json{
            { "samples", snapshot.FrameSamples },
            { "meanMs", snapshot.MeanMs },
            { "p50Ms", snapshot.P50Ms },
            { "p95Ms", snapshot.P95Ms },
            { "p99Ms", snapshot.P99Ms },
            { "maxMs", snapshot.MaxMs },
            // A COUNT beside the percentiles: at a 600-sample window p99 is six
            // frames, so the percentile alone cannot tell one bad frame from
            // six — and six is a visible stutter while one is not.
            { "overBudgetFrames", snapshot.OverBudgetFrames },
            { "budgetMs", snapshot.FrameBudgetMs },
        };
        return out;
    }
} // namespace OloEngine::MCP::GroomBudgetStats
