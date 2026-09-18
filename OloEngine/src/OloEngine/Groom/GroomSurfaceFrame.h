#pragma once

// =============================================================================
// GroomSurfaceFrame.h — how a groom sees a body surface. Issue #1249.
//
// Two things live here, and they live TOGETHER on purpose.
//
//   GroomSurfaceView — a triangle mesh as the binder and the runtime both see
//   it: positions at a stride, indices, and the identity of the skeleton behind
//   it. A view rather than a MeshSource because the two callers hold different
//   things (the binder holds bind-pose positions, the runtime holds the live,
//   morph-deformed vertex array) and because a test wants to hand it three
//   vec3s without constructing a renderer asset.
//
//   MakeGroomSurfaceFrame — the ONE construction of a triangle's orthonormal
//   frame in the engine. The bind-time frame stored in a GroomRootBinding and
//   the deformed frame computed every frame at runtime MUST be built by the
//   same code: a disagreement about handedness, or about which edge becomes the
//   tangent, is a constant rotation applied to every strand — a coat that sits
//   on the body, points the wrong way, and looks deliberate. That is why this is
//   a header with one inline function and not two similar loops in two files.
//
// THE FRAME. z is the triangle's geometric normal; x is the first edge
// orthogonalised against it; y closes a right-handed basis. Geometric, not
// interpolated from vertex normals — an interpolated normal is a SHADING
// quantity that a normal map, a smoothing group or a re-export can change
// without the surface moving, and a root's frame must depend on the surface
// alone.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

namespace OloEngine
{
    /**
     * @brief A triangle mesh addressed by position stride and index buffer.
     *
     * `PositionData` points at the first vertex's position; `PositionStride` is
     * the byte distance to the next one. The stride form is what lets the
     * runtime pass OloEngine::Vertex (stride 32, position at offset 0) and a
     * test pass a plain `glm::vec3[]` (stride 12) through one type.
     *
     * Nothing here owns anything. A view is constructed at the point of use and
     * never stored — the array behind it is the mesh's live vertex data, which
     * the morph pass rewrites in place.
     */
    struct GroomSurfaceView
    {
        const std::byte* PositionData = nullptr;
        u32 PositionStride = 0;
        u32 VertexCount = 0;

        const u32* Indices = nullptr;
        u32 IndexCount = 0;

        /// Bones in the skeleton behind this surface; 0 when it is not skinned.
        u32 BoneCount = 0;
        /// Identity of that skeleton, so a re-rig is a detectable change. See
        /// GroomBindingTargetSignature.
        u64 SkeletonNameHash = 0;

        [[nodiscard]] u32 TriangleCount() const noexcept
        {
            return IndexCount / 3u;
        }

        /// True when there is a surface to bind to at all. Deliberately not
        /// "non-null pointers": a mesh that is still loading has both, and zero
        /// of everything.
        [[nodiscard]] bool IsUsable() const noexcept
        {
            return PositionData != nullptr && PositionStride >= sizeof(glm::vec3) && VertexCount > 0u &&
                   Indices != nullptr && IndexCount >= 3u && (IndexCount % 3u) == 0u;
        }

        /// Callers must have checked `index < VertexCount`.
        [[nodiscard]] glm::vec3 Position(u32 index) const noexcept
        {
            glm::vec3 value{ 0.0f };
            const std::byte* source = PositionData + static_cast<sizet>(index) * PositionStride;
            std::memcpy(&value, source, sizeof(glm::vec3));
            return value;
        }

        /// The three corner indices of `triangle`. Callers must have checked
        /// `triangle < TriangleCount()`.
        [[nodiscard]] glm::uvec3 TriangleIndices(u32 triangle) const noexcept
        {
            const sizet base = static_cast<sizet>(triangle) * 3u;
            return { Indices[base], Indices[base + 1u], Indices[base + 2u] };
        }

        /// True when every corner of `triangle` addresses a vertex this view
        /// has. An index buffer that overruns its vertex array is a corrupt
        /// mesh, and the binder and the runtime both refuse such a triangle
        /// rather than reading past the end of it.
        [[nodiscard]] bool TriangleInRange(u32 triangle) const noexcept
        {
            if (triangle >= TriangleCount())
            {
                return false;
            }
            const glm::uvec3 corners = TriangleIndices(triangle);
            return corners.x < VertexCount && corners.y < VertexCount && corners.z < VertexCount;
        }
    };

    /**
     * @brief The identity of the rig behind a surface, from its bone names.
     *
     * The NAMES rather than the Skeleton's address, because the check has to
     * survive a reload: the same rig loaded twice is two Skeleton objects and
     * one rig, and keying on the pointer would refuse every binding after the
     * first asset reload. In order, with a separator, so a re-rig that merely
     * reorders bones is still a change — which it is, because the palette the
     * runtime indexes is ordered.
     *
     * Takes the NAMES and not a Skeleton so this header stays free of the
     * Animation subsystem; it lives here because the binder, the runtime and
     * the editor must all produce the same number, and three copies of an FNV
     * loop is three chances for one of them to drift by a separator byte — at
     * which point the editor says a binding attaches and the renderer refuses
     * it, with no other symptom.
     */
    [[nodiscard]] inline u64 HashGroomSkeletonNames(std::span<const std::string> boneNames) noexcept
    {
        u64 hash = 1469598103934665603ull;
        for (const auto& name : boneNames)
        {
            for (const char c : name)
            {
                hash ^= static_cast<u64>(static_cast<unsigned char>(c));
                hash *= 1099511628211ull;
            }
            hash ^= 0xFFull; // a separator, so {"ab","c"} and {"a","bc"} differ
            hash *= 1099511628211ull;
        }
        return hash;
    }

    /**
     * @brief The matrix that takes a body's object space into its groom's.
     *
     * A groom and the body it grows on are two entities with two transforms,
     * and assuming they share one is wrong in the very first scene that has
     * both: Scenes/GroomStrandCoat.olo authors its body sphere at scale 0.088
     * and its coat at scale 1, because the groom asset is already at world
     * size. Bound in the body's object space, every root of that coat would sit
     * at a tenth of the sphere's radius.
     *
     * A degenerate groom transform — a zero scale on some axis — has no
     * inverse, and glm::inverse of a singular matrix returns infinities rather
     * than failing. Identity is the honest fallback: the coat then binds as
     * though the two shared a space, which is wrong in a way somebody can see,
     * rather than NaN in a way that poisons the groom's bounds.
     */
    [[nodiscard]] inline glm::mat4 MakeGroomSurfaceToGroomMatrix(const glm::mat4& groomWorld,
                                                                 const glm::mat4& targetWorld) noexcept
    {
        const f32 determinant = glm::determinant(groomWorld);
        if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-12f)
        {
            return glm::mat4(1.0f);
        }
        return glm::inverse(groomWorld) * targetWorld;
    }

    /**
     * @brief A triangle's orthonormal frame at a point on it.
     *
     * `Valid` is false for a DEGENERATE triangle — zero area, or two coincident
     * corners — and that case is real: a cooked character mesh routinely carries
     * a handful of them, and a pose can collapse one that was fine at rest. An
     * invalid frame is never substituted with a plausible one; the caller counts
     * it and holds the strand still, which is visible as a patch that does not
     * move rather than as a patch that flies off.
     */
    struct GroomSurfaceFrame
    {
        glm::vec3 Origin{ 0.0f };
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        bool Valid = false;
    };

    /**
     * @brief Build the frame of a triangle at a barycentric point on it.
     *
     * The single definition of a groom's surface frame — see the file header for
     * why there is exactly one. Pure: the same three corners and the same
     * barycentric always give the same frame, on every machine, which is what
     * makes the cooked binding deterministic.
     *
     * @param v0,v1,v2   the triangle's corners, in index order.
     * @param barycentric weights over (v0, v1, v2); the origin is their
     *                    weighted sum, so the caller's clamping decides where on
     *                    the triangle the frame sits.
     */
    [[nodiscard]] inline GroomSurfaceFrame MakeGroomSurfaceFrame(const glm::vec3& v0, const glm::vec3& v1,
                                                                 const glm::vec3& v2,
                                                                 const glm::vec3& barycentric) noexcept
    {
        GroomSurfaceFrame frame;
        frame.Origin = v0 * barycentric.x + v1 * barycentric.y + v2 * barycentric.z;

        const glm::vec3 edge1 = v1 - v0;
        const glm::vec3 edge2 = v2 - v0;
        const glm::vec3 cross = glm::cross(edge1, edge2);

        // Length, not glm::normalize: normalize on a zero vector produces NaNs
        // silently, and a NaN frame propagates into every point of the strand it
        // carries and then into the groom's bounds. The magnitude is compared
        // against an absolute floor because the surface is in engine units
        // (metres) — a triangle with an area under 1e-12 m^2 is not a triangle a
        // hair grows out of.
        const f32 crossLength = glm::length(cross);
        if (!std::isfinite(crossLength) || crossLength < 1.0e-12f)
        {
            return frame; // Valid stays false
        }
        const glm::vec3 normal = cross / crossLength;

        // The tangent is edge1 with the normal component removed. edge1 cannot
        // be parallel to the normal (the normal is perpendicular to it by
        // construction), so the only way this degenerates is a zero-length
        // edge1 — which would have made the cross product zero too.
        const glm::vec3 tangentRaw = edge1 - normal * glm::dot(normal, edge1);
        const f32 tangentLength = glm::length(tangentRaw);
        if (!std::isfinite(tangentLength) || tangentLength < 1.0e-12f)
        {
            return frame;
        }
        const glm::vec3 tangent = tangentRaw / tangentLength;
        const glm::vec3 bitangent = glm::cross(normal, tangent);

        // Columns, because glm::mat3's constructor takes columns and
        // glm::quat_cast reads a rotation matrix whose columns are the basis
        // vectors. Getting this the other way round transposes every frame,
        // which is a rotation that still looks like a rotation.
        const glm::mat3 basis{ tangent, bitangent, normal };
        frame.Rotation = glm::normalize(glm::quat_cast(basis));

        if (!std::isfinite(frame.Rotation.x) || !std::isfinite(frame.Rotation.y) ||
            !std::isfinite(frame.Rotation.z) || !std::isfinite(frame.Rotation.w) ||
            !std::isfinite(frame.Origin.x) || !std::isfinite(frame.Origin.y) || !std::isfinite(frame.Origin.z))
        {
            frame.Rotation = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
            return frame;
        }

        frame.Valid = true;
        return frame;
    }
} // namespace OloEngine
