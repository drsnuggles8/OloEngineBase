#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include "OloEngine/Groom/GroomAsset.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_map>

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
        //
        // WHY THE STRIDE IS PER ROLE (issue #1251). One stride over everything
        // removes the same FRACTION of every coat layer, so a budget that halves
        // a coat also halves the sparse guard hairs that draw its outline — and
        // the animal loses its silhouette before it loses any of the fuzz. The
        // per-role strides below are solved so that the retained fraction of a
        // role is proportional to GroomCoatBudgetWeight(role), saturated at 1.
        // On a groom with no coat authoring every curve is Unassigned, every
        // weight is the same, and the solution is the single stride this code
        // computed before the roles existed.
        struct Selection
        {
            std::array<u32, GroomCoatRoleCount> Stride{};
            std::array<u32, GroomCoatRoleCount> Available{};
            u32 Selected = 0;
            u32 AvailableTotal = 0;
            u32 DroppedByCoat = 0;
            bool SegmentBudgetLimited = false;
            /// True when any group asks for clumping, so the build knows whether
            /// the clump prepass is worth a second walk over the groom.
            bool WantsClumping = false;

            Selection() noexcept
            {
                Stride.fill(1u);
            }
        };

        // A per-role counter, so "every Nth strand of this role" is a rule the
        // selection, the plan and the build all apply identically. They must:
        // a selection that disagreed with the build by one curve would deform a
        // strand that is not drawn and draw a strand that was not deformed.
        struct RoleWalk
        {
            std::array<u32, GroomCoatRoleCount> Taken{};

            [[nodiscard]] bool Take(GroomCoatRole role, const Selection& selection) noexcept
            {
                const auto index = static_cast<sizet>(role);
                const bool selected = (Taken[index] % selection.Stride[index]) == 0u;
                ++Taken[index];
                return selected;
            }
        };

        [[nodiscard]] u32 CountCurveSegments(const GroomCurveView& groom, u32 curveIndex) noexcept
        {
            const u32 points = groom.GetCurvePointCount(curveIndex);
            return points >= 2u ? points - 1u : 0u;
        }

        // What the COAT says about one curve, plus its role — the answer every
        // pass needs and the only place the evaluation is spelled out.
        struct CurveCoat
        {
            GroomCoatRole Role = GroomCoatRole::Unassigned;
            GroomCoatStrandParams Params{};
        };

        [[nodiscard]] CurveCoat CoatOfCurve(const GroomCurveView& groom, u32 curveIndex, const GroomCoatContext* coat)
        {
            CurveCoat result;
            if (coat == nullptr || !coat->IsActive())
            {
                return result;
            }
            const u16 groupId = groom.GetCurveGroupIds()[curveIndex];
            result.Role = coat->GroupDesc(groupId).GetRole();
            result.Params = EvaluateGroomCoatStrand(*coat, curveIndex, groom.GetRootUVs()[curveIndex], groupId);
            return result;
        }

        // Topology and the guides-only switch: the eligibility that existed
        // before the coat did. Kept separate from the coat's own Keep decision so
        // the stats can say which of the two removed a strand.
        [[nodiscard]] bool CurveIsEligible(const GroomCurveView& groom, u32 curveIndex,
                                           const GroomStrandBuildSettings& settings) noexcept
        {
            if (settings.GuidesOnly && !groom.IsGuide(curveIndex))
            {
                return false;
            }
            return groom.GetCurvePointCount(curveIndex) >= 2u;
        }

        [[nodiscard]] Selection SelectCurves(const GroomCurveView& groom, const GroomStrandBuildSettings& settings,
                                             const GroomCoatContext* coat)
        {
            Selection selection;

            std::array<u64, GroomCoatRoleCount> segmentsByRole{};
            const u32 curveCount = groom.GetCurveCount();
            for (u32 curve = 0; curve < curveCount; ++curve)
            {
                if (!CurveIsEligible(groom, curve, settings))
                {
                    continue;
                }
                const CurveCoat curveCoat = CoatOfCurve(groom, curve, coat);
                if (!curveCoat.Params.Keep)
                {
                    ++selection.DroppedByCoat;
                    continue;
                }
                if (curveCoat.Params.Clump > 0.0f)
                {
                    selection.WantsClumping = true;
                }
                const auto role = static_cast<sizet>(curveCoat.Role);
                ++selection.Available[role];
                ++selection.AvailableTotal;
                segmentsByRole[role] += CountCurveSegments(groom, curve);
            }

            if (selection.AvailableTotal == 0u)
            {
                return selection;
            }

            // ── The budget, as a fraction per role ───────────────────
            //
            // Find k in (0, 1] with sum_r count_r * min(1, k * w_r) == budget,
            // for BOTH budgets — strands, and segments expressed as an
            // equivalent strand count through each role's own mean strand
            // length. The tighter of the two answers wins.
            //
            // Bisection rather than a closed form: the saturation makes the sum
            // piecewise linear in k with a breakpoint per distinct weight, and
            // forty halvings of [0, 1] in f64 reach the exact answer to far
            // beyond the precision a u32 stride can express. It is forty
            // iterations of a five-element loop, once per build.
            const auto retainedFor = [](f64 k, sizet role)
            {
                const f64 weight = static_cast<f64>(GroomCoatBudgetWeight(static_cast<GroomCoatRole>(role)));
                return std::min(1.0, k * weight);
            };

            const auto solve = [&](auto&& costOfRole, f64 budget) -> f64
            {
                f64 total = 0.0;
                for (sizet role = 0; role < GroomCoatRoleCount; ++role)
                {
                    total += costOfRole(role);
                }
                if (total <= budget || total <= 0.0)
                {
                    return 1.0;
                }
                f64 lo = 0.0;
                f64 hi = 1.0;
                for (i32 iteration = 0; iteration < 40; ++iteration)
                {
                    const f64 mid = (lo + hi) * 0.5;
                    f64 spent = 0.0;
                    for (sizet role = 0; role < GroomCoatRoleCount; ++role)
                    {
                        spent += costOfRole(role) * retainedFor(mid, role);
                    }
                    if (spent > budget)
                    {
                        hi = mid;
                    }
                    else
                    {
                        lo = mid;
                    }
                }
                return lo;
            };

            const f64 strandK = solve([&selection](sizet role)
                                      { return static_cast<f64>(selection.Available[role]); },
                                      static_cast<f64>(std::max(1u, settings.MaxStrands)));
            const f64 segmentK = solve([&segmentsByRole](sizet role)
                                       { return static_cast<f64>(segmentsByRole[role]); },
                                       static_cast<f64>(std::max(1u, settings.MaxSegments)));

            // The SEGMENT budget is the one that actually sizes the buffer, so
            // when it is the binding one the stats say so — raising "Max
            // Strands" and seeing no change is otherwise indistinguishable from
            // a broken slider.
            selection.SegmentBudgetLimited = segmentK < strandK;
            const f64 k = std::min(strandK, segmentK);

            selection.Selected = 0;
            for (sizet role = 0; role < GroomCoatRoleCount; ++role)
            {
                const f64 fraction = retainedFor(k, role);
                // std::ceil, and the direction is load-bearing.
                //
                // The budget is an UPPER BOUND, so the stride must round the
                // retained fraction DOWN — which means rounding 1/fraction up.
                // Rounding to nearest looks more accurate and is not: 1/0.4 is
                // 2.5, which rounds to a stride of 2 and keeps half the role
                // where the solver had decided on four tenths. Across five roles
                // that overshot MaxStrands by about a third, and MaxStrands has
                // no hard cap downstream the way MaxSegments does — the build
                // enforces the segment budget exactly and simply believes the
                // strand one.
                //
                // Ceil also reproduces the pre-#1251 arithmetic EXACTLY on a
                // groom with no roles, where every weight is equal: the solver's
                // k is then budget/available, and ceil(1/k) is ceil(available/
                // budget), which is the stride the old code computed directly.
                // AGroomWithNoRolesBehavesExactlyAsItDidBefore asserts on that
                // number.
                //
                // THE TOLERANCE IS NOT COSMETIC. The bisection converges on `lo`,
                // the largest k it proved affordable, so it approaches the true
                // root FROM BELOW and never reaches it: for 500 strands into a
                // budget of 100 it returns 0.2 minus about 1e-12, whose inverse
                // is 5.000000000023, and a bare ceil makes that a stride of SIX.
                // Every budget would then thin by one more than it was asked to,
                // and the pre-#1251 parity test above would be off by one with no
                // visible symptom. A relative nudge of 1e-6 is orders of
                // magnitude above the bisection's error and orders of magnitude
                // below a stride step.
                const f64 inverse = 1.0 / std::max(fraction, 1e-9);
                const u32 stride =
                    fraction >= 1.0 ? 1u : std::max(1u, static_cast<u32>(std::ceil(inverse * (1.0 - 1e-6))));
                selection.Stride[role] = stride;
                selection.Selected += (selection.Available[role] + stride - 1u) / stride;
            }
            return selection;
        }

        // ── Clumping (issue #1251) ───────────────────────────────────
        //
        // A clump is a patch of the PELT, quantised in root UV, and its shape is
        // the mean growth vector (tip - root, after the length scale) of the
        // strands that grow in it. A strand is then pulled toward its own root
        // plus that vector — so roots never move and tips converge.
        //
        // COMPUTED OVER EVERY COAT-KEPT CURVE, NOT OVER THE SELECTED ONES, and
        // that is deliberate: the budget's stride is a distance/LOD decision, and
        // letting it into the clump mean would make a tuft change shape as the
        // camera walked toward it. Two walks over the groom rather than one, paid
        // once per cache miss.
        //
        // The accumulator is an unordered_map, which GroomCooker.h forbids
        // ITERATING to produce output. Nothing here iterates it — it is only ever
        // looked up by key — so the output order and every value in it are a pure
        // function of the groom.
        using ClumpTable = std::unordered_map<u64, GroomCoatClumpAccum>;

        [[nodiscard]] ClumpTable BuildClumpTable(const GroomCurveView& groom, const GroomStrandBuildSettings& settings,
                                                 const GroomCoatContext* coat)
        {
            ClumpTable table;
            if (coat == nullptr || !coat->IsActive())
            {
                return table;
            }
            const f32 cellSize = coat->Settings->ClumpCellSize;
            const auto& points = groom.GetPoints();
            const u32 curveCount = groom.GetCurveCount();
            for (u32 curve = 0; curve < curveCount; ++curve)
            {
                if (!CurveIsEligible(groom, curve, settings))
                {
                    continue;
                }
                const CurveCoat curveCoat = CoatOfCurve(groom, curve, coat);
                if (!curveCoat.Params.Keep)
                {
                    continue;
                }
                const u32 first = groom.GetCurveFirstPoint(curve);
                const u32 count = groom.GetCurvePointCount(curve);
                const glm::vec3& root = points[first];
                const glm::vec3& tip = points[first + count - 1u];
                const u64 cell = GroomCoatClumpCell(groom.GetRootUVs()[curve], cellSize);
                GroomCoatClumpAccum& accum = table[cell];
                // The LENGTH-SCALED growth, so a clump of strands that were all
                // shortened converges at the shortened tips rather than reaching
                // for where the tips used to be.
                accum.GrowthSum += (tip - root) * curveCoat.Params.Length;
                ++accum.Count;
            }
            return table;
        }

        [[nodiscard]] glm::vec3 ClumpGrowthFor(const ClumpTable& table, const GroomCurveView& groom, u32 curveIndex,
                                               f32 cellSize) noexcept
        {
            const auto it = table.find(GroomCoatClumpCell(groom.GetRootUVs()[curveIndex], cellSize));
            return it != table.end() ? it->second.MeanGrowth() : glm::vec3(0.0f);
        }

        void FillRoleStats(GroomStrandMeshStats& stats, const Selection& selection) noexcept
        {
            for (sizet role = 0; role < GroomCoatRoleCount; ++role)
            {
                stats.AvailableByRole[role] = selection.Available[role];
                stats.StrideByRole[role] = selection.Stride[role];
            }
        }
    } // namespace

    GroomBuildSource GroomBuildSource::FromAsset(const GroomAsset& groom) noexcept
    {
        GroomBuildSource source;
        source.Curves = groom.GetCurveView();
        source.BaseCurveCount = groom.GetCurveCount();
        return source;
    }

    GroomBuildSource GroomBuildSource::FromLevel(const GroomAsset& base, const GroomLodLevel& level) noexcept
    {
        GroomBuildSource source;
        source.Curves = level.GetCurveView();
        source.SourceCurves = level.SourceCurves;
        // The BASE's count, deliberately: it is what the binding's
        // root-transform array and the guide influence table are sized by, and
        // what SourceCurves indexes into. GroomLodLevel::Validate has already
        // bounded every entry against it, on cook and on load.
        source.BaseCurveCount = base.GetCurveCount();
        return source;
    }

    void SelectGroomStrandCurves(const GroomBuildSource& source, const GroomStrandBuildSettings& settings,
                                 std::vector<u32>& outCurves, const GroomCoatContext* coat)
    {
        outCurves.clear();
        const GroomCurveView& groom = source.Curves;

        // Deliberately the SAME eligibility test and the SAME stride arithmetic
        // the build uses, reached through the same helpers rather than
        // reimplemented: a selection that disagreed with the build by one curve
        // would deform a strand that is not drawn and draw a strand that was
        // not deformed, and the second of those is a coat with one stiff hair
        // in it that no assertion would ever catch.
        const Selection selection = SelectCurves(groom, settings, coat);
        outCurves.reserve(selection.Selected);

        RoleWalk walk;
        u64 segments = 0;
        const u32 curveCount = groom.GetCurveCount();
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            if (!CurveIsEligible(groom, curve, settings))
            {
                continue;
            }
            const CurveCoat curveCoat = CoatOfCurve(groom, curve, coat);
            if (!curveCoat.Params.Keep)
            {
                continue;
            }
            if (walk.Take(curveCoat.Role, selection))
            {
                // And the SEGMENT BUDGET too, not only the stride. The stride
                // comes from an average strand length, so the build can run out
                // of budget before the last stride-aligned curve and emit
                // nothing for the rest -- while this list still named them. Every
                // such curve is then deformed (paying for a root evaluation and
                // a surface frame) for geometry that is never built, and the
                // binding preview draws a frame at a strand the viewport does
                // not contain.
                //
                // The test is `>=` BEFORE the curve is taken, which is the same
                // test BuildGroomStrandMesh makes at the top of its own curve
                // loop -- so the one curve that STRADDLES the cap is on both
                // lists (it is partly emitted, so it is really deformed) and
                // every curve after it is on neither.
                if (segments >= static_cast<u64>(settings.MaxSegments))
                {
                    break;
                }
                outCurves.push_back(curve);
                segments += CountCurveSegments(groom, curve);
            }
        }
    }

    GroomStrandMeshStats PlanGroomStrandMesh(const GroomBuildSource& source, const GroomStrandBuildSettings& settings,
                                             const GroomCoatContext* coat)
    {
        const GroomCurveView& groom = source.Curves;
        GroomStrandMeshStats stats;
        const Selection selection = SelectCurves(groom, settings, coat);
        stats.StrandsAvailable = selection.AvailableTotal;
        stats.StrandsSelected = selection.Selected;
        stats.StrandsDroppedByCoat = selection.DroppedByCoat;
        stats.SegmentBudgetLimited = selection.SegmentBudgetLimited;
        FillRoleStats(stats, selection);
        // The reported scalar stride is the one the LARGEST role pays, which is
        // the number an inspector showing a single "Stride" field should show: a
        // whisker group's stride of 1 says nothing about why the coat is thin.
        stats.Stride = *std::max_element(selection.Stride.begin(), selection.Stride.end());

        u64 segments = 0;
        RoleWalk walk;
        const u32 curveCount = groom.GetCurveCount();
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            if (!CurveIsEligible(groom, curve, settings))
            {
                continue;
            }
            const CurveCoat curveCoat = CoatOfCurve(groom, curve, coat);
            if (!curveCoat.Params.Keep)
            {
                continue;
            }
            if (walk.Take(curveCoat.Role, selection))
            {
                ++stats.SelectedByRole[static_cast<sizet>(curveCoat.Role)];
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
        }

        stats.SegmentCount = static_cast<u32>(segments);
        stats.VertexCount = stats.SegmentCount * 4u;
        stats.IndexCount = stats.SegmentCount * 6u;
        stats.VertexBytes = static_cast<u64>(stats.VertexCount) * sizeof(GroomStrandVertex);
        stats.IndexBytes = static_cast<u64>(stats.IndexCount) * sizeof(u32);
        return stats;
    }

    GroomStrandMeshStats BuildGroomStrandMesh(const GroomBuildSource& source,
                                              const GroomStrandBuildSettings& settings,
                                              std::vector<GroomStrandVertex>& outVertices, std::vector<u32>& outIndices,
                                              const GroomStrandDeformation* deformation, const GroomCoatContext* coat,
                                              const GroomStrandSimulation* simulation)
    {
        outVertices.clear();
        outIndices.clear();
        const GroomCurveView& groom = source.Curves;

        // A deformation that does not span this groom is treated as ABSENT
        // rather than partially applied. Half a deformed coat is the plausible
        // wrong image #1249 exists to prevent; an undeformed one is visibly at
        // the bind pose, and the caller already counted the refusal that got it
        // here (GroomBindingRejectReason).
        // AGAINST THE BASE GROOM'S CURVE COUNT, not this curve set's. A cooked
        // LOD level has fewer curves than the groom it stands in for, and the
        // binding's root-transform array is sized by the BASE — so comparing
        // against the level's count would reject every deformation at card
        // range and the coat would detach from the body at exactly the distance
        // nobody is watching closely (issue #1252).
        const bool deformed = deformation != nullptr && deformation->IsUsable(source.BaseCurveCount);

        // Same rule, and for the same reason: a simulation that does not span
        // this groom is ABSENT rather than partially applied. Half a moving coat
        // is the plausible wrong image; a still one is visibly at its groomed
        // rest shape, and the stats below say how many strands that is.
        const bool simulated = simulation != nullptr && simulation->IsUsable(source.BaseCurveCount);

        GroomStrandMeshStats stats;
        const Selection selection = SelectCurves(groom, settings, coat);
        stats.StrandsAvailable = selection.AvailableTotal;
        stats.StrandsSelected = selection.Selected;
        stats.StrandsDroppedByCoat = selection.DroppedByCoat;
        stats.SegmentBudgetLimited = selection.SegmentBudgetLimited;
        FillRoleStats(stats, selection);
        stats.Stride = *std::max_element(selection.Stride.begin(), selection.Stride.end());

        const GroomStrandMeshStats plan = PlanGroomStrandMesh(source, settings, coat);
        outVertices.reserve(plan.VertexCount);
        outIndices.reserve(plan.IndexCount);

        // Only when something actually asks to clump: the prepass is a whole
        // extra walk over the groom plus a hash-map insert per strand, and a
        // coat with no clumping is the common case.
        const ClumpTable clumps = selection.WantsClumping ? BuildClumpTable(groom, settings, coat) : ClumpTable{};
        const f32 clumpCellSize = (coat != nullptr && coat->IsActive()) ? coat->Settings->ClumpCellSize : 0.0f;

        const auto& points = groom.GetPoints();
        const auto& widths = groom.GetPointWidths();

        glm::vec3 boundsMin{ std::numeric_limits<f32>::max() };
        glm::vec3 boundsMax{ std::numeric_limits<f32>::lowest() };

        RoleWalk walk;
        u32 emittedSegments = 0;
        const u32 curveCount = groom.GetCurveCount();
        for (u32 curve = 0; curve < curveCount; ++curve)
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

            const CurveCoat curveCoat = CoatOfCurve(groom, curve, coat);
            if (!curveCoat.Params.Keep)
            {
                continue;
            }
            if (!walk.Take(curveCoat.Role, selection))
            {
                continue;
            }
            ++stats.SelectedByRole[static_cast<sizet>(curveCoat.Role)];

            // The budget is spent: every remaining curve would enter the segment
            // loop below and leave it on the first iteration having emitted
            // nothing. Leaving now is not only cheaper -- it stops
            // StrandsHeldAtRest counting strands that were never going to be
            // drawn, and it makes this loop stop on exactly the curve
            // SelectGroomStrandCurves stops on, which is the property the
            // deformer depends on.
            if (emittedSegments >= settings.MaxSegments)
            {
                stats.SegmentBudgetLimited = true;
                break;
            }

            const u32 first = groom.GetCurveFirstPoint(curve);
            const u32 count = groom.GetCurvePointCount(curve);
            const f32 invSpan = 1.0f / static_cast<f32>(count - 1u);

            // The BASE curve this one stands in for. Identity for the base
            // groom; for a cooked card it is the member strand whose rest frame
            // and guides the card borrows (issue #1252). Everything indexed by
            // the BINDING or by the guide influence table goes through it, and
            // everything indexed by this curve set's own geometry does not.
            const u32 sourceCurve = source.SourceCurve(curve);

            // One lookup per CURVE, not per point: the root transform is a
            // property of the strand, and re-reading it per segment would be the
            // dominant cost of a long coat for no change in the result.
            const GroomRootBinding* record = nullptr;
            const GroomRootTransform* transform = nullptr;
            if (deformed)
            {
                record = &deformation->Binding->GetRoot(sourceCurve);
                transform = &deformation->RootTransforms[sourceCurve];
                if (!transform->Valid)
                {
                    // Held at rest, and counted, exactly as
                    // EvaluateGroomRootTransforms intends: a patch of coat that
                    // does not move is diagnosable, a patch that flies off is a
                    // bug report about the wrong subsystem.
                    ++stats.StrandsHeldAtRest;
                }
            }

            // ── The coat's shape, in REST space, BEFORE the deformation ──
            //
            // That order is the whole of criterion 2. Length, clump and width
            // are functions of the root UV and the curve index, applied to the
            // asset's own points; the binding's root transform is applied to the
            // result. So a coat authored on a bind-pose pelt arrives on a
            // running animal transformed by the body and by nothing else — the
            // regional map cannot slide, because it was never consulted in a
            // space the body moves.
            const glm::vec3& curveRoot = points[first];
            const glm::vec3 clumpGrowth =
                curveCoat.Params.Clump > 0.0f ? ClumpGrowthFor(clumps, groom, curve, clumpCellSize) : glm::vec3(0.0f);
            const f32 packedTint = PackGroomCoatTint(curveCoat.Params.Tint);

            const auto shape = [&](u32 pointIndex)
            {
                const f32 t = static_cast<f32>(pointIndex) * invSpan;
                return ApplyGroomCoatShape(curveRoot, points[first + pointIndex], t, curveCoat.Params.Length,
                                           curveCoat.Params.Clump, clumpGrowth);
            };

            // The guide simulation's displacement, if this strand has one. Read
            // ONCE per curve rather than per point: the weights are a property of
            // the strand, and the per-point part is the parameter `t` below.
            bool curveSimulated = false;
            if (simulated)
            {
                // A strand counts as simulated when it names at least one guide
                // that this frame's budget actually moved — a predicate, never a
                // comparison of a displacement against zero.
                curveSimulated = HasGroomGuideInfluence(*simulation, sourceCurve);
                if (curveSimulated)
                {
                    ++stats.StrandsSimulated;
                }
                else
                {
                    ++stats.StrandsUnguided;
                }
            }

            const auto place = [&](const glm::vec3& restPoint, f32 t, bool previous)
            {
                glm::vec3 placed = transform != nullptr
                                       ? ApplyGroomRootTransform(*record, *transform, restPoint, previous)
                                       : restPoint;
                // ADDED TO the deformed rest position, never substituted for it.
                // The displacement is measured from that same position, so a
                // guide at rest contributes exactly zero and the coat is the one
                // #1251 built.
                if (curveSimulated)
                {
                    placed += SampleGroomGuideDisplacement(*simulation, sourceCurve, t, previous);
                }
                return placed;
            };

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

                // The REST points from the asset THROUGH THE COAT, then the
                // deformed pair this frame and the deformed pair last frame. An
                // undeformed groom takes the identity path through `place`, and a
                // groom with no coat takes the identity path through `shape`, so
                // `p0 == rest0` and `prev0 == p0` and the emitted bytes are what
                // they were before #1249 and #1251.
                const glm::vec3 rest0 = shape(i);
                const glm::vec3 rest1 = shape(i + 1u);
                // The SAME parameter the coat's shape term uses, so the guide
                // sample and the length multiplier agree about where this point
                // sits along the strand.
                const f32 t0 = static_cast<f32>(i) * invSpan;
                const f32 t1 = static_cast<f32>(i + 1u) * invSpan;
                const glm::vec3 p0 = place(rest0, t0, false);
                const glm::vec3 p1 = place(rest1, t1, false);
                const glm::vec3 prev0 = place(rest0, t0, true);
                const glm::vec3 prev1 = place(rest1, t1, true);
                // Stored the same way for every corner of the quad so the
                // vertex shader derives ONE screen-space tangent per segment.
                // See GroomStrandVertex::Other for what goes wrong otherwise.
                const glm::vec3 delta = p1 - p0;

                // The cooked widths are DIAMETERS (the Alembic/USD
                // convention); the halving happens exactly once, here. The
                // coat's width multiplier rides along with it rather than being
                // folded into the request's WidthScale, because that one is a
                // per-GROOM unit-scale lever and this one is per strand.
                const f32 r0 = widths[first + i] * 0.5f * curveCoat.Params.Width;
                const f32 r1 = widths[first + i + 1u] * 0.5f * curveCoat.Params.Width;

                // The box covers the RIBBON, not the centreline it is built
                // around. GroomStrand.glsl expands each segment sideways by
                // Radius, so a centreline-only box is smaller than the thing
                // drawn from it -- and a culler handed it removes strands that
                // are visibly on screen, at exactly the grazing angles where the
                // expansion is largest and a coat losing its silhouette is most
                // obvious.
                //
                // The expansion is isotropic because the sideways direction is
                // chosen per view: it is perpendicular to the segment and to the
                // eye vector, so no axis-aligned bound can be tighter than the
                // sphere swept along the centreline without knowing the camera.
                // A hair radius is a fraction of a millimetre against a body, so
                // this costs the culler nothing measurable.
                const f32 radius = std::max(r0, r1);
                const glm::vec3 expand{ radius };
                boundsMin = glm::min(boundsMin, glm::min(p0, p1) - expand);
                boundsMax = glm::max(boundsMax, glm::max(p0, p1) + expand);

                // Previous positions widen the box as well: the motion-vector
                // pass reads them through the same geometry, so a box that
                // holds only this frame's ribbon can cull a strand whose
                // previous position is still on screen.
                boundsMin = glm::min(boundsMin, glm::min(prev0, prev1) - expand);
                boundsMax = glm::max(boundsMax, glm::max(prev0, prev1) + expand);

                const f32 u0 = static_cast<f32>(i) * invSpan;
                const f32 u1 = static_cast<f32>(i + 1u) * invSpan;

                const f32 segmentId =
                    std::bit_cast<f32>(GroomSegmentIdentity(curve, i));

                const u32 base = static_cast<u32>(outVertices.size());

                GroomStrandVertex vertex;
                vertex.SegmentId = segmentId;
                vertex.Tint = packedTint;

                // Corner order: (-side at P0), (+side at P0), (+side at P1),
                // (-side at P1) — a quad, not a bowtie, because `Other` gives
                // all four the same tangent.
                vertex.Position = p0;
                vertex.PrevPosition = prev0;
                vertex.Other = p0 + delta;
                vertex.Radius = r0;
                vertex.Side = -1.0f;
                vertex.Coords = { u0, -1.0f };
                outVertices.push_back(vertex);

                vertex.Side = 1.0f;
                vertex.Coords = { u0, 1.0f };
                outVertices.push_back(vertex);

                vertex.Position = p1;
                vertex.PrevPosition = prev1;
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

        // The box is published only if something was emitted. The sentinel
        // (max, lowest) is a perfectly valid-looking box that contains
        // everything, and handing it to a culler as though it were a measurement
        // is how an empty groom becomes a groom that is never culled.
        stats.BoundsValid = emittedSegments != 0u;
        if (stats.BoundsValid)
        {
            stats.BoundsMin = boundsMin;
            stats.BoundsMax = boundsMax;
        }
        return stats;
    }

    // ── The GroomAsset overloads ────────────────────────────────────────────
    //
    // Every call site that predates #1252 builds the BASE groom, so it says so
    // by passing the asset and gets the identity source map. Keeping them as
    // thin forwarders rather than making every caller assemble a
    // GroomBuildSource is what keeps the LOD change invisible to the debug
    // preview, the tests and the binding authoring tools.
    void SelectGroomStrandCurves(const GroomAsset& groom, const GroomStrandBuildSettings& settings,
                                 std::vector<u32>& outCurves, const GroomCoatContext* coat)
    {
        SelectGroomStrandCurves(GroomBuildSource::FromAsset(groom), settings, outCurves, coat);
    }

    GroomStrandMeshStats PlanGroomStrandMesh(const GroomAsset& groom, const GroomStrandBuildSettings& settings,
                                             const GroomCoatContext* coat)
    {
        return PlanGroomStrandMesh(GroomBuildSource::FromAsset(groom), settings, coat);
    }

    GroomStrandMeshStats BuildGroomStrandMesh(const GroomAsset& groom, const GroomStrandBuildSettings& settings,
                                              std::vector<GroomStrandVertex>& outVertices, std::vector<u32>& outIndices,
                                              const GroomStrandDeformation* deformation, const GroomCoatContext* coat,
                                              const GroomStrandSimulation* simulation)
    {
        return BuildGroomStrandMesh(GroomBuildSource::FromAsset(groom), settings, outVertices, outIndices, deformation,
                                    coat, simulation);
    }
} // namespace OloEngine
