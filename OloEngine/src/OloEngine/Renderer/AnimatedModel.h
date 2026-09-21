#pragma once

#include <string>
#include <span>
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
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
    // @brief AnimatedModel class for loading and managing skeletal animated models
    //
    // This class handles loading of skeletal animated models from various formats (glTF, FBX, etc.)
    // using Assimp. It creates MeshSource objects with separated bone influences, Skeleton data, and AnimationClip objects.
    class AnimatedModel : public RefCounted
    {
      public:
        // .omesh cache namespace for animated imports. Public because the routing decision
        // in AssimpMeshImporter has to ask "is there already a warm ANIMATED cache for this
        // file?" before it decides which importer to run (issue #1272).
        static constexpr const char* kCachePrefix = "anim_";

        AnimatedModel() = default;
        AnimatedModel(const std::string& path);
        ~AnimatedModel() = default;

        void LoadModel(const std::string& path);

        // Flatten the loaded meshes into ONE MeshSource: geometry, bone influences, bone
        // info, the skeleton, merged morph targets, and the imported materials, with each
        // mesh as one submesh. This is the shape the asset pipeline wants -- a MeshSource
        // asset that a VirtualMeshComponent or MeshComponent can render from a handle alone
        // (issue #1272).
        //
        // Like Model::CreateCombinedMeshSource it deliberately does NOT Build(): the result
        // is also used for cache-only serialization on the headless path, and the caller
        // decides. The result is marked pre-optimized, because every input already went
        // through OptimizeMesh.
        [[nodiscard]] Ref<MeshSource> CreateCombinedMeshSource() const;

        // Accessors
        [[nodiscard]] std::span<const Ref<MeshSource>> GetMeshes() const
        {
            return { m_Meshes.GetData(), static_cast<sizet>(m_Meshes.Num()) };
        }
        [[nodiscard]] std::span<const Material> GetMaterials() const
        {
            return { m_Materials.GetData(), static_cast<sizet>(m_Materials.Num()) };
        }
        [[nodiscard]] const Ref<Skeleton>& GetSkeleton() const
        {
            return m_Skeleton;
        }
        [[nodiscard]] std::span<const Ref<AnimationClip>> GetAnimations() const
        {
            return { m_Animations.GetData(), static_cast<sizet>(m_Animations.Num()) };
        }
        [[nodiscard]] const BoundingBox& GetBoundingBox() const
        {
            return m_BoundingBox;
        }
        [[nodiscard]] const BoundingSphere& GetBoundingSphere() const
        {
            return m_BoundingSphere;
        }
        [[nodiscard]] const FString& GetDirectory() const
        {
            return m_Directory;
        }

        // Get animation by name
        [[nodiscard]] Ref<AnimationClip> GetAnimation(const std::string& name) const;

        // Utility methods
        [[nodiscard]] bool HasAnimations() const
        {
            return !m_Animations.IsEmpty();
        }
        [[nodiscard]] bool HasSkeleton() const
        {
            return m_Skeleton != nullptr;
        }

        // Static dynamic sampling methods for optimized keyframe storage (public for AnimationSystem)
        static glm::vec3 SampleBonePosition(std::span<const BonePositionKey> keys, f32 time);
        static glm::quat SampleBoneRotation(std::span<const BoneRotationKey> keys, f32 time);
        static glm::vec3 SampleBoneScale(std::span<const BoneScaleKey> keys, f32 time);

      private:
        // Model processing
        void ProcessNode(const aiNode* node, const aiScene* scene, const glm::mat4& parentTransform);
        Ref<MeshSource> ProcessMesh(const aiMesh* mesh, const aiScene* scene);

        // Material and texture loading
        // `semanticIndex` >= 0 probes EXACTLY that semantic index instead of
        // iterating GetTextureCount -- which is the only way to reach glTF's
        // volume THICKNESS map, since it shares aiTextureType_TRANSMISSION with
        // the transmission map and is distinguished only by index 1 (issue
        // #1242). -1, the default, keeps the historical count-driven behaviour.
        TArray<Ref<Texture2D>> LoadMaterialTextures(const aiMaterial* mat, aiTextureType type,
                                                    i32 semanticIndex = -1);
        Material ProcessMaterial(const aiMaterial* mat);

        // Skeleton and animation processing
        void ProcessSkeleton(const aiScene* scene);
        void ProcessAnimations(const aiScene* scene);
        void ProcessBones(const aiMesh* mesh, TArray<BoneInfluence>& outBoneInfluences);

        // Helper methods
        void CalculateBounds();
        glm::mat4 AssimpMatrixToGLM(const aiMatrix4x4& from) const;
        u32 FindBoneIndex(const std::string& boneName);

        // Animation sampling helpers
        glm::vec3 SamplePosition(const aiNodeAnim* nodeAnim, f64 time) const;
        glm::quat SampleRotation(const aiNodeAnim* nodeAnim, f64 time) const;
        glm::vec3 SampleScale(const aiNodeAnim* nodeAnim, f64 time) const;

        // Bone mapping structure
        struct BoneInfo
        {
            u32 Id;
            glm::mat4 Offset;
        };

        // Data members
        TArray<Ref<MeshSource>> m_Meshes;
        TArray<Material> m_Materials;
        TArray<Ref<AnimationClip>> m_Animations;
        Ref<Skeleton> m_Skeleton;

        FString m_Directory;
        std::unordered_map<std::string, Ref<Texture2D>> m_LoadedTextures;

        // Bone name to BoneInfo mapping for O(1) lookup during mesh processing.
        // Built once during ProcessSkeleton() and used by ProcessBones() for efficient bone index resolution.
        std::unordered_map<std::string, BoneInfo> m_BoneInfoMap;

        BoundingBox m_BoundingBox;
        BoundingSphere m_BoundingSphere;

        // Global transform of the first mesh node encountered during ProcessNode.
        // Used to correct axis orientation: mesh vertices are in mesh-local space,
        // but skinning expects them relative to the scene root coordinate system.
        glm::mat4 m_MeshNodeGlobalTransform{ 1.0f };
        bool m_HasMeshNodeTransform = false;

        u32 m_BoneCounter = 0;
    };
} // namespace OloEngine
