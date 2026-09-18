#pragma once

// =============================================================================
// GroomStrandMesh.h — the cooked groom's curves as ribbon geometry. Issue #1246.
//
// One curve SEGMENT becomes one quad: four vertices, two triangles, expanded to
// a screen-facing ribbon in the vertex shader rather than here. The expansion is
// deferred to the GPU because the ribbon's width is a SCREEN-SPACE quantity —
// it has a one-pixel floor (see GroomStrandCommon.glsl) — so baking it on the
// CPU would fix a strand's apparent thickness at whatever the camera was when
// the buffer was built.
//
// WHY A CPU BUILD AT ALL, THEN. Because a cooked groom is a structure-of-arrays
// with an offset table and a GPU draw needs a flat vertex stream. Building it
// once per asset and caching it is the whole of the asset-to-GPU step, and
// keeping it here — with no GL, no Vulkan and no renderer headers — is what
// lets the layout, the budget and the segment identity be tested on a machine
// with no GPU.
//
// WHAT THIS IS NOT. It is not LOD. The strand budget below exists so a
// million-strand groom cannot size a buffer by accident; choosing WHICH strands
// to draw at which distance, and keeping that stable under motion, is #1252.
// The budget is therefore a flat stride over the whole groom, reported in the
// stats, and never a silent truncation — a truncated groom would show as a bald
// patch on one side, which reads as an import failure rather than as a budget.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomVisibility.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace OloEngine
{
    class GroomAsset;

    // One ribbon-corner vertex. 64 bytes, sixteen floats — the layout
    // GroomStrand.glsl declares as attributes on OpenGL and PULLS by index on
    // Vulkan (ADR 0011 §5 leaves the Vulkan backend with no vertex input
    // state). The float count is therefore load-bearing on the Vulkan arm: a
    // seventeenth float here reads every strand's data at the wrong offset, and
    // GroomStrandMeshTest pins the size for that reason.
    //
    // IT WAS TWELVE FLOATS UNTIL #1249, and the four that were added are
    // PrevPosition plus its padding. Why the previous position has to be a
    // per-vertex attribute rather than derived: before the binding existed, a
    // groom moved only as a rigid object, so the previous position WAS the
    // current one under the previous model matrix and the shader could derive
    // it. A groom bound to a body deforms per strand, so last frame's position
    // of THIS point is not recoverable from any matrix — the body's pose moved,
    // not the groom's transform. An unbound groom writes PrevPosition ==
    // Position and gets exactly its old velocity back, which is what makes this
    // a widening rather than a behaviour change.
    struct GroomStrandVertex
    {
        /// This corner's centreline point, object space.
        glm::vec3 Position{ 0.0f };

        /// Position plus the segment's delta (P1 - P0), object space.
        ///
        /// Stored as a POINT rather than a direction, and stored the same way
        /// for all four corners, so the vertex shader gets one consistent
        /// screen-space tangent per quad. Storing "the other end of my
        /// segment" instead was the obvious encoding and is wrong: the two
        /// corners at P1 would see the tangent reversed, so their widening
        /// would go the other way and every quad would be a bowtie.
        glm::vec3 Other{ 0.0f };

        /// -1 or +1: which edge of the ribbon this corner is.
        f32 Side = 0.0f;

        /// Object-space RADIUS at this corner. The cooked groom stores
        /// DIAMETERS (the Alembic/USD convention), so the halving happens here,
        /// exactly once, and never again downstream.
        f32 Radius = 0.0f;

        /// x = root-to-tip parameter in [0,1]; y = across-ribbon in [-1,+1].
        glm::vec2 Coords{ 0.0f };

        /// GroomSegmentIdentity(curve, segment), bit-cast to a float so it
        /// rides in the same float stream the Vulkan pull reads. The shader
        /// bit-casts it back; no arithmetic is ever done on it as a float.
        f32 SegmentId = 0.0f;

        f32 Pad0 = 0.0f;

        /// This corner's centreline point AS IT WAS LAST FRAME, object space.
        ///
        /// Equal to Position for an unbound groom, and for a bound one on the
        /// frame its history was rejected — so the velocity the shader derives
        /// is then exactly zero rather than approximately zero, because both
        /// ends go through identical arithmetic.
        ///
        /// The WIDENING offset is deliberately not in here and never will be: a
        /// strand's motion is its centreline's motion, and carrying a
        /// width-dependent offset into the velocity would make a resolution
        /// change read as movement. GroomStrand.glsl says the same thing at the
        /// point it uses this.
        glm::vec3 PrevPosition{ 0.0f };

        f32 Pad1 = 0.0f;
    };

    static_assert(sizeof(GroomStrandVertex) == 64,
                  "GroomStrandVertex must be exactly sixteen floats: GroomStrand.glsl's Vulkan vertex pull "
                  "indexes it as a flat float array with a stride of 16");

    struct GroomStrandBuildSettings
    {
        /// Upper bound on the strands the mesh contains. A stride over the
        /// whole groom, never the first N — see the header.
        u32 MaxStrands = 100000;

        /// Upper bound on the SEGMENTS, which is what actually sizes the
        /// buffer: 4 vertices and 6 indices each. 2M segments is 96 MB of
        /// vertex data, which is the point past which a groom should be
        /// answered with a budget rather than an allocation.
        u32 MaxSegments = 2000000;

        /// Draw only the guide curves. The authoring view, not a quality tier.
        bool GuidesOnly = false;

        [[nodiscard]] bool operator==(const GroomStrandBuildSettings&) const = default;
    };

    // What the build actually produced. Returned rather than logged so the
    // editor can say "48 000 of 1.2M strands" instead of showing a fraction of
    // an asset as if it were all of it — the same reason GroomPreviewStats
    // exists for the debug preview.
    struct GroomStrandMeshStats
    {
        u32 StrandsAvailable = 0;
        u32 StrandsSelected = 0;
        u32 Stride = 1;
        u32 SegmentCount = 0;
        u32 VertexCount = 0;
        u32 IndexCount = 0;
        u64 VertexBytes = 0;
        u64 IndexBytes = 0;

        /// True when MaxSegments, rather than MaxStrands, set the stride.
        /// Surfaced so raising the strand budget and seeing no change is
        /// explicable instead of looking broken.
        bool SegmentBudgetLimited = false;

        /// Curves skipped because they carry fewer than two points. A cooked
        /// groom cannot contain one (GroomLimits::MinPointsPerCurve), so a
        /// non-zero count here means the caller was handed something that did
        /// not come through GroomBuilder.
        u32 CurvesSkippedTooShort = 0;

        // ── Deformed bounds (#1249) ───────────────────────────────────
        //
        // The object-space box the EMITTED centrelines actually occupy. For an
        // undeformed groom this is the asset's own bounds narrowed to the
        // selected strands; for a bound one it is the box the coat occupies in
        // THIS pose, which is the only box a culler or a bounds readout may use
        // — the asset's bind-pose bounds do not contain a raised arm's fur.
        //
        // `BoundsValid` is false when nothing was emitted, which is what keeps
        // an empty build from publishing the sentinel box as if it were a
        // measurement.
        glm::vec3 BoundsMin{ 0.0f };
        glm::vec3 BoundsMax{ 0.0f };
        bool BoundsValid = false;

        /// Strands that were drawn at REST because their root had no valid
        /// deformed frame. Zero on an unbound groom, by construction.
        u32 StrandsHeldAtRest = 0;

        [[nodiscard]] bool operator==(const GroomStrandMeshStats&) const = default;
    };

    /**
     * @brief The per-frame deformation a bound groom is built with (#1249).
     *
     * Passed by pointer and null for an unbound groom, so the unbound path is
     * byte-for-byte the one that existed before this issue rather than a special
     * case of a new one.
     *
     * `RootTransforms` is indexed by CURVE and must span the whole groom — see
     * EvaluateGroomRootTransforms, which fills it that way precisely so an index
     * into it is always safe.
     */
    struct GroomStrandDeformation
    {
        const GroomBindingAsset* Binding = nullptr;
        std::span<const GroomRootTransform> RootTransforms{};

        [[nodiscard]] bool IsUsable(u32 curveCount) const noexcept
        {
            return Binding != nullptr && Binding->GetRootCount() == curveCount &&
                   RootTransforms.size() == curveCount;
        }
    };

    // Expands `groom` into ribbon geometry. `outVertices` and `outIndices` are
    // cleared first. Pure: the same groom and settings always produce the same
    // bytes, which is what lets a cache key on the pair.
    //
    // The index buffer is 32-bit and its values are bounded by the vertex
    // count, which the segment budget bounds in turn — so a groom cannot
    // produce indices a u32 cannot address.
    GroomStrandMeshStats BuildGroomStrandMesh(const GroomAsset& groom, const GroomStrandBuildSettings& settings,
                                              std::vector<GroomStrandVertex>& outVertices,
                                              std::vector<u32>& outIndices,
                                              const GroomStrandDeformation* deformation = nullptr);

    // The curve indices this build will walk, in the order it walks them.
    //
    // Exposed because the binding deformation (#1249) is per-curve work that has
    // to happen BEFORE the build, and doing it for every curve of a 200k-strand
    // groom when the budget will draw 20k of them is the difference between a
    // frame cost and a frame. `outCurves` is cleared first.
    void SelectGroomStrandCurves(const GroomAsset& groom, const GroomStrandBuildSettings& settings,
                                 std::vector<u32>& outCurves);

    // The stats a build WOULD produce, without building anything. Pure and
    // cheap, so the editor's inspector can show the budget's effect on every
    // frame without allocating a megabyte to find out.
    [[nodiscard]] GroomStrandMeshStats PlanGroomStrandMesh(const GroomAsset& groom,
                                                           const GroomStrandBuildSettings& settings);
} // namespace OloEngine
