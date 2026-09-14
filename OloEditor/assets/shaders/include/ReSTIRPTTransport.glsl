#ifndef OLO_RESTIR_PT_TRANSPORT
#define OLO_RESTIR_PT_TRANSPORT
vec3 PTOrigin(PTVertex v, vec3 direction)
{
    float side = dot(v.GeometricNormalMetallic.xyz, direction) >= 0.0 ? 1.0 : -1.0;
    float bias = v.Identity.x == 0xffffffffu ? max(u_Params.x, u_Params.y) : u_Params.y;
    return v.PositionRoughness.xyz + v.GeometricNormalMetallic.xyz * (side * bias);
}
bool PTTrace(PTVertex v, vec3 direction, out OloRtHit hit)
{
    PTCount(0u);
    return OloRtTraceClosest(PTOrigin(v, direction), direction, u_Params.z, hit);
}
bool PTVisible(PTVertex v, vec3 target)
{
    vec3 delta = target - v.PositionRoughness.xyz;
    if (length(delta) <= 4.0 * u_Params.y)
        return false;
    vec3 origin = PTOrigin(v, normalize(delta));
    if (length(target - origin) <= 2.0 * u_Params.y)
        return false;
    PTCount(0u);
    PTCount(1u);
    return !OloRtIsOccluded(origin, target, u_Params.y);
}
vec3 PTEnvironment(vec3 direction)
{
    if ((u_EmissiveTable.w & OLO_RESTIR_PT_FLAG_ENVIRONMENT) != 0u)
        return textureLod(u_EnvironmentCube, direction, 0.0).rgb * u_EstimatorParams.y;
    return u_Environment.rgb;
}
PTVertex PTFromHit(OloRtHit hit, vec3 view)
{
    PTVertex v = PTEmptyVertex();
    v.PositionRoughness = vec4(hit.Position, hit.Roughness);
    float side = dot(hit.GeometricNormal, view) >= 0.0 ? 1.0 : -1.0;
    v.GeometricNormalMetallic = vec4(hit.GeometricNormal * side, hit.Metallic);
    v.ShadingNormalClosure = vec4(hit.ShadingNormal * side, float(hit.ClosureVersion));
    v.Albedo = vec4(hit.Albedo, 1.0);
    v.Incoming = vec4(view, 0);
    v.Identity = hit.Identity;
    if (hit.Roughness < u_Params.w)
        PTCount(12u);
    return v;
}
uint PTGroups()
{
    return min(u_SlotCounts.w, OLO_LIGHT_MAX_SLOTS) + (u_EmissiveTable.z > 0u ? 1u : 0u);
}
float PTPower(float a, float b)
{
    float m = max(a, b);
    if (!(m > 0.0))
        return 0.0;
    a /= m;
    b /= m;
    return a * a / (a * a + b * b);
}
vec3 PTDirection(PTPath p, uint k)
{
    // Tracing offsets the ray origin. Subtracting the unoffset receiver from
    // the hit would change the sampled solid angle and its chart density.
    // The next vertex stores the actual arriving ray direction exactly.
    if (k < p.Metadata.x)
        return -p.Vertices[k + 1u].Incoming.xyz;
    if (p.Metadata.y == 1u)
    {
        vec3 direction;
        if (PTForward(p.Vertices[k], direction))
            return direction;
        return vec3(0);
    }
    if (p.Endpoint.PositionKind.w < 1.5)
        return p.Endpoint.PositionKind.xyz;
    return normalize(p.Endpoint.PositionKind.xyz - p.Vertices[k].PositionRoughness.xyz);
}
void PTRandomVertex(inout PTVertex v, inout OloPathSampler pathSampler)
{
    v.Randoms.x = oloPtGet1D(pathSampler);
    v.Randoms.yz = oloPtGet2D(pathSampler);
    v.Randoms.w = v.Randoms.x < PTProbability(v) ? 1.0 : 0.0;
}
bool PTNEE(inout PTPath p, bool resample)
{
    uint n = p.Metadata.x;
    PTVertex v = p.Vertices[n];
    uint groups = PTGroups();
    if (groups == 0u)
        return false;
    float selector = p.Endpoint.Randoms.x * float(groups);
    uint slot = min(uint(selector), groups - 1u);
    float mass = 1.0 / float(groups);
    uint lightCount = min(u_SlotCounts.w, OLO_LIGHT_MAX_SLOTS);
    if (slot < lightCount)
    {
        GPUSceneLight light = g_GPUSceneLights[slot];
        if ((light.Flags & OLO_GPU_SCENE_LIGHT_ACTIVE) == 0u)
            return false;
        OloPunctualLightView view = OloViewPunctualLight(light, v.PositionRoughness.xyz, u_Params.z);
        if (!view.Valid)
            return false;
        if (!resample && p.Endpoint.Identity.x != slot)
            return false;
        p.Endpoint.PositionKind = vec4(view.ShadowTarget, view.Directional ? 4.0 : 3.0);
        p.Endpoint.RadianceDensity = vec4(view.Radiance, mass);
        p.Endpoint.NormalTwoSided = vec4(0);
        p.Endpoint.Identity = uvec4(slot, 0, 0, 0);
    }
    else
    {
        OloEmissiveSample s = OloSampleEmissiveTriangle(OloEmissiveTable(u_EmissiveTable.xy), u_EmissiveTable.z,
                                                        u_EstimatorParams.x, fract(selector), p.Endpoint.Randoms.yz);
        if (!s.Valid)
            return false;
        if (!resample && p.Endpoint.Identity.x != s.TriangleIndex)
            return false;
        p.Endpoint.PositionKind = vec4(s.Position, 2.0);
        p.Endpoint.NormalTwoSided = vec4(s.Normal, s.TwoSided ? 1.0 : 0.0);
        p.Endpoint.RadianceDensity = vec4(s.Radiance, mass * s.PdfArea);
        p.Endpoint.Identity = uvec4(s.TriangleIndex, 0, 0, 0);
    }
    p.Endpoint.Randoms.w = mass;
    return PTVisible(v, p.Endpoint.PositionKind.xyz);
}
bool PTBSDFEndpoint(inout PTPath p)
{
    uint n = p.Metadata.x;
    PTVertex v = p.Vertices[n];
    vec3 l;
    if (!PTForward(v, l))
        return false;
    OloRtHit hit;
    if (!PTTrace(v, l, hit))
    {
        p.Endpoint.PositionKind = vec4(l, 1.0);
        p.Endpoint.NormalTwoSided = vec4(0);
        p.Endpoint.RadianceDensity = vec4(PTEnvironment(l), 1.0);
        p.Endpoint.Identity = uvec4(0xffffffffu);
        return true;
    }
    if (n == 0u || hit.Unshadeable || hit.SphereLight >= 0)
        return false;
    float c = dot(hit.GeometricNormal, -l);
    if (hit.TwoSidedEmission)
        c = abs(c);
    if (!(c > 0.0) || !any(greaterThan(hit.Emissive, vec3(0))))
        return false;
    p.Endpoint.PositionKind = vec4(hit.Position, 2.0);
    p.Endpoint.NormalTwoSided = vec4(hit.GeometricNormal, hit.TwoSidedEmission ? 1.0 : 0.0);
    p.Endpoint.RadianceDensity = vec4(hit.Emissive, 1.0);
    p.Endpoint.Identity = hit.Identity;
    return true;
}
// Evaluate one terminal contribution in the stored extended chart measure.
// Initial pdf is separate; no throughput clamping or hidden Russian roulette.
bool PTEvaluatePath(inout PTPath p, out float proposal)
{
    uint n = p.Metadata.x;
    proposal = 0.125;
    vec3 value = vec3(1);
    if (n > 3u || p.Endpoint.PositionKind.w < 0.5)
        return false;
    if (n == 0u && (p.Metadata.y == 0u || p.Endpoint.PositionKind.w > 1.5))
        return false;
    for (uint k = 0u; k < 4u; ++k)
    {
        if (k > n)
            break;
        PTVertex v = p.Vertices[k];
        vec3 l = PTDirection(p, k);
        float cosine = dot(v.ShadingNormalClosure.xyz, l);
        if (!(cosine > 0.0) || !PTFinite3(l))
            return false;
        vec3 factor = PTEvaluate(v, l) * cosine;
        if (k < n || p.Metadata.y == 1u)
        {
            float c = PTChartDensity(v, l);
            float mixture = PTMixture(v, l);
            if (!(c > 0.0) || !(mixture > 0.0))
                return false;
            factor *= c / mixture;
            proposal *= c;
        }
        value *= factor;
    }
    PTVertex last = p.Vertices[n];
    vec3 l = PTDirection(p, n);
    if (p.Metadata.y == 0u)
    {
        float density = p.Endpoint.RadianceDensity.w;
        if (!(density > 0.0))
            return false;
        proposal *= density;
        if (p.Endpoint.PositionKind.w < 2.5)
        {
            vec3 delta = p.Endpoint.PositionKind.xyz - last.PositionRoughness.xyz;
            float cosEmitter = dot(p.Endpoint.NormalTwoSided.xyz, -l);
            if (p.Endpoint.NormalTwoSided.w > 0.5)
                cosEmitter = abs(cosEmitter);
            if (!(cosEmitter > 0.0) || !(dot(delta, delta) > 0.0))
                return false;
            float geometry = cosEmitter / dot(delta, delta);
            value *= geometry * PTPower(density / geometry, PTMixture(last, l));
        }
    }
    else if (p.Endpoint.PositionKind.w > 1.5)
    {
        vec3 delta = p.Endpoint.PositionKind.xyz - last.PositionRoughness.xyz;
        float cosEmitter = dot(p.Endpoint.NormalTwoSided.xyz, -l);
        if (p.Endpoint.NormalTwoSided.w > 0.5)
            cosEmitter = abs(cosEmitter);
        if (!(cosEmitter > 0.0))
            return false;
        float areaDensity = PTGroups() > 0u ? u_EstimatorParams.x / float(PTGroups()) : 0.0;
        value *= PTPower(PTMixture(last, l), areaDensity * dot(delta, delta) / cosEmitter);
    }
    value *= p.Endpoint.RadianceDensity.rgb;
    if (!PTFinite3(value) || !PTFinite(proposal) || !(proposal > 0.0))
    {
        PTCount(13u);
        return false;
    }
    p.Value = vec4(value, 0);
    p.State.z = dot(value, vec3(0.2126, 0.7152, 0.0722));
    return PTFinite(p.State.z) && p.State.z >= 0.0;
}
PTPath PTGenerate(PTVertex receiver, uint pixel, inout OloPathSampler pathSampler)
{
    PTPath p = PTEmpty();
    p.Vertices[0] = receiver;
    uint secondaryCount = min(uint(oloPtGet1D(pathSampler) * 4.0), 3u);
    uint strategy = oloPtGet1D(pathSampler) < 0.5 ? 0u : 1u;
    p.Metadata.xy = uvec2(secondaryCount, strategy);
    p.Lineage = uvec4(u_TlasAddressAndFrame.w, pixel, 0, OLO_RESTIR_PT_LAYOUT_BITS | 1u);
    // Consume a fixed padded schedule even when the chosen terminal is earlier.
    vec4 randoms[4];
    for (uint k = 0u; k < 4u; ++k)
    {
        float lobe = oloPtGet1D(pathSampler);
        vec2 angular = oloPtGet2D(pathSampler);
        randoms[k] = vec4(lobe, angular, 0);
        p.Vertices[k].Randoms = randoms[k];
    }
    float selector = oloPtGet1D(pathSampler);
    vec2 endpointPoint = oloPtGet2D(pathSampler);
    p.Endpoint.Randoms = vec4(selector, endpointPoint, 0);
    for (uint k = 0u; k < 4u; ++k)
    {
        if (k > p.Metadata.x)
            break;
        p.Vertices[k].Randoms = randoms[k];
        p.Vertices[k].Randoms.w = p.Vertices[k].Randoms.x < PTProbability(p.Vertices[k]) ? 1.0 : 0.0;
        if (k == p.Metadata.x)
            break;
        vec3 l;
        if (!PTForward(p.Vertices[k], l))
            return p;
        OloRtHit hit;
        if (!PTTrace(p.Vertices[k], l, hit) || hit.Unshadeable || hit.SphereLight >= 0)
            return p;
        p.Vertices[k + 1u] = PTFromHit(hit, -l);
    }
    if (p.Metadata.y == 0u)
    {
        if (p.Metadata.x == 0u || !PTNEE(p, true))
            return p;
    }
    else if (!PTBSDFEndpoint(p))
        return p;
    float proposal;
    if (!PTEvaluatePath(p, proposal))
    {
        p.State.z = 0.0;
        p.Value = vec4(0);
        return p;
    }
    if (p.State.z > 0.0)
    {
        p.State.x = 1.0 / proposal;
        if (!PTFinite(p.State.x))
        {
            p.State.x = 0.0;
            PTCount(13u);
            return p;
        }
        p.Lineage.w |= 2u;
        PTCount(3u);
    }
    return p;
}
#endif
