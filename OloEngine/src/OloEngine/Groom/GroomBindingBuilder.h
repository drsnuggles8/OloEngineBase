#pragma once

// =============================================================================
// GroomBindingBuilder.h — building a binding, and the determinism contract.
// Issue #1249.
//
// "Deterministic rebind behaviour" is an acceptance criterion, so it is treated
// here as a CONTRACT the same way GroomCooker treats deterministic cooking:
// binding the same groom to the same surface twice, in either order, in either
// process, on either platform, produces byte-identical records.
//
// What that forbids, and what this file does about each:
//
//   * NEAREST-TRIANGLE TIES BROKEN BY TRAVERSAL ORDER. Two triangles at exactly
//     the same distance are a real case on a closed mesh (a root on a shared
//     edge hits both faces). The tie-break is the LOWER TRIANGLE INDEX, applied
//     with a strict `<` on the distance so a later candidate never displaces an
//     equal earlier one — not "whichever the grid visited first", which depends
//     on the cell size.
//   * FLOATING-POINT ACCUMULATION ORDER. Every root is solved independently
//     against one triangle at a time; nothing is summed across triangles, so
//     there is no order-dependent reduction to get wrong. That is also what
//     makes the loop trivially parallelisable later without changing a byte.
//   * UNORDERED ITERATION. The grid is a vector of vectors indexed by cell, and
//     every cell's triangle list is built by a single ordered pass, so it is
//     sorted by triangle index by construction.
//
// WHAT A BUILD IS NOT. It is not a re-cook of the groom and it does not touch
// the target. It reads two things and writes a third, which is why an editor
// can offer it as a button and a cooker can offer it as a step, and why the
// runtime never does it (see GroomBinding.h on why an implicit rebind is the
// failure this feature exists to prevent).
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"

#include <glm/glm.hpp>

#include <string>

namespace OloEngine
{
    class GroomAsset;

    struct GroomBindingBuildSettings
    {
        /// How far from the surface a root may be and still count as Exact or
        /// Clamped, in object-space units. Past it the root is still bound to
        /// the closest triangle found — dropping it would leave a bald patch —
        /// but is counted as Distant so the editor can say so.
        ///
        /// 0.25 engine units (25 cm) is deliberately generous: a groom authored
        /// with a shell offset, or one exported at a slightly different scale,
        /// is a normal thing to bind and an over-tight radius would flag a
        /// perfectly good coat. It is the WRONG-BODY case this is sized to
        /// catch, which misses by metres.
        f32 SearchRadius = 0.25f;

        /// Cells along the longest axis of the target's bounding box. The grid
        /// is a build-time accelerator only: it changes how long a bind takes
        /// and never which triangle wins, because the tie-break is on the
        /// triangle index rather than on visit order.
        u32 GridResolution = 32;

        /// Maps the TARGET's object space into the GROOM's.
        ///
        /// A groom and the body it grows on are two entities with two
        /// transforms, and assuming they share one is wrong in the very first
        /// scene that has both: Scenes/GroomStrandCoat.olo authors its body
        /// sphere at scale 0.088 and its coat at scale 1, because the groom
        /// asset is already at world size. Bound in the body's object space,
        /// every root of that coat would sit at a tenth of the sphere's radius
        /// and bind Distant to whatever triangle faced the origin.
        ///
        /// The GROOM's space is the common one, not the body's, for one
        /// concrete reason: a strand's own points live there, so the local
        /// offset `conjugate(RestRotation) * (P - RestOrigin)` needs no
        /// conversion at all — and doing it in the body's space instead would
        /// mean storing the bind-time relative transform in the file and
        /// applying it to every point, every frame, forever.
        ///
        /// `inverse(groomWorld) * targetWorld`. Identity when the two entities
        /// share a transform, which is the common authoring case and the reason
        /// this was not noticed sooner.
        glm::mat4 SurfaceToGroom{ 1.0f };

        [[nodiscard]] bool operator==(const GroomBindingBuildSettings&) const = default;
    };

    /// What a build actually found. Returned rather than logged, so the editor
    /// can show "40 000 roots bound, 12 further than 25 cm from the body"
    /// instead of a coat whose wrongness has to be inferred from its look.
    struct GroomBindingBuildStats
    {
        u32 RootsBound = 0;
        u32 RootsExact = 0;
        u32 RootsClamped = 0;
        u32 RootsDistant = 0;

        /// Roots whose closest triangle was degenerate (zero area) and so has no
        /// frame. They are bound — the record keeps its triangle and barycentric
        /// — with an identity rest rotation, and they are counted here because a
        /// non-zero value means the TARGET MESH has degenerate triangles, which
        /// is a fact about the body rather than about the groom.
        u32 RootsOnDegenerateTriangles = 0;

        /// Triangles the index buffer addressed past the end of the vertex
        /// array. Non-zero means a corrupt target, and the build refuses.
        u32 TrianglesOutOfRange = 0;

        f32 MaxRestDistance = 0.0f;
        f32 MeanRestDistance = 0.0f;

        [[nodiscard]] bool operator==(const GroomBindingBuildStats&) const = default;
    };

    class GroomBindingBuilder
    {
      public:
        /**
         * @brief Bind every curve of `groom` to the closest triangle of `target`.
         *
         * `target` must carry the surface at its BIND POSE — the positions the
         * binding's rest frames are recorded from. Handing it a deformed surface
         * is not detectable here and produces a binding that is correct for that
         * pose and wrong for the rest pose, which is why the editor's bind action
         * refuses to run while the target is playing (SceneHierarchyPanel) rather
         * than leaving it to the caller to remember.
         *
         * `targetSourcePath` is recorded in the binding so a file on disk can
         * name the body it belongs to. Project-relative or a bare file name,
         * never absolute: an absolute path would make two machines binding the
         * same pair produce different bytes, and determinism is the contract.
         *
         * Returns false with a named reason and leaves `outBinding` untouched on
         * any failure — the all-or-nothing rule GroomSerializer::DecodeFromBytes
         * states.
         */
        [[nodiscard]] static bool Build(const GroomAsset& groom, const GroomSurfaceView& target,
                                        const std::string& targetSourcePath,
                                        const GroomBindingBuildSettings& settings,
                                        Ref<GroomBindingAsset>& outBinding, GroomBindingBuildStats& outStats,
                                        std::string& outReason);

        /// The identity of a groom, for the binding's source signature and for
        /// the runtime's compatibility check. Pure; hashes ROOTS only — see
        /// GroomBindingSourceSignature on why not every point.
        [[nodiscard]] static GroomBindingSourceSignature SignGroom(const GroomAsset& groom) noexcept;

        /// The identity of a surface. `RestPositionHash` is taken from the
        /// positions in the view, so it is only the BIND-POSE hash when the view
        /// holds bind-pose positions — which is exactly why it is not part of
        /// the per-attach check (GroomBindingTargetSignature).
        [[nodiscard]] static GroomBindingTargetSignature SignTarget(const GroomSurfaceView& target) noexcept;

        /// 64-bit FNV-1a over raw bytes, the one hash this subsystem uses.
        /// Named here rather than inlined so the builder, the serializer and the
        /// tests cannot disagree about it.
        [[nodiscard]] static u64 HashBytes(const void* data, sizet size) noexcept;

        // --- The pieces, exposed because they are the testable halves --------

        /// Closest point on one triangle, as barycentric weights.
        ///
        /// `outInterior` is true when the projection landed strictly inside the
        /// triangle, which is what separates GroomRootBindQuality::Exact from
        /// Clamped. Returns the SQUARED distance, because the search compares
        /// distances and never needs the root of one.
        [[nodiscard]] static f32 ClosestPointOnTriangle(const glm::vec3& point, const glm::vec3& v0,
                                                        const glm::vec3& v1, const glm::vec3& v2,
                                                        glm::vec3& outBarycentric, bool& outInterior) noexcept;
    };
} // namespace OloEngine
