#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainQuadtree.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Renderer/Frustum.h"

#include <glm/gtc/matrix_access.hpp>
#include <algorithm>
#include <cmath>
#include <span>

namespace OloEngine
{
    namespace
    {
        // Level-major index of node (level, nx, ny) — twin of
        // oloTerrainNodeIndex() in include/TerrainQuadtreeCommon.glsl.
        [[nodiscard]] sizet PyramidIndex(u32 level, u32 nx, u32 ny)
        {
            const sizet levelOffset = ((static_cast<sizet>(1) << (2 * level)) - 1) / 3;
            return levelOffset + (static_cast<sizet>(ny) << level) + nx;
        }
    } // namespace

    std::vector<glm::vec2> TerrainQuadtree::BuildHeightPyramid(const TerrainData& terrainData,
                                                               f32 heightScale, u32 maxDepth)
    {
        return BuildHeightPyramid(terrainData.GetHeightData(), terrainData.GetResolution(),
                                  heightScale, maxDepth);
    }

    std::vector<glm::vec2> TerrainQuadtree::BuildHeightPyramid(std::span<const f32> heights,
                                                               u32 resolution, f32 heightScale,
                                                               u32 maxDepth)
    {
        OLO_PROFILE_FUNCTION();

        if (resolution == 0 || heights.size() < static_cast<sizet>(resolution) * resolution)
        {
            return {};
        }

        sizet total = 0;
        for (u32 d = 0; d <= maxDepth; ++d)
        {
            total += static_cast<sizet>(1) << (2 * d);
        }
        std::vector<glm::vec2> pyramid(total, glm::vec2(0.0f));

        // Finest level: sample the heightmap directly. The inclusive texel range
        // is derived exactly as the pre-#714 BuildNode did, so adjacent nodes
        // share their boundary texel and the bounds stay conservative at seams.
        const u32 finestSpan = 1u << maxDepth;
        const f32 texelMax = static_cast<f32>(resolution - 1);
        for (u32 ny = 0; ny < finestSpan; ++ny)
        {
            const f32 minZ = static_cast<f32>(ny) / static_cast<f32>(finestSpan);
            const f32 maxZ = static_cast<f32>(ny + 1) / static_cast<f32>(finestSpan);
            const u32 sampleMinZ = static_cast<u32>(minZ * texelMax);
            const u32 sampleMaxZ = std::min(static_cast<u32>(maxZ * texelMax), resolution - 1);

            for (u32 nx = 0; nx < finestSpan; ++nx)
            {
                const f32 minX = static_cast<f32>(nx) / static_cast<f32>(finestSpan);
                const f32 maxX = static_cast<f32>(nx + 1) / static_cast<f32>(finestSpan);
                const u32 sampleMinX = static_cast<u32>(minX * texelMax);
                const u32 sampleMaxX = std::min(static_cast<u32>(maxX * texelMax), resolution - 1);

                f32 hMin = std::numeric_limits<f32>::max();
                f32 hMax = std::numeric_limits<f32>::lowest();
                for (u32 z = sampleMinZ; z <= sampleMaxZ; ++z)
                {
                    const sizet row = static_cast<sizet>(z) * resolution;
                    for (u32 x = sampleMinX; x <= sampleMaxX; ++x)
                    {
                        const f32 h = heights[row + x] * heightScale;
                        hMin = std::min(hMin, h);
                        hMax = std::max(hMax, h);
                    }
                }
                // A perfectly flat node would produce a zero-thickness AABB,
                // which the positive-vertex plane test treats as a plane and
                // can reject at a grazing angle. Same epsilon the pre-#714
                // BuildNode applied, for the same reason.
                if (hMin >= hMax)
                {
                    hMin -= 0.01f;
                    hMax += 0.01f;
                }
                pyramid[PyramidIndex(maxDepth, nx, ny)] = glm::vec2(hMin, hMax);
            }
        }

        // Coarser levels: reduce over the four children. Exact, not merely
        // conservative — the children's inclusive texel ranges tile the parent's.
        for (u32 level = maxDepth; level-- > 0;)
        {
            const u32 span = 1u << level;
            for (u32 ny = 0; ny < span; ++ny)
            {
                for (u32 nx = 0; nx < span; ++nx)
                {
                    f32 hMin = std::numeric_limits<f32>::max();
                    f32 hMax = std::numeric_limits<f32>::lowest();
                    for (u32 cy = 0; cy < 2; ++cy)
                    {
                        for (u32 cx = 0; cx < 2; ++cx)
                        {
                            const glm::vec2 child = pyramid[PyramidIndex(level + 1, nx * 2 + cx, ny * 2 + cy)];
                            hMin = std::min(hMin, child.x);
                            hMax = std::max(hMax, child.y);
                        }
                    }
                    pyramid[PyramidIndex(level, nx, ny)] = glm::vec2(hMin, hMax);
                }
            }
        }

        return pyramid;
    }

    void TerrainQuadtree::Build(const TerrainData& terrainData,
                                f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                                u32 maxDepth)
    {
        OLO_PROFILE_FUNCTION();

        m_HeightScale = heightScale;

        if (u32 resolution = terrainData.GetResolution(); resolution == 0 || terrainData.GetHeightData().size() < static_cast<sizet>(resolution) * resolution)
        {
            OLO_CORE_ERROR("TerrainQuadtree::Build: Invalid terrain data (resolution={}, heights={})",
                           resolution, terrainData.GetHeightData().size());
            m_Nodes.clear();
            m_SelectedNodes.clear();
            m_NodeHeightPyramid.clear();
            m_RootIndex = -1;
            return;
        }

        // Node bounds come from the pyramid rather than a per-node strided
        // resample (issue #714). The old sampler stepped every (extent / 16)
        // texels plus the four corners, so a spike between samples was missed
        // and the AABB could clip its own terrain; this is exact, costs one
        // pass over the heightmap for the whole tree, and — because the GPU
        // descent uploads the same array — is what makes the two paths select
        // the same nodes.
        const u32 clampedDepth = std::min(maxDepth, TerrainLODConfig::MAX_LOD_LEVELS);
        BuildFromPyramid(BuildHeightPyramid(terrainData, heightScale, clampedDepth),
                         worldSizeX, worldSizeZ, maxDepth);
    }

    void TerrainQuadtree::BuildFromPyramid(std::vector<glm::vec2> pyramid,
                                           f32 worldSizeX, f32 worldSizeZ, u32 maxDepth)
    {
        OLO_PROFILE_FUNCTION();

        m_WorldSizeX = worldSizeX;
        m_WorldSizeZ = worldSizeZ;
        m_MaxDepth = std::min(maxDepth, TerrainLODConfig::MAX_LOD_LEVELS);

        m_Nodes.clear();
        m_SelectedNodes.clear();
        m_NodeHeightPyramid = std::move(pyramid);

        sizet expectedNodes = 0;
        for (u32 d = 0; d <= m_MaxDepth; ++d)
        {
            expectedNodes += static_cast<sizet>(1) << (2 * d); // 4^d
        }
        if (m_NodeHeightPyramid.size() != expectedNodes)
        {
            OLO_CORE_ERROR("TerrainQuadtree::BuildFromPyramid: pyramid has {} entries, expected {} for depth {}",
                           m_NodeHeightPyramid.size(), expectedNodes, m_MaxDepth);
            m_NodeHeightPyramid.clear();
            m_RootIndex = -1;
            return;
        }

        m_Nodes.reserve(std::min(expectedNodes, static_cast<sizet>(100000)));
        m_RootIndex = BuildNode(worldSizeX, worldSizeZ, 0.0f, 0.0f, 1.0f, 1.0f, 0);

        OLO_CORE_INFO("TerrainQuadtree: Built {} nodes, max depth {}", m_Nodes.size(), m_MaxDepth);
    }

    i32 TerrainQuadtree::BuildNode(f32 worldSizeX, f32 worldSizeZ,
                                   f32 minX, f32 minZ, f32 maxX, f32 maxZ,
                                   u32 depth)
    {
        auto nodeIndex = static_cast<i32>(m_Nodes.size());
        m_Nodes.emplace_back();

        // Set node properties — use index-based access since recursive BuildNode
        // calls below may reallocate m_Nodes, invalidating any references.
        m_Nodes[static_cast<sizet>(nodeIndex)].MinX = minX;
        m_Nodes[static_cast<sizet>(nodeIndex)].MinZ = minZ;
        m_Nodes[static_cast<sizet>(nodeIndex)].MaxX = maxX;
        m_Nodes[static_cast<sizet>(nodeIndex)].MaxZ = maxZ;
        m_Nodes[static_cast<sizet>(nodeIndex)].Depth = depth;
        m_Nodes[static_cast<sizet>(nodeIndex)].IsLeaf = true;

        // World-space bounding box: XZ from the node's share of the footprint,
        // Y from the shared height pyramid.
        f32 worldMinX = minX * worldSizeX;
        f32 worldMinZ = minZ * worldSizeZ;
        f32 worldMaxX = maxX * worldSizeX;
        f32 worldMaxZ = maxZ * worldSizeZ;

        // Height extremes come from the shared pyramid (issue #714), so the CPU
        // node bounds and the AABB the GPU descent tests are the same numbers.
        const u32 span = 1u << depth;
        const auto nx = static_cast<u32>(std::lround(minX * static_cast<f32>(span)));
        const auto nz = static_cast<u32>(std::lround(minZ * static_cast<f32>(span)));
        const glm::vec2 heightRange = m_NodeHeightPyramid[PyramidIndex(depth, std::min(nx, span - 1), std::min(nz, span - 1))];

        m_Nodes[static_cast<sizet>(nodeIndex)].Bounds = BoundingBox(
            glm::vec3(worldMinX, heightRange.x, worldMinZ),
            glm::vec3(worldMaxX, heightRange.y, worldMaxZ));

        // Recursively subdivide if not at max depth
        if (depth < m_MaxDepth)
        {
            m_Nodes[static_cast<sizet>(nodeIndex)].IsLeaf = false;
            f32 midX = (minX + maxX) * 0.5f;
            f32 midZ = (minZ + maxZ) * 0.5f;

            // Children: [0]=SW, [1]=SE, [2]=NW, [3]=NE
            // Each BuildNode call may reallocate m_Nodes, so re-index after each call.
            i32 child0 = BuildNode(worldSizeX, worldSizeZ, minX, minZ, midX, midZ, depth + 1);
            m_Nodes[static_cast<sizet>(nodeIndex)].Children[0] = child0;

            i32 child1 = BuildNode(worldSizeX, worldSizeZ, midX, minZ, maxX, midZ, depth + 1);
            m_Nodes[static_cast<sizet>(nodeIndex)].Children[1] = child1;

            i32 child2 = BuildNode(worldSizeX, worldSizeZ, minX, midZ, midX, maxZ, depth + 1);
            m_Nodes[static_cast<sizet>(nodeIndex)].Children[2] = child2;

            i32 child3 = BuildNode(worldSizeX, worldSizeZ, midX, midZ, maxX, maxZ, depth + 1);
            m_Nodes[static_cast<sizet>(nodeIndex)].Children[3] = child3;
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
        // If error is below threshold, this node is fine — render at this LOD
        if (f32 screenError = CalculateScreenSpaceError(node, cameraPos, viewProjection, viewportHeight); screenError < m_Config.TargetTriangleSize)
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
        auto edgeTess = [this, &baseTess](u32 neighborLOD) -> f32
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
