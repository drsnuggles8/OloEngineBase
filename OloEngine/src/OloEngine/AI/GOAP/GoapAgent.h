#pragma once

#include "OloEngine/AI/GOAP/GoapAction.h"
#include "OloEngine/AI/GOAP/GoapGoal.h"
#include "OloEngine/AI/GOAP/GoapPlanner.h"
#include "OloEngine/AI/GOAP/GoapWorldState.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <functional>
#include <string>
#include <vector>

namespace OloEngine
{
    // Runtime owner of a GOAP brain for one agent: a set of available actions,
    // a prioritised list of goals, and the agent's current belief about the
    // world. Each Update it (re)plans toward the most important relevant goal
    // and drives the current plan one step, replanning when a step fails or the
    // world drifts out from under the plan.
    //
    // The agent is deliberately engine-agnostic: it never touches Entity, Scene
    // or a blackboard directly. Game code wires those in through the action
    // hooks (OnEnter/Perform/IsUsable closures) and an optional Sensor that
    // refreshes the world state from the real world each tick.
    class GoapAgent : public RefCounted
    {
      public:
        // The sensor runs at the top of every Update to fold fresh observations
        // into the world state before planning/execution. Optional.
        using SensorFn = std::function<void(GoapWorldState&)>;

        GoapAgent() = default;
        ~GoapAgent() override = default;

        void AddAction(GoapAction action)
        {
            m_Actions.push_back(std::move(action));
        }
        void AddGoal(GoapGoal goal)
        {
            m_Goals.push_back(std::move(goal));
        }
        void SetActions(std::vector<GoapAction> actions)
        {
            m_Actions = std::move(actions);
            Abort();
        }
        void SetGoals(std::vector<GoapGoal> goals)
        {
            m_Goals = std::move(goals);
            Abort();
        }
        void SetSensor(SensorFn sensor)
        {
            m_Sensor = std::move(sensor);
        }

        [[nodiscard]] GoapWorldState& WorldState()
        {
            return m_WorldState;
        }
        [[nodiscard]] const GoapWorldState& WorldState() const
        {
            return m_WorldState;
        }

        void SetFact(const std::string& key, GoapWorldState::Value value)
        {
            m_WorldState.Set(key, std::move(value));
        }

        // Advance the brain by one tick.
        void Update(f32 dt);

        // Drop the current plan and force a fresh plan on the next Update.
        void Abort()
        {
            m_Plan = {};
            m_Step = 0;
            m_CurrentGoal.clear();
            m_NeedsReplan = true;
        }

        // Force a replan next tick without discarding what's known about the world.
        void Invalidate()
        {
            m_NeedsReplan = true;
        }

        [[nodiscard]] const GoapPlan& CurrentPlan() const
        {
            return m_Plan;
        }
        [[nodiscard]] const std::string& CurrentGoalName() const
        {
            return m_CurrentGoal;
        }
        [[nodiscard]] sizet CurrentStepIndex() const
        {
            return m_Step;
        }
        [[nodiscard]] bool HasPlan() const
        {
            return m_Plan.Found && !m_Plan.Steps.empty();
        }
        [[nodiscard]] const std::vector<GoapAction>& Actions() const
        {
            return m_Actions;
        }
        [[nodiscard]] const std::vector<GoapGoal>& Goals() const
        {
            return m_Goals;
        }

        // Count of completed Update ticks that ran a plan to completion. Handy
        // for tests and debug overlays.
        [[nodiscard]] u32 GoalsAchieved() const
        {
            return m_GoalsAchieved;
        }

        GoapPlannerSettings PlannerSettings;

      private:
        // Choose the best relevant, unsatisfied goal and plan toward it. Returns
        // true and sets up m_Plan/m_CurrentGoal/m_Step when a plan is found.
        bool SelectGoalAndPlan();

        std::vector<GoapAction> m_Actions;
        std::vector<GoapGoal> m_Goals;
        GoapWorldState m_WorldState;
        SensorFn m_Sensor;

        GoapPlan m_Plan;
        sizet m_Step = 0;
        std::string m_CurrentGoal;
        bool m_NeedsReplan = true;
        u32 m_GoalsAchieved = 0;
    };
} // namespace OloEngine
