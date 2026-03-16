#pragma once

#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
    class SceneStatisticsPanel
    {
      public:
        SceneStatisticsPanel() = default;

        void SetContext(const Ref<Scene>& context)
        {
            m_Context = context;
        }
        void SetHoveredEntity(Entity entity)
        {
            m_HoveredEntity = entity;
        }
        void OnImGuiRender(bool* p_open = nullptr);

      private:
        Ref<Scene> m_Context;
        Entity m_HoveredEntity;
    };
} // namespace OloEngine
