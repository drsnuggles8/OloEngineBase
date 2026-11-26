#version 460 core

#include "../include/WaterShoreCommon.glsl"
vec4 waterShoreFetchDepth(vec2 uv) { return vec4(0.0); }

layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer Inputs { vec4 inputs[]; };
layout(std430, binding = 1) writeonly buffer Outputs { vec4 outputs[]; };

void main()
{
    uint row = 3u * gl_GlobalInvocationID.x;
    // The W lanes carry depth and its two spatial derivatives.
    vec3 delta = inputs[row].xyz;
    mat2x3 derivative = mat2x3(inputs[row + 1u].xyz, inputs[row + 2u].xyz);
    WaterShoreSample shore = WaterShoreSample(inputs[row].w, vec2(inputs[row + 1u].w, inputs[row + 2u].w), 1.0);
    float breaking = waterShoreLimitFFT(shore, WATER_SHORE_BREAKER_INDEX, delta, derivative);
    outputs[row] = vec4(delta, breaking);
    outputs[row + 1u] = vec4(derivative[0], 0.0);
    outputs[row + 2u] = vec4(derivative[1], 0.0);
}
