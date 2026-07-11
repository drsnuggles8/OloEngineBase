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

// Forward+ cluster grid SSBO (binding 12 = SSBO_FPLUS_LIGHT_GRID)
layout(std430, binding = 12) readonly buffer FPlusLightGridBuf { uvec2 fplusGrid[]; };

// Forward+ clustered parameters UBO (binding 25)
layout(std140, binding = 25) uniform ForwardPlusParams {
    uvec4 fplus_Params;       // x = ClusterCountX, y = ClusterCountY, z = Enabled (0/1), w = ClusterCountZ
    vec4  fplus_TileScale;    // xy = clusterCount / screenSize, zw = unused
    vec4  fplus_DepthSlicing; // x = sliceScale, y = sliceBias, z = zNear, w = zFar
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

    uint countX = fplus_Params.x;
    uint countY = fplus_Params.y;
    uint countZ = fplus_Params.w;

    vec2 tileCoordF = gl_FragCoord.xy * fplus_TileScale.xy;
    uvec2 tileCoord = min(uvec2(tileCoordF), uvec2(countX - 1u, countY - 1u));

    // A 2D overlay can't show one specific depth slice, so visualize the
    // worst (max) per-cluster light count across the column's Z slices —
    // the number that determines this screen tile's peak shading cost.
    uint lightCount = 0u;
    for (uint slice = 0u; slice < countZ; ++slice)
    {
        uint clusterIndex = (slice * countY + tileCoord.y) * countX + tileCoord.x;
        lightCount = max(lightCount, fplusGrid[clusterIndex].y);
    }

    // Heatmap normalization (0 lights = blue, 32+ lights = red)
    float t = float(lightCount) / 32.0;
    vec3 heat = heatmap(t);

    // Draw cluster tile grid lines (fractional tile coordinate near an edge)
    vec2 tileFract = fract(tileCoordF);
    vec2 edgeWidth = fplus_TileScale.xy; // one pixel in tile units
    bool onEdge = (tileFract.x < edgeWidth.x || tileFract.y < edgeWidth.y);
    float edgeFactor = onEdge ? 0.6 : 0.0;

    // Semi-transparent overlay blended with the scene
    float alpha = (lightCount > 0u) ? 0.35 : 0.08;
    alpha = max(alpha, edgeFactor);

    o_Color = vec4(heat, alpha);
}
