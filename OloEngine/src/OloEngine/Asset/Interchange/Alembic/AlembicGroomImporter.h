#pragma once

// The Alembic groom importer only exists when the OLO_WITH_ALEMBIC CMake option
// compiled the Alembic + Imath dependency in. When it's off this header is empty
// and nothing references the class, so the engine builds with zero Alembic
// footprint — and note that READING a cooked .ologroom never needs Alembic at
// all (GroomSerializer is pure binary decode), exactly the cook/runtime split
// .olovol uses for OpenVDB.
#if defined(OLO_WITH_ALEMBIC)

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomAsset.h"

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine
{
    // ============================================================================
    // The documented Alembic curve input convention (issue #1232, AC 1)
    // ============================================================================
    //
    // READ THIS BEFORE ASSUMING AN .abc WILL IMPORT. Polygon Alembic import
    // (AlembicMeshImporter, IPolyMesh / ISubD) is NOT curve support and never
    // routes here; a groom must be authored as Alembic's `ICurves` schema.
    //
    // WHAT IS READ, per ICurves prim, at sample 0:
    //
    //   P                      control points, ROOT FIRST. Transformed by the
    //                          accumulated IXform chain into object space.
    //   nVertices              per-curve control point counts. Their sum must
    //                          equal P's size exactly.
    //   type                   kLinear -> GroomCurveBasis::Linear
    //                          kCubic  -> GroomCurveBasis::BSpline
    //                          kVariableOrder is REJECTED: per-curve order has
    //                          no representation in the cooked format, and
    //                          silently collapsing it to one basis would change
    //                          the strand shape.
    //   wrap                   kNonPeriodic only. A periodic curve is a loop,
    //                          which is not a hair strand; REJECTED.
    //   widths                 IFloatGeomParam, DIAMETERS in source units.
    //                          kVertexScope -> one per control point.
    //                          kUniformScope -> one per curve, broadcast.
    //                          kConstantScope -> one for the prim, broadcast.
    //                          Absent -> every width is kDefaultWidth below,
    //                          and the import LOGS that it substituted.
    //                          Any other scope is REJECTED.
    //   uvs                    IV2fGeomParam, the ROOT UV.
    //                          kUniformScope -> one per curve, taken as-is.
    //                          kVertexScope -> the value at the curve's ROOT
    //                          control point is taken; the rest are ignored
    //                          (and that is logged, because discarding data
    //                          silently is the failure this convention exists
    //                          to avoid).
    //                          Absent -> (0,0) for every curve, logged.
    //                          Any other scope is REJECTED.
    //
    // ARBITRARY GEOMETRY PARAMETERS. Exactly two are understood, both
    // IInt32GeomParam with kUniformScope (one value per curve):
    //
    //   groom_guide            non-zero marks the curve as a GUIDE.
    //   groom_group            a sub-group index within the prim. The curve's
    //                          group becomes "<prim path>#<index>".
    //   groom_role             a GroomCoatRole (issue #1251): 0 Unassigned,
    //                          1 Undercoat, 2 GuardHair, 3 Whisker, 4 LongHair.
    //                          Authored PER CURVE because Alembic has no
    //                          per-group scope, but it IS a per-group property:
    //                          a group whose curves disagree is rejected by
    //                          name, and an out-of-range value is rejected too.
    //                          Absent -> every group keeps the role inferred
    //                          from its NAME (InferGroomCoatRole), which is what
    //                          a groom exported by a DCC that knows nothing of
    //                          this engine gets.
    //
    // Any OTHER arbGeomParam whose name begins with `groom_` is REJECTED by
    // name: it is groom semantics this build does not implement, and importing
    // the file without it would produce a groom that is quietly missing
    // authored intent. Non-`groom_` params (a DCC's own bookkeeping) are
    // ignored and listed at TRACE level.
    //
    // GROUPS. Absent `groom_group`, one ICurves prim is one group, named by its
    // full Alembic path. Group ids are assigned in ARCHIVE TRAVERSAL ORDER,
    // which is deterministic, and the cook then sorts curves so each group is
    // a contiguous range.
    //
    // UNITS AND HANDEDNESS. The IXform chain is baked into the points, so the
    // resulting groom is in the prim's world space. Widths are scaled by the
    // transform's average axis length — a groom under a 2x scale gets 2x-thick
    // strands, which is what every DCC means by it. A non-uniform scale is
    // ACCEPTED but logged, because a single scalar width cannot represent an
    // anisotropically scaled strand.
    // ============================================================================

    class AlembicGroomImporter
    {
      public:
        // Bumped whenever this importer's output for an unchanged input
        // changes. Stored in every cooked groom's provenance, so a stale
        // .ologroom can be told apart from a current one without re-importing.
        // 2: issue #1251 added `groom_role` and the name-inferred coat role, so
        // this importer's output for an UNCHANGED input now carries a coat table
        // it did not before — which is exactly what this number is for.
        static constexpr u32 kImporterVersion = 2;

        // The width substituted when a prim carries no `widths` param, in
        // source units. 0.1 mm — a human hair is 0.06-0.1 mm, so this is a
        // recognisable "nobody authored this" value at human scale rather than
        // an invisible zero.
        static constexpr f32 kDefaultWidth = 0.0001f;

        struct Result
        {
            Ref<GroomAsset> Groom;  // null on failure
            std::string Diagnostic; // why it failed, or "" on success
            // Announced SUBSTITUTIONS and lossy accommodations made on a
            // SUCCESSFUL import: a missing `widths` or `uvs` param, a
            // non-uniform transform, per-vertex uvs collapsed to the root.
            // These are logged too, but a log line is not something a caller
            // (or a test) can assert on, and Diagnostic is empty on success —
            // so "the import announced what it substituted" had no
            // machine-readable form until this existed.
            std::vector<std::string> Warnings;
            u32 CurvesRead = 0;
            u32 PrimsRead = 0;

            [[nodiscard]] bool Succeeded() const noexcept
            {
                return static_cast<bool>(Groom);
            }

            [[nodiscard]] static Result Failure(std::string diagnostic)
            {
                Result result;
                result.Diagnostic = std::move(diagnostic);
                return result;
            }
        };

        struct Options
        {
            // Recorded verbatim in the groom's provenance. Pass a
            // project-relative path; the file name is used when empty. NEVER
            // pass an absolute path — it would make the cooked bytes differ
            // between machines, which breaks the determinism contract.
            std::string ProvenancePath;
        };

        // Returns a Result whose Groom is null and whose Diagnostic NAMES the
        // problem on any rejection — there is no partial import and no silent
        // degradation (issue #1232, AC 3).
        [[nodiscard]] static Result Import(const std::filesystem::path& path, const Options& options = {});

        // What the editor's "Import as Groom" action does, minus the UI:
        // check the archive holds curves, import it, cook it, and write the
        // sibling `<stem>.ologroom` the asset system can then register.
        //
        // It lives HERE rather than inline in the content-browser handler so it
        // is testable. The first version of this feature put the whole sequence
        // nowhere at all — the importer had no production caller, so a groom
        // .abc could not be imported in the editor and nothing noticed, because
        // every test called Import() directly.
        struct SidecarCookResult
        {
            bool Ok = false;
            std::filesystem::path OutputPath;
            std::string Diagnostic;            // why it failed, or "" on success
            std::vector<std::string> Warnings; // announced substitutions, as above
            u32 CurveCount = 0;
            u32 GroupCount = 0;
            u32 GuideCount = 0;
            u64 CookedBytes = 0;
        };

        // `outputPath` empty means "next to the source, with the .ologroom
        // extension". A non-curve archive is a NAMED failure, not a silent
        // no-op: polygon Alembic is a different import path entirely.
        [[nodiscard]] static SidecarCookResult ImportAndCookToSidecar(const std::filesystem::path& abcPath,
                                                                      const Options& options = {},
                                                                      const std::filesystem::path& outputPath = {});

        // True when the archive at `path` contains at least one ICurves prim.
        // The editor uses it to route an .abc to this importer rather than to
        // AlembicMeshImporter — one extension, two schemas.
        [[nodiscard]] static bool ArchiveContainsCurves(const std::filesystem::path& path);
    };
} // namespace OloEngine

#endif // OLO_WITH_ALEMBIC
