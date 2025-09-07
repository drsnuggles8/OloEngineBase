#include "OloEnginePCH.h"

#include "AnimationAsset.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{

AnimationAsset::AnimationAsset(AssetHandle animationSource, AssetHandle mesh, const std::string& animationName, 
                              bool extractRootMotion, u32 rootBoneIndex,
                              const glm::vec3& rootTranslationMask,
                              const glm::vec3& rootRotationMask,
                              bool discardRootMotion)
    : m_AnimationSource(animationSource), m_Mesh(mesh), m_AnimationName(animationName), 
      m_IsExtractRootMotion(extractRootMotion), m_RootBoneIndex(rootBoneIndex), 
      m_RootTranslationMask(rootTranslationMask), m_RootRotationMask(rootRotationMask),
      m_IsDiscardRootMotion(discardRootMotion)
{
}

void AnimationAsset::OnDependencyUpdated(AssetHandle handle)
{
    // TODO: When a dependency is updated (like the source mesh or animation data),
    // we should notify the asset manager to reload this asset to pick up the changes
    // For now, this is a placeholder for future dependency management
    OLO_CORE_TRACE("AnimationAsset dependency updated: {}", (u64)handle);
}

} // namespace OloEngine
