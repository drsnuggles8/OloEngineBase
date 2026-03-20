#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

void main()
{
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

// Forward+ grid SSBO (binding 12 = SSBO_FPLUS_LIGHT_GRID)
layout(std430, binding = 12) readonly buffer FPlusLightGridBuf { uvec2 fplusGrid[]; };

// Forward+ parameters UBO (binding 25)
layout(std140, binding = 25) uniform ForwardPlusParams {
    uvec4 fplus_Params; // x = TileSizePixels, y = TileCountX, z = Enabled (0/1), w = reserved
};

// Heatmap: maps light count [0..maxLights] -> color (blue to green to red)
vec3 heatmap(float t)
{
    t = clamp(t, 0.0, 1.0);
    if (t < 0.5)
    {
        float s = t * 2.0;
        return mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0), s);
    }
    float s = (t - 0.5) * 2.0;
    return mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), s);
}

void main()
{
    if (fplus_Params.z == 0u)
    {
        discard;
    }

    uint tileSizePixels = fplus_Params.x;
    uint tileCountX     = fplus_Params.y;

    ivec2 fragCoord = ivec2(gl_FragCoord.xy);
    ivec2 tileCoord = fragCoord / ivec2(tileSizePixels);
    uint tileIndex = uint(tileCoord.y) * tileCountX + uint(tileCoord.x);

    uvec2 tileData = fplusGrid[tileIndex];
    uint lightCount = tileData.y;

    // Heatmap normalization (0 lights = blue, 32+ lights = red)
    float t = float(lightCount) / 32.0;
    vec3 heat = heatmap(t);

    // Draw tile grid lines
    ivec2 localPixel = fragCoord - tileCoord * ivec2(tileSizePixels);
    bool onEdge = (localPixel.x == 0 || localPixel.y == 0);
    float edgeFactor = onEdge ? 0.6 : 0.0;

    // Semi-transparent overlay blended with the scene
    float alpha = (lightCount > 0u) ? 0.35 : 0.08;
    alpha = max(alpha, edgeFactor);

    o_Color = vec4(heat, alpha);
}
