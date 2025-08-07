#include "OloEnginePCH.h"
#include "MeshSource.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"

namespace OloEngine
{
    MeshSource::MeshSource(const std::vector<Vertex>& vertices, const std::vector<u32>& indices)
        : m_Vertices(vertices), m_Indices(indices)
    {
        Build();
        CalculateBounds();
    }

    MeshSource::MeshSource(std::vector<Vertex>&& vertices, std::vector<u32>&& indices)
        : m_Vertices(std::move(vertices)), m_Indices(std::move(indices))
    {
        Build();
        CalculateBounds();
    }

    void MeshSource::Build()
    {
        if (m_Built)
            return;

        BuildVertexBuffer();
        BuildIndexBuffer();
        
        m_VertexArray = VertexArray::Create();
        m_VertexArray->Bind();
        
        m_VertexBuffer->Bind();
        m_VertexArray->AddVertexBuffer(m_VertexBuffer);
        
        m_IndexBuffer->Bind();
        m_VertexArray->SetIndexBuffer(m_IndexBuffer);
        
        m_VertexArray->Unbind();
        
        m_Built = true;
    }

    void MeshSource::BuildVertexBuffer()
    {
        if (m_Vertices.empty())
            return;

        m_VertexBuffer = VertexBuffer::Create(reinterpret_cast<f32*>(m_Vertices.data()), 
                                             static_cast<u32>(m_Vertices.size() * sizeof(Vertex)));
        m_VertexBuffer->SetLayout(Vertex::GetLayout());
    }

    void MeshSource::BuildIndexBuffer()
    {
        if (m_Indices.empty())
            return;

        m_IndexBuffer = IndexBuffer::Create(m_Indices.data(), 
                                           static_cast<u32>(m_Indices.size()));
    }

    void MeshSource::CalculateBounds()
    {
        if (m_Vertices.empty())
        {
            m_BoundingBox = BoundingBox();
            m_BoundingSphere = BoundingSphere();
            return;
        }

        glm::vec3 min = m_Vertices[0].Position;
        glm::vec3 max = m_Vertices[0].Position;

        for (const auto& vertex : m_Vertices)
        {
            min = glm::min(min, vertex.Position);
            max = glm::max(max, vertex.Position);
        }

        m_BoundingBox = BoundingBox(min, max);
        
        glm::vec3 center = (min + max) * 0.5f;
        f32 radius = 0.0f;
        
        for (const auto& vertex : m_Vertices)
        {
            f32 distance = glm::length(vertex.Position - center);
            radius = glm::max(radius, distance);
        }
        
        m_BoundingSphere = BoundingSphere(center, radius);
    }
}
