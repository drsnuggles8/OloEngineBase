// Shared path tracer and ReSTIR PT material evaluation and sampling.
#ifndef OLO_PATH_TRACER_BSDF_GLSL
#define OLO_PATH_TRACER_BSDF_GLSL

vec3 PtEvaluateBRDF(PtHit hit, vec3 n, vec3 v, vec3 l)
{
    return closureV2Evaluate(n, v, l, hit.Albedo, hit.Metallic, hit.Roughness);
}

float PtBsdfPdf(PtHit hit, vec3 n, vec3 v, vec3 l)
{
    return closureV2Pdf(n, v, l, hit.Albedo, hit.Metallic, hit.Roughness);
}

ClosureV2Sample PtSampleBRDF(PtHit hit, vec3 n, vec3 v, float lobeXi, vec2 xi)
{
    return closureV2SampleBRDF(n, v, hit.Albedo, hit.Metallic, hit.Roughness, lobeXi, xi);
}

#endif
