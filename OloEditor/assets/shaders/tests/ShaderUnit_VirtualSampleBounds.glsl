// Layer-2 GPU contract for the virtual-geometry raster's sub-sample-miss rule
// (issue #712) — runs the SHIPPED include, not a copy of it, so a divergence
// between VirtualRasterCoverage.glsl and the CPU mirror in
// VirtualRasterCoverageTest.cpp fails here rather than in a screenshot.
#version 460 core

#include "../include/VirtualRasterCoverage.glsl"

layout(local_size_x = 1) in;

// Two vec4s per case: [s0.xy, s1.xy] and [s2.xy, viewport.xy].
layout(std430, binding = 0) readonly buffer Inputs { vec4 cases[]; };

// One per case: xy = sampleMin, zw = sampleMax (unspecified when .x below is 0).
layout(std430, binding = 1) writeonly buffer Ranges { ivec4 ranges[]; };

// One per case: x = 1 when the triangle covers at least one pixel centre,
// y = the sign of the window-space doubled area (0 = negative, 1 = positive,
// 2 = neither, i.e. zero or NaN).
layout(std430, binding = 2) writeonly buffer Meta { uvec4 meta[]; };

void main()
{
    uint i = gl_GlobalInvocationID.x;
    vec4 a = cases[i * 2u];
    vec4 b = cases[i * 2u + 1u];
    vec2 s0 = a.xy;
    vec2 s1 = a.zw;
    vec2 s2 = b.xy;
    vec2 viewport = b.zw;

    ivec2 lo;
    ivec2 hi;
    bool covered = OloVirtualTriangleSampleRange(s0, s1, s2, viewport, lo, hi);
    ranges[i] = ivec4(lo, hi);

    float area = OloVirtualSignedArea2(s0, s1, s2);
    uint areaSign = (area > 0.0) ? 1u : ((area < 0.0) ? 0u : 2u);
    meta[i] = uvec4(covered ? 1u : 0u, areaSign, 0u, 0u);
}
