#include "OloEnginePCH.h"
#include "OloEngine/AI/GOAP/GoapPlanner.h"

#include <limits>
#include <queue>
#include <unordered_map>

namespace OloEngine
{
    namespace
    {
        // A node in the search arena. States are stored by value; the arena owns
        // them and child nodes point back at their parent by index so the path
        // can be reconstructed without per-node heap allocation.
        struct SearchNode
        {
            GoapWorldState State;
            f32 G = 0.0f;       // cost from start
            f32 F = 0.0f;       // G + weighted heuristic
            i32 Parent = -1;    // index into the arena, -1 for the root
            i32 ActionIdx = -1; // action taken from Parent to reach here
            u32 Depth = 0;      // plan length so far (cap with MaxPlanLength)
        };

        // Min-heap entry: order by F, with G as a stable tie-breaker so deeper,
        // more-progressed nodes win ties (mild but helps determinism).
        struct OpenEntry
        {
            f32 F;
            f32 G;
            i32 NodeIdx;
        };

        struct OpenGreater
        {
            bool operator()(const OpenEntry& a, const OpenEntry& b) const
            {
                if (a.F > b.F)
                    return true;
                if (a.F < b.F)
                    return false;
                return a.G > b.G;
            }
        };

        // Hash a world state for the closed set. The map keys on the full state
        // (not the raw hash) so two distinct states that happen to collide land
        // in the same bucket and are separated by GoapWorldState::operator==,
        // rather than one silently pruning the other.
        struct WorldStateHash
        {
            sizet operator()(const GoapWorldState& s) const
            {
                return static_cast<sizet>(s.Hash());
            }
        };
    } // namespace

    GoapPlan GoapPlanner::Plan(const GoapWorldState& start,
                               const GoapGoal& goal,
                               const std::vector<GoapAction>& actions,
                               const GoapPlannerSettings& settings,
                               Stats* outStats)
    {
        OLO_PROFILE_FUNCTION();

        GoapPlan plan;
        Stats stats;

        // Goal already met → trivially found with an empty plan.
        if (start.Satisfies(goal.DesiredState))
        {
            plan.Found = true;
            if (outStats)
                *outStats = stats;
            return plan;
        }

        std::vector<SearchNode> arena;
        arena.reserve(64);

        // Best known cost-to-reach for each finalised state. Keyed by the full
        // GoapWorldState (collision-safe via operator==). The strict-less check
        // below both prunes worse revisits and makes zero-cost no-op actions
        // self-terminating (a state can never improve on itself).
        std::unordered_map<GoapWorldState, f32, WorldStateHash> bestG;

        std::priority_queue<OpenEntry, std::vector<OpenEntry>, OpenGreater> open;

        const auto heuristic = [&](const GoapWorldState& s) -> f32
        {
            return static_cast<f32>(s.UnsatisfiedCount(goal.DesiredState)) * settings.HeuristicWeight;
        };

        // Seed with the start state.
        {
            SearchNode root;
            root.State = start;
            root.G = 0.0f;
            root.F = heuristic(start);
            arena.push_back(std::move(root));
            bestG[start] = 0.0f;
            open.push(OpenEntry{ arena[0].F, 0.0f, 0 });
        }

        i32 goalNodeIdx = -1;
        u32 iterations = 0;

        while (!open.empty())
        {
            if (iterations >= settings.MaxIterations)
            {
                stats.HitIterationCap = true;
                break;
            }

            const OpenEntry top = open.top();
            open.pop();
            const i32 currentIdx = top.NodeIdx;

            // Lazy deletion: skip entries made stale by a cheaper path to the
            // same state discovered after this one was queued.
            if (auto it = bestG.find(arena[static_cast<sizet>(currentIdx)].State);
                it != bestG.end() && top.G > it->second)
                continue;

            ++iterations;
            ++stats.NodesExpanded;

            // Copy out the fields we need; `arena` may reallocate on push below,
            // which would dangle a reference into it.
            const GoapWorldState currentState = arena[static_cast<sizet>(currentIdx)].State;
            const f32 currentG = arena[static_cast<sizet>(currentIdx)].G;
            const u32 currentDepth = arena[static_cast<sizet>(currentIdx)].Depth;

            if (currentState.Satisfies(goal.DesiredState))
            {
                goalNodeIdx = currentIdx;
                break;
            }

            if (currentDepth >= settings.MaxPlanLength)
                continue;

            for (i32 a = 0; a < static_cast<i32>(actions.size()); ++a)
            {
                const GoapAction& action = actions[static_cast<sizet>(a)];

                if (!action.CheckUsable())
                    continue;
                if (!currentState.Satisfies(action.Preconditions))
                    continue;

                GoapWorldState next = currentState;
                next.ApplyEffects(action.Effects);

                const f32 stepCost = action.Cost < 0.0f ? 0.0f : action.Cost;
                const f32 tentativeG = currentG + stepCost;

                if (auto it = bestG.find(next); it != bestG.end() && !(tentativeG < it->second))
                    continue; // no improvement over a known path to this state

                bestG[next] = tentativeG;

                SearchNode child;
                child.G = tentativeG;
                child.F = tentativeG + heuristic(next);
                child.Parent = currentIdx;
                child.ActionIdx = a;
                child.Depth = currentDepth + 1;
                child.State = std::move(next);

                const i32 childIdx = static_cast<i32>(arena.size());
                arena.push_back(std::move(child));
                open.push(OpenEntry{ arena[static_cast<sizet>(childIdx)].F, tentativeG, childIdx });
                ++stats.NodesGenerated;
            }
        }

        if (goalNodeIdx >= 0)
        {
            plan.Found = true;
            plan.TotalCost = arena[static_cast<sizet>(goalNodeIdx)].G;

            // Walk parents back to the root, collecting action indices, then
            // reverse into execution order.
            std::vector<i32> actionChain;
            for (i32 idx = goalNodeIdx; idx >= 0; idx = arena[static_cast<sizet>(idx)].Parent)
            {
                const i32 actionIdx = arena[static_cast<sizet>(idx)].ActionIdx;
                if (actionIdx >= 0)
                    actionChain.push_back(actionIdx);
            }
            plan.Steps.reserve(actionChain.size());
            for (auto it = actionChain.rbegin(); it != actionChain.rend(); ++it)
            {
                GoapAction step = actions[static_cast<sizet>(*it)];
                step.ResetRuntime();
                plan.Steps.push_back(std::move(step));
            }
        }

        if (outStats)
            *outStats = stats;
        return plan;
    }
} // namespace OloEngine
