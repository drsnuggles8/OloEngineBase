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

// FXAA 3.11 — Based on Timothy Lottes' algorithm
// Operates on LDR input (must run after tone mapping)

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_Texture;

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

// FXAA quality settings
#define FXAA_EDGE_THRESHOLD     (1.0/8.0)
#define FXAA_EDGE_THRESHOLD_MIN (1.0/32.0)
#define FXAA_SEARCH_STEPS       8
#define FXAA_SUBPIX_QUALITY     0.75

float luminance(vec3 color)
{
    return dot(color, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec2 texelSize = vec2(u_InverseScreenWidth, u_InverseScreenHeight);
    vec2 uv = v_TexCoord;

    // Sample center and 4 neighbors
    vec3 rgbM  = texture(u_Texture, uv).rgb;
    vec3 rgbN  = texture(u_Texture, uv + vec2( 0.0, -1.0) * texelSize).rgb;
    vec3 rgbS  = texture(u_Texture, uv + vec2( 0.0,  1.0) * texelSize).rgb;
    vec3 rgbW  = texture(u_Texture, uv + vec2(-1.0,  0.0) * texelSize).rgb;
    vec3 rgbE  = texture(u_Texture, uv + vec2( 1.0,  0.0) * texelSize).rgb;

    float lumM = luminance(rgbM);
    float lumN = luminance(rgbN);
    float lumS = luminance(rgbS);
    float lumW = luminance(rgbW);
    float lumE = luminance(rgbE);

    float lumMin = min(lumM, min(min(lumN, lumS), min(lumW, lumE)));
    float lumMax = max(lumM, max(max(lumN, lumS), max(lumW, lumE)));
    float lumRange = lumMax - lumMin;

    // Early exit: no significant contrast
    if (lumRange < max(FXAA_EDGE_THRESHOLD_MIN, lumMax * FXAA_EDGE_THRESHOLD))
    {
        o_Color = vec4(rgbM, 1.0);
        return;
    }

    // Sample 4 diagonal neighbors
    vec3 rgbNW = texture(u_Texture, uv + vec2(-1.0, -1.0) * texelSize).rgb;
    vec3 rgbNE = texture(u_Texture, uv + vec2( 1.0, -1.0) * texelSize).rgb;
    vec3 rgbSW = texture(u_Texture, uv + vec2(-1.0,  1.0) * texelSize).rgb;
    vec3 rgbSE = texture(u_Texture, uv + vec2( 1.0,  1.0) * texelSize).rgb;

    float lumNW = luminance(rgbNW);
    float lumNE = luminance(rgbNE);
    float lumSW = luminance(rgbSW);
    float lumSE = luminance(rgbSE);

    // Sub-pixel blend factor
    float lumAvg = (lumN + lumS + lumW + lumE) * 0.25;
    float subpixRange = abs(lumAvg - lumM);
    float subpixFactor = clamp(subpixRange / lumRange, 0.0, 1.0);
    subpixFactor = smoothstep(0.0, 1.0, subpixFactor);
    subpixFactor = subpixFactor * subpixFactor * FXAA_SUBPIX_QUALITY;

    // Determine edge direction (horizontal vs vertical)
    float edgeHorz = abs(lumNW + lumNE - 2.0 * lumN) +
                     2.0 * abs(lumW + lumE - 2.0 * lumM) +
                     abs(lumSW + lumSE - 2.0 * lumS);
    float edgeVert = abs(lumNW + lumSW - 2.0 * lumW) +
                     2.0 * abs(lumN + lumS - 2.0 * lumM) +
                     abs(lumNE + lumSE - 2.0 * lumE);
    bool isHorizontal = (edgeHorz >= edgeVert);

    float stepLength = isHorizontal ? texelSize.y : texelSize.x;

    float lumPos = isHorizontal ? lumS : lumE;
    float lumNeg = isHorizontal ? lumN : lumW;

    float gradPos = abs(lumPos - lumM);
    float gradNeg = abs(lumNeg - lumM);

    bool pairN = (gradNeg >= gradPos);
    if (pairN) stepLength = -stepLength;

    // Edge search along perpendicular direction
    vec2 edgeUV = uv;
    if (isHorizontal)
        edgeUV.y += stepLength * 0.5;
    else
        edgeUV.x += stepLength * 0.5;

    vec2 edgeStep = isHorizontal ? vec2(texelSize.x, 0.0) : vec2(0.0, texelSize.y);

    float lumEdge = (pairN ? lumNeg : lumPos) * 0.5 + lumM * 0.5;

    // Search positive direction
    vec2 uvPos = edgeUV + edgeStep;
    float lumEndPos = luminance(texture(u_Texture, uvPos).rgb);
    lumEndPos -= lumEdge;
    bool donePos = abs(lumEndPos) >= lumRange * 0.25;

    for (int i = 1; i < FXAA_SEARCH_STEPS && !donePos; i++)
    {
        uvPos += edgeStep;
        lumEndPos = luminance(texture(u_Texture, uvPos).rgb) - lumEdge;
        donePos = abs(lumEndPos) >= lumRange * 0.25;
    }

    // Search negative direction
    vec2 uvNeg = edgeUV - edgeStep;
    float lumEndNeg = luminance(texture(u_Texture, uvNeg).rgb);
    lumEndNeg -= lumEdge;
    bool doneNeg = abs(lumEndNeg) >= lumRange * 0.25;

    for (int i = 1; i < FXAA_SEARCH_STEPS && !doneNeg; i++)
    {
        uvNeg -= edgeStep;
        lumEndNeg = luminance(texture(u_Texture, uvNeg).rgb) - lumEdge;
        doneNeg = abs(lumEndNeg) >= lumRange * 0.25;
    }

    // Calculate offset
    float distPos = isHorizontal ? (uvPos.x - uv.x) : (uvPos.y - uv.y);
    float distNeg = isHorizontal ? (uv.x - uvNeg.x) : (uv.y - uvNeg.y);
    float dist = min(distPos, distNeg);
    float spanLength = distPos + distNeg;
    float pixelOffset = -dist / spanLength + 0.5;

    bool goodSpan = ((distPos < distNeg) ? (lumEndPos < 0.0) : (lumEndNeg < 0.0)) != (lumM - lumEdge < 0.0);
    float finalOffset = goodSpan ? pixelOffset : 0.0;
    finalOffset = max(finalOffset, subpixFactor);

    vec2 finalUV = uv;
    if (isHorizontal)
        finalUV.y += finalOffset * stepLength;
    else
        finalUV.x += finalOffset * stepLength;

    o_Color = vec4(texture(u_Texture, finalUV).rgb, 1.0);
}
