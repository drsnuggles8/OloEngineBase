// =============================================================================
// Terrain_VoxelGreedyDepth.glsl - Depth-only shadow pass for the packed-quad
// voxel path (issue #727).
//
// Instanced sibling of Terrain_VoxelDepth.glsl. Must reconstruct the quad
// EXACTLY the way Terrain_VoxelGreedy.glsl does, or the shadow silhouette
// drifts from the lit one — a divergence that renders as peter-panning or a
// shadow of geometry that is not there.
// =============================================================================

#type vertex
#version 460 core
#include "include/TerrainVoxelGreedyDepthVertexStage.glsl"

#type fragment
#version 460 core
#include "include/TerrainVoxelGreedyDepthFragmentStage.glsl"
