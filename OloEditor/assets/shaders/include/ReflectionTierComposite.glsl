// =============================================================================
// ReflectionTierComposite.glsl - one reflection tier's hand-off (issue #1325).
//
// ADR 0020's algebra is over R, the radiance along the specular lobe. The frame
// colour is not R: it is diffuse + direct + emission + transmission + the
// INDIRECT SPECULAR TERM S = W * R, where W is the lobe's BRDF weight per unit
// of incident radiance -- split-sum (F * A + B), times the ambient occlusion and
// the skin profile's tint, exactly as DeferredLighting composed it. A tier
// replaces S and nothing else:
//
//     S' = S + c * (W * L - S)        C' = C + (S' - S)
//
// S is the term the colour holds NOW: the lighting pass's own for the first
// tier, the previous tier's S' for the next. Chained, that is the ADR's "over"
// on R, scaled by W. Confidence says how much of the lobe the tier's estimate L
// may answer for; Fresnel lives in W and never in c. A black hit removes c * S
// and leaves every other term of the colour alone -- the old mix(C, L, c) scaled
// diffuse, emission and direct light by (1 - c) as well.
//
// TWIN OF ComposeSpecularTier in OloEngine/Renderer/ReflectionTier.h.
// =============================================================================
#ifndef OLO_REFLECTION_TIER_COMPOSITE_GLSL
#define OLO_REFLECTION_TIER_COMPOSITE_GLSL

// The SPECULAR TERM this tier adds to the colour: c * (W * L - S). Signed -- a
// reflection darker than the term it replaces is legitimate.
vec3 oloSpecularTierDelta(vec3 specular, vec3 weight, vec3 radiance, float confidence)
{
    // The same NaN-first clamp as ComputeReflectionTierWeights: a broken
    // confidence answers nothing.
    float c = (confidence > 0.0) ? min(confidence, 1.0) : 0.0;
    return c * (weight * radiance - specular);
}

#endif // OLO_REFLECTION_TIER_COMPOSITE_GLSL
