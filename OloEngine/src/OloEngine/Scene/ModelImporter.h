#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/MeshOptimization.h"

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
        bool AddedLODGroupComponent = false; ///< An automatic LOD chain was generated (issue #711).
        bool IsAnimated = false;             ///< The source had a skeleton and/or animation clips.

        [[nodiscard]] bool AddedAnyComponent() const noexcept
        {
            return AddedMeshComponent || AddedSkeletonComponent ||
                   AddedAnimationStateComponent || AddedMaterialComponent ||
                   AddedLODGroupComponent;
        }
    };

    /**
     * @brief Whether and how an import generates a mesh LOD chain (issue #711).
     *
     * An imported mesh with no authored LOD group gets one generated automatically,
     * so a scene does not depend on somebody remembering to press "Generate LODs".
     * An entity that ALREADY has a @c LODGroupComponent is never touched — an
     * authored chain always wins.
     */
    struct AutoLODImportConfig
    {
        bool Enabled = true;
        /**
         * @brief Skip generation when the process has no graphics device.
         *
         * The generated levels are @c Mesh assets whose GPU buffers a headless
         * process (OloServer, most of the test suite) can never create, so
         * generating them there is pure cook cost for something nothing can draw.
         * Tests that want the chain without a device clear this.
         */
        bool RequireGraphicsDevice = true;
        MeshOptimization::AutoLODSettings Settings;
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
         *                            only clamps the current-clip index into range. It also gates
         *                            automatic LOD generation (@ref EnsureAutoLODGroup): a deserialize
         *                            must not invent a component the scene file never had.
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

        /// Process-wide automatic-LOD policy applied by every import path.
        [[nodiscard]] static AutoLODImportConfig& GetAutoLODConfig();

        /**
         * @brief Generate and attach an automatic LOD chain for @p entity's mesh.
         *
         * No-op when auto-LOD is disabled, when the entity already has a
         * @c LODGroupComponent (an authored chain wins), when the entity has no
         * simplifiable @c MeshComponent, or when the generated chain would be LOD 0
         * alone (a mesh the simplifier cannot reduce gains nothing from a group).
         *
         * @return @c true if a @c LODGroupComponent was added.
         */
        static bool EnsureAutoLODGroup(Entity entity);

        /**
         * @brief Release the memory-only LOD meshes a generated group owns.
         *
         * Bounds the lifetime of assets nothing else frees: a generated chain is
         * rebuilt on every scene load, so without this each reopen would strand a
         * whole set of CPU + GPU buffers in the process-global @c AssetManager.
         * Only handles still registered as memory-only are removed.
         */
        static void ReleaseGeneratedLODAssets(struct LODGroupComponent& lodComp);
    };
} // namespace OloEngine
