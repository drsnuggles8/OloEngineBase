#ifndef OLO_RESTIR_PT_CHARTS
#define OLO_RESTIR_PT_CHARTS
// Chart coordinates: Randoms.xyz = raw lobe selector, two angular uniforms;
// Randoms.w = discrete selected chart (0 diffuse, 1 specular).
float PTProbability(PTVertex v)
{
    return closureV2SpecularProbability(v.Albedo.rgb, v.GeometricNormalMetallic.w);
}
float PTBranchMass(PTVertex v)
{
    float q = PTProbability(v);
    return v.Randoms.w > 0.5 ? q : 1.0 - q;
}
float PTResidual(PTVertex v)
{
    float q = PTProbability(v);
    return v.Randoms.w > 0.5 ? v.Randoms.x / q : (v.Randoms.x - q) / (1.0 - q);
}
bool PTBranchValid(PTVertex v)
{
    return PTOpen(v.Randoms.x) && PTOpen(v.Randoms.y) && PTOpen(v.Randoms.z) &&
           ((v.Randoms.x < PTProbability(v)) == (v.Randoms.w > 0.5)) && PTOpen(PTResidual(v));
}
OloRtHit PTHitFromVertex(PTVertex v)
{
    OloRtHit h;
    h.Albedo = v.Albedo.rgb;
    h.Metallic = v.GeometricNormalMetallic.w;
    h.Roughness = v.PositionRoughness.w;
    h.ClosureVersion = uint(v.ShadingNormalClosure.w);
    return h;
}
vec3 PTEvaluate(PTVertex v, vec3 l)
{
    return PtEvaluateBRDF(PTHitFromVertex(v), v.ShadingNormalClosure.xyz, v.Incoming.xyz, l);
}
float PTMixture(PTVertex v, vec3 l)
{
    return PtBsdfPdf(PTHitFromVertex(v), v.ShadingNormalClosure.xyz, v.Incoming.xyz, l);
}
float PTChartDensity(PTVertex v, vec3 l)
{
    vec3 n = v.ShadingNormalClosure.xyz;
    vec3 view = v.Incoming.xyz;
    if (!PTBranchValid(v) || dot(n, l) <= 0.0 || dot(n, view) <= 0.0)
        return 0.0;
    float conditional = dot(n, l) * INV_PI;
    if (v.Randoms.w > 0.5)
    {
        vec3 h = normalize(view + l);
        float r = closureV2Roughness(v.PositionRoughness.w);
        conditional = uint(v.ShadingNormalClosure.w) == OLO_RT_CLOSURE_V2 ? distributionGGXUnclamped(max(dot(n, h), 0.0), r) / (4.0 * dot(n, view) * (1.0 + ggxSmithLambda(dot(n, view), r * r))) : PtLegacyPdfGGX(dot(n, h), dot(view, h), r);
    }
    float c = PTBranchMass(v) * conditional;
    return PTFinite(c) && c > 0.0 ? c : 0.0;
}
bool PTForward(PTVertex v, out vec3 l)
{
    l = vec3(0);
    if (!PTBranchValid(v) || dot(v.ShadingNormalClosure.xyz, v.Incoming.xyz) <= 0.0)
        return false;
    ClosureV2Sample s = PtSampleBRDF(PTHitFromVertex(v), v.ShadingNormalClosure.xyz, v.Incoming.xyz, v.Randoms.x, v.Randoms.yz);
    // Match the oracle's shading-normal hemisphere. Reconnection applies its
    // stronger geometric-front-side domain separately; imposing it on fresh
    // sampling would discard the oracle's normal-mapped transport.
    l = s.L;
    return s.Pdf > 0.0 && PTFinite3(l);
}
float PTAzimuth(vec2 p)
{
    float a = atan(p.y, p.x) / TWO_PI;
    return a < 0.0 ? a + 1.0 : a;
}
bool PTInverseDirection(inout PTVertex v, vec3 l, float residual)
{
    // Reconnection keeps the chart label and residual, but changes angular
    // uniforms. Random replay never invokes this inverse on sharp charts.
    vec3 n = v.ShadingNormalClosure.xyz;
    vec3 view = v.Incoming.xyz;
    if (!PTOpen(residual) || dot(n, l) <= 0.0 || dot(n, view) <= 0.0)
        return false;
    vec3 tangent, bitangent;
    OrthonormalBasis(n, tangent, bitangent);
    vec2 xi;
    if (v.Randoms.w < 0.5)
    {
        vec2 d = vec2(dot(l, tangent), dot(l, bitangent));
        xi = vec2(dot(d, d), PTAzimuth(d));
    }
    else
    {
        if (v.PositionRoughness.w < u_Params.w)
            return false;
        vec3 halfVector = normalize(view + l);
        vec3 h = vec3(dot(halfVector, tangent), dot(halfVector, bitangent), dot(halfVector, n));
        if (h.z <= 0.0 || dot(view, halfVector) <= 0.0)
            return false;
        float r = closureV2Roughness(v.PositionRoughness.w);
        float alpha = r * r;
        if (uint(v.ShadingNormalClosure.w) == OLO_RT_CLOSURE_LEGACY)
        {
            float sinSq = dot(h.xy, h.xy);
            xi = vec2(PTAzimuth(h.xy), sinSq / (sinSq + alpha * alpha * h.z * h.z));
        }
        else
        {
            vec3 ve = vec3(dot(view, tangent), dot(view, bitangent), dot(view, n));
            vec3 vh = normalize(vec3(alpha * ve.xy, ve.z));
            float lenSq = dot(vh.xy, vh.xy);
            vec3 t1 = lenSq > 0.0 ? vec3(-vh.y, vh.x, 0.0) / sqrt(lenSq) : vec3(1, 0, 0);
            vec3 t2 = cross(vh, t1);
            vec3 nh = normalize(vec3(h.xy / alpha, h.z));
            float p1 = dot(nh, t1);
            float rad = 1.0 - p1 * p1;
            if (rad <= 0.0)
                return false;
            float s = 0.5 * (1.0 + vh.z);
            float p2 = (dot(nh, t2) - (1.0 - s) * sqrt(rad)) / s;
            xi = vec2(p1 * p1 + p2 * p2, PTAzimuth(vec2(p1, p2)));
        }
    }
    if (!PTOpen(xi.x) || !PTOpen(xi.y))
        return false;
    float q = PTProbability(v);
    v.Randoms.x = v.Randoms.w > 0.5 ? q * residual : q + (1.0 - q) * residual;
    v.Randoms.yz = xi;
    vec3 reconstructed;
    return PTForward(v, reconstructed) && length(reconstructed - l) < 2e-5;
}
#endif
