// Shared oracle BSDF dispatch. Include after PBRCommon and the PtHit alias;
// OLO_PT_CLOSURE_V2 names the material closure version. Extracted unchanged
// from GpuPathTracer.glsl so replay and reference use one sampling density.
#ifndef OLO_PATH_TRACER_BSDF_GLSL
#define OLO_PATH_TRACER_BSDF_GLSL

float PtLegacySamplingRoughness(float roughness)
{
    return clamp(roughness, MIN_ROUGHNESS, 1.0);
}

// From the vectors, with PBRCommon's cancellation-free distributionGGXUnclampedNH
// (issue #1347). Twin: ReferenceBRDF.h's vector PdfGGX.
float PtLegacyPdfGGX(vec3 n, vec3 v, vec3 h, float roughness)
{
    const float vDotH = dot(v, h);
    if (vDotH <= 0.0)
        return 0.0;
    return distributionGGXUnclampedNH(n, h, roughness) * max(dot(n, h), 0.0) / (4.0 * vDotH);
}

vec3 PtLegacyImportanceSampleGGX(vec2 xi, vec3 n, float roughness)
{
    const float a = roughness * roughness;
    const float phi = 2.0 * PI * xi.x;
    // Same cancellation-free sin as MathCommon.glsl ImportanceSampleGGX (#1347).
    const float denom = 1.0 + (a * a - 1.0) * xi.y;
    const float cosTheta = sqrt(max(0.0, (1.0 - xi.y) / denom));
    const float sinTheta = sqrt(max(0.0, a * a * xi.y / denom));
    const vec3 h = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    vec3 tangent, bitangent;
    OrthonormalBasis(n, tangent, bitangent);
    return normalize(tangent * h.x + bitangent * h.y + n * h.z);
}

float PtLegacyPdf(vec3 n, vec3 v, vec3 l, vec3 albedo, float metallic, float roughness)
{
    const float nDotL = dot(n, l);
    if (nDotL <= 0.0)
        return 0.0;
    const float pdfDiffuse = max(nDotL, 0.0) * INV_PI;
    const float pSpecular = closureV2SpecularProbability(albedo, metallic);
    if (!(pSpecular > 0.0))
        return pdfDiffuse;
    const vec3 h = normalize(v + l);
    const float pdfSpecular = PtLegacyPdfGGX(n, v, h, PtLegacySamplingRoughness(roughness));
    return pSpecular * pdfSpecular + (1.0 - pSpecular) * pdfDiffuse;
}

ClosureV2Sample PtLegacySampleBRDF(vec3 n, vec3 v, vec3 albedo, float metallic, float roughness, float lobeXi,
                                   vec2 xi)
{
    ClosureV2Sample result;
    result.L = vec3(0.0);
    result.Value = vec3(0.0);
    result.Pdf = 0.0;
    const float pSpecular = closureV2SpecularProbability(albedo, metallic);
    vec3 l;
    if (lobeXi < pSpecular)
    {
        const vec3 h = PtLegacyImportanceSampleGGX(xi, n, PtLegacySamplingRoughness(roughness));
        l = reflect(-v, h);
    }
    else
    {
        l = closureV2CosineSampleHemisphere(xi, n);
    }
    if (dot(n, l) <= 0.0)
        return result;
    const float pdf = PtLegacyPdf(n, v, l, albedo, metallic, roughness);
    if (!(pdf > 0.0))
        return result;
    result.L = l;
    result.Value = cookTorranceBRDF(n, v, l, albedo, metallic, roughness);
    result.Pdf = pdf;
    return result;
}

vec3 PtEvaluateBRDF(PtHit hit, vec3 n, vec3 v, vec3 l)
{
    if (hit.ClosureVersion == OLO_PT_CLOSURE_V2)
        return closureV2Evaluate(n, v, l, hit.Albedo, hit.Metallic, hit.Roughness);
    return cookTorranceBRDF(n, v, l, hit.Albedo, hit.Metallic, hit.Roughness);
}

float PtBsdfPdf(PtHit hit, vec3 n, vec3 v, vec3 l)
{
    if (hit.ClosureVersion == OLO_PT_CLOSURE_V2)
        return closureV2Pdf(n, v, l, hit.Albedo, hit.Metallic, hit.Roughness);
    return PtLegacyPdf(n, v, l, hit.Albedo, hit.Metallic, hit.Roughness);
}

ClosureV2Sample PtSampleBRDF(PtHit hit, vec3 n, vec3 v, float lobeXi, vec2 xi)
{
    if (hit.ClosureVersion == OLO_PT_CLOSURE_V2)
        return closureV2SampleBRDF(n, v, hit.Albedo, hit.Metallic, hit.Roughness, lobeXi, xi);
    return PtLegacySampleBRDF(n, v, hit.Albedo, hit.Metallic, hit.Roughness, lobeXi, xi);
}

#endif
