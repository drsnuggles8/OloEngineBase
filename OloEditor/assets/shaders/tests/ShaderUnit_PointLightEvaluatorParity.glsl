// =============================================================================
// ShaderUnit_PointLightEvaluatorParity.glsl
//
// Pins issue #1457: the two point-light evaluators every raster path shades
// with must return the same radiance for the same light and surface.
//
//   ROW 0  the light LOOP — oloSkinLightContributionSplit over a LightData,
//          the call PBR_MultiLight.glsl makes on Forward and
//          DeferredLightingShared.glsl makes when the tiles are off.
//   ROW 1  the clustered TILE evaluator — fplusEvaluateTileLightsSplit over the
//          Forward+ SSBOs, which Forward+ and Deferred both use.
//
// ONE COLUMN PER CASE. The CPU (PointLightEvaluatorParityGpuTest.cpp) packs the
// light for BOTH rows from one canonical record through the production
// adapters (GPUSceneLightAdapter::ToMultiLightData / ToForwardPlusPoint), so a
// packing drift between the UBO and the SSBO is visible here, not just a
// shader drift. Each column is its own one-light cluster: countX = columns,
// countY = countZ = 1, so fplusClusterIndex(any depth) at column c is c.
//
// rgb = diffuse + specular, a = 1 so an unwritten texel cannot pass.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

void main()
{
    gl_Position = vec4(a_Position.xy, 0.0, 1.0);
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"
#include "include/ForwardPlusCommon.glsl"

layout(location = 0) out vec4 o_Result;

// The loop's input, one entry per column (UBOStructures::MultiLightData bytes).
layout(std430, binding = 40) readonly buffer ProbeLoopLights { LightData probeLoopLights[]; };

struct ProbeSurface
{
    vec4 PositionAndRoughness; // xyz = shaded point, w = roughness
    vec4 NormalAndMetallic;    // xyz = normal (unnormalised ok), w = metallic
    vec4 CameraAndModel;       // xyz = camera position, w = PBR model
    vec4 Albedo;
};
layout(std430, binding = 41) readonly buffer ProbeSurfaces { ProbeSurface probeSurfaces[]; };

void main()
{
    int column = int(gl_FragCoord.x);
    int row = int(gl_FragCoord.y);
    ProbeSurface s = probeSurfaces[column];

    vec3 worldPos = s.PositionAndRoughness.xyz;
    vec3 N = normalize(s.NormalAndMetallic.xyz);
    vec3 V = normalize(s.CameraAndModel.xyz - worldPos);
    int pbrModel = int(s.CameraAndModel.w + 0.5);

    OloSurfaceLighting lit;
    if (row == 0)
    {
        lit = oloSkinLightContributionSplit(probeLoopLights[column], N, V, s.Albedo.rgb, s.NormalAndMetallic.w,
                                            s.PositionAndRoughness.w, worldPos, pbrModel, vec2(0.0, 1.0));
    }
    else
    {
        // Any positive depth: the probe's grid has one slice.
        lit = fplusEvaluateTileLightsSplit(N, V, worldPos, s.Albedo.rgb, s.NormalAndMetallic.w,
                                           s.PositionAndRoughness.w, 1.0, pbrModel);
    }
    o_Result = vec4(oloSurfaceLightingSum(lit), 1.0);
}
