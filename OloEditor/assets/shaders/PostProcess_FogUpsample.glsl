//--------------------------
// - OloEngine -
// Volumetric Fog Bilateral Upsample + Composite
//
// Takes half-resolution fog result (RGB = inscatter, A = transmittance)
// and composites it onto the full-resolution scene color using
// depth-aware bilateral upsampling to preserve silhouette edges.
// --------------------------
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

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_SceneColor;    // Full-res HDR scene
layout(binding = 1) uniform sampler2D u_FogTexture;    // Half-res fog (RGB=inscatter, A=transmittance)
layout(binding = 19) uniform sampler2D u_DepthTexture; // Full-res depth

// Camera UBO for near/far linearization
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// ---------------------------------------------------------------------------
// Linearize depth for bilateral weight computation
// ---------------------------------------------------------------------------
float linearizeDepth(float d)
{
    // Extract near/far from projection matrix
    float near = u_Projection[3][2] / (u_Projection[2][2] - 1.0);
    float far  = u_Projection[3][2] / (u_Projection[2][2] + 1.0);
    float ndc = d * 2.0 - 1.0;
    return (2.0 * near * far) / (far + near - ndc * (far - near));
}

// ---------------------------------------------------------------------------
// Bilateral upsample: 2×2 bilinear with depth-aware edge rejection.
// Prevents fog bleeding across depth discontinuities (object silhouettes).
// ---------------------------------------------------------------------------
vec4 bilateralUpsampleFog(vec2 uv)
{
    vec2 fogTexSize = vec2(textureSize(u_FogTexture, 0));
    vec2 texelSize = 1.0 / fogTexSize;

    // Map full-res UV to half-res texel grid
    vec2 halfResCoord = uv * fogTexSize - 0.5;
    vec2 baseCoord = floor(halfResCoord);
    vec2 frac = halfResCoord - baseCoord;

    // Full-res center depth (reference for edge detection)
    float centerDepth = linearizeDepth(texture(u_DepthTexture, uv).r);

    // Depth sensitivity — adaptive to view distance
    float depthSigma = 0.02 * abs(centerDepth);
    depthSigma = max(depthSigma, 0.01);
    float invTwoSigmaSq = 1.0 / (2.0 * depthSigma * depthSigma);

    float totalWeight = 0.0;
    vec4 totalFog = vec4(0.0);

    // Sample 2×2 nearest half-res texels
    for (int dy = 0; dy <= 1; ++dy)
    {
        for (int dx = 0; dx <= 1; ++dx)
        {
            vec2 sampleUV = (baseCoord + vec2(float(dx), float(dy)) + 0.5) * texelSize;
            sampleUV = clamp(sampleUV, texelSize * 0.5, 1.0 - texelSize * 0.5);

            // Sample half-res fog
            vec4 sampleFog = texture(u_FogTexture, sampleUV);

            // Depth at the half-res sample location (read from full-res depth)
            float sampleDepth = linearizeDepth(texture(u_DepthTexture, sampleUV).r);

            // Bilinear weight
            float bx = (dx == 0) ? (1.0 - frac.x) : frac.x;
            float by = (dy == 0) ? (1.0 - frac.y) : frac.y;
            float bilinearWeight = bx * by;

            // Depth-based edge weight (Gaussian falloff on depth difference)
            float depthDiff = centerDepth - sampleDepth;
            float depthWeight = exp(-depthDiff * depthDiff * invTwoSigmaSq);

            float w = bilinearWeight * depthWeight;
            totalFog += sampleFog * w;
            totalWeight += w;
        }
    }

    // Normalize, fallback to simple bilinear if all weights are tiny
    if (totalWeight > 0.0001)
        return totalFog / totalWeight;
    else
        return texture(u_FogTexture, uv);
}

void main()
{
    vec3 sceneColor = texture(u_SceneColor, v_TexCoord).rgb;

    // Bilateral upsample the half-res fog
    vec4 fog = bilateralUpsampleFog(v_TexCoord);

    vec3 inscatter = fog.rgb;
    float transmittance = fog.a;

    // Composite: scene * transmittance + inscattered light
    vec3 result = sceneColor * transmittance + inscatter;

    o_Color = vec4(result, 1.0);
}
