#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <glm/glm.hpp>
#include <array>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    class TerrainData;
    class Frustum;

    // LOD level for a quadtree node — determines tessellation factor
    // Level 0 = highest detail (closest), MAX_LOD_LEVELS-1 = coarsest
    struct TerrainLODConfig
    {
        static constexpr u32 MAX_LOD_LEVELS = 5;
        // Screen-space error thresholds per LOD level (pixels)
        // Node splits when its error exceeds the threshold for current LOD
        std::array<f32, MAX_LOD_LEVELS> ErrorThresholds = { 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
        // Tessellation factor per LOD level
        std::array<f32, MAX_LOD_LEVELS> TessFactors = { 64.0f, 32.0f, 16.0f, 8.0f, 4.0f };
        // Morph region as fraction of LOD transition distance [0,1]
        f32 MorphRegion = 0.3f;
        // Target screen-space triangle size in pixels
        f32 TargetTriangleSize = 8.0f;
    };

    // Represents a single quadtree node — covers a rectangular region of terrain
    struct TerrainQuadNode
    {
        // Terrain-space region (normalized [0,1])
        f32 MinX = 0.0f;
        f32 MinZ = 0.0f;
        f32 MaxX = 1.0f;
        f32 MaxZ = 1.0f;

        // Bounding box in world space (includes height extremes)
        BoundingBox Bounds;

        // LOD level assigned during selection (0 = finest, higher = coarser)
        u32 LODLevel = 0;

        // Morph factor for LOD transition blending [0,1]
        f32 MorphFactor = 0.0f;

        // Neighbor LOD levels for crack-free edge tessellation
        // Order: +X, -X, +Z, -Z
        std::array<u32, 4> NeighborLODs = { 0, 0, 0, 0 };

        // Index into associated chunk mesh (or -1 if no mesh)
        i32 ChunkIndex = -1;

        // Tree structure
        std::array<i32, 4> Children = { -1, -1, -1, -1 }; // Indices into node pool
        bool IsLeaf = true;
        u32 Depth = 0; // Tree depth (0 = root)
    };

    // Per-chunk LOD data uploaded to GPU for tessellation control
    struct TerrainChunkLODData
    {
        glm::vec4 TessFactors;  // x=inner, y=+X edge, z=-X edge, w=+Z edge
        glm::vec4 TessFactors2; // x=-Z edge, y=morphFactor, z=LODLevel, w=unused
    };

    // Quadtree-based terrain LOD system. Provides frustum culling,
    // screen-space error LOD selection, and neighbor info for stitching.
    class TerrainQuadtree
    {
      public:
        TerrainQuadtree() = default;

        // Build the full quadtree from terrain data
        void Build(const TerrainData& terrainData,
                   f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                   u32 maxDepth = TerrainLODConfig::MAX_LOD_LEVELS);

        // Select visible leaf nodes at appropriate LOD levels for rendering
        // cameraPos: world-space camera position
        // viewProjection: combined VP matrix for screen-space error
        // viewportHeight: viewport pixel height for error calculation
        void SelectLOD(const Frustum& frustum,
                       const glm::vec3& cameraPos,
                       const glm::mat4& viewProjection,
                       f32 viewportHeight);

        // Get selected (visible, LOD-assigned) nodes for rendering
        [[nodiscard]] const std::vector<const TerrainQuadNode*>& GetSelectedNodes() const
        {
            return m_SelectedNodes;
        }

        // Get LOD data for a selected node (for GPU tessellation upload)
        [[nodiscard]] TerrainChunkLODData GetChunkLODData(const TerrainQuadNode& node) const;

        [[nodiscard]] const TerrainLODConfig& GetConfig() const
        {
            return m_Config;
        }
        TerrainLODConfig& GetConfig()
        {
            return m_Config;
        }

        [[nodiscard]] u32 GetMaxDepth() const
        {
            return m_MaxDepth;
        }
        [[nodiscard]] u32 GetNodeCount() const
        {
            return static_cast<u32>(m_Nodes.size());
        }

      private:
        // Recursively build quadtree nodes
        i32 BuildNode(const TerrainData& terrainData,
                      f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                      f32 minX, f32 minZ, f32 maxX, f32 maxZ,
                      u32 depth);

        // Recursively select LOD nodes
        void SelectNode(i32 nodeIndex, const Frustum& frustum,
                        const glm::vec3& cameraPos,
                        const glm::mat4& viewProjection,
                        f32 viewportHeight);

        // Calculate screen-space geometric error for a node
        f32 CalculateScreenSpaceError(const TerrainQuadNode& node,
                                      const glm::vec3& cameraPos,
                                      const glm::mat4& viewProjection,
                                      f32 viewportHeight) const;

        // Find and assign neighbor LOD levels for selected leaf nodes
        void ResolveNeighborLODs();

        // Find the selected leaf node that contains a given terrain-space point
        const TerrainQuadNode* FindLeafAt(f32 normX, f32 normZ) const;

        std::vector<TerrainQuadNode> m_Nodes;
        std::vector<const TerrainQuadNode*> m_SelectedNodes;
        std::unordered_set<const TerrainQuadNode*> m_SelectedNodeSet; // O(1) lookup for FindLeafAt
        i32 m_RootIndex = -1;
        u32 m_MaxDepth = 6;
        TerrainLODConfig m_Config;
        f32 m_WorldSizeX = 0.0f;
        f32 m_WorldSizeZ = 0.0f;
        f32 m_HeightScale = 0.0f;
    };
} // namespace OloEngine
