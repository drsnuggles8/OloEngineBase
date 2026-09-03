// =============================================================================
// SplatSpike_OpaqueBaseline.glsl
//
// The control for the Gaussian-splat measurement (issue #971). It draws the
// SAME primitives from the SAME buffers at the SAME screen positions and sizes
// as SplatSpike_Gaussian.glsl -- and then does none of the things that make a
// splat a splat: no per-fragment Gaussian, no blending, no view-dependent
// order, depth test on and depth write on.
//
// That is the comparison the spike needs. Timing splats against an unrelated
// PBR mesh would measure the shader complexity difference; timing them against
// this measures exactly what the SPLAT REPRESENTATION costs over conventional
// opaque geometry covering the same pixels.
// =============================================================================

#type vertex
#version 460 core

struct SplatRecord
{
    float PosX;
    float PosY;
    float PosZ;
    uint ColorOpacity;
    uint CovXXXY;
    uint CovXZYY;
    uint CovYZZZ;
    uint Pad0;
};

layout(std430, binding = 0) readonly buffer SplatBuffer
{
    SplatRecord b_Splats[];
};

layout(std140, binding = 7) uniform SplatViewUniforms
{
    mat4 u_View;
    mat4 u_Projection;
    vec4 u_ViewportFocal;
};

layout(location = 0) out vec4 v_Color;

const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main()
{
    // Indexed directly, NOT through a sorted order buffer: not needing one is
    // half of what makes opaque geometry cheaper, so the control must not pay
    // for it.
    SplatRecord splat = b_Splats[uint(gl_InstanceIndex)];

    vec3 viewPos = (u_View * vec4(splat.PosX, splat.PosY, splat.PosZ, 1.0)).xyz;
    float depth = max(-viewPos.z, 1e-4);

    vec2 covA = unpackHalf2x16(splat.CovXXXY);
    vec2 covB = unpackHalf2x16(splat.CovXZYY);
    vec2 covC = unpackHalf2x16(splat.CovYZZZ);

    // Same footprint as the splat path: 3 sigma of the largest world axis,
    // projected. Bounding it by the trace (rather than the exact projected
    // covariance) keeps the control's own vertex work minimal while landing
    // within a pixel or two of the same quad size.
    float sigma = sqrt(max(covA.x + covB.y + covC.y, 0.0));
    float radiusPixels = ceil(3.0 * sigma * u_ViewportFocal.w / depth);

    vec4 clip = u_Projection * vec4(viewPos, 1.0);
    vec2 centerPixels = (clip.xy / clip.w * 0.5 + 0.5) * u_ViewportFocal.xy;
    vec2 pixel = centerPixels + kCorners[gl_VertexIndex & 3] * radiusPixels;

    v_Color = vec4(float((splat.ColorOpacity) & 0xFFu) / 255.0, float((splat.ColorOpacity >> 8) & 0xFFu) / 255.0,
                   float((splat.ColorOpacity >> 16) & 0xFFu) / 255.0, 1.0);

    gl_Position = vec4((pixel / u_ViewportFocal.xy) * 2.0 - 1.0, clip.z / clip.w, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec4 v_Color;
layout(location = 0) out vec4 o_Color;

void main()
{
    o_Color = v_Color;
}
