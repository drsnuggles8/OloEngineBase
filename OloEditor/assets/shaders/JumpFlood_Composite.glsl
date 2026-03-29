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

layout(binding = 0) uniform sampler2D u_SceneColor;
layout(binding = 1) uniform sampler2D u_JFAResult;

layout(std140, binding = 29) uniform JumpFloodUBO
{
    vec4  u_TexelSize;
    vec4  u_OutlineColor;           // rgb = color, a = opacity
    float u_OutlineThicknessInner;  // smoothstep inner edge
    float u_OutlineThicknessOuter;  // smoothstep outer edge
    int   u_Step;
    int   _pad0;
};

void main()
{
    vec4 sceneColor = texture(u_SceneColor, v_TexCoord);
    vec4 jfa = texture(u_JFAResult, v_TexCoord);

    // Convert squared distance to linear distance
    float dist = sqrt(jfa.z);

    // Anti-aliased outline band via smoothstep (outer → inner = transparent → opaque)
    float alpha = smoothstep(u_OutlineThicknessOuter, u_OutlineThicknessInner, dist);

    if (alpha == 0.0)
    {
        o_Color = sceneColor;
        return;
    }

    // Alpha-blend outline color over scene
    float blendAlpha = alpha * u_OutlineColor.a;
    o_Color = vec4(mix(sceneColor.rgb, u_OutlineColor.rgb, blendAlpha), sceneColor.a);
}
