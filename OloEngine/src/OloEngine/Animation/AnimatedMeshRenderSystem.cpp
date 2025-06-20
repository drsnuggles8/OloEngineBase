#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimatedMeshRenderSystem.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SkinnedMesh.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    AnimatedMeshRenderSystem::Statistics AnimatedMeshRenderSystem::s_Stats;

    void AnimatedMeshRenderSystem::RenderAnimatedMeshes(const Ref<Scene>& scene, const Material& defaultMaterial)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
        {
            OLO_CORE_WARN("AnimatedMeshRenderSystem::RenderAnimatedMeshes: Scene is null");
            return;
        }        // Get all entities with the required components for animated mesh rendering
        auto view = scene->GetAllEntitiesWith<AnimatedMeshComponent, SkeletonComponent, TransformComponent>();

        for (auto entityID : view)
        {
            Entity entity = { entityID, scene.get() };
            s_Stats.TotalAnimatedMeshes++;

            RenderAnimatedMesh(entity, defaultMaterial);
        }
    }

    void AnimatedMeshRenderSystem::RenderAnimatedMesh(Entity entity, const Material& defaultMaterial)
    {
        OLO_PROFILE_FUNCTION();        // Check required components
        if (!entity.HasComponent<AnimatedMeshComponent>() || 
            !entity.HasComponent<SkeletonComponent>() ||
            !entity.HasComponent<TransformComponent>())
        {
            s_Stats.SkippedAnimatedMeshes++;
            return;
        }

        auto& animatedMeshComp = entity.GetComponent<AnimatedMeshComponent>();
        auto& skeletonComp = entity.GetComponent<SkeletonComponent>();
        auto& transformComp = entity.GetComponent<TransformComponent>();

        // Validate mesh
        if (!animatedMeshComp.m_Mesh)
        {
            OLO_CORE_WARN("AnimatedMeshRenderSystem::RenderAnimatedMesh: Entity {} has invalid mesh", 
                         entity.GetComponent<TagComponent>().Tag);
            s_Stats.SkippedAnimatedMeshes++;
            return;
        }

        // Get the world transform matrix
        glm::mat4 worldTransform = transformComp.GetTransform();

        // Use default material (in a full implementation, entities could have MaterialComponent)
        Material material = defaultMaterial;

        // Get bone matrices for GPU skinning from SkeletonComponent
        const std::vector<glm::mat4>& boneMatrices = skeletonComp.m_FinalBoneMatrices;

        // Submit skinned mesh draw command
        auto* packet = Renderer3D::DrawSkinnedMesh(
            animatedMeshComp.m_Mesh,
            worldTransform,
            material,
            boneMatrices,
            false // Dynamic objects (animated meshes are not static)
        );

        if (packet)
        {
            Renderer3D::SubmitPacket(packet);
            s_Stats.RenderedAnimatedMeshes++;
        }
        else
        {
            OLO_CORE_WARN("AnimatedMeshRenderSystem::RenderAnimatedMesh: Failed to create draw packet for entity {}", 
                         entity.GetComponent<TagComponent>().Tag);
            s_Stats.SkippedAnimatedMeshes++;
        }
    }
}
