// =============================================================================
// ScreenSpaceAOSampling.glsl — reading the screen-space AO buffer at full
// resolution (SSAO / GTAO, with the sphere-proxy term folded in).
//
// ONE UPSAMPLE, EVERY CONSUMER (issues #1336, #1452). The AO buffer is a
// VISIBILITY signal for the AMBIENT term — the indirect light a ladder rung
// assumed arrives unoccluded — and every path applies it there
// (OloEngine::SelectScreenSpaceAOApplication):
//
//   - Deferred: DeferredLighting multiplies the ambient split by it.
//   - Forward / Forward+: the depth prepass writes the view normals the AO
//     passes read, the AO passes run before forward colour, and every forward
//     shader multiplies its ambient term by it through
//     include/ForwardScreenSpaceAO.glsl.
//   - PostProcess_SSAOApply draws only the AO debug view.
//
// All of them call this function, so the value a pixel is multiplied by is the
// same whichever consumer applies it. The caller defines OLO_SSAO_TAP_DEPTH(uv) to
// return the FULL-RESOLUTION depth-buffer value at `uv` (a [0,1] window depth),
// before including this file.
// =============================================================================

#ifndef SCREEN_SPACE_AO_SAMPLING_GLSL
#define SCREEN_SPACE_AO_SAMPLING_GLSL

#ifndef OLO_SSAO_TAP_DEPTH
#error "Define OLO_SSAO_TAP_DEPTH(uv) before including ScreenSpaceAOSampling.glsl"
#endif

// View-space distance from a window depth, with the reconstruction
// projection's (2,2) / (3,2) coefficients — the pair SSAORenderPass uploads.
float oloLinearizeScreenAODepth(float depth, float projA, float projB)
{
    float ndc = depth * 2.0 - 1.0;
    return projB / (projA + ndc);
}

// Depth-aware bilateral upsample: sample the 4 nearest AO texels and weight
// them by depth similarity so a half-resolution AO does not bleed across depth
// discontinuities (blurry dark halos). Returns visibility in [0,1]: 1 = open.
// A non-finite result reads as fully open, never as black.
float oloSampleScreenSpaceAO(sampler2D aoTexture, vec2 uv, float projA, float projB)
{
    vec2 aoTexSize = vec2(textureSize(aoTexture, 0));
    vec2 texelSize = 1.0 / aoTexSize;

    // Map full-res UV to AO texel coordinates
    vec2 aoCoord = uv * aoTexSize - 0.5;
    vec2 baseCoord = floor(aoCoord);
    vec2 frac = aoCoord - baseCoord;

    // Full-res center depth for bilateral weighting
    float centerDepth = oloLinearizeScreenAODepth(OLO_SSAO_TAP_DEPTH(uv), projA, projB);

    // Depth sensitivity — controls edge sharpness
    float depthSigma = 0.02 * abs(centerDepth);
    depthSigma = max(depthSigma, 0.01);
    float invTwoSigmaSq = 1.0 / (2.0 * depthSigma * depthSigma);

    float totalWeight = 0.0;
    float totalAO = 0.0;

    for (int dy = 0; dy <= 1; ++dy)
    {
        for (int dx = 0; dx <= 1; ++dx)
        {
            vec2 sampleUV = (baseCoord + vec2(float(dx), float(dy)) + 0.5) * texelSize;
            sampleUV = clamp(sampleUV, texelSize * 0.5, 1.0 - texelSize * 0.5);

            float sampleAO = texture(aoTexture, sampleUV).r;

            // Approximate depth at this AO sample location using full-res depth
            float sampleDepth = oloLinearizeScreenAODepth(OLO_SSAO_TAP_DEPTH(sampleUV), projA, projB);

            // Bilinear weight
            float bx = (dx == 0) ? (1.0 - frac.x) : frac.x;
            float by = (dy == 0) ? (1.0 - frac.y) : frac.y;
            float bilinearWeight = bx * by;

            // Depth weight — suppress samples across depth discontinuities
            float depthDiff = centerDepth - sampleDepth;
            float depthWeight = exp(-depthDiff * depthDiff * invTwoSigmaSq);

            float w = bilinearWeight * depthWeight;
            totalAO += sampleAO * w;
            totalWeight += w;
        }
    }

    float ao = (totalWeight > 0.001) ? totalAO / totalWeight : texture(aoTexture, uv).r;
    if (isnan(ao) || isinf(ao))
        ao = 1.0;
    return clamp(ao, 0.0, 1.0);
}

// The strength slider, applied the one way both consumers apply it.
float oloScreenSpaceAOVisibility(float ao, float intensity)
{
    return mix(1.0, ao, intensity);
}

#endif // SCREEN_SPACE_AO_SAMPLING_GLSL
