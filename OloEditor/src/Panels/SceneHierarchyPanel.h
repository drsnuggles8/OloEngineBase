#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
    class SceneHierarchyPanel
    {
      public:
        SceneHierarchyPanel() = default;
        SceneHierarchyPanel(const Ref<Scene>& context);

        void SetContext(const Ref<Scene>& context);

        void OnImGuiRender();

        [[nodiscard("Store this!")]] Entity GetSelectedEntity() const
        {
            return m_SelectionContext;
        }
        void SetSelectedEntity(Entity entity);

      private:
        template<typename T>
        void DisplayAddComponentEntry(const std::string& entryName);

        void DrawEntityNode(Entity entity);
        void DrawComponents(Entity entity);

        Entity FindOrCreateCanvas();
        Entity CreateUIWidget(const std::string& name);

      private:
        Ref<Scene> m_Context;
        Entity m_SelectionContext;

        // GPU particle alive-count readback state (throttled to avoid per-frame GPU stalls)
        bool m_DebugGPUAliveReadback = false;
        float m_LastGPUAliveReadbackTime = 0.0f;
        u32 m_LastGPUAliveCount = 0;
    };
} // namespace OloEngine
