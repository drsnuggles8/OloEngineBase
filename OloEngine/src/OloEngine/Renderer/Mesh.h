#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <vector>
#include <memory>

namespace OloEngine 
{
    class Mesh : public RefCounted
    {
    public:
        Mesh() = default;
        Mesh(const std::vector<Vertex>& vertices, const std::vector<u32>& indices);
        Mesh(std::vector<Vertex>&& vertices, std::vector<u32>&& indices);
        ~Mesh() = default;

        void SetVertices(const std::vector<Vertex>& vertices);
        void SetVertices(std::vector<Vertex>&& vertices);
        void SetIndices(const std::vector<u32>& indices);
        void SetIndices(std::vector<u32>&& indices);

        void Build();
        void CalculateBounds();

        // Create primitive meshes - these return newly created meshes
        static AssetRef<Mesh> CreateCube();
        static Ref<Mesh> CreateSkyboxCube(); // Special cube for skybox rendering
        static AssetRef<Mesh> CreateSphere(f32 radius = 1.0f, u32 segments = 16);
        static Ref<Mesh> CreatePlane(f32 width = 1.0f, f32 length = 1.0f);

        // Draw the mesh
        void Draw() const;

        [[nodiscard]] const std::vector<Vertex>& GetVertices() const { return m_Vertices; }
        [[nodiscard]] const std::vector<u32>& GetIndices() const { return m_Indices; }
        [[nodiscard]] const AssetRef<VertexArray>& GetVertexArray() const { return m_VertexArray; }
        
        // Bounding volume accessors
        [[nodiscard]] const BoundingBox& GetBoundingBox() const { return m_BoundingBox; }
        [[nodiscard]] const BoundingSphere& GetBoundingSphere() const { return m_BoundingSphere; }
        
        // Get transformed bounding volumes
        [[nodiscard]] BoundingBox GetTransformedBoundingBox(const glm::mat4& transform) const { return m_BoundingBox.Transform(transform); }
        [[nodiscard]] BoundingSphere GetTransformedBoundingSphere(const glm::mat4& transform) const { return m_BoundingSphere.Transform(transform); }

		// Add to Mesh.h in the public section
		[[nodiscard]] u32 GetRendererID() const { return m_VertexArray ? m_VertexArray->GetRendererID() : 0; }
		[[nodiscard]] u32 GetIndexCount() const { return static_cast<u32>(m_Indices.size()); }

    private:
        std::vector<Vertex> m_Vertices;
        std::vector<u32> m_Indices;
        
        AssetRef<VertexArray> m_VertexArray;
        Ref<VertexBuffer> m_VertexBuffer;
        Ref<IndexBuffer> m_IndexBuffer;
        
        BoundingBox m_BoundingBox;
        BoundingSphere m_BoundingSphere;
        
        bool m_Built = false;
    };
}