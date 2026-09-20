#pragma once

// =============================================================================
// GroomAsset.h — the imported, cooked hair/fur groom. Issue #1232.
//
// A groom is a flat set of CURVES, not a mesh. Every curve is an open polyline
// of control points running ROOT -> TIP, carrying a per-point width, a per-curve
// root UV (where on the scalp/pelt it grows), a group, and a guide flag. That is
// the whole data model; hair SHADING (#1246/#1247) consumes it and is explicitly
// out of scope here.
//
// WHY A DEDICATED ASSET AND NOT A MESHSOURCE. The engine already imports
// Alembic, but only the IPolyMesh / ISubD schemas — see AlembicMeshImporter.
// Polygon Alembic import is NOT curve support: a groom exported as ribbons loses
// the per-strand identity (the offset table), the root parameterisation (root UV)
// and the guide designation that every downstream groom feature is defined in
// terms of. Storing curves as a mesh would also mean re-deriving strand topology
// from triangle connectivity in every consumer. So: its own asset, its own cooked
// format (.ologroom, see Serialization/GroomBinaryFormat.h), its own importer.
//
// WHY THE LAYOUT IS STRUCTURE-OF-ARRAYS WITH AN OFFSET TABLE. `m_CurveOffsets`
// is a prefix-sum table of length CurveCount + 1, so curve i owns the point
// range [m_CurveOffsets[i], m_CurveOffsets[i + 1]). This is the layout a GPU
// strand expansion wants directly (one buffer, one offset lookup) and it makes
// "the curves of group g" a contiguous range after cooking — see GroomCooker.h.
// =============================================================================

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomLod.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // The documented curve input convention (acceptance criterion 1)
    // -------------------------------------------------------------------------
    // These limits are REJECTION bounds, not taste bounds. A groom outside them
    // is either authoring corruption or a file the cooked format cannot address,
    // and the importer says so by name rather than clamping silently. They live
    // here so the importer, the cooker, the reader and the editor all reject on
    // the same numbers instead of three of them agreeing and the fourth drifting.
    namespace GroomLimits
    {
        // A curve needs at least a root and a tip to have a direction at all.
        constexpr u32 MinPointsPerCurve = 2;
        // 4096 control points on ONE strand is already orders of magnitude past
        // a production groom (a long-coat strand is 8-32).
        constexpr u32 MaxPointsPerCurve = 4096;

        // 8M strands is roughly 4x the densest shipping human groom. The cap
        // exists so a corrupt count in a header cannot size an allocation.
        constexpr u32 MaxCurveCount = 8u * 1024u * 1024u;
        constexpr u64 MaxPointCount = 256ull * 1024ull * 1024ull;

        // Group ids are stored as u16 on disk, so this is a format bound.
        constexpr u32 MaxGroupCount = 65535;
        constexpr u32 MaxGroupNameLength = 512;
        constexpr u32 MaxSourcePathLength = 4096;

        // A width is a DIAMETER in object-space units (the Alembic/USD `widths`
        // convention). Zero is legal (a strand tapering to nothing); negative
        // is not.
        constexpr f32 MaxWidth = 1.0e4f;

        // Object-space coordinate bound. Anything past it is a unit-scale
        // accident, not a groom, and would poison the bounds.
        constexpr f32 MaxCoordinate = 1.0e7f;
    } // namespace GroomLimits

    // The per-curve bit flags stored on disk. Append, never renumber.
    enum class GroomCurveFlag : u8
    {
        None = 0,
        // A GUIDE curve: authored by hand and used to drive interpolated
        // strands, rather than necessarily being rendered itself. The
        // designation is authored upstream and must survive the round trip
        // untouched — #1246 onwards is defined in terms of it.
        Guide = OloBit8(0),
    };

    [[nodiscard]] inline constexpr bool HasGroomFlag(u8 flags, GroomCurveFlag flag) noexcept
    {
        return (flags & std::to_underlying(flag)) != 0;
    }

    // The curve basis a groom was authored in. Append, never renumber — the
    // number is on disk in every .ologroom.
    enum class GroomCurveBasis : u8
    {
        // Control points ARE the strand: consecutive points are joined by
        // segments. What every DCC groom exporter emits by default.
        Linear = 0,
        // Uniform cubic B-spline control points. Kept as authored — the cook
        // deliberately does NOT tessellate, because the sample count a strand
        // needs is a RENDERING decision (#1246) and baking one here would fix
        // it forever at the wrong end of the pipeline. Recording the basis is
        // what lets that later choice be made instead of guessed.
        BSpline = 1,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GroomCurveBasis basis)
    {
        switch (basis)
        {
            case GroomCurveBasis::Linear:
                return "Linear";
            case GroomCurveBasis::BSpline:
                return "BSpline";
            case GroomCurveBasis::Count:
                break;
        }
        return "Linear";
    }

    [[nodiscard]] inline constexpr bool IsValidGroomCurveBasis(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomCurveBasis::Count);
    }

    // -------------------------------------------------------------------------
    // Provenance (acceptance criterion 4)
    // -------------------------------------------------------------------------
    // Where the cooked groom came from, carried inside the cooked file so a
    // .ologroom found on disk can name its own source. Deliberately carries NO
    // timestamp and no absolute path: both would make the cooked bytes differ
    // between two machines cooking the same input, and determinism is the
    // contract (see GroomCooker.h). SourcePath is project-relative when the
    // importer is given a project root, and the file NAME otherwise.
    struct GroomProvenance
    {
        std::string SourcePath;    // e.g. "grooms/human-scalp.abc"
        std::string SourceFormat;  // e.g. "AlembicCurves"
        u64 SourceContentHash = 0; // 64-bit FNV-1a of the source file's bytes
        u32 ImporterVersion = 0;   // bumped when the importer's output changes
        u32 Pad0 = 0;

        [[nodiscard]] bool operator==(const GroomProvenance&) const = default;
    };

    // A contiguous run of curves sharing a group. Produced by the cook, which
    // sorts curves so that every group IS a contiguous range — the preview and
    // any future per-group shading then need no per-curve group scan.
    struct GroomGroupRange
    {
        u32 FirstCurve = 0;
        u32 CurveCount = 0;

        [[nodiscard]] bool operator==(const GroomGroupRange&) const = default;
    };

    // -------------------------------------------------------------------------
    // A non-owning view over one curve set
    // -------------------------------------------------------------------------
    // Issue #1252. The strand build used to take a `const GroomAsset&` because
    // there was only ever one curve set to build from. A cooked LOD level
    // (GroomLodLevel below) is a SECOND curve set in the same layout, and the
    // whole value of the card tier is that it goes through the SAME build, the
    // same shader and the same lighting — so the build's parameter became the
    // thing both of them are rather than one of them.
    //
    // A VIEW OF SPANS, NOT A BASE CLASS. The build is a pure function of curve
    // arrays; making GroomLodLevel derive from GroomAsset would give a LOD level
    // an asset handle, a registry identity and a serializer it has no use for,
    // and would let one be handed to AssetManager. Spans also make a synthetic
    // curve set in a test one aggregate initialiser rather than a built asset.
    //
    // Every accessor mirrors GroomAsset's by NAME, so the build reads the same
    // either way and a reviewer diffing the refactor sees a substitution rather
    // than a rewrite.
    struct GroomCurveView
    {
        std::span<const u32> CurveOffsets;  // CurveCount + 1, non-decreasing
        std::span<const glm::vec3> Points;  // PointCount
        std::span<const f32> PointWidths;   // PointCount — DIAMETERS
        std::span<const glm::vec2> RootUVs; // CurveCount
        std::span<const u16> CurveGroupIds; // CurveCount
        std::span<const u8> CurveFlags;     // CurveCount

        [[nodiscard]] u32 GetCurveCount() const noexcept
        {
            return CurveOffsets.empty() ? 0u : static_cast<u32>(CurveOffsets.size() - 1);
        }
        [[nodiscard]] u32 GetPointCount() const noexcept
        {
            return static_cast<u32>(Points.size());
        }
        [[nodiscard]] u32 GetCurveFirstPoint(u32 curveIndex) const noexcept
        {
            return CurveOffsets[curveIndex];
        }
        [[nodiscard]] u32 GetCurvePointCount(u32 curveIndex) const noexcept
        {
            return CurveOffsets[curveIndex + 1] - CurveOffsets[curveIndex];
        }
        [[nodiscard]] const std::span<const glm::vec3>& GetPoints() const noexcept
        {
            return Points;
        }
        [[nodiscard]] const std::span<const f32>& GetPointWidths() const noexcept
        {
            return PointWidths;
        }
        [[nodiscard]] const std::span<const glm::vec2>& GetRootUVs() const noexcept
        {
            return RootUVs;
        }
        [[nodiscard]] const std::span<const u16>& GetCurveGroupIds() const noexcept
        {
            return CurveGroupIds;
        }
        [[nodiscard]] bool IsGuide(u32 curveIndex) const noexcept
        {
            return HasGroomFlag(CurveFlags[curveIndex], GroomCurveFlag::Guide);
        }

        /// True when every array has the length the offset table implies and the
        /// table is a usable prefix sum. A view that fails this is REFUSED by
        /// the build rather than indexed — a short array here is an out-of-
        /// bounds read in the innermost loop of the renderer.
        [[nodiscard]] bool IsConsistent() const noexcept;
    };

    // -------------------------------------------------------------------------
    // One cooked LOD level
    // -------------------------------------------------------------------------
    // Issue #1252, section 10 of the cooked format. A coarser curve set standing
    // in for the base groom past a distance, plus the map back to the curves it
    // stands in for.
    //
    // WHY THE SOURCE MAP IS NOT OPTIONAL. A card follows its clump's MEAN
    // centreline, so it is not any one strand — but the binding's rest frame
    // (#1249) and the guide influence table (#1250) are both indexed by BASE
    // curve. Without the map a coat at card range would stop following the body
    // and stop moving, which is criterion 3's "missing coats" showing up as a
    // silent detachment at exactly the distance nobody is looking closely. The
    // map names, for each card, the member strand whose root frame and guides
    // the card borrows.
    struct GroomLodLevel
    {
        std::vector<u32> CurveOffsets;  // CurveCount + 1
        std::vector<glm::vec3> Points;  // PointCount
        std::vector<f32> PointWidths;   // PointCount — DIAMETERS, as the base
        std::vector<glm::vec2> RootUVs; // CurveCount
        std::vector<u16> CurveGroupIds; // CurveCount
        std::vector<u8> CurveFlags;     // CurveCount

        /// Level curve -> BASE curve. CurveCount entries, each < the base
        /// groom's curve count. Validated on cook AND on load: a corrupt entry
        /// here indexes the root-transform array out of bounds.
        std::vector<u32> SourceCurves;

        /// Which tier this level serves.
        GroomRepresentation Representation = GroomRepresentation::Card;

        /// The apparent size, in pixels of frame height, the cook AIMED this
        /// level at. Advisory and recorded rather than enforced: the runtime
        /// policy (GroomLodPolicy) decides where the hand-over happens, and a
        /// level cooked for 256 px being selected at 200 px is a tuning choice,
        /// not a mismatch. It is carried so the editor can say what the level
        /// was built for beside what it is being used for.
        f32 SourcePixelSize = 0.0f;

        [[nodiscard]] u32 GetCurveCount() const noexcept
        {
            return CurveOffsets.empty() ? 0u : static_cast<u32>(CurveOffsets.size() - 1);
        }

        [[nodiscard]] GroomCurveView GetCurveView() const noexcept
        {
            return GroomCurveView{ CurveOffsets, Points, PointWidths, RootUVs, CurveGroupIds, CurveFlags };
        }

        /// Approximate resident CPU size, for the memory readout criterion 4
        /// asks for by representation.
        [[nodiscard]] u64 GetCpuMemoryBytes() const noexcept;

        /// Structural self-check. `baseCurveCount` bounds SourceCurves and
        /// `groupCount` bounds CurveGroupIds; pass the base groom's counts.
        /// On failure `outReason` names the invariant.
        ///
        /// THE GROUP BOUND IS NOT COSMETIC. A card's group id is what
        /// GroomCoatContext::GroupDesc looks the coat description up by, and an
        /// out-of-range id there does not throw — it answers IDENTITY. So a
        /// corrupt id costs that card its role, its density and its budget
        /// weight, and the only symptom is a coat that is slightly too uniform
        /// at range. The base groom's ids are bounded the same way, in
        /// GroomAsset::Validate; a level's were not until CodeRabbit asked.
        [[nodiscard]] bool Validate(u32 baseCurveCount, u32 groupCount, std::string& outReason) const;

        /// Field by field: a defaulted operator== would compare
        /// SourcePixelSize — and every element of the three float arrays — with
        /// a float `==`, which cpp-coding-quality §2a forbids. The integer
        /// arrays compare elementwise; the float ones compare as BYTES, which
        /// is what a cache-invalidation or round-trip check actually wants
        /// (two grooms differing by one ulp are two cooked artifacts).
        [[nodiscard]] bool operator==(const GroomLodLevel& other) const;
    };

    class GroomAsset;

    // Forward-declared so GroomAsset can friend exactly the one cook entry
    // point that is allowed to reorder its arrays, rather than opening them up
    // to the whole namespace. Defined in GroomCooker.h/.cpp.
    namespace GroomCooker
    {
        [[nodiscard]] bool Canonicalize(GroomAsset& groom, std::string& outReason);
        [[nodiscard]] bool CookToBytes(const GroomAsset& groom, std::vector<u8>& outBytes, std::string& outReason);
    } // namespace GroomCooker

    /**
     * @brief An imported groom: curves with root UVs, widths, groups and guides.
     *
     * Object space, engine units — the importer bakes the source's transform and
     * unit scale in, exactly as AlembicMeshImporter does for meshes, so a
     * GroomAsset never needs its source file's conventions again.
     */
    class GroomAsset : public Asset
    {
      public:
        GroomAsset() = default;
        ~GroomAsset() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::Groom;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // ── Topology ────────────────────────────────────────────────────────
        [[nodiscard]] u32 GetCurveCount() const noexcept
        {
            // CurveOffsets is a prefix table of size CurveCount + 1; an empty
            // table means an empty groom rather than an underflow.
            return m_CurveOffsets.empty() ? 0u : static_cast<u32>(m_CurveOffsets.size() - 1);
        }
        [[nodiscard]] u32 GetPointCount() const noexcept
        {
            return static_cast<u32>(m_Points.size());
        }
        [[nodiscard]] const std::vector<u32>& GetCurveOffsets() const noexcept
        {
            return m_CurveOffsets;
        }

        // First / count of the point range of `curveIndex`. Callers must have
        // checked curveIndex < GetCurveCount().
        [[nodiscard]] u32 GetCurveFirstPoint(u32 curveIndex) const noexcept
        {
            return m_CurveOffsets[curveIndex];
        }
        [[nodiscard]] u32 GetCurvePointCount(u32 curveIndex) const noexcept
        {
            return m_CurveOffsets[curveIndex + 1] - m_CurveOffsets[curveIndex];
        }

        // ── Per-point data ──────────────────────────────────────────────────
        [[nodiscard]] const std::vector<glm::vec3>& GetPoints() const noexcept
        {
            return m_Points;
        }
        // Per-point DIAMETER in object-space units, parallel to GetPoints().
        [[nodiscard]] const std::vector<f32>& GetPointWidths() const noexcept
        {
            return m_PointWidths;
        }

        // ── Per-curve data ──────────────────────────────────────────────────
        // Where on the surface's UV parameterisation the strand grows from.
        [[nodiscard]] const std::vector<glm::vec2>& GetRootUVs() const noexcept
        {
            return m_RootUVs;
        }
        [[nodiscard]] const std::vector<u16>& GetCurveGroupIds() const noexcept
        {
            return m_CurveGroupIds;
        }
        [[nodiscard]] const std::vector<u8>& GetCurveFlags() const noexcept
        {
            return m_CurveFlags;
        }
        [[nodiscard]] bool IsGuide(u32 curveIndex) const noexcept
        {
            return HasGroomFlag(m_CurveFlags[curveIndex], GroomCurveFlag::Guide);
        }
        [[nodiscard]] u32 GetGuideCount() const noexcept
        {
            return m_GuideCount;
        }

        // ── Groups ──────────────────────────────────────────────────────────
        [[nodiscard]] const std::vector<std::string>& GetGroupNames() const noexcept
        {
            return m_GroupNames;
        }
        // Parallel to GetGroupNames(). Derived, never serialized.
        [[nodiscard]] const std::vector<GroomGroupRange>& GetGroupRanges() const noexcept
        {
            return m_GroupRanges;
        }
        [[nodiscard]] u32 GetGroupCount() const noexcept
        {
            return static_cast<u32>(m_GroupNames.size());
        }

        // ── Coat authoring (issue #1251) ────────────────────────────
        //
        // Parallel to GetGroupNames(), and SERIALIZED — section 9 of the cooked
        // format. This is what makes a coat group a first-class authored thing
        // rather than a name: the role (undercoat, guard hair, whisker, long
        // hair) and the density, length, width, clump and tint it was groomed
        // with. See GroomCoat.h.
        //
        // EMPTY IS LEGAL AND MEANS "IDENTITY", not "no groups": a .ologroom
        // written before format version 2 cannot be read at all (the format is a
        // derived artifact and moves its minimum version with its current one),
        // but a groom built in memory by a test or by a tool that does not care
        // about coats has an empty table and every consumer must treat that as
        // the description GroomCoat.h calls identity. GroomCoatContext::GroupDesc
        // is the one place that decision is made.
        [[nodiscard]] const std::vector<GroomCoatGroupDesc>& GetGroupCoats() const noexcept
        {
            return m_GroupCoats;
        }

        // The description for `groupId`, identity when the table is empty or the
        // id is out of range.
        [[nodiscard]] GroomCoatGroupDesc GetGroupCoat(u16 groupId) const noexcept
        {
            const auto index = static_cast<sizet>(groupId);
            return index < m_GroupCoats.size() ? m_GroupCoats[index] : GroomCoatGroupDesc{};
        }

        // ── LOD levels (issue #1252) ────────────────────────────────
        //
        // Cooked, SERIALIZED (section 10), and EMPTY IS LEGAL: a groom cooked
        // with no coarser representation simply never leaves the strand tier,
        // which is reported as GroomLodFallbackReason::LevelNotCooked rather
        // than as a missing coat. Every level's curves are in the same object
        // space and the same units as the base groom's, so a level and the base
        // are interchangeable inputs to the strand build.
        [[nodiscard]] const std::vector<GroomLodLevel>& GetLodLevels() const noexcept
        {
            return m_LodLevels;
        }

        /// The level serving `representation`, or null. Linear over a table of
        /// at most a handful of entries, which is cheaper than any index and
        /// cannot go stale when a level is added.
        [[nodiscard]] const GroomLodLevel* FindLodLevel(GroomRepresentation representation) const noexcept;

        /// A view over the BASE curve set — what the strand tier builds from,
        /// and what every consumer that predates #1252 was already using.
        [[nodiscard]] GroomCurveView GetCurveView() const noexcept
        {
            return GroomCurveView{ m_CurveOffsets, m_Points, m_PointWidths,
                                   m_RootUVs, m_CurveGroupIds, m_CurveFlags };
        }

        // ── Bounds / basis / provenance ─────────────────────────────────────
        [[nodiscard]] const glm::vec3& GetBoundsMin() const noexcept
        {
            return m_BoundsMin;
        }
        [[nodiscard]] const glm::vec3& GetBoundsMax() const noexcept
        {
            return m_BoundsMax;
        }
        [[nodiscard]] GroomCurveBasis GetBasis() const noexcept
        {
            return m_Basis;
        }
        [[nodiscard]] const GroomProvenance& GetProvenance() const noexcept
        {
            return m_Provenance;
        }

        [[nodiscard]] const std::string& GetName() const noexcept
        {
            return m_Name;
        }
        void SetName(std::string name)
        {
            m_Name = std::move(name);
        }

        // Approximate resident CPU size, for the editor's memory readout and
        // the issue's memory measurement.
        [[nodiscard]] u64 GetCpuMemoryBytes() const noexcept;

        // Recomputes m_BoundsMin/Max, m_GuideCount and m_GroupRanges from the
        // curve arrays. Cheap and idempotent; the cook calls it last.
        void RecomputeDerivedData();

        // Structural self-check against GroomLimits — the same predicate the
        // writer refuses on and the reader validates with, so a file this
        // process would reject can never be a file this process wrote. On
        // failure `outReason` names WHICH invariant broke.
        [[nodiscard]] bool Validate(std::string& outReason) const;

      private:
        // Only the builder and the serializer may populate the arrays; every
        // other path is read-only, so a half-built groom is not a state a
        // consumer can observe.
        friend class GroomBuilder;
        friend class GroomSerializer;
        // Issue #1252: the only path allowed to attach cooked LOD levels, for the
        // reason every other writer here is a friend rather than a setter — a
        // half-attached level is not a state a consumer can observe.
        friend class GroomLodBuilder;
        friend bool GroomCooker::Canonicalize(GroomAsset& groom, std::string& outReason);
        friend bool GroomCooker::CookToBytes(const GroomAsset& groom, std::vector<u8>& outBytes, std::string& outReason);

        std::string m_Name;

        std::vector<u32> m_CurveOffsets;  // CurveCount + 1 entries, non-decreasing
        std::vector<glm::vec3> m_Points;  // PointCount
        std::vector<f32> m_PointWidths;   // PointCount — diameters
        std::vector<glm::vec2> m_RootUVs; // CurveCount
        std::vector<u16> m_CurveGroupIds; // CurveCount
        std::vector<u8> m_CurveFlags;     // CurveCount — GroomCurveFlag bits

        std::vector<std::string> m_GroupNames;
        // Parallel to m_GroupNames, or EMPTY. Not derived: authored, cooked and
        // read back. Kept as a separate array rather than a field on a group
        // struct so the name table's on-disk section (7) is unchanged and the
        // coat table is its own appended section.
        std::vector<GroomCoatGroupDesc> m_GroupCoats;
        std::vector<GroomGroupRange> m_GroupRanges; // derived

        // Cooked coarser representations (#1252). Not derived: built by
        // GroomLodBuilder at cook time and read back from section 10, so the
        // runtime never has to cluster a million strands on a load.
        std::vector<GroomLodLevel> m_LodLevels;

        glm::vec3 m_BoundsMin{ 0.0f };
        glm::vec3 m_BoundsMax{ 0.0f };
        u32 m_GuideCount = 0;
        GroomCurveBasis m_Basis = GroomCurveBasis::Linear;

        GroomProvenance m_Provenance;
    };
} // namespace OloEngine
