#pragma once

// =============================================================================
// GroomRayTracingProxy.h — what a coat looks like TO A RAY, which tier it gets
// at this distance, and how far that tier is from the truth. Issue #1253.
//
// WHY A COAT NEEDS A SECOND REPRESENTATION AT ALL. The ribbons GroomRenderPass
// draws are SCREEN-FACING: GroomStrandMesh emits a centreline plus a tangent
// and GroomStrand.glsl widens each segment perpendicular to the segment and to
// the EYE vector, with a one-pixel floor. That geometry does not exist in the
// world — it is a different shape for every camera and it has no thickness at
// all along the view axis — so it cannot be the geometry a shadow ray or a
// reflection ray intersects. A ray needs world-space triangles, and the coat
// has none. This file is the smallest thing that produces some.
//
// THE SCOPE BOUNDARY, QUOTED BECAUSE IT DECIDES THE DESIGN. The issue asks for
// "a measured real-time representation, not mandatory per-strand hardware ray
// tracing". A reference coat here is ~20k strands of ~12 segments; expanded to
// crossed ribbons that is roughly a million triangles PER ANIMAL, rebuilt every
// frame for a bound groom. So the representation is a THINNED strand set whose
// radii are widened to put the missing coat back, and the two tiers differ only
// in how hard they thin.
//
// WHY THINNING-AND-WIDENING IS THE RIGHT KNOB, stated as the invariant it
// preserves. What a shadow ray measures is how much fibre lies between a point
// and a light; what a reflection ray measures is whether the coat's silhouette
// is where it should be. Both are the coat's PROJECTED AREA along a direction,
// and for a set of tapered cylinders that is
//
//     C(w) = sum over segments of 2 * rbar_i * L_i * sin(angle between t_i, w)
//
// which is LINEAR in the radius. Keeping one strand in k and multiplying every
// radius by k therefore preserves C in expectation, exactly — no square root,
// for the reason GroomLod.h gives for its own compensation being 1/k: a strand
// is a BAND, and a band's area is length times width. DirectionalCoverage below
// is that sum, CompareGroomProxyCoverage is the residual after the compensation
// is applied, and GroomRayTracingProxyTest sweeps it rather than sampling it.
//
// WHAT IT DOES NOT PRESERVE, said out loud because it is the honest limit: C is
// the area the coat would project IF NO FIBRE OCCLUDED ANOTHER. In a dense coat
// they do, so a thinned-and-widened coat is slightly MORE opaque than the coat
// it stands in for — the widened strands overlap less than the strands they
// replace. That error grows with the compensation, which is why it is capped
// (GroomProxyPolicy::MaxWidthCompensation) and why the achieved compensation is
// reported next to the cap instead of being clamped silently.
//
// CROSSED RIBBONS, NOT ONE. A single flat ribbon per segment is half the
// triangles and disappears edge-on — and a shadow ray's direction is the
// LIGHT's, which the geometry cannot be oriented towards without becoming
// per-light. Two ribbons in perpendicular planes through the centreline keep a
// non-degenerate silhouette from every direction. The pair costs 4 triangles
// per segment against 2, and that factor is the whole of the cost difference;
// the analysis document measures both arms.
//
// THE DOUBLE-COUNT BOUNDARY, WHICH IS THIS ISSUE'S CENTRAL INVARIANT.
// GroomCoatShadow.h already names three disjoint attenuations: inside one fibre
// (#1247), BETWEEN the fibres of one coat (#1248's tau), and everything else in
// the scene. A proxy in the TLAS is a member of the third. So:
//
//     A GROOM PROXY OCCLUDES OTHER RECEIVERS. IT NEVER OCCLUDES ITS OWN COAT.
//
// The coat is not a ray-traced-shadow receiver — GroomRenderPass does not
// participate in the G-Buffer, so no screen-space shadow term reaches a strand,
// and this issue deliberately does not make it one. That is what keeps tau the
// only thing attenuating a strand, and it is checkable as a picture: with groom
// proxies on and off, the coat's own pixels must be IDENTICAL while the body
// and the ground beneath it darken. A coat that dimmed would be the "shadow map
// is binary, a coat is not" failure (a groom that both casts and receives went
// from 44.98 to 0.22 luma) arriving through a new door.
//
// CPU-ONLY AND GPU-FREE, like GroomLod.h and GroomCoatShadowTechnique.h beside
// it: the tier ladder, the budget, the conversion and the error metric all
// compile and are tested on a machine with no GPU, which is every CI runner
// here. The residency, the buffers and the GPU Scene records are
// Renderer/RayTracing/GroomSurfaceCache.h's, on the far side of that line.
//
// The one renderer header this includes is Renderer/Vertex.h, for the 32-byte
// POD the acceleration structure is built over. It carries no GL, no Vulkan and
// no renderer state; emitting a private twin of it instead would be two structs
// that must agree about a stride, which is the drift GroomStrandVertex's own
// "sixteen floats" warning is about.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // The ladder
    // -------------------------------------------------------------------------

    // Which ray-space representation a coat gets. TWO tiers, not three, and
    // that is a decision rather than an omission: GroomLod's third tier (the
    // shell) is not a tier this engine draws, and a shell in ray space has the
    // same defect it has in raster — it claims a coverage of 1 everywhere
    // inside the silhouette, so a coat you can see through casts the shadow of
    // a solid lump. Measured, not assumed; see
    // docs/analysis/groom-rt-proxies-1253.md.
    //
    // Ordered FINEST FIRST so "coarser" is "greater", the same direction
    // GroomRepresentation is ordered in, so the hysteresis below can say
    // `requested > current` for a coarsening exactly the way AdvanceGroomLod
    // does.
    enum class GroomProxyTier : u8
    {
        /// The coat's own strands, thinned to the detailed budget and widened
        /// to put back what the thinning removed. The near-animal tier.
        Detailed = 0,

        /// The same construction at a much harder thinning. The far-animal
        /// tier, and the reason this issue exists: a distant animal's coat is a
        /// few pixels of silhouette and a shadow whose SHAPE matters more than
        /// its microstructure.
        Proxy = 1,

        Count
    };

    constexpr u32 GroomProxyTierCount = static_cast<u32>(GroomProxyTier::Count);

    [[nodiscard]] constexpr std::string_view ToString(GroomProxyTier tier) noexcept
    {
        switch (tier)
        {
            case GroomProxyTier::Detailed:
                return "Detailed";
            case GroomProxyTier::Proxy:
                return "Proxy";
            case GroomProxyTier::Count:
                break;
        }
        return "Detailed";
    }

    // -------------------------------------------------------------------------
    // Why a coat is not in the ray-traced scene
    // -------------------------------------------------------------------------
    //
    // ORDERED MOST-FUNDAMENTAL FIRST and the FIRST match is reported, the rule
    // GroomCoatShadowFallbackReason and GroomLodFallbackReason already follow
    // here: a coat that trips an earlier row would trip the later ones too, so
    // the first gives a reason a user can act on.
    //
    // These are REFUSALS, not fallbacks, and the difference matters for what
    // the picture looks like. A groom that falls back inside the ladder still
    // occludes, just more coarsely. A groom that is refused casts no ray-traced
    // shadow at all — the raster tier is untouched and still draws it, which is
    // criterion 4's quality tier surviving, but the ray-traced world no longer
    // contains that animal and a counter has to say so.
    enum class GroomProxyRefusalReason : u32
    {
        /// The groom is in the ray-traced scene.
        None = 0,

        /// Ray tracing is unavailable, the frame asked for no hybrid effect, or
        /// the active rendering path does not reach one. NOT a failure and
        /// deliberately first among the non-reasons — counting every groom in a
        /// scene with ray tracing switched off as a refusal saturates the
        /// counter that is supposed to explain a coat that is missing from a
        /// scene where it should be.
        NotRequested,

        /// The groom cooked to nothing, or the tier's budget selected no
        /// strands from it. There is no geometry to stand in for.
        GroomHasNoGeometry,

        /// The per-frame update budget is spent. Transient by construction:
        /// this groom refreshes on a later frame, and the structure it already
        /// has stays resident and keeps occluding in the meantime.
        BudgetExhausted,

        /// The resident set is full, or this groom's geometry would not fit
        /// under the resident byte cap. Distinct from BudgetExhausted because
        /// one clears on the next frame and the other does not clear until
        /// something leaves the scene.
        ResidencyExhausted,

        /// The conversion produced no triangles, or a GPU buffer could not be
        /// created. Last, because everything above it would also produce this
        /// symptom and naming it would send someone looking at an allocator.
        BuildFailed,

        Count
    };

    // A SENTENCE per reason, not a token — GroomCoatShadowFallbackReason's rule,
    // and a test asserts none of these is empty.
    [[nodiscard]] constexpr std::string_view Describe(GroomProxyRefusalReason reason) noexcept
    {
        switch (reason)
        {
            case GroomProxyRefusalReason::None:
                return "The coat is in the ray-traced scene at the tier its apparent size selects.";
            case GroomProxyRefusalReason::NotRequested:
                return "Ray tracing is unavailable or no hybrid effect asked for it this frame, so the coat is "
                       "drawn by the raster tier only.";
            case GroomProxyRefusalReason::GroomHasNoGeometry:
                return "The groom produced no strands at this tier's budget, so there is nothing to build a "
                       "ray-space representation from.";
            case GroomProxyRefusalReason::BudgetExhausted:
                return "The per-frame proxy update budget is spent; this coat keeps the structure it already has "
                       "and refreshes on a later frame.";
            case GroomProxyRefusalReason::ResidencyExhausted:
                return "Every resident proxy slot, or the resident byte budget, is already held by another coat.";
            case GroomProxyRefusalReason::BuildFailed:
                return "The proxy conversion emitted no triangles, or its GPU buffers could not be created.";
            case GroomProxyRefusalReason::Count:
                break;
        }
        return "Unknown";
    }

    [[nodiscard]] constexpr std::string_view ToString(GroomProxyRefusalReason reason) noexcept
    {
        switch (reason)
        {
            case GroomProxyRefusalReason::None:
                return "None";
            case GroomProxyRefusalReason::NotRequested:
                return "NotRequested";
            case GroomProxyRefusalReason::GroomHasNoGeometry:
                return "GroomHasNoGeometry";
            case GroomProxyRefusalReason::BudgetExhausted:
                return "BudgetExhausted";
            case GroomProxyRefusalReason::ResidencyExhausted:
                return "ResidencyExhausted";
            case GroomProxyRefusalReason::BuildFailed:
                return "BuildFailed";
            case GroomProxyRefusalReason::Count:
                break;
        }
        return "Unknown";
    }

    // -------------------------------------------------------------------------
    // The policy
    // -------------------------------------------------------------------------

    // Every number here is a CONSTANT rather than an authored field, and that
    // is deliberate: a coat's ray-space representation is not an artistic
    // choice, it is what the frame can afford to trace. GroomLodComponent still
    // owns what the coat LOOKS like; this owns what it costs a ray. The one
    // per-groom lever is the entity's own strand budget, which the tiers scale.
    //
    // The figures are sized from the reference fixtures measured in
    // docs/analysis/groom-rt-proxies-1253.md — not guessed, and the document
    // carries the measurement each one came from.
    struct GroomProxyPolicy
    {
        /// Apparent size, in pixels of the render target's HEIGHT, at and above
        /// which a coat gets the detailed tier. The same unit and the same
        /// orientation-independent projection every other LOD in this engine
        /// uses (EstimateProjectedPixelSize, issue #726) — deliberately not a
        /// distance in metres, which is wrong at the next field of view and
        /// wrong again at 4K.
        static constexpr f32 DetailedPixelSize = 192.0f;

        /// Fraction the threshold slides by to HOLD the tier a coat already
        /// has. ONE offset applied to both edges so the band moves bodily
        /// rather than changing width — GroomLodPolicy::Hysteresis' rule.
        static constexpr f32 Hysteresis = 0.15f;

        /// Consecutive frames a COARSENING request must persist before it is
        /// taken. Refining is immediate, for AdvanceGroomLod's reason: a coat
        /// that just got closer and is still on the proxy is visibly wrong in a
        /// reflection, while one that stays detailed a few frames too long is
        /// merely expensive, and only one of those is a picture anyone sees.
        static constexpr u32 HoldFrames = 4;

        /// Strands turned into ray-space geometry at each tier, before the
        /// entity's own budget is applied. Applied as a STRIDE over the cooked
        /// curve set, never a prefix — the cook sorts curves so each group is
        /// contiguous and "the first N strands" is one side of the animal
        /// (groom-strand-visibility.md rule 7), so a prefix would build a coat
        /// that is bald on one flank and cast exactly that shadow.
        static constexpr u32 DetailedStrandBudget = 6144u;
        static constexpr u32 ProxyStrandBudget = 768u;

        /// Cap on the radius compensation. Past this a widened strand is a
        /// tube far fatter than any fibre and the union-versus-sum error above
        /// stops being small; beyond the cap the coat genuinely thins out in
        /// ray space and the counters say so rather than clamping silently.
        static constexpr f32 MaxWidthCompensation = 48.0f;

        /// Resident proxies, and the geometry bytes they may hold between them.
        /// A refusal past either is fail-closed: the coat keeps its raster tier
        /// and leaves the ray-traced scene, counted.
        static constexpr u32 ResidentGrooms = 128u;
        static constexpr u64 GeometryBytes = 192u * 1024u * 1024u;

        /// Device acceleration-structure bytes the groom proxies may hold. The
        /// twin of VegetationPolicy::AccelerationStructureBytes and applied the
        /// same way, in the Vulkan backend, because that is the only place the
        /// real size of a built structure is known.
        static constexpr u64 AccelerationStructureBytes = 96u * 1024u * 1024u;

        /// Per-FRAME conversion work. These have to cover a FULL refresh of
        /// every resident coat, not a staggered fraction: a bound groom is
        /// rebuilt every frame by construction (GroomRenderPass::AcquireGeometry
        /// says why), so a cap below the full refresh does not slow the feature
        /// down, it switches it off for whichever animals sort last — which is
        /// a coat that vanishes from the shadows of half the frames.
        static constexpr u32 UpdatesPerFrame = 256u;
        static constexpr u32 VerticesPerFrame = 4u * 1024u * 1024u;
        static constexpr u32 TrianglesPerFrame = 2u * 1024u * 1024u;

        /// The strand budget this tier spends.
        [[nodiscard]] static constexpr u32 StrandBudget(GroomProxyTier tier) noexcept
        {
            return tier == GroomProxyTier::Proxy ? ProxyStrandBudget : DetailedStrandBudget;
        }
    };

    // The radius multiplier that puts back the coat a thinning removed.
    //
    // `achievedFraction` MUST BE THE FRACTION THE BUILD ACTUALLY RETAINED, and
    // that warning is attached here for the reason it is attached to
    // GroomLodWidthCompensation: the budget is spent as an integer stride, so a
    // request for 0.4 of the curves retains 1/3 of them, and compensating on the
    // request leaves the coat a sixth thin at every step. The producer plans the
    // build, reads StrandsSelected / StrandsAvailable out of the stats, and
    // compensates on THAT.
    //
    // LINEAR, capped, and never below 1: see the file header for why it is 1/k
    // and not 1/sqrt(k).
    [[nodiscard]] f32 GroomProxyWidthCompensation(f32 achievedFraction, f32 maxScale) noexcept;

    // -------------------------------------------------------------------------
    // The hysteresis state and the decision
    // -------------------------------------------------------------------------

    // One groom's tier memory, advanced once per frame.
    //
    // It lives with the PRODUCER, keyed by entity, for the reason #1252 gives
    // for putting GroomLodState in Scene rather than in the pass: a pass runs
    // once per CAMERA and would advance the hold twice in a split-screen scene,
    // halving it. The ray-traced scene is built once per frame, so the producer
    // is the place that advances this exactly once.
    struct GroomProxyState
    {
        // 4-byte members first, the two one-byte enums and their explicit
        // padding last: operator== is a whole-object memcmp (issue #1019).
        u32 StableFrames = 0;
        GroomProxyTier Tier = GroomProxyTier::Detailed;
        GroomProxyTier Requested = GroomProxyTier::Detailed;
        u8 Pad0 = 0;
        u8 Pad1 = 0;

        [[nodiscard]] auto operator==(const GroomProxyState& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(GroomProxyState) == 8,
                  "GroomProxyState is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<GroomProxyState>);

    // EVERY FIELD IS A RESULT, never a request — GroomCoatShadowInputs' rule.
    struct GroomProxyInputs
    {
        /// Apparent size in pixels of the render target's height, from
        /// EstimateProjectedPixelSize against Renderer3D::GetLODViewParams().
        f32 PixelSize = 0.0f;

        /// Curves the cooked set this frame selected actually carries. Zero
        /// means there is nothing to represent.
        u32 StrandsAvailable = 0;

        /// The frame resolved a ray-traced consumer that would read this coat.
        bool Requested = false;
    };

    struct GroomProxyDecision
    {
        // 4-byte members first, the enums and their padding last: operator== is
        // a whole-object memcmp (issue #1019).
        GroomProxyRefusalReason Reason = GroomProxyRefusalReason::NotRequested;

        /// Strands this tier asks the build for, after the tier's budget.
        u32 StrandBudget = 0;

        /// The apparent size the decision was made at, carried so a panel can
        /// show the input beside the answer.
        f32 PixelSize = 0.0f;

        GroomProxyTier Tier = GroomProxyTier::Detailed;

        /// True on the ONE frame the tier actually changed. Carried on the
        /// decision rather than recomputed by each consumer, because the only
        /// thing that knows is the state this call just advanced — a consumer
        /// comparing "what I have now" against "what I had last frame" would be
        /// keeping a second copy of the hysteresis, which is how two copies
        /// drift. A counter that stays at the groom count IS thrashing.
        bool TierChanged = false;
        u8 Pad0 = 0;
        u8 Pad1 = 0;

        [[nodiscard]] constexpr bool IsRefused() const noexcept
        {
            return Reason != GroomProxyRefusalReason::None;
        }

        [[nodiscard]] auto operator==(const GroomProxyDecision& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(GroomProxyDecision) == 16,
                  "GroomProxyDecision is compared with a whole-object memcmp: it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<GroomProxyDecision>);

    /// Advances `state` by one frame and returns this frame's tier.
    ///
    /// PURE GIVEN (inputs, state): it reads no clock, no camera and no global.
    /// `state` is in/out because the hysteresis IS the state — AdvanceGroomLod's
    /// reason, and the same failure it prevents.
    ///
    /// A non-finite or negative PixelSize takes the COARSE tier rather than
    /// being rejected: an unusable apparent size must not buy a coat the
    /// expensive representation, and the raster tier is unaffected either way.
    GroomProxyDecision AdvanceGroomProxyTier(const GroomProxyInputs& inputs, GroomProxyState& state) noexcept;

    // -------------------------------------------------------------------------
    // The frame budget
    // -------------------------------------------------------------------------

    // Reservation is TRANSACTIONAL: an oversized request consumes no budget, so
    // one unaffordable coat cannot starve the ones behind it. Subtraction
    // guards addition, and the multiplication stays in the caller's checked u64
    // — VegetationFrameBudget's shape, for the same reasons.
    struct GroomProxyFrameBudget
    {
        u32 Updates = 0u;
        u32 Vertices = 0u;
        u32 Triangles = 0u;

        // True when no further coat can be refreshed whatever its size.
        //
        // EXISTS SO THE WORK CAN BE SKIPPED, not merely the commit. The
        // vertex and triangle caps can only be tested against a real
        // size, which is known only after the conversion has run — so
        // without this a scene past the budget would pay a full strand
        // build and ribbon conversion PER COAT PER FRAME for work it then
        // throws away. This is the one cap that can be tested for free.
        [[nodiscard]] bool Exhausted() const noexcept
        {
            return Updates >= GroomProxyPolicy::UpdatesPerFrame;
        }

        [[nodiscard]] bool Reserve(u64 vertices, u64 triangles) noexcept
        {
            if (Updates >= GroomProxyPolicy::UpdatesPerFrame ||
                vertices > GroomProxyPolicy::VerticesPerFrame - Vertices ||
                triangles > GroomProxyPolicy::TrianglesPerFrame - Triangles)
            {
                return false;
            }
            ++Updates;
            Vertices += static_cast<u32>(vertices);
            Triangles += static_cast<u32>(triangles);
            return true;
        }
    };

    // -------------------------------------------------------------------------
    // The geometry
    // -------------------------------------------------------------------------

    struct GroomProxyConversionSettings
    {
        /// Multiplies every radius. The compensation from
        /// GroomProxyWidthCompensation, times the request's own WidthScale.
        f32 WidthScale = 1.0f;

        /// Two ribbons per segment instead of one. True in every shipping
        /// configuration; the single-ribbon arm exists so the analysis document
        /// can measure what the second one buys. See the file header.
        ///
        /// The three pad bytes are EXPLICIT because operator== below is a
        /// whole-object memcmp and implicit padding compares unequal when the
        /// values are equal (issue #1019).
        bool CrossedRibbons = true;
        u8 Pad0 = 0;
        u8 Pad1 = 0;
        u8 Pad2 = 0;

        [[nodiscard]] auto operator==(const GroomProxyConversionSettings& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(sizeof(GroomProxyConversionSettings) == 8,
                  "GroomProxyConversionSettings is compared with a whole-object memcmp: no implicit padding");

    struct GroomProxyMeshStats
    {
        u32 SegmentCount = 0;    ///< Segments the conversion accepted.
        u32 SegmentsDropped = 0; ///< Degenerate or non-finite segments skipped.
        u32 VertexCount = 0;
        u32 IndexCount = 0;
        u64 VertexBytes = 0;
        u64 IndexBytes = 0;

        [[nodiscard]] u32 TriangleCount() const noexcept
        {
            return IndexCount / 3u;
        }

        [[nodiscard]] auto operator==(const GroomProxyMeshStats&) const -> bool = default;
    };

    // Turns the raster strand mesh into WORLD-INDEPENDENT triangles.
    //
    // It reads GroomStrandMesh's own output rather than the cooked curves, and
    // that is the load-bearing choice: the binding deformation (#1249), the
    // guide simulation (#1250), the coat's per-strand width (#1251) and the LOD
    // level (#1252) have all already been applied to those vertices. Rebuilding
    // from the curves would be a second evaluation of four features that would
    // then have to agree with the first, and the frame where they disagreed
    // would be a coat whose shadow is at last frame's pose.
    //
    // THE LAYOUT IT RELIES ON, named so a change to it fails here rather than
    // silently: BuildGroomStrandMesh emits exactly FOUR vertices per segment,
    // in the order (-side at P0), (+side at P0), (+side at P1), (-side at P1).
    // So vertex 4s+0 carries P0 and its radius and vertex 4s+2 carries P1 and
    // its radius, and GroomRayTracingProxyTest asserts that correspondence
    // against a real build rather than trusting this comment.
    //
    // `outVertices` and `outIndices` are cleared first. Pure: the same input
    // and settings always produce the same bytes, which is what lets a cache
    // key on the pair.
    GroomProxyMeshStats ConvertGroomStrandMeshToProxy(std::span<const GroomStrandVertex> strandVertices,
                                                      const GroomProxyConversionSettings& settings,
                                                      std::vector<Vertex>& outVertices, std::vector<u32>& outIndices,
                                                      glm::vec3* outBoundsMin = nullptr,
                                                      glm::vec3* outBoundsMax = nullptr);

    // -------------------------------------------------------------------------
    // The error metric — criterion 1's "compare detailed vs proxy", as a number
    // -------------------------------------------------------------------------

    // The segments a strand mesh stands for, as tapered cylinders, with
    // `widthScale` applied. The same CoatSegment #1248 already measures with, so
    // a row in the analysis document and the thing the renderer traces cannot be
    // two different shapes.
    void CollectGroomProxySegments(std::span<const GroomStrandVertex> strandVertices, f32 widthScale,
                                   std::vector<GroomCoatShadow::CoatSegment>& outSegments);

    // The area this segment set projects onto a plane perpendicular to
    // `direction`, IF NO SEGMENT OCCLUDED ANOTHER: sum of 2 * rbar * L * sin(t,
    // direction). See the file header for why that is the right quantity and
    // where it stops being the truth.
    //
    // Returns 0 for an empty set or a degenerate direction. f64 because a coat
    // is tens of thousands of terms each a fraction of a square millimetre, and
    // an f32 accumulator loses the tail of that sum to rounding.
    [[nodiscard]] f64 DirectionalCoverage(std::span<const GroomCoatShadow::CoatSegment> segments,
                                          const glm::vec3& direction) noexcept;

    struct GroomProxyCoverageError
    {
        /// Mean and maximum of |proxy/detailed - 1| over the direction set.
        f64 MeanRelativeError = 0.0;
        f64 MaxRelativeError = 0.0;
        /// The two totals, averaged over the same directions, so a reader can
        /// see which way the proxy errs rather than only how far.
        f64 DetailedCoverage = 0.0;
        f64 ProxyCoverage = 0.0;
        u32 Directions = 0;

        [[nodiscard]] auto operator==(const GroomProxyCoverageError& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    /// A deterministic, machine-independent direction set: the `count` points
    /// of a Fibonacci sphere. Deterministic and integer-seeded for
    /// GroomCoverage.h's reason — a metric a test cannot re-derive is a number
    /// in a PR body that nothing defends.
    [[nodiscard]] std::vector<glm::vec3> GroomProxyErrorDirections(u32 count);

    /// How far `proxy` is from `detailed`, over GroomProxyErrorDirections.
    [[nodiscard]] GroomProxyCoverageError CompareGroomProxyCoverage(
        std::span<const GroomCoatShadow::CoatSegment> detailed,
        std::span<const GroomCoatShadow::CoatSegment> proxy, u32 directions = 64u);

    // -------------------------------------------------------------------------
    // Counters
    // -------------------------------------------------------------------------

    // What a user reads when an animal is missing from the ray-traced world, or
    // when its coat's shadow is the wrong shape. Criterion 4 asks for proxy
    // error, update time, memory and unsupported cases to be REPORTED; these
    // are those, as counters rather than a log line, so the panel can show them
    // without the renderer having to have said anything.
    struct GroomProxyStats
    {
        u32 GroomsConsidered = 0;
        u32 GroomsRepresented = 0;
        u32 GroomsRefused = 0;

        std::array<u32, GroomProxyTierCount> ByTier{};
        std::array<u32, static_cast<sizet>(GroomProxyRefusalReason::Count)> ByReason{};

        /// Tier changes that actually happened this frame. A steady camera
        /// scores 0; a camera crossing the threshold scores 1 and then 0 for at
        /// least HoldFrames frames. A number that stays at the groom count IS
        /// thrashing, reported before anyone has to see it.
        u32 TierChanges = 0;

        /// Conversions that ran, and what they cost. `Reused` is a coat whose
        /// geometry did not change and kept the structure it had — the number
        /// that should dominate in a still frame.
        u32 Rebuilds = 0;
        u32 Reused = 0;
        /// Coats that wanted a refresh, could not have one this frame, and
        /// KEPT the structure they already had.
        ///
        /// Counted as REPRESENTED, not refused, because they are still in
        /// the ray-traced scene and still occluding — but the frame is
        /// marked incomplete, because one of them is a deforming coat
        /// whose structure is now one frame behind its raster twin. A
        /// number that stays high means the per-frame budget is under this
        /// scene's animated-coat population, which is a capacity fact
        /// rather than a failure.
        u32 RefreshDeferred = 0;
        u32 SegmentsConverted = 0;
        u32 TrianglesBuilt = 0;

        /// Geometry bytes and triangles resident across every proxy.
        ///
        /// RESIDENT, not built-this-frame, and the difference is the whole
        /// reason both exist: `Rebuilds` and `TrianglesBuilt` are this
        /// frame's WORK and correctly fall to zero when a still scene
        /// rebuilds nothing, which would make a panel that showed only
        /// them report a coat-free ray-traced scene every steady frame.
        u64 ResidentBytes = 0;
        u64 ResidentTriangles = 0;

        /// Wall-clock microseconds spent converting and uploading this frame.
        /// Criterion 4's "update time", measured rather than estimated, and on
        /// the CPU side because that is where this producer's whole cost is —
        /// the device build time is RayTracing::SceneStats'.
        u64 UpdateMicroseconds = 0;

        /// The worst compensation any RESIDENT coat carries, and the cap it
        /// is read against. A compensation AT the cap means that coat's
        /// ray-space density is below its raster density, which is the one
        /// way this representation loses coverage silently — so it is
        /// reported rather than clamped out of sight.
        ///
        /// TAKEN FROM THE RESIDENT SET, never from this frame's rebuilds.
        /// The compensation is a property of the structure a coat HAS, not
        /// of whether it was rebuilt just now, and reporting the latter
        /// made a still scene read 0.0x — which says "no compensation",
        /// the opposite of what a resident 22x coat is doing.
        f32 MaxWidthCompensation = 0.0f;
        f32 WidthCompensationCap = GroomProxyPolicy::MaxWidthCompensation;

        /// True when something this frame did not get what it asked for, so a
        /// consumer can treat the frame's proxy set as incomplete without
        /// re-deriving that from the counters.
        bool Complete = true;

        void Record(const GroomProxyDecision& decision) noexcept
        {
            ++GroomsConsidered;
            ByReason[static_cast<sizet>(decision.Reason)] += 1u;
            if (decision.TierChanged)
            {
                ++TierChanges;
            }
            if (decision.IsRefused())
            {
                ++GroomsRefused;
                if (decision.Reason != GroomProxyRefusalReason::NotRequested)
                {
                    Complete = false;
                }
                return;
            }
            ++GroomsRepresented;
            ByTier[static_cast<sizet>(decision.Tier)] += 1u;
        }

        // The reason to put in front of a user. First match in enum order, so
        // it names the most fundamental thing that went wrong rather than the
        // most common symptom. NotRequested is skipped: it is not a failure.
        [[nodiscard]] GroomProxyRefusalReason DominantRefusalReason() const noexcept
        {
            for (sizet i = 0; i < ByReason.size(); ++i)
            {
                const auto reason = static_cast<GroomProxyRefusalReason>(i);
                if (reason == GroomProxyRefusalReason::None || reason == GroomProxyRefusalReason::NotRequested)
                {
                    continue;
                }
                if (ByReason[i] > 0u)
                {
                    return reason;
                }
            }
            return GroomProxyRefusalReason::None;
        }

        void Reset() noexcept
        {
            *this = GroomProxyStats{};
        }
    };
} // namespace OloEngine
