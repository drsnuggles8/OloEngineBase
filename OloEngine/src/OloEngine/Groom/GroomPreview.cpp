#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomPreview.h"

#include "OloEngine/Renderer/Renderer3D.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Renderer3D::DrawLine BUILDS a command packet and hands it back; it
        // does NOT queue it. Every caller in the engine pairs it with
        // SubmitPacket (see Renderer3D::DrawWorldAxisHelper), and forgetting
        // the pair is silent: the preview runs, reports thousands of segments,
        // and draws nothing at all.
        void SubmitLine(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color, f32 thickness)
        {
            if (CommandPacket* packet = Renderer3D::DrawLine(from, to, color, thickness))
            {
                Renderer3D::SubmitPacket(packet);
            }
        }

        // Hue -> RGB at full saturation, value 1. Small and local; pulling in a
        // general colour-space header for six lines would be the bigger cost.
        glm::vec3 HueToRgb(f32 hue)
        {
            const f32 h = hue * 6.0f;
            const f32 x = 1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f);
            if (h < 1.0f)
                return { 1.0f, x, 0.0f };
            if (h < 2.0f)
                return { x, 1.0f, 0.0f };
            if (h < 3.0f)
                return { 0.0f, 1.0f, x };
            if (h < 4.0f)
                return { 0.0f, x, 1.0f };
            if (h < 5.0f)
                return { x, 0.0f, 1.0f };
            return { 1.0f, 0.0f, x };
        }
    } // anonymous namespace

    glm::vec3 GroomGroupColor(u32 groupId) noexcept
    {
        // Golden-ratio hue rotation: consecutive group ids land far apart on
        // the wheel, so neighbouring groups are always distinguishable — which
        // a linear id/count mapping stops being past a handful of groups.
        constexpr f32 kGoldenRatioConjugate = 0.618033988749895f;
        const f32 hue = std::fmod(static_cast<f32>(groupId) * kGoldenRatioConjugate, 1.0f);
        // Pulled off full saturation so the guide highlight (pure white) and
        // the root markers stay readable against it.
        return glm::mix(HueToRgb(hue), glm::vec3(1.0f), 0.25f);
    }

    GroomPreviewStats PlanGroomPreview(const GroomAsset& groom, const GroomPreviewSettings& settings)
    {
        GroomPreviewStats stats;
        const u32 curveCount = groom.GetCurveCount();
        if (curveCount == 0 || (!settings.ShowStrands && !settings.ShowRoots))
        {
            return stats;
        }

        // Which curves are candidates at all. GuidesOnly is resolved first so
        // the stride below subsamples the guides, not the whole groom (a groom
        // with 300 guides in 1M strands would otherwise show almost none).
        const u32 candidateCount = settings.GuidesOnly ? groom.GetGuideCount() : curveCount;
        stats.StrandsAvailable = candidateCount;
        if (candidateCount == 0)
        {
            return stats;
        }

        // Two caps, and the tighter one wins. The strand cap is the authored
        // intent; the segment cap is the frame's transform budget (see
        // GroomPreviewSettings::MaxSegments). Both are turned into a STRIDE
        // rather than a truncation, so whichever binds, the drawn subset still
        // spans the whole groom.
        const u32 maxStrands = std::max(1u, settings.MaxStrands);
        const u32 strideFromStrands = (candidateCount + maxStrands - 1u) / maxStrands;

        // Lines per strand, averaged over the groom: one per segment when
        // strands are drawn, plus three for the root cross. Using the average
        // rather than a per-curve sum keeps this O(1) — the strand lengths in a
        // groom are near-uniform, and the budget is a safety bound, not an
        // exact reservation.
        const f32 averageSegments = std::max(1.0f, (static_cast<f32>(groom.GetPointCount()) /
                                                    static_cast<f32>(curveCount)) -
                                                       1.0f);
        const f32 linesPerStrand = (settings.ShowStrands ? averageSegments : 0.0f) + (settings.ShowRoots ? 3.0f : 0.0f);
        u32 strideFromSegments = 1;
        if (linesPerStrand > 0.0f)
        {
            const u32 affordableStrands =
                std::max(1u, static_cast<u32>(static_cast<f32>(std::max(1u, settings.MaxSegments)) / linesPerStrand));
            strideFromSegments = (candidateCount + affordableStrands - 1u) / affordableStrands;
        }

        stats.Stride = std::max(1u, std::max(strideFromStrands, strideFromSegments));
        stats.SegmentBudgetLimited = strideFromSegments > strideFromStrands;
        return stats;
    }

    GroomPreviewStats DrawGroomPreview(const GroomAsset& groom, const glm::mat4& transform,
                                       const GroomPreviewSettings& settings)
    {
        OLO_PROFILE_FUNCTION();

        GroomPreviewStats stats = PlanGroomPreview(groom, settings);
        if (stats.StrandsAvailable == 0)
        {
            return stats;
        }
        const u32 curveCount = groom.GetCurveCount();

        const glm::vec3& boundsMin = groom.GetBoundsMin();
        const glm::vec3& boundsMax = groom.GetBoundsMax();
        // The root marker is sized relative to the groom so a centimetre-scale
        // groom and a metre-scale one both get a visible, non-dominating cross.
        const f32 diagonal = glm::length(boundsMax - boundsMin);
        const f32 markerSize = std::max(settings.RootMarkerSize, diagonal * 0.004f);

        // Renderer3D::DrawLine's `thickness` is a WORLD-space width in units of
        // 5 mm (it multiplies by 0.005), not a pixel width. A groom is authored
        // at real scale — a human scalp is 9 cm across — so the default
        // thickness of 1.0 would draw every strand 5 mm thick and turn 2000 of
        // them into one solid blob. Derive it from the groom's own size instead,
        // with a floor so a tiny groom still produces a visible line.
        const f32 strandThickness = std::max(0.05f, diagonal * 0.4f);
        // Roots a little fatter than the strands they sit on, so the markers
        // read as markers rather than as a denser patch of hair.
        const f32 rootThickness = strandThickness * 1.8f;

        u32 candidateIndex = 0;
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            const bool isGuide = groom.IsGuide(curve);
            if (settings.GuidesOnly && !isGuide)
            {
                continue;
            }
            const u32 thisCandidate = candidateIndex++;
            if ((thisCandidate % stats.Stride) != 0)
            {
                continue;
            }

            const u32 first = groom.GetCurveFirstPoint(curve);
            const u32 points = groom.GetCurvePointCount(curve);
            if (points < 2)
            {
                continue; // Validate() forbids this; skip rather than assert in a draw path.
            }

            glm::vec3 baseColor(0.8f);
            if (settings.ColorByGroup)
            {
                baseColor = GroomGroupColor(groom.GetCurveGroupIds()[curve]);
            }
            // A guide is drawn white-hot: guides drive everything downstream,
            // so "which strands are guides" must be answerable at a glance.
            if (isGuide)
            {
                baseColor = glm::mix(baseColor, glm::vec3(1.0f), 0.6f);
            }

            const glm::vec3 rootWorld = glm::vec3(transform * glm::vec4(groom.GetPoints()[first], 1.0f));

            if (settings.ShowStrands)
            {
                glm::vec3 previous = rootWorld;
                for (u32 i = 1; i < points; ++i)
                {
                    const glm::vec3 current = glm::vec3(transform * glm::vec4(groom.GetPoints()[first + i], 1.0f));

                    glm::vec3 color = baseColor;
                    if (settings.ShowDirection)
                    {
                        // Dark at the root, full colour at the tip. This is the
                        // whole direction cue — a reversed import reads as a
                        // strand that is bright where it meets the scalp.
                        const f32 t = static_cast<f32>(i) / static_cast<f32>(points - 1);
                        color = baseColor * glm::mix(0.15f, 1.0f, t);
                    }
                    SubmitLine(previous, current, color, strandThickness);
                    ++stats.SegmentsDrawn;
                    previous = current;
                }
            }

            if (settings.ShowRoots)
            {
                // A three-axis cross rather than a sphere: it costs three lines
                // instead of a mesh, and it stays visible edge-on.
                const glm::vec3 rootColor = glm::mix(baseColor, glm::vec3(1.0f, 1.0f, 1.0f), 0.5f);
                SubmitLine(rootWorld - glm::vec3(markerSize, 0.0f, 0.0f),
                           rootWorld + glm::vec3(markerSize, 0.0f, 0.0f), rootColor, rootThickness);
                SubmitLine(rootWorld - glm::vec3(0.0f, markerSize, 0.0f),
                           rootWorld + glm::vec3(0.0f, markerSize, 0.0f), rootColor, rootThickness);
                SubmitLine(rootWorld - glm::vec3(0.0f, 0.0f, markerSize),
                           rootWorld + glm::vec3(0.0f, 0.0f, markerSize), rootColor, rootThickness);
                stats.SegmentsDrawn += 3;
            }

            ++stats.StrandsDrawn;
        }

        return stats;
    }
} // namespace OloEngine
