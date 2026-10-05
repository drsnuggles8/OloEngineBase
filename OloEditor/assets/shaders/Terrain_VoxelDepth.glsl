// =============================================================================
// Terrain_VoxelDepth.glsl - Voxel Override Depth-Only Shader for Shadow Maps
// Part of OloEngine Terrain System
// Simple VS → FS pipeline (no tessellation)
// =============================================================================

#type vertex
#version 460 core
#include "include/TerrainVoxelDepthVertexStage.glsl"

#type fragment
#version 460 core
#include "include/TerrainVoxelDepthFragmentStage.glsl"
