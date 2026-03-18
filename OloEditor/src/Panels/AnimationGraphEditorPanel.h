#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

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

        Ref<Scene> m_Context;
        Entity m_SelectedEntity;
        // TODO: integrate m_CommandHistory->Push() at mutation sites (parameter/state/transition/layer CRUD)
        CommandHistory* m_CommandHistory = nullptr;

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
