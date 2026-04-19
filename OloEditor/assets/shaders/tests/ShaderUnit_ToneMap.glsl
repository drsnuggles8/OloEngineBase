#version 460 core

// =============================================================================
// ShaderUnit_ToneMap.glsl
//
// Layer-2 Shader Unit Test harness for tone mapping operators. Mirrors the
// operator bodies in assets/shaders/PostProcess_ToneMap.glsl verbatim so
// that changes there must be reflected here — this intentional duplication
// lets the tests fail if someone tweaks the production shader's curve
// without understanding the impact.
//
// Inputs (SSBO binding 0): flat array of floats; interpreted as N × vec3
// per operator-agnostic dispatch. Caller writes triples of (r,g,b).
// Outputs (SSBO binding 1): triples of (r,g,b) after tone mapping, no
// exposure, no gamma.
//
// Operator selected by u_Op uniform:
//   0 = Reinhard, 1 = ACES (Narkowicz), 2 = Uncharted 2 (Hable)
// =============================================================================

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly  buffer Inputs  { float u_Inputs[]; };
layout(std430, binding = 1) writeonly buffer Outputs { float u_Outputs[]; };

uniform int u_Op;

vec3 ReinhardToneMapping(vec3 color)
{
    return color / (color + vec3(1.0));
}

vec3 AcesToneMapping(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 Uncharted2ToneMapping(vec3 color)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
}

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    uint total = u_Inputs.length() / 3u;
    if (idx >= total)
        return;

    uint base = idx * 3u;
    vec3 hdr = vec3(u_Inputs[base + 0u], u_Inputs[base + 1u], u_Inputs[base + 2u]);
    vec3 mapped;
    if (u_Op == 0)
        mapped = ReinhardToneMapping(hdr);
    else if (u_Op == 1)
        mapped = AcesToneMapping(hdr);
    else
        mapped = Uncharted2ToneMapping(hdr);

    u_Outputs[base + 0u] = mapped.x;
    u_Outputs[base + 1u] = mapped.y;
    u_Outputs[base + 2u] = mapped.z;
}
