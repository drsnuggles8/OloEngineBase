// =============================================================================
// SphereAreaLightVisualProbe.glsl
//
// Four roughness columns under three light evaluators: raster representative
// point, punctual control, and solid-angle quadrature over the sphere emitter.
// The quadrature uses the emitter view, sampler, density and BRDF shared by
// ReSTIR DI and the GPU reference tracer. It runs without ray queries because
// the analytical scene has no occluders.
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

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/PBRCommon.glsl"
#include "include/SphereLightSampling.glsl"

// Image layout constants — must match the C++ harness in
// SphereAreaLightVisualTest.cpp. Total image is kColumns x 2 cells.
const int kColumns = 4;
const float kRoughnessByColumn[4] = float[4](0.05, 0.25, 0.5, 0.85);

// Sphere & camera setup (cell-local view space, treating view space as
// world space — no model matrix needed for a unit sphere at the cell origin).
const vec3 kCameraPos = vec3(0.0, 0.0, 3.0);
const float kSphereRadiusGeo = 1.0; // The mesh sphere being shaded.
const vec3 kAlbedo = vec3(0.85, 0.82, 0.8); // Mildly warm off-white dielectric.

// Light authored so the highlight clearly lands on the sphere's upper-
// right hemisphere. Intensity = 8 lands a comfortably-visible specular
// on a roughness=0.05 sphere at distance ~3.
const vec3 kLightPos = vec3(2.2, 2.0, 2.0);
const vec3 kLightColor = vec3(1.0, 0.97, 0.92);
const float kLightIntensity = 8.0;
const float kLightRange = 15.0;
const float kAreaLightRadius = 1.0; // Emitter radius for the top row.

// Background tint so the sphere silhouette stands out clearly in the PNG.
const vec3 kBackground = vec3(0.04, 0.05, 0.07);

// Reinhard tone-map. RGBA8 needs [0,1] but the highlight on a
// roughness=0.05 sphere comfortably overshoots 1.0 in linear space.
// Reinhard preserves the relative softness contrast that's the point
// of the side-by-side comparison.
vec3 ReinhardToneMap(vec3 c)
{
    return c / (c + vec3(1.0));
}

// gamma-2.2 approximation — matches the post-process pipeline's default
// gamma so the PNG reads close to what the editor viewport would show.
vec3 LinearToSrgbApprox(vec3 c)
{
    return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
}

// Ray-vs-sphere. Returns hit t along ray (negative = miss).
float IntersectSphere(vec3 rayOrigin, vec3 rayDir, vec3 sphereCenter, float sphereRadius)
{
    vec3 oc = rayOrigin - sphereCenter;
    float b = dot(oc, rayDir);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float disc = b * b - c;
    if (disc < 0.0)
        return -1.0;
    float t = -b - sqrt(disc);
    return t;
}

// Point-light reference. Uses the *same* Epic-style smooth-square distance
// falloff that calculateSphereAreaLightContribution builds into the area-
// light path (PBRCommon.glsl line ~559), so the two rows differ only in
// the BRDF / representative-point math. If we routed the point light
// through `calculateAttenuation` with quadratic=0, it'd be ~10× brighter
// than the area light at the same distance — the comparison would be
// dominated by attenuation curves rather than highlight shape, defeating
// the point of the test.
vec3 EvaluatePointLight(vec3 N, vec3 V, vec3 worldPos, float roughness)
{
    vec3 toLight = kLightPos - worldPos;
    float distance = length(toLight);
    if (distance > kLightRange)
        return vec3(0.0);

    vec3 L = toLight / max(distance, EPSILON);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= EPSILON)
        return vec3(0.0);

    // Mirror the area-light path's distance attenuation exactly.
    float distRatio = distance / max(kLightRange, EPSILON);
    float distAtten = max(1.0 - distRatio * distRatio, 0.0);
    distAtten = distAtten * distAtten / (distance * distance + 1.0);

    vec3 radiance = kLightColor * kLightIntensity * distAtten;
    vec3 brdf = closureV2Evaluate(N, V, L, kAlbedo, 0.0 /*metallic*/, roughness);
    return brdf * radiance * NdotL;
}

vec3 IntegrateSphereEmitter(vec3 N, vec3 V, vec3 worldPos, float roughness)
{
    GPUSceneLight light;
    light.PositionAndRange = vec4(kLightPos, kLightRange);
    light.ColorAndIntensity = vec4(kLightColor, kLightIntensity);
    light.DirectionAndRadius = vec4(0.0, 0.0, 0.0, kAreaLightRadius);
    OloSphereLightView view = OloViewSphereLight(light, worldPos);
    if (!view.Valid)
        return vec3(0.0);

    // Balance the cone samples against BSDF samples, as the reference tracer
    // does. Cone sampling alone misses the near-mirror lobe at this budget.
    const uint samples = 256u;
    float emitterPdf = OloSphereConePdf(view.CosThetaMax);
    vec3 centre = normalize(kLightPos - worldPos);
    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < samples; ++i)
    {
        vec2 xi = Hammersley(i, samples);
        vec3 L = OloSampleSphereLightDirection(light, view, worldPos, xi);
        float bsdfPdf = closureV2Pdf(N, V, L, kAlbedo, 0.0, roughness);
        sum += closureV2Evaluate(N, V, L, kAlbedo, 0.0, roughness)
               * (max(dot(N, L), 0.0) / (emitterPdf + bsdfPdf));

        // Separate the discrete lobe choice from the angular coordinates.
        float lobe = fract((float(i) + 0.5) * 0.61803398875);
        ClosureV2Sample bsdf = closureV2SampleBRDF(N, V, kAlbedo, 0.0, roughness, lobe, xi);
        if (bsdf.Pdf > 0.0 && dot(bsdf.L, centre) >= view.CosThetaMax)
            sum += bsdf.Value * (max(dot(N, bsdf.L), 0.0) / (emitterPdf + bsdf.Pdf));
    }
    return view.Radiance * sum / float(samples);
}

void main()
{
    // Map v_TexCoord ([0,1] x [0,1]) into a (col, row, localUv) tuple.
    // row 0 = top half (area light), row 1 = bottom half (point light).
    float colF = v_TexCoord.x * float(kColumns);
    int col = clamp(int(floor(colF)), 0, kColumns - 1);
    int row = clamp(int(floor(v_TexCoord.y * 3.0)), 0, 2);

    // Cell-local NDC in [-1, 1] x [-1, 1].
    vec2 cellUv = vec2(fract(colF), fract(v_TexCoord.y * 3.0));
    vec2 ndc = cellUv * 2.0 - 1.0;

    // Squeeze the FOV so the sphere comfortably fills the cell without
    // touching edges — avoids hard discontinuities at cell seams in the
    // PNG that would distract from the highlight comparison.
    vec3 rayDir = normalize(vec3(ndc.x * 0.6, ndc.y * 0.6, -1.0));
    vec3 rayOrigin = kCameraPos;

    float roughness = max(kRoughnessByColumn[col], MIN_ROUGHNESS);

    float tHit = IntersectSphere(rayOrigin, rayDir, vec3(0.0), kSphereRadiusGeo);
    if (tHit < 0.0)
    {
        // Background — slight per-column blue tint so column boundaries
        // are subtly visible in the PNG, helping the eye line up
        // top/bottom pairs.
        vec3 bg = kBackground + vec3(0.0, 0.0, float(col) * 0.005);
        o_Color = vec4(LinearToSrgbApprox(bg), 1.0);
        return;
    }

    vec3 hitPos = rayOrigin + rayDir * tHit;
    vec3 N = normalize(hitPos);
    vec3 V = normalize(rayOrigin - hitPos);

    vec3 Lo;
    if (row == 0)
    {
        // Top row: sphere area light (Karis representative-point path).
        Lo = calculateSphereAreaLightContribution(
            N, V, kLightPos, kAreaLightRadius,
            kLightColor, kLightIntensity, kLightRange,
            kAlbedo, 0.0 /*metallic*/, roughness, hitPos);
    }
    else if (row == 1)
    {
        // Middle row: point-light reference, same colour / intensity / range.
        Lo = EvaluatePointLight(N, V, hitPos, roughness);
    }
    else
    {
        Lo = IntegrateSphereEmitter(N, V, hitPos, roughness);
    }

    // Soft ambient so the dark side of the sphere is not pure black —
    // the silhouette + highlight comparison reads better with a tiny
    // ambient lift. Conceptually mimics IBL with intensity ~0.03.
    vec3 ambient = kAlbedo * 0.03;
    vec3 colorLinear = ambient + Lo;
    vec3 mapped = LinearToSrgbApprox(ReinhardToneMap(colorLinear));
    o_Color = vec4(mapped, 1.0);
}
