#include "OloEnginePCH.h"
#include "BTPerceptionNodes.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
    // --- BTCanSeeTarget ---

    BTStatus BTCanSeeTarget::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        const bool canSee = entity.HasComponent<PerceptionComponent>() &&
                            entity.GetComponent<PerceptionComponent>().HasVisibleTarget;

        if (!canSee)
        {
            if (!Children.empty())
            {
                Children[0]->Reset();
            }
            return BTStatus::Failure;
        }

        // Visible: leaf condition succeeds; guard form runs its child.
        if (Children.empty())
        {
            return BTStatus::Success;
        }

        return Children[0]->Tick(dt, blackboard, entity);
    }

    void BTCanSeeTarget::Reset()
    {
        OLO_PROFILE_FUNCTION();

        for (auto& child : Children)
        {
            child->Reset();
        }
    }
} // namespace OloEngine
