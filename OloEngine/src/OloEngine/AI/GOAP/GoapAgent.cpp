#include "OloEnginePCH.h"
#include "OloEngine/AI/GOAP/GoapAgent.h"

#include <algorithm>

namespace OloEngine
{
    bool GoapAgent::SelectGoalAndPlan()
    {
        // Visit goals strongest-first; the first relevant, not-yet-satisfied goal
        // that yields a plan wins. Ties in priority keep author order.
        std::vector<const GoapGoal*> ordered;
        ordered.reserve(m_Goals.size());
        for (const auto& goal : m_Goals)
            ordered.push_back(&goal);
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const GoapGoal* a, const GoapGoal* b)
                         { return a->Priority > b->Priority; });

        for (const GoapGoal* goal : ordered)
        {
            if (!goal->CheckValid(m_WorldState))
                continue;
            if (goal->IsSatisfiedBy(m_WorldState))
                continue;

            GoapPlan plan = GoapPlanner::Plan(m_WorldState, *goal, m_Actions, PlannerSettings);
            if (plan.Found && !plan.Steps.empty())
            {
                m_Plan = std::move(plan);
                m_CurrentGoal = goal->Name;
                m_Step = 0;
                return true;
            }
        }

        // Nothing actionable right now.
        m_Plan = {};
        m_CurrentGoal.clear();
        m_Step = 0;
        return false;
    }

    const GoapGoal* GoapAgent::FindGoal(const std::string& name) const
    {
        for (const auto& goal : m_Goals)
        {
            if (goal.Name == name)
                return &goal;
        }
        return nullptr;
    }

    void GoapAgent::Update(f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Sensor)
            m_Sensor(m_WorldState);

        // The sensor may have changed the world so the active plan's goal is
        // already met or no longer relevant (or the goal was removed). Drop the
        // in-flight plan and reconsider rather than finishing a now-pointless one.
        if (!m_NeedsReplan && HasPlan())
        {
            const GoapGoal* goal = FindGoal(m_CurrentGoal);
            if (!goal || goal->IsSatisfiedBy(m_WorldState) || !goal->CheckValid(m_WorldState))
                m_NeedsReplan = true;
        }

        if (m_NeedsReplan || !HasPlan())
        {
            SelectGoalAndPlan();
            m_NeedsReplan = false;
        }

        if (!HasPlan())
            return;

        // HasPlan() guarantees a non-empty Steps list and, because a plan is
        // Abort()ed the moment its final step succeeds, m_Step always indexes a
        // live step here.
        GoapAction& action = m_Plan.Steps[m_Step];

        // Guard against a world that drifted out from under the plan: if the
        // next step's symbolic preconditions no longer hold, or a dynamic
        // availability gate closed, abandon the plan and replan next tick.
        if (!action.CheckUsable() || !m_WorldState.Satisfies(action.Preconditions))
        {
            m_NeedsReplan = true;
            return;
        }

        switch (action.Execute(dt))
        {
            case GoapActionStatus::Success:
                // Optimistically fold the action's effects into the world state
                // so later steps see them even before the next sensor pass.
                m_WorldState.ApplyEffects(action.Effects);
                ++m_Step;
                if (m_Step >= m_Plan.Steps.size())
                {
                    ++m_GoalsAchieved;
                    Abort();
                }
                break;

            case GoapActionStatus::Failure:
                m_NeedsReplan = true;
                break;

            case GoapActionStatus::Running:
                // Keep ticking the same step next Update.
                break;
        }
    }
} // namespace OloEngine
