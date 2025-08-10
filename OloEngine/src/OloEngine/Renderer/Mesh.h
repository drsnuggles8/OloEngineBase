#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "MeshSource.h"
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
}
