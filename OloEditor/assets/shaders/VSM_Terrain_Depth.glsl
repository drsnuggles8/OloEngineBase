// VSM family raster (#1524); geometry is shared with the fixed-map route.
#type vertex
#version 460 core
#define OLO_VSM_FAMILY 1
#include "include/TerrainDepthVertexStage.glsl"

#type tess_control
#version 460 core
#define OLO_VSM_FAMILY 1
#include "include/TerrainDepthTess_ControlStage.glsl"

#type tess_evaluation
#version 460 core
#define OLO_VSM_FAMILY 1
#include "include/TerrainDepthTess_EvaluationStage.glsl"

#type fragment
#version 460 core
#define OLO_VSM_FAMILY 1
#include "include/TerrainDepthFragmentStage.glsl"
