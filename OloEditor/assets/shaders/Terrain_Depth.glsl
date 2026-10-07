// =============================================================================
// Terrain_Depth.glsl - Terrain Depth-Only Shader for Shadow Maps
// Part of OloEngine Terrain System — with tessellation
// =============================================================================

#type vertex
#version 460 core
#include "include/TerrainDepthVertexStage.glsl"

#type tess_control
#version 460 core
#include "include/TerrainDepthTess_ControlStage.glsl"

#type tess_evaluation
#version 460 core
#include "include/TerrainDepthTess_EvaluationStage.glsl"

#type fragment
#version 460 core
#include "include/TerrainDepthFragmentStage.glsl"
