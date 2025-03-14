#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <vector>
#include <memory>

namespace OloEngine 
{
    class Mesh 
    {
    public:
        Mesh() = default;
        Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
        Mesh(std::vector<Vertex>&& vertices, std::vector<uint32_t>&& indices);
        ~Mesh() = default;

        void SetVertices(const std::vector<Vertex>& vertices);
        void SetVertices(std::vector<Vertex>&& vertices);
        void SetIndices(const std::vector<uint32_t>& indices);
        void SetIndices(std::vector<uint32_t>&& indices);

        void Build();

        // Create primitive meshes - these return newly created meshes
        static Ref<Mesh> CreateCube();
        static Ref<Mesh> CreateSphere(float radius = 1.0f, uint32_t segments = 16);
        static Ref<Mesh> CreatePlane(float width = 1.0f, float length = 1.0f);

        // Draw the mesh
        void Draw() const;

        [[nodiscard]] const std::vector<Vertex>& GetVertices() const { return m_Vertices; }
        [[nodiscard]] const std::vector<uint32_t>& GetIndices() const { return m_Indices; }
        [[nodiscard]] const Ref<VertexArray>& GetVertexArray() const { return m_VertexArray; }

    private:
        std::vector<Vertex> m_Vertices;
        std::vector<uint32_t> m_Indices;
        
        Ref<VertexArray> m_VertexArray;
        Ref<VertexBuffer> m_VertexBuffer;
        Ref<IndexBuffer> m_IndexBuffer;
        
        bool m_Built = false;
    };
}