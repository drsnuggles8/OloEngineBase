// Everything the four ReSTIR GI draws share: the shading point, the target
// function they all resample against, and the RECONNECTION RAY. Issue #1169.
//
// WHY ONE INCLUDE AND NOT FOUR SHADERS' WORTH OF COPIES. The target function is
// the single most duplication-prone thing in a ReSTIR implementation: initial
// sampling, temporal reuse, spatial reuse and the resolve must all evaluate the
// SAME pHat, and if any two of them disagree the estimator is biased in a way
// that looks exactly like noise. So it is defined once, here, and the four draws
// are thin.
//
// WHAT IS *NOT* HERE: generating a bounce sample and shading its vertex. That
// is include/ReSTIRGIBounce.glsl's, and only the initial-sample draw includes
// it — it is the only draw that needs the environment cube and the probe
// atlases, and dragging six more bindings into three draws that never read them
// is how a binding budget runs out.
//
// WHAT THE CALLER MUST HAVE INCLUDED FIRST: include/PBRCommon.glsl,
// include/ReSTIRGIParams.glsl, include/GBufferRaySurface.glsl,
// include/ReservoirGI.glsl and include/RayTracedSurfaceHit.glsl.
#ifndef OLO_RESTIR_GI_COMMON_GLSL
#define OLO_RESTIR_GI_COMMON_GLSL

// ---------------------------------------------------------------------------
// The target function (design note §2)
// ---------------------------------------------------------------------------

// The direction from a shading point toward a GI sample, and whether one
// exists. An ENVIRONMENT sample stores the direction itself; a SURFACE HIT
// stores a vertex, and the direction has to be derived — which reading applies
// comes from Kind and never from inspecting the vector.
bool OloGISampleDirection(OloGISample s, vec3 shadingPoint, out vec3 direction, out float distanceToVertex)
{
    direction = vec3(0.0);
    distanceToVertex = 0.0;
    if (s.Kind == OLO_GI_SAMPLE_NONE)
        return false;
    if (OloGISampleIsDistant(s.Kind))
    {
        float lengthSq = dot(s.Position, s.Position);
        if (!(lengthSq > 0.0))
            return false;
        direction = s.Position * inversesqrt(lengthSq);
        // An environment sample has no vertex, so there is no finite distance
        // to report. The callers that care (the reconnection ray, the
        // ReconnectionLength debug view) branch on Kind rather than on this
        // being zero, because zero is also what a degenerate surface hit gives.
        distanceToVertex = 0.0;
        return true;
    }
    vec3 toVertex = s.Position - shadingPoint;
    float distanceSq = dot(toVertex, toVertex);
    if (!(distanceSq > 1e-12))
        return false;
    distanceToVertex = sqrt(distanceSq);
    direction = toVertex / distanceToVertex;
    return true;
}

// The contribution of one GI sample to this surface's outgoing radiance:
// f_r(x0) * L_o(x1) * cos(theta0).
//
// THE RECONNECTION'S VISIBILITY IS DELIBERATELY NOT IN IT, exactly as DI's
// target function excludes the shadow ray and for the same reason: a target
// function with a ray in it would make every reuse cost a trace, which is the
// cost ReSTIR exists to avoid. The consequence is bigger here than it is for
// DI — see design note §6.2 — which is why the resolve traces one ray on the
// survivor and why the diagnostics count when it kills one.
//
// `viewDirection` points FROM the surface TOWARD the eye.
vec3 OloGIBounceContribution(OloGBufferSurface surface, OloGISample giSample, vec3 viewDirection)
{
    vec3 l;
    float unusedDistance;
    if (!OloGISampleDirection(giSample, surface.Position, l, unusedDistance))
        return vec3(0.0);

    float nDotL = dot(surface.ShadingNormal, l);
    if (!(nDotL > 0.0))
        return vec3(0.0);

    // evaluatePBRClosure is the #975 versioned dispatch — the SAME function the
    // deferred lighting pass, the ReSTIR DI tier and the path tracer's
    // PtEvaluateBRDF route through, so the oracle and the tier being validated
    // against it shade a surface with one BRDF rather than with two plausible
    // ones. The FULL closure at x0, not the diffuse lobe: the diffuse
    // restriction is a statement about the BOUNCE VERTEX, not about the pixel
    // being shaded, and applying it here would drop indirect light from every
    // glossy surface in the frame.
    vec3 f = evaluatePBRClosure(surface.PbrModel, surface.ShadingNormal, viewDirection, l, surface.Albedo,
                                surface.Metallic, surface.Roughness);
    return f * giSample.Radiance * nDotL;
}

// The scalar the resampling actually compares. A luminance-weighted norm rather
// than the vector length: it is what the estimator's variance is measured in,
// and it keeps a saturated blue bounce from outranking a brighter white one.
// The same scalar DI uses, for the same reason.
float OloGITargetPdf(OloGBufferSurface surface, OloGISample giSample, vec3 viewDirection)
{
    vec3 contribution = OloGIBounceContribution(surface, giSample, viewDirection);
    float t = dot(contribution, vec3(0.2126, 0.7152, 0.0722));
    return (OloReservoirFinite(t) && t > 0.0) ? t : 0.0;
}

// ---------------------------------------------------------------------------
// The reconnection ray (design note §6.2)
// ---------------------------------------------------------------------------

// Is the sample vertex actually reachable from this shading point?
//
// THIS HAS NO DI ANALOGUE AND IT IS NOT AN OPTIMISATION. DI's spatial reuse
// traces no rays at all, and is right not to: the reused emitter point is the
// same point in space, and the destination pixel's own resolve ray tests it.
// GI's reconnection introduces the segment x0' -> x1, which NEVER EXISTED IN
// THE SOURCE PATH — a wall between the destination pixel and the neighbour's
// vertex is invisible to the source reservoir. Without this test, reuse lights
// surfaces through that wall, and because reuse is spatially coherent it does so
// as a SMOOTH GRADIENT, which is the shape nobody files as a bug.
//
// An ENVIRONMENT sample is tested along its direction out to the bounce
// distance: the sky is only reachable if nothing is in the way, and treating an
// escape as automatically visible would light the inside of a closed room.
//
// OloRtIsOccluded is include/RayTracedSurfaceHit.glsl's — the SAME segment
// query the path tracer's shadow rays use, down to the sphere-light rule. If one
// tier's visibility test saw spheres and another's did not, the two would
// disagree about what is lit and the oracle comparison would stop meaning
// anything.
bool OloGIReconnectionVisible(OloGBufferSurface surface, OloGISample giSample, float epsilon, float normalBias)
{
    if (giSample.Kind == OLO_GI_SAMPLE_NONE)
        return false;
    if (u_TlasAddressAndFrame.x == 0u && u_TlasAddressAndFrame.y == 0u)
        return true; // No structure to trace against: the caller's guards decided this frame stands down.

    vec3 origin = surface.Position + surface.GeometricNormal * normalBias;
    vec3 direction;
    float distanceToVertex;
    if (!OloGISampleDirection(giSample, surface.Position, direction, distanceToVertex))
        return false;

    vec3 target;
    if (OloGISampleIsDistant(giSample.Kind))
    {
        target = origin + direction * u_EstimatorParams.y;
    }
    else
    {
        // The segment stops SHORT of the vertex: a ray aimed at a surface point
        // hits that surface, so an unshortened segment reports every
        // reconnection occluded and the whole tier goes black. The same inset
        // OloRtIsOccluded already applies at both ends, doubled here because
        // the far end is a real surface rather than an emitter's interior.
        vec3 toVertex = giSample.Position - origin;
        float reach = length(toVertex);
        if (!(reach > 4.0 * epsilon))
            return true;
        target = origin + (toVertex / reach) * (reach - 2.0 * epsilon);
    }
    return !OloRtIsOccluded(origin, target, epsilon);
}

#endif // OLO_RESTIR_GI_COMMON_GLSL
