#pragma once

#include <functional>
#include <string>
#include <span>
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
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
        FString AlbedoPath;
        FString MetallicPath;
        FString NormalPath;
        FString RoughnessPath;
        FString AOPath;
        FString EmissivePath;

        bool HasAnyTexture() const
        {
            return !AlbedoPath.IsEmpty() || !MetallicPath.IsEmpty() || !NormalPath.IsEmpty() ||
                   !RoughnessPath.IsEmpty() || !AOPath.IsEmpty() || !EmissivePath.IsEmpty();
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
        // As above, plus a baked lightmap region per mesh (issue #867).
        //
        // `lightmapRegionForMesh(meshIndex)` is called once per mesh and returns
        // that mesh's ENCODED atlas region, or vec4(0) for "no lightmap". A
        // callback rather than a span because the caller has to resolve the
        // model's sub-key per mesh anyway (LightmapSubKeyForModelMesh), and a
        // model with no bake must cost nothing.
        void DrawParallel(const glm::mat4& transform, const Material* overrideMaterial,
                          const Material& fallbackMaterial, i32 entityID,
                          const std::function<glm::vec4(sizet)>& lightmapRegionForMesh) const;

        void GetDrawCommands(const glm::mat4& transform, const Material& material, TArray<CommandPacket*>& outCommands) const;
        void GetDrawCommands(const glm::mat4& transform, const Ref<const Material>& material, TArray<CommandPacket*>& outCommands) const;
        void GetDrawCommands(const glm::mat4& transform, TArray<CommandPacket*>& outCommands) const;

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
        [[nodiscard]] std::span<const Ref<Material>> GetMaterials() const
        {
            return { m_Materials.GetData(), static_cast<sizet>(m_Materials.Num()) };
        }

        // Index-based material accessors with proper const-correctness
        [[nodiscard]] Ref<Material> GetMaterial(sizet index)
        {
            return index < static_cast<sizet>(m_Materials.Num()) ? m_Materials[index] : nullptr;
        }
        [[nodiscard]] const Ref<Material>& GetMaterial(sizet index) const
        {
            return index < static_cast<sizet>(m_Materials.Num()) ? m_Materials[index] : GetNullMaterialRef();
        }

        // Get material count for safe iteration
        [[nodiscard]] sizet GetMaterialCount() const
        {
            return static_cast<sizet>(m_Materials.Num());
        }

        // Mesh accessors for extracting mesh data after loading
        [[nodiscard]] std::span<const Ref<Mesh>> GetMeshes() const
        {
            return { m_Meshes.GetData(), static_cast<sizet>(m_Meshes.Num()) };
        }

        [[nodiscard]] sizet GetMeshCount() const
        {
            return static_cast<sizet>(m_Meshes.Num());
        }

        [[nodiscard]] Ref<Mesh> GetMesh(sizet index) const
        {
            return index < static_cast<sizet>(m_Meshes.Num()) ? m_Meshes[index] : nullptr;
        }

        // Create a combined MeshSource from all meshes in the model
        // Each mesh becomes a submesh in the combined MeshSource
        [[nodiscard]] Ref<MeshSource> CreateCombinedMeshSource() const;

        // Did the source file contain bones (issue #1272)?
        //
        // Model does not IMPORT bones -- it never has, and teaching it to would give the
        // engine two importers that must agree about what a mesh is. It only OBSERVES the
        // one bit that says another importer should have handled this file, both on a cold
        // Assimp import (aiMesh::mNumBones) and on a warm .omesh load (the cached
        // FlagSourceRigged). AssimpMeshImporter uses it to re-route to AnimatedModel.
        [[nodiscard]] bool IsSourceRigged() const
        {
            return m_SourceIsRigged;
        }

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
        // `semanticIndex` >= 0 probes EXACTLY that semantic index instead of
        // iterating GetTextureCount -- which is the only way to reach glTF's
        // volume THICKNESS map, since it shares aiTextureType_TRANSMISSION with
        // the transmission map and is distinguished only by index 1 (issue
        // #1242). -1, the default, keeps the historical count-driven behaviour.
        TArray<Ref<Texture2D>> LoadMaterialTextures(const aiMaterial* mat, aiTextureType type,
                                                    const aiScene* scene, i32 semanticIndex = -1);
        Ref<Material> ProcessMaterial(const aiMaterial* mat, const aiScene* scene);
        // Builds the virtualized-geometry cluster DAG (issue #629) for the
        // combined source and serializes it into m_CookedVirtualMeshBlob, so
        // both the .omesh cache write and any later CreateCombinedMeshSource
        // result carry the cook. No-op for multi-submesh / rigged / tiny meshes.
        void CookVirtualMesh(const MeshSource& combined);

        TArray<Ref<Mesh>> m_Meshes;
        // The already-combined MeshSource restored from the .omesh cache. On that path every
        // m_Meshes[i] is a submesh *view* into this one source rather than a source of its own,
        // so CreateCombinedMeshSource must return it instead of concatenating (see there).
        Ref<MeshSource> m_CachedCombinedSource;
        TArray<u8> m_CookedVirtualMeshBlob;              // OVGM cook; attached by CreateCombinedMeshSource
        TArray<Ref<Material>> m_Materials;               // Materials corresponding to each mesh
        std::unordered_map<u32, u32> m_MaterialIndexMap; // Maps Assimp material indices to m_Materials indices
        FString m_Directory;
        // The model file this Model was loaded from. Used (with the Assimp texture
        // reference) to derive the STABLE cooked filename of an embedded texture, so
        // re-importing the same model reuses the same cooked asset and its handle.
        FString m_SourcePath;
        std::unordered_map<std::string, Ref<Texture2D>> m_LoadedTextures;
        std::optional<TextureOverride> m_TextureOverride;
        bool m_FlipUV = false;
        bool m_SourceIsRigged = false; // Source had bones; see IsSourceRigged() (issue #1272)

        BoundingBox m_BoundingBox;
        BoundingSphere m_BoundingSphere;
    };
} // namespace OloEngine
