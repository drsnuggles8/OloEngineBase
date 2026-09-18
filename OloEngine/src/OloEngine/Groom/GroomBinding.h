#pragma once

// =============================================================================
// GroomBinding.h — where a groom's roots live on a body surface. Issue #1249.
//
// A groom is authored in the object space of the body it grows on, and that is
// the only place it is ever correct: the moment the body bends or expresses,
// the curves have to go with it. A BINDING is the record that says, for every
// curve, WHICH triangle of the body that curve grows out of and WHERE on it —
// so that a deformed triangle can carry its strands.
//
// WHAT A BINDING IS, PRECISELY. Per curve: a triangle index, a barycentric
// coordinate on that triangle, and the ORTHONORMAL FRAME the triangle had at
// bind time (origin + rotation). Nothing else. The strand's own points stay in
// the groom asset and are never copied here — a binding for a 200k-strand
// groom is 56 bytes a strand, not a second copy of the coat.
//
// WHY THE REST FRAME IS STORED RATHER THAN RECOMPUTED. The obvious design
// recomputes the bind-pose frame from the target mesh whenever it is needed,
// and it is wrong here for one concrete reason: morph targets are applied on
// the CPU straight into MeshSource's vertex array (MorphTargetSystem), so the
// "rest" positions a runtime consumer can read are the morphed ones. A binding
// that derived its reference frame from them would drift with every expression
// — the coat would slide across the face and nothing would say so. Storing the
// frame makes the binding self-contained: the runtime needs the DEFORMED
// triangle and nothing else.
//
// WHY A RIGID FRAME PER ROOT AND NOT PER-POINT SKINNING. The issue's scope is
// "deformation and interpolation, BEFORE physical simulation" (#1250 owns the
// simulation). A rigid root transfer — every point of a curve carried by its
// root's frame — is the deformation that cannot collapse a coat: it preserves
// every strand's length and shape exactly, whatever the body does. Per-point
// skinning against the body would pinch long strands into the surface wherever
// the nearest bone disagreed with the root's, which is the "gross coat
// collapse" criterion 2 names.
//
// TOPOLOGY AND VERSION CHECKS ARE THE POINT, NOT A GUARD. A binding addresses
// triangles by INDEX, so a target whose topology changed silently rebinds every
// root to a different part of the body — a plausible-looking wrong coat, which
// is the worst failure mode this feature has. So GroomBindingTargetSignature is
// carried in the asset, checked on every attach, and a mismatch REFUSES. There
// is deliberately no nearest-point re-snap fallback: re-binding is a build-time
// operation with a deterministic result (GroomBindingBuilder), and doing it
// implicitly at runtime would hide exactly the authoring error it was asked to
// report.
// =============================================================================

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace OloEngine
{
    namespace GroomBindingLimits
    {
        // Mirrors GroomLimits::MaxCurveCount — a binding has exactly one record
        // per curve, so it can never legitimately be larger than the groom it
        // binds. Restated rather than aliased so the format cap and the asset
        // cap are checkable against each other (static_assert in the
        // serializer) instead of silently moving together.
        constexpr u32 MaxRootCount = 8u * 1024u * 1024u;

        // A target mesh past this is not a character; it is an import accident.
        // The cap exists so a corrupt count in a header cannot size a lookup.
        constexpr u32 MaxTargetTriangleCount = 64u * 1024u * 1024u;

        // Barycentric coordinates are a partition of unity. This is the slack
        // allowed on their sum before the record is called corrupt — generous
        // enough for the f32 round trip through the cooked file, far tighter
        // than any authoring error.
        constexpr f32 BarycentricSumTolerance = 1.0e-3f;

        // Object-space coordinate bound, the same one GroomLimits uses. A frame
        // origin past it is a unit-scale accident rather than a body.
        constexpr f32 MaxCoordinate = 1.0e7f;
    } // namespace GroomBindingLimits

    /**
     * @brief How a root that could not be placed on the surface was handled.
     *
     * Every root gets a record, including the ones the projection could not
     * place well, because a binding with holes in it would make the record
     * array stop being parallel to the curve array — and every consumer indexes
     * it by curve. What varies is the QUALITY flag, which is carried per root
     * and counted in the asset, so "12 of 40 000 roots were placed further than
     * the search radius" is a number the editor can show rather than something
     * a coat's appearance has to imply.
     */
    enum class GroomRootBindQuality : u8
    {
        /// The root projected into its triangle's interior, within the radius.
        Exact = 0,
        /// The closest point was on a triangle EDGE or vertex — the root sits
        /// outside the triangle's interior. Normal for a coat authored slightly
        /// off the surface; the frame is still the triangle's.
        Clamped = 1,
        /// The closest surface point was further away than the search radius.
        /// Still bound (to the closest triangle found), still deformed, but
        /// counted separately: a scalp groom bound to the wrong body reads as a
        /// large Distant count rather than as a coat that merely looks odd.
        Distant = 2,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GroomRootBindQuality quality)
    {
        switch (quality)
        {
            case GroomRootBindQuality::Exact:
                return "Exact";
            case GroomRootBindQuality::Clamped:
                return "Clamped";
            case GroomRootBindQuality::Distant:
                return "Distant";
            case GroomRootBindQuality::Count:
                break;
        }
        return "Exact";
    }

    [[nodiscard]] inline constexpr bool IsValidGroomRootBindQuality(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomRootBindQuality::Count);
    }

    /**
     * @brief One curve's attachment to the body, in 56 bytes.
     *
     * `Barycentric` is over the triangle's three corners in index order, so the
     * deformed surface point is `b.x*v0 + b.y*v1 + b.z*v2` with the DEFORMED
     * corners — there is no second convention to get wrong.
     *
     * `RestOrigin` / `RestRotation` are the bind-time frame: the surface point
     * and the orthonormal basis built from the triangle there (see
     * MakeGroomSurfaceFrame, which is the ONE place a frame is constructed, so
     * the bind-time and runtime frames cannot disagree about handedness or
     * which edge is the tangent).
     *
     * A root's offset from the surface is NOT stored: it is
     * `conjugate(RestRotation) * (curveRoot - RestOrigin)`, derived from the
     * groom the binding was built against. Storing it would be a second copy of
     * a fact the groom already holds, and the two could then disagree after a
     * re-cook.
     */
    struct GroomRootBinding
    {
        /// Index of the triangle in the TARGET's index buffer, i.e. its indices
        /// are [3*TriangleIndex, 3*TriangleIndex + 3).
        u32 TriangleIndex = 0;

        /// GroomRootBindQuality, widened to keep the struct hole-free.
        u32 Quality = 0;

        glm::vec3 Barycentric{ 1.0f, 0.0f, 0.0f };

        /// Distance from the curve's root to `RestOrigin` at bind time, in
        /// object-space units. Redundant with the derived offset and kept
        /// anyway, because it is the number the editor reports and the number a
        /// "this groom is bound to the wrong body" diagnosis is made from — and
        /// deriving it needs the groom, which a binding inspector may not hold.
        f32 RestDistance = 0.0f;

        glm::vec3 RestOrigin{ 0.0f };
        f32 Pad0 = 0.0f;

        /// The bind-time orthonormal frame: x = tangent, y = bitangent,
        /// z = surface normal. A quaternion rather than a mat3 because it is
        /// 16 bytes instead of 36 and because the runtime composes it with the
        /// deformed frame, which is a rotation composition either way.
        glm::quat RestRotation{ 1.0f, 0.0f, 0.0f, 0.0f };

        [[nodiscard]] bool operator==(const GroomRootBinding&) const = default;
    };

    static_assert(sizeof(GroomRootBinding) == 56,
                  "GroomRootBinding is written to disk as a flat array; a size change is a format change");
    static_assert(std::is_trivially_copyable_v<GroomRootBinding>,
                  "GroomRootBinding is written and read as a block of bytes");

    /**
     * @brief The identity of the surface a binding was built against.
     *
     * The contract: two of these match exactly when a binding built for one may
     * be attached to the other. What is in it, and why each field:
     *
     *   VertexCount / IndexCount — a binding indexes both arrays, so a count
     *     change is a hard mismatch and the cheapest one to detect.
     *
     *   IndexHash — the CONNECTIVITY. Two meshes with identical counts and
     *     different triangles are different surfaces, and re-indexing is exactly
     *     what a mesh optimizer does. This is the check that catches an LOD
     *     level or a re-exported body, and it is MORPH-INVARIANT, which is what
     *     lets it run on every attach (see RestPositionHash).
     *
     *   BoneCount / SkeletonNameHash — the deformation input. A body re-rigged
     *     against a different skeleton deforms differently through the same
     *     triangles, and the palette the runtime reads is indexed by bone. Zero
     *     on a non-skinned (morph-only or static) target, which is a legitimate
     *     binding target.
     *
     *   RestPositionHash — the bind-pose GEOMETRY. Deliberately NOT part of the
     *     per-attach check, and this is the one subtle field here. Morph targets
     *     are applied on the CPU into MeshSource's vertex array, so the
     *     positions a runtime consumer can read are the morphed ones and would
     *     mismatch the instant a face expressed. It is therefore a BUILD-TIME
     *     check (GroomBindingBuilder records it; the editor's validate action
     *     compares it) and never a runtime gate whose verdict would depend on
     *     which frame it was asked on.
     */
    struct GroomBindingTargetSignature
    {
        u64 IndexHash = 0;
        u64 RestPositionHash = 0;
        u64 SkeletonNameHash = 0;
        u32 VertexCount = 0;
        u32 IndexCount = 0;
        u32 BoneCount = 0;
        u32 Pad0 = 0;

        /// The per-attach check: everything a morphing, animating target keeps
        /// constant. RestPositionHash is excluded on purpose — see above.
        [[nodiscard]] bool MatchesTopology(const GroomBindingTargetSignature& other) const noexcept
        {
            return VertexCount == other.VertexCount && IndexCount == other.IndexCount &&
                   IndexHash == other.IndexHash && BoneCount == other.BoneCount &&
                   SkeletonNameHash == other.SkeletonNameHash;
        }

        [[nodiscard]] bool operator==(const GroomBindingTargetSignature&) const = default;
    };

    /**
     * @brief The identity of the groom a binding was built against.
     *
     * `RootHash` is over the curve ROOT positions and root UVs only, not the
     * whole coat: a re-cook that changed a strand's tip is still the same set of
     * roots and the binding is still correct, while a re-cook that moved the
     * roots is a different groom and must be rebuilt. Hashing every point would
     * refuse the first case for no reason, which is how a correct guard gets
     * disabled by whoever hits it on a Friday.
     */
    struct GroomBindingSourceSignature
    {
        u64 RootHash = 0;
        u32 CurveCount = 0;
        u32 GuideCount = 0;

        [[nodiscard]] bool operator==(const GroomBindingSourceSignature&) const = default;
    };

    /// Bumped whenever GroomBindingBuilder's OUTPUT changes for an unchanged
    /// input — a different projection, a different frame convention, a
    /// different tie-break. A binding built by an older builder is refused with
    /// "rebuild it", not migrated: the build is deterministic and cheap, so a
    /// migration path would be a permanent maintenance cost incurred to avoid
    /// one re-cook. This is NOT the file-format version, which lives in
    /// Serialization/GroomBindingBinaryFormat.h and moves for a different
    /// reason.
    constexpr u32 kGroomBinderVersion = 1;

    /**
     * @brief Why an attach was refused, as a value rather than a log line.
     *
     * Ordered most-fundamental first, in GroomCompositionFallbackReason's style:
     * a binding that trips an earlier row would trip later ones too, so the
     * FIRST match is what is reported and the counter is unambiguous.
     */
    enum class GroomBindingRejectReason : u32
    {
        /// Not a rejection: the binding attached.
        None = 0,
        /// The groom asked to be bound but no binding asset resolved.
        NoBinding,
        /// The named target entity does not exist, or carries no mesh source.
        NoTarget,
        /// The target mesh has no vertices or no triangles yet (still loading).
        TargetNotReady,
        /// The binding was produced by a different builder — rebuild it.
        BinderVersionMismatch,
        /// The binding holds a different number of roots than the groom has
        /// curves: a re-cooked groom, or a binding paired with the wrong one.
        RootCountMismatch,
        /// The groom's roots moved since the binding was built.
        SourceSignatureMismatch,
        /// The target's topology is not the one the binding was built against.
        TargetTopologyMismatch,
        /// A record addresses a triangle the target does not have. Only
        /// reachable on a file that passed validation against a target that then
        /// changed, so it is reported separately from the signature mismatch
        /// that should have caught it first.
        TriangleOutOfRange,

        Count
    };

    [[nodiscard]] std::string_view ToString(GroomBindingRejectReason reason);

    /// The sentence a user can act on, for the log and the editor panel.
    [[nodiscard]] std::string_view DescribeGroomBindingReject(GroomBindingRejectReason reason);

    [[nodiscard]] inline constexpr bool IsValidGroomBindingRejectReason(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomBindingRejectReason::Count);
    }

    /**
     * @brief A cooked groom binding: one root record per curve, plus identity.
     *
     * Object space of the TARGET mesh throughout — the groom and the body are
     * authored in the same space, which is what makes a barycentric placement
     * meaningful without a transform anywhere in this file.
     */
    class GroomBindingAsset : public Asset
    {
      public:
        GroomBindingAsset() = default;
        ~GroomBindingAsset() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::GroomBinding;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        [[nodiscard]] u32 GetRootCount() const noexcept
        {
            return static_cast<u32>(m_Roots.size());
        }
        [[nodiscard]] const std::vector<GroomRootBinding>& GetRoots() const noexcept
        {
            return m_Roots;
        }
        /// Callers must have checked curveIndex < GetRootCount().
        [[nodiscard]] const GroomRootBinding& GetRoot(u32 curveIndex) const noexcept
        {
            return m_Roots[curveIndex];
        }

        [[nodiscard]] const GroomBindingTargetSignature& GetTargetSignature() const noexcept
        {
            return m_Target;
        }
        [[nodiscard]] const GroomBindingSourceSignature& GetSourceSignature() const noexcept
        {
            return m_Source;
        }
        [[nodiscard]] u32 GetBinderVersion() const noexcept
        {
            return m_BinderVersion;
        }

        /// Counts by GroomRootBindQuality, parallel to the enumerators. Derived,
        /// never serialized — see RecomputeDerivedData.
        [[nodiscard]] u32 GetQualityCount(GroomRootBindQuality quality) const noexcept
        {
            return m_QualityCounts[static_cast<sizet>(quality)];
        }
        /// The largest RestDistance in the binding, object-space units. The one
        /// number that says "this groom is not sitting on this body".
        [[nodiscard]] f32 GetMaxRestDistance() const noexcept
        {
            return m_MaxRestDistance;
        }

        [[nodiscard]] const std::string& GetName() const noexcept
        {
            return m_Name;
        }
        void SetName(std::string name)
        {
            m_Name = std::move(name);
        }

        /// Project-relative paths of the pair this binding was built from, so a
        /// binding found on disk can name both halves and the editor's rebuild
        /// action has somewhere to start. Carries no timestamp and no absolute
        /// path, for the reason GroomProvenance gives.
        [[nodiscard]] const std::string& GetGroomSourcePath() const noexcept
        {
            return m_GroomSourcePath;
        }
        [[nodiscard]] const std::string& GetTargetSourcePath() const noexcept
        {
            return m_TargetSourcePath;
        }

        [[nodiscard]] u64 GetCpuMemoryBytes() const noexcept;

        /// Recomputes the quality counts and the maximum rest distance from the
        /// root array. Cheap and idempotent; the build and the decode call it.
        void RecomputeDerivedData();

        /// Structural self-check — the same predicate the writer refuses on and
        /// the reader validates with, so a file this process would reject can
        /// never be a file this process wrote. On failure `outReason` names
        /// WHICH invariant broke and for which root.
        [[nodiscard]] bool Validate(std::string& outReason) const;

        /// Whether this binding may be attached to a groom and a target with the
        /// given identities. Returns the FIRST reason it may not, so the caller
        /// reports one actionable sentence rather than a list.
        [[nodiscard]] GroomBindingRejectReason CheckCompatibility(
            const GroomBindingSourceSignature& groom, const GroomBindingTargetSignature& target) const noexcept;

      private:
        // Only the builder and the serializer may populate the arrays; every
        // other path is read-only, so a half-built binding is not a state a
        // consumer can observe. The same rule GroomAsset states.
        friend class GroomBindingBuilder;
        friend class GroomBindingSerializer;

        std::string m_Name;
        std::string m_GroomSourcePath;
        std::string m_TargetSourcePath;

        std::vector<GroomRootBinding> m_Roots;
        GroomBindingTargetSignature m_Target;
        GroomBindingSourceSignature m_Source;
        u32 m_BinderVersion = kGroomBinderVersion;

        // Derived.
        u32 m_QualityCounts[static_cast<sizet>(GroomRootBindQuality::Count)] = { 0, 0, 0 };
        f32 m_MaxRestDistance = 0.0f;
    };
} // namespace OloEngine
