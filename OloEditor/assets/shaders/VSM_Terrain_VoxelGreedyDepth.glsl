// VSM family raster (#1524); geometry is shared with the fixed-map route.
#type vertex
#version 460 core
#define OLO_VSM_FAMILY 1
#include "include/TerrainVoxelGreedyDepthVertexStage.glsl"

#type fragment
#version 460 core
#define OLO_VSM_FAMILY 1
#include "include/TerrainVoxelGreedyDepthFragmentStage.glsl"
