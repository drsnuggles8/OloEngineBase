// =============================================================================
// ShaderUnit_DofCoc.glsl
//
// CoC-only probe for the DOF pass. Reproduces exactly the CoC math from
// PostProcess_DOF.glsl (linear model, clamped to [0, 1]) but outputs the
// CoC value + blur radius directly instead of running the bokeh tap loop.
//
// Output encoding (R channel): coc in [0, 1]
//                  (G channel): blurRadius / u_DOFBokehRadius (== coc, redundant but validates the multiply)
//
// Test fixture authors a 1D gradient of NDC-encoded depth values across U
// so one draw covers the full near→focus→far sweep. Invariants asserted:
//   - CoC(focus) = 0
//   - CoC is monotonic in |linearDepth - focusDistance|
//   - CoC saturates to 1.0 once |Δdepth| >= focusRange
//   - Symmetric around focus (CoC(focus+d) == CoC(focus-d))
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

layout(binding = 19) uniform sampler2D u_DepthTexture; // Matches production DOF binding

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

float linearizeDepth(float d)
{
    float z = d * 2.0 - 1.0;
    return (2.0 * u_Near * u_Far) / (u_Far + u_Near - z * (u_Far - u_Near));
}

void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;
    float linearDepth = linearizeDepth(depth);

    float coc = abs(linearDepth - u_DOFFocusDistance) / u_DOFFocusRange;
    coc = clamp(coc, 0.0, 1.0);

    float blurRadius = coc * u_DOFBokehRadius;

    o_Color = vec4(coc, blurRadius / max(u_DOFBokehRadius, 1e-6), linearDepth / u_Far, 1.0);
}
