#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Renderer/Material.h"

namespace OloEngine
{
    /**
     * @brief ECS-based animated mesh rendering system
     * 
     * This system iterates over entities with AnimatedMeshComponent and submits
     * skinned mesh draw commands to the Renderer3D. It integrates the animation
     * system with the command-based rendering architecture.
     */
    class AnimatedMeshRenderSystem
    {
    public:
        /**
         * @brief Render all animated meshes in the scene
         * 
         * Iterates over entities with AnimatedMeshComponent, AnimationStateComponent,
         * and SkeletonComponent, then submits skinned mesh draw commands.
         * 
         * @param scene The scene containing animated mesh entities
         * @param defaultMaterial Material to use for entities without a material component
         */
        static void RenderAnimatedMeshes(const Ref<Scene>& scene, const Material& defaultMaterial);

        /**
         * @brief Render a single animated mesh entity
         * 
         * @param entity Entity with animated mesh components
         * @param defaultMaterial Material to use if entity doesn't have a material component
         */
        static void RenderAnimatedMesh(Entity entity, const Material& defaultMaterial);

        /**
         * @brief Statistics for animated mesh rendering
         */
        struct Statistics
        {
            u32 TotalAnimatedMeshes = 0;
            u32 RenderedAnimatedMeshes = 0;
            u32 SkippedAnimatedMeshes = 0; // Due to culling, invalid data, etc.
            
            void Reset() 
            { 
                TotalAnimatedMeshes = 0; 
                RenderedAnimatedMeshes = 0; 
                SkippedAnimatedMeshes = 0; 
            }
        };

        /**
         * @brief Get rendering statistics
         */
        static const Statistics& GetStats() { return s_Stats; }

        /**
         * @brief Reset rendering statistics
         */
        static void ResetStats() { s_Stats.Reset(); }

    private:
        static Statistics s_Stats;
    };
}
