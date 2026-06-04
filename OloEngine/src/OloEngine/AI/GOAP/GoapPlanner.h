#pragma once

#include "OloEngine/AI/GOAP/GoapAction.h"
#include "OloEngine/AI/GOAP/GoapGoal.h"
#include "OloEngine/AI/GOAP/GoapWorldState.h"
#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    struct GoapPlannerSettings
    {
        // Hard cap on node expansions — guarantees termination even for an
        // ill-posed problem. 1000 is comfortably above any sane game plan.
        u32 MaxIterations = 1000;

        // Hard cap on plan depth (number of actions). Bounds the search and
        // stops a pathological action set from chaining forever.
        u32 MaxPlanLength = 32;

        // Heuristic multiplier on the count of unsatisfied goal conditions.
        //   1.0 (default) — the textbook GOAP heuristic: fast, finds low-cost
        //                   plans, optimal when no single action satisfies more
        //                   than one outstanding goal condition.
        //   0.0           — pure Dijkstra: slower but provably minimum-cost.
        //   > 1.0         — greedier/faster, may return a costlier plan.
        f32 HeuristicWeight = 1.0f;
    };

    struct GoapPlan
    {
        std::vector<GoapAction> Steps; // ordered actions to execute
        f32 TotalCost = 0.0f;
        bool Found = false; // a goal already satisfied at start yields Found + empty Steps

        [[nodiscard]] bool Empty() const
        {
            return Steps.empty();
        }
        [[nodiscard]] sizet Length() const
        {
            return Steps.size();
        }
    };

    // Forward A* over symbolic world states. Searches from `start`, applying the
    // Effects of any action whose Preconditions (and IsUsable gate) currently
    // hold, until a state satisfies the goal — then walks the parent chain back
    // into an ordered plan. The planner only ever reads the *data* half of a
    // GoapAction, so it has no engine dependencies and is fully unit-testable.
    class GoapPlanner
    {
      public:
        struct Stats
        {
            u32 NodesExpanded = 0;  // states popped and goal-tested
            u32 NodesGenerated = 0; // successor states pushed
            bool HitIterationCap = false;
        };

        [[nodiscard]] static GoapPlan Plan(const GoapWorldState& start,
                                           const GoapGoal& goal,
                                           const std::vector<GoapAction>& actions,
                                           const GoapPlannerSettings& settings = {},
                                           Stats* outStats = nullptr);
    };
} // namespace OloEngine
