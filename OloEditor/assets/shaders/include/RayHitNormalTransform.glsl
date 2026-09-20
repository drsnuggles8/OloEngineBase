// RayHitNormalTransform.glsl — object-space normal to world space at a ray hit,
// and the handedness of the instance that was hit. Issue #1326.
//
// THE RULE. A normal is a covector: under an instance transform M it maps by
// the inverse transpose, not by M. Applying M directly is exact only for rigid
// and uniformly-scaled instances; under non-uniform scale it skews the normal.
// The issue's worked case: M = diag(2, 1, 1) and n = (1, 1, 0)/sqrt(2) give
// (2, 1, 0)/sqrt(5) through M and (1, 2, 0)/sqrt(5) through the inverse
// transpose. Normalising cannot repair that — the DIRECTION is wrong.
//
// NO INVERSE IS COMPUTED. GL_EXT_ray_query hands back
// rayQueryGetIntersectionWorldToObjectEXT alongside the object-to-world matrix,
// so M^-1 is already there; the transpose falls out of multiplying on the RIGHT
// (`n * A` in GLSL is the row-vector product, i.e. transpose(A) * n). Hence
// `objectNormal * mat3(worldToObject)` and not a per-hit matrix inversion.
// The older comments in this repo asserting that the correction "needs an
// inverse the ray query does not hand back" were wrong, and #1326 removed them.
//
// THIS MATCHES RASTER. The G-buffer path multiplies by
// `mat3(inst.NormalMatrix)` with NormalMatrix = transpose(inverse(model))
// (VirtualGBufferVertexStage.glsl, VirtualVisibilityResolve.glsl,
// DDGI_Capture.glsl). Same operation, same sign convention — including for a
// mirrored instance, where raster does NOT negate the result.
//
// MIRRORED (negative determinant) INSTANCES. The inverse transpose keeps a
// mirrored instance's shading normal pointing outward, but a GEOMETRIC normal
// taken from the world-space winding does not: cross(M a, M b) =
// det(M) * M^-T cross(a, b), so the winding cross flips with the basis. A
// consumer that snaps the shading normal to the geometric side, or that offsets
// a ray along the geometric normal, would then push it INTO the surface. Any
// site deriving a geometric normal from world-space winding multiplies it by
// oloRtInstanceWindingSign() to undo exactly that factor.
//
// NORMAL MAPS NEED NOTHING EXTRA, as long as the tangent frame is built from
// WORLD-space positions and UVs (OloRtApplyNormalMap does). A tangent is an
// ordinary direction vector: it maps by M, which world positions already carry.
// The frame is then Gram-Schmidt'd against the shading normal, so correcting
// the normal corrects the whole basis with it. A tangent frame derived in
// OBJECT space and pushed through the inverse transpose would be wrong, and no
// site here does that.
//
// SINGULAR TRANSFORMS. A degenerate instance basis has no inverse; the matrix
// the ray query returns for one is not required to be finite, and the product
// above then comes back zero, Inf or NaN. oloRtNormalizeHitNormal() reports
// that as a FAILURE rather than returning a plausible direction: each call site
// picks its own documented, finite substitute (the geometric normal where one
// exists) instead of a silent house fallback that would shade a broken instance
// as if it were fine.
#ifndef OLO_RAY_HIT_NORMAL_TRANSFORM_GLSL
#define OLO_RAY_HIT_NORMAL_TRANSFORM_GLSL

// The corrected operation, UNNORMALISED — the transform is linear, so a caller
// interpolating several vertex normals transforms them first and normalises the
// interpolated result once, which is what the hit shaders do.
//
// `worldToObject` is rayQueryGetIntersectionWorldToObjectEXT's mat4x3; only its
// linear 3x3 part participates, and mat3() takes exactly that.
vec3 oloRtObjectNormalToWorld(vec3 objectNormal, mat4x3 worldToObject)
{
    return objectNormal * mat3(worldToObject);
}

// +1.0 when the instance basis preserves handedness, -1.0 when it mirrors.
// `objectToWorld` is rayQueryGetIntersectionObjectToWorldEXT's mat4x3.
float oloRtInstanceWindingSign(mat4x3 objectToWorld)
{
    return (determinant(mat3(objectToWorld)) < 0.0) ? -1.0 : 1.0;
}

// Normalise a transformed hit normal, reporting whether the result is usable.
// False means the input was degenerate: a zero or unnormalised vertex normal,
// or a singular instance transform (which surfaces as an Inf or NaN length).
// `result` is left untouched on a failure, so the caller's own initialisation
// survives.
bool oloRtNormalizeHitNormal(vec3 worldNormal, out vec3 result)
{
    const float len = length(worldNormal);
    // A NaN length fails `len > k` on its own; Inf has to be named.
    if (!(len > 1e-12) || isinf(len))
        return false;
    result = worldNormal / len;
    return true;
}

#endif // OLO_RAY_HIT_NORMAL_TRANSFORM_GLSL
