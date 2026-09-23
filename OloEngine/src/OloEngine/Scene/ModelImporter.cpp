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
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"

namespace OloEngine
{
    ModelImportResult ModelImporter::PopulateAnimatedEntityFromParts(
        Entity entity,
        const Ref<MeshSource>& meshSource,
        const Ref<Skeleton>& skeleton,
        std::span<const Ref<AnimationClip>> clips,
        const Material* material,
        const std::string& sourcePath,
        bool resetPlaybackState)
    {
        OLO_PROFILE_FUNCTION();

        ModelImportResult result;
        // Morph targets make a source animated too (issue #1439): a blend-shape face
        // with no bones and no clips is still deformed every frame, and the automatic
        // LOD chain below would drop its morph deltas exactly as it drops bone weights.
        const bool hasMorphTargets = meshSource && meshSource->HasMorphTargets();
        result.IsAnimated = (skeleton != nullptr) || !clips.empty() || hasMorphTargets;

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

        // MorphTargetComponent — the weights that drive the mesh's morph targets
        // (issue #1439). Added, never replaced: a scene reload re-wires the entity
        // and then reads the saved weights into this same component.
        if (hasMorphTargets && !entity.HasComponent<MorphTargetComponent>())
        {
            entity.AddComponent<MorphTargetComponent>();
            result.AddedMorphTargetComponent = true;
        }

        // AnimationStateComponent — playback state + the available clip set. Also
        // for a clip-less MORPH source (issue #1439), because this component is what
        // records the source file: without it a saved scene has no path to re-import
        // the mesh from, and a morph-only face would reload with no mesh at all.
        if (!clips.empty() || hasMorphTargets)
        {
            if (!entity.HasComponent<AnimationStateComponent>())
            {
                entity.AddComponent<AnimationStateComponent>();
                result.AddedAnimationStateComponent = true;
            }
            auto& anim = entity.GetComponent<AnimationStateComponent>();
            anim.m_AvailableClips = std::vector<Ref<AnimationClip>>(clips.begin(), clips.end());

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
            // No clips at all (a clip-less morph source) leaves no current clip, which
            // the morph-only animation update already skips.
            if (anim.m_CurrentClipIndex < 0 || anim.m_CurrentClipIndex >= static_cast<int>(clips.size()))
            {
                anim.m_CurrentClipIndex = 0;
            }
            anim.m_CurrentClip = clips.empty() ? nullptr : clips[static_cast<sizet>(anim.m_CurrentClipIndex)];

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

        // Automatic LOD chain (issue #711).
        //
        // `resetPlaybackState` separates a FRESH IMPORT from a deserialize
        // (SceneSerializer passes false when it re-wires an entity from a saved
        // scene). Generating there would add a component the scene file never had,
        // cook every mesh on every scene load, and then persist the invented group.
        if (resetPlaybackState)
        {
            // DISCARD before the animated gate, not after. A fresh import replaces
            // m_MeshSource on the existing entity, so a chain generated for the
            // PREVIOUS mesh is stale either way — and re-importing a static model as
            // an animated one takes the skip path below, which would otherwise leave
            // that stale chain attached to a skinned mesh forever.
            DiscardGeneratedLODGroup(entity);

            // Animated sources get no chain: the simplifier drops bone weights and
            // morph deltas, so a skinned LOD renders in bind pose.
            if (!result.IsAnimated)
            {
                result.AddedLODGroupComponent = EnsureAutoLODGroup(entity);
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

        // Automatic LOD chain (issue #711) — see EnsureAutoLODGroup. The discard is
        // redundant here (EnsureAutoLODGroup replaces a generated group itself) but
        // keeps the two import entry points reading the same way.
        DiscardGeneratedLODGroup(entity);
        EnsureAutoLODGroup(entity);
        return true;
    }

    AutoLODImportConfig& ModelImporter::GetAutoLODConfig()
    {
        static AutoLODImportConfig s_Config;
        return s_Config;
    }

    void ModelImporter::ReleaseGeneratedLODAssets(LODGroupComponent& lodComp)
    {
        // Only the handles this component generated and owns, and only if they are
        // still memory-only — a level the user re-pointed at a real asset is not ours
        // to remove.
        if (!Project::HasAssetManager())
        {
            lodComp.m_GeneratedLODHandles.Reset();
            return;
        }
        for (const AssetHandle handle : lodComp.m_GeneratedLODHandles)
        {
            if (handle != 0 && AssetManager::IsMemoryAsset(handle))
            {
                AssetManager::RemoveAsset(handle);
            }
        }
        lodComp.m_GeneratedLODHandles.Reset();
    }

    void ModelImporter::DiscardGeneratedLODGroup(Entity entity)
    {
        // Only a GENERATED group. An authored chain is the user's data and survives a
        // re-import; a generated one describes geometry that is being replaced, so
        // keeping it would draw the PREVIOUS model past LOD 0.
        if (!entity || !entity.HasComponent<LODGroupComponent>())
        {
            return;
        }
        auto& lodComp = entity.GetComponent<LODGroupComponent>();
        if (!lodComp.m_AutoGenerated)
        {
            return;
        }
        ReleaseGeneratedLODAssets(lodComp);
        entity.RemoveComponent<LODGroupComponent>();
    }

    bool ModelImporter::EnsureAutoLODGroup(Entity entity)
    {
        OLO_PROFILE_FUNCTION();

        const AutoLODImportConfig& config = GetAutoLODConfig();
        if (!config.Enabled)
        {
            return false;
        }
        if (config.RequireGraphicsDevice && !RenderCommand::IsDeviceAvailable())
        {
            return false;
        }
        if (!Project::HasAssetManager())
        {
            // The generated levels are registered as memory-only assets, so without
            // an asset manager there is nowhere to put them and no handle to name
            // them by. AssetManager::AddMemoryOnlyAsset would assert.
            return false;
        }
        if (!entity || !entity.HasComponent<MeshComponent>())
        {
            return false;
        }

        if (entity.HasComponent<LODGroupComponent>())
        {
            // An AUTHORED group always wins — never overwrite one. A previously
            // GENERATED one is replaced; see DiscardGeneratedLODGroup.
            if (!entity.GetComponent<LODGroupComponent>().m_AutoGenerated)
            {
                return false;
            }
            DiscardGeneratedLODGroup(entity);
        }

        // By VALUE, not by reference into the component: AddComponent below is a
        // structural registry mutation that can move the MeshComponent pool.
        Ref<MeshSource> const meshSource = entity.GetComponent<MeshComponent>().m_MeshSource;
        // Skinned and morphing sources are ALLOWED here since #1227: the generators
        // carry the bone influences and the morph deltas through the same remap the
        // positions take (MeshOptimization::CopyDeformationStreams), and Scene's
        // frame boundary treats a level switch on an animated entity as a
        // deformation-history discontinuity. Before that they were refused, which
        // is why every imported character drew at LOD 0 however far away it was.
        // Multi-submesh sources are still refused: the generators collapse every
        // material into index 0.
        if (!meshSource || meshSource->GetSubmeshes().Num() > 1)
        {
            return false;
        }

        // LOD 0 needs a handle of its own so the group can name it like any other
        // level; the source MeshSource is not itself a Mesh asset.
        auto baseMesh = Ref<Mesh>::Create(meshSource, 0);
        AssetHandle const baseHandle = AssetManager::AddMemoryOnlyAsset(baseMesh);

        LODGroup group = MeshOptimization::GenerateAutoLODGroup(*meshSource, baseHandle, config.Settings);
        if (group.Levels.Num() < 2)
        {
            // Nothing simplified — a group holding only LOD 0 would add a per-draw
            // selection cost and a stray memory-only asset for no benefit.
            if (AssetManager::IsMemoryAsset(baseHandle))
            {
                AssetManager::RemoveAsset(baseHandle);
            }
            return false;
        }

        auto& lodComp = entity.AddComponent<LODGroupComponent>();
        lodComp.m_LODGroup = MoveTemp(group);
        // Marks the chain as derived data so the scene serializer regenerates it on
        // load instead of persisting handles to memory-only assets.
        lodComp.m_AutoGenerated = true;
        lodComp.m_GeneratedLODHandles.Reserve(lodComp.m_LODGroup.Levels.Num());
        for (const auto& level : lodComp.m_LODGroup.Levels)
        {
            lodComp.m_GeneratedLODHandles.Add(level.MeshHandle);
        }

        OLO_CORE_TRACE("ModelImporter::EnsureAutoLODGroup: generated {} LOD levels ({} -> {} triangles)",
                       lodComp.m_LODGroup.Levels.Num(),
                       lodComp.m_LODGroup.Levels[0].TriangleCount,
                       lodComp.m_LODGroup.Levels.Last().TriangleCount);
        return true;
    }
} // namespace OloEngine
