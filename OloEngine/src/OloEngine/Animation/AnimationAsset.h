#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Core/Base.h"
#include <string>
#include <glm/glm.hpp>

namespace OloEngine
{
    /**
     * @brief Animation asset that wraps AnimationClip with additional metadata
     * 
     * AnimationAsset represents a single animation from a source file (like FBX),
     * with additional parameters for root motion extraction and mesh association.
     * It follows the Hazel pattern for compatibility while adapting to OloEngine's 
     * AnimationClip-based system.
     */
    class AnimationAsset : public Asset
    {
    public:
        AnimationAsset() = default;
        explicit AnimationAsset(AssetHandle animationSource, AssetHandle mesh, std::string animationName, 
                      bool extractRootMotion = false, u32 rootBoneIndex = 0,
                      const glm::vec3& rootTranslationMask = glm::vec3(1.0f),
                      const glm::vec3& rootRotationMask = glm::vec3(1.0f),
                      bool discardRootMotion = false);

        static AssetType GetStaticType() { return AssetType::AnimationClip; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }

        // Animation source and mesh association
        AssetHandle GetAnimationSource() const { return m_AnimationSource; }
        AssetHandle GetMeshHandle() const { return m_Mesh; }
        const std::string& GetAnimationName() const { return m_AnimationName; }

        // Root motion extraction parameters
        bool IsExtractRootMotion() const { return m_IsExtractRootMotion; }
        u32 GetRootBoneIndex() const { return m_RootBoneIndex; }
        const glm::vec3& GetRootTranslationMask() const { return m_RootTranslationMask; }
        const glm::vec3& GetRootRotationMask() const { return m_RootRotationMask; }
        bool IsDiscardRootMotion() const { return m_IsDiscardRootMotion; }

        // Animation clip access
        void SetAnimationClip(Ref<AnimationClip> clip) { m_AnimationClip = clip; }
        Ref<AnimationClip> GetAnimationClip() const { return m_AnimationClip; }

        // Compatibility methods
        const AnimationClip* GetAnimation() const { return m_AnimationClip.get(); }

        // Asset system integration
        virtual void OnDependencyUpdated(AssetHandle handle) override;

    private:
        // Source and target data
        AssetHandle m_AnimationSource = 0;  // MeshSource that contains the animation data
        AssetHandle m_Mesh = 0;             // Mesh this animation is designed for
        std::string m_AnimationName;        // Name of animation within source file
        
        // Root motion extraction settings
        bool m_IsExtractRootMotion = false;
        u32 m_RootBoneIndex = 0;
        glm::vec3 m_RootTranslationMask = glm::vec3(1.0f);
        glm::vec3 m_RootRotationMask = glm::vec3(1.0f);
        bool m_IsDiscardRootMotion = false;

        // The actual animation data
        Ref<AnimationClip> m_AnimationClip;
    };
}
