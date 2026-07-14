#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <glm/glm.hpp>
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RendererResource.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace OloEngine
{
    // Forward declaration for CommandPacket (defined in Commands/CommandPacket.h)
    class CommandPacket;
    // Configuration for overriding texture paths when model's embedded paths are incorrect
    struct TextureOverride
    {
        std::string AlbedoPath;
        std::string MetallicPath;
        std::string NormalPath;
        std::string RoughnessPath;
        std::string AOPath;
        std::string EmissivePath;

        bool HasAnyTexture() const
        {
            return !AlbedoPath.empty() || !MetallicPath.empty() || !NormalPath.empty() ||
                   !RoughnessPath.empty() || !AOPath.empty() || !EmissivePath.empty();
        }
    };

    class Model : public RendererResource
    {
      public:
        Model() = default;
        explicit Model(const std::string& path, const TextureOverride& textureOverride = {}, bool flipUV = false);
        ~Model() = default;

        void LoadModel(const std::string& path, const TextureOverride& textureOverride = {}, bool flipUV = false);
        void Draw(const glm::mat4& transform, const Material& material) const;
        void Draw(const glm::mat4& transform, const Ref<const Material>& material) const;

        // Parallel draw methods - uses SubmitMeshesParallel for efficient multi-threaded command generation
        void DrawParallel(const glm::mat4& transform, const Material& fallbackMaterial, i32 entityID = -1) const;
        void DrawParallel(const glm::mat4& transform, i32 entityID = -1) const;
        // Full material precedence: an explicit override (the entity's MaterialComponent, or
        // nullptr for none) -> the material each submesh was imported with -> fallbackMaterial.
        // Shares OloEngine::ResolveSubmeshMaterial with the MeshComponent and virtualized
        // paths so the three cannot drift apart again (issue #629).
        void DrawParallel(const glm::mat4& transform, const Material* overrideMaterial,
                          const Material& fallbackMaterial, i32 entityID) const;

        void GetDrawCommands(const glm::mat4& transform, const Material& material, std::vector<CommandPacket*>& outCommands) const;
        void GetDrawCommands(const glm::mat4& transform, const Ref<const Material>& material, std::vector<CommandPacket*>& outCommands) const;
        void GetDrawCommands(const glm::mat4& transform, std::vector<CommandPacket*>& outCommands) const;

        // Calculate bounding volumes for the entire model
        void CalculateBounds();

        // Bounding volume accessors
        [[nodiscard]] const BoundingBox& GetBoundingBox() const
        {
            return m_BoundingBox;
        }
        [[nodiscard]] const BoundingSphere& GetBoundingSphere() const
        {
            return m_BoundingSphere;
        }

        // Get transformed bounding volumes
        [[nodiscard]] BoundingBox GetTransformedBoundingBox(const glm::mat4& transform) const
        {
            return m_BoundingBox.Transform(transform);
        }
        [[nodiscard]] BoundingSphere GetTransformedBoundingSphere(const glm::mat4& transform) const
        {
            return m_BoundingSphere.Transform(transform);
        }

        // Accessors for materials
        [[nodiscard]] const std::vector<Ref<Material>>& GetMaterials() const
        {
            return m_Materials;
        }

        // Index-based material accessors with proper const-correctness
        [[nodiscard]] Ref<Material> GetMaterial(sizet index)
        {
            return index < m_Materials.size() ? m_Materials[index] : nullptr;
        }
        [[nodiscard]] const Ref<Material>& GetMaterial(sizet index) const
        {
            return index < m_Materials.size() ? m_Materials[index] : GetNullMaterialRef();
        }

        // Get material count for safe iteration
        [[nodiscard]] sizet GetMaterialCount() const
        {
            return m_Materials.size();
        }

        // Mesh accessors for extracting mesh data after loading
        [[nodiscard]] const std::vector<Ref<Mesh>>& GetMeshes() const
        {
            return m_Meshes;
        }

        [[nodiscard]] sizet GetMeshCount() const
        {
            return m_Meshes.size();
        }

        [[nodiscard]] Ref<Mesh> GetMesh(sizet index) const
        {
            return index < m_Meshes.size() ? m_Meshes[index] : nullptr;
        }

        // Create a combined MeshSource from all meshes in the model
        // Each mesh becomes a submesh in the combined MeshSource
        [[nodiscard]] Ref<MeshSource> CreateCombinedMeshSource() const;

      private:
        // Helper method to return a null material reference for const access
        static const Ref<Material>& GetNullMaterialRef()
        {
            static const Ref<Material> nullMaterial = nullptr;
            return nullMaterial;
        }

      public:
        // Asset interface — must be public so AssetManager::GetAsset<Model>() /
        // GetAllAssetsWithType<T>() can read the static type tag.
        constexpr static AssetType GetStaticType()
        {
            return AssetType::Model;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

      private:
        void ProcessNode(const aiNode* node, const aiScene* scene);
        Ref<Mesh> ProcessMesh(const aiMesh* mesh, const aiScene* scene);
        std::vector<Ref<Texture2D>> LoadMaterialTextures(const aiMaterial* mat, const aiTextureType type, const aiScene* scene);
        Ref<Material> ProcessMaterial(const aiMaterial* mat, const aiScene* scene);
        // Builds the virtualized-geometry cluster DAG (issue #629) for the
        // combined source and serializes it into m_CookedVirtualMeshBlob, so
        // both the .omesh cache write and any later CreateCombinedMeshSource
        // result carry the cook. No-op for multi-submesh / rigged / tiny meshes.
        void CookVirtualMesh(const MeshSource& combined);

        std::vector<Ref<Mesh>> m_Meshes;
        // The already-combined MeshSource restored from the .omesh cache. On that path every
        // m_Meshes[i] is a submesh *view* into this one source rather than a source of its own,
        // so CreateCombinedMeshSource must return it instead of concatenating (see there).
        Ref<MeshSource> m_CachedCombinedSource;
        std::vector<u8> m_CookedVirtualMeshBlob;         // OVGM cook; attached by CreateCombinedMeshSource
        std::vector<Ref<Material>> m_Materials;          // Materials corresponding to each mesh
        std::unordered_map<u32, u32> m_MaterialIndexMap; // Maps Assimp material indices to m_Materials indices
        std::string m_Directory;
        std::unordered_map<std::string, Ref<Texture2D>> m_LoadedTextures;
        std::optional<TextureOverride> m_TextureOverride;
        bool m_FlipUV = false;

        BoundingBox m_BoundingBox;
        BoundingSphere m_BoundingSphere;
    };
} // namespace OloEngine
