#include "OloEnginePCH.h"
#include "MeshSource.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Task/ParallelFor.h"

namespace OloEngine
{
    // Context structs for ParallelForWithTaskContext - must be in OloEngine namespace for ADL
    struct FBoundsContext
    {
        glm::vec3 Min{ std::numeric_limits<f32>::max() };
        glm::vec3 Max{ std::numeric_limits<f32>::lowest() };
    };

    struct FRadiusContext
    {
        f32 MaxRadius = 0.0f;
    };

    MeshSource::MeshSource(const TArray<Vertex>& vertices, const TArray<u32>& indices)
        : m_Vertices(vertices), m_Indices(indices)
    {
        // Initialize bone influences with same size as vertices (all zeroed)
        m_BoneInfluences.SetNum(vertices.Num());
        CalculateBounds();
    }

    MeshSource::MeshSource(TArray<Vertex>&& vertices, TArray<u32>&& indices)
        : m_Vertices(MoveTemp(vertices)), m_Indices(MoveTemp(indices))
    {
        // Initialize bone influences with same size as vertices (all zeroed)
        m_BoneInfluences.SetNum(m_Vertices.Num());
        CalculateBounds();
    }

    MeshSource::MeshSource(const std::vector<Vertex>& vertices, const std::vector<u32>& indices)
    {
        // Convert std::vector to TArray
        m_Vertices.Reserve(static_cast<i32>(vertices.size()));
        m_Vertices.Append(vertices.data(), static_cast<i32>(vertices.size()));
        m_Indices.Reserve(static_cast<i32>(indices.size()));
        m_Indices.Append(indices.data(), static_cast<i32>(indices.size()));
        // Initialize bone influences with same size as vertices (all zeroed)
        m_BoneInfluences.SetNum(m_Vertices.Num());
        CalculateBounds();
    }

    MeshSource::MeshSource(std::vector<Vertex>&& vertices, std::vector<u32>&& indices)
    {
        // Convert std::vector to TArray (move data)
        m_Vertices.Reserve(static_cast<i32>(vertices.size()));
        for (auto& v : vertices)
        {
            m_Vertices.Add(std::move(v));
        }
        m_Indices.Reserve(static_cast<i32>(indices.size()));
        m_Indices.Append(indices.data(), static_cast<i32>(indices.size()));
        // Initialize bone influences with same size as vertices (all zeroed)
        m_BoneInfluences.SetNum(m_Vertices.Num());
        CalculateBounds();
    }

    void MeshSource::Build()
    {
        if (m_Built)
            return;

        // Ensure bounds are calculated before building GPU resources
        CalculateBounds();
        CalculateSubmeshBounds();

        BuildVertexBuffer();
        BuildIndexBuffer();

        // Build bone influence buffer if we have rigged data
        if (HasSkeleton() && !m_BoneInfluences.IsEmpty())
        {
            BuildBoneInfluenceBuffer();
        }

        m_VertexArray = VertexArray::Create();
        m_VertexArray->Bind();

        m_VertexBuffer->Bind();
        m_VertexArray->AddVertexBuffer(m_VertexBuffer);

        // Add bone influence buffer as second vertex buffer if available
        if (m_BoneInfluenceBuffer)
        {
            m_BoneInfluenceBuffer->Bind();
            m_VertexArray->AddVertexBuffer(m_BoneInfluenceBuffer);
        }

        m_IndexBuffer->Bind();
        m_VertexArray->SetIndexBuffer(m_IndexBuffer);

        m_VertexArray->Unbind();

        m_Built = true;
    }

    void MeshSource::BuildVertexBuffer()
    {
        if (m_Vertices.IsEmpty())
            return;

        m_VertexBuffer = VertexBuffer::Create(static_cast<const void*>(m_Vertices.GetData()),
                                              static_cast<u32>(m_Vertices.Num() * sizeof(Vertex)));
        m_VertexBuffer->SetLayout(Vertex::GetLayout());
    }

    void MeshSource::BuildIndexBuffer()
    {
        if (m_Indices.IsEmpty())
            return;

        m_IndexBuffer = IndexBuffer::Create(m_Indices.GetData(),
                                            static_cast<u32>(m_Indices.Num()));
    }

    void MeshSource::BuildBoneInfluenceBuffer()
    {
        if (m_BoneInfluences.IsEmpty())
            return;

        // Create buffer layout for bone influences
        BufferLayout boneInfluenceLayout = {
            { ShaderDataType::Int4, "a_BoneIDs" },      // 4 bone IDs
            { ShaderDataType::Float4, "a_BoneWeights" } // 4 bone weights
        };

        m_BoneInfluenceBuffer = VertexBuffer::Create(static_cast<const void*>(m_BoneInfluences.GetData()),
                                                     static_cast<u32>(m_BoneInfluences.Num() * sizeof(BoneInfluence)));
        m_BoneInfluenceBuffer->SetLayout(boneInfluenceLayout);
    }

    void MeshSource::CalculateBounds()
    {
        if (m_Vertices.IsEmpty())
        {
            m_BoundingBox = BoundingBox();
            m_BoundingSphere = BoundingSphere();
            return;
        }

        const i32 vertexCount = m_Vertices.Num();

        // Threshold for parallel processing - small meshes aren't worth the overhead
        constexpr i32 PARALLEL_THRESHOLD = 1024;

        glm::vec3 min, max;

        if (vertexCount >= PARALLEL_THRESHOLD)
        {
            // Use parallel reduction for large meshes
            TArray<FBoundsContext> contexts;
            ParallelForWithTaskContext(
                "MeshSource::CalculateBounds",
                contexts,
                vertexCount,
                256, // MinBatchSize
                [this](FBoundsContext& ctx, i32 index)
                {
                    const glm::vec3& pos = m_Vertices[index].Position;
                    ctx.Min = glm::min(ctx.Min, pos);
                    ctx.Max = glm::max(ctx.Max, pos);
                });

            // Reduce contexts to get final min/max
            min = glm::vec3(std::numeric_limits<f32>::max());
            max = glm::vec3(std::numeric_limits<f32>::lowest());
            for (const auto& ctx : contexts)
            {
                min = glm::min(min, ctx.Min);
                max = glm::max(max, ctx.Max);
            }
        }
        else
        {
            // Sequential for small meshes
            min = m_Vertices[0].Position;
            max = m_Vertices[0].Position;
            for (const auto& vertex : m_Vertices)
            {
                min = glm::min(min, vertex.Position);
                max = glm::max(max, vertex.Position);
            }
        }

        // For animated meshes, expand bounds to account for bone transformations
        if (HasBoneInfluences())
        {
            // Calculate the size of the bounding box
            glm::vec3 size = max - min;
            f32 maxDimension = glm::max(glm::max(size.x, size.y), size.z);

            // Expand by a large percentage for skeletal animation (200% to handle extended limbs)
            f32 expansionFactor = maxDimension * 2.0f;

            // Also ensure a substantial minimum expansion for small models
            expansionFactor = glm::max(expansionFactor, 0.5f);

            min -= glm::vec3(expansionFactor);
            max += glm::vec3(expansionFactor);
        }

        m_BoundingBox = BoundingBox(min, max);

        glm::vec3 center = (min + max) * 0.5f;
        f32 radius = 0.0f;

        if (vertexCount >= PARALLEL_THRESHOLD)
        {
            // Parallel radius calculation
            TArray<FRadiusContext> radiusContexts;
            ParallelForWithTaskContext(
                "MeshSource::CalculateSphereRadius",
                radiusContexts,
                vertexCount,
                256, // MinBatchSize
                [this, center](FRadiusContext& ctx, i32 index)
                {
                    f32 distance = glm::length(m_Vertices[index].Position - center);
                    ctx.MaxRadius = glm::max(ctx.MaxRadius, distance);
                });

            // Reduce to find max radius
            for (const auto& ctx : radiusContexts)
            {
                radius = glm::max(radius, ctx.MaxRadius);
            }
        }
        else
        {
            // Sequential for small meshes
            for (const auto& vertex : m_Vertices)
            {
                f32 distance = glm::length(vertex.Position - center);
                radius = glm::max(radius, distance);
            }
        }

        // For animated meshes, also expand the bounding sphere radius
        if (HasBoneInfluences())
        {
            radius *= 1.5f; // 50% expansion for animated models
        }

        m_BoundingSphere = BoundingSphere(center, radius);
    }

    void MeshSource::CalculateSubmeshBounds()
    {
        for (auto& submesh : m_Submeshes)
        {
            if (submesh.m_VertexCount == 0 || submesh.m_BaseVertex >= static_cast<u32>(m_Vertices.Num()))
            {
                submesh.m_BoundingBox = BoundingBox();
                continue;
            }

            // Calculate bounds for this specific submesh
            u32 startVertex = submesh.m_BaseVertex;
            u32 endVertex = std::min(startVertex + submesh.m_VertexCount, static_cast<u32>(m_Vertices.Num()));

            if (startVertex >= endVertex)
            {
                submesh.m_BoundingBox = BoundingBox();
                continue;
            }

            glm::vec3 min = m_Vertices[startVertex].Position;
            glm::vec3 max = m_Vertices[startVertex].Position;

            for (u32 i = startVertex; i < endVertex; i++)
            {
                min = glm::min(min, m_Vertices[i].Position);
                max = glm::max(max, m_Vertices[i].Position);
            }

            // For animated meshes, expand submesh bounds to account for bone transformations
            if (HasBoneInfluences())
            {
                // Calculate the size of the submesh bounding box
                glm::vec3 size = max - min;
                f32 maxDimension = glm::max(glm::max(size.x, size.y), size.z);

                // Expand by a large percentage for skeletal animation (200% to handle extended limbs)
                f32 expansionFactor = maxDimension * 2.0f;

                // Also ensure a substantial minimum expansion for small submeshes
                expansionFactor = glm::max(expansionFactor, 0.5f);

                min -= glm::vec3(expansionFactor);
                max += glm::vec3(expansionFactor);
            }

            submesh.m_BoundingBox = BoundingBox(min, max);
        }
    }
} // namespace OloEngine
