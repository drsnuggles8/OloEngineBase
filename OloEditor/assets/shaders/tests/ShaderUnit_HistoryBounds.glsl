#version 460 core

// =============================================================================
// ShaderUnit_HistoryBounds.glsl
//
// L2 probe for the history bounds (issue #1537) shared by
// include/TemporalResolve.glsl (OloTemporalHistoryUVValid) and
// include/SurfaceHistory.glsl (OloEvaluateSurfaceHistory's off-screen test).
// Under a dynamic render scale the history lives in the HALF-OPEN corner
// [0, bounds): uv == bounds is the first unrendered texel through a texel
// fetch, so both tests must reject it, and both must read the same bounds.
//
// The bounds come from the inputs through OLO_TEMPORAL_HISTORY_BOUNDS, the
// macro a resolve under the scale defines before including either header.
//
// Per case i, input:  (bounds.xy, uv.xy)
//            output: (history-uv-valid flag, off-screen-reject flag, 0, 0)
// =============================================================================

layout(local_size_x = 1, local_size_y = 1) in;

layout(std430, binding = 2) readonly buffer Inputs
{
    vec4 u_Inputs[];
};

layout(std430, binding = 1) writeonly buffer Outputs
{
    vec4 u_Outputs[];
};

vec2 g_ProbeBounds = vec2(1.0);
#define OLO_TEMPORAL_HISTORY_BOUNDS g_ProbeBounds
#include "../include/TemporalResolve.glsl"
#include "../include/SurfaceHistory.glsl"

OloSurfaceHistoryRecord ProbeRecord()
{
    OloSurfaceHistoryRecord r;
    r.LinearDepth = 1.0;
    r.GeometricNormal = vec3(0.0, 0.0, 1.0);
    r.ShadingNormal = vec3(0.0, 0.0, 1.0);
    r.Roughness = 0.5;
    r.MaterialClass = 0u;
    r.Coverage = 1.0;
    r.MaterialProfile = 0.0;
    r.Motion = vec2(0.0);
    r.Instance = uvec2(0u);
    r.Primitive = uvec2(0u);
    r.Material = uvec2(0u);
    r.Flags = 0u;
    r.HitDistance = 0.0;
    r.PrimitiveLocalIndex = 0u;
    return r;
}

void main()
{
    uint i = gl_GlobalInvocationID.x;
    vec4 a = u_Inputs[i];
    g_ProbeBounds = a.xy;

    OloSurfaceHistorySettings settings;
    settings.TestMask = 0u; // only the unconditional history and off-screen tests
    settings.RelativeDepthThreshold = 0.05;
    settings.GeometricNormalCosineThreshold = 0.9;
    settings.ShadingNormalCosineThreshold = 0.9;
    settings.RoughnessThreshold = 0.1;
    settings.MotionThresholdPixels = 64.0;
    settings.RelativeHitDistanceThreshold = 0.1;
    settings.CoverageRejectThreshold = 0.5;
    settings.PixelSize = vec2(1.0 / 1024.0);

    OloSurfaceHistoryRecord record = ProbeRecord();
    uint reasons = OloEvaluateSurfaceHistory(record, record, a.zw, true, settings);
    u_Outputs[i] = vec4(OloTemporalHistoryUVValid(a.zw) ? 1.0 : 0.0,
                        (reasons & OLO_SURFACE_REJECT_OFF_SCREEN) != 0u ? 1.0 : 0.0, 0.0, 0.0);
}
