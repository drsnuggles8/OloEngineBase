// Display output math (issue #1353). Mirrors OloEngine/Renderer/DisplayOutput.h
// term by term; DisplayOutputMathTest pins the CPU side and the display-output
// evidence tests pin this file against it on both backends.
//
// The spaces (DisplayOutput.h explains them in full):
//   scene linear Rec.709, 1.0 = reference white (after exposure)
//   -> display nits (oloToneMapForHdrDisplay)
//   -> COMPOSITION: gamma-encoded, Rec.709, 1.0 = UI white (oloNitsToComposition)
//   -> the swapchain's encoding (oloEncodeCompositionForDisplay)

#ifndef OLO_DISPLAY_ENCODE_GLSL
#define OLO_DISPLAY_ENCODE_GLSL

#define OLO_DISPLAY_SDR   0
#define OLO_DISPLAY_HDR10 1
#define OLO_DISPLAY_SCRGB 2

#define OLO_PQ_MAX_NITS 10000.0
#define OLO_SCRGB_REFERENCE_NITS 80.0

// SMPTE ST 2084 inverse EOTF: nits -> PQ signal [0, 1].
float oloPqEncodeNits(float nits)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float y = clamp(nits / OLO_PQ_MAX_NITS, 0.0, 1.0);
    float ym1 = pow(y, m1);
    return pow((c1 + c2 * ym1) / (1.0 + c3 * ym1), m2);
}

// SMPTE ST 2084 EOTF: PQ signal [0, 1] -> nits.
float oloPqDecodeToNits(float signal)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float ep = pow(clamp(signal, 0.0, 1.0), 1.0 / m2);
    return pow(max(ep - c1, 0.0) / (c2 - c3 * ep), 1.0 / m1) * OLO_PQ_MAX_NITS;
}

// Linear Rec.709 -> linear Rec.2020, ITU-R BT.2087 (columns of the row matrix).
vec3 oloRec709ToRec2020(vec3 rgb709)
{
    const mat3 m = mat3(
        vec3(0.627403896, 0.069097289, 0.016391439),
        vec3(0.329283039, 0.919540395, 0.088013308),
        vec3(0.043313065, 0.011362316, 0.895595253));
    return m * rgb709;
}

// ITU-R BT.2390-3 EETF: PQ signal mastered for [0, 10000] nits onto a display
// with [minNits, peakNits].
float oloBt2390Eetf(float pqSignal, float minNits, float peakNits)
{
    float e1 = clamp(pqSignal, 0.0, 1.0);
    float maxLum = oloPqEncodeNits(peakNits);
    float minLum = oloPqEncodeNits(minNits);
    float ks = 1.5 * maxLum - 0.5;
    float e2 = e1;
    if (e1 >= ks && ks < 1.0)
    {
        float t = (e1 - ks) / (1.0 - ks);
        float t2 = t * t;
        float t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * maxLum;
    }
    float oneMinus = 1.0 - e2;
    return e2 + minLum * oneMinus * oneMinus * oneMinus * oneMinus;
}

// Scene linear (1.0 = reference white) -> display nits. The EETF acts on
// max(R, G, B) and the colour is scaled by the ratio; clipOnly clamps instead.
vec3 oloToneMapForHdrDisplay(vec3 sceneLinear, float paperWhiteNits, float peakNits, float minNits, bool clipOnly)
{
    vec3 nits = max(sceneLinear, vec3(0.0)) * paperWhiteNits;
    if (clipOnly)
        return min(nits, vec3(peakNits));
    float maxNits = max(nits.r, max(nits.g, nits.b));
    if (maxNits <= 0.0)
        return vec3(oloPqDecodeToNits(oloBt2390Eetf(0.0, minNits, peakNits)));
    float mapped = oloPqDecodeToNits(oloBt2390Eetf(oloPqEncodeNits(min(maxNits, OLO_PQ_MAX_NITS)), minNits, peakNits));
    return nits * (mapped / maxNits);
}

// DisplayOutput.h kMinHdrCompositionGamma: below it the brightest HDR
// composition value no longer fits in RGBA16F.
#define OLO_MIN_HDR_COMPOSITION_GAMMA 0.5

// Display nits -> composition (1.0 = UI white, gamma-encoded).
vec3 oloNitsToComposition(vec3 nits, float uiWhiteNits, float gamma)
{
    float g = max(gamma, OLO_MIN_HDR_COMPOSITION_GAMMA);
    return pow(max(nits / uiWhiteNits, vec3(0.0)), vec3(1.0 / g));
}

// Composition -> what the swapchain stores for `mode`.
vec3 oloEncodeCompositionForDisplay(vec3 composition, int mode, float gamma, float uiWhiteNits, float peakNits)
{
    if (mode == OLO_DISPLAY_SDR)
        return composition;
    float g = max(gamma, OLO_MIN_HDR_COMPOSITION_GAMMA);
    vec3 nits709 = pow(max(composition, vec3(0.0)), vec3(g)) * uiWhiteNits;
    if (mode == OLO_DISPLAY_HDR10)
    {
        vec3 nits2020 = clamp(oloRec709ToRec2020(nits709), vec3(0.0), vec3(peakNits));
        return vec3(oloPqEncodeNits(nits2020.r), oloPqEncodeNits(nits2020.g), oloPqEncodeNits(nits2020.b));
    }
    return min(nits709, vec3(peakNits)) / OLO_SCRGB_REFERENCE_NITS;
}

#endif // OLO_DISPLAY_ENCODE_GLSL
