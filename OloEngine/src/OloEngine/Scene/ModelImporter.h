#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <string>
#include <vector>

namespace OloEngine
{
    class Entity;
    class Model;
    class AnimatedModel;
    class MeshSource;
    class Skeleton;
    class AnimationClip;
    class Material;

    /**
     * @brief Records which components a model import added or updated on an entity.
     *
     * Returned by the @ref ModelImporter helpers so callers (e.g. the editor) can
     * build an undo step from exactly the components that were newly added.
     */
    struct ModelImportResult
    {
        bool AddedMeshComponent = false;
        bool AddedSkeletonComponent = false;
        bool AddedAnimationStateComponent = false;
        bool AddedMaterialComponent = false;
        bool IsAnimated = false; ///< The source had a skeleton and/or animation clips.

        [[nodiscard]] bool AddedAnyComponent() const noexcept
        {
            return AddedMeshComponent || AddedSkeletonComponent ||
                   AddedAnimationStateComponent || AddedMaterialComponent;
        }
    };

    /**
     * @brief Single source of truth for wiring a loaded model onto a scene entity.
     *
     * Importing an animated model means assigning a coherent set of components —
     * @c MeshComponent, @c SkeletonComponent, @c AnimationStateComponent and
     * @c MaterialComponent — rather than a single mesh. That wiring used to be
     * duplicated across the editor's "Import Animated Model" button and the scene
     * deserializer; this helper centralises it so every import path (the editor
     * buttons, viewport drag-drop, and @c SceneSerializer reload) stays in lockstep.
     *
     * @note The core does pure ECS assignment — no GL calls — so the
     *       parts-based overload is unit-testable without a render context.
     */
    class ModelImporter
    {
    public:
        /**
         * @brief Wire an entity from a loaded @ref AnimatedModel.
         *
         * Adds or updates @c MeshComponent (mesh 0), @c SkeletonComponent and
         * @c AnimationStateComponent when the model carries skeletal / animation
         * data, plus @c MaterialComponent (material 0) when no shader graph is
         * already assigned. Components that already exist are reused, not replaced.
         *
         * @param entity              Target entity.
         * @param model               Loaded animated model (may be a non-animated model too).
         * @param sourcePath          File path stored on @c AnimationStateComponent for reload/serialization.
         * @param resetPlaybackState  @c true (fresh import) resets playback to clip 0 / Idle / stopped;
         *                            @c false (deserialize) preserves the existing playback scalars and
         *                            only clamps the current-clip index into range.
         */
        static ModelImportResult PopulateAnimatedEntity(Entity entity, const Ref<AnimatedModel>& model,
                                                        const std::string& sourcePath, bool resetPlaybackState = true);

        /**
         * @brief Pure-ECS core operating on already-extracted parts (no asset loading, no GL).
         *
         * @param material  Optional material to copy into a @c MaterialComponent (nullptr to skip).
         * @see PopulateAnimatedEntity
         */
        static ModelImportResult PopulateAnimatedEntityFromParts(
            Entity entity,
            const Ref<MeshSource>& meshSource,
            const Ref<Skeleton>& skeleton,
            const std::vector<Ref<AnimationClip>>& clips,
            const Material* material,
            const std::string& sourcePath,
            bool resetPlaybackState = true);

        /**
         * @brief Wire an entity from a static @ref Model by combining its meshes into one MeshSource.
         * @return @c true if a mesh was assigned, @c false if the model was empty.
         */
        static bool PopulateStaticEntity(Entity entity, const Ref<Model>& model);
    };
} // namespace OloEngine
