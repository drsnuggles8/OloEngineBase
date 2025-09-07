#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "MeshSource.h"
#include "MaterialAsset.h"
#include <vector>
#include <glm/mat4x4.hpp>

namespace OloEngine 
{
    /**
     * @brief Mesh asset that references a MeshSource and specifies a submesh index
     * 
     * Similar to Hazel's Mesh class, this acts as a lightweight reference to a specific
     * submesh within a MeshSource. Multiple Mesh assets can reference the same MeshSource
     * but different submeshes.
     */
    class Mesh : public Asset
    {
    public:
        Mesh() = default;
        explicit Mesh(Ref<MeshSource> meshSource, u32 submeshIndex = 0);
        ~Mesh() override = default;

        // MeshSource and submesh access
        Ref<MeshSource> GetMeshSource() const { return m_MeshSource; }
        void SetMeshSource(Ref<MeshSource> meshSource);
        
        u32 GetSubmeshIndex() const { return m_SubmeshIndex; }
        void SetSubmeshIndex(u32 submeshIndex);
        
        // Validation
        bool IsValid() const { return m_MeshSource && m_SubmeshIndex < m_MeshSource->GetSubmeshes().size(); }

        // Convenience accessors that delegate to MeshSource
        const std::vector<Vertex>& GetVertices() const;
        const std::vector<u32>& GetIndices() const;
        Ref<VertexArray> GetVertexArray() const;
        
        // Submesh-specific data
        const Submesh& GetSubmesh() const;
        bool IsRigged() const;
        
        // Bounding volume accessors for this specific submesh
        BoundingBox GetBoundingBox() const;
        BoundingSphere GetBoundingSphere() const;
        
        // Get transformed bounding volumes
        BoundingBox GetTransformedBoundingBox(const glm::mat4& transform) const;
        BoundingSphere GetTransformedBoundingSphere(const glm::mat4& transform) const;
        
        u32 GetRendererID() const;
        u32 GetIndexCount() const;

        // Asset interface
        static AssetType GetStaticType() { return AssetType::Mesh; }
        AssetType GetAssetType() const override { return GetStaticType(); }

    private:
        Ref<MeshSource> m_MeshSource;
        u32 m_SubmeshIndex = 0;
    };

    /**
     * @brief Static mesh asset - a flattened mesh without skeletal animation support
     * 
     * StaticMesh represents a mesh optimized for static rendering. Unlike the dynamic Mesh class,
     * StaticMesh doesn't retain the node hierarchy and doesn't support skeletal animation.
     * It references a MeshSource and can specify multiple submeshes for rendering.
     * This follows the Hazel pattern for compatibility.
     */
    class StaticMesh : public Asset
    {
    public:
        explicit StaticMesh(AssetHandle meshSource, bool generateColliders = false);
        StaticMesh(AssetHandle meshSource, const std::vector<u32>& submeshes, bool generateColliders = false);
        virtual ~StaticMesh() = default;

        virtual void OnDependencyUpdated(AssetHandle handle) override;

        // Submesh management
        const std::vector<u32>& GetSubmeshes() const { return m_Submeshes; }
        void SetSubmeshes(const std::vector<u32>& submeshes, Ref<MeshSource> meshSourceAsset);

        // MeshSource access
        AssetHandle GetMeshSource() const { return m_MeshSource; }
        void SetMeshAsset(AssetHandle meshSource) { m_MeshSource = meshSource; }

        // Materials
        Ref<MaterialTable> GetMaterials() const { return m_Materials; }

        // Collider generation
        bool ShouldGenerateColliders() const { return m_GenerateColliders; }

        // Asset interface
        static AssetType GetStaticType() { return AssetType::StaticMesh; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }

    private:
        void SetupStaticMesh();

        AssetHandle m_MeshSource;
        std::vector<u32> m_Submeshes; // Submesh indices to render

        // Materials
        Ref<MaterialTable> m_Materials;

        bool m_GenerateColliders = false; // should we generate physics colliders when (re)loading this static mesh?
    };
}
