#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomPreview.h"

#include "OloEngine/Renderer/Renderer3D.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

    namespace
    {
        // Whether the debug view may draw this curve at all: the guides-only
        // switch and the coat's role mask (#1251). One helper because the plan
        // and the selection must agree exactly — a plan that counted a curve the
        // selection then skipped would make the stride wrong for every group
        // after it.
        [[nodiscard]] bool PreviewCurveVisible(const GroomAsset& groom, u32 curve,
                                               const GroomPreviewSettings& settings) noexcept
        {
            if (settings.GuidesOnly && !groom.IsGuide(curve))
            {
                return false;
            }
            const GroomCoatRole role = groom.GetGroupCoat(groom.GetCurveGroupIds()[curve]).GetRole();
            return (settings.RoleVisibilityMask & (1u << static_cast<u32>(role))) != 0u;
        }

        [[nodiscard]] bool AllRolesVisible(const GroomPreviewSettings& settings) noexcept
        {
            constexpr u32 all = (1u << GroomCoatRoleCount) - 1u;
            return (settings.RoleVisibilityMask & all) == all;
        }
    } // namespace

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
        // The O(1) shortcut survives for the common case — every role visible —
        // because GetGuideCount() and the curve count are both already derived.
        // A mask with a bit cleared has no such counter, so it is scanned; that
        // is O(curveCount), which every other loop in this file already is.
        u32 candidateCount = settings.GuidesOnly ? groom.GetGuideCount() : curveCount;
        if (!AllRolesVisible(settings))
        {
            candidateCount = 0;
            for (u32 curve = 0; curve < curveCount; ++curve)
            {
                if (PreviewCurveVisible(groom, curve, settings))
                {
                    ++candidateCount;
                }
            }
        }
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
            u32 affordableStrands =
                std::max(1u, static_cast<u32>(static_cast<f32>(std::max(1u, settings.MaxSegments)) / linesPerStrand));
            // Per-group phasing guarantees every represented group its first
            // candidate, which can add up to one strand per group on top of
            // what the stride alone selects. Price that in, so the exact cap in
            // DrawGroomPreview is a backstop for variable-length grooms rather
            // than something that fires on every ordinary multi-group one.
            // Conservative on purpose: GetGroupCount() counts DECLARED groups,
            // which is an over-estimate under GuidesOnly.
            const u32 groupCount = groom.GetGroupCount();
            affordableStrands = (affordableStrands > groupCount) ? (affordableStrands - groupCount) : 1u;
            strideFromSegments = (candidateCount + affordableStrands - 1u) / affordableStrands;
        }

        stats.Stride = std::max(1u, std::max(strideFromStrands, strideFromSegments));
        stats.SegmentBudgetLimited = strideFromSegments > strideFromStrands;

        // Per-group phasing needs the cook's contiguous group ranges. On an
        // un-canonicalised groom the ids are interleaved, and resetting the
        // per-group counter at every id change would select nearly every curve;
        // such a groom keeps the global stride instead. O(curveCount), which the
        // draw loop below already is.
        const auto& groupIds = groom.GetCurveGroupIds();
        stats.GroupPhasedSelection = std::is_sorted(groupIds.begin(), groupIds.end());
        return stats;
    }

    void SelectGroomPreviewCurves(const GroomAsset& groom, const GroomPreviewSettings& settings,
                                  GroomPreviewStats& plan, std::vector<u32>& outCurves)
    {
        OLO_PROFILE_FUNCTION();

        outCurves.clear();
        plan.StrandsDrawn = 0;
        plan.SegmentsDrawn = 0;
        plan.SegmentBudgetExhausted = false;
        if (plan.StrandsAvailable == 0 || plan.Stride == 0)
        {
            return;
        }

        const u32 curveCount = groom.GetCurveCount();

        // The stride is phased PER GROUP when the groom is canonicalised: the
        // cook makes every group a contiguous range, so resetting the counter at
        // each boundary means index 0 of every represented group is always
        // selected. A single GLOBAL modulo can skip a small group outright when
        // none of its indices happen to be divisible by the stride — the "shows
        // one group and looks fine" failure the subsampling exists to prevent,
        // and invisible on screen.
        u32 candidateIndex = 0;
        u32 currentGroup = std::numeric_limits<u32>::max();

        // MaxSegments is enforced HERE, against each curve's EXACT cost, not
        // only through the average-derived stride. Every debug line takes one
        // entry of the frame's shared transform buffer, so a variable-length
        // groom whose average understated its real cost would otherwise
        // overflow that buffer. Running out is reported, not silent.
        const u32 lineBudget = std::max(1u, settings.MaxSegments);
        u32 plannedLines = 0;

        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            if (!PreviewCurveVisible(groom, curve, settings))
            {
                continue;
            }

            if (plan.GroupPhasedSelection)
            {
                if (const u32 group = groom.GetCurveGroupIds()[curve]; group != currentGroup)
                {
                    currentGroup = group;
                    candidateIndex = 0;
                }
            }
            if ((candidateIndex++ % plan.Stride) != 0)
            {
                continue;
            }

            const u32 points = groom.GetCurvePointCount(curve);
            if (points < 2)
            {
                continue; // Validate() forbids this; skip rather than assert in a draw path.
            }

            const u32 curveCost = (settings.ShowStrands ? (points - 1u) : 0u) + (settings.ShowRoots ? 3u : 0u);
            if (plannedLines + curveCost > lineBudget)
            {
                plan.SegmentBudgetExhausted = true;
                break;
            }
            plannedLines += curveCost;

            outCurves.push_back(curve);
            ++plan.StrandsDrawn;
            plan.SegmentsDrawn += curveCost;
        }
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

        std::vector<u32> selected;
        SelectGroomPreviewCurves(groom, settings, stats, selected);

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

        // WHICH curves are drawn is decided above, by SelectGroomPreviewCurves.
        // This loop only draws them: the stride, the per-group phasing and the
        // exact line budget have one implementation, and it is the one the
        // tests exercise.
        for (const u32 curve : selected)
        {
            const u32 first = groom.GetCurveFirstPoint(curve);
            const u32 points = groom.GetCurvePointCount(curve);

            glm::vec3 baseColor(0.8f);
            if (settings.ColorByGroup)
            {
                baseColor = GroomGroupColor(groom.GetCurveGroupIds()[curve]);
            }
            // A guide is drawn white-hot: guides drive everything downstream,
            // so "which strands are guides" must be answerable at a glance.
            if (groom.IsGuide(curve))
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
            }
        }

        return stats;
    }

    // ── Binding preview (issue #1249) ─────────────────────────────

    GroomBindingPreviewStats DrawGroomBindingPreview(const GroomBindingAsset& binding,
                                                     std::span<const GroomRootTransform> transforms,
                                                     const glm::mat4& transform,
                                                     const GroomBindingPreviewSettings& settings)
    {
        GroomBindingPreviewStats stats;
        const u32 rootCount = binding.GetRootCount();
        if (rootCount == 0u || transforms.size() != rootCount)
        {
            // A transform array that does not span the binding is treated as
            // ABSENT rather than partially drawn: half a preview is a picture
            // that says the other half is unbound, which is the opposite of
            // what it would mean.
            return stats;
        }
        stats.RootsAvailable = rootCount;

        const u32 maxRoots = std::max(1u, settings.MaxRoots);
        stats.Stride = rootCount > maxRoots ? ((rootCount + maxRoots - 1u) / maxRoots) : 1u;

        // World-space axes, so the arms are the same length whatever the
        // entity's scale is — a frame drawn at the entity's scale would vanish
        // on a small character and swamp a large one, and this view is read by
        // eye at whatever framing the user has.
        const f32 axis = std::max(settings.AxisLength, 1.0e-4f);

        // Renderer3D::DrawLine's `thickness` is a WORLD-space width in units of
        // 5 mm, not a pixel width — see the note in DrawGroomPreview. The frame
        // arms are read by eye at close range, so they are deliberately fatter
        // than a strand: 0.2 is one millimetre.
        constexpr f32 kFrameThickness = 0.2f;

        for (u32 curve = 0; curve < rootCount; curve += stats.Stride)
        {
            const GroomRootTransform& rootTransform = transforms[curve];
            const GroomRootBinding& record = binding.GetRoot(curve);

            if (!rootTransform.Valid)
            {
                // A curve the strand budget did not select was never evaluated;
                // it is not a degeneracy and must not be drawn as one.
                if (!rootTransform.Held || !settings.ShowHeldRoots)
                {
                    continue;
                }
                // Held at rest: drawn at the BIND-POSE origin, because that is
                // where the strand actually is, and in warning orange so it
                // reads as a diagnosis rather than as another frame.
                const glm::vec3 origin = glm::vec3(transform * glm::vec4(record.RestOrigin, 1.0f));
                constexpr glm::vec3 held{ 1.0f, 0.55f, 0.15f };
                SubmitLine(origin - glm::vec3(axis, 0.0f, 0.0f), origin + glm::vec3(axis, 0.0f, 0.0f), held, kFrameThickness);
                SubmitLine(origin - glm::vec3(0.0f, axis, 0.0f), origin + glm::vec3(0.0f, axis, 0.0f), held, kFrameThickness);
                SubmitLine(origin - glm::vec3(0.0f, 0.0f, axis), origin + glm::vec3(0.0f, 0.0f, axis), held, kFrameThickness);
                stats.LinesDrawn += 3u;
                ++stats.RootsHeldDrawn;
                continue;
            }

            const glm::vec3 origin = glm::vec3(transform * glm::vec4(rootTransform.Origin, 1.0f));
            // The frame's own axes, rotated into world space by the entity's
            // transform. mat3 of the world matrix, so a rotated or scaled
            // entity's frames point where its coat does; normalised so the arm
            // LENGTH stays the authored one.
            const glm::mat3 world{ transform };
            const auto axisIn = [&](const glm::vec3& local)
            {
                const glm::vec3 direction = world * (rootTransform.Rotation * local);
                const f32 length = glm::length(direction);
                return length > 1.0e-8f ? direction / length * axis : glm::vec3{ 0.0f };
            };

            // x = tangent (red), y = bitangent (green), z = surface normal
            // (blue) — the same convention MakeGroomSurfaceFrame builds and the
            // same one every DCC shows, so a frame that is visibly rolled is a
            // frame that really is.
            SubmitLine(origin, origin + axisIn({ 1.0f, 0.0f, 0.0f }), glm::vec3(0.9f, 0.25f, 0.25f), kFrameThickness);
            SubmitLine(origin, origin + axisIn({ 0.0f, 1.0f, 0.0f }), glm::vec3(0.25f, 0.9f, 0.25f), kFrameThickness);
            SubmitLine(origin, origin + axisIn({ 0.0f, 0.0f, 1.0f }), glm::vec3(0.3f, 0.5f, 1.0f), kFrameThickness);
            stats.LinesDrawn += 3u;
            ++stats.RootsDrawn;
        }

        return stats;
    }
} // namespace OloEngine
