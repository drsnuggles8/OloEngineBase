#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/AnimationClip.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace OloEngine
{
    /**
     * @brief AnimatedModel class for loading and managing skeletal animated models
     * 
     * This class handles loading of skeletal animated models from various formats (glTF, FBX, etc.)
     * using Assimp. It creates MeshSource objects with separated bone influences, Skeleton data, and AnimationClip objects.
     */
    class AnimatedModel : public RefCounted
    {
    public:
        AnimatedModel() = default;
        AnimatedModel(const std::string& path);
        ~AnimatedModel() = default;

        void LoadModel(const std::string& path);
        
        // Accessors
        [[nodiscard]] const std::vector<Ref<MeshSource>>& GetMeshes() const { return m_Meshes; }
        [[nodiscard]] const std::vector<Material>& GetMaterials() const { return m_Materials; }
        [[nodiscard]] const Ref<Skeleton>& GetSkeleton() const { return m_Skeleton; }
        [[nodiscard]] const std::vector<Ref<AnimationClip>>& GetAnimations() const { return m_Animations; }
        [[nodiscard]] const BoundingBox& GetBoundingBox() const { return m_BoundingBox; }
        [[nodiscard]] const BoundingSphere& GetBoundingSphere() const { return m_BoundingSphere; }
        [[nodiscard]] const std::string& GetDirectory() const { return m_Directory; }
        
        // Get animation by name
        [[nodiscard]] Ref<AnimationClip> GetAnimation(const std::string& name) const;
        
        // Utility methods
        [[nodiscard]] bool HasAnimations() const { return !m_Animations.empty(); }
        [[nodiscard]] bool HasSkeleton() const { return m_Skeleton != nullptr; }

        // Static dynamic sampling methods for optimized keyframe storage (public for AnimationSystem)
        static glm::vec3 SampleBonePosition(const std::vector<BonePositionKey>& keys, f32 time);
        static glm::quat SampleBoneRotation(const std::vector<BoneRotationKey>& keys, f32 time);
        static glm::vec3 SampleBoneScale(const std::vector<BoneScaleKey>& keys, f32 time);

    private:
        // Model processing
        void ProcessNode(const aiNode* node, const aiScene* scene);
        Ref<MeshSource> ProcessMesh(const aiMesh* mesh, const aiScene* scene);
        
        // Material and texture loading
        std::vector<Ref<Texture2D>> LoadMaterialTextures(const aiMaterial* mat, const aiTextureType type);
        Material ProcessMaterial(const aiMaterial* mat);
        
        // Skeleton and animation processing
        void ProcessSkeleton(const aiScene* scene);
        void ProcessAnimations(const aiScene* scene);
        void ProcessBones(const aiMesh* mesh, std::vector<BoneInfluence>& outBoneInfluences);
        
        // Helper methods
        void CalculateBounds();
        glm::mat4 AssimpMatrixToGLM(const aiMatrix4x4& from);
        u32 FindBoneIndex(const std::string& boneName);
        
        // Animation sampling helpers
        glm::vec3 SamplePosition(const aiNodeAnim* nodeAnim, f64 time);
        glm::quat SampleRotation(const aiNodeAnim* nodeAnim, f64 time);
        glm::vec3 SampleScale(const aiNodeAnim* nodeAnim, f64 time);
        
        // Bone mapping structure
        struct BoneInfo
        {
            u32 Id;
            glm::mat4 Offset;
        };

        // Data members
        std::vector<Ref<MeshSource>> m_Meshes;
        std::vector<Material> m_Materials;
        std::vector<Ref<AnimationClip>> m_Animations;
        Ref<Skeleton> m_Skeleton;
        
        std::string m_Directory;
        std::unordered_map<std::string, Ref<Texture2D>> m_LoadedTextures;
        
        // Bone name to BoneInfo mapping for O(1) lookup during mesh processing.
        // Built once during ProcessSkeleton() and used by ProcessBones() for efficient bone index resolution.
        std::unordered_map<std::string, BoneInfo> m_BoneInfoMap;
        
        BoundingBox m_BoundingBox;
        BoundingSphere m_BoundingSphere;
        
        u32 m_BoneCounter = 0;
    };
}
