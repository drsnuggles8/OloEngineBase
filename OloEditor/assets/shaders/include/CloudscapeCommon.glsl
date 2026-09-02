// =============================================================================
// CloudscapeCommon.glsl — shared volumetric-cloud density field (issue #633)
//
// One home for the cloud density function so the raymarch pass
// (PostProcess_Cloudscape.glsl), the cloud-shadow compute
// (CloudShadow_Generate.comp), and any future consumer evaluate the SAME
// field (the light-path-photometric-parity discipline applied to media).
//
// Requires NoiseCommon.glsl for remap(); expects the includer to bind:
//   TEX_CLOUD_BASE_NOISE   (56) sampler3D u_CloudBaseNoise    (RGBA8, repeat)
//   TEX_CLOUD_DETAIL_NOISE (57) sampler3D u_CloudDetailNoise  (RGBA8, repeat)
//   TEX_CLOUD_WEATHER_MAP  (58) sampler2D u_CloudWeatherMap   (RGBA8: R=coverage, G=type, B=wetness)
// and the CloudscapeData UBO at binding 52 (ShaderBindingLayout::UBO_CLOUDSCAPE,
// CPU twin CloudscapeUBO in CloudscapeRenderPass.h).
// =============================================================================

#ifndef CLOUDSCAPE_COMMON_GLSL
#define CLOUDSCAPE_COMMON_GLSL

layout(binding = 59) uniform sampler3D u_CloudBaseNoise;
layout(binding = 60) uniform sampler3D u_CloudDetailNoise;
layout(binding = 61) uniform sampler2D u_CloudWeatherMap;

// Must match ShaderBindingLayout::UBO_CLOUDSCAPE (53) and the CPU-side
// CloudscapeUBO struct (CloudscapeRenderPass.h) member for member.
layout(std140, binding = 53) uniform CloudscapeData {
    vec4 u_CloudLayer;      // x = bottom (m), y = top (m), z = 1/(top-bottom), w = density scale
    vec4 u_CloudField;      // x = coverage [0,1], y = type blend, z = erosion, w = cloud wetness
    vec4 u_CloudWind;       // xy = accumulated wind offset (world m, xz), z = anim scale, w = time (s)
    vec4 u_CloudMap;        // x = 1/weather-map extent (1/m), y = max steps, z = light steps, w = phase g
    vec4 u_CloudLight;      // x = sun scale, y = ambient scale, z = multi-scatter, w = powder
    vec4 u_CloudMisc;       // x = temporal blend, y = frame index, z = shadow strength, w = enabled
    vec4 u_CloudSunDir;     // xyz = toward the active directional body (sun/moon), w = night blend
    vec4 u_CloudSunColor;   // rgb = directional colour * intensity, w = unused
    vec4 u_CloudAmbient;    // rgb = ambient (sky) colour estimate, w = unused
};

// sigma_t per unit density — the one extinction scale every consumer of the
// field (raymarch, cloud shadow map, froxel mirror) must share so ground
// shadows and in-cloud darkness agree photometrically.
const float kCloudExtinction = 0.03;

// Height fraction inside the layer: 0 at the bottom, 1 at the top.
float cloudHeightFraction(float worldY)
{
    return clamp((worldY - u_CloudLayer.x) * u_CloudLayer.z, 0.0, 1.0);
}

// Two-layer vertical density profile. Stratus: thin, flat, bottom-heavy.
// Cumulus: tall, rounded, mass in the lower-middle of the layer. The type
// value (0 = stratus, 1 = cumulus) blends the two gradients — the standard
// Nubis/Schneider height-gradient approach.
float cloudHeightGradient(float heightFrac, float cloudType)
{
    // smoothstep up from the base, down toward each profile's ceiling.
    float stratus = smoothstep(0.0, 0.08, heightFrac) * (1.0 - smoothstep(0.18, 0.32, heightFrac));
    float cumulus = smoothstep(0.0, 0.14, heightFrac) * (1.0 - smoothstep(0.55, 0.95, heightFrac));
    return mix(stratus, cumulus, clamp(cloudType, 0.0, 1.0));
}


// ---------------------------------------------------------------------------
// Deterministic texture filtering (issue #1008)
//
// GL leaves the PRECISION of texture filter weights implementation-defined, and
// drivers commonly use 8-bit fixed point rather than fp32. That is invisible in
// most shading, where the sampled value is used proportionally. It is not
// invisible here: cloudDensity() puts every sample through a threshold chain
// (smoothstep(0.28, 0.75, ...) then two hard-edged remap() calls), which turns a
// sub-ULP input difference into a BINARY cloud/no-cloud outcome for that sample.
// Along a grazing ray the march takes ~48 such samples over 8-18 km, so the flips
// accumulate into a visible difference; overhead the path is short and they do not.
//
// Measured: AMD and NVIDIA converge to horizon-band luma 21 (Overcast) to 81
// (Storm) apart, each individually stable to 0.2 luma under 8x the samples and 8x
// the temporal warm-up. Doing the interpolation in fp32 ourselves removes the
// driver's filter precision from that chain.
//
// GL_REPEAT is reproduced explicitly because texelFetch does not wrap.
// ---------------------------------------------------------------------------
ivec3 cloudWrap3(ivec3 c, ivec3 size)
{
    return ((c % size) + size) % size;
}

ivec2 cloudWrap2(ivec2 c, ivec2 size)
{
    return ((c % size) + size) % size;
}

vec4 cloudTexture3DDeterministic(sampler3D tex, vec3 uvw)
{
    ivec3 size = textureSize(tex, 0);
    vec3 coord = uvw * vec3(size) - 0.5;
    vec3 baseF = floor(coord);
    vec3 f = coord - baseF;
    ivec3 i0 = ivec3(baseF);

    vec4 c000 = texelFetch(tex, cloudWrap3(i0 + ivec3(0, 0, 0), size), 0);
    vec4 c100 = texelFetch(tex, cloudWrap3(i0 + ivec3(1, 0, 0), size), 0);
    vec4 c010 = texelFetch(tex, cloudWrap3(i0 + ivec3(0, 1, 0), size), 0);
    vec4 c110 = texelFetch(tex, cloudWrap3(i0 + ivec3(1, 1, 0), size), 0);
    vec4 c001 = texelFetch(tex, cloudWrap3(i0 + ivec3(0, 0, 1), size), 0);
    vec4 c101 = texelFetch(tex, cloudWrap3(i0 + ivec3(1, 0, 1), size), 0);
    vec4 c011 = texelFetch(tex, cloudWrap3(i0 + ivec3(0, 1, 1), size), 0);
    vec4 c111 = texelFetch(tex, cloudWrap3(i0 + ivec3(1, 1, 1), size), 0);

    vec4 c00 = mix(c000, c100, f.x);
    vec4 c10 = mix(c010, c110, f.x);
    vec4 c01 = mix(c001, c101, f.x);
    vec4 c11 = mix(c011, c111, f.x);
    return mix(mix(c00, c10, f.y), mix(c01, c11, f.y), f.z);
}

vec4 cloudTexture2DDeterministic(sampler2D tex, vec2 uv)
{
    ivec2 size = textureSize(tex, 0);
    vec2 coord = uv * vec2(size) - 0.5;
    vec2 baseF = floor(coord);
    vec2 f = coord - baseF;
    ivec2 i0 = ivec2(baseF);

    vec4 c00 = texelFetch(tex, cloudWrap2(i0 + ivec2(0, 0), size), 0);
    vec4 c10 = texelFetch(tex, cloudWrap2(i0 + ivec2(1, 0), size), 0);
    vec4 c01 = texelFetch(tex, cloudWrap2(i0 + ivec2(0, 1), size), 0);
    vec4 c11 = texelFetch(tex, cloudWrap2(i0 + ivec2(1, 1), size), 0);
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

// Weather-map sample for a world position (xz tiling over the map extent).
// R = coverage multiplier, G = cloud type, B = local wetness.
vec3 cloudWeatherSample(vec3 worldPos)
{
    vec2 uv = worldPos.xz * u_CloudMap.x;
    return cloudTexture2DDeterministic(u_CloudWeatherMap, uv).rgb;
}

// Full density evaluation at a world position. `cheap` skips the detail
// erosion (used by the shadow map + the raymarch's light steps).
float cloudDensity(vec3 worldPos, bool cheap)
{
    float heightFrac = cloudHeightFraction(worldPos.y);
    if (heightFrac <= 0.0 || heightFrac >= 1.0)
        return 0.0;

    // Wind advection: the field scrolls with the accumulated wind offset,
    // with a slight per-height skew so tops shear ahead of bases.
    vec3 samplePos = worldPos;
    samplePos.xz += u_CloudWind.xy * (1.0 + 0.35 * heightFrac);

    vec3 weather = cloudWeatherSample(samplePos);
    // Sharpen the weather map's coverage channel into genuinely clear and
    // genuinely cloudy regions (the procedural map's R hovers mid-range, so a
    // plain multiply never fully clears the sky — tuned live: without the
    // smoothstep, coverage 0.5 rendered a solid deck with no blue gaps).
    float weatherCoverage = smoothstep(0.28, 0.75, weather.r);
    float coverage = clamp(u_CloudField.x * weatherCoverage * 2.0, 0.0, 1.0);
    if (coverage <= 1.0e-3)
        return 0.0;
    float cloudType = clamp(u_CloudField.y * weather.g * 2.0, 0.0, 1.0);

    float gradient = cloudHeightGradient(heightFrac, cloudType);
    if (gradient <= 1.0e-4)
        return 0.0;

    // Base shape: Perlin-Worley in R, three Worley octaves in GBA build the
    // low-frequency FBM that ERODES the base. The erosion floor is the FBM
    // itself (not fbm-1): remap's low edge must sit inside the Perlin-Worley
    // value range or every sample survives — the fbm-1 form compressed the
    // whole field into [0.65, 0.95] and no coverage threshold could carve
    // holes (found live via a shader probe, issue #633).
    const float kBaseNoiseScale = 1.0 / 4800.0; // one base-noise repeat every ~4.8 km
    vec4 baseNoise = cloudTexture3DDeterministic(u_CloudBaseNoise, samplePos * kBaseNoiseScale);
    float baseFbm = baseNoise.g * 0.625 + baseNoise.b * 0.25 + baseNoise.a * 0.125;
    float baseShape = remap(baseNoise.r, baseFbm * 0.85, 1.0, 0.0, 1.0);

    // Coverage remap on the UN-gradiented base shape: the horizontal
    // patchiness is thresholded first (the base shape spans ~[0.2, 0.9], so a
    // mid coverage keeps broken clouds instead of erasing everything), THEN
    // the vertical profile sculpts what survived. The 0.9 softener widens
    // the usable coverage range — without it nothing appears below ~0.6;
    // tuned live against the AtmosphereTest scene (0.75 gave a near-solid
    // deck at coverage 0.5).
    float coverageSignal = (1.0 - coverage) * 0.9;
    float shaped = remap(baseShape, coverageSignal, 1.0, 0.0, 1.0);
    shaped *= coverage * gradient;
    if (shaped <= 1.0e-4)
        return 0.0;

    if (!cheap)
    {
        // Detail erosion: high-frequency Worley eats the cloud edges; inverted
        // near the base for the wispy underside (Nubis detail formula).
        const float kDetailNoiseScale = 1.0 / 900.0;
        vec3 detail = cloudTexture3DDeterministic(u_CloudDetailNoise, samplePos * kDetailNoiseScale +
                                                    vec3(u_CloudWind.w * 0.005)).rgb;
        float detailFbm = detail.r * 0.625 + detail.g * 0.25 + detail.b * 0.125;
        float detailMod = mix(detailFbm, 1.0 - detailFbm, clamp(heightFrac * 5.0, 0.0, 1.0));
        shaped = remap(shaped, detailMod * u_CloudField.z * 0.5, 1.0, 0.0, 1.0);
    }

    return max(shaped, 0.0) * u_CloudLayer.w;
}

// Henyey-Greenstein phase function (normalized over the sphere).
float cloudPhaseHG(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (12.5663706 * denom * sqrt(max(denom, 1.0e-4)));
}

// Dual-lobe phase: forward silver-lining lobe + weak back lobe, blended —
// the usual cheap approximation of Mie scattering in clouds.
float cloudPhase(float cosTheta, float g)
{
    return mix(cloudPhaseHG(cosTheta, -0.25 * g), cloudPhaseHG(cosTheta, g), 0.7);
}

// Beer-Lambert with the powder darkening term (Schneider): approaching-white
// fluffy look on light-facing sides, dark creases where density is low along
// the light path.
float cloudBeerPowder(float opticalDepth, float powderStrength)
{
    float beer = exp(-opticalDepth);
    float powder = 1.0 - exp(-2.0 * opticalDepth);
    return beer * mix(1.0, powder, clamp(powderStrength, 0.0, 2.0) * 0.5);
}

// Ray/slab intersection with the cloud layer. Returns (tEnter, tExit) along
// the ray, or tEnter >= tExit when the layer is missed.
vec2 cloudLayerIntersect(vec3 rayOrigin, vec3 rayDir)
{
    float invY = 1.0 / (abs(rayDir.y) > 1.0e-5 ? rayDir.y : (rayDir.y >= 0.0 ? 1.0e-5 : -1.0e-5));
    float t0 = (u_CloudLayer.x - rayOrigin.y) * invY;
    float t1 = (u_CloudLayer.y - rayOrigin.y) * invY;
    float tEnter = min(t0, t1);
    float tExit = max(t0, t1);
    return vec2(max(tEnter, 0.0), max(tExit, 0.0));
}

#endif // CLOUDSCAPE_COMMON_GLSL
