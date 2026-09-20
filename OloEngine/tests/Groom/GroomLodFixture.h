#pragma once

// =============================================================================
// GroomLodFixture.h — turning a LOD level, or a subset of a groom, back into a
// GroomAsset so the coverage model can measure it. Issue #1252.
//
// WHY THIS EXISTS. GroomCoverage::ProjectGroom takes a GroomAsset, because a
// groom is what the renderer draws. The comparison behind this issue has to
// measure THREE curve sets against each other — the full groom, the groom
// thinned by the strand budget, and the cooked card level — and only the first
// of those is already an asset. Rather than widen the coverage model (which
// would put a test's convenience into the file whose whole value is being the
// faithful model of the GPU), the two others are rebuilt into real assets here,
// through the real GroomBuilder, with the real validation.
//
// It also means the measured card set is the SAME bytes the cook writes and the
// renderer draws, rather than a parallel reimplementation that could agree with
// the analysis and disagree with the picture.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"

#include <glm/glm.hpp>

#include <cmath>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::Tests::GroomLodFixture
{
    // A groom holding exactly `curves` of `base`, in the given order.
    //
    // Group NAMES are carried over wholesale rather than compacted, so a
    // subset's group ids mean what they meant in the base — a compaction would
    // silently repoint every coat description and the measured coat would not
    // be the authored one.
    [[nodiscard]] inline Ref<GroomAsset> MakeSubsetGroom(const GroomAsset& base, std::span<const u32> curves,
                                                         std::string& outReason)
    {
        GroomBuilder builder;
        for (const std::string& name : base.GetGroupNames())
        {
            u16 id = 0;
            if (!builder.AddGroup(name, id, outReason))
            {
                return nullptr;
            }
        }

        std::vector<glm::vec3> points;
        std::vector<f32> widths;
        for (const u32 curve : curves)
        {
            if (curve >= base.GetCurveCount())
            {
                outReason = std::format("subset names curve {} but the groom has {}", curve, base.GetCurveCount());
                return nullptr;
            }
            const u32 first = base.GetCurveFirstPoint(curve);
            const u32 count = base.GetCurvePointCount(curve);
            points.assign(base.GetPoints().begin() + first, base.GetPoints().begin() + first + count);
            widths.assign(base.GetPointWidths().begin() + first, base.GetPointWidths().begin() + first + count);

            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = base.GetRootUVs()[curve];
            input.GroupId = base.GetCurveGroupIds()[curve];
            input.IsGuide = base.IsGuide(curve);
            if (!builder.AddCurve(input, outReason))
            {
                return nullptr;
            }
        }

        Ref<GroomAsset> groom = builder.Build(outReason);
        if (groom && !GroomCooker::Canonicalize(*groom, outReason))
        {
            return nullptr;
        }
        return groom;
    }

    // The same groom with its root UVs folded into the unit chart.
    //
    // WHY THIS EXISTS, AND WHY IT IS HERE RATHER THAN IN GroomStrandFixture.
    // #1246's generators place a strand at the golden angle and record its root
    // U as `phi / 2pi` WITHOUT wrapping, so on a 20 000-strand scalp the last
    // strand's U is about 11 459. Nothing in #1246, #1247 or #1248 reads a root
    // UV, so it never mattered there — but the clump-cell addressing clamps a
    // UV to +/-16 (GroomCoatClumpCell), so clustering that groom puts every
    // strand past the clamp in ONE cell. The first run of this issue's
    // comparison cooked 64 cards for that scalp and measured them carrying 16%
    // of its coverage, which is a measurement of the fixture rather than of the
    // representation.
    //
    // Folded rather than fixed upstream: those generators are #1246's, their
    // shapes are what three committed analyses were measured on, and a real
    // groom's root UV is a position on a chart — so wrapping HERE is both the
    // smaller change and the more honest one, because it says out loud that the
    // card comparison needs a realistic parameterisation and the coverage
    // comparison never did.
    [[nodiscard]] inline Ref<GroomAsset> RebuildWithWrappedRootUVs(const GroomAsset& base, std::string& outReason)
    {
        GroomBuilder builder;
        for (const std::string& name : base.GetGroupNames())
        {
            u16 id = 0;
            if (!builder.AddGroup(name, id, outReason))
            {
                return nullptr;
            }
        }

        std::vector<glm::vec3> points;
        std::vector<f32> widths;
        const u32 curveCount = base.GetCurveCount();
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            const u32 first = base.GetCurveFirstPoint(curve);
            const u32 count = base.GetCurvePointCount(curve);
            points.assign(base.GetPoints().begin() + first, base.GetPoints().begin() + first + count);
            widths.assign(base.GetPointWidths().begin() + first, base.GetPointWidths().begin() + first + count);

            const glm::vec2 uv = base.GetRootUVs()[curve];
            // std::floor, so a negative U folds to the same place a positive one
            // does rather than reflecting — the same reason GroomCoatClumpCell
            // floors rather than truncating.
            const glm::vec2 wrapped{ uv.x - std::floor(uv.x), uv.y - std::floor(uv.y) };

            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = wrapped;
            input.GroupId = base.GetCurveGroupIds()[curve];
            input.IsGuide = base.IsGuide(curve);
            if (!builder.AddCurve(input, outReason))
            {
                return nullptr;
            }
        }

        Ref<GroomAsset> groom = builder.Build(outReason);
        if (groom && !GroomCooker::Canonicalize(*groom, outReason))
        {
            return nullptr;
        }
        return groom;
    }

    // A groom holding a cooked LOD level's curves.
    //
    // `base` supplies the group NAME table only. The level's own group ids are
    // the base's, by construction (GroomLodBuilder never crosses a group), so
    // carrying the names over keeps a card's role and coat description the ones
    // its members were authored with.
    [[nodiscard]] inline Ref<GroomAsset> MakeGroomFromLevel(const GroomAsset& base, const GroomLodLevel& level,
                                                            std::string& outReason)
    {
        GroomBuilder builder;
        for (const std::string& name : base.GetGroupNames())
        {
            u16 id = 0;
            if (!builder.AddGroup(name, id, outReason))
            {
                return nullptr;
            }
        }

        const GroomCurveView view = level.GetCurveView();
        std::vector<glm::vec3> points;
        std::vector<f32> widths;
        const u32 curveCount = view.GetCurveCount();
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            const u32 first = view.GetCurveFirstPoint(curve);
            const u32 count = view.GetCurvePointCount(curve);
            points.assign(view.Points.begin() + first, view.Points.begin() + first + count);
            widths.assign(view.PointWidths.begin() + first, view.PointWidths.begin() + first + count);

            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = view.RootUVs[curve];
            input.GroupId = view.CurveGroupIds[curve];
            input.IsGuide = view.IsGuide(curve);
            if (!builder.AddCurve(input, outReason))
            {
                return nullptr;
            }
        }

        Ref<GroomAsset> groom = builder.Build(outReason);
        if (groom && !GroomCooker::Canonicalize(*groom, outReason))
        {
            return nullptr;
        }
        return groom;
    }
} // namespace OloEngine::Tests::GroomLodFixture
