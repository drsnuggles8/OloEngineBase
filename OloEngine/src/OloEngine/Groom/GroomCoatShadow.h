#pragma once

// =============================================================================
// GroomCoatShadow.h — how much coat is between a point and a light. Issue #1248.
//
// WHAT THIS ANSWERS, AND WHY IT IS A NEW THING. #1247 shipped the LOCAL fibre
// response: a strand lit as if it were alone in space. A real coat is not one
// strand, it is tens of thousands of them, and the reason a dense coat reads as
// having depth is that the ones underneath are occluded by the ones on top. So
// the missing quantity is a single scalar per (point, light direction):
//
//     tau(x, L) = the EXPECTED NUMBER OF FIBRE CROSSINGS along the ray from x
//                 towards the light,
//
// from which the coat transmittance is exp(-tau * (1 - exp(-kappa))) — the
// MEAN of the per-ray transmittances over the footprint rather than the
// transmittance of the mean crossing count, see CoatTransmittance and issue
// #1360. Everything in this
// header exists to compute that number, to approximate it cheaply enough for a
// fragment shader, and to MEASURE how far each approximation is from the truth.
//
// WHY "EXPECTED CROSSINGS" AND NOT "OPTICAL DEPTH IN METRES". A fibre is not a
// participating medium with a density in kg/m3; it is a collection of thin
// opaque-ish cylinders. A ray through the coat either misses a fibre or crosses
// it, and what attenuates the light is HOW MANY it crosses — which is a pure
// geometric quantity (fibre length density x diameter x the sine of the angle
// between the ray and the fibre), independent of the pigment. The pigment is
// already in #1247's per-fibre attenuations and must not be applied twice; see
// "the double-count boundary" below.
//
// A single ray's crossing count is an INTEGER and therefore noisy. A fragment
// covers many fibres, so the quantity a shader wants is the expectation over the
// fragment's footprint, which is what ReferenceCoatOpticalDepth averages over a
// deterministic ray bundle. Same reasoning, same shape, as #1246's 16x16
// coverage reference: the ground truth is arithmetic over the real geometry, it
// is deterministic and machine-independent, and every number the analysis
// document reports is re-derived by a test rather than recorded in prose.
//
// THE DOUBLE-COUNT BOUNDARY, stated once and relied on everywhere. The issue's
// own scope note says not to double-count the shadow and transmission terms.
// There are three attenuations in play and they are disjoint by construction:
//
//   1. INSIDE one fibre — exp(-sigma_a * chord) in GroomFibreCommon.glsl. Owned
//      by #1247. This header never touches sigma_a and never sees a colour.
//   2. BETWEEN the fibres of one coat — tau, computed here. Greyscale, geometric.
//   3. EVERYTHING ELSE IN THE SCENE occluding the coat — the engine's shadow map.
//      Not computed here at all.
//
// A consequence worth stating because it is the trap: a groom that is also a
// shadow-map caster appears in (3) as well as in (2), and a strand that read
// both would be shadowed by its own coat twice. GroomCoatShadowTechnique.h owns
// that seam; this header only promises that (2) contains the groom's own strands
// and nothing else.
//
// WHAT IS MEASURED HERE AND WHAT IS NOT. This is a CPU model: deterministic,
// integer-hashed, IEEE-754, no <random>, no GPU. It settles QUALITY and MEMORY,
// which are machine-independent, and it counts the WORK each candidate does
// (segments touched, samples fetched) so cost can be compared without a clock.
// Wall-clock cost on named hardware is a separate measurement and lives in the
// analysis document, not in an assertion.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"

#include <glm/glm.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class GroomAsset;
    struct GroomRootTransform;
    class GroomBindingAsset;
    struct GroomStrandVertex;

    namespace GroomCoatShadow
    {
        // ── The geometry every candidate is built from ──────────────────────

        // One curve segment as a tapered cylinder, in the space the whole
        // comparison runs in (groom object space unless a model matrix was
        // supplied to BuildCoatSegments).
        //
        // RADII, not diameters. GroomAsset stores DIAMETERS — the Alembic
        // convention — and the halving happens exactly once, in
        // BuildCoatSegments, for the same reason GroomStrandMesh halves exactly
        // once: a second halving downstream produces a coat half as thick as
        // authored, which looks like a plausible groom.
        struct CoatSegment
        {
            glm::vec3 A{ 0.0f };
            glm::vec3 B{ 0.0f };
            f32 RadiusA = 0.0f;
            f32 RadiusB = 0.0f;

            [[nodiscard]] bool operator==(const CoatSegment&) const = default;
        };

        // How much of a groom to turn into segments. Mirrors
        // GroomStrandBuildSettings' contract deliberately: a budget over a
        // cooked groom is a STRIDE, never a prefix, because the cook sorts
        // curves so each group is contiguous and "the first N strands" is one
        // side of the animal (groom-strand-visibility.md rule 7). A prefix here
        // would measure the density of a bald flank.
        struct CoatSampleSettings
        {
            /// Upper bound on curves turned into segments. Applied as a stride.
            u32 MaxStrands = 100000;
            /// Hard upper bound on emitted segments, so a corrupt groom cannot
            /// size an unbounded allocation.
            u32 MaxSegments = 2000000;
            /// Multiplies the cooked object-space DIAMETERS before halving.
            f32 WidthScale = 1.0f;
            /// Guide curves only — the coarse coat, useful as a cheap build
            /// input and as the natural shadow LOD (see CoatLodPolicy).
            bool GuidesOnly = false;
        };

        // Fills `outSegments` (cleared first) with `groom`'s segments
        // transformed by `model`. Returns the number of segments emitted.
        //
        // A segment whose endpoints are not finite, or whose radii are not
        // finite and non-negative, is DROPPED rather than clamped: a coat built
        // from a corrupt curve would poison every measurement downstream with a
        // number that looks like data.
        [[nodiscard]] u32 BuildCoatSegments(const GroomAsset& groom, const glm::mat4& model,
                                            const CoatSampleSettings& settings,
                                            std::vector<CoatSegment>& outSegments);

        // The axis-aligned bounds of a segment set, INFLATED by each segment's
        // radius. Returns false and leaves the outputs untouched for an empty
        // set — a zero-extent box would divide by zero in every voxel mapping.
        //
        // A NON-FINITE segment is SKIPPED, not fatal. Taking the min/max over
        // one NaN endpoint poisons the whole box, so an earlier version
        // returned false and a single corrupt curve cost the entire coat its
        // shadow — the opposite of BuildCoatSegments' promise that a bad curve
        // is dropped rather than allowed to poison the measurement. False now
        // means "nothing usable here at all".
        [[nodiscard]] bool CoatSegmentBounds(std::span<const CoatSegment> segments, glm::vec3& outMin,
                                             glm::vec3& outMax);

        // ── A deformed coat (#1426) ─────────────────────────────────────────
        //
        // A BOUND groom is baked from the strands the pass DRAWS, not from the
        // asset's rest curves. Those vertices are already in groom object space
        // with the body's pose applied, and they reach the screen through the
        // same model matrix the march inverts — so a volume baked from them sits
        // in the space the shader already looks it up in, and nothing on the
        // GPU side changes. Baking the rest curves instead is what #1248 refused
        // to do: the coat would carry its bind-pose shadow around.

        // Fills `outSegments` (cleared first) from a GroomStrandMesh vertex
        // stream: FOUR corners per segment, corner 0 at P0 and corner 2 at P1
        // (see BuildGroomStrandMesh's corner order). Returns the number emitted.
        //
        // `widthScale` multiplies the stored RADII. The mesh has already halved
        // the cooked diameters and applied the coat's per-strand width, so the
        // one lever still outstanding is the per-groom WidthScale the shader
        // applies — the same one BuildCoatSegments takes. A stream whose length
        // is not a multiple of four is not a strand mesh and yields nothing.
        [[nodiscard]] u32 BuildCoatSegmentsFromStrandVertices(std::span<const GroomStrandVertex> vertices,
                                                              f32 widthScale, std::vector<CoatSegment>& outSegments);

        // The pose a volume was baked at: each drawn segment's centreline
        // midpoint, in segment order. A SNAPSHOT of the geometry, not of the
        // skeleton — two poses that put every strand in the same place are the
        // same coat as far as the shadow is concerned, whatever the bones did.
        void CaptureCoatPose(std::span<const GroomStrandVertex> vertices, std::vector<glm::vec3>& outMidpoints);

        // How far the drawn coat has moved since `bakedMidpoints` was captured:
        // the LARGEST midpoint displacement, in object units.
        //
        // The maximum, not the mean. A walk moves the legs' fur a hand's width
        // while the back barely shifts, and a mean over the whole coat would
        // average the one region that is visibly wrong into the ninety percent
        // that is not.
        //
        // +INFINITY when the two cannot be compared — a different segment count
        // (a LOD hand-over rebuilt the curve set) or a non-finite point. An
        // incomparable pose must read as "moved infinitely far", never as "did
        // not move", or a bake of a different curve set would be kept forever.
        [[nodiscard]] f32 MaxCoatPoseDrift(std::span<const glm::vec3> bakedMidpoints,
                                           std::span<const GroomStrandVertex> vertices) noexcept;

        // ── The same three, over a pose given as centrelines (#1427) ─────
        //
        // A GPU-deformed coat has no CPU vertex stream: its drawn pose is
        // evaluated on the CPU as one CoatSegment per drawn segment
        // (EvaluateGroomDeformedPose), with the radii the stream holds and no
        // width scale. These overloads read that, and the stream overloads above
        // read corners 0 and 2 of a GroomStrandMesh stream; both are the same
        // templates underneath, so the two paths cannot disagree about what a
        // pose, a midpoint or a drift is.
        [[nodiscard]] u32 BuildCoatSegmentsFromPose(std::span<const CoatSegment> pose, f32 widthScale,
                                                    std::vector<CoatSegment>& outSegments);
        void CaptureCoatPose(std::span<const CoatSegment> pose, std::vector<glm::vec3>& outMidpoints);
        [[nodiscard]] f32 MaxCoatPoseDrift(std::span<const glm::vec3> bakedMidpoints,
                                           std::span<const CoatSegment> pose) noexcept;

        // A GroomStrandMesh stream's drawn pose as centrelines — corners 0 and 2
        // of each segment. `outPose` is cleared first; a stream whose length is
        // not a multiple of four yields nothing.
        void CoatPoseFromStrandVertices(std::span<const GroomStrandVertex> vertices, std::vector<CoatSegment>& outPose);

        // When a deformed coat's volume is rebuilt.
        //
        // A DRIFT BOUND, not a frame count. A cadence of "every N frames" bounds
        // the cost and leaves the error to whatever the animation does in N
        // frames — a trot and an idle breath get the same budget. A bound on how
        // far the strands have moved since the bake bounds the ERROR, and the
        // cost then follows the motion: an idle coat costs nothing, a galloping
        // one pays per frame. The numbers behind the default are in
        // docs/agent-rules/groom-deformed-coat-self-shadowing.md.
        struct CoatRebakePolicy
        {
            /// Rebuild once the drawn strands have moved further than this since
            /// the bake, in voxels of the volume in force.
            f32 MaxDriftVoxels = 0.5f;

            /// Beyond this many voxels a volume the pass could not rebuild is
            /// STALE and is not sampled: the coat reads fully lit (rule 10)
            /// rather than shadowed by where its strands used to be. Only
            /// reachable when a rebake failed or `RebakeOnDrift` is off; it is
            /// the detector the negative control switches the rebake off to
            /// exercise.
            f32 StaleDriftVoxels = 2.0f;

            /// Rebuild when the drift bound is crossed. OFF freezes only the
            /// DRIFT-triggered rebake, for a diagnostic or a negative control:
            /// a shadow-LOD, resolution or WidthScale change still rebuilds,
            /// because those make the resident bake a different coat.
            bool RebakeOnDrift = true;

            /// Bit-exact per float, per cpp-coding-quality §2a: a defaulted
            /// operator== here would be a float `==`. Field by field rather
            /// than a whole-object memcmp, because the trailing bool leaves
            /// padding.
            [[nodiscard]] auto operator==(const CoatRebakePolicy& other) const -> bool
            {
                return Math::BitwiseEqual(MaxDriftVoxels, other.MaxDriftVoxels) &&
                       Math::BitwiseEqual(StaleDriftVoxels, other.StaleDriftVoxels) &&
                       RebakeOnDrift == other.RebakeOnDrift;
            }
        };

        // A policy with a non-finite or non-positive bound is replaced by the
        // default rather than obeyed: a NaN bound compares false against every
        // drift and would freeze the bake forever, and a zero one would rebuild
        // every frame whether or not anything moved.
        [[nodiscard]] CoatRebakePolicy SanitizeCoatRebakePolicy(const CoatRebakePolicy& policy) noexcept;

        // Drift expressed in voxels of a volume whose SMALLEST voxel edge is
        // `voxelSize`. +infinity for a non-finite drift or a degenerate voxel,
        // for the reason MaxCoatPoseDrift gives.
        [[nodiscard]] f32 CoatDriftInVoxels(f32 drift, f32 voxelSize) noexcept;

        // Should the volume be rebuilt this frame?
        [[nodiscard]] bool CoatRebakeIsDue(f32 driftVoxels, const CoatRebakePolicy& policy) noexcept;

        // Is the volume too far from the drawn coat to be sampled at all?
        [[nodiscard]] bool CoatBakeIsStale(f32 driftVoxels, const CoatRebakePolicy& policy) noexcept;

        // ── The ground truth ────────────────────────────────────────────────

        // How the reference bundle is shaped. A single ray's crossing count is
        // an integer, and a fragment covers many fibres, so the truth a shader
        // wants is the EXPECTATION over the fragment footprint.
        struct ReferenceSettings
        {
            /// Rays in the bundle. 64 is the value the analysis reports; below
            /// 16 the reference's own quantisation (1/N) stops being finer than
            /// the error it is meant to resolve, so values under 16 are
            /// REJECTED rather than silently producing a reference that
            /// flatters everything. Same refusal, same reason, as
            /// ReferenceCoverage's supersample floor.
            u32 Rays = 64;

            /// Radius of the disc the bundle is jittered over, perpendicular to
            /// the ray. In the segment set's units.
            ///
            /// THE FOOTPRINT MUST SPAN SEVERAL STRAND SPACINGS, and this is the
            /// one setting that silently produces a wrong reference if it does
            /// not. A coat is close to a regular lattice, and a footprint
            /// narrower than its pitch aliases against it: measured on a 40x40
            /// lattice of 0.6 mm-radius strands at 5.1 mm pitch, whose analytic
            /// answer is 9.36 crossings, the reference reads
            ///
            ///     footprint 0.5 mm -> 0.00   (the disc fits between strands)
            ///     footprint 2.0 mm -> 11.55
            ///     footprint 4.0 mm -> 8.93
            ///     footprint  20 mm -> 9.54
            ///
            /// so it converges only once the disc is several pitches across,
            /// and below one pitch it reports a confident ZERO — a coat that
            /// casts no shadow at all. Raising the ray count does not fix it;
            /// every ray in a too-small disc misses the same way. Judge a
            /// footprint against the coat's spacing, never against its radius.
            f32 FootprintRadius = 0.01f;

            /// Integer hash seed for the jitter. Changing it must not move any
            /// reported statistic beyond the bundle's own noise floor, and a
            /// test asserts that.
            u32 Seed = 1248;

            /// Distance the ray is traced. Rays are cast TOWARDS the light, so
            /// this bounds how far out of the coat the search goes; the default
            /// 0 means "until the segment bounds are left", which is what a
            /// coat wants and what every caller should use unless it is
            /// deliberately measuring a truncated march.
            f32 MaxDistance = 0.0f;
        };

        // Expected fibre crossings along the ray from `origin` towards
        // `direction` (which is normalised internally; a zero direction returns
        // 0). Exact ray-cylinder intersection, averaged over the bundle.
        //
        // This is O(rays * segments) and is a REFERENCE, not a runtime path: a
        // 140k-segment coat at 96 rays over 600 probes is 8e9 intersection
        // tests, which does not finish. Use CoatSegmentGrid below for anything
        // beyond a toy; this overload exists because the brute-force answer is
        // the definition the grid is checked against, and a ground truth whose
        // only implementation is accelerated is a ground truth nobody can
        // audit.
        [[nodiscard]] f64 ReferenceCoatOpticalDepth(std::span<const CoatSegment> segments, const glm::vec3& origin,
                                                    const glm::vec3& direction, const ReferenceSettings& settings);

        // A uniform grid over the segment set, so the reference can be taken on
        // a coat the size of a real one.
        //
        // It changes the COST, never the ANSWER: a segment is visited if its
        // bounding box overlaps a cell the ray enters, and the same exact
        // ray-cylinder test then decides the hit. The grid can only add
        // candidates, never remove a real one, which is the property that lets
        // GroomCoatShadowReference.TheGridChangesTheCostAndNotTheAnswer assert
        // bit-equality rather than a tolerance.
        struct CoatSegmentGrid
        {
            glm::ivec3 Dimensions{ 0 };
            glm::vec3 BoundsMin{ 0.0f };
            glm::vec3 BoundsMax{ 0.0f };
            /// Prefix-sum offsets, `x*y*z + 1` entries; cell c owns
            /// `Indices[CellStart[c] .. CellStart[c+1])`.
            std::vector<u32> CellStart;
            std::vector<u32> Indices;

            [[nodiscard]] bool IsValid() const noexcept;
            [[nodiscard]] u64 CpuBytes() const noexcept;
        };

        // `cellsPerAxis` is a hint on the longest axis; cells stay cubic for the
        // same reason the density volume's voxels do.
        [[nodiscard]] bool BuildCoatSegmentGrid(std::span<const CoatSegment> segments, u32 cellsPerAxis,
                                                CoatSegmentGrid& outGrid);

        [[nodiscard]] f64 ReferenceCoatOpticalDepthGrid(std::span<const CoatSegment> segments,
                                                        const CoatSegmentGrid& grid, const glm::vec3& origin,
                                                        const glm::vec3& direction,
                                                        const ReferenceSettings& settings);

        // Transmittance from an optical depth: exp(-tau * (1 - exp(-kappa))),
        // clamped into [0,1] and finite for every input.
        //
        // THE MEAN OF THE TRANSMITTANCES, NOT THE TRANSMITTANCE OF THE MEAN
        // (issue #1360). `tau` is E[N] — the expected number of fibre crossings
        // over the fragment's footprint — and what the footprint receives is
        // E[exp(-kappa N)]. Jensen's inequality puts the naive exp(-kappa * tau)
        // strictly below it, so that form OVER-DARKENS, by an amount set by the
        // coat's disorder rather than by any authored value. Modelling N as
        // Poisson gives the exact mean in closed form as that distribution's
        // probability generating function at exp(-kappa), which is the
        // expression above. The residual on a perfectly ORDERED coat, where N
        // has no variance and exp(-kappa * tau) was already right, is a slight
        // over-brightening measured by
        // CoatTransportReference.TheGapClosesOnAnOrderedCoat.
        //
        // One consequence is worth stating because it is a behaviour change, not
        // a rounding one: transmittance now floors at exp(-tau) as kappa grows,
        // because a coat of perfectly opaque fibres still passes light through
        // wherever the footprint happened to cross nothing, and for a Poisson N
        // that is exactly P(N = 0) = exp(-tau).
        //
        // A NON-FINITE tau or kappa reads FULLY LIT, including +infinity. That
        // is deliberate rather than a missed `exp(-inf) == 0`: the march cannot
        // produce an infinity from a finite density over a finite span, so one
        // means the representation itself is corrupt — and a corrupt occlusion
        // term has to fail bright, where it is visible as "this did not run",
        // rather than black, where it is indistinguishable from a correct
        // silhouette. A merely large tau is a real measurement and does reach
        // zero, so the rule costs nothing where it matters.
        //
        // `kappa` is the per-crossing extinction and is DIMENSIONLESS: it
        // describes ONE FIBRE, so 1.0 means a single crossing passes 1/e of the
        // light through it. (Before #1360 it described the aggregate — one
        // EXPECTED crossing attenuating to 1/e — which is the same number only
        // when the crossing count is deterministic.) It is an authored coat
        // property (how opaque one fibre is to direct light), deliberately
        // separate from the pigment, which already attenuates INSIDE the fibre
        // in #1247's model. See the double-count boundary in the file header.
        [[nodiscard]] f32 CoatTransmittance(f64 opticalDepth, f32 kappa) noexcept;

        // ── The candidates ──────────────────────────────────────────────────

        // Which representation answers tau(x, L).
        //
        // Ordered cheapest-first. The enum is the comparison's subject AND the
        // runtime's choice, so the two cannot drift: a mode measured here and a
        // mode shipped are literally the same enumerator.
        enum class CoatShadowMode : u8
        {
            /// No coat shadowing at all. #1247's state, kept as the A/B control
            /// every measurement is quoted against — and as the honest answer
            /// when nothing can be built. tau is identically 0.
            None = 0,

            /// A scalar fibre-area density field over the groom's bounds,
            /// ray-marched towards the light. Isotropic: it assumes the fibres
            /// in a voxel point in every direction equally, so it cannot know
            /// that a combed coat is more transparent ALONG the hair.
            IsotropicDensityVolume,

            /// The same grid, plus the voxel's mean fibre direction and how
            /// coherent it is. The projected area a voxel presents to a ray is
            /// then direction-dependent, which is the whole reason a coat looks
            /// different lit along the lay of the hair than across it.
            AnisotropicDensityVolume,

            /// Light-space layered opacity (deep opacity maps, Yuksel & Keyser
            /// 2008): a front-depth map per texel, then K layers of accumulated
            /// crossings at depths warped to start at that front depth. Angular
            /// resolution is the light's, not the coat's.
            DeepOpacityMap,

            Count
        };

        [[nodiscard]] constexpr std::string_view ToString(CoatShadowMode mode)
        {
            switch (mode)
            {
                case CoatShadowMode::None:
                    return "None";
                case CoatShadowMode::IsotropicDensityVolume:
                    return "IsotropicDensityVolume";
                case CoatShadowMode::AnisotropicDensityVolume:
                    return "AnisotropicDensityVolume";
                case CoatShadowMode::DeepOpacityMap:
                    return "DeepOpacityMap";
                case CoatShadowMode::Count:
                    break;
            }
            return "Unknown";
        }

        // Range check for a value that came off disk, out of a script or over
        // MCP. Count is NOT valid — it is the enumerator count, and accepting it
        // would put a mode nobody authored into the renderer.
        [[nodiscard]] inline constexpr bool IsValidCoatShadowMode(i32 value) noexcept
        {
            return value >= 0 && value < static_cast<i32>(CoatShadowMode::Count);
        }

        // Whether a mode's representation depends on the LIGHT as well as on
        // the coat. A volume is built once per groom and serves every light; a
        // deep opacity map is per light and must be rebuilt when the light
        // moves. That difference is the whole of the update policy, so it is a
        // function rather than a comment.
        [[nodiscard]] constexpr bool CoatShadowModeIsPerLight(CoatShadowMode mode) noexcept
        {
            return mode == CoatShadowMode::DeepOpacityMap;
        }

        // ── Density volume ──────────────────────────────────────────────────

        // A voxel grid over the coat's bounds.
        //
        // Each voxel holds the fibre AREAL DENSITY — total fibre length times
        // diameter per unit volume, in 1/length — and the mean fibre direction
        // weighted by that length. The mean direction's MAGNITUDE is the
        // coherence: 1 when every fibre in the voxel is parallel, 0 when they
        // cancel. Both are needed: the density alone gives the isotropic mode,
        // and the direction turns it into the anisotropic one, so one build
        // serves both candidates and the comparison cannot accidentally be
        // measuring two different bakes.
        struct DensityVolume
        {
            glm::ivec3 Dimensions{ 0 };
            glm::vec3 BoundsMin{ 0.0f };
            glm::vec3 BoundsMax{ 0.0f };
            /// Areal density per voxel, `Dimensions.x*y*z` entries, x fastest.
            std::vector<f32> Density;
            /// Mean fibre direction times coherence, same indexing.
            std::vector<glm::vec3> Direction;

            [[nodiscard]] bool IsValid() const noexcept;
            /// Bytes the GPU copy occupies: ONE RGBA32F 3D texture at 16 bytes
            /// a voxel, which is what GroomRenderPass actually uploads. Not the
            /// tighter packing the channels would allow — see the definition
            /// for why RGBA16F is unavailable through Texture3D::SetData.
            [[nodiscard]] u64 GpuBytes() const noexcept;
            [[nodiscard]] glm::vec3 VoxelSize() const noexcept;
        };

        struct DensityVolumeSettings
        {
            /// Voxels along the LONGEST bounds axis; the other two are derived
            /// so voxels stay cubic. Cubic voxels are not a nicety: a march
            /// step is one length in every direction, and anisotropic voxels
            /// would make the optical depth depend on which way the light
            /// happens to point.
            u32 Resolution = 64;
            /// Padding around the coat bounds, as a fraction of the bounds'
            /// longest axis. A coat whose outermost strands sit exactly on the
            /// boundary loses half of each boundary voxel's density to
            /// clamping; a little padding costs memory and removes a rim
            /// artefact that reads as a bald edge.
            f32 BoundsPadding = 0.02f;
        };

        struct DensityVolumeBuildStats
        {
            u32 SegmentsBinned = 0;
            u32 SegmentsRejected = 0;
            /// Voxels with any density at all. The fill ratio is what says
            /// whether the chosen resolution is resolving the coat or smearing
            /// it: a coat that fills 2 % of its bounding box at 64^3 is telling
            /// you the box is mostly air.
            u32 OccupiedVoxels = 0;
            u32 TotalVoxels = 0;
            /// Sum of length*diameter binned, for the conservation check: a
            /// build that loses fibre area is losing shadow, silently.
            f64 TotalArealMass = 0.0;
        };

        // Bins every segment into the grid by walking it in sub-voxel steps and
        // depositing length*diameter. Returns false (and leaves `outVolume`
        // cleared) for an empty segment set or an out-of-range resolution.
        [[nodiscard]] bool BuildDensityVolume(std::span<const CoatSegment> segments,
                                              const DensityVolumeSettings& settings, DensityVolume& outVolume,
                                              DensityVolumeBuildStats* outStats = nullptr);

        // Ray-marches the volume from `origin` towards `direction`, returning
        // expected crossings. `stepScale` is the march step as a fraction of a
        // voxel; 1.0 is one voxel per step.
        //
        // `anisotropic` selects between the two volume modes. It is a parameter
        // rather than two functions because the ONLY difference is how the
        // voxel's projected area is derived, and two copies of a ray march
        // would drift.
        [[nodiscard]] f64 SampleDensityVolume(const DensityVolume& volume, const glm::vec3& origin,
                                              const glm::vec3& direction, bool anisotropic, f32 stepScale = 1.0f,
                                              u32* outSteps = nullptr);

        // ── Deep opacity map ────────────────────────────────────────────────

        // Light-space layered accumulated crossings.
        //
        // Layer k of texel (x,y) holds the expected crossings between the
        // light and depth `Front(x,y) + LayerDepth(k)`, so a lookup is a
        // depth-ordered interpolation in a texel's own column. The layers start
        // at the coat's own front surface rather than at a fixed distance,
        // which is what makes them follow the silhouette instead of slicing it
        // — the difference between deep opacity maps and the flat opacity
        // shadow maps that preceded them.
        struct DeepOpacityMap
        {
            u32 Width = 0;
            u32 Height = 0;
            u32 Layers = 0;
            /// Light-space transform: world -> [0,1]^2 x depth.
            glm::mat4 LightViewProjection{ 1.0f };
            /// Nearest coat depth per texel, in the light's normalised depth.
            /// Infinity where the coat does not cover the texel.
            std::vector<f32> Front;
            /// Accumulated crossings, `Width*Height*Layers`, layer-major per
            /// texel (texel t, layer k lives at t * Layers + k).
            std::vector<f32> Accumulated;
            /// Layer depth offsets from `Front`, in light-space depth units.
            std::vector<f32> LayerOffsets;

            [[nodiscard]] bool IsValid() const noexcept;
            /// Bytes the GPU copy would occupy: an R32F front map plus an
            /// R16F array of `Layers` slices.
            [[nodiscard]] u64 GpuBytes() const noexcept;
        };

        struct DeepOpacityMapSettings
        {
            u32 Width = 512;
            u32 Height = 512;
            /// Layers per texel. 4 is the count the original paper uses and
            /// what an RGBA texture holds in one fetch; more layers resolve the
            /// coat's interior better and cost linearly.
            u32 Layers = 4;
            /// Depth of the last layer, as a fraction of the coat's extent
            /// along the light direction. Layers are distributed over
            /// [0, Extent] with the first one thin, because the density
            /// gradient at the coat's surface is where the visible structure
            /// is.
            f32 LayerSpan = 1.0f;
        };

        struct DeepOpacityMapBuildStats
        {
            u32 SegmentsRasterised = 0;
            u32 SegmentsRejected = 0;
            u32 CoveredTexels = 0;
            u32 TotalTexels = 0;
        };

        // Builds a map for a directional light travelling along
        // `lightDirection` (the direction the light TRAVELS, matching
        // LightData::direction and every lit shader in the engine).
        [[nodiscard]] bool BuildDeepOpacityMap(std::span<const CoatSegment> segments, const glm::vec3& lightDirection,
                                               const DeepOpacityMapSettings& settings, DeepOpacityMap& outMap,
                                               DeepOpacityMapBuildStats* outStats = nullptr);

        // Expected crossings between the light and `position`. Returns 0 for a
        // point outside the map's footprint or in front of the coat — an
        // unlit-by-this-representation point is UNSHADOWED, never fully
        // shadowed, which is the same "clear the mask to fully lit" rule
        // technique-selection-seams.md states: the failure mode of a missing
        // lookup must be a bright coat, not a black one.
        [[nodiscard]] f64 SampleDeepOpacityMap(const DeepOpacityMap& map, const glm::vec3& position);

        // ── Shadow LOD ──────────────────────────────────────────────────────

        // What the representation's resolution should be at a given apparent
        // size, and how often it should be rebuilt.
        //
        // A POLICY AS A VALUE, for the same reason the technique is:
        // "representation resolution and update policy are inspectable" is an
        // acceptance criterion, and a policy scattered across three call sites
        // cannot be printed in a panel.
        struct CoatLodPolicy
        {
            /// Resolution at LOD 0 (the longest-axis voxel count, or the deep
            /// map's width).
            u32 BaseResolution = 64;
            /// Halvings allowed. Each LOD step halves the resolution, so the
            /// memory falls 8x for a volume and 4x for a map.
            u32 MaxLodSteps = 3;
            /// Apparent coat size, in pixels of the frame's height, at which
            /// LOD 0 is used. Below `PixelSize / 2^k` the policy asks for LOD k.
            f32 PixelSizeForLod0 = 512.0f;
            /// Hard floor on the resolution, whatever the LOD says. Below this
            /// the representation stops resolving the coat at all and the
            /// honest answer is to fall back rather than to march a 4^3 grid.
            u32 MinResolution = 8;
        };

        // The LOD step and resolution this policy asks for at `pixelSize`.
        // Pure and monotone: a coat that gets smaller never asks for MORE
        // resolution, which is the property that stops a LOD oscillation.
        [[nodiscard]] u32 SelectCoatLodStep(const CoatLodPolicy& policy, f32 pixelSize) noexcept;
        [[nodiscard]] u32 CoatLodResolution(const CoatLodPolicy& policy, u32 lodStep) noexcept;

        // Hysteresis: the LOD step to USE given the one currently in force and
        // the one the policy asks for. A step change is only taken when the
        // request has been stable, which is what stops a coat straddling a LOD
        // boundary from rebuilding its representation every frame — the
        // "uncontrolled flicker" acceptance criterion names exactly this.
        //
        // `framesStable` is how many consecutive frames `requested` has been
        // asked for; `threshold` how many are needed. A request that is CLOSER
        // (a lower step) is taken immediately, because a coat that just got
        // bigger and is still at the coarse LOD is visibly wrong, while one
        // that stays at the fine LOD a few frames too long is merely expensive.
        [[nodiscard]] u32 ApplyCoatLodHysteresis(u32 current, u32 requested, u32 framesStable,
                                                 u32 threshold) noexcept;

        // ── The comparison ──────────────────────────────────────────────────

        // One (point, direction) the comparison is evaluated at.
        struct CoatProbe
        {
            glm::vec3 Position{ 0.0f };
            glm::vec3 Direction{ 0.0f, 1.0f, 0.0f };
        };

        // Error of a candidate against the reference, in TRANSMITTANCE rather
        // than in optical depth.
        //
        // Transmittance is what the shader multiplies into the radiance, so it
        // is what an error is visible in; an optical-depth error of 0.5 is
        // invisible at tau = 20 and is the whole picture at tau = 0.1. Judging
        // in tau would score a candidate by how well it models a regime the eye
        // cannot see.
        struct CoatShadowError
        {
            /// Mean |T_candidate - T_reference| over the probes.
            f64 MeanAbs = 0.0;
            f64 Rmse = 0.0;
            f64 MaxAbs = 0.0;
            /// SIGNED mean. Separates a candidate that is noisy-but-unbiased
            /// from one that systematically under-shadows, which MeanAbs
            /// cannot. A positive bias means the candidate is BRIGHTER than the
            /// truth — the failure that reads as "the coat has no depth".
            f64 Bias = 0.0;
            /// Probes the statistics were computed over.
            u32 Probes = 0;
            /// Mean work per query, in the candidate's own unit (march steps
            /// for a volume, texture fetches for a map). Not a time; a count.
            f64 MeanSamples = 0.0;
        };

        [[nodiscard]] CoatShadowError CompareCoatShadow(std::span<const f32> candidate, std::span<const f32> reference,
                                                        f64 meanSamples);

        // A deterministic probe set INSIDE the coat: points on actual strands,
        // at a stride, each with one direction. Points are taken from the
        // geometry rather than from the bounding box because most of a coat's
        // bounding box is air, and a probe set drawn from the box would measure
        // mostly tau = 0, where every candidate agrees.
        [[nodiscard]] u32 BuildCoatProbes(std::span<const CoatSegment> segments, const glm::vec3& lightDirection,
                                          u32 maxProbes, u32 seed, std::vector<CoatProbe>& outProbes);

        // Transmittance at every probe, for one mode. `kappa` is the
        // per-crossing extinction. Returns an empty vector if the mode's
        // representation is not valid.
        [[nodiscard]] std::vector<f32> EvaluateCoatShadowMode(CoatShadowMode mode, const DensityVolume& volume,
                                                              const DeepOpacityMap& map,
                                                              std::span<const CoatProbe> probes, f32 kappa,
                                                              f64* outMeanSamples = nullptr);

        // Transmittance at every probe, from the ground truth.
        //
        // `grid` is optional and changes only the COST. Pass one for anything
        // larger than a toy: without it this is probes x rays x segments, which
        // on a 140k-segment coat at 96 rays over 600 probes is 8e9 intersection
        // tests and does not finish inside a test run.
        [[nodiscard]] std::vector<f32> EvaluateCoatShadowReference(std::span<const CoatSegment> segments,
                                                                   std::span<const CoatProbe> probes, f32 kappa,
                                                                   const ReferenceSettings& settings,
                                                                   const CoatSegmentGrid* grid = nullptr);
    } // namespace GroomCoatShadow
} // namespace OloEngine
