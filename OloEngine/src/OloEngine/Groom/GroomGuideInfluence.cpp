#include "OloEnginePCH.h"

#include "OloEngine/Groom/GroomGuideInfluence.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Math/Math.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace OloEngine
{
    namespace
    {
        // Below this the inverse-distance weight would be an infinity. A strand
        // this close to a guide root is, for every purpose here, AT it.
        constexpr f32 kCoincidentDistance2 = 1.0e-12f;

        struct Candidate
        {
            f32 Distance2 = std::numeric_limits<f32>::max();
            u32 Slot = GroomNoGuide;
        };

        // Insert into a fixed-size ascending-by-distance list. A full sort of
        // every guide per strand would be O(N G log G); this is O(N G K) with
        // K == 4 and no allocation.
        //
        // STRICTLY closer, never "closer or equal". Two guides equidistant from
        // a strand must resolve the same way on every build, and the caller
        // already walks slots in ascending order — so a strict test keeps the
        // lower slot and the table is deterministic without comparing two floats
        // for equality, which is a thing this codebase does not do at all.
        void Consider(std::array<Candidate, GroomGuideInfluenceCount>& best, f32 distance2, u32 slot) noexcept
        {
            for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
            {
                if (!(distance2 < best[k].Distance2))
                {
                    continue;
                }
                for (u32 j = GroomGuideInfluenceCount - 1u; j > k; --j)
                {
                    best[j] = best[j - 1u];
                }
                best[k] = Candidate{ distance2, slot };
                return;
            }
        }
    } // namespace

    u64 GroomGuideInfluenceTable::GetCpuMemoryBytes() const noexcept
    {
        return static_cast<u64>(m_Weights.size() * sizeof(GroomGuideWeights)) +
               static_cast<u64>(m_GuideCurves.size() * sizeof(u32));
    }

    Ref<GroomGuideInfluenceTable> BuildGroomGuideInfluence(const GroomAsset& groom)
    {
        OLO_PROFILE_FUNCTION();

        auto table = Ref<GroomGuideInfluenceTable>::Create();

        const u32 curveCount = groom.GetCurveCount();
        table->m_Weights.assign(curveCount, GroomGuideWeights{});
        if (curveCount == 0u)
        {
            return table;
        }

        const auto& points = groom.GetPoints();
        const auto& groupIds = groom.GetCurveGroupIds();

        // ── The guide slots, in curve order ─────────────────────────────────
        //
        // Ascending by construction, which is the property a budget's stride
        // depends on: taking every Nth slot then takes an even sample of the
        // pelt rather than one end of it.
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            if (groom.IsGuide(curve) && groom.GetCurvePointCount(curve) >= 2u)
            {
                table->m_GuideCurves.push_back(curve);
            }
        }
        const u32 guideCount = static_cast<u32>(table->m_GuideCurves.size());
        if (guideCount == 0u)
        {
            // Every strand unguided. The correct description of a groom that
            // was exported with no guide flags — and the reason this is counted
            // rather than logged is that the answer belongs in the inspector
            // beside the coat it explains.
            table->m_UnguidedStrands = curveCount;
            return table;
        }

        // Slots bucketed BY GROUP, so a strand searches only its own group.
        // Built once rather than re-scanned per strand: the alternative is
        // O(N G) group comparisons on a 200k-strand groom purely to skip.
        std::unordered_map<u16, std::vector<u32>> slotsByGroup;
        for (u32 slot = 0; slot < guideCount; ++slot)
        {
            const u32 curve = table->m_GuideCurves[slot];
            slotsByGroup[groupIds[curve]].push_back(slot);
        }

        // The guide ROOTS, cached: the search reads each of them once per
        // strand, and re-deriving a root through two indirections inside the
        // inner loop is the whole cost of this build.
        std::vector<glm::vec3> guideRoots(guideCount);
        for (u32 slot = 0; slot < guideCount; ++slot)
        {
            guideRoots[slot] = points[groom.GetCurveFirstPoint(table->m_GuideCurves[slot])];
        }

        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            GroomGuideWeights& weights = table->m_Weights[curve];

            if (groom.GetCurvePointCount(curve) < 2u)
            {
                ++table->m_UnguidedStrands;
                continue;
            }

            // A GUIDE IS ITS OWN SOLE INFLUENCE. Blending it toward its
            // neighbours would make the rendered guide disagree with the
            // particle the solver moved, so the debug overlay and the coat
            // would draw two different curves.
            if (groom.IsGuide(curve))
            {
                const auto found = std::ranges::lower_bound(table->m_GuideCurves, curve);
                if (found != table->m_GuideCurves.end() && *found == curve)
                {
                    weights.Guides[0] = static_cast<u32>(found - table->m_GuideCurves.begin());
                    weights.Weights[0] = 1.0f;
                    continue;
                }
            }

            const auto group = slotsByGroup.find(groupIds[curve]);
            if (group == slotsByGroup.end())
            {
                // Its group was groomed with no guide. A fact about the
                // authoring, counted so the editor can say so.
                ++table->m_UnguidedStrands;
                continue;
            }

            // The ROOT, not the centroid and not the tip: the root is where the
            // strand grows, it is invariant under every deformation the body can
            // undergo, and it is the quantity the guide's own influence falls
            // off from. A tip-based search would hand a strand on the shoulder
            // to a guide on the flank whenever the two happened to lie down
            // together.
            const glm::vec3 root = points[groom.GetCurveFirstPoint(curve)];

            std::array<Candidate, GroomGuideInfluenceCount> best{};
            for (const u32 slot : group->second)
            {
                Consider(best, glm::length2(root - guideRoots[slot]), slot);
            }

            // Inverse distance, not inverse square: the square makes the nearest
            // guide dominate so completely that a strand midway between two
            // guides snaps to one of them, and the seam between two guide
            // territories is then visible as a crease in the moving coat.
            f32 total = 0.0f;
            std::array<f32, GroomGuideInfluenceCount> raw{};
            for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
            {
                if (best[k].Slot == GroomNoGuide)
                {
                    continue;
                }
                if (best[k].Distance2 <= kCoincidentDistance2)
                {
                    // Coincident roots: take that guide alone. Deriving a weight
                    // from a zero distance is an infinity, and an infinity
                    // normalised against another infinity is a NaN in the coat.
                    weights.Guides[0] = best[k].Slot;
                    weights.Weights[0] = 1.0f;
                    total = -1.0f; // sentinel: handled, skip the normalise below
                    break;
                }
                raw[k] = glm::inversesqrt(best[k].Distance2);
                total += raw[k];
            }
            if (total < 0.0f)
            {
                continue;
            }
            if (!(total > 0.0f) || !Math::IsFinite(total))
            {
                ++table->m_UnguidedStrands;
                continue;
            }

            const f32 inverseTotal = 1.0f / total;
            for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
            {
                if (best[k].Slot == GroomNoGuide)
                {
                    continue;
                }
                weights.Guides[k] = best[k].Slot;
                weights.Weights[k] = raw[k] * inverseTotal;
            }
        }

        return table;
    }

    bool HasGroomGuideInfluence(const GroomStrandSimulation& simulation, u32 curveIndex) noexcept
    {
        const auto& weights = simulation.Influence->GetWeights();
        if (curveIndex >= weights.size())
        {
            return false;
        }
        const GroomGuideWeights& influence = weights[curveIndex];
        for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
        {
            const u32 slot = influence.Guides[k];
            if (slot == GroomNoGuide || slot >= simulation.GuideOfSlot.size() || !(influence.Weights[k] > 0.0f))
            {
                continue;
            }
            if (simulation.GuideOfSlot[slot] != GroomNoGuide)
            {
                return true;
            }
        }
        return false;
    }

    glm::vec3 SampleGroomGuideDisplacement(const GroomStrandSimulation& simulation, u32 curveIndex, f32 t,
                                           bool previous) noexcept
    {
        const auto& weights = simulation.Influence->GetWeights();
        if (curveIndex >= weights.size() || !Math::IsFinite(t))
        {
            return glm::vec3(0.0f);
        }

        // An absent previous frame is "the same as current", which makes the
        // velocity the shader derives EXACTLY zero rather than approximately
        // zero — both ends go through identical arithmetic, the same rule
        // GroomStrandVertex::PrevPosition is written under.
        const std::span<const glm::vec3> displacements =
            (previous && !simulation.Displacements.PrevDisplacements.empty())
                ? simulation.Displacements.PrevDisplacements
                : simulation.Displacements.Displacements;

        const f32 parameter = std::clamp(t, 0.0f, 1.0f);
        const GroomGuideWeights& influence = weights[curveIndex];

        glm::vec3 result{ 0.0f };
        f32 appliedWeight = 0.0f;
        for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
        {
            const u32 slot = influence.Guides[k];
            if (slot == GroomNoGuide || slot >= simulation.GuideOfSlot.size() || influence.Weights[k] <= 0.0f)
            {
                continue;
            }
            const u32 guide = simulation.GuideOfSlot[slot];
            if (guide == GroomNoGuide || guide + 1u >= simulation.Displacements.GuideOffsets.size())
            {
                // This slot exists in the asset's table but the BUDGET did not
                // simulate it this frame. Skipped, and the remaining weights are
                // renormalised below — dropping the strand entirely would make
                // lowering the guide budget freeze random strands rather than
                // coarsen the motion.
                continue;
            }

            const u32 first = simulation.Displacements.GuideOffsets[guide];
            const u32 last = simulation.Displacements.GuideOffsets[guide + 1u];
            const u32 count = last - first;
            if (count == 0u || last > displacements.size())
            {
                continue;
            }
            if (count == 1u)
            {
                result += displacements[first] * influence.Weights[k];
                appliedWeight += influence.Weights[k];
                continue;
            }

            // SAMPLED BY PARAMETER, never by index. A guide with 12 points and
            // a strand with 6 must agree about where "halfway up" is, or a short
            // strand in a long guide's territory is dragged toward the guide's
            // tip and the coat splays.
            const f32 scaled = parameter * static_cast<f32>(count - 1u);
            const f32 floored = std::floor(scaled);
            const u32 lower = static_cast<u32>(floored);
            const u32 upper = std::min(lower + 1u, count - 1u);
            const f32 fraction = scaled - floored;
            const glm::vec3 sample =
                glm::mix(displacements[first + lower], displacements[first + upper], fraction);
            result += sample * influence.Weights[k];
            appliedWeight += influence.Weights[k];
        }

        if (!(appliedWeight > 0.0f))
        {
            return glm::vec3(0.0f);
        }
        // Renormalised against the weight ACTUALLY applied, so a strand whose
        // nearest guide fell outside the budget follows its remaining guides at
        // full amplitude instead of at a fraction of it. Without this, raising
        // the budget would look like raising the simulation's strength.
        return result / appliedWeight;
    }
} // namespace OloEngine
