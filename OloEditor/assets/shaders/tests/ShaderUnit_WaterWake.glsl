#version 460 core

// =============================================================================
// ShaderUnit_WaterWake.glsl — the GPU half of the #968 CPU/GPU wake parity test.
//
// Layer-2 of the renderer pyramid: a production shader function driven on the
// real GPU by an SSBO of inputs, read back and compared against its C++ mirror.
//
// This probe #includes include/WaterWakeCommon.glsl rather than copying it.
// That is the whole point and it is a deliberate departure from the older
// probes here (ShaderUnit_Fog.glsl duplicates FogCommon.glsl verbatim and says
// so): a copy tests the copy. WaterWakeParityTest exists to prove that the text
// the WATER SHADER runs agrees with WaterWake::Evaluate, and it can only prove
// that about text it actually shares with it.
//
// The evaluator asks for its hull records through the `waterWakeFetch` hook, so
// a consumer supplies them from wherever it keeps them — the WaterParams UBO in
// the water stages, this SSBO here.
//
// Inputs  (SSBO binding 0) : vec4[1 + WATER_WAKE_HULL_VEC4_COUNT]
//     [0]   = (hullCount, heightScale, flattenStrength, unused)
//     [1..] = the packed hull records, WaterWake.h's layout verbatim
// Inputs  (SSBO binding 1) : vec4[N] queries — xy = world XZ, z = vertexSpacing
// Outputs (SSBO binding 2) : vec4[N] — x = height (m), y = flatten, zw = 0
// =============================================================================

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer Hulls { vec4 u_Hulls[]; };
layout(std430, binding = 1) readonly buffer Queries { vec4 u_Queries[]; };
layout(std430, binding = 2) writeonly buffer Outputs { vec4 u_Outputs[]; };

#include "../include/WaterWakeCommon.glsl"

// Record 0 is the header, so the evaluator's index 0 is our index 1.
vec4 waterWakeFetch(int index)
{
    return u_Hulls[index + 1];
}

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= u_Queries.length())
        return;

    vec4 query = u_Queries[idx];
    vec4 header = u_Hulls[0];

    vec2 wake = waterWakeEvaluate(header.x, header.y, header.z, query.xy, query.z);
    u_Outputs[idx] = vec4(wake.x, wake.y, 0.0, 0.0);
}
