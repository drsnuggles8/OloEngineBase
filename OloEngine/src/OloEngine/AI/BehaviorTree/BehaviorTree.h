#pragma once

#include "OloEngine/AI/BehaviorTree/BTNode.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    class Entity;

    class BehaviorTree : public RefCounted
    {
      public:
        BehaviorTree() = default;
        ~BehaviorTree() override = default;

        void SetRoot(Ref<BTNode> root)
        {
            m_Root = std::move(root);
        }

        [[nodiscard]] Ref<BTNode> GetRoot() const
        {
            return m_Root;
        }

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity);

        void Reset()
        {
            if (m_Root)
            {
                m_Root->Reset();
            }
        }

      private:
        Ref<BTNode> m_Root;
    };
} // namespace OloEngine
