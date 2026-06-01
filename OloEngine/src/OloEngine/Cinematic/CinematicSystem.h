#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"

namespace OloEngine
{
    class Scene;
    class CinematicSequence;
    struct CinematicComponent;

    /**
     * @brief Drives every CinematicComponent in a Scene each runtime tick.
     *
     * Called once per frame from Scene::OnUpdateRuntime. For each playing
     * component it advances the playhead (CinematicPlayer), records fired
     * events, and samples the sequence's tracks onto the targeted entities'
     * TransformComponent / CameraComponent / ModelComponent.
     */
    class CinematicSystem
    {
      public:
        /// Advance and apply all playing CinematicComponents in `scene`.
        static void Update(Scene& scene, Timestep ts);

        /// Sample `sequence` at `time` and write the result onto the scene's
        /// entities. Exposed for editor scrubbing and tests (no playhead
        /// advance, no event firing).
        static void ApplyAtTime(Scene& scene, const CinematicSequence& sequence, f32 time);

        /// Advance one component's playhead by `dt` and apply the sampled pose
        /// to `scene` (the per-component step used by Update). Assumes the
        /// caller has already decided the component should play. Used by the
        /// runtime tick and by the editor inspector's edit-mode preview so both
        /// share identical timeline/event semantics.
        static void Advance(Scene& scene, CinematicComponent& component, CinematicSequence& sequence, f32 dt);

      private:
        /// Resolve a component's sequence: prefer the already-set RuntimeSequence,
        /// else lazily load it from the Sequence asset handle and cache it.
        static Ref<CinematicSequence> ResolveSequence(CinematicComponent& component);
    };
} // namespace OloEngine
