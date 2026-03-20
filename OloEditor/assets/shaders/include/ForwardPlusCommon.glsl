// =============================================================================
// ForwardPlusCommon.glsl — Shared include for Forward+ tile-based shading
//
// Include this in fragment shaders that need to read per-tile light lists.
// Provides the SSBO declarations and helper functions for iterating
// over the lights assigned to the current fragment's tile.
// =============================================================================

#ifndef FORWARD_PLUS_COMMON_GLSL
#define FORWARD_PLUS_COMMON_GLSL

// ---------------------------------------------------------------------------
// GPU light structures (must match C++ GPUPointLight / GPUSpotLight)
// ---------------------------------------------------------------------------

struct FPlusPointLight
{
    vec4 PositionAndRadius;    // xyz = world pos, w = range
    vec4 ColorAndIntensity;    // xyz = color, w = intensity
};

struct FPlusSpotLight
{
    vec4 PositionAndRadius;    // xyz = world pos, w = range
    vec4 DirectionAndAngle;    // xyz = dir, w = cos(outerAngle)
    vec4 ColorAndIntensity;    // xyz = color, w = intensity
    vec4 SpotParams;           // x = cos(innerAngle), y = falloff, zw = 0
};

// ---------------------------------------------------------------------------
// SSBOs — bindings match ShaderBindingLayout constants
// ---------------------------------------------------------------------------

layout(std430, binding = 9)  readonly buffer FPlusPointLightBuf  { FPlusPointLight fplusPointLights[]; };
layout(std430, binding = 10) readonly buffer FPlusSpotLightBuf   { FPlusSpotLight  fplusSpotLights[];  };
layout(std430, binding = 11) readonly buffer FPlusLightIndexBuf  { uint fplusLightIndices[];           };
layout(std430, binding = 12) readonly buffer FPlusLightGridBuf   { uvec2 fplusGrid[];                  };

// ---------------------------------------------------------------------------
// UBO — matches C++ ForwardPlusUBO (binding = 25)
// ---------------------------------------------------------------------------

layout(std140, binding = 25) uniform ForwardPlusParams {
    uvec4 fplus_Params; // x = TileSizePixels, y = TileCountX, z = Enabled (0/1), w = reserved
};

// ---------------------------------------------------------------------------
// Helper: get tile (offset, count) for the current fragment
// ---------------------------------------------------------------------------

uvec2 fplusGetTileData()
{
    uint tileSizePixels = fplus_Params.x;
    uint tileCountX     = fplus_Params.y;
    ivec2 tileCoord = ivec2(gl_FragCoord.xy) / ivec2(tileSizePixels);
    uint tileIndex = uint(tileCoord.y) * tileCountX + uint(tileCoord.x);
    return fplusGrid[tileIndex];
}

// ---------------------------------------------------------------------------
// Evaluate all Forward+ point & spot lights for a given surface point
//
// Returns the accumulated radiance from all lights in the current tile.
// Uses the same PBR BRDF functions from PBRCommon.glsl.
// ---------------------------------------------------------------------------

vec3 fplusEvaluateTileLights(vec3 N, vec3 V, vec3 worldPos,
                              vec3 albedo, float metallic, float roughness)
{
    uvec2 tileData = fplusGetTileData();
    uint offset = tileData.x;
    uint count  = tileData.y;

    vec3 Lo = vec3(0.0);

    for (uint i = 0; i < count; ++i)
    {
        uint packedIdx = fplusLightIndices[offset + i];

        // High bit set = spot light (see LightCulling.comp encoding)
        bool isSpot = (packedIdx & 0x80000000u) != 0u;
        uint idx = packedIdx & 0x7FFFFFFFu;

        if (!isSpot)
        {
            // Point light
            FPlusPointLight pl = fplusPointLights[idx];
            vec3 lightPos   = pl.PositionAndRadius.xyz;
            float range     = pl.PositionAndRadius.w;
            vec3 lightColor = pl.ColorAndIntensity.xyz * pl.ColorAndIntensity.w;

            vec3 L = lightPos - worldPos;
            float dist = length(L);
            if (dist > range) continue;
            L /= dist;

            // Smooth distance attenuation (UE4-style)
            float distRatio = dist / range;
            float atten = max(1.0 - distRatio * distRatio, 0.0);
            atten = atten * atten / (dist * dist + 1.0);

            vec3 radiance = lightColor * atten;
            Lo += cookTorranceBRDF(N, V, L, albedo, metallic, roughness) * radiance;
        }
        else
        {
            // Spot light
            FPlusSpotLight sl = fplusSpotLights[idx];
            vec3 lightPos     = sl.PositionAndRadius.xyz;
            float range       = sl.PositionAndRadius.w;
            vec3 lightDir     = sl.DirectionAndAngle.xyz;
            float cosOuter    = sl.DirectionAndAngle.w;
            vec3 lightColor   = sl.ColorAndIntensity.xyz * sl.ColorAndIntensity.w;
            float cosInner    = sl.SpotParams.x;

            vec3 L = lightPos - worldPos;
            float dist = length(L);
            if (dist > range) continue;
            L /= dist;

            // Spot cone attenuation
            float cosTheta = dot(-L, normalize(lightDir));
            float spotAtten = clamp((cosTheta - cosOuter) / max(cosInner - cosOuter, EPSILON), 0.0, 1.0);

            // Distance attenuation
            float distRatio = dist / range;
            float distAtten = max(1.0 - distRatio * distRatio, 0.0);
            distAtten = distAtten * distAtten / (dist * dist + 1.0);

            vec3 radiance = lightColor * distAtten * spotAtten;
            Lo += cookTorranceBRDF(N, V, L, albedo, metallic, roughness) * radiance;
        }
    }

    return Lo;
}

#endif // FORWARD_PLUS_COMMON_GLSL
