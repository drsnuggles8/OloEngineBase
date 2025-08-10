#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>
#include <limits>

namespace OloEngine 
{
    // Forward declarations
    class VertexArray;
    class VertexBuffer;
    class IndexBuffer;

    /**
     * @brief Bone info structure for mapping mesh vertices to skeleton bones
     * Similar to Hazel's BoneInfo but adapted for OloEngine's architecture
     */
    struct BoneInfo
    {
        glm::mat4 m_InverseBindPose;  // Inverse bind pose matrix for skinning
        u32 m_BoneIndex;              // Index into the skeleton
        
        BoneInfo() : m_InverseBindPose(1.0f), m_BoneIndex(std::numeric_limits<u32>::max()) {}
        BoneInfo(const glm::mat4& inverseBindPose, u32 boneIndex)
            : m_InverseBindPose(inverseBindPose), m_BoneIndex(boneIndex) {}
    };

    /**
     * @brief Bone influence structure for vertex skinning data (Hazel-style)
     * This stores bone IDs and weights separately from vertex data
     */
    struct BoneInfluence
    {
        u32 m_BoneIDs[4] = { 0, 0, 0, 0 };     // Up to 4 bone IDs affecting this vertex
        f32 m_Weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // Corresponding weights (should sum to 1.0)
        
        BoneInfluence() = default;
        BoneInfluence(const glm::ivec4& boneIds, const glm::vec4& weights)
        {
            for (int i = 0; i < 4; ++i)
            {
                m_BoneIDs[i] = boneIds[i];
                m_Weights[i] = weights[i];
            }
        }
        
        // Helper methods for easier access
        void SetBoneData(u32 index, u32 boneId, f32 weight)
        {
            if (index >= 4)
            {
                OLO_CORE_ERROR("SetBoneData: index out of bounds (index: {0}, max: 3)", index);
                return;
            }
            
            m_BoneIDs[index] = boneId;
            m_Weights[index] = weight;
        }
        
        void Normalize()
        {
            f32 totalWeight = m_Weights[0] + m_Weights[1] + m_Weights[2] + m_Weights[3];
            if (totalWeight > 0.0f)
            {
                for (int i = 0; i < 4; ++i)
                    m_Weights[i] /= totalWeight;
            }
        }
    };

    /**
     * @brief Submesh information within a MeshSource
     */
    struct Submesh
    {
    u32 m_BaseVertex = 0;
    u32 m_BaseIndex = 0;
    u32 m_MaterialIndex = 0;
    u32 m_IndexCount = 0;
    u32 m_VertexCount = 0;
        
    glm::mat4 m_Transform = glm::mat4(1.0f);
    BoundingBox m_BoundingBox;
    std::string m_NodeName;
        
    // Rigging information
    bool m_IsRigged = false;
    std::vector<u32> m_BoneIndices;  // Indices of bones that affect this submesh
        
        Submesh() = default;
    };

    /**
     * @brief Unified mesh source that can handle both static and animated meshes
     * 
     * This class replaces the previous separate Mesh/animated mesh distinction with a unified
     * approach similar to Hazel's MeshSource. It can contain skeleton data for
     * animated meshes but also works perfectly for static meshes.
     */
    class MeshSource : public Asset
    {
    public:
        MeshSource() = default;
        MeshSource(const std::vector<Vertex>& vertices, const std::vector<u32>& indices);
        MeshSource(std::vector<Vertex>&& vertices, std::vector<u32>&& indices);
        virtual ~MeshSource() = default;

        // Core mesh data accessors
        const std::vector<Vertex>& GetVertices() const { return m_Vertices; }
        const std::vector<u32>& GetIndices() const { return m_Indices; }
        const std::vector<Submesh>& GetSubmeshes() const { return m_Submeshes; }
        
        std::vector<Vertex>& GetVertices() { return m_Vertices; }
        std::vector<u32>& GetIndices() { return m_Indices; }
        std::vector<Submesh>& GetSubmeshes() { return m_Submeshes; }

        // Skeleton and rigging
        bool HasSkeleton() const { return m_Skeleton != nullptr; }
        const Skeleton* GetSkeleton() const { return m_Skeleton.get(); }
        void SetSkeleton(Ref<Skeleton> skeleton) { m_Skeleton = skeleton; }
        
        bool IsSubmeshRigged(u32 submeshIndex) const 
        { 
            if (submeshIndex >= m_Submeshes.size())
            {
                OLO_CORE_ERROR("IsSubmeshRigged: submesh index {} out of range (size: {})", submeshIndex, m_Submeshes.size());
                throw std::out_of_range("Submesh index out of range");
            }
            return m_Submeshes[submeshIndex].m_IsRigged; 
        }

        // Bone information for skinning
        const std::vector<BoneInfo>& GetBoneInfo() const { return m_BoneInfo; }
        std::vector<BoneInfo>& GetBoneInfo() { return m_BoneInfo; }
        
        const BoneInfo& GetBoneInfo(u32 index) const 
        { 
            if (index >= m_BoneInfo.size())
            {
                OLO_CORE_ERROR("Bone info index {} out of range (size: {})", index, m_BoneInfo.size());
                throw std::out_of_range("Bone info index out of range");
            }
            return m_BoneInfo[index]; 
        }

        // Bone influences for vertices (Hazel-style: one per vertex, separate from vertex data)
        const std::vector<BoneInfluence>& GetBoneInfluences() const { return m_BoneInfluences; }
        std::vector<BoneInfluence>& GetBoneInfluences() { return m_BoneInfluences; }
        
        // Add bone influence for a specific vertex
        void SetVertexBoneData(u32 vertexIndex, const BoneInfluence& influence)
        {
            if (vertexIndex >= m_BoneInfluences.size())
            {
                OLO_CORE_ERROR("SetVertexBoneData: vertex index out of bounds (index: {0}, size: {1})", vertexIndex, m_BoneInfluences.size());
                throw std::out_of_range("Vertex index out of range");
            }
            m_BoneInfluences[vertexIndex] = influence;
        }
        
        // Get bone influence for a specific vertex
        const BoneInfluence& GetVertexBoneData(u32 vertexIndex) const
        {
            if (vertexIndex >= m_BoneInfluences.size())
            {
                OLO_CORE_ERROR("GetVertexBoneData: vertex index out of bounds (index: {0}, size: {1})", vertexIndex, m_BoneInfluences.size());
                throw std::out_of_range("Vertex index out of range");
            }
            return m_BoneInfluences[vertexIndex];
        }

        // Check if this mesh has bone influences for animation
        bool HasBoneInfluences() const { return !m_BoneInfluences.empty(); }

        // Utility methods
        void AddSubmesh(const Submesh& submesh) { m_Submeshes.push_back(submesh); }
        void CalculateBounds();
        void CalculateSubmeshBounds(); // Calculate individual submesh bounds
        void Build(); // Build GPU resources
        
        // GPU resource accessors (with lazy initialization)
        const Ref<VertexArray>& GetVertexArray() const 
        { 
            OLO_CORE_ASSERT(m_VertexArray, "VertexArray not initialized. Call Build() first.");
            return m_VertexArray; 
        }
        const Ref<VertexBuffer>& GetVertexBuffer() const 
        { 
            OLO_CORE_ASSERT(m_VertexBuffer, "VertexBuffer not initialized. Call Build() first.");
            return m_VertexBuffer; 
        }
        const Ref<IndexBuffer>& GetIndexBuffer() const 
        { 
            OLO_CORE_ASSERT(m_IndexBuffer, "IndexBuffer not initialized. Call Build() first.");
            return m_IndexBuffer; 
        }
        
        // Bone influence buffer for rigged meshes (Hazel-style)
        const Ref<VertexBuffer>& GetBoneInfluenceBuffer() const 
        { 
            OLO_CORE_ASSERT(m_BoneInfluenceBuffer, "BoneInfluenceBuffer not initialized or not rigged. Call Build() first.");
            return m_BoneInfluenceBuffer; 
        }
        bool HasBoneInfluenceBuffer() const { return m_BoneInfluenceBuffer != nullptr; }        // Bounding volume accessors
        const BoundingBox& GetBoundingBox() const { return m_BoundingBox; }
        const BoundingSphere& GetBoundingSphere() const { return m_BoundingSphere; }
        
        // Asset interface
        static AssetType GetStaticType() { return AssetType::MeshSource; }
        AssetType GetAssetType() const override { return GetStaticType(); }

    private:
        void BuildVertexBuffer();
        void BuildIndexBuffer();
        void BuildBoneInfluenceBuffer(); // New: build separate bone influence buffer

    private:
        // Core mesh data
        std::vector<Vertex> m_Vertices;
        std::vector<u32> m_Indices;
        std::vector<Submesh> m_Submeshes;
        
        // Rigging data (Hazel-style: separated from vertex data)
        Ref<Skeleton> m_Skeleton;
        std::vector<BoneInfo> m_BoneInfo;
        std::vector<BoneInfluence> m_BoneInfluences; // One per vertex, separate from vertex data
        
        // GPU resources (similar to current Mesh class)
        Ref<VertexArray> m_VertexArray;
        Ref<VertexBuffer> m_VertexBuffer;
        Ref<IndexBuffer> m_IndexBuffer;
        Ref<VertexBuffer> m_BoneInfluenceBuffer; // New: separate buffer for bone influences        // Bounding volumes
    BoundingBox m_BoundingBox;
    BoundingSphere m_BoundingSphere;
        
    bool m_Built = false;
    };
}
