#pragma once

#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

#include <string>

namespace OloEngine
{
    class CommandHistory;

    class AnimationGraphEditorPanel
    {
      public:
        AnimationGraphEditorPanel() = default;
        explicit AnimationGraphEditorPanel(const Ref<Scene>& context);

        void SetContext(const Ref<Scene>& context);
        void SetSelectedEntity(Entity entity);
        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }

        void OnImGuiRender(bool* p_open = nullptr);

      private:
        void DrawParameterEditor();
        void DrawStateGraph();
        void DrawStateEditor();
        void DrawTransitionEditor();
        void DrawBlendTreeEditor();
        void DrawLayerManager();
        void DrawLivePreview();

        // Snapshot the current graph state for a discrete mutation. Returns a deep clone
        // suitable for passing to PushSnapshot() after the mutation completes. Returns
        // nullptr (and PushSnapshot will no-op) if there is no command history or no
        // active runtime graph.
        Ref<AnimationGraph> SnapshotGraph() const;

        // Push an undo command capturing oldSnap (taken before the mutation) and the
        // current post-mutation state. Safe to call with a nullptr oldSnap.
        void PushSnapshot(Ref<AnimationGraph> oldSnap, std::string description);

        // Session-style edit tracking for continuous-edit widgets (sliders, drag fields).
        // BeginEditSession captures a snapshot when no edit is currently in flight;
        // EndEditSession flushes the snapshot to the undo stack once ImGui reports that
        // no widget is active anymore. This collapses a multi-frame slider drag into a
        // single undo entry rather than 60-per-second.
        void BeginEditSession();
        void EndEditSession(const char* description);

        Ref<Scene> m_Context;
        Entity m_SelectedEntity;
        CommandHistory* m_CommandHistory = nullptr;

        // Edit-session state for continuous widgets.
        Ref<AnimationGraph> m_EditSessionSnapshot;
        bool m_EditSessionActive = false;

        // Editor state
        std::string m_SelectedStateName;
        i32 m_SelectedTransitionIndex = -1;
        i32 m_SelectedLayerIndex = 0;

        // New parameter dialog
        bool m_ShowNewParamDialog = false;
        char m_NewParamName[128] = {};
        i32 m_NewParamType = 0;
        f32 m_NewParamDefaultFloat = 0.0f;
        i32 m_NewParamDefaultInt = 0;
        bool m_NewParamDefaultBool = false;

        // New state dialog
        bool m_ShowNewStateDialog = false;
        char m_NewStateName[128] = {};
        bool m_NewStateIsBlendTree = false;

        // New transition dialog
        bool m_ShowNewTransitionDialog = false;
        char m_NewTransitionSource[128] = {};
        char m_NewTransitionDest[128] = {};
        f32 m_NewTransitionBlendDuration = 0.2f;
    };
} // namespace OloEngine
