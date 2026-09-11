// Everything the four ReSTIR DI draws share: the UBO, the surface they all
// reconstruct from the G-Buffer, the target function they all resample against,
// the candidate generator, and the visibility ray. Issue #1140.
//
// WHY ONE INCLUDE AND NOT FOUR SHADERS' WORTH OF COPIES. The target function is
// the single most duplication-prone thing in a ReSTIR implementation: initial
// sampling, temporal reuse, spatial reuse and the resolve must all evaluate the
// SAME pHat, and if any two of them disagree the estimator is biased in a way
// that looks exactly like noise. So it is defined once, here, and the four
// draws are thin.
//
// WHAT THE CALLER MUST HAVE INCLUDED FIRST:
//   include/PBRCommon.glsl, include/GPUScene.glsl and the GPUScene table
//   includes, include/LightSampling.glsl, include/Reservoir.glsl.
//
// A caller that can index the descriptor heap should also define, before
// including this file:
//   #define OLO_RESTIR_CANDIDATE_IS_SOLID(instanceSlot, primitive, barycentrics)
// so an alpha-MASKED occluder is tested rather than shadowing as a rectangle.
// Undefined, every candidate hit is treated as SOLID — the conservative
// direction (a masked leaf casts its whole quad) and the one the stats report
// through ReSTIRDIStats rather than leaving to a reviewer to notice.
#ifndef OLO_RESTIR_DI_COMMON_GLSL
#define OLO_RESTIR_DI_COMMON_GLSL

#ifndef OLO_RESTIR_CANDIDATE_IS_SOLID
#define OLO_RESTIR_CANDIDATE_IS_SOLID(instanceSlot, primitive, barycentrics) true
#endif

// The parameter block, the flags, the debug-view enum and the loop bounds live
// in include/ReSTIRDIParams.glsl, which a caller must include before the
// alpha-MASK test it feeds. This file assumes it is already in scope.
#include "ReSTIRDIParams.glsl"

// ---------------------------------------------------------------------------
// The surface — include/GBufferRaySurface.glsl's, under DI's names
// ---------------------------------------------------------------------------
//
// The G-Buffer reconstruction moved there for issue #1169, so ReSTIR DI and
// ReSTIR GI reconstruct the SAME shading point from the same texels. Both tiers
// trace rays from it and both convert densities through it, so a half-texel
// difference between two copies would make one tier's rays start somewhere the
// other's do not — which reads as "the two tiers disagree slightly about
// contact shadowing" rather than as a reconstruction bug.
//
// The names below are ALIASES: no DI draw changed, and the compiled SPIR-V is
// unmoved.
#include "GBufferRaySurface.glsl"

#define OloReSTIRSurface OloGBufferSurface
#define OloReSTIROctDecode OloGBufferOctDecode
#define OloReSTIRViewPosFromDepth OloGBufferViewPosFromDepth
#define OloReSTIRLoadSurface OloLoadGBufferSurface
#define OloReSTIRInvalidSurface OloInvalidGBufferSurface

// ---------------------------------------------------------------------------
// The target function
// ---------------------------------------------------------------------------

// The UNSHADOWED contribution of one light sample to this surface's outgoing
// radiance: f * L * cos(thetaShading). Visibility is deliberately NOT in it —
// a target function that included a ray would make every reuse cost a trace,
// which is the cost ReSTIR exists to avoid. The consequence is that reuse can
// resurrect a shadowed sample, which is why VisibilityReuse tests the survivor
// once and why the diagnostics count when it kills one.
//
// `viewDirection` points FROM the surface TOWARD the eye.
vec3 OloReSTIRUnshadowedContribution(OloReSTIRSurface surface, OloLightSample lightSample, vec3 viewDirection)
{
    if (lightSample.Kind == OLO_LIGHT_SAMPLE_NONE)
        return vec3(0.0);

    vec3 l;
    if (lightSample.Kind == OLO_LIGHT_SAMPLE_DIRECTIONAL)
    {
        l = lightSample.Position; // a direction, per OloLightSample's contract
        float lengthSq = dot(l, l);
        if (!(lengthSq > 0.0))
            return vec3(0.0);
        l *= inversesqrt(lengthSq);
    }
    else
    {
        vec3 toLight = lightSample.Position - surface.Position;
        float distanceSq = dot(toLight, toLight);
        if (!(distanceSq > 1e-12))
            return vec3(0.0);
        l = toLight * inversesqrt(distanceSq);
    }

    float nDotL = dot(surface.ShadingNormal, l);
    if (!(nDotL > 0.0))
        return vec3(0.0);

    // evaluatePBRClosure is the #975 versioned dispatch — the SAME function the
    // deferred lighting pass and the path tracer's PtEvaluateBRDF route
    // through, so the oracle and the tier being validated against it shade a
    // surface with one BRDF rather than with two plausible ones.
    vec3 f = evaluatePBRClosure(surface.PbrModel, surface.ShadingNormal, viewDirection, l, surface.Albedo,
                                surface.Metallic, surface.Roughness);
    return f * lightSample.Radiance * nDotL;
}

// The scalar the resampling actually compares. A luminance-weighted norm rather
// than the vector length: it is what the estimator's variance is measured in,
// and it keeps a saturated blue light from outranking a brighter white one.
float OloReSTIRTargetPdf(OloReSTIRSurface surface, OloLightSample lightSample, vec3 viewDirection)
{
    vec3 contribution = OloReSTIRUnshadowedContribution(surface, lightSample, viewDirection);
    float t = dot(contribution, vec3(0.2126, 0.7152, 0.0722));
    return (OloReservoirFinite(t) && t > 0.0) ? t : 0.0;
}

// ---------------------------------------------------------------------------
// Candidate generation — the source pdf, shared with the path tracer's NEE
// ---------------------------------------------------------------------------

// How many emitters there are to choose from, counted the way the source pdf
// weights them: every punctual and sphere-area light is one candidate, and the
// emissive table contributes one per triangle. That count is also what
// ReSTIRDIEngageInputs::CandidateLightCount holds on the CPU, so the criterion
// that decides whether this tier runs and the pdf that runs inside it are
// counting the same thing.
// Whether this light family is ReSTIR's to resample.
//
// DIRECTIONAL LIGHTS ARE NOT. One delta light has no variance for resampling to
// remove, and taking it would silently drop the CSM / VSM cascades, the
// ray-traced shadow mask channel and the cloud shadow that the clustered loop
// applies to it — see DeferredLightingShared.glsl's ComputeDeferredLit, which
// keeps walking the directional lights for exactly this reason. Excluding them
// HERE rather than only in the consumer is what keeps the two ends from
// disagreeing about who owns which light.
bool OloReSTIROwnsLight(GPUSceneLight light)
{
    return (light.Flags & OLO_GPU_SCENE_LIGHT_ACTIVE) != 0u &&
           light.Type != OLO_GPU_SCENE_LIGHT_DIRECTIONAL;
}

// NO OloReSTIRCandidateCount() HERE ON PURPOSE. There was one; it counted the
// OWNED lights plus the emissive triangles, it had no callers, and it was a trap
// for the next one. The candidate sampler below draws uniformly over ALL light
// slots plus the emissive triangles — rejections included, see its comment — so
// a caller that reached for the "obvious" candidate count as the source density
// would be off by exactly the ratio of owned slots to total slots, in a
// direction that varies with how many directional lights the scene happens to
// have. The density and the loop that produces it stay in one function.

// Draw one candidate from the light set. `xiSelect` picks the emitter,
// `xiPoint` the point on it (unused by a delta light, but consumed regardless
// so the sampler's dimension index stays aligned across candidates — the same
// discipline GpuPathTracer.glsl's NEE keeps, and for the same reason).
//
// `outSourcePdf` is the density in the sample's OWN measure: a probability for
// a delta light, a solid-angle density for a sphere light, an AREA density for
// an emissive triangle. Which one it is follows from the returned Kind, and
// OloReSTIRSourcePdfSolidAngle converts it when a reuse needs one measure.
bool OloReSTIRSampleCandidate(OloReSTIRSurface surface, float xiSelect, vec2 xiPoint, out OloLightSample lightSample,
                              out float outSourcePdf)
{
    lightSample = OloMakeEmptyLightSample();
    outSourcePdf = 0.0;

    uint lights = min(u_SlotCounts.w, OLO_LIGHT_MAX_SLOTS);
    uint total = lights + u_EmissiveTable.z;
    if (total == 0u)
        return false;

    // Uniform over the whole emitter set. Uniform is the right SOURCE pdf here
    // precisely because the target function does the importance work: a clever
    // source pdf would duplicate that effort and would have to be inverted
    // exactly in every reuse path.
    float selection = clamp(xiSelect, 0.0, 0.9999999);
    uint index = min(uint(selection * float(total)), total - 1u);
    float familyProbability = 1.0 / float(total);

    if (index < lights)
    {
        const GPUSceneLight light = g_GPUSceneLights[index];
        // A slot this tier does not own is a REJECTED draw, not a skipped one:
        // the caller still counts it toward M, which keeps the source density
        // uniform over the whole slot range rather than over the owned subset.
        // Compacting the range instead would need a per-frame remap table for a
        // handful of slots.
        if (!OloReSTIROwnsLight(light))
            return false;

        if (light.Type == OLO_GPU_SCENE_LIGHT_SPHERE_AREA)
        {
            const OloSphereLightView view = OloViewSphereLight(light, surface.Position);
            if (!view.Valid)
                return false;
            float conePdf = OloSphereConePdf(view.CosThetaMax);
            if (!(conePdf > 0.0))
                return false;
            vec3 l = OloSampleSphereLightDirection(light, view, surface.Position, xiPoint);
            float t = OloSphereLightPointDistance(view.Distance, light.DirectionAndRadius.w,
                                                  dot(l, normalize(light.PositionAndRange.xyz - surface.Position)));
            if (!(t > 0.0))
                return false;
            lightSample.Kind = OLO_LIGHT_SAMPLE_SPHERE_AREA;
            lightSample.LightIndex = index;
            lightSample.Position = surface.Position + l * t;
            // The outward normal at the sampled point on the sphere.
            lightSample.Normal = normalize(lightSample.Position - light.PositionAndRange.xyz);
            lightSample.Radiance = view.Radiance;
            outSourcePdf = familyProbability * conePdf;
            return true;
        }

        const OloPunctualLightView view =
            OloViewPunctualLight(light, surface.Position, u_EstimatorParams.y);
        if (!view.Valid)
            return false;
        // Point and spot only: OloReSTIROwnsLight already rejected the
        // directional slots, so view.Directional cannot be true here. The
        // OLO_LIGHT_SAMPLE_DIRECTIONAL kind stays defined in Reservoir.glsl
        // because the layout is versioned and shared with what comes after DI,
        // and because the Jacobian's delta-light arm is pinned against it.
        lightSample.Kind = OLO_LIGHT_SAMPLE_PUNCTUAL;
        lightSample.LightIndex = index;
        lightSample.Position = view.ShadowTarget;
        lightSample.Normal = vec3(0.0);
        lightSample.Radiance = view.Radiance;
        // A delta light in a discrete measure: the density IS the selection
        // probability, with no solid-angle factor to apply.
        outSourcePdf = familyProbability;
        return true;
    }

    // The emissive table, area-sampled through the SHARED sampler and the
    // SHARED 1/totalArea density, so this tier and the oracle it is validated
    // against cannot disagree about the measure.
    if (!(u_EstimatorParams.x > 0.0))
        return false;
    // Re-map the selection scalar into [0,1) over the emissive sub-range so the
    // table's own area CDF sees a uniform, not the tail of the light range.
    float xiEmissive = clamp((selection * float(total) - float(lights)) / float(u_EmissiveTable.z), 0.0, 0.9999999);
    const OloEmissiveSample emissive = OloSampleEmissiveTriangle(
        OloEmissiveTable(u_EmissiveTable.xy), u_EmissiveTable.z, u_EstimatorParams.x, xiEmissive, xiPoint);
    if (!emissive.Valid)
        return false;
    // A one-sided emitter facing away contributes nothing; rejecting here keeps
    // a zero-radiance sample out of the reservoir rather than letting it win a
    // slot with weight zero.
    vec3 toLight = emissive.Position - surface.Position;
    float distanceSq = dot(toLight, toLight);
    if (!(distanceSq > 1e-12))
        return false;
    float cosLight = dot(emissive.Normal, -toLight * inversesqrt(distanceSq));
    if (!((emissive.TwoSided ? abs(cosLight) : cosLight) > 0.0))
        return false;

    lightSample.Kind = OLO_LIGHT_SAMPLE_EMISSIVE_TRIANGLE;
    lightSample.LightIndex = emissive.TriangleIndex;
    lightSample.Position = emissive.Position;
    // A two-sided emitter is stored with the normal facing the receiver, so the
    // Jacobian's |cos| is taken about the side that actually emits toward the
    // pixel that selected it.
    lightSample.Normal = (emissive.TwoSided && cosLight < 0.0) ? -emissive.Normal : emissive.Normal;
    lightSample.Radiance = emissive.Radiance;
    // AREA measure, scaled by the family probability. The conversion to solid
    // angle happens in the estimator, once, where the geometry is known.
    outSourcePdf = familyProbability * emissive.PdfArea;
    return true;
}

// The source pdf in SOLID-ANGLE measure at `shadingPoint`, whatever measure the
// sample was drawn in. This is the single conversion point: a candidate's RIS
// weight is pHat / this, and pHat is a solid-angle-measure integrand.
float OloReSTIRSourcePdfSolidAngle(OloLightSample lightSample, float sourcePdf, vec3 shadingPoint)
{
    if (lightSample.Kind == OLO_LIGHT_SAMPLE_EMISSIVE_TRIANGLE)
        return OloAreaPdfToSolidAnglePdf(sourcePdf, lightSample, shadingPoint);
    // Sphere lights were already drawn in solid angle; delta lights live in a
    // discrete measure that needs no conversion.
    return sourcePdf;
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

// Is the sample visible from the surface? The #1056 shadow-technique seam's own
// ray: the same TLAS, the same instance mask, the same alpha-tested candidate
// rule, and the same "a sphere light in the way blocks the ray" rule the path
// tracer's IsOccluded applies — because if one tier's visibility test sees
// spheres and another's does not, the two disagree about what is lit and the
// oracle comparison stops meaning anything.
bool OloReSTIRSampleVisible(OloReSTIRSurface surface, OloLightSample lightSample, float epsilon, float normalBias)
{
    if (lightSample.Kind == OLO_LIGHT_SAMPLE_NONE)
        return false;
    if (u_TlasAddressAndFrame.x == 0u && u_TlasAddressAndFrame.y == 0u)
        return true; // No structure to trace against: the caller's guards decided this frame stands down.

    vec3 origin = surface.Position + surface.GeometricNormal * normalBias;
    vec3 direction;
    float distance;
    if (lightSample.Kind == OLO_LIGHT_SAMPLE_DIRECTIONAL)
    {
        direction = normalize(lightSample.Position);
        distance = u_EstimatorParams.y;
    }
    else
    {
        vec3 delta = lightSample.Position - origin;
        float deltaLength = length(delta);
        if (!(deltaLength > 2.0 * epsilon))
            return true;
        direction = delta / deltaLength;
        distance = deltaLength - epsilon;
    }
    if (!(distance > OLO_RESTIR_RAY_TMIN))
        return true;

    rayQueryEXT query;
    rayQueryInitializeEXT(query, accelerationStructureEXT(u_TlasAddressAndFrame.xy),
                          gl_RayFlagsTerminateOnFirstHitEXT, u_TlasAddressAndFrame.z & 0xFFu, origin,
                          OLO_RESTIR_RAY_TMIN, direction, distance);
    while (rayQueryProceedEXT(query))
    {
        if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            OLO_RESTIR_CANDIDATE_IS_SOLID(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false)),
                                          uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false)),
                                          rayQueryGetIntersectionBarycentricsEXT(query, false)))
        {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT)
        return false;

    // A sphere light in the way blocks the ray too. The segment ends epsilon
    // short of its target, so a ray aimed AT a sphere light's surface does not
    // count that sphere.
    float sphereT;
    return OloIntersectSphereLights(origin + direction * OLO_RESTIR_RAY_TMIN, direction,
                                    max(distance - 2.0 * OLO_RESTIR_RAY_TMIN, 0.0),
                                    min(u_SlotCounts.w, OLO_LIGHT_MAX_SLOTS), sphereT) < 0;
}

#endif // OLO_RESTIR_DI_COMMON_GLSL
