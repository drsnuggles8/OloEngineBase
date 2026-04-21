#version 460 core

// =============================================================================
// ShaderUnit_Fog.glsl
//
// Test harness for the analytical distance-fog formulas in FogCommon.glsl.
// The production functions are duplicated verbatim below — changes there must
// be reflected here or the test fails.
//
// Verifies two physical invariants:
//   1. Fog at zero distance contributes exactly zero (no fog at the camera).
//   2. Fog at infinite distance converges to full opacity (1.0) for the two
//      exponential modes. Linear mode saturates at fogEnd.
//
// Inputs  (SSBO binding 0) : N × 5 × float
//     [mode, density, fogStart, fogEnd, dist]
// Outputs (SSBO binding 1) : N × float, the computed fog factor.
// =============================================================================

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly  buffer Inputs  { float u_Inputs[]; };
layout(std430, binding = 1) writeonly buffer Outputs { float u_Outputs[]; };

const int FOG_LINEAR          = 0;
const int FOG_EXPONENTIAL     = 1;
const int FOG_EXPONENTIAL_SQ  = 2;

// Duplicated verbatim from FogCommon.glsl::computeDistanceFog
float computeDistanceFog(float dist, int mode, float density, float fogStart, float fogEnd)
{
    if (mode == FOG_LINEAR)
    {
        return 1.0 - clamp((fogEnd - dist) / max(fogEnd - fogStart, 0.0001), 0.0, 1.0);
    }
    else if (mode == FOG_EXPONENTIAL)
    {
        return 1.0 - exp(-density * dist);
    }
    else // FOG_EXPONENTIAL_SQ
    {
        float dd = density * dist;
        return 1.0 - exp(-dd * dd);
    }
}

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    uint total = u_Inputs.length() / 5u;
    if (idx >= total)
        return;

    uint base = idx * 5u;
    int   mode     = int(u_Inputs[base + 0u] + 0.5);
    float density  = u_Inputs[base + 1u];
    float fogStart = u_Inputs[base + 2u];
    float fogEnd   = u_Inputs[base + 3u];
    float dist     = u_Inputs[base + 4u];

    u_Outputs[idx] = computeDistanceFog(dist, mode, density, fogStart, fogEnd);
}
