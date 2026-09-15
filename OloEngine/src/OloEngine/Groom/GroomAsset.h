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

#include <glm/glm.hpp>

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
        std::vector<GroomGroupRange> m_GroupRanges; // derived

        glm::vec3 m_BoundsMin{ 0.0f };
        glm::vec3 m_BoundsMax{ 0.0f };
        u32 m_GuideCount = 0;
        GroomCurveBasis m_Basis = GroomCurveBasis::Linear;

        GroomProvenance m_Provenance;
    };
} // namespace OloEngine
