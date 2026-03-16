#pragma once

#include "OloEngine/Scene/Scene.h"

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
        void OnImGuiRender();

      private:
        Ref<Scene> m_Context;
    };
} // namespace OloEngine
