#ifndef OLO_GROOM_SHADOW_WIDENING_GLSL
#define OLO_GROOM_SHADOW_WIDENING_GLSL

// =============================================================================
// GroomShadowWidening.glsl — one strand segment, widened to at least one shadow
// texel and projected into a light's clip space. Issue #1323.
//
// ONE FILE BECAUSE THERE ARE TWO CONSUMERS AND THEY MUST NOT DRIFT:
// GroomStrandDepth.glsl (CSM cascades and the local-light atlas) and
// VSM_GroomDepth.glsl (the Virtual Shadow Map clip levels). A family reaches a
// technique only if somebody wired it there, and two techniques wired with two
// copies of this arithmetic would disagree about where a coat's shadow is by an
// amount nothing detects — which is the shape of the gap
// virtual-geometry-into-a-second-shadow-technique.md was written about.
//
// C++ TWIN: GroomShadowWidening.h — OloGroomShadowNdcPerWorld and
// OloGroomShadowHalfWidthNdc are mirrored there so the floor can be asserted
// without a GPU, the same twin discipline GroomStrandCommon.glsl and
// GroomCoverage.h keep.
// =============================================================================

// NDC per world metre, for an offset roughly PERPENDICULAR to the light's view
// axis — which a ribbon's widening offset is, because it is perpendicular to
// the segment as the light sees it.
//
// ONE EXPRESSION FOR BOTH PROJECTION KINDS, and that is why it divides by w
// rather than branching. Row 0 of a view-projection, read as a row vector over
// world xyz, has length |P[0][0]| for a perspective projection and 1/halfExtent
// for an orthographic one; clip.w is 1 in the orthographic case, so the divide
// is the identity there and the perspective foreshortening everywhere else.
//
// THE MAGNITUDE, never the signed element. Vulkan's clip space has +Y
// downwards, so the engine uploads a projection whose [1][1] is negative; a
// signed read would hand back a negative scale, the half width would clamp to
// the floor in one axis and the widening would be silently wrong on exactly one
// backend. length() takes the magnitude by construction — the same trap
// groom-strand-visibility.md gives its own heading to.
//
// The two axes are AVERAGED rather than one being picked: a CSM cascade and an
// atlas tile are both square and symmetric, so the two are equal there and the
// average is exact; averaging keeps it sane rather than arbitrary if a
// non-square target ever appears.
float oloGroomShadowNdcPerWorld(mat4 viewProjection, float clipW)
{
	float safeW = max(abs(clipW), 1.0e-6);
	float sx = length(vec3(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0]));
	float sy = length(vec3(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1]));
	return 0.5 * (sx + sy) / safeW;
}

// The half width, in NDC, a strand of `radiusWorld` metres is rasterised at
// against a `resolutionTexels`-wide target, floored at `minWidthTexels` texels
// of FULL width.
//
// The floor is on the FULL width, not the half width: NDC spans [-1, 1] over
// `resolutionTexels` texels, so one texel is 2/resolution of NDC and a band of
// half width 1/resolution is one texel across. That is the smallest band that
// reliably contains a texel centre, which is the whole point.
float oloGroomShadowHalfWidthNdc(float radiusWorld, float ndcPerWorld, float resolutionTexels, float minWidthTexels)
{
	float trueHalfNdc = radiusWorld * ndcPerWorld;
	float floorHalfNdc = max(minWidthTexels, 0.0) / max(resolutionTexels, 1.0);
	return max(trueHalfNdc, floorHalfNdc);
}

// The full ribbon vertex: model, project, widen sideways in the light's clip
// space, and hand back a clip position.
//
// A VERTEX AT OR BEHIND THE LIGHT'S NEAR PLANE COLLAPSES THE QUAD rather than
// projecting through a near-zero w, which would throw a strand across the whole
// shadow map and stamp a band of occluder depth over geometry nowhere near it.
// The main pass drops the same segments for the same reason.
vec4 oloGroomShadowRibbonPosition(mat4 viewProjection, mat4 model, vec3 positionObject, vec3 otherObject,
                                  float radiusObject, float side, float widthScale, float objectScale,
                                  float resolutionTexels, float minWidthTexels)
{
	vec4 clipCurr = viewProjection * (model * vec4(positionObject, 1.0));
	vec4 clipOther = viewProjection * (model * vec4(otherObject, 1.0));

	if (clipCurr.w <= 1.0e-6 || clipOther.w <= 1.0e-6)
	{
		return vec4(0.0, 0.0, 2.0, 1.0); // beyond the far plane: clipped away
	}

	vec2 ndcCurr = clipCurr.xy / clipCurr.w;
	vec2 ndcOther = clipOther.xy / clipOther.w;

	vec2 delta = ndcOther - ndcCurr;
	float deltaLength = length(delta);
	// A segment whose two ends land on one texel still has a direction to widen
	// along; +X is as good as any and keeps the quad non-degenerate.
	vec2 tangent = deltaLength > 1.0e-7 ? delta / deltaLength : vec2(1.0, 0.0);
	vec2 normal = vec2(-tangent.y, tangent.x);

	float radiusWorld = radiusObject * widthScale * objectScale;
	float halfWidthNdc = oloGroomShadowHalfWidthNdc(radiusWorld, oloGroomShadowNdcPerWorld(viewProjection, clipCurr.w),
	                                                resolutionTexels, minWidthTexels);

	vec4 position = clipCurr;
	position.xy += normal * (halfWidthNdc * side) * clipCurr.w;
	return position;
}

#endif // OLO_GROOM_SHADOW_WIDENING_GLSL
