#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec2 v_TexelSize;
layout(location = 2) out vec2 v_UV0;
layout(location = 3) out vec2 v_UV1;
layout(location = 4) out vec2 v_UV2;
layout(location = 5) out vec2 v_UV3;
layout(location = 6) out vec2 v_UV4;
layout(location = 7) out vec2 v_UV5;
layout(location = 8) out vec2 v_UV6;
layout(location = 9) out vec2 v_UV7;
layout(location = 10) out vec2 v_UV8;

layout(std140, binding = 29) uniform JumpFloodUBO
{
    vec4  u_TexelSize;              // xy = 1/width, 1/height
    vec4  u_OutlineColor;           // rgb = color, a = opacity
    float u_OutlineThicknessInner;  // smoothstep inner edge (default 0.002)
    float u_OutlineThicknessOuter;  // smoothstep outer edge (default 0.004)
    int   u_Step;                   // current JFA step size
    int   _pad0;
};

void main()
{
    v_TexCoord = a_TexCoord;
    v_TexelSize = u_TexelSize.xy;

    vec2 dx = vec2(u_TexelSize.x, 0.0) * u_Step;
    vec2 dy = vec2(0.0, u_TexelSize.y) * u_Step;

    v_UV0 = a_TexCoord;                // center
    v_UV1 = a_TexCoord + dx;           // right
    v_UV2 = a_TexCoord - dx;           // left
    v_UV3 = a_TexCoord + dy;           // up
    v_UV4 = a_TexCoord - dy;           // down
    v_UV5 = a_TexCoord + dx + dy;      // up-right
    v_UV6 = a_TexCoord + dx - dy;      // down-right
    v_UV7 = a_TexCoord - dx + dy;      // up-left
    v_UV8 = a_TexCoord - dx - dy;      // down-left

    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in vec2 v_TexelSize;
layout(location = 2) in vec2 v_UV0;
layout(location = 3) in vec2 v_UV1;
layout(location = 4) in vec2 v_UV2;
layout(location = 5) in vec2 v_UV3;
layout(location = 6) in vec2 v_UV4;
layout(location = 7) in vec2 v_UV5;
layout(location = 8) in vec2 v_UV6;
layout(location = 9) in vec2 v_UV7;
layout(location = 10) in vec2 v_UV8;

layout(binding = 0) uniform sampler2D u_Texture;

// Aspect-ratio-corrected squared distance
float screenDistance(vec2 v)
{
    float ratio = v_TexelSize.x / v_TexelSize.y;
    v.x /= ratio;
    return dot(v, v);
}

void boundsCheck(inout vec2 xy, vec2 uv)
{
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        xy = vec2(1000.0);
}

void main()
{
    vec2 uvs[9] = vec2[](v_UV0, v_UV1, v_UV2, v_UV3, v_UV4, v_UV5, v_UV6, v_UV7, v_UV8);

    vec4 pixel = texture(u_Texture, uvs[0]);

    for (int j = 1; j <= 8; ++j)
    {
        vec4 n = texture(u_Texture, uvs[j]);

        // Only propagate seeds on the same side (inside/outside)
        if (n.w != pixel.w)
            n.xyz = vec3(0.0);

        // Adjust offset relative to center
        n.xy += uvs[j] - uvs[0];

        // Invalidate out-of-bounds neighbors
        boundsCheck(n.xy, uvs[j]);

        float dist = screenDistance(n.xy);
        if (dist < pixel.z)
            pixel.xyz = vec3(n.xy, dist);
    }

    o_Color = pixel;
}
