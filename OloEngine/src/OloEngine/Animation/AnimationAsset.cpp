#include "OloEnginePCH.h"

#include "AnimationAsset.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Application.h"

namespace OloEngine
{

AnimationAsset::AnimationAsset(AssetHandle animationSource, AssetHandle mesh, std::string animationName, 
                              bool extractRootMotion, u32 rootBoneIndex,
                              const glm::vec3& rootTranslationMask,
                              const glm::vec3& rootRotationMask,
                              bool discardRootMotion)
    : m_AnimationSource(animationSource), m_Mesh(mesh), m_AnimationName(std::move(animationName)), 
      m_IsExtractRootMotion(extractRootMotion), m_RootBoneIndex(rootBoneIndex), 
      m_RootTranslationMask(rootTranslationMask), m_RootRotationMask(rootRotationMask),
      m_IsDiscardRootMotion(discardRootMotion)
{
}

void AnimationAsset::OnDependencyUpdated(AssetHandle handle)
{
    // Thread-safe dependency update handling
    try
    {
        OLO_CORE_TRACE("AnimationAsset dependency updated: {}", (u64)handle);
        
        // Ensure we're working with valid handles
        if (handle == 0 || GetHandle() == 0)
        {
            OLO_CORE_WARN("AnimationAsset::OnDependencyUpdated - Invalid handle(s): dependency={}, self={}", 
                          (u64)handle, (u64)GetHandle());
            return;
        }

        // Check if the updated dependency is one we actually depend on
        bool isRelevantDependency = (handle == m_AnimationSource || handle == m_Mesh);
        if (!isRelevantDependency)
        {
            OLO_CORE_TRACE("AnimationAsset dependency {} not relevant to animation asset {}", 
                          (u64)handle, (u64)GetHandle());
            return;
        }

        // Capture the necessary data for the async operation
        AssetHandle selfHandle = GetHandle();
        AssetHandle animationSource = m_AnimationSource;
        AssetHandle mesh = m_Mesh;

        // Dispatch async reload to the main thread to avoid blocking
        // This ensures thread-safe reload and proper synchronization
        Application::Get().SubmitToMainThread([selfHandle, dependencyHandle = handle, animationSource, mesh]() mutable {
            try
            {
                // Deregister existing dependencies before reload
                AssetManager::DeregisterDependencies(selfHandle);
                
                // Trigger synchronous reload of this animation asset
                // Note: ReloadDataAsync is not available in the OloEngine AssetManager static interface
                // So we use the synchronous version which is thread-safe when called from main thread
                bool reloadSuccess = AssetManager::ReloadData(selfHandle);
                
                if (reloadSuccess)
                {
                    // Re-register dependencies after successful reload
                    if (animationSource != 0)
                    {
                        AssetManager::RegisterDependency(selfHandle, animationSource);
                    }
                    if (mesh != 0)
                    {
                        AssetManager::RegisterDependency(selfHandle, mesh);
                    }
                    
                    OLO_CORE_INFO("AnimationAsset {} reload successful due to dependency {} update", 
                                 (u64)selfHandle, (u64)dependencyHandle);
                }
                else
                {
                    OLO_CORE_ERROR("AnimationAsset {} reload failed due to dependency {} update", 
                                  (u64)selfHandle, (u64)dependencyHandle);
                }
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("AnimationAsset::OnDependencyUpdated failed during reload: {}", e.what());
                // Leave asset in safe state - don't throw, just log the error
            }
            catch (...)
            {
                OLO_CORE_ERROR("AnimationAsset::OnDependencyUpdated failed during reload: unknown exception");
                // Leave asset in safe state - don't throw, just log the error
            }
        });
    }
    catch (const std::exception& e)
    {
        OLO_CORE_ERROR("AnimationAsset::OnDependencyUpdated failed: {}", e.what());
        // Leave asset in safe state - don't throw from this callback
    }
    catch (...)
    {
        OLO_CORE_ERROR("AnimationAsset::OnDependencyUpdated failed: unknown exception");
        // Leave asset in safe state - don't throw from this callback
    }
}

} // namespace OloEngine
