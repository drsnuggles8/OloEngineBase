#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Animation/Retargeting/HumanoidBoneMap.h"

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

        // Inspector field for an entity reference (e.g. an IK target). Renders a
        // button showing the referenced entity's name; clicking opens a filterable
        // picker popup. Still accepts an ENTITY_REPARENT drag payload as a fallback.
        // idSuffix must be unique within the component to avoid ImGui ID clashes.
        void DrawEntityReferenceField(const char* label, const char* idSuffix, UUID& targetEntity);

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

        // Entity currently being renamed (inline text-edit state). Panel-local
        // rather than a TagComponent flag (issue #444 hot/cold split) — only one
        // entity can be mid-rename at a time, so this doesn't need to live on
        // the runtime component.
        UUID m_RenamingEntityUUID{ 0 };

        // Entity search filter
        char m_FilterText[256] = {};

        // Cached HumanoidBoneMap::AutoDetect for the retargeting inspector's role
        // tree — recomputing it every frame while the node is expanded is wasteful.
        // Keyed on the skeleton pointer; refreshed when it changes.
        const void* m_CachedRoleSkeleton = nullptr;
        Animation::HumanoidBoneMap m_CachedRoleMap;
    };
} // namespace OloEngine
