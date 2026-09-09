// Light sampling and its densities — the ONE place the engine's ray-traced
// tiers agree on how a light is sampled and what the resulting pdf is (#1140).
//
// WHY THIS FILE EXISTS. It was extracted VERBATIM from GpuPathTracer.glsl, not
// written beside it. #1140's scope is explicit that ReSTIR DI's light-sampling
// pdf must be "shared with the PT's NEE rather than re-derived", and the reason
// is the one PBRClosureBSDF.h states for the closure: an independently written
// density produces an estimator whose MIS weights are subtly wrong, and a
// subtly wrong resampled estimator converges beautifully to the wrong image.
// The oracle and the tier being validated against it cannot be allowed to
// disagree about the measure, because then the comparison proves nothing.
//
// The extraction was verified by disassembling GpuPathTracer's SPIR-V before
// and after and diffing it with the debug names stripped: the oracle's code is
// unchanged, so its pinned CPU-parity hashes still mean what they meant.
//
// WHAT THE CALLER MUST HAVE INCLUDED FIRST:
//   include/PBRCommon.glsl        (PI, calculateAttenuation, OrthonormalBasis)
//   include/GPUSceneLights.glsl   (GPUSceneLight and the OLO_GPU_SCENE_LIGHT_* enums)
//
// AND, before including this file, a caller that wants the emissive sampler to
// see emitter TEXTURES must define, in its own scope:
//   #define OLO_LIGHT_SAMPLE_EMISSIVE_TEXTURE(byteOffset, uv)  <a vec4 sample>
//   #define OLO_LIGHT_EMISSIVE_TEXTURES_ENABLED               <a bool expression>
// Left undefined, emitters sample their radiance FACTOR alone — the same
// arrangement RayTracingAlphaTest.glsl uses for OLO_RT_SAMPLE_ALPHA, and for
// the same reason: the heap uniforms are per-shader, the arithmetic is not.
// The enable predicate is a SEPARATE macro rather than folded into the sampler
// so the guard stays a short-circuit; a sampler that returned white when
// textures are off would multiply by one instead of skipping, which is
// numerically identical and not the same generated code.
#ifndef OLO_LIGHT_SAMPLING_GLSL
#define OLO_LIGHT_SAMPLING_GLSL

#if defined(OLO_LIGHT_SAMPLE_EMISSIVE_TEXTURE) && !defined(OLO_LIGHT_EMISSIVE_TEXTURES_ENABLED)
#define OLO_LIGHT_EMISSIVE_TEXTURES_ENABLED true
#endif

// Bounded loop count for the emissive CDF's binary search. A uniform-driven
// loop bound cannot be unrolled and a bad upload could hang the GPU; 32 steps
// reach 2^32 entries, so the bound is not a limit in practice.
#define OLO_LIGHT_MAX_EMISSIVE_SEARCH 32u

// The GPU Scene light slots any loop here will walk, with a runtime break at
// the live count. Same reason: a uniform-driven bound cannot be unrolled.
// GpuPathTracer.glsl's OLO_PT_MAX_LIGHTS is the same number and is pinned to
// kGpuPathTracerMaxLights by GpuPathTracerContractTest.
#define OLO_LIGHT_MAX_SLOTS 256u

// PathTracer.cpp's kEpsilon (ReferenceBRDF.h) — the punctual-light early-outs
// mirror calculateLightContribution's, so the two agree on which lights
// contribute at all.
#define OLO_LIGHT_EPSILON 0.0001

// ---------------------------------------------------------------------------
// Emissive geometry
// ---------------------------------------------------------------------------

// One emissive triangle, in the render-relative frame the TLAS is built in.
// Mirrors OloEngine::EmissiveTriangleRecord (EmissiveTriangleTable.h): seven
// vec4-sized rows so the C++ side uploads the struct verbatim with no packing
// step.
struct OloEmissiveTriangle
{
    vec4 V0;               // xyz vertex 0, w = area
    vec4 V1;               // xyz vertex 1, w = uv2.x
    vec4 V2;               // xyz vertex 2, w = uv2.y
    vec4 NormalAndCdf;     // xyz winding normal, w = cumulative area fraction (last entry exactly 1)
    vec4 RadianceAndFlags; // rgb emitted radiance FACTOR, w = 1 when the emitter is two-sided
    vec4 Uv01;             // xy = uv0, zw = uv1 — with the map below, NEE sees the textured radiance
    uvec4 Texture;         // x = emissive map heap byte offset (OLO_HEAP_OFFSET_INVALID = none), yzw pad
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OloEmissiveTable
{
    OloEmissiveTriangle Triangles[];
};

// One sampled point on an emitter, in AREA measure. `PdfArea` is the density
// this point was drawn from over the emitter set's total area — the constant
// 1/totalArea that uniform-over-area selection makes possible, and the reason
// the BSDF-hit MIS side can reuse it without knowing which triangle it hit.
struct OloEmissiveSample
{
    bool Valid;
    vec3 Position;
    vec3 Normal;
    vec3 Radiance;
    bool TwoSided;
    float PdfArea;
    uint TriangleIndex;
};

// Area-proportional triangle selection through the table's cumulative area
// fraction, then a uniform barycentric point (Turk's square-root warp).
// ReferenceScene::SampleEmissive on the CPU.
//
// Uniform over AREA rather than over triangles is what makes the density the
// single constant `pdfArea`, which is why that value is a uniform the caller
// passes in rather than something recomputed here.
OloEmissiveSample OloSampleEmissiveTriangle(OloEmissiveTable table, uint count, float pdfArea, float xiSelect,
                                            vec2 xiPoint)
{
    OloEmissiveSample s;
    s.Valid = false;
    s.Position = vec3(0.0);
    s.Normal = vec3(0.0);
    s.Radiance = vec3(0.0);
    s.TwoSided = false;
    s.PdfArea = 0.0;
    s.TriangleIndex = 0u;
    if (count == 0u || !(pdfArea > 0.0))
        return s;

    // std::lower_bound on the CDF: the first entry whose cumulative fraction is
    // >= xiSelect.
    uint lo = 0u;
    uint hi = count;
    for (uint step = 0u; step < OLO_LIGHT_MAX_EMISSIVE_SEARCH; ++step)
    {
        if (lo >= hi)
            break;
        const uint mid = lo + (hi - lo) / 2u;
        if (table.Triangles[mid].NormalAndCdf.w < xiSelect)
            lo = mid + 1u;
        else
            hi = mid;
    }
    const uint triangleIndex = min(lo, count - 1u);
    const OloEmissiveTriangle emitter = table.Triangles[triangleIndex];

    const float sqrtU = sqrt(clamp(xiPoint.x, 0.0, 1.0));
    const float b0 = 1.0 - sqrtU;
    const float b1 = clamp(xiPoint.y, 0.0, 1.0) * sqrtU;
    const float b2 = 1.0 - b0 - b1;

    s.Valid = true;
    s.Position = emitter.V0.xyz * b0 + emitter.V1.xyz * b1 + emitter.V2.xyz * b2;
    s.Normal = emitter.NormalAndCdf.xyz;
    s.Radiance = emitter.RadianceAndFlags.rgb;
    // The emitter's map at the sampled point, so NEE and the emitter-hit path
    // see one radiance and MIS weights the same integrand twice.
#ifdef OLO_LIGHT_SAMPLE_EMISSIVE_TEXTURE
    if (emitter.Texture.x != OLO_HEAP_OFFSET_INVALID && OLO_LIGHT_EMISSIVE_TEXTURES_ENABLED)
    {
        const vec2 uv = emitter.Uv01.xy * b0 + emitter.Uv01.zw * b1 + vec2(emitter.V1.w, emitter.V2.w) * b2;
        s.Radiance *= OLO_LIGHT_SAMPLE_EMISSIVE_TEXTURE(emitter.Texture.x, uv).rgb;
    }
#endif
    s.TwoSided = emitter.RadianceAndFlags.w > 0.5;
    s.PdfArea = pdfArea;
    s.TriangleIndex = triangleIndex;
    return s;
}

// The area -> solid-angle change of variables the direct-lighting estimator
// needs, with the one-sided test folded in. Returns 0 when the emitter faces
// away, which is a REJECT and not a zero-radiance sample.
float OloEmissiveSolidAnglePdf(OloEmissiveSample s, vec3 shadingPoint)
{
    const vec3 toLight = s.Position - shadingPoint;
    const float distanceSq = dot(toLight, toLight);
    if (!(distanceSq > 1e-12) || !(s.PdfArea > 0.0))
        return 0.0;
    const vec3 l = toLight * inversesqrt(distanceSq);
    const float cosLight = dot(s.Normal, -l);
    const float effectiveCosLight = s.TwoSided ? abs(cosLight) : cosLight;
    if (!(effectiveCosLight > 0.0))
        return 0.0;
    return s.PdfArea * distanceSq / effectiveCosLight;
}

// ---------------------------------------------------------------------------
// Sphere area lights
// ---------------------------------------------------------------------------
//
// PathTracer.cpp's ViewSphereLight / SphereConePdf / SphereLightPointDistance,
// function for function. A sphere of radius r at distance d is a uniform
// emitter whose radiance reproduces the raster's diffuse irradiance at the
// receiver, sampled by uniform solid angle over the cone it subtends.
struct OloSphereLightView
{
    bool Valid;
    float Distance;
    float CosThetaMax;
    vec3 Radiance;
};

OloSphereLightView OloViewSphereLight(GPUSceneLight light, vec3 from)
{
    OloSphereLightView view;
    view.Valid = false;
    view.Distance = 0.0;
    view.CosThetaMax = 1.0;
    view.Radiance = vec3(0.0);
    const vec3 toCenter = light.PositionAndRange.xyz - from;
    const float distance = length(toCenter);
    const float radius = light.DirectionAndRadius.w;
    const float range = light.PositionAndRange.w;
    if (!(radius > 0.0) || !(distance > radius) || distance > range)
        return view;
    const float distRatio = distance / max(range, 1e-6);
    const float window = max(1.0 - distRatio * distRatio, 0.0);
    const float attenuation = window * window / (distance * distance + 1.0);
    view.Valid = true;
    view.Distance = distance;
    view.CosThetaMax = sqrt(max(0.0, 1.0 - (radius * radius) / (distance * distance)));
    view.Radiance = light.ColorAndIntensity.rgb * light.ColorAndIntensity.w * attenuation * (distance * distance) /
                    (PI * radius * radius);
    return view;
}

float OloSphereConePdf(float cosThetaMax)
{
    const float solidAngle = 2.0 * PI * (1.0 - cosThetaMax);
    return solidAngle > 0.0 ? 1.0 / solidAngle : 0.0;
}

float OloSphereLightPointDistance(float distance, float radius, float cosTheta)
{
    const float sinThetaSq = max(0.0, 1.0 - cosTheta * cosTheta);
    return distance * cosTheta - sqrt(max(0.0, radius * radius - distance * distance * sinThetaSq));
}

// The nearest sphere light the ray reaches before tMax: its light slot, or -1.
//
// Shared because a shadow ray must stop at a sphere light for exactly the same
// reason a closest-hit ray does — PathTracer.cpp's ShadowRayBlocked. If one
// tier's visibility test sees spheres and another's does not, the two disagree
// about what is lit, and the disagreement is the shape of a soft shadow rather
// than an obvious artefact.
int OloIntersectSphereLights(vec3 origin, vec3 direction, float tMax, uint lightCount, out float outT)
{
    int found = -1;
    outT = tMax;
    for (uint i = 0u; i < OLO_LIGHT_MAX_SLOTS; ++i)
    {
        if (i >= lightCount)
            break;
        const GPUSceneLight light = g_GPUSceneLights[i];
        if ((light.Flags & OLO_GPU_SCENE_LIGHT_ACTIVE) == 0u || light.Type != OLO_GPU_SCENE_LIGHT_SPHERE_AREA)
            continue;
        const float radius = light.DirectionAndRadius.w;
        if (!(radius > 0.0))
            continue;
        // Inside or past the range: transparent (PathTracer.cpp's rule).
        const vec3 oc = origin - light.PositionAndRange.xyz;
        const float centreDistance = length(oc);
        if (!(centreDistance > radius) || centreDistance > light.PositionAndRange.w)
            continue;
        const float b = dot(oc, direction);
        const float c = dot(oc, oc) - radius * radius;
        const float discriminant = b * b - c;
        if (discriminant < 0.0)
            continue;
        const float root = sqrt(discriminant);
        float t = -b - root;
        if (!(t > 0.0))
            t = -b + root;
        if (!(t > 0.0) || !(t < outT))
            continue;
        found = int(i);
        outT = t;
    }
    return found;
}

// The direction drawn from the cone, in the receiver's frame. Split out from
// the pdf so a caller that only needs the density (a MIS weight on a BSDF hit)
// does not build a basis it will not use.
vec3 OloSampleSphereLightDirection(GPUSceneLight light, OloSphereLightView view, vec3 from, vec2 xi)
{
    const float cosTheta = 1.0 - xi.x * (1.0 - view.CosThetaMax);
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float phi = 2.0 * PI * xi.y;
    const vec3 toCenter = (light.PositionAndRange.xyz - from) / view.Distance;
    vec3 tangent, bitangent;
    OrthonormalBasis(toCenter, tangent, bitangent);
    return tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + toCenter * cosTheta;
}

// ---------------------------------------------------------------------------
// Punctual lights
// ---------------------------------------------------------------------------

// ReferenceBRDF.h's CalculateSpotIntensity rather than PBRCommon's
// calculateSpotIntensity: the CPU twin guards a zero-width cone (inner ==
// outer) and PBRCommon's divides by it. The oracle has to agree with the
// oracle, so the guarded form is the one transcribed here.
float OloSpotIntensity(vec3 l, vec3 spotDir, vec4 spotParams)
{
    const float innerCutoff = spotParams.x;
    const float outerCutoff = spotParams.y;
    const float theta = dot(l, normalize(-spotDir));
    const float epsilon = innerCutoff - outerCutoff;
    if (!(abs(epsilon) > 0.0))
        return theta >= innerCutoff ? 1.0 : 0.0;
    const float intensity = clamp((theta - outerCutoff) / epsilon, 0.0, 1.0);
    return intensity * intensity;
}

// A punctual light as a receiver sees it: the direction toward it, the point a
// shadow ray must reach, and the radiance arriving with attenuation and the
// spot cone already applied. `farDistance` is how far a directional light's
// probe travels — it has no position, so the caller supplies a distance that
// leaves the scene.
struct OloPunctualLightView
{
    bool Valid;
    vec3 Direction;    // unit, toward the light
    vec3 ShadowTarget; // render-relative point the shadow ray must reach
    vec3 Radiance;     // arriving radiance, attenuation and spot applied
    bool Directional;
};

OloPunctualLightView OloViewPunctualLight(GPUSceneLight light, vec3 from, float farDistance)
{
    OloPunctualLightView view;
    view.Valid = false;
    view.Direction = vec3(0.0);
    view.ShadowTarget = vec3(0.0);
    view.Radiance = vec3(0.0);
    view.Directional = false;

    float attenuation = 1.0;
    if (light.Type == OLO_GPU_SCENE_LIGHT_DIRECTIONAL)
    {
        view.Directional = true;
        view.Direction = normalize(-light.DirectionAndRadius.xyz);
        // Infinitely far away: probe far enough to leave the scene.
        view.ShadowTarget = from + view.Direction * farDistance;
    }
    else if (light.Type == OLO_GPU_SCENE_LIGHT_POINT || light.Type == OLO_GPU_SCENE_LIGHT_SPOT)
    {
        const vec3 lightPos = light.PositionAndRange.xyz;
        const vec3 toLight = lightPos - from;
        const float dist = length(toLight);
        if (!(dist > 0.0))
            return view;
        view.Direction = toLight / dist;
        // (constant, linear, quadratic, range) packed the way Scene.cpp fills
        // the MultiLight UBO and ReferenceSceneBuilder fills
        // ReferenceLight::AttenuationParams: (1, 0, attenuation, range).
        attenuation = calculateAttenuation(lightPos, from,
                                           vec4(1.0, 0.0, light.ShapeParams.z, light.PositionAndRange.w));
        if (light.Type == OLO_GPU_SCENE_LIGHT_SPOT)
        {
            attenuation *= OloSpotIntensity(view.Direction, light.DirectionAndRadius.xyz,
                                            vec4(light.ShapeParams.x, light.ShapeParams.y, light.ShapeParams.w, 1.0));
        }
        view.ShadowTarget = lightPos;
    }
    else
    {
        // A sphere light is an AREA light: OloViewSphereLight's business.
        return view;
    }

    if (attenuation <= OLO_LIGHT_EPSILON)
        return view;
    view.Valid = true;
    view.Radiance = light.ColorAndIntensity.rgb * light.ColorAndIntensity.w * attenuation;
    return view;
}

#endif // OLO_LIGHT_SAMPLING_GLSL
