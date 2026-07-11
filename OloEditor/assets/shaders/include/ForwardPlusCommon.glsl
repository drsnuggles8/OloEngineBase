// =============================================================================
// ForwardPlusCommon.glsl — Shared include for clustered (froxel) Forward+
// shading (issue #435).
//
// Include this in fragment shaders that need to read per-cluster light lists.
// Provides the SSBO declarations and helper functions for iterating over the
// lights assigned to the current fragment's froxel cluster.
//
// The cluster is selected from gl_FragCoord.xy (screen tile) plus the
// fragment's positive view-space depth (exponential Z slice), so callers pass
// viewDepth = -(u_View * vec4(worldPos, 1.0)).z.
// =============================================================================

#ifndef FORWARD_PLUS_COMMON_GLSL
#define FORWARD_PLUS_COMMON_GLSL

// ---------------------------------------------------------------------------
// GPU light structures (must match C++ GPUPointLight / GPUSpotLight / GPUSphereAreaLight)
// ---------------------------------------------------------------------------

struct FPlusPointLight
{
    vec4 PositionAndRadius;     // xyz = world pos, w = range
    vec4 ColorAndIntensity;     // xyz = color, w = intensity
    vec4 ShadowAndAttenuation;  // x = base atlas entry (float, -1 = none), y = quadratic attenuation coefficient, zw = reserved
};

struct FPlusSpotLight
{
    vec4 PositionAndRadius;    // xyz = world pos, w = range
    vec4 DirectionAndAngle;    // xyz = dir, w = cos(outerAngle)
    vec4 ColorAndIntensity;    // xyz = color, w = intensity
    vec4 SpotParams;           // x = cos(innerAngle), y = quadratic attenuation coefficient, z = atlas entry (float, -1 = none), w = 0
};

struct FPlusSphereAreaLight
{
    vec4 PositionAndRadius;    // xyz = world pos, w = emitter sphere radius
    vec4 ColorAndIntensity;    // xyz = color, w = intensity
    vec4 RangeAndPadding;      // x = range (falloff), y = base atlas entry (float, -1 = none), zw = reserved
};

// ---------------------------------------------------------------------------
// SSBOs — bindings match ShaderBindingLayout constants
// ---------------------------------------------------------------------------

layout(std430, binding = 9)  readonly buffer FPlusPointLightBuf       { FPlusPointLight fplusPointLights[];           };
layout(std430, binding = 10) readonly buffer FPlusSpotLightBuf        { FPlusSpotLight  fplusSpotLights[];            };
layout(std430, binding = 11) readonly buffer FPlusLightIndexBuf       { uint fplusLightIndices[];                     };
layout(std430, binding = 12) readonly buffer FPlusLightGridBuf        { uvec2 fplusGrid[];                            };
layout(std430, binding = 18) readonly buffer FPlusSphereAreaLightBuf  { FPlusSphereAreaLight fplusSphereAreaLights[]; };

// ---------------------------------------------------------------------------
// Packed light-index encoding written by LightCulling.comp
//
//   bits 31..30 — type tag: 0 = point, 1 = sphere area, 2 = spot
//   bits 29..0  — index into the corresponding SSBO
//
// The legacy spot encoding (0x80000000 high-bit) maps to type tag 2, so old
// shaders that only checked the top bit for "isSpot" stay correct as long as
// no sphere area lights are present. Once any are present, callers MUST use
// the 2-bit decoder below.
// ---------------------------------------------------------------------------
#define FPLUS_TYPE_TAG_POINT        0u
#define FPLUS_TYPE_TAG_SPHERE_AREA  1u
#define FPLUS_TYPE_TAG_SPOT         2u
#define FPLUS_TYPE_TAG_MASK         0xC0000000u
#define FPLUS_INDEX_MASK            0x3FFFFFFFu
#define FPLUS_TYPE_TAG_SHIFT        30u

// ---------------------------------------------------------------------------
// UBO — matches C++ ForwardPlusUBO (binding = 25)
// ---------------------------------------------------------------------------

layout(std140, binding = 25) uniform ForwardPlusParams {
    uvec4 fplus_Params;       // x = ClusterCountX, y = ClusterCountY, z = Enabled (0/1), w = ClusterCountZ
    vec4  fplus_TileScale;    // xy = clusterCount / screenSize, zw = unused
    vec4  fplus_DepthSlicing; // x = sliceScale, y = sliceBias, z = zNear, w = zFar
};

// ---------------------------------------------------------------------------
// Cluster lookup — screen tile from gl_FragCoord, exponential depth slice
// from the fragment's positive view-space depth. Mirrors ClusteredLighting.h
// and the cluster indexing in LightCulling.comp.
// ---------------------------------------------------------------------------

uint fplusClusterIndex(float viewDepth)
{
    uint countX = fplus_Params.x;
    uint countY = fplus_Params.y;
    uint countZ = fplus_Params.w;

    uvec2 tileCoord = uvec2(gl_FragCoord.xy * fplus_TileScale.xy);
    tileCoord = min(tileCoord, uvec2(countX - 1u, countY - 1u));

    float z = max(viewDepth, 0.005);
    int slice = int(floor(log2(z) * fplus_DepthSlicing.x + fplus_DepthSlicing.y));
    slice = clamp(slice, 0, int(countZ) - 1);

    return (uint(slice) * countY + tileCoord.y) * countX + tileCoord.x;
}

// Get cluster (offset, count) for the current fragment.
uvec2 fplusGetTileData(float viewDepth)
{
    return fplusGrid[fplusClusterIndex(viewDepth)];
}

// ---------------------------------------------------------------------------
// Evaluate all Forward+ point & spot lights for a given surface point
//
// Returns the accumulated radiance from all lights in the fragment's froxel
// cluster. Uses the same PBR BRDF functions from PBRCommon.glsl.
// viewDepth = -(u_View * vec4(worldPos, 1.0)).z (positive into the screen).
//
// When the includer defines FPLUS_ATLAS_SHADOWS (after declaring the
// ShadowData UBO + u_ShadowAtlas / u_ShadowAtlasRaw samplers), every culled
// light with a shadow-atlas entry is attenuated by its shadow (issue #435 —
// previously tile-culled lights were shadowless).
// ---------------------------------------------------------------------------

vec3 fplusEvaluateTileLights(vec3 N, vec3 V, vec3 worldPos,
                              vec3 albedo, float metallic, float roughness,
                              float viewDepth)
{
    uvec2 tileData = fplusGetTileData(viewDepth);
    uint offset = tileData.x;
    uint count  = tileData.y;

    vec3 Lo = vec3(0.0);

    for (uint i = 0u; i < count; ++i)
    {
        uint packedIdx = fplusLightIndices[offset + i];

        uint typeTag = (packedIdx >> FPLUS_TYPE_TAG_SHIFT) & 0x3u;
        uint idx     = packedIdx & FPLUS_INDEX_MASK;

        if (typeTag == FPLUS_TYPE_TAG_POINT)
        {
            FPlusPointLight pl = fplusPointLights[idx];
            vec3 lightPos   = pl.PositionAndRadius.xyz;
            float range     = pl.PositionAndRadius.w;
            vec3 lightColor = pl.ColorAndIntensity.xyz * pl.ColorAndIntensity.w;

            vec3 L = lightPos - worldPos;
            float dist = length(L);
            if (dist > range) continue;
            L /= dist;

            // Same falloff as the brute-force MultiLightUBO path (PBRCommon
            // calculateAttenuation, constant=1 / linear=0 / quadratic from the
            // component) so Forward / Forward+ / Deferred agree photometrically.
            float atten = calculateAttenuation(lightPos, worldPos,
                                               vec4(1.0, 0.0, pl.ShadowAndAttenuation.y, range));

            // NdotL cosine term — calculateLightContribution applies it after
            // the BRDF (cookTorranceBRDF itself does not include it); without
            // this the clustered path over-lights grazing surfaces and leaks
            // diffuse onto back faces.
            float NdotL = max(dot(N, L), 0.0);
            if (NdotL <= EPSILON || atten <= EPSILON) continue;

            vec3 radiance = lightColor * atten * NdotL;

#ifdef FPLUS_ATLAS_SHADOWS
            int baseEntry = int(pl.ShadowAndAttenuation.x);
            if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
            {
                int entry = baseEntry + atlasCubeFace(worldPos - lightPos);
                radiance *= calculateAtlasEntryShadow(
                    worldPos, u_AtlasEntryMatrices[entry], u_AtlasEntryScaleOffset[entry],
                    u_ShadowAtlas, u_ShadowAtlasRaw,
                    u_ShadowParams.x, u_AtlasResolution, 0, u_ShadowParams.z);
            }
#endif

            Lo += cookTorranceBRDF(N, V, L, albedo, metallic, roughness) * radiance;
        }
        else if (typeTag == FPLUS_TYPE_TAG_SPHERE_AREA)
        {
            FPlusSphereAreaLight sl = fplusSphereAreaLights[idx];
            vec3 lightPos      = sl.PositionAndRadius.xyz;
            float sphereRadius = sl.PositionAndRadius.w;
            vec3 lightColor    = sl.ColorAndIntensity.xyz;
            float intensity    = sl.ColorAndIntensity.w;
            float range        = sl.RangeAndPadding.x;

            vec3 contribution = calculateSphereAreaLightContribution(N, V, lightPos, sphereRadius,
                                                                     lightColor, intensity, range,
                                                                     albedo, metallic, roughness, worldPos);

#ifdef FPLUS_ATLAS_SHADOWS
            int baseEntry = int(sl.RangeAndPadding.y);
            if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
            {
                int entry = baseEntry + atlasCubeFace(worldPos - lightPos);
                contribution *= calculateAtlasEntryShadow(
                    worldPos, u_AtlasEntryMatrices[entry], u_AtlasEntryScaleOffset[entry],
                    u_ShadowAtlas, u_ShadowAtlasRaw,
                    u_ShadowParams.x, u_AtlasResolution, 0, u_ShadowParams.z);
            }
#endif

            Lo += contribution;
        }
        else // FPLUS_TYPE_TAG_SPOT
        {
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

            // Cone + distance falloff via the same PBRCommon helpers the
            // brute-force MultiLightUBO path uses (calculateSpotIntensity
            // squares the cone ramp; calculateAttenuation is constant=1 /
            // linear=0 / quadratic) so the paths agree photometrically.
            float spotAtten = calculateSpotIntensity(L, lightDir,
                                                     vec4(cosInner, cosOuter, 0.0, 0.0));
            float distAtten = calculateAttenuation(lightPos, worldPos,
                                                   vec4(1.0, 0.0, sl.SpotParams.y, range));

            // NdotL cosine term — see the point-light branch above.
            float NdotL = max(dot(N, L), 0.0);
            if (NdotL <= EPSILON || distAtten * spotAtten <= EPSILON) continue;

            vec3 radiance = lightColor * distAtten * spotAtten * NdotL;

#ifdef FPLUS_ATLAS_SHADOWS
            int atlasEntry = int(sl.SpotParams.z);
            if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
            {
                radiance *= calculateAtlasEntryShadow(
                    worldPos, u_AtlasEntryMatrices[atlasEntry], u_AtlasEntryScaleOffset[atlasEntry],
                    u_ShadowAtlas, u_ShadowAtlasRaw,
                    u_ShadowParams.x, u_AtlasResolution, u_SoftShadowMode, u_ShadowParams.z);
            }
#endif

            Lo += cookTorranceBRDF(N, V, L, albedo, metallic, roughness) * radiance;
        }
    }

    return Lo;
}

#endif // FORWARD_PLUS_COMMON_GLSL
