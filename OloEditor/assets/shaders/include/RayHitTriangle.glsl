#ifndef OLO_RAY_HIT_TRIANGLE_GLSL
#define OLO_RAY_HIT_TRIANGLE_GLSL

#include "RayHitNormalTransform.glsl"

// The analytic tangent frame of the hit triangle from its UV gradients — the
// derivative-free twin of PBRCommon's applyNormalMapTBN, and the SAME formula
// the CPU reference uses (ReferenceScene::ApplyNormalMap), so a normal-mapped
// hit shades identically on both. Degenerate UVs (a constant texcoord over the
// triangle) leave the interpolated normal alone.
vec3 OloRtApplyNormalMap(vec3 n, vec3 p0, vec3 p1, vec3 p2, vec2 uv0, vec2 uv1, vec2 uv2, vec2 sampledXY,
                         float normalScale)
{
    const vec3 e1 = p1 - p0;
    const vec3 e2 = p2 - p0;
    const vec2 d1 = uv1 - uv0;
    const vec2 d2 = uv2 - uv0;
    const float det = d1.x * d2.y - d2.x * d1.y;
    if (abs(det) < 1e-12)
        return n;
    const float inv = 1.0 / det;
    vec3 t = (e1 * d2.y - e2 * d1.y) * inv;
    const vec3 bRaw = (e2 * d1.x - e1 * d2.x) * inv;
    t -= n * dot(n, t);
    const float tLen = length(t);
    if (!(tLen > 1e-12))
        return n;
    t /= tLen;
    const vec3 nCrossT = cross(n, t);
    const vec3 b = dot(nCrossT, bRaw) < 0.0 ? -nCrossT : nCrossT;
    const vec3 tn = decodeTangentNormal(sampledXY, normalScale);
    const vec3 result = t * tn.x + b * tn.y + n * tn.z;
    const float len = length(result);
    return (len > 1e-12) ? result / len : n;
}

// Read the hit triangle in the TLAS render-relative space. Positions use
// the ray query object-to-world transform and normals use the transpose
// of the world-to-object transform.
void OloRtFetchTriangle(GPUSceneGeometry geometry, uint primitiveIndex, mat4x3 objectToWorld,
                        mat4x3 worldToObject,
                        out vec3 p0, out vec3 p1, out vec3 p2, out vec3 n0, out vec3 n1, out vec3 n2,
                        out vec2 uv0, out vec2 uv1, out vec2 uv2)
{
    OloRtIndexStream indices = OloRtIndexStream(geometry.IndexAddress);
    OloRtVertexUVStream vertices = OloRtVertexUVStream(geometry.VertexAddress);

    const uint indexBase = geometry.FirstIndex + primitiveIndex * 3u;
    const uint i0 = uint(int(indices.Indices[indexBase + 0u]) + geometry.BaseVertex);
    const uint i1 = uint(int(indices.Indices[indexBase + 1u]) + geometry.BaseVertex);
    const uint i2 = uint(int(indices.Indices[indexBase + 2u]) + geometry.BaseVertex);

    const uint stride = OLO_RT_VERTEX_STRIDE / 4u;
    const uint normalOffset = OLO_RT_VERTEX_NORMAL_OFFSET / 4u;
    const uint uvOffset = OLO_RT_VERTEX_TEXCOORD_OFFSET / 4u;

    const vec3 lp0 = vec3(vertices.Floats[i0 * stride + 0u], vertices.Floats[i0 * stride + 1u], vertices.Floats[i0 * stride + 2u]);
    const vec3 lp1 = vec3(vertices.Floats[i1 * stride + 0u], vertices.Floats[i1 * stride + 1u], vertices.Floats[i1 * stride + 2u]);
    const vec3 lp2 = vec3(vertices.Floats[i2 * stride + 0u], vertices.Floats[i2 * stride + 1u], vertices.Floats[i2 * stride + 2u]);
    const vec3 ln0 = vec3(vertices.Floats[i0 * stride + normalOffset + 0u], vertices.Floats[i0 * stride + normalOffset + 1u], vertices.Floats[i0 * stride + normalOffset + 2u]);
    const vec3 ln1 = vec3(vertices.Floats[i1 * stride + normalOffset + 0u], vertices.Floats[i1 * stride + normalOffset + 1u], vertices.Floats[i1 * stride + normalOffset + 2u]);
    const vec3 ln2 = vec3(vertices.Floats[i2 * stride + normalOffset + 0u], vertices.Floats[i2 * stride + normalOffset + 1u], vertices.Floats[i2 * stride + normalOffset + 2u]);

    uv0 = vec2(vertices.Floats[i0 * stride + uvOffset + 0u], vertices.Floats[i0 * stride + uvOffset + 1u]);
    uv1 = vec2(vertices.Floats[i1 * stride + uvOffset + 0u], vertices.Floats[i1 * stride + uvOffset + 1u]);
    uv2 = vec2(vertices.Floats[i2 * stride + uvOffset + 0u], vertices.Floats[i2 * stride + uvOffset + 1u]);

    p0 = objectToWorld * vec4(lp0, 1.0);
    p1 = objectToWorld * vec4(lp1, 1.0);
    p2 = objectToWorld * vec4(lp2, 1.0);

    // Normals by the INVERSE TRANSPOSE — include/RayHitNormalTransform.glsl owns
    // the algebra and why no inverse is computed (#1326). Unnormalised on
    // purpose: the transform is linear, so the caller interpolates these three
    // and normalises once. Positions above use objectToWorld; normals use
    // worldToObject. They are different matrices for a reason, and this is the
    // only function that holds both.
    //
    // The CPU reference (ReferenceScene::AddInstance) still accepts only rigid +
    // uniform-scale instances, where the two forms agree, so this widens the GPU
    // path beyond the class the reference can be compared on rather than
    // diverging from it within that class.
    n0 = oloRtObjectNormalToWorld(ln0, worldToObject);
    n1 = oloRtObjectNormalToWorld(ln1, worldToObject);
    n2 = oloRtObjectNormalToWorld(ln2, worldToObject);
}

#endif
