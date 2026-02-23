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

// SSAO — Screen-Space Ambient Occlusion
// Uses G-buffer view-space normals and depth to compute per-pixel occlusion.
// Hemisphere sampling with random kernel + noise texture rotation.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// Scene depth (from ScenePass)
layout(binding = 19) uniform sampler2D u_DepthTexture;

// View-space normals (from ScenePass G-buffer, attachment 2)
layout(binding = 22) uniform sampler2D u_NormalsTexture;

// 4x4 random rotation noise texture
layout(binding = 21) uniform sampler2D u_NoiseTexture;

// SSAO UBO (binding 9)
layout(std140, binding = 9) uniform SSAOUBO
{
    float u_Radius;
    float u_Bias;
    float u_Intensity;
    int   u_Samples;

    int   u_ScreenWidth;
    int   u_ScreenHeight;
    float _pad0;
    float _pad1;

    mat4  u_Projection;
    mat4  u_InverseProjection;
};

// Reconstruct view-space position from depth and screen UV
vec3 reconstructViewPos(vec2 uv, float depth)
{
    // Convert to NDC
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = u_InverseProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

// Hash function for generating sample kernel in-shader
// Based on interleaved gradient noise pattern
float interleavedGradientNoise(vec2 coord)
{
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(coord, magic.xy)));
}

// Cosine-weighted hemisphere sample distribution
vec3 hemisphereSample(int index, int totalSamples)
{
    // Use Hammersley sequence for well-distributed points
    float i = float(index);
    float n = float(totalSamples);

    // Van der Corput radical inverse
    uint bits = uint(index);
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float radicalInverse = float(bits) * 2.3283064365386963e-10;

    float phi = radicalInverse * 2.0 * 3.14159265;
    float cosTheta = 1.0 - i / n; // Cosine-weighted
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    // Scale so samples are distributed within the hemisphere,
    // with more samples near the origin (closer samples contribute more)
    float scale = (i + 1.0) / n;
    scale = mix(0.1, 1.0, scale * scale); // Accelerating interpolation

    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta) * scale;
}

void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;

    // Skip skybox / far plane
    if (depth >= 1.0)
    {
        o_Color = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    // Reconstruct view-space position and read view-space normal
    vec3 fragPos = reconstructViewPos(v_TexCoord, depth);
    vec3 normal = normalize(texture(u_NormalsTexture, v_TexCoord).xyz);

    // If normal is zero (particles, 2D quads, etc.), no occlusion
    if (dot(normal, normal) < 0.01)
    {
        o_Color = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    // Sample noise for random rotation (tile 4x4 across screen)
    vec2 noiseScale = vec2(float(u_ScreenWidth) / 4.0, float(u_ScreenHeight) / 4.0);
    vec2 noiseVec = texture(u_NoiseTexture, v_TexCoord * noiseScale).rg;

    // Construct TBN matrix from noise rotation and surface normal
    vec3 tangent = normalize(noiseVec.x * vec3(1.0, 0.0, 0.0) + noiseVec.y * vec3(0.0, 1.0, 0.0));
    tangent = normalize(tangent - normal * dot(tangent, normal)); // Gram-Schmidt orthogonalize
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    // Accumulate occlusion
    float occlusion = 0.0;
    int sampleCount = clamp(u_Samples, 4, 64);

    for (int i = 0; i < sampleCount; ++i)
    {
        // Get sample position in hemisphere
        vec3 sampleDir = TBN * hemisphereSample(i, sampleCount);
        vec3 samplePos = fragPos + sampleDir * u_Radius;

        // Project sample to screen space
        vec4 offset = u_Projection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        offset.xy = offset.xy * 0.5 + 0.5; // NDC to [0, 1]

        // Skip out-of-bounds samples
        if (offset.x < 0.0 || offset.x > 1.0 || offset.y < 0.0 || offset.y > 1.0)
        {
            continue;
        }

        // Sample scene depth at projected position
        float sampleDepth = texture(u_DepthTexture, offset.xy).r;
        vec3 sampleViewPos = reconstructViewPos(offset.xy, sampleDepth);

        // Range check: only occlude if the sampled geometry is within radius
        float rangeCheck = smoothstep(0.0, 1.0, u_Radius / abs(fragPos.z - sampleViewPos.z));

        // Compare: if the geometry at the sample is closer than our sample point, it occludes
        occlusion += (sampleViewPos.z >= samplePos.z + u_Bias ? 1.0 : 0.0) * rangeCheck;
    }

    float ao = 1.0 - (occlusion / float(sampleCount)) * u_Intensity;
    ao = clamp(ao, 0.0, 1.0);

    o_Color = vec4(ao, 0.0, 0.0, 1.0);
}
