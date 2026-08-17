#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <glm/glm.hpp>
#include <array>
#include <span>
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

        // Build from an already-computed height pyramid — what Build() actually
        // does once it has one. Exposed because the tree never needed the asset
        // for anything else, and populating a TerrainData creates a GPU texture:
        // without this, a CPU-only caller pinning the selection math would need
        // a live GL context to do arithmetic. `pyramid` must have
        // BuildHeightPyramid()'s layout for the same depth.
        void BuildFromPyramid(std::vector<glm::vec2> pyramid,
                              f32 worldSizeX, f32 worldSizeZ, u32 maxDepth);

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

        // World-space height extremes (x = min, y = max) for every node of a
        // full quadtree of `maxDepth`, level-major: level L occupies 4^L
        // consecutive entries starting at (4^L - 1) / 3, addressed as
        // y * 2^L + x. The finest level samples the heightmap; coarser levels
        // take the min/max of their four children, which is exact rather than
        // merely conservative because the children's inclusive texel ranges tile
        // the parent's.
        //
        // This is the SAME data the CPU node bounds are built from and the same
        // buffer the GPU descent uploads (issue #714), which is what lets
        // TerrainGPUQuadtreeTest assert an identical selected-node set instead
        // of an approximate one. Callers that want a DEEPER pyramid than the CPU
        // tree (the GPU descent is not bound by TerrainLODConfig::MAX_LOD_LEVELS)
        // call this directly with their own depth.
        [[nodiscard]] static std::vector<glm::vec2> BuildHeightPyramid(const TerrainData& terrainData,
                                                                       f32 heightScale, u32 maxDepth);

        // Raw-heightfield overload — the actual implementation; the TerrainData
        // one forwards to it. Separate because populating a TerrainData creates a
        // GPU heightmap texture, so a CPU-only caller (the L1 tests that pin this
        // math) would otherwise need a live GL context to exercise pure
        // arithmetic. `heights` is row-major, resolution x resolution.
        [[nodiscard]] static std::vector<glm::vec2> BuildHeightPyramid(std::span<const f32> heights,
                                                                       u32 resolution, f32 heightScale,
                                                                       u32 maxDepth);

        // The pyramid this tree was built from, at GetMaxDepth(). Empty until
        // Build() runs.
        [[nodiscard]] const std::vector<glm::vec2>& GetNodeHeightPyramid() const
        {
            return m_NodeHeightPyramid;
        }

      private:
        // Recursively build quadtree nodes. Takes no TerrainData: since #714 the
        // node's height extremes come from m_NodeHeightPyramid, which Build()
        // computes once for the whole tree.
        i32 BuildNode(f32 worldSizeX, f32 worldSizeZ,
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

        std::vector<glm::vec2> m_NodeHeightPyramid;
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
