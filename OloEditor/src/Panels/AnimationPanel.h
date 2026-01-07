#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
    class AnimationPanel
    {
      public:
        AnimationPanel() = default;
        explicit AnimationPanel(const Ref<Scene>& context);

        void SetContext(const Ref<Scene>& context);
        void SetSelectedEntity(Entity entity);

        void OnImGuiRender();

      private:
        void DrawAnimationControls(Entity entity);
        void DrawAnimationTimeline(Entity entity);
        void DrawBoneHierarchy(Entity entity);

      private:
        Ref<Scene> m_Context;
        Entity m_SelectedEntity;
        
        // Playback state
        bool m_IsPlaying = false;
        f32 m_PlaybackSpeed = 1.0f;
        bool m_LoopPlayback = true;
        
        // Timeline state
        f32 m_TimelineZoom = 1.0f;
        f32 m_TimelineOffset = 0.0f;
    };
} // namespace OloEngine
