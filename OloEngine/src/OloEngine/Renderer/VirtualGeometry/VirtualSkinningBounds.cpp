#include "OloEnginePCH.h"

#include "OloEngine/Renderer/VirtualGeometry/VirtualSkinningBounds.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // sqrt(||A||_1 * ||A||_inf) — see SkinBoneScale in the header for why
        // this and not the longest column. glm is column-major, so `m[i]` is
        // column i and `m[i][r]` is row r of it.
        //
        // GLSL twin: oloSkinMatrixNormBound in include/VirtualSkinning.glsl.
        [[nodiscard]] f32 MatrixNormBound(const glm::mat4& m, f32 diagonalOffset)
        {
            f32 columnSum = 0.0f;
            f32 rowSum = 0.0f;
            for (u32 i = 0; i < 3; ++i)
            {
                f32 column = 0.0f;
                f32 row = 0.0f;
                for (u32 r = 0; r < 3; ++r)
                {
                    column += std::abs(m[i][r] - (i == r ? diagonalOffset : 0.0f));
                    row += std::abs(m[r][i] - (i == r ? diagonalOffset : 0.0f));
                }
                columnSum = std::max(columnSum, column);
                rowSum = std::max(rowSum, row);
            }
            return std::sqrt(columnSum * rowSum);
        }
    } // namespace

    f32 SkinBoneScale(const glm::mat4& boneMatrix)
    {
        f32 const scale = MatrixNormBound(boneMatrix, 0.0f);
        // A non-finite palette entry (an unposed skeleton, a NaN from a blend)
        // must not turn into a non-finite radius further down, where it would
        // make every comparison false and cull the whole instance. 1.0 is the
        // rigid answer, which is the honest fallback for "this bone says
        // nothing usable".
        return std::isfinite(scale) ? std::max(scale, 0.0f) : 1.0f;
    }

    f32 SkinBoneDeltaScale(const glm::mat4& boneMatrix)
    {
        f32 const scale = MatrixNormBound(boneMatrix, 1.0f);
        // Falls back to 1.0 for the same reason SkinBoneScale does, and 1.0 is
        // still the conservative direction here: it claims the bone can displace
        // a vector by its own length.
        return std::isfinite(scale) ? std::max(scale, 0.0f) : 1.0f;
    }

    f32 SkinMaxBoneScale(std::span<const glm::mat4> palette)
    {
        f32 maxScale = 1.0f;
        for (const glm::mat4& bone : palette)
        {
            maxScale = std::max(maxScale, SkinBoneScale(bone));
        }
        return maxScale;
    }

    f32 SkinDisplacementBound(std::span<const VirtualBoneBounds> boneBounds, std::span<const glm::mat4> palette)
    {
        f32 bound = 0.0f;
        sizet const count = std::min(boneBounds.size(), palette.size());
        for (sizet b = 0; b < count; ++b)
        {
            const VirtualBoneBounds& rest = boneBounds[b];
            if (!rest.Influences())
            {
                continue; // no vertex binds to this bone; it cannot move anything
            }

            const glm::mat4& bone = palette[b];
            glm::vec3 const posedCenter = glm::vec3(bone * glm::vec4(rest.Center, 1.0f));
            f32 const drift = glm::length(posedCenter - rest.Center);
            if (!std::isfinite(drift))
            {
                continue;
            }
            // ||A - I|| * R_b + |M_b c_b - c_b| — see the derivation in the
            // header. The delta norm rather than the norm itself is what makes
            // this ZERO in the rest pose and small for a small pose, instead of
            // always paying at least 2 * R_b.
            bound = std::max(bound, SkinBoneDeltaScale(bone) * rest.Radius + drift);
        }
        return std::isfinite(bound) ? bound : 0.0f;
    }

    bool SkinnedClusterSphereIsTight(std::span<const u32> boneRefs, std::span<const glm::mat4> palette)
    {
        u32 listed = 0;
        u32 reachable = 0;
        for (u32 const boneId : boneRefs)
        {
            if (boneId == kNoClusterBone)
            {
                continue;
            }
            ++listed;
            reachable += boneId < palette.size() ? 1u : 0u;
        }
        // EVERY listed bone must be in the palette, not merely one of them. A
        // vertex whose only influences are bones the palette does not have is
        // left in its REST position by the skin (there is nothing to pose it
        // with), and the hull of the bones that ARE present need not contain
        // that position — the same hole a rigid vertex opens, reached by a
        // palette that is shorter than the cook expected.
        return listed > 0 && listed == reachable;
    }

    glm::vec4 SkinnedClusterSphere(const glm::vec4& restSphere, std::span<const u32> boneRefs,
                                   std::span<const glm::mat4> palette)
    {
        glm::vec3 const restCenter(restSphere);
        f32 const restRadius = restSphere.w;

        if (!SkinnedClusterSphereIsTight(boneRefs, palette))
        {
            return restSphere; // caller falls back to the instance-wide bound
        }

        // Pass 1 — the centroid of the per-bone images of the rest centre. Any
        // point inside the hull would do; the centroid is what keeps the radius
        // in pass 2 small when the bones pull in opposite directions.
        glm::vec3 sum(0.0f);
        u32 used = 0;
        for (u32 const boneId : boneRefs)
        {
            if (boneId == kNoClusterBone)
            {
                continue;
            }
            sum += glm::vec3(palette[boneId] * glm::vec4(restCenter, 1.0f));
            ++used;
        }
        glm::vec3 const center = sum / static_cast<f32>(used);

        // Pass 2 — the radius that reaches every bone's image sphere. Each
        // M_b * v of this cluster lies within sphere(M_b * c, s_b * r), and
        // p'(v) is a convex combination of those points, so a sphere covering
        // all of them covers the deformed cluster.
        f32 radius = 0.0f;
        for (u32 const boneId : boneRefs)
        {
            if (boneId == kNoClusterBone)
            {
                continue;
            }
            const glm::mat4& bone = palette[boneId];
            glm::vec3 const posedCenter = glm::vec3(bone * glm::vec4(restCenter, 1.0f));
            radius = std::max(radius, glm::length(posedCenter - center) + SkinBoneScale(bone) * restRadius);
        }

        if (!std::isfinite(radius) || !std::isfinite(center.x) || !std::isfinite(center.y) ||
            !std::isfinite(center.z))
        {
            return restSphere;
        }
        return { center, radius };
    }

    glm::vec3 SkinPosition(const glm::vec3& position, const VirtualVertexSkinning& binding,
                           std::span<const glm::mat4> palette)
    {
        f32 const total = binding.Weights[0] + binding.Weights[1] + binding.Weights[2] + binding.Weights[3];
        if (!(total > 1e-3f))
        {
            return position; // rigid vertex inside a skinned mesh
        }

        glm::vec4 skinned(0.0f);
        f32 applied = 0.0f;
        for (u32 i = 0; i < 4; ++i)
        {
            u32 const boneId = binding.BoneIDs[i];
            f32 const weight = binding.Weights[i];
            // `!(weight > 0)`, matching SkinVirtualVertex's `weight <= 0.0`
            // exactly. A bare `== 0.0f` would APPLY a negative weight here and
            // drop it on the GPU, so this reference — which is the oracle the
            // containment tests generate their sample points with — would stop
            // describing what the shader does. (It is also a float equality,
            // which cpp-coding-quality.md bans for its own reasons.)
            if (!(weight > 0.0f) || boneId >= palette.size())
            {
                continue;
            }
            skinned += palette[boneId] * glm::vec4(position, 1.0f) * weight;
            applied += weight;
        }
        if (!(applied > 1e-3f))
        {
            return position;
        }
        // Renormalize by the weight that ACTUALLY landed rather than by `total`.
        // The two differ exactly when an influence names a bone outside the
        // palette, and dividing by `total` there would shrink the vertex toward
        // the origin instead of leaving it where its remaining bones put it.
        return glm::vec3(skinned) / applied;
    }
} // namespace OloEngine
