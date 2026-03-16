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

        // Get skeleton visualization settings (for use by Scene rendering)
        [[nodiscard]] bool IsSkeletonVisualizationEnabled() const
        {
            return m_ShowSkeleton;
        }
        [[nodiscard]] bool ShowSkeletonBones() const
        {
            return m_ShowBones;
        }
        [[nodiscard]] bool ShowSkeletonJoints() const
        {
            return m_ShowJoints;
        }
        [[nodiscard]] f32 GetJointSize() const
        {
            return m_JointSize;
        }
        [[nodiscard]] f32 GetBoneThickness() const
        {
            return m_BoneThickness;
        }

      private:
        void DrawAnimationControls(Entity entity);
        void DrawAnimationTimeline(Entity entity);
        void DrawBoneHierarchy(Entity entity);
        void DrawSkeletonVisualization(Entity entity);

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

        // Skeleton visualization state
        bool m_ShowSkeleton = false;
        bool m_ShowBones = true;
        bool m_ShowJoints = true;
        f32 m_JointSize = 0.02f;
        f32 m_BoneThickness = 2.0f;
    };
} // namespace OloEngine
