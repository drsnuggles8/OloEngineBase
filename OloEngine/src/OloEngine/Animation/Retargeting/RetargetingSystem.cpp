#include "OloEnginePCH.h"
#include "OloEngine/Animation/Retargeting/RetargetingSystem.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Retargeting/AnimationRetargeter.h"
#include "OloEngine/Animation/Retargeting/HumanoidBoneMap.h"
#include "OloEngine/Animation/Retargeting/RetargetingComponent.h"
#include "OloEngine/Animation/Retargeting/SkeletonRetargetMap.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace OloEngine::Animation
{
    namespace
    {
        // Apply the component's hand corrections on top of an AutoDetect result:
        // bone name (exact match against the skeleton) → role; negative role
        // clears the bone's detected assignment.
        void ApplyRoleOverrides(HumanoidBoneMap& roles,
                                const std::unordered_map<std::string, i32>& overrides,
                                const SkeletonData& skeleton)
        {
            for (const auto& [boneName, role] : overrides)
            {
                const auto it = std::ranges::find(skeleton.m_BoneNames, boneName);
                if (it == skeleton.m_BoneNames.end())
                {
                    continue; // stale override for a bone the rig no longer has
                }
                const int boneIndex = static_cast<int>(std::distance(skeleton.m_BoneNames.begin(), it));
                if (role < 0)
                {
                    // Clear whatever role the detector assigned this bone.
                    if (const HumanoidBone detected = roles.GetRole(boneIndex); detected != HumanoidBone::None)
                    {
                        roles.SetBone(detected, -1);
                    }
                }
                else if (role < static_cast<i32>(HumanoidBoneCount))
                {
                    roles.SetBone(static_cast<HumanoidBone>(role), boneIndex);
                }
            }
        }

        [[nodiscard("built map must be used")]] SkeletonRetargetMap BuildMap(
            const RetargetingComponent& settings,
            const SkeletonData& source,
            const SkeletonData& target)
        {
            if (!settings.UseHumanoidRoles)
            {
                return SkeletonRetargetMap::BuildByName(source, target);
            }
            HumanoidBoneMap sourceRoles = HumanoidBoneMap::AutoDetect(source);
            HumanoidBoneMap targetRoles = HumanoidBoneMap::AutoDetect(target);
            ApplyRoleOverrides(sourceRoles, settings.m_SourceRoleOverrides, source);
            ApplyRoleOverrides(targetRoles, settings.m_TargetRoleOverrides, target);
            SkeletonRetargetMap map = SkeletonRetargetMap::BuildByHumanoidRole(source, target, sourceRoles, targetRoles);
            map.FillUnmappedFrom(SkeletonRetargetMap::BuildByName(source, target));
            return map;
        }

        // Splice a baked clip into the entity's available-clips list, replacing
        // an earlier clip of the same name (e.g. a previous bake) or appending.
        void SpliceClip(std::vector<Ref<AnimationClip>>& availableClips, const Ref<AnimationClip>& baked)
        {
            for (auto& clip : availableClips)
            {
                if (clip && clip->Name == baked->Name)
                {
                    clip = baked;
                    return;
                }
            }
            availableClips.push_back(baked);
        }
    } // namespace

    void RetargetingSystem::OnUpdate(Scene* scene)
    {
        OLO_PROFILE_FUNCTION();

        auto view = scene->GetAllEntitiesWith<RetargetingComponent, AnimationStateComponent, SkeletonComponent>();
        for (auto e : view)
        {
            const auto& settings = view.template get<RetargetingComponent>(e);
            if (!settings.Enabled || (settings.m_SourcePath.empty() && settings.m_SourceEntity == 0))
            {
                continue;
            }

            auto& skelComp = view.template get<SkeletonComponent>(e);
            if (!skelComp.m_Skeleton || skelComp.m_Skeleton->m_BoneNames.empty())
            {
                continue;
            }

            Entity entity{ e, scene };
            if (!entity.HasComponent<RetargetingStateComponent>())
            {
                entity.AddComponent<RetargetingStateComponent>();
            }
            auto& state = entity.GetComponent<RetargetingStateComponent>();
            if (state.Attempted && state.BakedFromSettings == settings)
            {
                continue; // settings unchanged since the last bake (or failed attempt)
            }
            state.Attempted = true;
            state.BakedFromSettings = settings;
            state.BakedClips.clear();

            // Resolve the SOURCE skeleton + clips: scene entity first, file second.
            Ref<Skeleton> sourceSkeleton;
            const std::vector<Ref<AnimationClip>>* sourceClips = nullptr;
            Ref<AnimatedModel> sourceModel; // keeps file-route data alive through the bake
            if (settings.m_SourceEntity != 0)
            {
                if (auto source = scene->TryGetEntityWithUUID(settings.m_SourceEntity); source.has_value() &&
                                                                                        source->HasComponent<SkeletonComponent>() &&
                                                                                        source->HasComponent<AnimationStateComponent>())
                {
                    sourceSkeleton = source->GetComponent<SkeletonComponent>().m_Skeleton;
                    sourceClips = &source->GetComponent<AnimationStateComponent>().m_AvailableClips;
                }
            }
            else
            {
                sourceModel = Ref<AnimatedModel>::Create(settings.m_SourcePath);
                if (sourceModel && sourceModel->GetSkeleton() && sourceModel->HasAnimations())
                {
                    sourceSkeleton = sourceModel->GetSkeleton();
                    sourceClips = &sourceModel->GetAnimations();
                }
            }
            if (!sourceSkeleton || sourceSkeleton->m_BoneNames.empty() || !sourceClips || sourceClips->empty())
            {
                OLO_CORE_WARN("RetargetingSystem: no source skeleton/clips for entity '{}' (path '{}', source entity {})",
                              entity.GetName(), settings.m_SourcePath, static_cast<u64>(settings.m_SourceEntity));
                continue; // Attempted stays set — no per-tick retry until settings change
            }

            const SkeletonRetargetMap map = BuildMap(settings, *sourceSkeleton, *skelComp.m_Skeleton);

            RetargetOptions options;
            options.RetargetRootTranslation = settings.TransferRootTranslation;
            options.Translation = settings.PerBoneTranslation
                                      ? RetargetOptions::TranslationMode::PerBoneRatio
                                      : RetargetOptions::TranslationMode::RootOnly;
            options.RootTranslationScale = (settings.RootTranslationScale > 0.0f && std::isfinite(settings.RootTranslationScale))
                                               ? settings.RootTranslationScale
                                               : AnimationRetargeter::ComputeRootTranslationScale(*sourceSkeleton, *skelComp.m_Skeleton);

            auto& animState = view.template get<AnimationStateComponent>(e);
            for (const auto& sourceClip : *sourceClips)
            {
                if (!sourceClip)
                {
                    continue;
                }
                Ref<AnimationClip> baked = AnimationRetargeter::RetargetClip(sourceClip, *sourceSkeleton, *skelComp.m_Skeleton, map, options);
                if (!baked)
                {
                    continue;
                }
                // Keep the SOURCE name: graphs resolve clips by name and users
                // address "Walk", not "Walk_Retargeted".
                baked->Name = sourceClip->Name;
                state.BakedClips.push_back(baked);
                SpliceClip(animState.m_AvailableClips, baked);
            }

            OLO_CORE_INFO("RetargetingSystem: baked {} clip(s) onto entity '{}'",
                          state.BakedClips.size(), entity.GetName());
        }
    }
} // namespace OloEngine::Animation
