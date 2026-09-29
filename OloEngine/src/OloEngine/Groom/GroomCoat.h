#pragma once

// =============================================================================
// GroomCoat.h — coat groups and texture-driven coat variation. Issue #1251.
//
// A cooked groom (#1232) already carries GROUPS: a curve's u16 group id and a
// group name table, made contiguous by the cook. What it did not carry is what
// a group IS — whether those curves are the dense short undercoat that holds
// the warmth, the sparse long guard hairs that make the silhouette, or the few
// stiff whiskers on the muzzle. Without that, every knob a coat has is global,
// and turning the density down thins the guard hairs and the undercoat by the
// same factor: the coat loses its outline and stays just as fuzzy. That is
// acceptance criterion 4's "uniformly fuzzy coat", stated as a mechanism.
//
// So this file adds two things and nothing else:
//
//   * A ROLE PER GROUP, plus the per-group density / length / width / clump /
//     tint the role is authored with — and, since coat authoring v4 (#1533),
//     a tip tint, a curl, a wave and a simulation stiffness scale. This lives
//     in the ASSET and is cooked (section 9 of the .ologroom), because it is a
//     property of how the groom was groomed, not of the entity wearing it.
//     Criterion 1's "preserving density and silhouette during cooking" is
//     exactly the requirement that this survives GroomCooker::Canonicalize —
//     which reorders curves — with every group's share of the strand budget
//     intact.
//
//   * A BOUNDED RUNTIME OVERRIDE, plus two root-UV maps and a deterministic
//     per-strand variation. This lives on the COMPONENT and reaches the build
//     through GroomStrandRequest, because it is a per-entity authoring lever
//     and because criterion 3 asks for it to be editable in the editor.
//
// EVERYTHING HERE IS KEYED ON THE ROOT UV OR ON THE CURVE INDEX, and both are
// invariant under body deformation: the root UV is where the strand grows on
// the pelt's parameterisation, and the curve index is its identity in the
// cooked asset. Nothing is keyed on a world position, a bone or a frame. That
// is the whole of criterion 2's "remain attached under body deformation" — not
// a feature that had to be built, but a consequence of the key, and
// GroomCoatAuthoringTest pins it by evaluating the same coat against two
// different poses and demanding identical answers.
//
// WHAT IS DELIBERATELY NOT HERE. Wetness, mud and interactive grooming are the
// issue's stated later extensions. Guide-driven interpolation is #1250. The
// fibre BCSDF is #1247's and is not touched: a per-strand tint multiplies the
// shaded radiance at the very end of GroomStrand.glsl rather than modulating
// sigma_a, which is an approximation and is named as one at the point it is
// applied.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Math/Math.h"

#include <glm/glm.hpp>

#include <bit>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // The role a coat group plays
    // -------------------------------------------------------------------------
    // Stored as a u8 in the cooked file. APPEND, NEVER RENUMBER — the number is
    // on disk in every .ologroom written since format version 2.
    //
    // `Unassigned` is 0 and is the value a groom imported without role
    // information gets, so an old asset re-imported by a new importer keeps
    // identity behaviour rather than silently becoming an undercoat.
    enum class GroomCoatRole : u8
    {
        Unassigned = 0,
        // The dense, short, fine inner coat. Most of the strand COUNT and
        // almost none of the silhouette.
        Undercoat = 1,
        // The sparse, long, coarse outer hairs. Almost none of the count and
        // most of the silhouette — which is why they are the last thing the
        // strand budget is allowed to thin.
        GuardHair = 2,
        // Vibrissae: a handful of very long, very stiff hairs on the muzzle and
        // brow. A separate role rather than long guard hairs because they must
        // never be decimated at all: losing three of twelve whiskers is
        // immediately readable as damage.
        Whisker = 3,
        // Mane, tail plume, feathering — long hair that is neither structural
        // guard coat nor whisker.
        LongHair = 4,

        Count
    };

    constexpr u32 GroomCoatRoleCount = static_cast<u32>(GroomCoatRole::Count);

    [[nodiscard]] constexpr std::string_view ToString(GroomCoatRole role) noexcept
    {
        switch (role)
        {
            case GroomCoatRole::Unassigned:
                return "Unassigned";
            case GroomCoatRole::Undercoat:
                return "Undercoat";
            case GroomCoatRole::GuardHair:
                return "GuardHair";
            case GroomCoatRole::Whisker:
                return "Whisker";
            case GroomCoatRole::LongHair:
                return "LongHair";
            case GroomCoatRole::Count:
                break;
        }
        return "Unassigned";
    }

    [[nodiscard]] inline constexpr bool IsValidGroomCoatRole(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomCoatRole::Count);
    }

    // The role implied by a group's NAME, for a source that carries no explicit
    // `groom_role` attribute — which is every groom exported by a DCC that does
    // not know about this engine, i.e. all of them.
    //
    // Name matching is a HEURISTIC and is treated as one: it is applied once, at
    // import, and the answer is then written into the asset as data. Nothing at
    // runtime ever re-reads a group name to decide how to shade it, so a
    // renamed group cannot change the look of a cooked coat.
    [[nodiscard]] GroomCoatRole InferGroomCoatRole(std::string_view groupName) noexcept;

    // -------------------------------------------------------------------------
    // Per-group authored coat parameters — cooked into the .ologroom
    // -------------------------------------------------------------------------
    // Bounds are REJECTION-then-clamp: a value read from disk that is not finite
    // or is outside the documented range is replaced by the default and the
    // reason is named, because a NaN density silently removes a whole group and
    // a NaN length turns a coat into a cloud of infinities at the first cast to
    // an integer.
    namespace GroomCoatLimits
    {
        // Density is the fraction of the group's strands a full-budget build
        // keeps. Zero is legal and means "authored, currently hidden"; the
        // group still exists and still round-trips.
        constexpr f32 MinDensity = 0.0f;
        constexpr f32 MaxDensity = 1.0f;

        // Length and width are MULTIPLIERS on the cooked geometry. The upper
        // bounds are authoring bounds, not format bounds: a 16x length on a
        // 3 cm fur is half a metre, which is past any coat.
        constexpr f32 MinLength = 0.01f;
        constexpr f32 MaxLength = 16.0f;
        constexpr f32 MinWidth = 0.01f;
        constexpr f32 MaxWidth = 16.0f;

        // Clump in [0,1] is the fraction of the way a strand's TIP is pulled
        // toward its clump's mean growth direction. 1.0 is a perfect tuft.
        constexpr f32 MinClump = 0.0f;
        constexpr f32 MaxClump = 1.0f;

        // A tint is a multiplier on the shaded colour, so above 1 it is a gain.
        constexpr f32 MinTint = 0.0f;
        constexpr f32 MaxTint = 4.0f;

        // The root-UV cell edge a clump is formed over. Below a texel of a
        // 4096-map every strand is its own clump (no clumping at all); above a
        // quarter of the pelt the whole animal is one tuft.
        constexpr f32 MinClumpCellSize = 1.0f / 4096.0f;
        constexpr f32 MaxClumpCellSize = 0.25f;

        // Jitter amplitudes, as a fraction of the value they perturb.
        constexpr f32 MaxJitter = 1.0f;

        // ── Coat authoring v4 (#1533) ─────────────────────────────────────
        //
        // Curl: a HELIX about the strand's own centreline. The radius is in
        // METRES and the frequency in turns per metre of REST arc length,
        // measured from the root along the shaped strand. Five centimetres is
        // past any ringlet; 400 turns a metre is a 2.5 mm pitch, past the
        // tightest kink a coat has.
        constexpr f32 MinCurlRadius = 0.0f;
        constexpr f32 MaxCurlRadius = 0.05f;
        constexpr f32 MinCurlFrequency = 0.0f;
        constexpr f32 MaxCurlFrequency = 400.0f;

        // Wave: a PLANAR sine across the strand, amplitude in metres and
        // frequency in periods per metre of rest arc length.
        constexpr f32 MinWaveAmplitude = 0.0f;
        constexpr f32 MaxWaveAmplitude = 0.05f;
        constexpr f32 MinWaveFrequency = 0.0f;
        constexpr f32 MaxWaveFrequency = 200.0f;

        // The factor on the entity's guide-simulation stiffness for a group's
        // guides: a twentieth to twenty times, a limp tail plume to a stiff
        // whisker either side of an undercoat. Strictly positive — a group
        // that should not hold its groom at all has no guides.
        constexpr f32 MinStiffnessScale = 0.05f;
        constexpr f32 MaxStiffnessScale = 20.0f;
    } // namespace GroomCoatLimits

    /// The shape of the curl and the wave (#1533), stated once for the build,
    /// the evaluation and the tests. See ApplyGroomCoatCurl for the formula.
    namespace GroomCoatCurl
    {
        /// Per-strand jitter on an authored AMPLITUDE (curl radius, wave
        /// amplitude): each strand draws a factor in [0.8, 1.2]. Without it a
        /// curled coat is a field of identical corkscrews.
        constexpr f32 AmplitudeJitter = 0.20f;

        /// Per-strand jitter on an authored FREQUENCY: a factor in [0.85, 1.15].
        constexpr f32 FrequencyJitter = 0.15f;

        /// The strand parameter at which the offset reaches full amplitude. The
        /// envelope is smoothstep(0, EnvelopeEnd, t): ZERO at the root, so the
        /// root is a fixed point, and with zero slope there, so the strand
        /// leaves the skin along its groomed direction and winds up over its
        /// first quarter instead of kinking at the follicle.
        constexpr f32 EnvelopeEnd = 0.25f;

        /// The control points a strand needs per turn for its curl to read as a
        /// curl rather than an aliased zigzag. The build NEVER subdivides — a
        /// strand of n points is n - 1 segments, curled or not — so this is a
        /// number for the GROWER: a 3 cm strand at 60 turns a metre is 1.8 turns
        /// and wants 15 or more points.
        constexpr u32 MinPointsPerTurn = 8;
    } // namespace GroomCoatCurl

    /// One coat group's authored parameters. Cooked; see GroomBinaryFormat.h
    /// section 9.
    ///
    /// Explicitly padded and size-asserted because it goes to disk as a block of
    /// bytes and because Math::BitwiseEqual below compares the whole object —
    /// an implicit padding byte would make two identical descriptions compare
    /// unequal and rebuild the strand geometry every frame.
    ///
    /// Every field added by coat authoring v4 (#1533) defaults to "no change":
    /// a group that authors none of them builds exactly the strands, and packs
    /// exactly the tints, it built at format version 3.
    struct GroomCoatGroupDesc
    {
        /// Multiplier on the shaded colour at the strand's ROOT. White is
        /// identity.
        glm::vec3 Tint{ 1.0f, 1.0f, 1.0f };

        /// A further multiplier at the strand's TIP (#1533): the tip's colour
        /// is Tint * TipTint, and each ribbon corner takes mix(Tint, Tint *
        /// TipTint, t), so a strand can run from a dark root to a sun-bleached
        /// tip. RELATIVE to Tint, and white by default, so every group that
        /// authors Tint alone -- every coat written before v4, every importer --
        /// stays one colour root to tip. An absolute tip colour defaulting to
        /// white would fade every such group to white at its tips.
        glm::vec3 TipTint{ 1.0f, 1.0f, 1.0f };

        /// Fraction of this group's strands a full-budget build keeps.
        f32 Density = 1.0f;
        /// Multiplier on the cooked strand LENGTH, applied from the root.
        f32 Length = 1.0f;
        /// Multiplier on the cooked strand DIAMETERS.
        f32 Width = 1.0f;
        /// How strongly this group tufts. 0 is the cooked layout.
        f32 Clump = 0.0f;

        /// Curl (#1533): the helix radius in metres, and turns per metre of rest
        /// arc length. Either at zero is the straight coat.
        f32 CurlRadius = 0.0f;
        f32 CurlFrequency = 0.0f;

        /// Wave (#1533): a planar sine's amplitude in metres, and periods per
        /// metre of rest arc length. Either at zero is no wave.
        f32 WaveAmplitude = 0.0f;
        f32 WaveFrequency = 0.0f;

        /// Factor on the entity's guide-simulation stiffness for this group's
        /// guides (#1533): a tail plume can hang where the undercoat holds. 1 is
        /// the entity's own stiffness.
        f32 StiffnessScale = 1.0f;

        u8 Role = static_cast<u8>(GroomCoatRole::Unassigned);
        u8 Pad0 = 0;
        u8 Pad1 = 0;
        u8 Pad2 = 0;

        [[nodiscard]] auto operator==(const GroomCoatGroupDesc& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }

        [[nodiscard]] GroomCoatRole GetRole() const noexcept
        {
            return IsValidGroomCoatRole(static_cast<i32>(Role)) ? static_cast<GroomCoatRole>(Role)
                                                                : GroomCoatRole::Unassigned;
        }
    };

    static_assert(sizeof(GroomCoatGroupDesc) == 64,
                  "GroomCoatGroupDesc goes to disk as bytes and is compared with a whole-object memcmp: "
                  "it must have no implicit padding");
    static_assert(std::is_trivially_copyable_v<GroomCoatGroupDesc>);

    /// Replaces every non-finite or out-of-range field with its default and
    /// appends a one-line reason per repair to `outReasons`. Returns true when
    /// nothing had to be repaired.
    ///
    /// Separate from the reader so the SAME sanitisation runs on a value from a
    /// cooked file, from the importer and from an MCP write — this repo's
    /// "three of them agree and the fourth drifts" failure, which GroomAsset.h
    /// already names for the limit constants.
    bool SanitizeGroomCoatGroupDesc(GroomCoatGroupDesc& desc, u32 groupIndex, std::vector<std::string>& outReasons);

    /// The per-group description a groom has for `groupIndex`, or the identity
    /// description when the groom predates roles or the index is out of range.
    /// Free function rather than a GroomAsset method so the evaluation below can
    /// be tested against a hand-built table with no GroomAsset at all.
    [[nodiscard]] GroomCoatGroupDesc DefaultGroomCoatGroupDesc() noexcept;

    // -------------------------------------------------------------------------
    // A root-UV map
    // -------------------------------------------------------------------------
    // The "regional map" of criterion 2: an RGB image in the pelt's UV space,
    // sampled ONCE PER STRAND at its root UV, on the CPU, at strand-build time.
    //
    // WHY ON THE CPU AND WHY PER STRAND. A strand's root UV is a per-curve
    // constant, so every texel this modulation needs is fetched once per strand
    // rather than once per fragment — and the values drive DENSITY and LENGTH,
    // which are decisions about whether a strand exists and how long it is.
    // Neither is expressible in a fragment shader at all. Doing it on the GPU
    // would mean a compute prepass to produce the same per-strand constants,
    // for a fetch count three orders of magnitude below the fragment count.
    //
    // WHY ITS OWN TYPE AND NOT Texture2D. Texture2D lives on the GPU and its
    // GetData() is a readback that needs a live context, which would make every
    // CPU-only test of this file a GPU test. This is a plain CPU image with a
    // content hash, built from a readback once by the editor/scene path and
    // built from synthetic pixels by the tests.
    class GroomRegionMap : public RefCounted
    {
      public:
        /// `rgba` is tightly packed 8-bit RGBA, `width * height * 4` bytes, row
        /// 0 at v == 0. Returns null when the dimensions are zero or the span is
        /// the wrong size — a half-read map would modulate a coat with whatever
        /// was after the buffer.
        [[nodiscard]] static Ref<GroomRegionMap> FromRGBA8(u32 width, u32 height, std::span<const u8> rgba);

        /// `rgb` is tightly packed 8-bit RGB, `width * height * 3` bytes -- what
        /// an RGB8 texture reads back, and therefore what every JPEG albedo
        /// gives. Only R, G and B are ever sampled, so this is the same map as
        /// FromRGBA8 of the same pixels with an opaque alpha. The same refusals
        /// apply: zero dimensions or a wrong-sized span return null.
        [[nodiscard]] static Ref<GroomRegionMap> FromRGB8(u32 width, u32 height, std::span<const u8> rgb);

      private:
        // Takes ownership of already-validated, tightly packed RGBA8 and hashes it.
        [[nodiscard]] static Ref<GroomRegionMap> Adopt(u32 width, u32 height, std::vector<u8>&& rgba);

      public:
        /// Bilinear, with CLAMPED addressing. Clamped rather than wrapped
        /// because a root UV is a position on a pelt's chart, and wrapping puts
        /// the muzzle's modulation on the tail across a seam.
        ///
        /// A non-finite uv answers the map's (0,0) texel rather than indexing
        /// with a NaN; the caller has already validated, and this is the
        /// backstop that keeps a bad value from reaching a cast.
        [[nodiscard]] glm::vec3 Sample(glm::vec2 uv) const noexcept;

        [[nodiscard]] u32 GetWidth() const noexcept
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const noexcept
        {
            return m_Height;
        }
        /// 64-bit FNV-1a over the pixels. The strand-geometry cache key mixes
        /// it, so a map edited in place rebuilds the coat instead of serving the
        /// geometry the previous pixels produced.
        [[nodiscard]] u64 GetContentHash() const noexcept
        {
            return m_ContentHash;
        }

      private:
        u32 m_Width = 0;
        u32 m_Height = 0;
        u64 m_ContentHash = 0;
        std::vector<u8> m_Pixels; // RGBA8, width * height * 4
    };

    // -------------------------------------------------------------------------
    // The bounded runtime override
    // -------------------------------------------------------------------------

    /// Multipliers applied on top of a group's authored description. All four
    /// default to identity, so a GroomCoatComponent added with defaults is a
    /// no-op and the A/B control for every capture is "remove the component".
    struct GroomCoatRoleOverride
    {
        f32 Density = 1.0f;
        f32 Length = 1.0f;
        f32 Width = 1.0f;
        f32 Clump = 1.0f;

        [[nodiscard]] auto operator==(const GroomCoatRoleOverride& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }
    };

    static_assert(std::is_trivially_copyable_v<GroomCoatRoleOverride>);

    /// Everything the strand build needs to know about coat authoring for one
    /// request. Assembled by Scene from GroomCoatComponent and carried in
    /// GroomStrandRequest.
    ///
    /// UNDERCOAT AND GUARD HAIR GET AN OVERRIDE EACH AND THE OTHER ROLES DO NOT.
    /// That is criterion 1 read literally — "undercoat and guard hair groups can
    /// be adjusted independently" — and it is a deliberate bound rather than an
    /// omission: whiskers and mane are authored in the ASSET, where their
    /// parameters are part of the groom, and exposing a runtime density on a
    /// twelve-strand whisker group would offer a knob whose only settings are
    /// "all" and "visibly missing". The visibility MASK still reaches every
    /// role, which is what criterion 3's "group visibility" asks for.
    struct GroomCoatSettings
    {
        /// False leaves every strand exactly as the cook produced it. The whole
        /// of this file is skipped, not merely evaluated to identity.
        bool Enabled = false;

        /// Bit per GroomCoatRole. A clear bit removes that role's strands from
        /// the build entirely — no geometry, no budget spent.
        u32 RoleVisibilityMask = (1u << GroomCoatRoleCount) - 1u;

        GroomCoatRoleOverride Undercoat;
        GroomCoatRoleOverride Guard;

        /// Deterministic per-strand variation, as a fraction of the value. 0.25
        /// on length means each strand is scaled by a value in [0.75, 1.25]
        /// drawn from its own hash.
        f32 LengthJitter = 0.0f;
        f32 WidthJitter = 0.0f;
        /// Multiplies the strand's tint by 1 +/- this. The cheapest defence
        /// against a coat that reads as one flat colour.
        ///
        /// IT DARKENS ON AVERAGE, and that is a property of what a tint IS rather
        /// than a transport limitation to work around. The group's authored tint
        /// is the strand's albedo, so a per-strand perturbation ABOVE it is an
        /// albedo above the authored one — which the [0,1] tint lane clamps and
        /// which nothing physical would produce anyway. On the default white tint
        /// the upper half of the jitter is therefore flat and the lower half is
        /// not, so turning this up makes the coat both more varied and slightly
        /// darker. Author the group tint below white to get headroom in both
        /// directions.
        f32 ShadeJitter = 0.0f;

        /// Root-UV cell edge a clump forms over.
        f32 ClumpCellSize = 0.03f;

        /// Salts every hash. Two animals sharing one groom asset and one set of
        /// parameters look identical without it.
        u32 Seed = 0;

        /// RGB at the root UV: R scales length, G scales density, B scales
        /// clump. Null means all three are 1 — so a coat with no map authored is
        /// the coat the asset describes.
        Ref<GroomRegionMap> RegionMap;
        /// RGB at the root UV, multiplied into the strand tint. This is
        /// criterion 2's "root UV colour pattern".
        Ref<GroomRegionMap> ColorMap;

        /// Whether a role's strands may be built at all.
        [[nodiscard]] bool IsRoleVisible(GroomCoatRole role) const noexcept
        {
            const u32 bit = 1u << static_cast<u32>(role);
            return (RoleVisibilityMask & bit) != 0u;
        }
    };

    // Reaches a TArray via GroomStrandRequest::Coat, so growth relocates it
    // BITWISE. Not trivially copyable only because of the two Ref<> handles,
    // which own separately allocated region maps; nothing points back into
    // the settings. Derived member-by-member so a future field that is not
    // relocatable fails here rather than corrupting a groom build.
    template<>
    struct TIsTriviallyRelocatable<GroomCoatSettings>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::Enabled)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::RoleVisibilityMask)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::Undercoat)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::Guard)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::LengthJitter)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::WidthJitter)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::ShadeJitter)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::ClumpCellSize)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::Seed)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::RegionMap)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomCoatSettings::ColorMap)>;
    };

    /// The per-group table plus the settings, resolved once per build.
    ///
    /// A VIEW, not an owner: `Groups` points at the asset's own table for the
    /// lifetime of one build call. Held as a span so the evaluation can be
    /// tested against a hand-built vector with no asset in sight.
    struct GroomCoatContext
    {
        const GroomCoatSettings* Settings = nullptr;
        std::span<const GroomCoatGroupDesc> Groups{};

        /// Per group: the factor on every per-strand JITTER amplitude. Empty
        /// means 1 for every group, which is every strand-tier build. A card
        /// level fills it (GroomCardTierCoat in GroomStrandMesh.cpp, #1428): a
        /// card stands for N strands, so it carries their MEAN jitter, whose
        /// spread is the per-strand amplitude over sqrt(N).
        std::span<const f32> JitterScales{};

        /// True when there is anything to apply at all.
        [[nodiscard]] bool IsActive() const noexcept
        {
            return Settings != nullptr && Settings->Enabled;
        }

        /// The description for `groupId`, or the identity description when the
        /// groom carries no table (an asset cooked before format version 2 that
        /// was loaded rather than re-imported) or the id is out of range.
        [[nodiscard]] GroomCoatGroupDesc GroupDesc(u16 groupId) const noexcept;

        /// JitterScales[groupId], or 1 when the table is empty or the id is out
        /// of range.
        [[nodiscard]] f32 JitterScale(u16 groupId) const noexcept;
    };

    // -------------------------------------------------------------------------
    // The per-strand answer
    // -------------------------------------------------------------------------

    /// Members ordered 4-byte then 1-byte, with the tail padded explicitly, so
    /// the whole-object comparison below has no implicit padding to read.
    struct GroomCoatStrandParams
    {
        /// Multiplier on the strand's LENGTH, applied from its root.
        f32 Length = 1.0f;
        /// Multiplier on the strand's cooked diameters.
        f32 Width = 1.0f;
        /// How far this strand's tip is pulled toward its clump, in [0,1].
        f32 Clump = 0.0f;
        /// Multiplies the shaded colour at the ROOT: the group's Tint through
        /// the colour map and the shade jitter. White is identity.
        glm::vec3 Tint{ 1.0f, 1.0f, 1.0f };
        /// The same at the TIP (#1533), through the same map and shade. The
        /// corners between take PackGroomCoatTintAt.
        glm::vec3 TipTint{ 1.0f, 1.0f, 1.0f };

        /// This strand's curl and wave (#1533): the group's, jittered by the
        /// strand's own draws and, on a card, scaled by the JitterScale. The
        /// phases and the azimuth are in TURNS, in [0, 1). ApplyGroomCoatCurl
        /// spends them.
        f32 CurlRadius = 0.0f;
        f32 CurlFrequency = 0.0f;
        f32 CurlPhase = 0.0f;
        f32 CurlAzimuth = 0.0f;
        f32 WaveAmplitude = 0.0f;
        f32 WaveFrequency = 0.0f;
        f32 WavePhase = 0.0f;

        /// False means this strand is not built: its role is hidden, or it lost
        /// the density draw. A dropped strand costs no geometry and no budget.
        bool Keep = true;
        u8 Pad0 = 0;
        u8 Pad1 = 0;
        u8 Pad2 = 0;

        [[nodiscard]] auto operator==(const GroomCoatStrandParams& other) const -> bool
        {
            return Math::BitwiseEqual(*this, other);
        }

        /// True when ApplyGroomCoatCurl has anything to do for this strand. A
        /// radius with no frequency, or the reverse, is not a curl.
        [[nodiscard]] bool CurlsOrWaves() const noexcept
        {
            return (CurlRadius > 0.0f && CurlFrequency > 0.0f) || (WaveAmplitude > 0.0f && WaveFrequency > 0.0f);
        }
    };

    static_assert(sizeof(GroomCoatStrandParams) == 68,
                  "GroomCoatStrandParams is compared with a whole-object memcmp: it must have no implicit padding");

    /// The identity answer: what every strand gets when no coat is authored.
    [[nodiscard]] GroomCoatStrandParams IdentityGroomCoatStrandParams() noexcept;

    /// Evaluates one strand's coat parameters.
    ///
    /// PURE, AND KEYED ONLY ON `curveIndex`, `rootUV` AND `groupId`. Nothing
    /// about the entity, the pose, the frame or the camera reaches it — which is
    /// what makes criterion 2 true by construction rather than by care, and is
    /// what GroomCoatAuthoringTest asserts by calling it twice across a pose
    /// change.
    [[nodiscard]] GroomCoatStrandParams EvaluateGroomCoatStrand(const GroomCoatContext& coat, u32 curveIndex,
                                                                glm::vec2 rootUV, u16 groupId) noexcept;

    // -------------------------------------------------------------------------
    // The deterministic hash
    // -------------------------------------------------------------------------

    /// A value in [0, 1) from an integer key and a salt.
    ///
    /// EVERY OPERATION IS A 32-BIT UNSIGNED ONE whose wraparound is defined, and
    /// the result is a 24-bit integer scaled by an exact power of two — the same
    /// construction, and the same final mixer, as GroomCoverage::StochasticHash,
    /// for the same reason: the value must be identical on every platform and in
    /// every build, or a cooked coat looks different on the artist's machine and
    /// in the capture. The two are deliberately SEPARATE functions rather than
    /// one shared call: the stochastic alpha test's hash is a per-pixel,
    /// per-frame estimator that #1246 may retune, and this one is a per-strand
    /// constant that must never change again once a coat is authored against it.
    [[nodiscard]] constexpr f32 GroomCoatHash01(u32 key, u32 salt) noexcept
    {
        u32 h = key * 0x9E3779B1u;
        h ^= salt * 0x85EBCA6Bu;
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        h ^= h >> 16;
        return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
    }

    /// Salts, named so the same strand's length, width, shade and density draws
    /// are four independent values rather than four uses of one.
    namespace GroomCoatSalt
    {
        constexpr u32 Density = 0x1251'0001u;
        constexpr u32 Length = 0x1251'0002u;
        constexpr u32 Width = 0x1251'0003u;
        constexpr u32 Shade = 0x1251'0004u;

        // Coat authoring v4 (#1533). NEW salts, never a reuse of one above: a
        // curl radius drawn on the length salt would make every long strand a
        // loose curl and every short one a tight one, which is a correlation
        // the eye picks out at once.
        constexpr u32 CurlRadius = 0x1533'0001u;
        constexpr u32 CurlFrequency = 0x1533'0002u;
        constexpr u32 CurlPhase = 0x1533'0003u;
        constexpr u32 CurlAzimuth = 0x1533'0004u;
        constexpr u32 WaveAmplitude = 0x1533'0005u;
        constexpr u32 WaveFrequency = 0x1533'0006u;
        constexpr u32 WavePhase = 0x1533'0007u;
    } // namespace GroomCoatSalt

    // -------------------------------------------------------------------------
    // Tint transport
    // -------------------------------------------------------------------------
    // The coat tint rides in the strand vertex's one spare float lane
    // (GroomStrandVertex::Tint, formerly Pad0) as 8:8:8 in the low 24 bits —
    // one value per ribbon CORNER since #1533, so a strand can run from its
    // root tint to its tip tint (PackGroomCoatTintAt).
    //
    // THE TOP BYTE IS FORCED TO 0x3F AND THAT IS LOAD-BEARING. A bare 24-bit
    // payload bit-cast to a float is a DENORMAL for every tint whose blue
    // channel is below 0x80 — and a vertex pipeline is allowed to flush
    // denormals to zero. A saturated red tint (0x0000C8) would then arrive as
    // 0.0 and the strand would render black, on some drivers and not others,
    // with nothing in any log. Forcing the exponent byte to 0x3F puts every
    // possible payload in [0.5, 2): always normal, never NaN or infinity, and
    // the shader masks the exponent off again. GroomStrand.glsl's unpack is the
    // twin of this.
    //
    // THE PAYLOAD IS 24 BITS AND THE MANTISSA IS 23, so the blue channel's top
    // bit lands in the exponent's LOW bit and moves the value between exponent
    // 126 and 127 — which is why the range is [0.5, 2) and not [0.5, 1). That is
    // harmless and deliberate: both exponents are normal, the unpack masks the
    // whole top byte off again, and no arithmetic is ever done on the lane as a
    // float. It is stated because "[0.5, 1)" is the number a reader would
    // otherwise write down, and a test asserting it would fail on every tint
    // whose blue channel is 0x80 or above.
    [[nodiscard]] constexpr f32 PackGroomCoatTint(const glm::vec3& tint) noexcept
    {
        const auto channel = [](f32 v) -> u32
        {
            // Not std::clamp: a NaN must not survive the cast, and
            // std::clamp(NaN, 0, 1) is NaN. The explicit comparisons make NaN
            // take the `else` and land on 0.
            if (v >= 1.0f)
            {
                return 255u;
            }
            if (v > 0.0f)
            {
                return static_cast<u32>((v * 255.0f) + 0.5f);
            }
            return 0u;
        };
        // The tint range is [0, MaxTint]; the 8-bit lane carries [0, 1], so a
        // gain above 1 is clamped HERE rather than pretending the transport can
        // express it. Authoring above 1 is legal on the group description
        // because it multiplies the shaded colour on other paths too (the
        // preview), and the clamp is what keeps the shipped picture honest.
        // .z/.y/.x, NOT .b/.g/.r. glm's component names are members of a UNION,
        // and reading one through a name other than the active member is not a
        // constant expression -- so the swizzle spelling is what decides whether
        // GroomCoatIdentityTint below compiles at all.
        const u32 packed = 0x3F000000u | (channel(tint.z) << 16) | (channel(tint.y) << 8) | channel(tint.x);
        return std::bit_cast<f32>(packed);
    }

    [[nodiscard]] constexpr glm::vec3 UnpackGroomCoatTint(f32 packed) noexcept
    {
        const u32 bits = std::bit_cast<u32>(packed) & 0x00FFFFFFu;
        return glm::vec3(static_cast<f32>(bits & 0xFFu), static_cast<f32>((bits >> 8) & 0xFFu),
                         static_cast<f32>((bits >> 16) & 0xFFu)) *
               (1.0f / 255.0f);
    }

    /// The packed lane for "no tint". Named so the vertex's default initialiser
    /// and the build agree on one constant.
    constexpr f32 GroomCoatIdentityTint = PackGroomCoatTint(glm::vec3(1.0f));

    /// The packed tint of a ribbon corner at strand parameter `t` (#1533):
    /// pack(ColorMap(rootUV) * shade * mix(Tint, Tint * TipTint, t)), with the
    /// map and the shade already inside `params.Tint` and `params.TipTint` (the
    /// strand's TipTint is the tip's absolute value; the group's is relative).
    ///
    /// Written as the root plus a delta and NOT as glm::mix, which is
    /// x * (1 - t) + y * t and is not exact when x == y. The delta form is: a
    /// strand whose tip tint equals its root tint — every coat that authors no
    /// gradient — packs, at every corner, exactly the one value format version 3
    /// packed for the whole strand.
    [[nodiscard]] inline f32 PackGroomCoatTintAt(const GroomCoatStrandParams& params, f32 t) noexcept
    {
        return PackGroomCoatTint(params.Tint + ((params.TipTint - params.Tint) * t));
    }

    // -------------------------------------------------------------------------
    // Clumping
    // -------------------------------------------------------------------------

    /// A 64-bit digest of everything in `settings` that changes the geometry a
    /// build produces — including the CONTENT HASH of both maps.
    ///
    /// It exists because the strand-geometry cache is keyed on the build
    /// settings and compared against them, and a Ref<GroomRegionMap> can be
    /// neither hashed nor usefully compared: two different maps loaded from one
    /// file are two pointers. The digest goes into GroomStrandBuildSettings, so
    /// the cache key, the collision guard and the rebuild trigger are all the
    /// existing ones rather than a second mechanism that can disagree with them.
    ///
    /// A map EDITED IN PLACE changes its content hash, so the coat rebuilds —
    /// which is the case a pointer-identity key gets wrong and nobody notices
    /// until an artist asks why the density map does nothing.
    [[nodiscard]] u64 GroomCoatDigest(const GroomCoatSettings& settings) noexcept;

    /// The role priority the strand BUDGET spends itself in, as a weight.
    ///
    /// This is criterion 1's "preserving density and silhouette" as a number.
    /// When the budget cannot afford every strand, the retained fraction of a
    /// role is proportional to its weight (saturated at 1), so an undercoat
    /// thins long before the guard hairs that draw the outline, and whiskers are
    /// effectively never thinned at all. A single global stride — what the build
    /// did before this issue — removes the same fraction of every role, so the
    /// first thing a tight budget takes is the silhouette.
    [[nodiscard]] constexpr f32 GroomCoatBudgetWeight(GroomCoatRole role) noexcept
    {
        switch (role)
        {
            case GroomCoatRole::Whisker:
                // A whisker group is a dozen strands against a million. Even a
                // brutal budget can afford all of them, and missing three of
                // twelve reads as damage rather than as distance.
                return 64.0f;
            case GroomCoatRole::GuardHair:
                return 8.0f;
            case GroomCoatRole::LongHair:
                return 4.0f;
            case GroomCoatRole::Unassigned:
                // A groom with no roles authored has every curve here, so this
                // weight is what makes the un-authored case behave EXACTLY as it
                // did before roles existed: one weight, one stride.
                return 1.0f;
            case GroomCoatRole::Undercoat:
                return 1.0f;
            case GroomCoatRole::Count:
                break;
        }
        return 1.0f;
    }

    /// The clump cell a root UV falls in, as one 64-bit key.
    ///
    /// Quantised in UV rather than in world space, so a clump is a patch of the
    /// PELT and moves with it — the same reason everything else here keys on the
    /// root UV. A world-space cell would make a tuft break apart the moment the
    /// animal bent, which is the visible failure criterion 2 is about.
    [[nodiscard]] u64 GroomCoatClumpCell(glm::vec2 rootUV, f32 cellSize) noexcept;

    /// One clump's accumulated geometry. The clump's growth vector is the mean
    /// over its member strands of (tip - root); a strand is pulled toward its
    /// own root plus that vector, so the roots never move and the tips converge.
    struct GroomCoatClumpAccum
    {
        glm::vec3 GrowthSum{ 0.0f };
        u32 Count = 0;

        [[nodiscard]] glm::vec3 MeanGrowth() const noexcept
        {
            return Count > 0u ? GrowthSum / static_cast<f32>(Count) : glm::vec3(0.0f);
        }
    };

    /// Where point `restPoint` at strand parameter `t` ends up once the strand's
    /// length scale and its clump pull are applied.
    ///
    /// `root` is the strand's own first control point; `clumpGrowth` is its
    /// clump's mean (tip - root). At t == 0 this returns `root` EXACTLY — both
    /// terms vanish — which is what keeps a clumped coat attached to the body
    /// rather than sliding off it.
    [[nodiscard]] glm::vec3 ApplyGroomCoatShape(const glm::vec3& root, const glm::vec3& restPoint, f32 t,
                                                f32 lengthScale, f32 clump, const glm::vec3& clumpGrowth) noexcept;

    /// Adds a strand's curl and wave to its SHAPED REST polyline, in place
    /// (#1533).
    ///
    /// `points` is the whole strand, root first, already through
    /// ApplyGroomCoatShape. Point i sits at strand parameter t = i / (n - 1) —
    /// the parameter the shape, the tint and the guide sample all use — and at
    /// arc length s from the root along `points`. It moves by
    ///
    ///   envelope(t) * [ R * (cos(2 pi (f s + phi)) N + sin(2 pi (f s + phi)) B)
    ///                 + A * sin(2 pi (fw s + phiW)) N ]
    ///
    /// with R, f, phi the strand's CurlRadius, CurlFrequency and CurlPhase, A,
    /// fw, phiW its wave's, and envelope(t) = smoothstep(0,
    /// GroomCoatCurl::EnvelopeEnd, t). (N, B) is a PARALLEL-TRANSPORT
    /// (rotation-minimising) frame of the polyline, carried point to point by
    /// double reflection (Wang, Juttler, Zheng and Liu, 2008) from a normal
    /// perpendicular to the root tangent at CurlAzimuth turns around it.
    /// Transported rather than Frenet: a Frenet frame is undefined along a
    /// straight run and flips at every inflection, and each flip would kink the
    /// helix.
    ///
    /// So the curl is a helix of radius R and pitch 1/f about the strand's own
    /// centreline, and the wave a sine along N — one direction, on a straight
    /// strand, which is what "planar" means here.
    ///
    /// THE ROOT IS NEVER WRITTEN. The envelope is zero there, and adding a zero
    /// offset would still turn a -0.0 coordinate positive; the loop starts at
    /// point 1 instead, so the root is a fixed point bit for bit and the coat
    /// cannot slide off the body at its follicles.
    ///
    /// A no-op when the strand neither curls nor waves (CurlsOrWaves), when any
    /// of its curl parameters is not finite, or when every point coincides with
    /// the root and there is no direction to curl about. It never adds a point:
    /// the curl is only as fine as the polyline the grower authored — see
    /// GroomCoatCurl::MinPointsPerTurn.
    void ApplyGroomCoatCurl(std::span<glm::vec3> points, const GroomCoatStrandParams& params) noexcept;
} // namespace OloEngine
