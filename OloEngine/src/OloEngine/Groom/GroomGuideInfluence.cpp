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
        // A null table is a DOCUMENTED state of GroomStrandSimulation -- it is
        // what GroomStrandRequest::Simulation() returns for an un-simulated
        // groom -- so these two helpers answer it rather than dereferencing it.
        // The strand build happens to guard every call through IsUsable today;
        // that is the caller being careful, not the contract being safe.
        if (simulation.Influence == nullptr)
        {
            return false;
        }
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
        if (simulation.Influence == nullptr)
        {
            return glm::vec3(0.0f);
        }
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
    void BuildGroomGuideStandIns(std::span<const glm::vec3> slotRoots, std::span<const u32> slotGroups,
                                 std::span<const u32> slotRoles, std::span<const u32> guideOfSlot,
                                 TArray<GroomGuideWeights>& outStandIns)
    {
        outStandIns.Reset();
        const sizet slotCount = guideOfSlot.size();
        if (slotRoots.size() != slotCount || slotGroups.size() != slotCount || slotRoles.size() != slotCount)
        {
            return;
        }
        outStandIns.SetNum(static_cast<i32>(slotCount));

        // The simulated slots, bucketed by (group, role) and by role, so a
        // left-out slot searches its own group's guides OF ITS OWN ROLE, then
        // its own role's anywhere, and never another role's -- a group may hold
        // slots of two roles.
        const auto groupRoleKey = [&](u32 slot)
        { return (static_cast<u64>(slotGroups[slot]) << 32u) | static_cast<u64>(slotRoles[slot]); };
        std::unordered_map<u64, TArray<u32>> simulatedByGroupRole;
        std::unordered_map<u32, TArray<u32>> simulatedByRole;
        for (u32 slot = 0; slot < slotCount; ++slot)
        {
            if (guideOfSlot[slot] != GroomNoGuide)
            {
                simulatedByGroupRole[groupRoleKey(slot)].Add(slot);
                simulatedByRole[slotRoles[slot]].Add(slot);
            }
        }

        for (u32 slot = 0; slot < slotCount; ++slot)
        {
            GroomGuideWeights& standIn = outStandIns[static_cast<i32>(slot)];
            if (guideOfSlot[slot] != GroomNoGuide)
            {
                standIn.Guides[0] = guideOfSlot[slot];
                standIn.Weights[0] = 1.0f;
                continue;
            }

            const TArray<u32>* candidates = nullptr;
            if (const auto group = simulatedByGroupRole.find(groupRoleKey(slot)); group != simulatedByGroupRole.end())
            {
                candidates = &group->second;
            }
            else if (const auto role = simulatedByRole.find(slotRoles[slot]); role != simulatedByRole.end())
            {
                candidates = &role->second;
            }
            if (candidates == nullptr)
            {
                continue; // its role simulates nothing: it stays at rest, as authored
            }

            // The K nearest by root distance, kept sorted by an insertion step:
            // K is four, so a heap would be slower than this.
            std::array<f32, GroomGuideInfluenceCount> bestDistance{};
            bestDistance.fill(std::numeric_limits<f32>::max());
            std::array<u32, GroomGuideInfluenceCount> bestSlot{};
            bestSlot.fill(GroomNoGuide);
            const glm::vec3 root = slotRoots[slot];
            for (const u32 candidate : *candidates)
            {
                const f32 distance = glm::distance2(root, slotRoots[candidate]);
                if (!(distance < bestDistance[GroomGuideInfluenceCount - 1u]))
                {
                    continue;
                }
                u32 at = GroomGuideInfluenceCount - 1u;
                while (at > 0u && distance < bestDistance[at - 1u])
                {
                    bestDistance[at] = bestDistance[at - 1u];
                    bestSlot[at] = bestSlot[at - 1u];
                    --at;
                }
                bestDistance[at] = distance;
                bestSlot[at] = candidate;
            }

            // Inverse distance, floored so a coincident root does not divide by
            // zero; normalised over the guides found.
            f32 total = 0.0f;
            for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
            {
                if (bestSlot[k] == GroomNoGuide)
                {
                    continue;
                }
                const f32 weight = 1.0f / std::max(std::sqrt(bestDistance[k]), 1.0e-6f);
                standIn.Guides[k] = guideOfSlot[bestSlot[k]];
                standIn.Weights[k] = weight;
                total += weight;
            }
            if (total > 0.0f)
            {
                for (f32& weight : standIn.Weights)
                {
                    weight /= total;
                }
            }
        }
    }

    namespace
    {
        // One simulated guide's displacement at parameter t, sampled BY
        // PARAMETER -- SampleGroomGuideDisplacement's rule, so a stand-in with
        // a different point count than the slot it stands in for agrees about
        // where "halfway up" is.
        [[nodiscard]] bool SampleGuideAt(std::span<const u32> guideOffsets, std::span<const glm::vec3> displacements,
                                         u32 guide, f32 t, glm::vec3& out) noexcept
        {
            if (guide + 1u >= guideOffsets.size())
            {
                return false;
            }
            const u32 first = guideOffsets[guide];
            const u32 last = guideOffsets[guide + 1u];
            if (last <= first || last > displacements.size())
            {
                return false;
            }
            const u32 count = last - first;
            if (count == 1u)
            {
                out = displacements[first];
                return true;
            }
            const f32 scaled = std::clamp(t, 0.0f, 1.0f) * static_cast<f32>(count - 1u);
            const f32 floored = std::floor(scaled);
            const u32 lower = std::min(static_cast<u32>(floored), count - 1u);
            const u32 upper = std::min(lower + 1u, count - 1u);
            out = glm::mix(displacements[first + lower], displacements[first + upper], scaled - floored);
            return true;
        }
    } // namespace

    bool ExpandGroomGuideDisplacements(std::span<const GroomGuideWeights> standIns,
                                       std::span<const u32> slotPointCounts, std::span<const u32> guideOffsets,
                                       std::span<const glm::vec3> displacements, TArray<u32>& outOffsets,
                                       TArray<glm::vec3>& outDisplacements)
    {
        outOffsets.Reset();
        outDisplacements.Reset();
        if (standIns.size() != slotPointCounts.size() || guideOffsets.empty())
        {
            return false;
        }
        // THE WHOLE LAYOUT IS CHECKED BEFORE EITHER OUTPUT IS WRITTEN, so a
        // refusal publishes nothing rather than a zero displacement for a guide
        // whose range was bad: offsets start at 0, never decrease and end inside
        // the displacements, and every stand-in names a guide in the table.
        if (guideOffsets.front() != 0u || guideOffsets.back() > displacements.size())
        {
            return false;
        }
        for (sizet i = 1; i < guideOffsets.size(); ++i)
        {
            if (guideOffsets[i] < guideOffsets[i - 1u])
            {
                return false;
            }
        }
        const sizet guideCount = guideOffsets.size() - 1u;
        for (const GroomGuideWeights& standIn : standIns)
        {
            for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
            {
                if (standIn.Guides[k] != GroomNoGuide && standIn.Guides[k] >= guideCount)
                {
                    return false;
                }
            }
        }

        u64 total = 0;
        for (const u32 count : slotPointCounts)
        {
            total += count;
        }
        outOffsets.Reserve(static_cast<i32>(standIns.size() + 1u));
        outDisplacements.Reserve(static_cast<i32>(total));
        outOffsets.Add(0u);

        for (sizet slot = 0; slot < standIns.size(); ++slot)
        {
            const GroomGuideWeights& standIn = standIns[slot];
            const u32 count = slotPointCounts[slot];
            const f32 invSpan = count > 1u ? 1.0f / static_cast<f32>(count - 1u) : 0.0f;
            for (u32 i = 0; i < count; ++i)
            {
                const f32 t = static_cast<f32>(i) * invSpan;
                glm::vec3 blended{ 0.0f };
                f32 applied = 0.0f;
                for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
                {
                    glm::vec3 sample;
                    if (standIn.Guides[k] == GroomNoGuide || !(standIn.Weights[k] > 0.0f) ||
                        !SampleGuideAt(guideOffsets, displacements, standIn.Guides[k], t, sample))
                    {
                        continue;
                    }
                    blended += sample * standIn.Weights[k];
                    applied += standIn.Weights[k];
                }
                outDisplacements.Add(applied > 0.0f ? blended / applied : glm::vec3(0.0f));
            }
            outOffsets.Add(static_cast<u32>(outDisplacements.Num()));
        }
        return true;
    }
} // namespace OloEngine
