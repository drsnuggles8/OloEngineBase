// =============================================================================
// DDGI_Relight.glsl — per-frame relight of the DDGI hit-point cache (#632)
//
// The Lumen-FinalLighting analog of ADR 0006: every frame, every ACTIVE
// probe's cached hit points are re-shaded with CURRENT shadowed direct
// lighting plus the PREVIOUS frame's probe irradiance at the hit point
// (infinite bounce), writing the radiance cache the irradiance blend then
// cosine-convolves. Fullscreen over the radiance atlas (optionally scissored
// to a tile-row window when RelightBudget > 0).
//
// This shader is DIFFUSE-ONLY, deliberately: the radiance cache is a
// view-independent quantity (there is no camera at a probe hit point), so the
// specular half of the BRDF is meaningless here — cookTorranceBRDF is NOT
// used. What IS reused from include/PBRCommon.glsl, so the cache stays
// photometrically consistent with the lit paths (see
// docs/agent-rules/light-path-photometric-parity.md):
//   - calculateAttenuation           (point/spot distance falloff)
//   - calculateSpotIntensity         (spot cone factor)
//   - calculateCascadedShadowFactorCSM (directional CSM visibility)
//   - calculateAtlasEntryShadow      (spot/point shadow-atlas visibility)
//   - atlasCubeFace                  (point-light cube-face entry selection)
//   - the DIRECTIONAL/POINT/SPOT/SPHERE_AREA_LIGHT type tags + light loop
//     shape of DeferredLightingShared.glsl (UBO brute-force path)
// The sphere-area diffuse term mirrors the diffuse half of
// calculateSphereAreaLightContribution (centre direction + Karis distance
// falloff); its representative-point specular half is skipped for the same
// view-independence reason.
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

#include "include/PBRCommon.glsl"
#include "include/DDGICommon.glsl"

// Camera UBO (binding 0) — the REAL scene camera (DDGIProbeUpdatePass restores
// it after the capture stage). Needed for CSM cascade selection: cascades are
// fit to the main view frustum, so hit points outside it simply fall through
// to "lit" exactly like the froxel fog's out-of-frustum samples.
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 _camPrevViewProjectionPad;
    vec3 u_RenderOrigin;
    float _padding1;
};

// MultiLight UBO (binding 5) — the same brute-force light array the lit
// shaders consume (positions are render-origin-relative, issue #429, matching
// the render-relative hit positions reconstructed below).
layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int u_MaxLights;
    int u_ShadowCasterCount;
    int u_DirectionalLightCount;
    LightData u_Lights[MAX_LIGHTS];
};

// Shadow UBO (binding 6) — identical declaration to DeferredLighting.glsl.
layout(std140, binding = 6) uniform ShadowData {
    mat4 u_DirectionalLightSpaceMatrices[4];
    vec4 u_CascadePlaneDistances;
    vec4 u_ShadowParams;
    mat4 u_AtlasEntryMatrices[48];
    vec4 u_AtlasEntryScaleOffset[48];
    int u_DirectionalShadowEnabled;
    int u_AtlasEntryCount;
    int u_ShadowMapResolution;
    int u_AtlasResolution;
    int u_CascadeDebugEnabled;
    int u_SoftShadowMode;
    int _shadowPad1;
    int _shadowPad2;
};

// Pass-local hit-cache inputs (units 0-5, bound by DDGIProbeUpdatePass) — the
// DDGI atlases are deliberately NOT taken from the global slots 56-58 here
// (no DDGI_GLOBAL_SAMPLERS): the bounce term must read the PREVIOUS
// irradiance atlas, not the one being published this frame.
layout(binding = 0) uniform sampler2D u_HitAlbedo;      // rgb albedo, a flag
layout(binding = 1) uniform sampler2D u_HitGeo;         // rg octN, b dist (<0 sky), a flag
layout(binding = 2) uniform sampler2D u_PrevIrradiance; // previous frame's blended atlas
layout(binding = 3) uniform sampler2D u_CurrVisibility; // current (post-blend) Chebyshev atlas
layout(binding = 4) uniform sampler2D u_ProbeData;      // xyz offsetN, w state
// Global environment cubemap for sky-miss texels, at the engine's canonical
// samplerCube slot (TEX_ENVIRONMENT = 9) so the cross-shader sampler-type
// consistency contract holds; black-cubemap fallback when no scene env exists.
layout(binding = 9) uniform samplerCube u_EnvironmentCube;

// Shadow maps at the units the PBRCommon evaluators expect.
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM;
layout(binding = 13) uniform sampler2DArrayShadow u_ShadowAtlas;
layout(binding = 33) uniform sampler2DArray u_ShadowMapCSMRaw;
layout(binding = 34) uniform sampler2DArray u_ShadowAtlasRaw;

layout(location = 0) in vec2 v_TexCoord;

layout(location = 0) out vec4 o_Radiance;

void main()
{
    int t = u_DDGIHitCacheTexels;
    ivec2 atlasTexel = ivec2(gl_FragCoord.xy);
    ivec2 tile = atlasTexel / t;
    ivec2 local = atlasTexel % t;
    ivec3 dims = u_DDGIGridDimensions.xyz;
    ivec3 coord = ivec3(tile.x, tile.y % dims.y, tile.y / dims.y);

    vec4 pdata = texelFetch(u_ProbeData, tile, 0);
    // Only ACTIVE probes relight — Uncaptured (0) and Inactive (2) keep their
    // old radiance texels (discard leaves the attachment untouched).
    if (pdata.w < 0.5 || pdata.w > 1.5)
    {
        discard;
    }

    vec4 geo = texelFetch(u_HitGeo, atlasTexel, 0);
    vec3 texelDir = ddgiTexelDirection(local, t);

    // Backface hit: zero radiance (RTXGI/Lumen feedback guard — a probe must
    // never gather light from inside geometry).
    if (geo.a > 0.25 && geo.a < 0.75)
    {
        o_Radiance = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Sky miss: environment radiance along the fixed texel direction.
    if (geo.b < 0.0)
    {
        o_Radiance = vec4(textureLod(u_EnvironmentCube, texelDir, 0.0).rgb, 1.0);
        return;
    }

    // Frontface hit — reconstruct the render-relative hit point. The DDGI UBO
    // bounds are render-origin-relative, so ddgiProbeWorldPosition already
    // yields a render-relative probe position, consistent with the
    // render-relative light positions in the MultiLight UBO (issue #429).
    vec3 probeRelPos = ddgiProbeWorldPosition(coord, pdata.xyz);
    vec3 hitPos = probeRelPos + texelDir * geo.b;
    vec3 N = ddgiOctDecode(geo.rg);
    vec3 albedo = texelFetch(u_HitAlbedo, atlasTexel, 0).rgb;

    // --- Direct diffuse irradiance: brute-force MultiLight UBO loop ---
    vec3 directE = vec3(0.0);
    int loopCount = min(u_LightCount, MAX_LIGHTS);
    for (int i = 0; i < loopCount; ++i)
    {
        int lightType = int(u_Lights[i].position.w);
        vec3 L;
        float attenuation = 1.0;

        if (lightType == DIRECTIONAL_LIGHT)
        {
            L = normalize(-u_Lights[i].direction.xyz);
        }
        else if (lightType == POINT_LIGHT)
        {
            L = normalize(u_Lights[i].position.xyz - hitPos);
            attenuation = calculateAttenuation(u_Lights[i].position.xyz, hitPos, u_Lights[i].attenuationParams);
        }
        else if (lightType == SPOT_LIGHT)
        {
            L = normalize(u_Lights[i].position.xyz - hitPos);
            attenuation = calculateAttenuation(u_Lights[i].position.xyz, hitPos, u_Lights[i].attenuationParams);
            attenuation *= calculateSpotIntensity(L, u_Lights[i].direction.xyz, u_Lights[i].spotParams);
        }
        else if (lightType == SPHERE_AREA_LIGHT)
        {
            // Diffuse half of calculateSphereAreaLightContribution: centre
            // direction + the Karis smooth distance falloff (the
            // representative-point specular half is view-dependent, skipped).
            vec3 toLight = u_Lights[i].position.xyz - hitPos;
            float dist = length(toLight);
            float range = u_Lights[i].attenuationParams.w;
            if (dist > range)
            {
                continue;
            }
            L = toLight / max(dist, EPSILON);
            float distRatio = dist / max(range, EPSILON);
            float distAtten = max(1.0 - distRatio * distRatio, 0.0);
            attenuation = distAtten * distAtten / (dist * dist + 1.0);
        }
        else
        {
            continue;
        }

        if (attenuation <= EPSILON)
        {
            continue;
        }
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= EPSILON)
        {
            continue;
        }

        // Shadow visibility — identical evaluator calls + entry indexing to
        // DeferredLightingShared.glsl's UBO path.
        float shadow = 1.0;
        if (lightType == DIRECTIONAL_LIGHT && u_DirectionalShadowEnabled != 0)
        {
            float viewDepth = (u_View * vec4(hitPos, 1.0)).z;
            shadow = calculateCascadedShadowFactorCSM(
                u_ShadowMapCSM,
                u_ShadowMapCSMRaw,
                hitPos,
                viewDepth,
                u_DirectionalLightSpaceMatrices,
                u_CascadePlaneDistances,
                u_ShadowParams,
                u_ShadowMapResolution,
                u_SoftShadowMode);
        }
        else if (lightType == SPOT_LIGHT)
        {
            int atlasEntry = int(u_Lights[i].direction.w);
            if (atlasEntry >= 0 && atlasEntry < u_AtlasEntryCount)
            {
                shadow = calculateAtlasEntryShadow(
                    hitPos,
                    u_AtlasEntryMatrices[atlasEntry],
                    u_AtlasEntryScaleOffset[atlasEntry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_ShadowParams.x,
                    u_AtlasResolution,
                    u_SoftShadowMode,
                    u_ShadowParams.z);
            }
        }
        else if (lightType == POINT_LIGHT || lightType == SPHERE_AREA_LIGHT)
        {
            int baseEntry = int(u_Lights[i].direction.w);
            if (baseEntry >= 0 && baseEntry + 5 < u_AtlasEntryCount)
            {
                int entry = baseEntry + atlasCubeFace(hitPos - u_Lights[i].position.xyz);
                shadow = calculateAtlasEntryShadow(
                    hitPos,
                    u_AtlasEntryMatrices[entry],
                    u_AtlasEntryScaleOffset[entry],
                    u_ShadowAtlas,
                    u_ShadowAtlasRaw,
                    u_ShadowParams.x,
                    u_AtlasResolution,
                    0, // PCF only on cube faces (matches the lit paths)
                    u_ShadowParams.z);
            }
        }

        directE += u_Lights[i].color.rgb * u_Lights[i].color.w * attenuation * shadow * NdotL;
    }

    // --- Infinite bounce: previous frame's probe irradiance at the hit point.
    // Pass-local samplers on purpose (NOT the global 56-58 slots): the bounce
    // must read the PREVIOUS atlas while the current one is being built.
    // viewDir = -texelDir: the "viewer" at a hit point is the probe itself.
    vec3 bounceE = ddgiSampleIrradiance(u_PrevIrradiance, u_CurrVisibility, u_ProbeData,
                                        hitPos, N, -texelDir);

    // Diffuse exitance with the energy-conservation albedo clamp (ADR 0006:
    // keeps the infinite-bounce feedback contractive).
    vec3 radiance = (min(albedo, vec3(u_DDGIEnergyConservation)) / PI) * (directE + bounceE);
    o_Radiance = vec4(radiance, 1.0);
}
