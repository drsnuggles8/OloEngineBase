#pragma once

namespace OloEngine
{
    class Scene;
} // namespace OloEngine

namespace OloEngine::Animation
{
    // Live-retargeting bake pass (issue #631 part 2): for every entity with an
    // enabled RetargetingComponent, lazily retargets the source clips onto the
    // entity's skeleton and splices the results into its
    // AnimationStateComponent::m_AvailableClips (see RetargetingComponent.h).
    // Runs as the "Retargeting" gameplay-scheduler node before Animation /
    // AnimationGraph, and at the top of the editor-preview animation loop so
    // live preview works in edit mode. Idempotent per settings: work happens
    // only when the authored settings differ from the last bake.
    class RetargetingSystem
    {
      public:
        static void OnUpdate(Scene* scene);
    };
} // namespace OloEngine::Animation
