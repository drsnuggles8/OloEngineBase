// NOTE: single-file compute shader (no `#type` prefix — the compute-shader
// loader in this project uses a plain `#version ... core` header directly).
#version 460 core

// =============================================================================
// ShaderUnit_SrgbRoundTrip.glsl
//
// Layer-2 (Shader Unit Test) harness for sRGB ↔ linear conversion math.
// Reads N input float values from SSBO 0, encodes to sRGB, decodes back to
// linear, writes both intermediate and final values to SSBO 1. The test
// side verifies:
//   1. Round-trip error < 1 LSB (linear → sRGB → linear) for all inputs
//   2. sRGB(0) == 0 and sRGB(1) == 1 (endpoints preserved)
//
// This is the canonical "Approach A: Compute Shader Harness" from
// docs/renderer-testing-strategy.md §2.
// =============================================================================

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer Inputs { float u_Inputs[]; };

struct OutputPair
{
    float sRGB;       // encoded value
    float decoded;    // decoded back to linear
};

layout(std430, binding = 1) writeonly buffer Outputs { OutputPair u_Outputs[]; };

// Matches the IEC 61966-2-1 sRGB transfer function.
float LinearToSrgb(float x)
{
    x = clamp(x, 0.0, 1.0);
    return (x <= 0.0031308) ? (12.92 * x) : (1.055 * pow(x, 1.0 / 2.4) - 0.055);
}

float SrgbToLinear(float x)
{
    x = clamp(x, 0.0, 1.0);
    return (x <= 0.04045) ? (x / 12.92) : pow((x + 0.055) / 1.055, 2.4);
}

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= u_Inputs.length())
        return;

    float linearIn = u_Inputs[idx];
    float srgb     = LinearToSrgb(linearIn);
    float back     = SrgbToLinear(srgb);

    u_Outputs[idx].sRGB = srgb;
    u_Outputs[idx].decoded = back;
}
