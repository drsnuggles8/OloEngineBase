#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Asset/Asset.h"

#include <vector>
#include <memory>
#include <stdexcept>

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
        glm::mat4 InverseBindPose;  // Inverse bind pose matrix for skinning
        u32 BoneIndex;              // Index into the skeleton
        
        BoneInfo() = default;
        BoneInfo(const glm::mat4& inverseBindPose, u32 boneIndex)
            : InverseBindPose(inverseBindPose), BoneIndex(boneIndex) {}
    };

    /**
     * @brief Bone influence structure for vertex skinning data
     */
    struct BoneInfluence
    {
        u32 VertexIndex;
        f32 Weight;
        
        BoneInfluence() = default;
        BoneInfluence(u32 vertexIndex, f32 weight) 
            : VertexIndex(vertexIndex), Weight(weight) {}
    };

    /**
     * @brief Submesh information within a MeshSource
     */
    struct Submesh
    {
        u32 BaseVertex = 0;
        u32 BaseIndex = 0;
        u32 MaterialIndex = 0;
        u32 IndexCount = 0;
        u32 VertexCount = 0;
        
        glm::mat4 Transform = glm::mat4(1.0f);
        BoundingBox BoundingBox;
        std::string NodeName;
        
        // Rigging information
        bool IsRigged = false;
        std::vector<u32> BoneIndices;  // Indices of bones that affect this submesh
        
        Submesh() = default;
    };

    /**
     * @brief Unified mesh source that can handle both static and rigged meshes
     * 
     * This class replaces the separate Mesh/SkinnedMesh distinction with a unified
     * approach similar to Hazel's MeshSource. It can contain skeleton data for
     * rigged meshes but also works perfectly for static meshes.
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
            return submeshIndex < m_Submeshes.size() && m_Submeshes[submeshIndex].IsRigged; 
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

        // Bone influences for vertices
        const std::vector<std::vector<BoneInfluence>>& GetBoneInfluences() const { return m_BoneInfluences; }
        std::vector<std::vector<BoneInfluence>>& GetBoneInfluences() { return m_BoneInfluences; }

        // Utility methods
        void AddSubmesh(const Submesh& submesh) { m_Submeshes.push_back(submesh); }
        void CalculateBounds();
        void CalculateSubmeshBounds(); // Calculate individual submesh bounds
        void Build(); // Build GPU resources
        
        // GPU resource accessors (with lazy initialization)
        const Ref<VertexArray>& GetVertexArray() const 
        { 
            if (!m_Built) 
            {
                // Lazy initialization - build GPU resources on first access
                const_cast<MeshSource*>(this)->Build();
            }
            return m_VertexArray; 
        }
        const Ref<VertexBuffer>& GetVertexBuffer() const 
        { 
            if (!m_Built) 
            {
                const_cast<MeshSource*>(this)->Build();
            }
            return m_VertexBuffer; 
        }
        const Ref<IndexBuffer>& GetIndexBuffer() const 
        { 
            if (!m_Built) 
            {
                const_cast<MeshSource*>(this)->Build();
            }
            return m_IndexBuffer; 
        }
        
        // Bounding volume accessors
        const BoundingBox& GetBoundingBox() const { return m_BoundingBox; }
        const BoundingSphere& GetBoundingSphere() const { return m_BoundingSphere; }
        
        // Asset interface
        static AssetType GetStaticType() { return AssetType::MeshSource; }
        AssetType GetAssetType() const override { return GetStaticType(); }

    private:
        void BuildVertexBuffer();
        void BuildIndexBuffer();

    private:
        // Core mesh data
        std::vector<Vertex> m_Vertices;
        std::vector<u32> m_Indices;
        std::vector<Submesh> m_Submeshes;
        
        // Rigging data
        Ref<Skeleton> m_Skeleton;
        std::vector<BoneInfo> m_BoneInfo;
        std::vector<std::vector<BoneInfluence>> m_BoneInfluences; // Per-bone vertex influences
        
        // GPU resources (similar to current Mesh class)
        Ref<VertexArray> m_VertexArray;
        Ref<VertexBuffer> m_VertexBuffer;
        Ref<IndexBuffer> m_IndexBuffer;
        
        // Bounding volumes
        BoundingBox m_BoundingBox;
        BoundingSphere m_BoundingSphere;
        
        bool m_Built = false;
    };
}
