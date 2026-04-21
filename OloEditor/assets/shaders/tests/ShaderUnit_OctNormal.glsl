#version 460 core

// =============================================================================
// ShaderUnit_OctNormal.glsl
//
// Test harness for octahedral normal encoding / decoding. Source of the encode
// function is in several production shaders (PBR.glsl, Water.glsl, Terrain_*.glsl);
// the decode lives in SSAO.glsl. Duplicated verbatim here — changes there must
// be reflected below or the test fails.
//
// Inputs  (SSBO binding 0) : N × vec3, packed as triples of floats.
// Outputs (SSBO binding 1) : N × vec3, the round-tripped normals.
//
// Expected property: decode(encode(n)) ≈ n (within ~1e-5 for fp32 math).
// Every normal on the unit sphere should survive the round-trip.
// =============================================================================

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly  buffer Inputs  { float u_Inputs[]; };
layout(std430, binding = 1) writeonly buffer Outputs { float u_Outputs[]; };

vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

vec3 octDecode(vec2 f)
{
    vec3 n = vec3(f.xy, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    uint total = u_Inputs.length() / 3u;
    if (idx >= total)
        return;

    uint base = idx * 3u;
    vec3 n = normalize(vec3(u_Inputs[base + 0u], u_Inputs[base + 1u], u_Inputs[base + 2u]));

    vec2 encoded = octEncode(n);
    vec3 decoded = octDecode(encoded);

    u_Outputs[base + 0u] = decoded.x;
    u_Outputs[base + 1u] = decoded.y;
    u_Outputs[base + 2u] = decoded.z;
}
