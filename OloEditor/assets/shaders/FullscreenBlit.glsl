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

layout(binding = 0) uniform sampler2D u_Texture;

// DRS bounds: xy = (renderWidth / physicalWidth, renderHeight / physicalHeight).
// Clamp screen-space UVs to [0, bounds] so the upscale blit never samples
// uninitialised texels beyond the DRS-rendered region.
// Both components are 1.0 when DRS is inactive (scale == 1.0).
layout(std140, binding = 33) uniform DRSParams
{
    vec2 u_RenderScaleBounds;
    vec2 _drsPad;
};

void main()
{
    vec2 uv = min(v_TexCoord, u_RenderScaleBounds);
    o_Color = texture(u_Texture, uv);
}
