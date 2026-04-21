// =============================================================================
// ShaderUnit_WhitePrefilter.glsl
//
// Unit probe for the GGX specular importance-sampling prefilter kernel used
// by IBLPrefilter.glsl. Mirrors `ShaderUnit_WhiteIrradiance.glsl` but for the
// specular lobe. Replaces the samplerCube lookup with uniform-white radiance
// so we can run the exact same integrator over a known analytic integrand.
//
// Invariant: for a uniform-white environment L_i(ω) = 1, the normalized
// prefilter integral reduces to
//
//     prefilteredColor = Σ L_i * NdotL / Σ NdotL = 1
//
// regardless of roughness. Any deviation indicates a normalization bug
// (missing/extra division by totalWeight), accumulation sign flip, or
// importance-sample PDF error.
//
// The probe sweeps roughness across the U axis (u ∈ [0, 1]) so one draw
// covers the full roughness range. The test samples several columns and
// asserts each is within tolerance of 1.0.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

const float PI = 3.14159265359;

// Hammersley / Van der Corput — identical to IBLPrefilter.glsl so a bug in
// the base sequence would show up here too.
float VanDerCorput(uint n, uint base)
{
    float invBase = 1.0 / float(base);
    float denom = 1.0;
    float result = 0.0;
    for (uint i = 0u; i < 32u; ++i)
    {
        if (n > 0u)
        {
            denom = mod(float(n), 2.0);
            result += denom * invBase;
            invBase = invBase / 2.0;
            n = uint(float(n) / 2.0);
        }
    }
    return result;
}

vec2 Hammersley(uint i, uint N)
{
    return vec2(float(i) / float(N), VanDerCorput(i, 2u));
}

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);

    vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

void main()
{
    // Sweep roughness across U so the test can sample several columns.
    float roughness = clamp(v_TexCoord.x, 0.0, 1.0);

    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 R = N;
    vec3 V = R;

    const uint SAMPLE_COUNT = 512u;
    float totalWeight = 0.0;
    vec3 prefilteredColor = vec3(0.0);

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            // Substitute the cubemap lookup with uniform-white radiance.
            vec3 sampleColor = vec3(1.0);
            prefilteredColor += sampleColor * NdotL;
            totalWeight += NdotL;
        }
    }
    prefilteredColor = prefilteredColor / max(totalWeight, 1e-6);
    o_Color = vec4(prefilteredColor, 1.0);
}
