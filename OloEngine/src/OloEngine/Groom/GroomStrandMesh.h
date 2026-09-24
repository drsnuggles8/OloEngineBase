#pragma once

#include "OloEngine/Containers/Array.h"

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
// THE BUDGET IS NOT THE LOD, AND STILL IS NOT SINCE #1252. The strand budget
// below exists so a million-strand groom cannot size a buffer by accident; it
// is a flat stride over the whole groom, reported in the stats, and never a
// silent truncation — a truncated groom would show as a bald patch on one side,
// which reads as an import failure rather than as a budget.
//
// What #1252 added is a layer ABOVE it, in Groom/GroomLod.h: which
// representation this groom is drawn as at this distance, and what each of its
// three budgets is set to. That layer decides the numbers; this file spends
// them. The one thing it added HERE is GroomBuildSource — the build now reads a
// CURVE SET rather than specifically the base groom, so a cooked card level
// goes through this same code, this same shader and this same lighting instead
// of a second implementation that could disagree with the first.
// =============================================================================

#include "OloEngine/Core/Base.h"
// The COMPLETE GroomAsset, not a forward declaration: GroomBuildSource below
// holds a GroomCurveView and a GroomLodLevel is one of the two things it is
// built from, and both live in that header (issue #1252).
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomGuideInfluence.h"
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

        /// This strand's coat TINT (issue #1251), 8:8:8 in the low 24 bits of
        /// this lane with the exponent forced — see PackGroomCoatTint, which is
        /// the twin of GroomStrand.glsl's unpack, and which explains why the
        /// exponent is not optional.
        ///
        /// It took the lane that was Pad0. The vertex is SIXTEEN FLOATS and the
        /// Vulkan arm pulls it as a flat float array at that stride, so spending
        /// the spare lane rather than widening the struct is what keeps this a
        /// change to a value and not to a layout.
        f32 Tint = GroomCoatIdentityTint;

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

    // ── The GPU-deformed encoding of the same sixteen floats (#1427) ──────────
    //
    // A bound coat's stream is no longer rebuilt per frame. BuildGroomStrandRestMesh
    // writes it ONCE, in each root's bind frame, and the vertex shader moves it
    // with a small per-frame buffer (Groom/GroomGpuDeformation.h). The layout is
    // the same struct so the GL attribute block, the Vulkan pull stride and every
    // byte-size contract stay exactly as they are; four lanes change meaning, and
    // GroomStrandParams' deformation mode says which meaning is in force:
    //
    //   Position      this corner's endpoint, BIND-LOCAL:
    //                 conjugate(RestRotation) * (rest - RestOrigin)
    //   Other         the segment's OTHER endpoint, bind-local — not "this point
    //                 plus the delta", which the shader re-derives once both
    //                 ends are deformed
    //   PrevPosition  x = root slot (an integer held as a float), y = the other
    //                 endpoint's parameter along the strand, z = 1 at P1, 0 at P0
    //
    // Side, Radius, Coords, SegmentId and Tint mean what they always meant.
    // An UNBOUND groom never uses this encoding: its stream is the one
    // BuildGroomStrandMesh has always produced, so a static coat is unchanged by
    // construction rather than by measurement.

    /// One segment of a GPU-deformed stream, kept on the CPU so the coat
    /// self-shadow bake (#1426) can evaluate the drawn pose without the stream
    /// having to come back from the GPU. The two endpoints only — the pose the
    /// bake reads is a centreline, and the four ribbon corners would be four
    /// times the memory for three copies of the same two points.
    struct GroomRestPoseSegment
    {
        u32 RootSlot = 0;
        glm::vec3 Local0{ 0.0f };
        glm::vec3 Local1{ 0.0f };
        f32 T0 = 0.0f;
        f32 T1 = 0.0f;
        f32 Radius0 = 0.0f;
        f32 Radius1 = 0.0f;
    };

    static_assert(sizeof(GroomStrandVertex) == 64,
                  "GroomStrandVertex must be exactly sixteen floats: GroomStrand.glsl's Vulkan vertex pull "
                  "indexes it as a flat float array with a stride of 16");

    struct GroomStrandBuildSettings
    {
        /// A digest of the coat authoring this build was made with (#1251).
        ///
        /// The coat itself travels as a GroomCoatContext POINTER beside these
        /// settings, because it holds two Ref<GroomRegionMap> and a span and is
        /// therefore neither hashable nor comparable. This lane is what puts it
        /// in the cache key and in the collision guard all the same — see
        /// GroomCoatDigest. Zero means "no coat authoring", which is the value
        /// every call site that predates #1251 leaves it at.
        u64 CoatDigest = 0;

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

        // ── Coat authoring (#1251) ────────────────────────────────────

        /// Strands the COAT removed: a hidden role, or a lost density draw.
        /// Counted apart from the budget because the two have different fixes —
        /// one is an authoring choice and the other is a budget — and a coat
        /// that came out sparse for the wrong reason is otherwise indisting-
        /// uishable from one that came out sparse for the right one.
        u32 StrandsDroppedByCoat = 0;

        /// Per GroomCoatRole: how many strands were available after the coat's
        /// density decision, and how many the BUDGET then kept. The ratio of the
        /// two per role IS criterion 1's silhouette claim as a number, which is
        /// what GroomCoatAuthoringTest asserts on and what the editor shows.
        u32 AvailableByRole[GroomCoatRoleCount]{};
        u32 SelectedByRole[GroomCoatRoleCount]{};

        /// The stride the budget chose for each role. All equal on a groom with
        /// no coat authoring, which is the pre-#1251 behaviour.
        u32 StrideByRole[GroomCoatRoleCount]{};

        // ── Guide simulation (#1250) ────────────────────────────
        //
        // A strand is SIMULATED when its guides moved and it had weight to
        // apply; it is UNGUIDED when its group was groomed without a guide, or
        // when every guide it names fell outside this frame's budget. The two
        // are counted apart because a coat that does not move has a different
        // fix in each case — authoring a guide, or raising the budget — and
        // one combined counter would not say which.
        u32 StrandsSimulated = 0;
        u32 StrandsUnguided = 0;

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

    /**
     * @brief The curve set a build reads, and how it maps back to the groom.
     *
     * Issue #1252. Until then there was only one curve set to build from, so
     * the build took a `const GroomAsset&`. A cooked LOD level is a SECOND
     * curve set in the same layout, and the value of the card tier is that it
     * goes through the SAME build, the same shader and the same lighting — so
     * the parameter became the thing both of them are.
     *
     * `SourceCurves` is EMPTY for the base groom, which makes the base path
     * byte-for-byte the one that existed before this issue rather than a
     * special case of a new one — the same shape `deformation`, `coat` and
     * `simulation` already use.
     */
    struct GroomBuildSource
    {
        GroomCurveView Curves{};

        /// Curve in `Curves` -> curve in the BASE groom. Empty means identity.
        /// Everything indexed by the BINDING (#1249) or by the guide influence
        /// table (#1250) goes through it; everything indexed by this curve
        /// set's own geometry does not.
        std::span<const u32> SourceCurves{};

        /// The base groom's curve count — what the deformation and the
        /// simulation are sized against. Carried rather than derived from
        /// `Curves`, because for a LOD level the two differ and using the
        /// level's count would reject every deformation at card range.
        u32 BaseCurveCount = 0;

        [[nodiscard]] u32 SourceCurve(u32 curve) const noexcept
        {
            return SourceCurves.empty() ? curve : SourceCurves[curve];
        }

        [[nodiscard]] static GroomBuildSource FromAsset(const GroomAsset& groom) noexcept;
        [[nodiscard]] static GroomBuildSource FromLevel(const GroomAsset& base, const GroomLodLevel& level) noexcept;
    };

    // Expands `source` into ribbon geometry. `outVertices` and `outIndices` are
    // cleared first. Pure: the same curves and settings always produce the same
    // bytes, which is what lets a cache key on the pair.
    //
    // The index buffer is 32-bit and its values are bounded by the vertex
    // count, which the segment budget bounds in turn — so a groom cannot
    // produce indices a u32 cannot address.
    //
    // `coat` is null for a groom with no coat authoring, so the un-authored path
    // is byte-for-byte the one that existed before #1251 rather than a special
    // case of a new one — the same shape `deformation` already uses.
    //
    // `simulation` is null for a groom whose guides are not being simulated,
    // for the same reason and with the same guarantee (#1250). It is applied
    // AFTER the deformation and never instead of it: the displacement it
    // carries is measured from the deformed rest shape, so a groom whose guides
    // happen not to have moved emits exactly the bytes it emitted without it.
    GroomStrandMeshStats BuildGroomStrandMesh(const GroomBuildSource& source,
                                              const GroomStrandBuildSettings& settings,
                                              std::vector<GroomStrandVertex>& outVertices,
                                              std::vector<u32>& outIndices,
                                              const GroomStrandDeformation* deformation = nullptr,
                                              const GroomCoatContext* coat = nullptr,
                                              const GroomStrandSimulation* simulation = nullptr);

    /**
     * @brief The same strands as BuildGroomStrandMesh, in the GPU-deformed
     *        encoding (#1427).
     *
     * Walks the groom exactly as BuildGroomStrandMesh does — the same selection,
     * the same coat shape, the same segment budget, one shared walk rather than
     * a copy of it — and writes each point in its root's BIND frame instead of
     * deforming it. The result depends on the asset, the settings, the coat and
     * the binding, and on nothing that moves, so it is built once and cached.
     *
     * `outRootCurves[slot]` is the BASE curve of the strand whose vertices carry
     * root slot `slot`; the per-frame buffer is laid out in that order.
     * `outPoseSegments`, when not null, receives the stream's centrelines for
     * the coat bake.
     *
     * Returns empty stats and no geometry when `binding` does not span the base
     * groom — the caller then keeps the CPU-deformed path, which refuses the
     * same binding and draws the coat at rest.
     */
    GroomStrandMeshStats BuildGroomStrandRestMesh(const GroomBuildSource& source,
                                                  const GroomStrandBuildSettings& settings,
                                                  const GroomBindingAsset& binding,
                                                  std::vector<GroomStrandVertex>& outVertices,
                                                  std::vector<u32>& outIndices, std::vector<u32>& outRootCurves,
                                                  const GroomCoatContext* coat = nullptr,
                                                  std::vector<GroomRestPoseSegment>* outPoseSegments = nullptr);

    /// The BASE groom. Every call site that predates #1252 takes this overload
    /// and gets the identity source map, so the LOD change is invisible to the
    /// debug preview, the binding authoring tools and every existing test.
    GroomStrandMeshStats BuildGroomStrandMesh(const GroomAsset& groom, const GroomStrandBuildSettings& settings,
                                              std::vector<GroomStrandVertex>& outVertices,
                                              std::vector<u32>& outIndices,
                                              const GroomStrandDeformation* deformation = nullptr,
                                              const GroomCoatContext* coat = nullptr,
                                              const GroomStrandSimulation* simulation = nullptr);

    // The curve indices this build will walk, in the order it walks them.
    //
    // Exposed because the binding deformation (#1249) is per-curve work that has
    // to happen BEFORE the build, and doing it for every curve of a 200k-strand
    // groom when the budget will draw 20k of them is the difference between a
    // frame cost and a frame. `outCurves` is cleared first.
    void SelectGroomStrandCurves(const GroomBuildSource& source, const GroomStrandBuildSettings& settings,
                                 TArray<u32>& outCurves, const GroomCoatContext* coat = nullptr);
    void SelectGroomStrandCurves(const GroomAsset& groom, const GroomStrandBuildSettings& settings,
                                 TArray<u32>& outCurves, const GroomCoatContext* coat = nullptr);

    // The stats a build WOULD produce, without building anything. Pure and
    // cheap, so the editor's inspector can show the budget's effect on every
    // frame without allocating a megabyte to find out — and so the pass can ask
    // what a budget would retain BEFORE it builds, which is where the LOD's
    // width compensation gets the ACHIEVED fraction it must be computed from
    // (GroomLodWidthCompensation, issue #1252).
    [[nodiscard]] GroomStrandMeshStats PlanGroomStrandMesh(const GroomBuildSource& source,
                                                           const GroomStrandBuildSettings& settings,
                                                           const GroomCoatContext* coat = nullptr);
    [[nodiscard]] GroomStrandMeshStats PlanGroomStrandMesh(const GroomAsset& groom,
                                                           const GroomStrandBuildSettings& settings,
                                                           const GroomCoatContext* coat = nullptr);
} // namespace OloEngine
