#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include "OloEngine/Groom/GroomAsset.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // The curves this build will actually walk, as a stride over the whole
        // groom rather than a prefix of it.
        //
        // WHY A STRIDE. A prefix of a cooked groom is one contiguous range of
        // curves, and the cook sorts curves by group — so "the first 50 000"
        // is one side of the animal. The visible result is a coat with a bald
        // flank, which reads as a broken import rather than as a budget, and
        // is the exact failure GroomPreview already learned. A stride covers
        // the whole groom at lower density, which is what a budget should look
        // like.
        struct Selection
        {
            u32 Stride = 1;
            u32 Selected = 0;
            u32 Available = 0;
            bool SegmentBudgetLimited = false;
        };

        [[nodiscard]] u32 CountCurveSegments(const GroomAsset& groom, u32 curveIndex) noexcept
        {
            const u32 points = groom.GetCurvePointCount(curveIndex);
            return points >= 2u ? points - 1u : 0u;
        }

        [[nodiscard]] bool CurveIsEligible(const GroomAsset& groom, u32 curveIndex,
                                           const GroomStrandBuildSettings& settings) noexcept
        {
            if (settings.GuidesOnly && !groom.IsGuide(curveIndex))
            {
                return false;
            }
            return groom.GetCurvePointCount(curveIndex) >= 2u;
        }

        [[nodiscard]] Selection SelectCurves(const GroomAsset& groom, const GroomStrandBuildSettings& settings)
        {
            Selection selection;

            u64 eligibleSegments = 0;
            for (u32 curve = 0; curve < groom.GetCurveCount(); ++curve)
            {
                if (!CurveIsEligible(groom, curve, settings))
                {
                    continue;
                }
                ++selection.Available;
                eligibleSegments += CountCurveSegments(groom, curve);
            }

            if (selection.Available == 0u)
            {
                selection.Selected = 0;
                return selection;
            }

            const u32 maxStrands = std::max(1u, settings.MaxStrands);
            u32 stride = 1u;
            if (selection.Available > maxStrands)
            {
                stride = (selection.Available + maxStrands - 1u) / maxStrands;
            }

            // The segment budget is the one that actually sizes the buffer, so
            // it can widen the stride further. It is applied from the AVERAGE
            // segments per eligible curve; a groom with wildly varying strand
            // lengths can still overshoot, which is why the build below also
            // stops exactly at the cap and says so.
            const u32 maxSegments = std::max(1u, settings.MaxSegments);
            const f64 segmentsPerCurve =
                static_cast<f64>(eligibleSegments) / static_cast<f64>(selection.Available);
            if (segmentsPerCurve > 0.0)
            {
                const f64 affordableCurves = static_cast<f64>(maxSegments) / segmentsPerCurve;
                if (affordableCurves >= 1.0 && static_cast<f64>(selection.Available) > affordableCurves)
                {
                    const u32 segmentStride = static_cast<u32>(
                        std::ceil(static_cast<f64>(selection.Available) / affordableCurves));
                    if (segmentStride > stride)
                    {
                        stride = segmentStride;
                        selection.SegmentBudgetLimited = true;
                    }
                }
            }

            selection.Stride = std::max(1u, stride);
            selection.Selected = (selection.Available + selection.Stride - 1u) / selection.Stride;
            return selection;
        }
    } // namespace

    GroomStrandMeshStats PlanGroomStrandMesh(const GroomAsset& groom, const GroomStrandBuildSettings& settings)
    {
        GroomStrandMeshStats stats;
        const Selection selection = SelectCurves(groom, settings);
        stats.StrandsAvailable = selection.Available;
        stats.StrandsSelected = selection.Selected;
        stats.Stride = selection.Stride;
        stats.SegmentBudgetLimited = selection.SegmentBudgetLimited;

        u64 segments = 0;
        u32 taken = 0;
        for (u32 curve = 0; curve < groom.GetCurveCount(); ++curve)
        {
            if (!CurveIsEligible(groom, curve, settings))
            {
                continue;
            }
            if ((taken % selection.Stride) == 0u)
            {
                // TRUNCATES MID-CURVE, exactly as BuildGroomStrandMesh does.
                // Refusing the whole curve instead would make the plan and the
                // build disagree whenever one curve straddles the cap: a groom
                // of one ten-segment curve with MaxSegments 5 would be planned
                // as 0 segments and 0 MiB — with no budget warning — while the
                // pass built 5. The inspector shows the PLAN on every frame, so
                // that divergence is a panel confidently describing a coat that
                // is not the one on screen.
                const u64 remaining = static_cast<u64>(settings.MaxSegments) - segments;
                const u64 wanted = CountCurveSegments(groom, curve);
                if (wanted > remaining)
                {
                    segments += remaining;
                    stats.SegmentBudgetLimited = true;
                    break;
                }
                segments += wanted;
            }
            ++taken;
        }

        stats.SegmentCount = static_cast<u32>(segments);
        stats.VertexCount = stats.SegmentCount * 4u;
        stats.IndexCount = stats.SegmentCount * 6u;
        stats.VertexBytes = static_cast<u64>(stats.VertexCount) * sizeof(GroomStrandVertex);
        stats.IndexBytes = static_cast<u64>(stats.IndexCount) * sizeof(u32);
        return stats;
    }

    GroomStrandMeshStats BuildGroomStrandMesh(const GroomAsset& groom, const GroomStrandBuildSettings& settings,
                                              std::vector<GroomStrandVertex>& outVertices, std::vector<u32>& outIndices)
    {
        outVertices.clear();
        outIndices.clear();

        GroomStrandMeshStats stats;
        const Selection selection = SelectCurves(groom, settings);
        stats.StrandsAvailable = selection.Available;
        stats.StrandsSelected = selection.Selected;
        stats.Stride = selection.Stride;
        stats.SegmentBudgetLimited = selection.SegmentBudgetLimited;

        const GroomStrandMeshStats plan = PlanGroomStrandMesh(groom, settings);
        outVertices.reserve(plan.VertexCount);
        outIndices.reserve(plan.IndexCount);

        const auto& points = groom.GetPoints();
        const auto& widths = groom.GetPointWidths();

        u32 taken = 0;
        u32 emittedSegments = 0;
        for (u32 curve = 0; curve < groom.GetCurveCount(); ++curve)
        {
            if (groom.GetCurvePointCount(curve) < 2u)
            {
                ++stats.CurvesSkippedTooShort;
                continue;
            }
            if (settings.GuidesOnly && !groom.IsGuide(curve))
            {
                continue;
            }

            const bool selected = (taken % selection.Stride) == 0u;
            ++taken;
            if (!selected)
            {
                continue;
            }

            const u32 first = groom.GetCurveFirstPoint(curve);
            const u32 count = groom.GetCurvePointCount(curve);
            const f32 invSpan = 1.0f / static_cast<f32>(count - 1u);

            for (u32 i = 0; i + 1u < count; ++i)
            {
                if (emittedSegments >= settings.MaxSegments)
                {
                    // Enforced exactly here rather than trusted from the
                    // stride: the stride is derived from an AVERAGE strand
                    // length, and a groom whose long strands happen to land on
                    // stride-aligned indices overshoots it. Stopping mid-groom
                    // is reported, never silent.
                    stats.SegmentBudgetLimited = true;
                    break;
                }

                const glm::vec3& p0 = points[first + i];
                const glm::vec3& p1 = points[first + i + 1u];
                // Stored the same way for every corner of the quad so the
                // vertex shader derives ONE screen-space tangent per segment.
                // See GroomStrandVertex::Other for what goes wrong otherwise.
                const glm::vec3 delta = p1 - p0;

                // The cooked widths are DIAMETERS (the Alembic/USD
                // convention); the halving happens exactly once, here.
                const f32 r0 = widths[first + i] * 0.5f;
                const f32 r1 = widths[first + i + 1u] * 0.5f;

                const f32 u0 = static_cast<f32>(i) * invSpan;
                const f32 u1 = static_cast<f32>(i + 1u) * invSpan;

                const f32 segmentId =
                    std::bit_cast<f32>(GroomSegmentIdentity(curve, i));

                const u32 base = static_cast<u32>(outVertices.size());

                GroomStrandVertex vertex;
                vertex.SegmentId = segmentId;

                // Corner order: (-side at P0), (+side at P0), (+side at P1),
                // (-side at P1) — a quad, not a bowtie, because `Other` gives
                // all four the same tangent.
                vertex.Position = p0;
                vertex.Other = p0 + delta;
                vertex.Radius = r0;
                vertex.Side = -1.0f;
                vertex.Coords = { u0, -1.0f };
                outVertices.push_back(vertex);

                vertex.Side = 1.0f;
                vertex.Coords = { u0, 1.0f };
                outVertices.push_back(vertex);

                vertex.Position = p1;
                vertex.Other = p1 + delta;
                vertex.Radius = r1;
                vertex.Side = 1.0f;
                vertex.Coords = { u1, 1.0f };
                outVertices.push_back(vertex);

                vertex.Side = -1.0f;
                vertex.Coords = { u1, -1.0f };
                outVertices.push_back(vertex);

                outIndices.push_back(base + 0u);
                outIndices.push_back(base + 1u);
                outIndices.push_back(base + 2u);
                outIndices.push_back(base + 0u);
                outIndices.push_back(base + 2u);
                outIndices.push_back(base + 3u);

                ++emittedSegments;
            }

            if (emittedSegments >= settings.MaxSegments)
            {
                break;
            }
        }

        stats.SegmentCount = emittedSegments;
        stats.VertexCount = static_cast<u32>(outVertices.size());
        stats.IndexCount = static_cast<u32>(outIndices.size());
        stats.VertexBytes = static_cast<u64>(stats.VertexCount) * sizeof(GroomStrandVertex);
        stats.IndexBytes = static_cast<u64>(stats.IndexCount) * sizeof(u32);
        return stats;
    }
} // namespace OloEngine
