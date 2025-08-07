#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <glm/gtc/constants.hpp>

namespace OloEngine
{
    Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<u32>& indices)
        : m_Vertices(vertices), m_Indices(indices)
    {
        Build();
    }

    Mesh::Mesh(std::vector<Vertex>&& vertices, std::vector<u32>&& indices)
        : m_Vertices(std::move(vertices)), m_Indices(std::move(indices))
    {
        Build();
    }

    void Mesh::SetVertices(const std::vector<Vertex>& vertices)
    {
        m_Vertices = vertices;
        m_Built = false;
    }

    void Mesh::SetVertices(std::vector<Vertex>&& vertices)
    {
        m_Vertices = std::move(vertices);
        m_Built = false;
    }

    void Mesh::SetIndices(const std::vector<u32>& indices)
    {
        m_Indices = indices;
        m_Built = false;
    }

    void Mesh::SetIndices(std::vector<u32>&& indices)
    {
        m_Indices = std::move(indices);
        m_Built = false;
    }

    void Mesh::Build()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Vertices.empty() || m_Indices.empty())
        {
            OLO_CORE_WARN("Mesh::Build: Attempting to build a mesh with no vertices or indices!");
            return;
        }

        m_VertexArray = VertexArray::Create();

        m_VertexBuffer = VertexBuffer::Create(
            reinterpret_cast<f32*>(m_Vertices.data()),
            static_cast<u32>(m_Vertices.size() * sizeof(Vertex))
        );
        
        m_VertexBuffer->SetLayout(Vertex::GetLayout());
        m_VertexArray->AddVertexBuffer(m_VertexBuffer);

        m_IndexBuffer = IndexBuffer::Create(
            m_Indices.data(),
            static_cast<u32>(m_Indices.size())
        );

        m_VertexArray->SetIndexBuffer(m_IndexBuffer);
        
        // Calculate bounding volumes
        CalculateBounds();
        
        m_Built = true;
    }

    void Mesh::CalculateBounds()
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_Vertices.empty())
        {
            // Default to a unit cube around the origin if no vertices
            m_BoundingBox = BoundingBox(glm::vec3(-0.5f), glm::vec3(0.5f));
            m_BoundingSphere = BoundingSphere(glm::vec3(0.0f), 0.5f);
            return;
        }
        
        // Extract positions from vertices for bounding volume calculation
        std::vector<glm::vec3> positions;
        positions.reserve(m_Vertices.size());
        
        for (const auto& vertex : m_Vertices)
        {
            positions.push_back(vertex.Position);
        }
        
        // Create bounding box from positions
        m_BoundingBox = BoundingBox(positions.data(), positions.size());
        
        // Create bounding sphere from bounding box (more efficient than from points)
        m_BoundingSphere = BoundingSphere(m_BoundingBox);
    }

    void Mesh::Draw() const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Built)
        {
            OLO_CORE_WARN("Mesh::Draw: Attempting to draw a mesh that hasn't been built!");
            return;
        }

        m_VertexArray->Bind();
        RenderCommand::DrawIndexed(m_VertexArray);
    }

}
