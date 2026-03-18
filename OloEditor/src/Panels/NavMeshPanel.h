#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Navigation/NavMeshSettings.h"

namespace OloEngine
{
    class Scene;

    class NavMeshPanel
    {
      public:
        NavMeshPanel() = default;
        explicit NavMeshPanel(const Ref<Scene>& context);

        void SetContext(const Ref<Scene>& context);
        void OnImGuiRender();

      private:
        void RefreshFromScene();

        Ref<Scene> m_Context;
        NavMeshSettings m_Settings;
        bool m_ShowDebugDraw = false;
        f32 m_LastBakeTimeMs = 0.0f;
        i32 m_LastPolyCount = 0;
    };
} // namespace OloEngine
