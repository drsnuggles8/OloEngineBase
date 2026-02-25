#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainQuadtree.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Renderer/Frustum.h"

#include <glm/gtc/matrix_access.hpp>
#include <algorithm>
#include <cmath>

namespace OloEngine
{
    void TerrainQuadtree::Build(const TerrainData& terrainData,
                                f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                                u32 maxDepth)
    {
        OLO_PROFILE_FUNCTION();

        m_WorldSizeX = worldSizeX;
        m_WorldSizeZ = worldSizeZ;
        m_HeightScale = heightScale;
        m_MaxDepth = std::min(maxDepth, TerrainLODConfig::MAX_LOD_LEVELS);

        m_Nodes.clear();
        m_SelectedNodes.clear();

        // Pre-allocate — a full quadtree of depth D has sum(4^i, i=0..D) nodes
        // But we won't necessarily fill all levels
        sizet estimatedNodes = 0;
        for (u32 d = 0; d <= m_MaxDepth; ++d)
        {
            estimatedNodes += static_cast<sizet>(1) << (2 * d); // 4^d
        }
        m_Nodes.reserve(std::min(estimatedNodes, static_cast<sizet>(100000)));

        m_RootIndex = BuildNode(terrainData, worldSizeX, worldSizeZ, heightScale,
                                0.0f, 0.0f, 1.0f, 1.0f, 0);

        OLO_CORE_INFO("TerrainQuadtree: Built {} nodes, max depth {}", m_Nodes.size(), maxDepth);
    }

    i32 TerrainQuadtree::BuildNode(const TerrainData& terrainData,
                                   f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                                   f32 minX, f32 minZ, f32 maxX, f32 maxZ,
                                   u32 depth)
    {
        auto nodeIndex = static_cast<i32>(m_Nodes.size());
        m_Nodes.emplace_back();
        auto& node = m_Nodes[static_cast<sizet>(nodeIndex)];

        node.MinX = minX;
        node.MinZ = minZ;
        node.MaxX = maxX;
        node.MaxZ = maxZ;
        node.Depth = depth;
        node.IsLeaf = true;

        // Compute world-space bounding box by sampling heights in the region
        f32 worldMinX = minX * worldSizeX;
        f32 worldMinZ = minZ * worldSizeZ;
        f32 worldMaxX = maxX * worldSizeX;
        f32 worldMaxZ = maxZ * worldSizeZ;

        // Sample heightmap to find height extremes in this region
        f32 hMin = std::numeric_limits<f32>::max();
        f32 hMax = std::numeric_limits<f32>::lowest();

        u32 resolution = terrainData.GetResolution();
        u32 sampleMinX = static_cast<u32>(minX * static_cast<f32>(resolution - 1));
        u32 sampleMinZ = static_cast<u32>(minZ * static_cast<f32>(resolution - 1));
        u32 sampleMaxX = std::min(static_cast<u32>(maxX * static_cast<f32>(resolution - 1)), resolution - 1);
        u32 sampleMaxZ = std::min(static_cast<u32>(maxZ * static_cast<f32>(resolution - 1)), resolution - 1);

        // Step through heightmap samples; at deep levels sample every texel,
        // at shallow levels skip to keep build fast
        u32 step = std::max(1u, (sampleMaxX - sampleMinX) / 16);
        const auto& heights = terrainData.GetHeightData();

        for (u32 z = sampleMinZ; z <= sampleMaxZ; z += step)
        {
            for (u32 x = sampleMinX; x <= sampleMaxX; x += step)
            {
                f32 h = heights[static_cast<sizet>(z) * resolution + x] * heightScale;
                hMin = std::min(hMin, h);
                hMax = std::max(hMax, h);
            }
        }
        // Always include boundary samples
        auto sampleHeight = [&](u32 x, u32 z)
        {
            f32 h = heights[static_cast<sizet>(z) * resolution + x] * heightScale;
            hMin = std::min(hMin, h);
            hMax = std::max(hMax, h);
        };
        sampleHeight(sampleMinX, sampleMinZ);
        sampleHeight(sampleMaxX, sampleMinZ);
        sampleHeight(sampleMinX, sampleMaxZ);
        sampleHeight(sampleMaxX, sampleMaxZ);

        if (hMin >= hMax)
        {
            hMin -= 0.01f;
            hMax += 0.01f;
        }

        node.Bounds = BoundingBox(
            glm::vec3(worldMinX, hMin, worldMinZ),
            glm::vec3(worldMaxX, hMax, worldMaxZ));

        // Recursively subdivide if not at max depth
        if (depth < m_MaxDepth)
        {
            node.IsLeaf = false;
            f32 midX = (minX + maxX) * 0.5f;
            f32 midZ = (minZ + maxZ) * 0.5f;

            // Children: [0]=SW, [1]=SE, [2]=NW, [3]=NE
            node.Children[0] = BuildNode(terrainData, worldSizeX, worldSizeZ, heightScale,
                                         minX, minZ, midX, midZ, depth + 1);
            node.Children[1] = BuildNode(terrainData, worldSizeX, worldSizeZ, heightScale,
                                         midX, minZ, maxX, midZ, depth + 1);
            node.Children[2] = BuildNode(terrainData, worldSizeX, worldSizeZ, heightScale,
                                         minX, midZ, midX, maxZ, depth + 1);
            node.Children[3] = BuildNode(terrainData, worldSizeX, worldSizeZ, heightScale,
                                         midX, midZ, maxX, maxZ, depth + 1);

            // NOTE: m_Nodes may have been reallocated by recursive calls,
            // so do NOT use `node` reference after this point.
        }

        return nodeIndex;
    }

    void TerrainQuadtree::SelectLOD(const Frustum& frustum,
                                    const glm::vec3& cameraPos,
                                    const glm::mat4& viewProjection,
                                    f32 viewportHeight)
    {
        OLO_PROFILE_FUNCTION();

        m_SelectedNodes.clear();
        m_SelectedNodeSet.clear();

        if (m_RootIndex < 0)
        {
            return;
        }

        SelectNode(m_RootIndex, frustum, cameraPos, viewProjection, viewportHeight);

        // Build O(1) lookup set for neighbor resolution
        m_SelectedNodeSet.insert(m_SelectedNodes.begin(), m_SelectedNodes.end());

        // After selecting nodes, resolve neighbor LODs for crack-free stitching
        ResolveNeighborLODs();
    }

    void TerrainQuadtree::SelectNode(i32 nodeIndex, const Frustum& frustum,
                                     const glm::vec3& cameraPos,
                                     const glm::mat4& viewProjection,
                                     f32 viewportHeight)
    {
        auto& node = m_Nodes[static_cast<sizet>(nodeIndex)];

        // Frustum cull
        if (!frustum.IsBoxVisible(node.Bounds.Min, node.Bounds.Max))
        {
            return;
        }

        // If leaf node, always select it
        if (node.IsLeaf)
        {
            node.LODLevel = node.Depth;
            m_SelectedNodes.push_back(&node);
            return;
        }

        // Calculate screen-space error to decide whether to use this node
        // or recurse into children
        f32 screenError = CalculateScreenSpaceError(node, cameraPos, viewProjection, viewportHeight);

        // If error is below threshold, this node is fine — render at this LOD
        if (screenError < m_Config.TargetTriangleSize)
        {
            node.LODLevel = node.Depth;

            // Calculate morph factor based on how close error is to threshold
            f32 morphStart = m_Config.TargetTriangleSize * (1.0f - m_Config.MorphRegion);
            node.MorphFactor = (screenError > morphStart)
                                   ? (screenError - morphStart) / (m_Config.TargetTriangleSize - morphStart)
                                   : 0.0f;
            node.MorphFactor = glm::clamp(node.MorphFactor, 0.0f, 1.0f);

            m_SelectedNodes.push_back(&node);
            return;
        }

        // Error too high — recurse into children for more detail
        for (i32 childIdx : node.Children)
        {
            if (childIdx >= 0)
            {
                SelectNode(childIdx, frustum, cameraPos, viewProjection, viewportHeight);
            }
        }
    }

    f32 TerrainQuadtree::CalculateScreenSpaceError(const TerrainQuadNode& node,
                                                   const glm::vec3& cameraPos,
                                                   const glm::mat4& viewProjection,
                                                   f32 viewportHeight) const
    {
        // Geometric error: proportional to the node's world-space extent
        // A node covering more terrain has more potential detail to miss
        f32 nodeWorldSizeX = (node.MaxX - node.MinX) * m_WorldSizeX;
        f32 nodeWorldSizeZ = (node.MaxZ - node.MinZ) * m_WorldSizeZ;
        f32 geometricError = std::max(nodeWorldSizeX, nodeWorldSizeZ);

        // Distance from camera to node center
        glm::vec3 nodeCenter = (node.Bounds.Min + node.Bounds.Max) * 0.5f;
        f32 distance = glm::length(cameraPos - nodeCenter);
        distance = std::max(distance, 0.001f); // Avoid division by zero

        // Project geometric error to screen space
        // screenError = (geometricError / distance) * (viewportHeight / (2 * tan(fov/2)))
        // We approximate the projection scale from the VP matrix
        f32 projScale = viewProjection[1][1] * viewportHeight * 0.5f;
        f32 screenError = (geometricError * projScale) / distance;

        return screenError;
    }

    void TerrainQuadtree::ResolveNeighborLODs()
    {
        // For each selected leaf, find neighbors in the 4 cardinal directions
        // and record their LOD level for edge tessellation matching
        for (const auto* nodePtr : m_SelectedNodes)
        {
            // We need mutable access to write NeighborLODs
            auto& node = m_Nodes[static_cast<sizet>(nodePtr - m_Nodes.data())];

            f32 cx = (node.MinX + node.MaxX) * 0.5f;
            f32 cz = (node.MinZ + node.MaxZ) * 0.5f;
            f32 halfW = (node.MaxX - node.MinX) * 0.5f;
            f32 halfH = (node.MaxZ - node.MinZ) * 0.5f;
            f32 eps = halfW * 0.1f; // Small offset into neighbor

            // +X neighbor
            const auto* nx = FindLeafAt(node.MaxX + eps, cz);
            node.NeighborLODs[0] = nx ? nx->LODLevel : node.LODLevel;

            // -X neighbor
            const auto* nxNeg = FindLeafAt(node.MinX - eps, cz);
            node.NeighborLODs[1] = nxNeg ? nxNeg->LODLevel : node.LODLevel;

            // +Z neighbor
            const auto* nz = FindLeafAt(cx, node.MaxZ + eps);
            node.NeighborLODs[2] = nz ? nz->LODLevel : node.LODLevel;

            // -Z neighbor
            const auto* nzNeg = FindLeafAt(cx, node.MinZ - eps);
            node.NeighborLODs[3] = nzNeg ? nzNeg->LODLevel : node.LODLevel;
        }
    }

    const TerrainQuadNode* TerrainQuadtree::FindLeafAt(f32 normX, f32 normZ) const
    {
        if (m_RootIndex < 0 || normX < 0.0f || normX > 1.0f || normZ < 0.0f || normZ > 1.0f)
        {
            return nullptr;
        }

        // Walk down the tree from the root
        i32 current = m_RootIndex;
        while (current >= 0)
        {
            const auto& node = m_Nodes[static_cast<sizet>(current)];

            // Check if this node was selected at this LOD level (O(1) set lookup)
            if (m_SelectedNodeSet.contains(&node))
            {
                return &node;
            }

            if (node.IsLeaf)
            {
                return &node;
            }

            // Determine which child contains the point
            f32 midX = (node.MinX + node.MaxX) * 0.5f;
            f32 midZ = (node.MinZ + node.MaxZ) * 0.5f;

            if (normX < midX)
            {
                current = (normZ < midZ) ? node.Children[0] : node.Children[2];
            }
            else
            {
                current = (normZ < midZ) ? node.Children[1] : node.Children[3];
            }
        }

        return nullptr;
    }

    TerrainChunkLODData TerrainQuadtree::GetChunkLODData(const TerrainQuadNode& node) const
    {
        TerrainChunkLODData data{};

        // Base tessellation factor for this LOD level
        u32 lodIdx = std::min(node.LODLevel, TerrainLODConfig::MAX_LOD_LEVELS - 1);
        f32 baseTess = m_Config.TessFactors[lodIdx];

        data.TessFactors.x = baseTess; // Inner tessellation

        // Edge tessellation: use minimum of this node's and neighbor's tess factor
        // to prevent cracks
        auto edgeTess = [&](u32 neighborLOD) -> f32
        {
            u32 nLod = std::min(neighborLOD, TerrainLODConfig::MAX_LOD_LEVELS - 1);
            f32 nTess = m_Config.TessFactors[nLod];
            return std::min(baseTess, nTess);
        };

        data.TessFactors.y = edgeTess(node.NeighborLODs[0]); // +X edge
        data.TessFactors.z = edgeTess(node.NeighborLODs[1]); // -X edge
        data.TessFactors.w = edgeTess(node.NeighborLODs[2]); // +Z edge

        data.TessFactors2.x = edgeTess(node.NeighborLODs[3]); // -Z edge
        data.TessFactors2.y = node.MorphFactor;
        data.TessFactors2.z = static_cast<f32>(node.LODLevel);
        data.TessFactors2.w = 0.0f;

        return data;
    }
} // namespace OloEngine
