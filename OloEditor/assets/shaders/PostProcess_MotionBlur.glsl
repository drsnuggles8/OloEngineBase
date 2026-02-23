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

layout(binding = 0) uniform sampler2D u_Texture;      // Scene color (HDR)
layout(binding = 19) uniform sampler2D u_DepthTexture;  // Scene depth

layout(std140, binding = 7) uniform PostProcessUBO
{
    int   u_TonemapOperator;
    float u_Exposure;
    float u_Gamma;
    float u_BloomThreshold;

    float u_BloomIntensity;
    float u_VignetteIntensity;
    float u_VignetteSmoothness;
    float u_ChromaticAberrationIntensity;

    float u_DOFFocusDistance;
    float u_DOFFocusRange;
    float u_DOFBokehRadius;
    float u_MotionBlurStrength;

    int   u_MotionBlurSamples;
    float u_InverseScreenWidth;
    float u_InverseScreenHeight;
    float _padding0;

    float u_TexelSizeX;
    float u_TexelSizeY;
    float u_Near;
    float u_Far;
};

// Motion blur matrices (UBO binding 8)
layout(std140, binding = 8) uniform MotionBlurUBO
{
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

void main()
{
    // Reconstruct world position from depth
    float depth = texture(u_DepthTexture, v_TexCoord).r;

    // NDC position
    vec4 ndcPos = vec4(v_TexCoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);

    // World position
    vec4 worldPos = u_InverseViewProjection * ndcPos;
    worldPos /= worldPos.w;

    // Project to previous frame's screen position
    vec4 prevClipPos = u_PrevViewProjection * worldPos;
    vec2 prevUV = (prevClipPos.xy / prevClipPos.w) * 0.5 + 0.5;

    // Velocity = current UV - previous UV
    vec2 velocity = (v_TexCoord - prevUV) * u_MotionBlurStrength;

    // Clamp velocity to prevent excessive blur
    float speed = length(velocity / vec2(u_InverseScreenWidth, u_InverseScreenHeight));
    float maxSpeed = 40.0; // max pixels of blur
    if (speed > maxSpeed)
    {
        velocity *= maxSpeed / speed;
    }

    // Accumulate samples along the velocity vector
    vec3 result = texture(u_Texture, v_TexCoord).rgb;
    float totalWeight = 1.0;

    int numSamples = u_MotionBlurSamples;
    for (int i = 1; i < numSamples; i++)
    {
        float t = float(i) / float(numSamples - 1) - 0.5; // [-0.5, 0.5]
        vec2 sampleUV = v_TexCoord + velocity * t;
        sampleUV = clamp(sampleUV, vec2(0.0), vec2(1.0));
        result += texture(u_Texture, sampleUV).rgb;
        totalWeight += 1.0;
    }

    result /= totalWeight;
    o_Color = vec4(result, 1.0);
}
