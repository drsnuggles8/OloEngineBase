#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PlanarReflection.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <cmath>

namespace OloEngine::PlanarReflection
{
    glm::vec4 NormalizePlane(const glm::vec4& plane)
    {
        // Reject non-finite input (NaN/Inf) before it can propagate through the
        // reflection / oblique-projection matrices into the camera UBO. Fall
        // back to a valid horizontal plane so a degenerate transform upstream
        // can never publish a non-finite matrix.
        if (!std::isfinite(plane.x) || !std::isfinite(plane.y) ||
            !std::isfinite(plane.z) || !std::isfinite(plane.w))
            return glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);

        const f32 lengthSq = plane.x * plane.x + plane.y * plane.y + plane.z * plane.z;
        // Degenerate normal (zero / near-zero length) — there is nothing sensible
        // to normalize against, and handing it back unchanged still lets a
        // zero-normal plane drive MakeObliqueProjection's dot-product divisor to
        // zero (→ NaN/Inf in the projection matrix and camera UBO). Fall back to
        // the same safe horizontal plane the non-finite branch uses.
        if (lengthSq <= 1e-12f)
            return glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        const f32 invLength = 1.0f / glm::sqrt(lengthSq);
        return plane * invLength;
    }

    glm::mat4 MakeReflectionMatrix(const glm::vec4& plane)
    {
        // Householder reflection across plane (a,b,c,d), a²+b²+c² = 1:
        //   R = I - 2 * [n; d] ⊗ [n; 0]   (homogeneous-point form)
        // Column-major (glm) result; reflects a column point p as R * p.
        const f32 a = plane.x;
        const f32 b = plane.y;
        const f32 c = plane.z;
        const f32 d = plane.w;

        glm::mat4 r(1.0f);
        // Column 0
        r[0][0] = 1.0f - 2.0f * a * a;
        r[0][1] = -2.0f * b * a;
        r[0][2] = -2.0f * c * a;
        // Column 1
        r[1][0] = -2.0f * a * b;
        r[1][1] = 1.0f - 2.0f * b * b;
        r[1][2] = -2.0f * c * b;
        // Column 2
        r[2][0] = -2.0f * a * c;
        r[2][1] = -2.0f * b * c;
        r[2][2] = 1.0f - 2.0f * c * c;
        // Column 3 (translation from the plane offset d)
        r[3][0] = -2.0f * a * d;
        r[3][1] = -2.0f * b * d;
        r[3][2] = -2.0f * c * d;
        r[3][3] = 1.0f;
        return r;
    }

    glm::vec3 ReflectPoint(const glm::vec4& plane, const glm::vec3& point)
    {
        const glm::vec4 p = NormalizePlane(plane);
        const f32 signedDistance = p.x * point.x + p.y * point.y + p.z * point.z + p.w;
        return point - 2.0f * signedDistance * glm::vec3(p.x, p.y, p.z);
    }

    glm::mat4 MakeMirrorView(const glm::mat4& view, const glm::vec4& worldPlane)
    {
        return view * MakeReflectionMatrix(NormalizePlane(worldPlane));
    }

    glm::vec4 TransformPlane(const glm::mat4& transform, const glm::vec4& worldPlane)
    {
        // Planes are covectors: a plane that holds for points p (dot(plane,p)=0)
        // must transform by the inverse-transpose so it still holds for the
        // transformed points (transform * p).
        return glm::transpose(glm::inverse(transform)) * worldPlane;
    }

    glm::mat4 MakeObliqueProjection(const glm::mat4& projection, const glm::vec4& viewSpacePlane)
    {
        // Lengyel's oblique near-plane clip. `viewSpacePlane` is the clip plane
        // in camera space with its normal pointing toward the kept half-space.
        // We replace the projection's near plane with this plane by rewriting
        // its third (clip-z) row. glm is column-major, so the clip-z row spans
        // projection[col][2] for col = 0..3.
        glm::mat4 result = projection;

        // Corner of the view frustum opposite the clip plane, in clip space,
        // transformed back into view space by the inverse projection. Encoded
        // directly from the projection's entries (it's diagonal for a standard
        // perspective, so only the four terms below are needed).
        glm::vec4 q;
        q.x = (glm::sign(viewSpacePlane.x) + projection[2][0]) / projection[0][0];
        q.y = (glm::sign(viewSpacePlane.y) + projection[2][1]) / projection[1][1];
        q.z = -1.0f;
        q.w = (1.0f + projection[2][2]) / projection[3][2];

        // Scale the plane so the back of the frustum stays put (dot(C, Q)).
        const glm::vec4 c = viewSpacePlane * (2.0f / glm::dot(viewSpacePlane, q));

        // Overwrite the clip-z row with the scaled plane (+1 on the z term keeps
        // the w-row, i.e. row 3, untouched — only near-plane mapping changes).
        result[0][2] = c.x;
        result[1][2] = c.y;
        result[2][2] = c.z + 1.0f;
        result[3][2] = c.w;
        return result;
    }

    ReflectionMatrices BuildReflectionMatrices(const glm::mat4& view,
                                               const glm::mat4& projection,
                                               const glm::vec3& cameraPosition,
                                               const glm::vec4& worldPlane)
    {
        const glm::vec4 plane = NormalizePlane(worldPlane);

        ReflectionMatrices out;
        out.MirrorView = view * MakeReflectionMatrix(plane);
        out.MirrorCameraPosition = ReflectPoint(plane, cameraPosition);

        // Express the reflection plane in the mirrored camera's view space, then
        // bias the normal slightly toward the kept side to avoid a hairline gap
        // of un-clipped geometry exactly at the waterline from float precision.
        const glm::vec4 viewPlane = TransformPlane(out.MirrorView, plane);
        out.ObliqueProjection = MakeObliqueProjection(projection, viewPlane);
        out.ViewProjection = out.ObliqueProjection * out.MirrorView;
        return out;
    }
} // namespace OloEngine::PlanarReflection
