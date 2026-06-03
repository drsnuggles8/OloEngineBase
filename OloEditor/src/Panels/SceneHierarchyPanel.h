#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

#include <functional>
#include <vector>

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

        // Set by EditorLayer: open a CinematicSequence asset in the timeline panel
        // (the "Edit in Timeline" button on the CinematicComponent inspector).
        void SetOpenCinematicTimelineCallback(std::function<void(AssetHandle)> cb)
        {
            m_OpenCinematicTimeline = std::move(cb);
        }

        void OnImGuiRender();

        // Single selection (primary — used for Properties panel and gizmos)
        [[nodiscard("Store this!")]] Entity GetSelectedEntity() const
        {
            return m_SelectionContext;
        }
        void SetSelectedEntity(Entity entity);

        // Multi-selection
        [[nodiscard("Store this!")]] const std::vector<Entity>& GetSelectedEntities() const
        {
            return m_SelectedEntities;
        }
        void ClearSelection();
        void ToggleEntitySelection(Entity entity);
        void DeleteSelectedEntities();

      private:
        template<typename T>
        void DisplayAddComponentEntry(const std::string& entryName);

        void DrawEntityNode(Entity entity);
        void DrawComponents(Entity entity);
        void CollectVisualOrder(Entity entity, std::vector<Entity>& out) const;

        Entity FindOrCreateCanvas();
        Entity CreateUIWidget(const std::string& name);

        bool IsEntitySelected(Entity entity) const;

      private:
        Ref<Scene> m_Context;
        Entity m_SelectionContext;
        std::vector<Entity> m_SelectedEntities;
        CommandHistory* m_CommandHistory = nullptr;
        std::function<void(AssetHandle)> m_OpenCinematicTimeline;

        // Rename tracking for undo
        std::string m_RenameOldName;

        // Entity search filter
        char m_FilterText[256] = {};
    };
} // namespace OloEngine
