// =============================================================================
// DeferredLightingShared.glsl — shared PBR shading body for deferred lighting.
// Consumed by DeferredLighting.glsl (non-MSAA) and DeferredLighting_MSAA.glsl.
//
// Prerequisites (the caller must declare these identically-typed resources
// BEFORE including this file):
//   - CameraMatrices UBO at binding 0
//   - MultiLightBuffer UBO at binding 5
//   - ShadowData UBO at binding 6
//   - MotionBlurMatrices UBO at binding 8
//   - DeferredLightingControls UBO at binding 30
//   - samplerCube u_IrradianceMap (10), u_PrefilterMap (11)
//   - sampler2D u_BRDFLutMap (12)
//   - sampler2DArrayShadow u_ShadowMapCSM (8), u_ShadowAtlas (13)
//   - sampler2DArray u_ShadowMapCSMRaw (33), u_ShadowAtlasRaw (34) — PCSS blocker search
//   - include/PBRCommon.glsl, include/LightProbeSampling.glsl,
//     include/ForwardPlusCommon.glsl (with FPLUS_ATLAS_SHADOWS defined, AFTER
//     the ShadowData block + atlas samplers)
//
// G-Buffer sampling is done by the caller (sampler2D vs sampler2DMS); this
// file consumes only the already-unpacked per-pixel values.
// =============================================================================

#ifndef DEFERRED_LIGHTING_SHARED_GLSL
#define DEFERRED_LIGHTING_SHARED_GLSL

vec3 OctDecodeGB(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

vec3 ReconstructWorldPosGB(vec2 uv, float depthNDC)
{
    float zNDC = depthNDC * 2.0 - 1.0;
    vec4 clipPos = vec4(uv * 2.0 - 1.0, zNDC, 1.0);
    vec4 worldPos = u_InverseViewProjection * clipPos;
    return worldPos.xyz / worldPos.w;
}

// Apply cascade-debug tint on top of the lit color. Shared between variants.
vec3 ApplyCascadeDebug(vec3 color, vec3 worldPos)
{
    vec4 viewSpacePos = u_View * vec4(worldPos, 1.0);
    float viewDepth = -viewSpacePos.z;
    vec3 cascadeColors[4] = vec3[4](
        vec3(1.0, 0.2, 0.2),
        vec3(0.2, 1.0, 0.2),
        vec3(0.2, 0.2, 1.0),
        vec3(1.0, 1.0, 0.2));
    int cascadeIdx = 3;
    for (int c = 0; c < 4; ++c)
    {
        if (viewDepth < u_CascadePlaneDistances[c])
        {
            cascadeIdx = c;
            break;
        }
    }
    return mix(color, cascadeColors[cascadeIdx], 0.3);
}

// Full deferred lighting evaluation for a single shaded point. Returns the
// linear HDR color (caller writes it to the output attachment).
//
// The `emissiveFlags` parameter carries the emissive colour in .rgb and a
// material-flags packed float in .a. Currently a single bit is consumed:
// `emissiveFlags.a > 0.5` marks the fragment as **unlit**, causing this
// routine to skip all direct + ambient + probe + IBL work and return the
// raw emissive colour directly. This is the mechanism G-Buffer overlay
// variants (Skybox_GBuffer, InfiniteGrid_GBuffer, LightCube_GBuffer) use
// to opt out of PBR shading while still participating in motion-vector
// + depth writes alongside the rest of the G-Buffer pipeline.
vec3 ComputeDeferredLit(
    vec3 albedo, float metallic,
    vec3 N, float roughness, float ao,
    vec4 emissiveFlags, vec3 worldPos)
{
    vec3 emissive = emissiveFlags.rgb;
    if (emissiveFlags.a > 0.5)
    {
        // Unlit pass-through — skybox, editor grid, light-cube billboards
        // etc. sit inside the G-Buffer but do not want PBR shading applied.
        return emissive;
    }

    vec3 V = normalize(u_CameraPosition - worldPos);

    bool enableIBL     = u_DeferredControls.x > 0.5;
    bool enableProbes  = u_DeferredControls.y > 0.5;
    float iblIntensity = u_DeferredControls.z;
    bool cascadeDebug  = u_DeferredControls.w > 0.5;

    vec3 Lo = vec3(0.0);

    bool fplusActive = (fplus_Params.z != 0u);
    if (fplusActive)
    {
        float fplusViewDepth = -(u_View * vec4(worldPos, 1.0)).z;
        Lo += fplusEvaluateTileLights(N, V, worldPos, albedo, metallic, roughness, fplusViewDepth);
    }

    int loopCount = fplusActive ? min(u_DirectionalLightCount, MAX_LIGHTS)
                                : min(u_LightCount, MAX_LIGHTS);
    for (int i = 0; i < loopCount; ++i)
    {
        int lightType = int(u_Lights[i].position.w);
        vec3 lightContrib = calculateLightContribution(u_Lights[i], N, V, albedo, metallic, roughness, worldPos);

        if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
            vec4 viewSpacePos = u_View * vec4(worldPos, 1.0);
            float viewDepth = viewSpacePos.z;
            float shadow = calculateCascadedShadowFactorCSM(
                u_ShadowMapCSM,
                u_ShadowMapCSMRaw,
                worldPos,
                viewDepth,
                u_DirectionalLightSpaceMatrices,
                u_CascadePlaneDistances,
                u_ShadowParams,
                u_ShadowMapResolution,
                u_SoftShadowMode);
            lightContrib *= shadow;
        }
        else if (lightType == SPOT_LIGHT)
        {
            // Spot light shadows come from the light's shadow-atlas entry
            // (issue #435); direction.w carries the entry index (-1 = none).
            int atlasEntry = int(u_Lights[i].direction.w);
            if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
            {
                float shadow = calculateAtlasEntryShadow(
                    worldPos,
                    u_AtlasEntryMatrices[atlasEntry],
                    u_AtlasEntryScaleOffset[atlasEntry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_ShadowParams.x,
                    u_AtlasResolution,
                    u_SoftShadowMode,
                    u_ShadowParams.z);
                lightContrib *= shadow;
            }
        }
        else if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
        {
            // Sphere area lights shadow from their centre (the representative
            // point), so both types share the point path: direction.w carries
            // the BASE atlas entry of the 6 cube-face tiles (issue #435).
            int baseEntry = int(u_Lights[i].direction.w);
            if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
            {
                vec3 lightPos = u_Lights[i].position.xyz;
                int entry = baseEntry + atlasCubeFace(worldPos - lightPos);
                float shadow = calculateAtlasEntryShadow(
                    worldPos,
                    u_AtlasEntryMatrices[entry],
                    u_AtlasEntryScaleOffset[entry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_ShadowParams.x,
                    u_AtlasResolution,
                    0, // PCF only on cube faces (matches the old cubemap path)
                    u_ShadowParams.z);
                lightContrib *= shadow;
            }
        }

        Lo += lightContrib;
    }

    vec3 ambient = vec3(0.0);
    if (enableProbes && enableIBL)
    {
        // Issue #632: unified probe sampling — realtime DDGI atlases when a
        // Realtime/Hybrid volume is bound, baked SH otherwise.
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(worldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
        {
            ambient = calculateCombinedAmbient(probeIrradiance, N, V, albedo, metallic, roughness,
                                               u_PrefilterMap, u_BRDFLutMap);
            ambient *= iblIntensity;
        }
        else
        {
            ambient = calculateIBL(N, V, albedo, metallic, roughness,
                                   u_IrradianceMap, u_PrefilterMap, u_BRDFLutMap);
            ambient *= iblIntensity;
        }
    }
    else if (enableProbes)
    {
        vec3 probeIrradiance = sampleProbeVolumeIrradiance(worldPos, N, V);
        if (dot(probeIrradiance, probeIrradiance) > 0.0)
            ambient = calculateLightProbeAmbient(probeIrradiance, albedo, metallic, roughness, N, V);
        else
            ambient = calculateSimpleAmbient(albedo, metallic, ao);
    }
    else if (enableIBL)
    {
        ambient = calculateIBL(N, V, albedo, metallic, roughness,
                               u_IrradianceMap, u_PrefilterMap, u_BRDFLutMap);
        ambient *= iblIntensity;
    }
    else
    {
        ambient = calculateSimpleAmbient(albedo, metallic, ao);
    }

    vec3 color = ambient * ao + Lo + emissive;

    if (cascadeDebug && u_DirectionalShadowEnabled != 0)
        color = ApplyCascadeDebug(color, worldPos);

    return color;
}

#endif // DEFERRED_LIGHTING_SHARED_GLSL
