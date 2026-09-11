// Generating one GI candidate: the cosine-hemisphere bounce, the vertex it
// lands on, and the DIRECTION-INDEPENDENT radiance leaving that vertex.
// Issue #1169. Included only by ReSTIR_GI_InitialSample.glsl — it is the only
// draw that needs the environment cube and the probe atlases, and dragging six
// more bindings into three draws that never read them is how a binding budget
// runs out.
//
// WHAT THE CALLER MUST HAVE INCLUDED FIRST: include/ReSTIRGICommon.glsl (and
// therefore the parameter block, the GPU Scene tables, include/LightSampling.glsl
// and include/RayTracedSurfaceHit.glsl), plus include/PathTracerSampler.glsl.
//
// OPTIONAL, and the reason it is optional: define
//
//   #define OLO_RESTIR_GI_PROBE_IRRADIANCE(worldPos, normal, viewDir)
//   #define OLO_RESTIR_GI_ENVIRONMENT_CUBE  <samplerCube expression>
//
// before including. Undefined, the path tail is zero and the environment is the
// uniform term alone — which is a DARKER but not a WRONG image, and the pass
// counts it rather than leaving it to be noticed.
#ifndef OLO_RESTIR_GI_BOUNCE_GLSL
#define OLO_RESTIR_GI_BOUNCE_GLSL

#ifndef OLO_RESTIR_GI_PROBE_IRRADIANCE
#define OLO_RESTIR_GI_PROBE_IRRADIANCE(worldPos, normal, viewDir) vec3(0.0)
#endif

// ---------------------------------------------------------------------------
// The environment a bounce ray escapes into
// ---------------------------------------------------------------------------

// The SAME pair GpuPathTracer.glsl's EnvironmentRadiance reads, from the same
// uniforms, so a bounce ray that escapes collects what the oracle's escaping ray
// collects. A second definition of "the sky" is how the two stop agreeing about
// an outdoor scene's ambient — and an ambient disagreement is exactly the kind
// of smooth, plausible error the whole tier is organised against.
vec3 OloGIEnvironmentRadiance(vec3 direction)
{
    vec3 radiance = u_Environment.rgb;
#ifdef OLO_RESTIR_GI_ENVIRONMENT_CUBE
    if ((u_EmissiveTable.w & OLO_RESTIR_GI_FLAG_ENVIRONMENT) != 0u)
        radiance += textureLod(OLO_RESTIR_GI_ENVIRONMENT_CUBE, direction, 0.0).rgb * u_Environment.a;
#endif
    return radiance;
}

// ---------------------------------------------------------------------------
// The bounce direction
// ---------------------------------------------------------------------------

// Cosine-weighted over the hemisphere about `n`, with the density it implies.
//
// COSINE-WEIGHTED RATHER THAN UNIFORM, and rather than BSDF-importance-sampled.
// Uniform would waste most of its samples at grazing angles the integrand's own
// cosine kills. BSDF sampling would be better for a glossy x0 — but the SAMPLE
// is a vertex that gets reused at neighbours with different BSDFs, and a source
// density that depended on the generating pixel's material would have to be
// inverted exactly at every reuse. Cosine about the NORMAL depends only on
// geometry, which every reusing pixel already has.
vec3 OloGISampleCosineHemisphere(vec3 n, vec2 xi, out float pdf)
{
    // Concentric-free form: the polar angle straight from the inverse CDF.
    float cosTheta = sqrt(max(1.0 - xi.x, 0.0));
    float sinTheta = sqrt(max(xi.x, 0.0));
    float phi = 6.28318530718 * xi.y;

    // An orthonormal basis with no branch-driven discontinuity — Duff et al.'s
    // signed variant, the same one PBRCommon uses. A basis built from a fixed
    // "up" vector degenerates exactly where the normal is vertical, which in a
    // GI pass is the floor and the ceiling.
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    vec3 t = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    vec3 bt = vec3(b, s + n.y * n.y * a, -n.y);

    vec3 direction = normalize(t * (sinTheta * cos(phi)) + bt * (sinTheta * sin(phi)) + n * cosTheta);
    pdf = cosTheta * (1.0 / 3.14159265359);
    return direction;
}

// ---------------------------------------------------------------------------
// Shading the bounce vertex
// ---------------------------------------------------------------------------

// One next-event-estimation draw at the vertex, returning the IRRADIANCE it
// delivers (radiance times the vertex cosine, divided by the density). The
// caller multiplies by the diffuse albedo over pi.
//
// EVERY LIGHT FAMILY, DIRECTIONAL INCLUDED — which differs from ReSTIR DI's
// candidate sampler on purpose. DI excludes directional lights because the
// clustered loop keeps them for their cascades, their mask channel and their
// cloud shadow; none of that applies at a bounce vertex, and the sun bouncing
// off a floor is exactly the indirect light this tier exists for. Excluding it
// would leave every outdoor scene with GI from the sky alone.
//
// ONE draw rather than a loop: the candidate is itself resampled downstream, and
// a second NEE sample here would cost a second shadow ray per candidate to lower
// a variance the reservoir is already lowering.
vec3 OloGIVertexIrradiance(OloRtHit hit, vec3 vertexNormal, float epsilon, float normalBias,
                           float xiSelect, vec2 xiPoint)
{
    uint lights = min(u_SlotCounts.w, OLO_LIGHT_MAX_SLOTS);
    uint total = lights + u_EmissiveTable.z;
    if (total == 0u)
        return vec3(0.0);

    float selection = clamp(xiSelect, 0.0, 0.9999999);
    uint index = min(uint(selection * float(total)), total - 1u);
    float familyProbability = 1.0 / float(total);

    vec3 origin = hit.Position + vertexNormal * normalBias;
    vec3 radiance = vec3(0.0);
    vec3 shadowTarget = vec3(0.0);
    vec3 toLight = vec3(0.0);
    float pdf = 0.0;

    if (index < lights)
    {
        const GPUSceneLight light = g_GPUSceneLights[index];
        // An inactive slot is a REJECTED draw, not a skipped one: the density
        // stays uniform over the whole slot range rather than over the live
        // subset, which is the same discipline OloReSTIRSampleCandidate keeps
        // and for the same reason — compacting would need a per-frame remap
        // table for a handful of slots.
        if ((light.Flags & OLO_GPU_SCENE_LIGHT_ACTIVE) == 0u)
            return vec3(0.0);

        if (light.Type == OLO_GPU_SCENE_LIGHT_SPHERE_AREA)
        {
            const OloSphereLightView view = OloViewSphereLight(light, hit.Position);
            if (!view.Valid)
                return vec3(0.0);
            float conePdf = OloSphereConePdf(view.CosThetaMax);
            if (!(conePdf > 0.0))
                return vec3(0.0);
            vec3 l = OloSampleSphereLightDirection(light, view, hit.Position, xiPoint);
            float t = OloSphereLightPointDistance(view.Distance, light.DirectionAndRadius.w,
                                                  dot(l, normalize(light.PositionAndRange.xyz - hit.Position)));
            if (!(t > 0.0))
                return vec3(0.0);
            toLight = l;
            shadowTarget = hit.Position + l * t;
            radiance = view.Radiance;
            pdf = familyProbability * conePdf;
        }
        else
        {
            const OloPunctualLightView view =
                OloViewPunctualLight(light, hit.Position, u_EstimatorParams.y);
            if (!view.Valid)
                return vec3(0.0);
            toLight = view.Direction;
            shadowTarget = view.ShadowTarget;
            radiance = view.Radiance;
            // A delta light in a discrete measure: the density IS the selection
            // probability, with no solid-angle factor to apply.
            pdf = familyProbability;
        }
    }
    else
    {
        if (!(u_EstimatorParams.x > 0.0))
            return vec3(0.0);
        // Re-map the selection scalar into [0,1) over the emissive sub-range so
        // the table's own area CDF sees a uniform, not the tail of the light
        // range.
        float xiEmissive =
            clamp((selection * float(total) - float(lights)) / float(u_EmissiveTable.z), 0.0, 0.9999999);
        const OloEmissiveSample emissive = OloSampleEmissiveTriangle(
            OloEmissiveTable(u_EmissiveTable.xy), u_EmissiveTable.z, u_EstimatorParams.x, xiEmissive, xiPoint);
        if (!emissive.Valid)
            return vec3(0.0);
        vec3 delta = emissive.Position - hit.Position;
        float distanceSq = dot(delta, delta);
        if (!(distanceSq > 1e-12))
            return vec3(0.0);
        toLight = delta * inversesqrt(distanceSq);
        float cosLight = dot(emissive.Normal, -toLight);
        if (!((emissive.TwoSided ? abs(cosLight) : cosLight) > 0.0))
            return vec3(0.0);
        shadowTarget = emissive.Position;
        radiance = emissive.Radiance;
        // The SAME area-to-solid-angle conversion the path tracer's NEE uses,
        // from the same table and the same 1/totalArea density.
        pdf = familyProbability * OloEmissiveSolidAnglePdf(emissive, hit.Position);
    }

    if (!(pdf > 0.0))
        return vec3(0.0);
    float nDotL = dot(vertexNormal, toLight);
    if (!(nDotL > 0.0))
        return vec3(0.0);
    if (OloRtIsOccluded(origin, shadowTarget, epsilon))
        return vec3(0.0);

    vec3 irradiance = radiance * nDotL / pdf;
    return OloReservoirFinite(irradiance) ? max(irradiance, vec3(0.0)) : vec3(0.0);
}

// ---------------------------------------------------------------------------
// One candidate
// ---------------------------------------------------------------------------

// Trace one bounce and turn what it finds into a GI sample.
//
// `outSourcePdf` is the density in SOLID ANGLE at the shading point, which is
// the measure the cosine-hemisphere draw is already in and the measure the
// target function is evaluated in — so the initial RIS weight is pHat / pdf with
// no conversion at all. The AREA measure the reservoir STORES the vertex in only
// becomes visible at reuse, as the shift Jacobian (design note §4.1).
//
// `outGlossyVertex` reports that the vertex was polished enough for the dropped
// specular lobe to be a visible fraction of what left it. It is a COUNT, not a
// rejection: rejecting the sample would leave a hole in the estimate, which is
// worse than the term it avoided.
bool OloGITraceBounceCandidate(OloGBufferSurface surface, vec2 xiDirection, float xiLightSelect,
                               vec2 xiLightPoint, out OloGISample giSample, out float outSourcePdf,
                               out bool outGlossyVertex)
{
    giSample = OloMakeEmptyGISample();
    outSourcePdf = 0.0;
    outGlossyVertex = false;

    float pdf;
    vec3 direction = OloGISampleCosineHemisphere(surface.ShadingNormal, xiDirection, pdf);
    if (!(pdf > 0.0))
        return false;

    const float epsilon = u_ReuseParams.z;
    const float normalBias = u_ReuseParams.w;
    vec3 origin = surface.Position + surface.GeometricNormal * normalBias;

    OloRtHit hit;
    if (!OloRtTraceClosest(origin, direction, u_EstimatorParams.y, hit) || !hit.Hit)
    {
        // The ray escaped. The sample is a DIRECTION, and its Jacobian under any
        // reconnection is exactly 1 — derived in design note §4.3 as the limit of
        // the vertex receding, not asserted.
        giSample.Kind = OLO_GI_SAMPLE_ENVIRONMENT;
        giSample.Age = 0u;
        giSample.Position = direction;
        giSample.Normal = vec3(0.0);
        giSample.Radiance = OloGIEnvironmentRadiance(direction);
        outSourcePdf = pdf;
        return true;
    }

    // Geometry the GPU Scene cannot describe. It IS in the way, so reporting the
    // environment through it would brighten a wall's far side; reporting it as a
    // black vertex is what the path tracer does and what keeps the two agreeing.
    if (hit.Unshadeable)
        return false;

    // The ray stopped at a SPHERE LIGHT — an emitter, so what leaves it is
    // EMISSION, and emission arriving with no reflection in between is DIRECT
    // lighting (design note §1.1). Taking it here would double-count every
    // sphere light against ReSTIR DI, exactly. The candidate is dropped and the
    // caller still counts it toward M.
    if (hit.SphereLight >= 0)
        return false;

    // The vertex normal on the side the ray arrived from. A two-sided emitter or
    // a single-sided wall seen from behind would otherwise give a normal facing
    // away, and every Jacobian through it would take the cosine of the wrong
    // hemisphere.
    vec3 vertexNormal = hit.ShadingNormal;
    if (dot(vertexNormal, -direction) < 0.0)
        vertexNormal = -vertexNormal;

    outGlossyVertex = hit.Roughness < u_GIParams.z;

    // L_o(x1): the DIFFUSE lobe only, and NO EMISSION (design note §1.1, §6.1).
    // The diffuse lobe is what makes the stored radiance direction-independent,
    // which is what licenses reconnecting to it without re-tracing; the emission
    // is the direct tier's.
    vec3 diffuseAlbedo = hit.Albedo * (1.0 - clamp(hit.Metallic, 0.0, 1.0));
    vec3 irradiance = OloGIVertexIrradiance(hit, vertexNormal, epsilon, normalBias, xiLightSelect, xiLightPoint);

    // THE PATH TAIL, read AT THE VERTEX (design note §5). This is the whole
    // DDGI hand-off: the probe cache supplies bounces 2 and beyond HERE, at x1,
    // and is not read at x0 while this tier is live. One cache read per path,
    // at a vertex that moves — never two.
    if ((u_EmissiveTable.w & OLO_RESTIR_GI_FLAG_DDGI_TAIL) != 0u)
    {
        vec3 tail = OLO_RESTIR_GI_PROBE_IRRADIANCE(hit.Position, vertexNormal, -direction);
        if (OloReservoirFinite(tail))
            irradiance += max(tail, vec3(0.0));
    }

    vec3 outgoing = diffuseAlbedo * (1.0 / 3.14159265359) * irradiance;
    if (!OloReservoirFinite(outgoing))
        return false;

    giSample.Kind = OLO_GI_SAMPLE_SURFACE_HIT;
    giSample.Age = 0u;
    giSample.Position = hit.Position;
    giSample.Normal = vertexNormal;
    giSample.Radiance = max(outgoing, vec3(0.0));
    outSourcePdf = pdf;
    return true;
}

#endif // OLO_RESTIR_GI_BOUNCE_GLSL
