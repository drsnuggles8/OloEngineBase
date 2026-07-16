// =============================================================================
// PostProcess_CloudscapeResolve.glsl — cloud temporal accumulation (pass B)
//
// Half-resolution: blends this frame's jittered raymarch (pass A) with the
// reprojected history using a 3x3 neighborhood clamp (the TAA variance-clip
// idea simplified to min/max — clouds are soft, min/max suffices). History
// reprojection uses the ray's cloud-layer midpoint as the representative
// world position (clouds have no depth buffer; the layer midpoint is stable
// and cheap to recompute here without carrying depth through pass A).
//
// The graph extracts this pass's output into the history sink each frame
// (TAA RegisterHistoryTextureSink/ExtractHistoryTexture pattern), so history
// validity is handled by the pipeline: u_CloudMisc.x (temporal blend) is
// forced to 0 on the first frame / after invalidation.
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
layout(location = 0) out vec4 o_Cloud;

#include "include/NoiseCommon.glsl"
#include "include/CloudscapeCommon.glsl"

layout(binding = 0) uniform sampler2D u_CloudCurrent; // pass A output (half-res)
layout(binding = 1) uniform sampler2D u_CloudHistory; // last frame's resolve (half-res)

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

layout(std140, binding = 8) uniform MotionBlurUBO {
    mat4 u_InverseViewProjection;
    mat4 u_MB_PrevViewProjection; // ABSOLUTE world -> previous clip
};

void main()
{
    vec4 current = texture(u_CloudCurrent, v_TexCoord);
    float blend = clamp(u_CloudMisc.x, 0.0, 0.98);
    if (blend <= 0.0 || u_CloudMisc.w < 0.5)
    {
        o_Cloud = current;
        return;
    }

    // Representative world position: the view ray's cloud-layer midpoint (or
    // a fixed far distance when the ray misses the layer — sky rotation).
    vec4 ndc = vec4(v_TexCoord * 2.0 - 1.0, 1.0, 1.0);
    vec4 worldFar = u_InverseViewProjection * ndc;
    worldFar.xyz /= worldFar.w;
    vec3 cameraPos = u_CameraPosition + u_RenderOrigin;
    vec3 rayDir = normalize((worldFar.xyz + u_RenderOrigin) - cameraPos);
    vec2 slab = cloudLayerIntersect(cameraPos, rayDir);
    float tMid = (slab.y > slab.x) ? 0.5 * (slab.x + slab.y) : 20000.0;
    vec3 worldMid = cameraPos + rayDir * tMid;

    // Reproject into last frame (absolute-world prev VP, camera-relative #429).
    vec4 prevClip = u_MB_PrevViewProjection * vec4(worldMid, 1.0);
    if (prevClip.w <= 0.0)
    {
        o_Cloud = current;
        return;
    }
    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
    {
        o_Cloud = current;
        return;
    }

    vec4 history = texture(u_CloudHistory, prevUV);

    // 3x3 neighborhood min/max clamp on the current frame kills ghosting when
    // the field itself changes (wind, weather transitions).
    vec2 texel = 1.0 / vec2(textureSize(u_CloudCurrent, 0));
    vec4 nMin = current;
    vec4 nMax = current;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec4 s = texture(u_CloudCurrent, v_TexCoord + vec2(x, y) * texel);
            nMin = min(nMin, s);
            nMax = max(nMax, s);
        }
    }
    history = clamp(history, nMin, nMax);

    o_Cloud = mix(current, history, blend);
}
