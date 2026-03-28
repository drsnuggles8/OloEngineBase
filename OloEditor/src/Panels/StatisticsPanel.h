#pragma once

#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
    class StatisticsPanel
    {
      public:
        StatisticsPanel() = default;

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
        void DrawRendererTab();
        void DrawAudioTab();
        void DrawPerformanceTab();
        void DrawMemoryTab();

        Ref<Scene> m_Context;
        Entity m_HoveredEntity;
    };
} // namespace OloEngine
