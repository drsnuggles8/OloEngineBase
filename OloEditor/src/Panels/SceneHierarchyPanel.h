#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
    class CommandHistory;

    class SceneHierarchyPanel
    {
      public:
        SceneHierarchyPanel() = default;
        SceneHierarchyPanel(const Ref<Scene>& context);

        void SetContext(const Ref<Scene>& context);
        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }

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
        CommandHistory* m_CommandHistory = nullptr;

        // Rename tracking for undo
        std::string m_RenameOldName;
    };
} // namespace OloEngine
