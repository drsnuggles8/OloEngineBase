#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomCasterCoverageTest -- issue #1533, the follow-up review of the caster share.
//
// A shadow view casts a hashed PREFIX of each run of a coat's strands
// (GroomCasterOrder, one run per group), as many as DecideGroomCasterRun asks
// for. Its layer count is an ESTIMATE -- a projected length over a box -- so
// these cases hold the decision to a MODEL of the depth map instead of to its
// own arithmetic: every segment drawn as the GPU draws it (a ribbon at the
// floored width, NO end caps) into a light-space texel grid, the whole coat
// beside the subset the rule keeps, per group.
//
// TWO MEASUREMENTS, NEITHER OF THEM THE RULE'S OWN NUMBERS:
//   - LAYERS KEPT. Over the texels a group's whole cast covers, the mean count
//     of its ribbons there, whole and in the subset. The rule claims a subset
//     keeps kGroomCasterMinLayers of a group's own layers wherever the group
//     has that many to spare; the measurement checks it per group.
//   - LEAK. The real coat's opacity per texel, 1 - exp(-c) with c the true
//     coverage (each covering ribbon weighted by its true width over its
//     floored one), against the subset's occupancy, both under a 3x3 filter
//     like the receiver's PCF. Light the real coat would block that the thinned
//     map lets through.
//
// The whole floored cast is the REGRESSION COMPARISON, not physical truth: it
// over-occludes by the widening factor, and a subset that differs from it in a
// sparse fringe is closer to the real coat there, not further. So its
// difference from the subset is REPORTED -- open texels in its solid interior,
// and the mean filtered difference -- and not held to a budget.
//
// Every adversarial case also measures the coat-wide rule the runs replaced (an
// order built without strand summaries: one run, an assumed half share of the
// length, the coat's own box) and expects it to FAIL there, so a case that
// stops being adversarial is noticed rather than passing for the wrong reason.
// =============================================================================

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCasterPose.h"
#include "OloEngine/Groom/GroomShadowWidening.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // A 4 mm texel: a 16 m cascade at 4096^2, the editor's quality tier. The
        // share thins a coat in the cascades past the first, so that is where
        // these coats are cast; the floor widens a 0.1 mm strand fortyfold.
        constexpr f32 kResolution = 4096.0f;
        constexpr f32 kExtentMetres = 16.0f;
        constexpr f32 kMinWidthTexels = 1.0f;
        // The light the thinned map may let through beyond the real coat's,
        // averaged over a group's covered texels after the 3x3 filter.
        constexpr f64 kLeakBudget = 0.02;
        // The share of its layers a group may lose to the prefix's rounding and
        // to sampling: a subset is a random share, not an exact one.
        constexpr f64 kLayerTolerance = 0.9;
        // The temporal budget: how much of a group's solid interior may open or
        // close between two steps of a turning light.
        constexpr f64 kStepBudget = 0.05;

        // Deterministic [0, 1) hash, so every case is the same coat on every run.
        [[nodiscard]] f32 Hash01(u32 a, u32 b)
        {
            u32 x = a * 0x9E3779B9u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
            x ^= x >> 16u;
            x *= 0x7FEB352Du;
            x ^= x >> 15u;
            x *= 0x846CA68Bu;
            x ^= x >> 16u;
            return static_cast<f32>(x >> 8u) / static_cast<f32>(1u << 24u);
        }

        struct Strand
        {
            std::vector<glm::vec3> Points;
            f32 Diameter = 1.0e-4f;
            u16 Group = 0;
        };

        // A straight strand from `root` along `direction`, `points` points.
        [[nodiscard]] Strand MakeStrand(const glm::vec3& root, const glm::vec3& direction, f32 length, f32 diameter,
                                        u16 group, u32 points = 6u)
        {
            Strand strand;
            strand.Diameter = diameter;
            strand.Group = group;
            for (u32 p = 0; p < points; ++p)
            {
                strand.Points.push_back(root +
                                        direction * (length * static_cast<f32>(p) / static_cast<f32>(points - 1u)));
            }
            return strand;
        }

        // A random direction in the hemisphere about `axis`, cosine-weighted
        // toward it by `spread` (1 = uniform hemisphere, 0 = the axis itself).
        [[nodiscard]] glm::vec3 HemisphereDirection(const glm::vec3& axis, f32 spread, u32 a, u32 b)
        {
            const f32 u = Hash01(a, b * 2u + 1u);
            const f32 v = Hash01(a, b * 2u + 2u);
            const f32 cosTheta = 1.0f - spread * u;
            const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            const f32 phi = 6.2831853f * v;
            const glm::vec3 helper = std::abs(axis.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 t = glm::normalize(glm::cross(helper, axis));
            const glm::vec3 bt = glm::cross(axis, t);
            return glm::normalize(axis * cosTheta + (t * std::cos(phi) + bt * std::sin(phi)) * sinTheta);
        }

        // A patch of `count` strands rooted on a `size` square at y = 0, pointing
        // into the hemisphere about `axis` with `spread`.
        void AddPatch(std::vector<Strand>& strands, u32 count, const glm::vec3& corner, f32 size, const glm::vec3& axis,
                      f32 spread, f32 length, f32 diameter, u16 group, u32 salt)
        {
            for (u32 i = 0; i < count; ++i)
            {
                const glm::vec3 root =
                    corner + glm::vec3(size * Hash01(salt, i * 3u), 0.0f, size * Hash01(salt, i * 3u + 1u));
                const f32 l = length * (0.75f + 0.5f * Hash01(salt, i * 3u + 2u));
                strands.push_back(MakeStrand(root, HemisphereDirection(axis, spread, salt + 7u, i), l, diameter, group));
            }
        }

        struct Cast
        {
            std::vector<GroomStrandVertex> Vertices;
            std::vector<u32> Indices;
            std::vector<u32> StrandFirstIndex;
            std::vector<GroomCasterStrand> Summaries;
            GroomCasterOrder Runs;     // with summaries: one run per group
            GroomCasterOrder CoatWide; // without: the rule the runs replaced
            glm::vec3 BoundsMin{ 0.0f };
            glm::vec3 BoundsMax{ 0.0f };
            std::vector<u16> SegmentGroup; // by segment ordinal (vertex / 4)
            u32 Groups = 0;
        };

        [[nodiscard]] Cast BuildCast(const std::vector<Strand>& strands, const std::vector<std::string>& groups)
        {
            GroomBuilder builder;
            std::string reason;
            std::vector<u16> ids;
            for (const std::string& name : groups)
            {
                u16 id = 0;
                EXPECT_TRUE(builder.AddGroup(name, id, reason)) << reason;
                ids.push_back(id);
            }
            for (const Strand& strand : strands)
            {
                std::vector<f32> widths(strand.Points.size(), strand.Diameter);
                GroomCurveInput input;
                input.Points = strand.Points;
                input.Widths = widths;
                input.GroupId = ids[strand.Group];
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }
            const Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            Cast cast;
            cast.Groups = static_cast<u32>(groups.size());
            if (!groom)
            {
                return cast;
            }
            GroomStrandBuildSettings settings;
            settings.MaxStrands = static_cast<u32>(strands.size());
            const GroomStrandMeshStats stats =
                BuildGroomStrandMesh(GroomBuildSource::FromAsset(*groom), settings, cast.Vertices, cast.Indices, nullptr,
                                     nullptr, nullptr, &cast.StrandFirstIndex, &cast.Summaries);
            EXPECT_EQ(stats.StrandsSelected, static_cast<u32>(strands.size())) << "the cast must be the whole coat";
            cast.Runs = BuildGroomCasterOrder(cast.Vertices, cast.Indices, cast.StrandFirstIndex, cast.Summaries);
            cast.CoatWide = BuildGroomCasterOrder(cast.Vertices, cast.Indices, cast.StrandFirstIndex);
            EXPECT_EQ(cast.Runs.Runs.size(), groups.size()) << "one run per group";
            EXPECT_EQ(cast.CoatWide.Runs.size(), 1u);
            cast.BoundsMin = stats.BoundsMin;
            cast.BoundsMax = stats.BoundsMax;
            // Each segment's group, through the strand it belongs to.
            cast.SegmentGroup.assign(cast.Vertices.size() / 4u, 0u);
            for (sizet s = 0; s < cast.StrandFirstIndex.size(); ++s)
            {
                const sizet end = s + 1u < cast.StrandFirstIndex.size() ? cast.StrandFirstIndex[s + 1u] : cast.Indices.size();
                for (sizet i = cast.StrandFirstIndex[s]; i < end; i += 6u)
                {
                    cast.SegmentGroup[cast.Indices[i] / 4u] = strands[s].Group;
                }
            }
            return cast;
        }

        struct LightView
        {
            glm::mat4 ViewProjection{ 1.0f };
            f32 NdcPerWorld = 0.0f;
        };

        // An orthographic cascade of `extent` metres down `towardsLight`.
        [[nodiscard]] LightView MakeLight(const glm::vec3& towardsLight, const glm::vec3& centre,
                                          f32 extent = kExtentMetres)
        {
            const glm::vec3 l = glm::normalize(towardsLight);
            const glm::vec3 up = std::abs(l.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const f32 half = 0.5f * extent;
            LightView view;
            view.ViewProjection =
                glm::ortho(-half, half, -half, half, 0.1f, 100.0f) * glm::lookAt(centre + l * 20.0f, centre, up);
            view.NdcPerWorld = GroomShadowNdcPerWorld(view.ViewProjection, 1.0f);
            return view;
        }

        // What the shadow pass decides for one order in one view: each run's
        // share through DecideGroomCasterRun, exactly as ShadowRenderPass asks,
        // with the coat as an unbound groom at the origin.
        struct Range
        {
            u32 First = 0;
            u32 Count = 0;
        };

        struct Decision
        {
            std::vector<Range> Ranges;
            std::vector<GroomCasterRunDecision> Runs;
            u32 IndexCount = 0;
            u32 WholeCount = 0;
        };

        [[nodiscard]] Decision DecideRuns(const Cast& cast, std::span<const GroomCasterRun> runs, const LightView& light,
                                          f32 resolution = kResolution, const glm::mat4& transform = glm::mat4(1.0f),
                                          f32 objectScale = 1.0f)
        {
            GroomCasterPlacement placement;
            placement.Transform = transform;
            placement.ObjectScale = objectScale;
            placement.MinWidthTexels = kMinWidthTexels;
            placement.CullMin = cast.BoundsMin;
            placement.CullMax = cast.BoundsMax;
            GroomCasterView view;
            view.ViewProjection = light.ViewProjection;
            view.ResolutionTexels = resolution;
            Decision d;
            for (const GroomCasterRun& run : runs)
            {
                const GroomCasterRunDecision decision = DecideGroomCasterRun(run, placement, view);
                d.Runs.push_back(decision);
                d.Ranges.push_back({ run.FirstIndex, decision.IndexCount });
                d.IndexCount += decision.IndexCount;
                d.WholeCount += run.Prefix[kGroomCasterPrefixLevels];
            }
            return d;
        }

        [[nodiscard]] Decision Decide(const Cast& cast, const GroomCasterOrder& order, const LightView& light,
                                      f32 resolution = kResolution)
        {
            return DecideRuns(cast, order.Runs, light, resolution);
        }

        // The model depth map, per group: how many ribbons cover each texel
        // centre, and the true coverage they carry there.
        struct Grid
        {
            i32 X0 = 0;
            i32 Y0 = 0;
            i32 Width = 0;
            i32 Height = 0;
            std::vector<std::vector<u16>> Count; // [group][texel]
            std::vector<std::vector<f32>> Truth; // [group][texel], sum of true/floored width
        };

        [[nodiscard]] glm::vec2 ToTexel(const LightView& view, const glm::vec3& p, f32 resolution)
        {
            const glm::vec4 clip = view.ViewProjection * glm::vec4(p, 1.0f);
            return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * resolution;
        }

        [[nodiscard]] Grid MakeGrid(const Cast& cast, const LightView& view, f32 resolution)
        {
            glm::vec2 lo{ 1.0e30f };
            glm::vec2 hi{ -1.0e30f };
            for (u32 c = 0; c < 8u; ++c)
            {
                const glm::vec3 p{ (c & 1u) ? cast.BoundsMax.x : cast.BoundsMin.x,
                                   (c & 2u) ? cast.BoundsMax.y : cast.BoundsMin.y,
                                   (c & 4u) ? cast.BoundsMax.z : cast.BoundsMin.z };
                const glm::vec2 t = ToTexel(view, p, resolution);
                lo = glm::min(lo, t);
                hi = glm::max(hi, t);
            }
            Grid grid;
            grid.X0 = static_cast<i32>(std::floor(lo.x)) - 4;
            grid.Y0 = static_cast<i32>(std::floor(lo.y)) - 4;
            grid.Width = static_cast<i32>(std::ceil(hi.x)) + 4 - grid.X0;
            grid.Height = static_cast<i32>(std::ceil(hi.y)) + 4 - grid.Y0;
            const sizet texels = static_cast<sizet>(grid.Width) * static_cast<sizet>(grid.Height);
            grid.Count.assign(cast.Groups, std::vector<u16>(texels, 0u));
            grid.Truth.assign(cast.Groups, std::vector<f32>(texels, 0.0f));
            return grid;
        }

        // Draws `ranges` of `order` as the depth shader does: each segment a
        // ribbon from P0 to P1, the floored width across it and NOTHING past
        // its ends (GroomShadowWidening.glsl has no caps).
        void Rasterize(const Cast& cast, const GroomCasterOrder& order, std::span<const Range> ranges,
                       const LightView& view, Grid& grid, f32 resolution)
        {
            for (const Range& range : ranges)
            {
                for (u32 i = range.First; i + 5u < range.First + range.Count; i += 6u)
                {
                    const u32 base = order.Indices[i] / 4u * 4u;
                    const GroomStrandVertex& v0 = cast.Vertices[base];
                    const glm::vec2 a = ToTexel(view, v0.Position, resolution) - glm::vec2(grid.X0, grid.Y0);
                    const glm::vec2 b = ToTexel(view, v0.Other, resolution) - glm::vec2(grid.X0, grid.Y0);
                    const glm::vec2 ab = b - a;
                    const f32 ab2 = glm::dot(ab, ab);
                    if (!(ab2 > 1.0e-12f))
                    {
                        continue; // a segment along the light: a zero-area quad
                    }
                    const f32 radius = 0.5f * (v0.Radius + cast.Vertices[base + 2u].Radius);
                    const f32 flooredHalfNdc =
                        GroomShadowHalfWidthNdc(radius, view.NdcPerWorld, resolution, kMinWidthTexels);
                    const f32 half = flooredHalfNdc * 0.5f * resolution;
                    const f32 truthWeight = std::min(1.0f, radius * view.NdcPerWorld / flooredHalfNdc);
                    const u32 group = cast.SegmentGroup[base / 4u];
                    std::vector<u16>& count = grid.Count[group];
                    std::vector<f32>& truth = grid.Truth[group];
                    const glm::vec2 lo = glm::min(a, b) - half;
                    const glm::vec2 hi = glm::max(a, b) + half;
                    const i32 y0 = std::max(0, static_cast<i32>(std::floor(lo.y)));
                    const i32 y1 = std::min(grid.Height - 1, static_cast<i32>(std::ceil(hi.y)));
                    const i32 x0 = std::max(0, static_cast<i32>(std::floor(lo.x)));
                    const i32 x1 = std::min(grid.Width - 1, static_cast<i32>(std::ceil(hi.x)));
                    for (i32 y = y0; y <= y1; ++y)
                    {
                        for (i32 x = x0; x <= x1; ++x)
                        {
                            const glm::vec2 p{ static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f };
                            const f32 t = glm::dot(p - a, ab) / ab2;
                            if (t < 0.0f || t > 1.0f)
                            {
                                continue;
                            }
                            const glm::vec2 d = p - (a + ab * t);
                            if (glm::dot(d, d) <= half * half)
                            {
                                const sizet k = static_cast<sizet>(y) * static_cast<sizet>(grid.Width) + static_cast<sizet>(x);
                                count[k] = static_cast<u16>(std::min<u32>(count[k] + 1u, 0xFFFFu));
                                truth[k] += truthWeight;
                            }
                        }
                    }
                }
            }
        }

        // One region's numbers: a group alone, or every group together.
        struct Region
        {
            u64 Covered = 0;             // texels the whole cast covers
            f64 ActualLayers = 0.0;      // mean whole ribbon count over them
            f64 KeptLayers = 0.0;        // mean subset ribbon count over them
            f64 Leak = 0.0;              // mean max(0, truth - subset), filtered
            f64 Truth = 0.0;             // mean real-coat opacity, filtered
            f64 Subset = 0.0;            // mean subset occupancy, filtered
            f64 Whole = 0.0;             // mean whole-cast occupancy, filtered
            f64 DifferenceVsWhole = 0.0; // mean |subset - whole|, filtered
            u64 Interior = 0;            // texels whose 3x3 the whole cast fills
            u64 InteriorOpen = 0;        // of those, the ones the subset leaves open
            [[nodiscard]] f64 InteriorHoles() const
            {
                return Interior > 0u ? static_cast<f64>(InteriorOpen) / static_cast<f64>(Interior) : 0.0;
            }
        };

        [[nodiscard]] Region Evaluate(const Grid& whole, const Grid& subset, i32 group)
        {
            const sizet texels = static_cast<sizet>(whole.Width) * static_cast<sizet>(whole.Height);
            std::vector<u32> wholeCount(texels, 0u);
            std::vector<u32> subsetCount(texels, 0u);
            std::vector<f32> truth(texels, 0.0f);
            for (u32 g = 0; g < whole.Count.size(); ++g)
            {
                if (group >= 0 && static_cast<i32>(g) != group)
                {
                    continue;
                }
                for (sizet k = 0; k < texels; ++k)
                {
                    wholeCount[k] += whole.Count[g][k];
                    subsetCount[k] += subset.Count[g][k];
                    truth[k] += whole.Truth[g][k];
                }
            }
            // The receiver's view of each map: occupancy, or the real coat's
            // opacity, averaged over 3x3 like a PCF kernel.
            const auto filtered = [&](const auto& value)
            {
                std::vector<f32> out(texels, 0.0f);
                for (i32 y = 1; y + 1 < whole.Height; ++y)
                {
                    for (i32 x = 1; x + 1 < whole.Width; ++x)
                    {
                        f32 sum = 0.0f;
                        for (i32 dy = -1; dy <= 1; ++dy)
                        {
                            for (i32 dx = -1; dx <= 1; ++dx)
                            {
                                sum += value(static_cast<sizet>(y + dy) * static_cast<sizet>(whole.Width) +
                                             static_cast<sizet>(x + dx));
                            }
                        }
                        out[static_cast<sizet>(y) * static_cast<sizet>(whole.Width) + static_cast<sizet>(x)] = sum / 9.0f;
                    }
                }
                return out;
            };
            const std::vector<f32> wholeF = filtered([&](sizet k)
                                                     { return wholeCount[k] > 0u ? 1.0f : 0.0f; });
            const std::vector<f32> subsetF = filtered([&](sizet k)
                                                      { return subsetCount[k] > 0u ? 1.0f : 0.0f; });
            const std::vector<f32> truthF = filtered([&](sizet k)
                                                     { return 1.0f - std::exp(-truth[k]); });

            Region region;
            for (sizet k = 0; k < texels; ++k)
            {
                if (wholeCount[k] == 0u)
                {
                    continue;
                }
                ++region.Covered;
                region.ActualLayers += wholeCount[k];
                region.KeptLayers += subsetCount[k];
                region.Leak += std::max(0.0, static_cast<f64>(truthF[k]) - static_cast<f64>(subsetF[k]));
                region.Truth += truthF[k];
                region.Subset += subsetF[k];
                region.Whole += wholeF[k];
                region.DifferenceVsWhole += std::abs(static_cast<f64>(subsetF[k]) - static_cast<f64>(wholeF[k]));
                // The whole cast fills this texel's 3x3: its solid interior.
                if (wholeF[k] > 0.999f)
                {
                    ++region.Interior;
                    region.InteriorOpen += subsetCount[k] == 0u ? 1u : 0u;
                }
            }
            if (region.Covered > 0u)
            {
                const f64 n = static_cast<f64>(region.Covered);
                region.ActualLayers /= n;
                region.KeptLayers /= n;
                region.Leak /= n;
                region.Truth /= n;
                region.Subset /= n;
                region.Whole /= n;
                region.DifferenceVsWhole /= n;
            }
            return region;
        }

        struct RuleResult
        {
            Decision D;
            Region All;
            std::vector<Region> ByGroup;
        };

        struct Measured
        {
            RuleResult Runs;
            RuleResult CoatWide;
        };

        [[nodiscard]] RuleResult MeasureRule(const Cast& cast, const GroomCasterOrder& order, const LightView& view,
                                             const Grid& whole, f32 resolution)
        {
            RuleResult result;
            result.D = Decide(cast, order, view, resolution);
            Grid subset = whole;
            for (auto& count : subset.Count)
            {
                std::ranges::fill(count, u16{ 0 });
            }
            Rasterize(cast, order, result.D.Ranges, view, subset, resolution);
            result.All = Evaluate(whole, subset, -1);
            for (u32 g = 0; g < cast.Groups; ++g)
            {
                result.ByGroup.push_back(Evaluate(whole, subset, static_cast<i32>(g)));
            }
            return result;
        }

        // The rule's subset of `drawn` (a coat built with the same strands, the
        // same points and so the same indices as the one decided from) against
        // the whole of it, per group: what one decision leaves of a coat it did
        // not measure.
        [[nodiscard]] RuleResult MeasureDecision(const Cast& drawn, const GroomCasterOrder& order, const Decision& decision,
                                                 const LightView& view, f32 resolution = kResolution)
        {
            Grid whole = MakeGrid(drawn, view, resolution);
            const Range everything{ 0u, static_cast<u32>(order.Indices.size()) };
            Rasterize(drawn, order, std::span<const Range>(&everything, 1u), view, whole, resolution);
            RuleResult result;
            result.D = decision;
            Grid subset = whole;
            for (auto& count : subset.Count)
            {
                std::ranges::fill(count, u16{ 0 });
            }
            Rasterize(drawn, order, result.D.Ranges, view, subset, resolution);
            result.All = Evaluate(whole, subset, -1);
            for (u32 g = 0; g < drawn.Groups; ++g)
            {
                result.ByGroup.push_back(Evaluate(whole, subset, static_cast<i32>(g)));
            }
            return result;
        }

        // `strands` with every point turned by `rotation` about the strand's
        // root -- a body part carrying its fur -- and widened by `widthScale`.
        [[nodiscard]] std::vector<Strand> Turned(const std::vector<Strand>& strands, const glm::mat3& rotation,
                                                 f32 widthScale = 1.0f)
        {
            std::vector<Strand> out = strands;
            for (Strand& strand : out)
            {
                const glm::vec3 root = strand.Points.front();
                for (glm::vec3& point : strand.Points)
                {
                    point = root + (rotation * (point - root));
                }
                strand.Diameter *= widthScale;
            }
            return out;
        }

        [[nodiscard]] Measured Measure(const Cast& cast, const LightView& view, f32 resolution = kResolution)
        {
            Grid whole = MakeGrid(cast, view, resolution);
            const Range everything{ 0u, static_cast<u32>(cast.CoatWide.Indices.size()) };
            Rasterize(cast, cast.CoatWide, std::span<const Range>(&everything, 1u), view, whole, resolution);
            Measured m;
            m.Runs = MeasureRule(cast, cast.Runs, view, whole, resolution);
            m.CoatWide = MeasureRule(cast, cast.CoatWide, view, whole, resolution);
            return m;
        }

        void ReportRule(const char* name, const char* rule, const RuleResult& r)
        {
            std::printf("[caster-coverage] %-34s %-9s casts %5.1f%% of the coat's indices\n", name, rule,
                        100.0 * static_cast<f64>(r.D.IndexCount) / static_cast<f64>(std::max<u32>(1u, r.D.WholeCount)));
            for (sizet g = 0; g <= r.ByGroup.size(); ++g)
            {
                const Region& region = g < r.ByGroup.size() ? r.ByGroup[g] : r.All;
                const GroomCasterRunDecision* run = g < r.D.Runs.size() ? &r.D.Runs[g] : nullptr;
                std::printf("[caster-coverage]     %-6s", g < r.ByGroup.size() ? ("group" + std::to_string(g)).c_str() : "all");
                if (run != nullptr)
                {
                    std::printf(" est layers %6.2f fraction %.3f |", run->Layers, run->Fraction);
                }
                else
                {
                    std::printf("                                |");
                }
                std::printf(" layers whole %6.2f kept %6.2f | opacity real %.3f subset %.3f whole %.3f | leak %.4f | "
                            "vs whole: interior holes %.4f, mean diff %.4f (%llu px)\n",
                            region.ActualLayers, region.KeptLayers, region.Truth, region.Subset, region.Whole,
                            region.Leak, region.InteriorHoles(), region.DifferenceVsWhole,
                            static_cast<unsigned long long>(region.Covered));
            }
            std::fflush(stdout);
        }

        void Report(const char* name, const Measured& m)
        {
            ReportRule(name, "runs", m.Runs);
            ReportRule(name, "coat-wide", m.CoatWide);
        }

        // What the rule claims for one region: the subset keeps the region's
        // own layers up to kGroomCasterMinLayers, and lets through no more light
        // than the real coat would.
        [[nodiscard]] bool KeepsItsLayers(const Region& region)
        {
            return region.KeptLayers >= std::min<f64>(kGroomCasterMinLayers, region.ActualLayers) * kLayerTolerance;
        }

        void ExpectHoldsTheClaim(const Region& region, const std::string& where)
        {
            EXPECT_TRUE(KeepsItsLayers(region)) << where << ": kept " << region.KeptLayers << " of "
                                                << region.ActualLayers << " layers";
            EXPECT_LE(region.Leak, kLeakBudget) << where << ": the thinned map lets through light the coat blocks";
        }
    } // namespace

    // The case the share was built for: a dense coat of fine strands lying every
    // which way, one group. Both rules hold it.
    TEST(GroomCasterCoverage, ADenseCoatLyingEveryWhichWayStaysSolid)
    {
        std::vector<Strand> strands;
        AddPatch(strands, 24000u, glm::vec3(0.0f), 0.12f, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.03f, 1.0e-4f, 0u, 11u);
        const Cast cast = BuildCast(strands, { "body_undercoat" });
        const Measured m = Measure(cast, MakeLight({ 0.3f, 1.0f, 0.2f }, glm::vec3(0.06f, 0.0f, 0.06f)));
        Report("dense, every which way", m);
        EXPECT_LT(m.Runs.D.Runs[0].Fraction, 1.0f) << "a dense coat must thin, or this case measures nothing";
        ExpectHoldsTheClaim(m.Runs.All, "runs");
        ExpectHoldsTheClaim(m.CoatWide.All, "coat-wide");
    }

    // Strands pointing ALONG the light project to dots -- with no end caps, to
    // nothing -- not to lines: an assumed half share of their length credits
    // them with layers they do not lay, and the coat-wide rule thins them past
    // what they have. The moment bound counts what they project to.
    TEST(GroomCasterCoverage, StrandsAlongTheLightAreNotCountedAsLongShadows)
    {
        std::vector<Strand> strands;
        // Hanging fur under an overhead sun: within ten degrees of the light.
        AddPatch(strands, 24000u, glm::vec3(0.0f), 0.12f, glm::vec3(0.0f, 1.0f, 0.0f), 0.015f, 0.03f, 1.0e-4f, 0u, 23u);
        const Cast cast = BuildCast(strands, { "body_undercoat" });
        const Measured m = Measure(cast, MakeLight({ 0.0f, 1.0f, 0.0f }, glm::vec3(0.06f, 0.0f, 0.06f)));
        Report("along the light", m);
        ExpectHoldsTheClaim(m.Runs.All, "runs");
        EXPECT_FALSE(KeepsItsLayers(m.CoatWide.All))
            << "the coat-wide rule kept this coat's layers: the case no longer tests the projected length";
    }

    // A coat-wide average says nothing about a REGION: a dense patch dominates
    // it, and a sparse plume beside it is thinned to the patch's share. A run
    // per group gives the plume its own.
    TEST(GroomCasterCoverage, ASparsePlumeBesideADensePatchKeepsItsOwnShadow)
    {
        std::vector<Strand> strands;
        AddPatch(strands, 24000u, glm::vec3(0.0f), 0.12f, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.03f, 1.0e-4f, 0u, 31u);
        // The plume: few long strands combed along the ground beside it.
        AddPatch(strands, 120u, glm::vec3(0.14f, 0.0f, 0.0f), 0.10f, glm::normalize(glm::vec3(1.0f, 0.15f, 0.0f)), 0.2f,
                 0.15f, 1.5e-4f, 1u, 37u);
        const Cast cast = BuildCast(strands, { "body_undercoat", "tail_longhair" });
        const Measured m = Measure(cast, MakeLight({ 0.2f, 1.0f, 0.1f }, glm::vec3(0.12f, 0.0f, 0.06f)));
        Report("dense patch beside a sparse plume", m);
        ExpectHoldsTheClaim(m.Runs.ByGroup[0], "runs, patch");
        ExpectHoldsTheClaim(m.Runs.ByGroup[1], "runs, plume");
        ExpectHoldsTheClaim(m.Runs.All, "runs, all");
        EXPECT_FALSE(KeepsItsLayers(m.CoatWide.ByGroup[1]))
            << "the coat-wide rule kept the plume's layers: the case no longer tests a region";
    }

    // Thick strands keep their own width and are not over-covered by the floor,
    // so thinning them removes real coverage; a coat-wide mean radius dominated
    // by a thin majority thins them anyway. A run per group keeps each its own.
    TEST(GroomCasterCoverage, MixedWidthsAndLengthsStaySolidInEveryGroup)
    {
        std::vector<Strand> strands;
        AddPatch(strands, 24000u, glm::vec3(0.0f), 0.12f, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.02f, 0.8e-4f, 0u, 41u);
        AddPatch(strands, 400u, glm::vec3(0.0f), 0.12f, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.06f, 3.0e-3f, 1u, 43u);
        const Cast cast = BuildCast(strands, { "body_undercoat", "body_guard" });
        const Measured m = Measure(cast, MakeLight({ 0.4f, 1.0f, 0.3f }, glm::vec3(0.06f, 0.0f, 0.06f)));
        Report("mixed widths and lengths", m);
        ExpectHoldsTheClaim(m.Runs.ByGroup[0], "runs, thin undercoat");
        ExpectHoldsTheClaim(m.Runs.ByGroup[1], "runs, thick guard hair");
        ExpectHoldsTheClaim(m.Runs.All, "runs, all");
        EXPECT_GT(m.CoatWide.ByGroup[1].Leak, kLeakBudget)
            << "the coat-wide rule let no light through the thick strands: the case no longer tests the widths";
    }

    // #1533: the share follows the POSE. A run lying across an overhead light
    // at rest, turned by its body part until it hangs within ten degrees of
    // the light: its rest moments credit it with the layers it lays lying
    // flat, and a view decided from them thins it into holes. Re-posed
    // (PoseGroomCasterRuns -- its sample turned, its moments scaled to the run)
    // the decision counts what the turned run projects to, holds the claim on
    // the turned coat, and its moments agree with the turned coat's own.
    TEST(GroomCasterCoverage, ARunItsBodyTurnsAlongTheLightIsDecidedFromItsPose)
    {
        std::vector<Strand> rest;
        AddPatch(rest, 24000u, glm::vec3(0.0f), 0.12f, glm::normalize(glm::vec3(1.0f, 0.05f, 0.0f)), 0.05f, 0.03f,
                 1.0e-4f, 0u, 71u);
        const glm::mat3 turn = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(80.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
        const Cast restCast = BuildCast(rest, { "leg_guard" });
        const Cast turnedCast = BuildCast(Turned(rest, turn), { "leg_guard" });
        ASSERT_EQ(restCast.Indices, turnedCast.Indices) << "the turned coat must be the same strands, indexed alike";

        std::vector<u32> strandOrder;
        const GroomCasterOrder order = BuildGroomCasterOrder(restCast.Vertices, restCast.Indices,
                                                             restCast.StrandFirstIndex, restCast.Summaries, &strandOrder);
        const GroomCasterPose pose = BuildGroomCasterPose(order.Runs, strandOrder, restCast.Summaries);
        ASSERT_TRUE(pose.IsUsable());
        std::vector<GroomCasterRun> posed;
        ASSERT_TRUE(PoseGroomCasterRuns(
            order.Runs, pose, [&turn](const GroomCasterPoseSample&)
            { return turn; },
            [&restCast](sizet, glm::vec3& lo, glm::vec3& hi)
            {
                // The roots stay where they are: the turn is about each root.
                lo = glm::vec3(restCast.BoundsMin.x, 0.0f, restCast.BoundsMin.z);
                hi = glm::vec3(restCast.BoundsMax.x, 0.0f, restCast.BoundsMax.z);
                return true;
            },
            1.0f, GroomCasterPosePadding{}, posed));

        // The estimate against the turned coat's own moments, measured whole.
        const GroomCasterOrder truth =
            BuildGroomCasterOrder(turnedCast.Vertices, turnedCast.Indices, turnedCast.StrandFirstIndex, turnedCast.Summaries);
        ASSERT_EQ(truth.Runs.size(), posed.size());
        const f32 trace = truth.Runs[0].Moments[0] + truth.Runs[0].Moments[1] + truth.Runs[0].Moments[2];
        for (sizet m = 0; m < 6u; ++m)
        {
            EXPECT_NEAR(posed[0].Moments[m], truth.Runs[0].Moments[m], 0.03f * trace)
                << "moment " << m << ": the sample's estimate is not the turned run's";
        }

        const LightView light = MakeLight({ 0.0f, 1.0f, 0.0f }, glm::vec3(0.06f, 0.0f, 0.06f));
        const RuleResult fromRest = MeasureDecision(turnedCast, order, DecideRuns(turnedCast, order.Runs, light), light);
        const RuleResult fromPose = MeasureDecision(turnedCast, order, DecideRuns(turnedCast, posed, light), light);
        ReportRule("turned along the light", "rest", fromRest);
        ReportRule("turned along the light", "posed", fromPose);
        ExpectHoldsTheClaim(fromPose.All, "posed");
        EXPECT_FALSE(KeepsItsLayers(fromRest.All) && fromRest.All.Leak <= kLeakBudget)
            << "the rest runs held the claim on the turned coat: the case no longer tests the pose";
    }

    // #1533: the share measures ANY linear transform by its smallest stretch.
    // Strands combed along X under an overhead light, the caster squashed to a
    // fifth along X: in the world they are a fifth as long and lay a fifth of
    // the layers. The transform's transpose and mean axis -- what the decision
    // used before -- credit them with nearly four times that length and thin
    // them into holes; the smallest stretch, through the inverse, does not.
    TEST(GroomCasterCoverage, ASquashedCasterIsMeasuredByItsSmallestStretch)
    {
        std::vector<Strand> local;
        // Sparse enough that the squashed run lays a handful of layers: the
        // share then rests on the layer estimate, not on the floor's.
        AddPatch(local, 800u, glm::vec3(0.0f), 0.12f, glm::normalize(glm::vec3(1.0f, 0.04f, 0.0f)), 0.05f, 0.03f,
                 1.0e-4f, 0u, 79u);
        const glm::mat4 squash = glm::scale(glm::mat4(1.0f), glm::vec3(0.2f, 1.0f, 1.0f));
        const f32 meanScale = (0.2f + 1.0f + 1.0f) / 3.0f;
        // What the GPU draws: every point under the transform, every width under
        // the shader's one mean scale.
        std::vector<Strand> world = local;
        for (Strand& strand : world)
        {
            for (glm::vec3& point : strand.Points)
            {
                point = glm::vec3(squash * glm::vec4(point, 1.0f));
            }
            strand.Diameter *= meanScale;
        }
        const Cast localCast = BuildCast(local, { "body_guard" });
        const Cast worldCast = BuildCast(world, { "body_guard" });
        ASSERT_EQ(localCast.Indices, worldCast.Indices);

        const LightView light = MakeLight({ 0.0f, 1.0f, 0.0f }, glm::vec3(0.012f, 0.0f, 0.06f));
        const Decision exact = DecideRuns(worldCast, localCast.Runs.Runs, light, kResolution, squash, meanScale);
        // THE CONTROL: the same decision with the strands credited the mean
        // scale's length -- what the transform's transpose and mean axis made
        // of them -- over the same box.
        std::vector<GroomCasterRun> credited = localCast.Runs.Runs;
        for (GroomCasterRun& run : credited)
        {
            const f32 k = meanScale / 0.2f;
            run.TotalLength *= k;
            for (f32& moment : run.Moments)
            {
                moment *= k;
            }
        }
        const Decision similar = DecideRuns(worldCast, credited, light, kResolution, squash, meanScale);
        const RuleResult fromExact = MeasureDecision(worldCast, localCast.Runs, exact, light);
        const RuleResult fromSimilar = MeasureDecision(worldCast, localCast.Runs, similar, light);
        ReportRule("squashed to a fifth along X", "stretch", fromExact);
        ReportRule("squashed to a fifth along X", "mean axis", fromSimilar);
        EXPECT_NEAR(exact.Runs[0].ProjectedLength, 0.2f * localCast.Runs.Runs[0].TotalLength,
                    0.05f * localCast.Runs.Runs[0].TotalLength)
            << "strands along the squashed axis project to a fifth of their length";
        ExpectHoldsTheClaim(fromExact.All, "smallest stretch");
        EXPECT_FALSE(KeepsItsLayers(fromSimilar.All) && fromSimilar.All.Leak <= kLeakBudget)
            << "crediting the mean axis held the claim: the case no longer tests the transform";
    }

    // #1533: under a PERSPECTIVE light the rays fan across a run, and a strand
    // along any of them lays nothing there. Upright strands over a wide patch,
    // a lamp just above one corner: the rays near it run along the strands,
    // the ray to the patch's centre crosses them. The decision's projected
    // length must be at most the moment bound along EVERY ray into the run's
    // box, not just the centre's -- checked against a thousand of them.
    TEST(GroomCasterCoverage, APerspectiveLightIsMeasuredAlongEveryRayIntoTheRun)
    {
        std::vector<Strand> strands;
        AddPatch(strands, 6000u, glm::vec3(-1.0f, 0.0f, -1.0f), 2.0f, glm::vec3(0.0f, 1.0f, 0.0f), 0.02f, 0.05f, 1.0e-4f,
                 0u, 83u);
        const Cast cast = BuildCast(strands, { "back_undercoat" });
        const glm::vec3 lamp(-0.9f, 0.8f, -0.9f);
        LightView light;
        light.ViewProjection = glm::perspective(glm::radians(150.0f), 1.0f, 0.05f, 20.0f) *
                               glm::lookAt(lamp, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        light.NdcPerWorld = GroomShadowNdcPerWorld(light.ViewProjection, 1.0f);
        const Decision d = DecideRuns(cast, cast.Runs.Runs, light);
        const GroomCasterRun& run = cast.Runs.Runs[0];
        ASSERT_TRUE(run.MomentsKnown);
        ASSERT_GT(d.Runs[0].ProjectedLength, 0.0f) << "the box met the lamp's plane, so nothing was measured";

        f32 alongEveryRay = std::numeric_limits<f32>::max();
        for (u32 k = 0; k < 1000u; ++k)
        {
            const glm::vec3 t{ Hash01(97u, k * 3u), Hash01(97u, k * 3u + 1u), Hash01(97u, k * 3u + 2u) };
            const glm::vec3 point = run.BoundsMin + (t * (run.BoundsMax - run.BoundsMin));
            const glm::vec3 ray = GroomShadowProjectionDirection(light.ViewProjection, point);
            alongEveryRay = std::min(alongEveryRay, GroomShadowProjectedLengthLowerBound(run.TotalLength, run.Moments, ray));
        }
        const glm::vec3 centreRay = GroomShadowProjectionDirection(light.ViewProjection, 0.5f * (run.BoundsMin + run.BoundsMax));
        const f32 alongTheCentre = GroomShadowProjectedLengthLowerBound(run.TotalLength, run.Moments, centreRay);
        std::printf("[caster-coverage] perspective lamp over a wide patch: decided %.4f, along every ray >= %.4f, along "
                    "the centre's %.4f (of %.4f)\n",
                    d.Runs[0].ProjectedLength, alongEveryRay, alongTheCentre, run.TotalLength);
        std::fflush(stdout);
        EXPECT_LE(d.Runs[0].ProjectedLength, alongEveryRay * 1.001f + 1.0e-6f)
            << "the decision credited the run with more projected length than one of its own rays sees";
        EXPECT_GT(alongTheCentre, 2.0f * alongEveryRay)
            << "the centre's ray sees about what every ray does: the case no longer tests the fan";
    }

    // A slowly turning light changes each run's share a little each step, and
    // a prefix count moves in sixty-fourths. At EVERY step the claim holds, and
    // no step opens or closes more of a group's solid interior than the
    // temporal budget -- a batch of strands dropping at once is a flicker.
    TEST(GroomCasterCoverage, ATurningLightKeepsTheClaimAtEveryStepAndChangesLittleBetweenThem)
    {
        std::vector<Strand> strands;
        AddPatch(strands, 24000u, glm::vec3(0.0f), 0.12f, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.03f, 1.0e-4f, 0u, 53u);
        AddPatch(strands, 120u, glm::vec3(0.14f, 0.0f, 0.0f), 0.10f, glm::normalize(glm::vec3(1.0f, 0.15f, 0.0f)), 0.2f,
                 0.15f, 1.5e-4f, 1u, 59u);
        const Cast cast = BuildCast(strands, { "body_undercoat", "tail_longhair" });
        const glm::vec3 centre(0.12f, 0.0f, 0.06f);
        f64 worstJump = 0.0;
        f64 worstLeak = 0.0;
        f64 worstKeptShare = 1.0e9;
        u32 shareChanges = 0;
        std::vector<f64> previousHoles;
        std::vector<f32> previousFractions;
        for (u32 step = 0; step <= 40u; ++step)
        {
            const f32 angle = glm::radians(10.0f + 1.5f * static_cast<f32>(step));
            const Measured m = Measure(cast, MakeLight({ std::sin(angle), std::cos(angle), 0.15f }, centre));
            std::vector<f64> holes;
            std::vector<f32> fractions;
            for (u32 g = 0; g < cast.Groups; ++g)
            {
                const Region& region = m.Runs.ByGroup[g];
                ExpectHoldsTheClaim(region, "step " + std::to_string(step) + " group " + std::to_string(g));
                worstLeak = std::max(worstLeak, region.Leak);
                worstKeptShare = std::min(worstKeptShare,
                                          region.KeptLayers /
                                              std::max(1.0e-9, std::min<f64>(kGroomCasterMinLayers, region.ActualLayers)));
                holes.push_back(region.InteriorHoles());
                fractions.push_back(m.Runs.D.Runs[g].Fraction);
            }
            for (sizet g = 0; g < previousHoles.size(); ++g)
            {
                worstJump = std::max(worstJump, std::abs(holes[g] - previousHoles[g]));
                shareChanges += std::abs(fractions[g] - previousFractions[g]) > 1.0e-6f ? 1u : 0u;
            }
            previousHoles = holes;
            previousFractions = fractions;
        }
        std::printf("[caster-coverage] turning light, 60 degrees in 1.5-degree steps: worst leak %.4f, worst kept/claimed "
                    "layers %.3f, worst step change of interior holes %.4f, %u run share changes\n",
                    worstLeak, worstKeptShare, worstJump, shareChanges);
        std::fflush(stdout);
        EXPECT_LE(worstJump, kStepBudget) << "a share step opened or closed a batch of a group's interior at once";
    }

    // Across a cascade boundary the texel size changes and so does each run's
    // share. A prefix makes every larger share a SUPERSET of a smaller one, so a
    // coat moving to a finer cascade only gains strands -- whole strands, in the
    // same hashed order -- and never trades one set for another, in every run.
    TEST(GroomCasterCoverage, ACascadeChangeOnlyAddsOrRemovesWholeStrandsInOneOrderPerRun)
    {
        std::vector<Strand> strands;
        AddPatch(strands, 24000u, glm::vec3(0.0f), 0.12f, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f, 0.03f, 1.0e-4f, 0u, 61u);
        AddPatch(strands, 2000u, glm::vec3(0.14f, 0.0f, 0.0f), 0.10f, glm::normalize(glm::vec3(1.0f, 0.15f, 0.0f)),
                 0.2f, 0.15f, 1.5e-4f, 1u, 67u);
        const Cast cast = BuildCast(strands, { "body_undercoat", "tail_longhair" });
        const glm::vec3 centre(0.12f, 0.0f, 0.06f);
        std::vector<u32> previous(cast.Runs.Runs.size(), 0u);
        for (const f32 extent : { 64.0f, 32.0f, 16.0f, 8.0f, 4.0f })
        {
            const Decision d = Decide(cast, cast.Runs, MakeLight({ 0.3f, 1.0f, 0.2f }, centre, extent));
            for (sizet r = 0; r < d.Runs.size(); ++r)
            {
                std::printf("[caster-coverage] cascade %5.1f m run %zu: est layers %8.2f fraction %.3f indices %u\n",
                            extent, r, d.Runs[r].Layers, d.Runs[r].Fraction, d.Runs[r].IndexCount);
                EXPECT_GE(d.Runs[r].IndexCount, previous[r])
                    << "run " << r << ": a finer cascade cast fewer strands than a coarser one";
                EXPECT_EQ(d.Runs[r].IndexCount % 6u, 0u) << "a prefix cut a strand's segment";
                previous[r] = d.Runs[r].IndexCount;
            }
        }
        std::fflush(stdout);
    }
} // namespace OloEngine::Tests
