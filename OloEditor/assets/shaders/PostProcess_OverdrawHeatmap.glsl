// =============================================================================
// PostProcess_OverdrawHeatmap.glsl - Overdraw debug view heatmap composite (#519)
//
// Fullscreen pass that reads the overdraw accumulation target (red channel =
// per-pixel fragment count, produced by re-drawing opaque geometry with additive
// blending and depth test off) and maps the count to a legible heat colour:
//   black -> blue -> green -> yellow -> red, as the layer count rises.
//
// The count->colour ramp is a line-for-line mirror of the CPU reference in
// OloEngine/src/OloEngine/Renderer/OverdrawHeatmap.h (pinned by
// OverdrawHeatmapMathTest). If you change one, change both. The pass runs late
// in the post chain (after tone mapping) so the LDR heat colours reach the
// viewport undistorted.
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

// Overdraw accumulation buffer (red channel = raw fragment count).
layout(binding = 0) uniform sampler2D u_OverdrawCount;

// Fragment count that saturates the ramp (matches
// OverdrawHeatmap::kOverdrawHeatmapMaxLayers).
const float kMaxLayers = 10.0;

vec3 HeatColor(float count)
{
    float t = clamp(count / max(kMaxLayers, 1e-6), 0.0, 1.0);

    const vec3 kBlack  = vec3(0.0, 0.0, 0.0);
    const vec3 kBlue   = vec3(0.0, 0.0, 1.0);
    const vec3 kGreen  = vec3(0.0, 1.0, 0.0);
    const vec3 kYellow = vec3(1.0, 1.0, 0.0);
    const vec3 kRed    = vec3(1.0, 0.0, 0.0);

    if (t < 0.25)
        return mix(kBlack, kBlue, t / 0.25);
    if (t < 0.5)
        return mix(kBlue, kGreen, (t - 0.25) / 0.25);
    if (t < 0.75)
        return mix(kGreen, kYellow, (t - 0.5) / 0.25);
    return mix(kYellow, kRed, (t - 0.75) / 0.25);
}

void main()
{
    float count = texture(u_OverdrawCount, v_TexCoord).r;
    o_Color = vec4(HeatColor(count), 1.0);
}
