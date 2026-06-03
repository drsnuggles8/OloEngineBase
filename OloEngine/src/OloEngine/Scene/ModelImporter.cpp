#include "OloEnginePCH.h"

#include "OloEngine/Scene/ModelImporter.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"

namespace OloEngine
{
    ModelImportResult ModelImporter::PopulateAnimatedEntityFromParts(
        Entity entity,
        const Ref<MeshSource>& meshSource,
        const Ref<Skeleton>& skeleton,
        const std::vector<Ref<AnimationClip>>& clips,
        const Material* material,
        const std::string& sourcePath,
        bool resetPlaybackState)
    {
        OLO_PROFILE_FUNCTION();

        ModelImportResult result;
        result.IsAnimated = (skeleton != nullptr) || !clips.empty();

        // MeshComponent — the skinned (or static) geometry.
        if (meshSource)
        {
            if (!entity.HasComponent<MeshComponent>())
            {
                entity.AddComponent<MeshComponent>();
                result.AddedMeshComponent = true;
            }
            entity.GetComponent<MeshComponent>().m_MeshSource = meshSource;
        }

        // SkeletonComponent — shared skeleton reference for bone-driven deformation.
        if (skeleton)
        {
            if (!entity.HasComponent<SkeletonComponent>())
            {
                entity.AddComponent<SkeletonComponent>();
                result.AddedSkeletonComponent = true;
            }
            // SetSkeleton invalidates the tag->entity cache (important on re-import).
            entity.GetComponent<SkeletonComponent>().SetSkeleton(skeleton);
        }

        // AnimationStateComponent — playback state + the available clip set.
        if (!clips.empty())
        {
            if (!entity.HasComponent<AnimationStateComponent>())
            {
                entity.AddComponent<AnimationStateComponent>();
                result.AddedAnimationStateComponent = true;
            }
            auto& anim = entity.GetComponent<AnimationStateComponent>();
            anim.m_AvailableClips = clips;

            if (resetPlaybackState)
            {
                anim.m_State = AnimationStateComponent::State::Idle;
                anim.m_CurrentClipIndex = 0;
                anim.m_CurrentTime = 0.0f;
                anim.m_IsPlaying = false;
                anim.m_Blending = false;
                anim.m_NextClip = nullptr;
            }

            // Clamp the (possibly deserialized) current-clip index into range and
            // resolve the matching clip. Mirrors the deserializer's fallback to clip 0.
            if (anim.m_CurrentClipIndex < 0 || anim.m_CurrentClipIndex >= static_cast<int>(clips.size()))
            {
                anim.m_CurrentClipIndex = 0;
            }
            anim.m_CurrentClip = clips[static_cast<sizet>(anim.m_CurrentClipIndex)];

            anim.m_SourceFilePath = sourcePath;
        }

        // MaterialComponent — never clobber an explicitly assigned shader graph.
        if (material)
        {
            if (!entity.HasComponent<MaterialComponent>())
            {
                entity.AddComponent<MaterialComponent>();
                result.AddedMaterialComponent = true;
            }
            auto& materialComp = entity.GetComponent<MaterialComponent>();
            if (materialComp.m_ShaderGraphHandle == 0)
            {
                materialComp.m_Material = *material;
            }
        }

        return result;
    }

    ModelImportResult ModelImporter::PopulateAnimatedEntity(Entity entity, const Ref<AnimatedModel>& model,
                                                            const std::string& sourcePath, bool resetPlaybackState)
    {
        if (!model)
        {
            return {};
        }

        const Ref<MeshSource> meshSource = model->GetMeshes().empty() ? nullptr : model->GetMeshes().front();
        const Ref<Skeleton> skeleton = model->HasSkeleton() ? model->GetSkeleton() : nullptr;
        const Material* material = model->GetMaterials().empty() ? nullptr : &model->GetMaterials().front();

        return PopulateAnimatedEntityFromParts(entity, meshSource, skeleton, model->GetAnimations(),
                                               material, sourcePath, resetPlaybackState);
    }

    bool ModelImporter::PopulateStaticEntity(Entity entity, const Ref<Model>& model)
    {
        OLO_PROFILE_FUNCTION();

        if (!model || model->GetMeshCount() == 0)
        {
            return false;
        }

        // Combine all submeshes into a single MeshSource so the entity renders the whole model.
        auto combinedMeshSource = model->CreateCombinedMeshSource();
        if (!combinedMeshSource)
        {
            return false;
        }

        if (!entity.HasComponent<MeshComponent>())
        {
            entity.AddComponent<MeshComponent>();
        }
        entity.GetComponent<MeshComponent>().m_MeshSource = combinedMeshSource;
        return true;
    }
} // namespace OloEngine
