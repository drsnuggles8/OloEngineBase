#pragma once

#include "OloEngine/AI/GOAP/GoapWorldState.h"
#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>

namespace OloEngine
{
    enum class GoapActionStatus : u8
    {
        Running, // still executing — call again next tick
        Success, // finished; effects are now true in the world
        Failure  // could not complete; the agent should replan
    };

    // A single planner operator plus its optional runtime behaviour.
    //
    // The *planning* half — Name, Cost, Preconditions, Effects — is pure data
    // and is all the GoapPlanner ever touches, which keeps the planner free of
    // any engine/scene dependency and trivially unit-testable.
    //
    // The *execution* half — IsUsable / OnEnter / Perform — is a set of optional
    // std::function hooks the GoapAgent drives. They capture whatever context
    // the game needs (the owning entity, a blackboard, scene services) in their
    // closures, so neither this struct nor the planner has to know about them.
    // An action with no hooks is an instantaneous symbolic operator: it succeeds
    // the moment it is reached and its Effects are applied. That default is what
    // makes the planner testable in isolation.
    struct GoapAction
    {
        std::string Name;
        f32 Cost = 1.0f;              // must be >= 0; the planner floors negatives at 0
        GoapWorldState Preconditions; // must hold before the action can run
        GoapWorldState Effects;       // become true once the action succeeds

        // Optional dynamic availability gate consulted during planning. When set
        // and it returns false, the action is excluded from the search this plan
        // (e.g. "no path to target right now"). Symbolic Preconditions still apply
        // on top of this.
        std::function<bool()> IsUsable;

        // Optional one-shot hook fired the first tick the agent begins this step.
        std::function<void()> OnEnter;

        // Optional per-tick driver. Returns Running until the work is done, then
        // Success (the agent applies Effects) or Failure (the agent replans).
        // Unset == an instantaneous action that succeeds immediately.
        std::function<GoapActionStatus(f32 /*dt*/)> Perform;

        [[nodiscard]] bool CheckUsable() const
        {
            return !IsUsable || IsUsable();
        }

        // Drive one tick of execution, firing OnEnter exactly once on entry.
        GoapActionStatus Execute(f32 dt)
        {
            if (!m_Entered)
            {
                m_Entered = true;
                if (OnEnter)
                    OnEnter();
            }
            if (Perform)
                return Perform(dt);
            return GoapActionStatus::Success;
        }

        // Clear per-run execution state so a freshly planned copy starts clean.
        void ResetRuntime()
        {
            m_Entered = false;
        }

      private:
        bool m_Entered = false;
    };
} // namespace OloEngine
