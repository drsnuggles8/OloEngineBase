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

// =============================================================================
// FSR1 depth + velocity upscale (#480). When the scene renders below display
// resolution, the post-process band still runs at FULL display res (EASU already
// upscaled the colour) — but the depth and motion-vector buffers it reads (DOF,
// Fog, MotionBlur, TAA, underwater fog in ToneMap) are still only available at
// the reduced scene resolution. This pass upsamples them to display res so those
// passes sample full-resolution-density depth/velocity with CORRECT per-texel
// values, instead of a bilinear-blended reduced buffer (bilinear-blending depth
// across a silhouette invents intermediate depths → DOF halos / bad TAA
// reprojection; blending velocity across an edge smears motion).
//
// The upsample is NEAREST — implemented by snapping the sample UV to the reduced
// texel centre, so it is robust regardless of the source sampler's filter mode
// (depth textures / attachment views can carry either). Silhouettes stay at the
// reduced resolution (genuine full-res depth would need a full-res geometry pass,
// which defeats the render-scale win); what this buys is correct depth VALUES at
// full sample density for the display-res post chain.
// =============================================================================

layout(location = 0) out float o_Depth;    // R32F  — reduced depth, nearest-upscaled
layout(location = 1) out vec2 o_Velocity;  // RG16F — reduced velocity, nearest-upscaled

layout(location = 0) in vec2 v_TexCoord;

// Binding slots follow the engine's post-process reuse conventions
// (ShaderBindingLayout::IsKnownTextureBinding): slot 1 (TEX_SPECULAR) is the
// depth-reuse slot for post/particle passes, slot 2 (TEX_NORMAL) is the
// velocity-reuse slot for TAA / motion-blur — matching the passes that read
// these buffers.
layout(binding = 1) uniform sampler2D u_Depth;    // Reduced-res scene depth (sampled .r)
layout(binding = 2) uniform sampler2D u_Velocity; // Reduced-res motion vectors (RG)

// Reduced (rendered) scene size in pixels. Reuses the EASU params UBO layout
// (binding 45): InputSizeAndTexel.xy = reduced width/height.
layout(std140, binding = 45) uniform EASUParams
{
    vec4 u_EASU_InputSizeAndTexel;
    vec4 u_EASU_Bounds;
};
#define u_ReducedSize (u_EASU_InputSizeAndTexel.xy)

// Snap the output UV to the centre of the reduced texel that contains it — a
// filter-mode-independent nearest tap.
vec2 NearestUV(vec2 uv)
{
    vec2 sz = max(u_ReducedSize, vec2(1.0));
    vec2 texel = floor(uv * sz);
    texel = clamp(texel, vec2(0.0), sz - vec2(1.0));
    return (texel + vec2(0.5)) / sz;
}

void main()
{
    vec2 uv = NearestUV(v_TexCoord);
    o_Depth = texture(u_Depth, uv).r;
    o_Velocity = texture(u_Velocity, uv).rg;
}
