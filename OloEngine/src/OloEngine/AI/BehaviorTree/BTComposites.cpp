#include "OloEnginePCH.h"
#include "BTComposites.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
    // --- BTSequence ---

    BTStatus BTSequence::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        for (; m_CurrentChild < static_cast<u32>(Children.size()); ++m_CurrentChild)
        {
            BTStatus status = Children[m_CurrentChild]->Tick(dt, blackboard, entity);
            if (status == BTStatus::Running)
            {
                return BTStatus::Running;
            }
            if (status == BTStatus::Failure)
            {
                Reset();
                return BTStatus::Failure;
            }
        }
        Reset();
        return BTStatus::Success;
    }

    void BTSequence::Reset()
    {
        OLO_PROFILE_FUNCTION();

        m_CurrentChild = 0;
        for (auto& child : Children)
        {
            child->Reset();
        }
    }

    // --- BTSelector ---

    BTStatus BTSelector::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        for (; m_CurrentChild < static_cast<u32>(Children.size()); ++m_CurrentChild)
        {
            BTStatus status = Children[m_CurrentChild]->Tick(dt, blackboard, entity);
            if (status == BTStatus::Running)
            {
                return BTStatus::Running;
            }
            if (status == BTStatus::Success)
            {
                Reset();
                return BTStatus::Success;
            }
        }
        Reset();
        return BTStatus::Failure;
    }

    void BTSelector::Reset()
    {
        OLO_PROFILE_FUNCTION();

        m_CurrentChild = 0;
        for (auto& child : Children)
        {
            child->Reset();
        }
    }

    // --- BTParallel ---

    BTStatus BTParallel::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        u32 successCount = 0;
        u32 failureCount = 0;

        for (auto& child : Children)
        {
            BTStatus status = child->Tick(dt, blackboard, entity);
            if (status == BTStatus::Success)
            {
                ++successCount;
            }
            else if (status == BTStatus::Failure)
            {
                ++failureCount;
            }
        }

        if (FailurePolicy == Policy::RequireOne && failureCount > 0)
        {
            Reset();
            return BTStatus::Failure;
        }
        if (FailurePolicy == Policy::RequireAll && failureCount == static_cast<u32>(Children.size()))
        {
            Reset();
            return BTStatus::Failure;
        }
        if (SuccessPolicy == Policy::RequireOne && successCount > 0)
        {
            Reset();
            return BTStatus::Success;
        }
        if (SuccessPolicy == Policy::RequireAll && successCount == static_cast<u32>(Children.size()))
        {
            Reset();
            return BTStatus::Success;
        }

        return BTStatus::Running;
    }

    void BTParallel::Reset()
    {
        OLO_PROFILE_FUNCTION();

        for (auto& child : Children)
        {
            child->Reset();
        }
    }
} // namespace OloEngine
