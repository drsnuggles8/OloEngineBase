#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <vector>
#include <memory>

namespace OloEngine 
{
    class SkinnedMesh 
    {
    public:
        SkinnedMesh() = default;
        SkinnedMesh(const std::vector<SkinnedVertex>& vertices, const std::vector<u32>& indices);
        SkinnedMesh(std::vector<SkinnedVertex>&& vertices, std::vector<u32>&& indices);
        ~SkinnedMesh() = default;

        void SetVertices(const std::vector<SkinnedVertex>& vertices);
        void SetVertices(std::vector<SkinnedVertex>&& vertices);
        void SetIndices(const std::vector<u32>& indices);
        void SetIndices(std::vector<u32>&& indices);

        void Build();
        void CalculateBounds();

        // Create primitive skinned meshes
        static Ref<SkinnedMesh> CreateCube();
        static Ref<SkinnedMesh> CreateMultiBoneCube(); // Cube with multiple bone influences

        // Draw the mesh
        void Draw() const;

        [[nodiscard]] const std::vector<SkinnedVertex>& GetVertices() const { return m_Vertices; }
        [[nodiscard]] const std::vector<u32>& GetIndices() const { return m_Indices; }
        [[nodiscard]] const Ref<VertexArray>& GetVertexArray() const { return m_VertexArray; }
        
        // Bounding volume accessors
        [[nodiscard]] const BoundingBox& GetBoundingBox() const { return m_BoundingBox; }
        [[nodiscard]] const BoundingSphere& GetBoundingSphere() const { return m_BoundingSphere; }
        
        // Get transformed bounding volumes
        [[nodiscard]] BoundingBox GetTransformedBoundingBox(const glm::mat4& transform) const { return m_BoundingBox.Transform(transform); }
        [[nodiscard]] BoundingSphere GetTransformedBoundingSphere(const glm::mat4& transform) const { return m_BoundingSphere.Transform(transform); }
		[[nodiscard]] u32 GetRendererID() const { return m_VertexArray ? m_VertexArray->GetRendererID() : 0; }
        [[nodiscard]] u32 GetIndexCount() const { return static_cast<u32>(m_Indices.size()); }

    private:
        std::vector<SkinnedVertex> m_Vertices;
        std::vector<u32> m_Indices;
        
        Ref<VertexArray> m_VertexArray;
        Ref<VertexBuffer> m_VertexBuffer;
        Ref<IndexBuffer> m_IndexBuffer;
        
        BoundingBox m_BoundingBox;
        BoundingSphere m_BoundingSphere;
        
        bool m_Built = false;
    };
}
