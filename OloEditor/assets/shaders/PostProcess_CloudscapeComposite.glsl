// =============================================================================
// PostProcess_CloudscapeComposite.glsl — cloud upsample + composite (pass C)
//
// Full resolution: depth-aware 2x2 upsample of the half-res resolved cloud
// buffer (mirrors PostProcess_FogUpsample's bilateral weighting — cloud
// edges against geometry need the depth guard, the open sky does not), then
// the standard transmittance composite over the scene colour:
//     result = sceneColor * cloudTransmittance + cloudInscatter
// This runs BEFORE the fog pass in the post chain, so the froxel fog +
// analytic tail apply aerial perspective over the clouds for free.
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

layout(binding = 0) uniform sampler2D u_SceneColor;   // full-res upstream colour
layout(binding = 1) uniform sampler2D u_CloudResolved; // half-res resolved clouds
layout(binding = 19) uniform sampler2D u_DepthTexture; // full-res scene depth

void main()
{
    vec3 scene = texture(u_SceneColor, v_TexCoord).rgb;

    // Depth-aware 2x2 gather: weight each half-res tap by how close its
    // (half-res) depth neighborhood is to this full-res pixel's depth,
    // mirroring the fog upsample's bilateral idea. Sky pixels (depth ~1)
    // dominate cloud coverage, where a plain bilinear tap is already right.
    float centerDepth = texture(u_DepthTexture, v_TexCoord).r;
    vec2 halfTexel = 1.0 / vec2(textureSize(u_CloudResolved, 0));

    vec4 cloud = vec4(0.0);
    float weightSum = 0.0;
    for (int y = 0; y <= 1; ++y)
    {
        for (int x = 0; x <= 1; ++x)
        {
            vec2 offset = (vec2(x, y) - 0.5) * halfTexel;
            vec2 uv = v_TexCoord + offset;
            // Compare against the full-res depth at the tap position — a
            // cheap stand-in for a half-res depth pyramid.
            float tapDepth = texture(u_DepthTexture, uv).r;
            float depthDelta = abs(tapDepth - centerDepth);
            float w = exp(-depthDelta * 400.0) + 1.0e-3;
            cloud += texture(u_CloudResolved, uv) * w;
            weightSum += w;
        }
    }
    cloud /= weightSum;

    o_Color = vec4(scene * clamp(cloud.a, 0.0, 1.0) + max(cloud.rgb, vec3(0.0)), 1.0);
}
