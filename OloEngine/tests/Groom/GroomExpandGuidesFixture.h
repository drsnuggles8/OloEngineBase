#pragma once

// =============================================================================
// GroomExpandGuidesFixture.h -- issue #1589.
//
// The inputs and the comparison GroomExpandGuidesParityTest (OpenGL) and its
// VulkanPassSuite tenant share, so the two backends' kernels are held to the
// same CPU expansion of the same coat by the same rule. Header-only and
// GPU-free: dispatching is each backend's own business.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomGpuDeformation.h"
#include "OloEngine/Groom/GroomGuideInfluence.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::GroomExpandGuidesTest
{
    /// Within 1e-6 m plus 1e-5 of the displacement: the two sides are two
    /// compilers' float arithmetic over the same operations, and the GPU's
    /// division, square root and fused multiply-adds are not the host's. The
    /// displacements reach ~4 cm, so the bound is at most ~1.4 um -- about a
    /// hundredth of a strand's width -- while a wrong weight, guide, parameter or
    /// frame moves a point by millimetres.
    constexpr f32 kAbsoluteTolerance = 1.0e-6f;
    constexpr f32 kRelativeTolerance = 1.0e-5f;

    /// A coat of `curves` curves in two groups, every third one a guide, with
    /// point counts from 2 to 9 so a stand-in and the slot it stands in for
    /// rarely share a point count: a sample taken by index instead of by
    /// parameter lands measurably elsewhere.
    [[nodiscard]] inline Ref<GroomAsset> MakeVariedCoat(u32 curves)
    {
        GroomBuilder builder;
        std::string reason;
        u16 under = 0;
        u16 guard = 0;
        EXPECT_TRUE(builder.AddGroup("undercoat", under, reason)) << reason;
        EXPECT_TRUE(builder.AddGroup("guard", guard, reason)) << reason;
        for (u32 c = 0; c < curves; ++c)
        {
            const u32 count = 2u + (c * 5u) % 8u;
            std::vector<glm::vec3> points(count);
            const std::vector<f32> widths(count, 0.001f);
            const glm::vec3 root{ 0.02f * static_cast<f32>(c % 12u), 0.0f, 0.02f * static_cast<f32>(c / 12u) };
            for (u32 p = 0; p < count; ++p)
            {
                const f32 up = 0.08f * static_cast<f32>(p) / static_cast<f32>(count - 1u);
                points[p] = root + glm::vec3(0.1f * up, up, 0.0f);
            }
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { static_cast<f32>(c % 12u) / 12.0f, static_cast<f32>(c / 12u) / 12.0f };
            input.GroupId = (c / 6u) % 2u == 0u ? under : guard;
            input.IsGuide = (c % 3u) == 0u;
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }
        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }

    /// One frame of a simulation at a budget that keeps every `keepEvery`th
    /// guide slot, published as the producer publishes it: the simulated
    /// guides' points only, and every slot's stand-ins.
    struct Budget
    {
        Ref<GroomGuideInfluenceTable> Table;
        std::vector<u32> GuideOfSlot;
        std::vector<u32> SlotOfGuide;
        std::vector<u32> Offsets;
        std::vector<glm::vec3> Current;
        std::vector<glm::vec3> Previous;
        Ref<GroomGuideStandInSet> StandIns;

        [[nodiscard]] GroomStrandSimulation View() const
        {
            GroomStrandSimulation sim;
            sim.Influence = Table.Raw();
            sim.GuideOfSlot = GuideOfSlot;
            sim.Displacements.GuideOffsets = Offsets;
            sim.Displacements.Displacements = Current;
            sim.Displacements.PrevDisplacements = Previous;
            sim.Displacements.SlotOfGuide = SlotOfGuide;
            sim.StandIns = StandIns.Raw();
            return sim;
        }
    };

    [[nodiscard]] inline Budget MakeBudget(const GroomAsset& groom, u32 keepEvery, bool withPrevious)
    {
        Budget budget;
        budget.Table = BuildGroomGuideInfluence(groom);
        const u32 slots = budget.Table->GetGuideCount();
        const auto& guideCurves = budget.Table->GetGuideCurves();
        const auto& groups = groom.GetCurveGroupIds();

        TArray<glm::vec3> roots;
        TArray<u32> slotGroups;
        TArray<u32> roles;
        TArray<u32> pointCounts;
        budget.Offsets.push_back(0u);
        for (u32 slot = 0; slot < slots; ++slot)
        {
            const u32 curve = guideCurves[slot];
            roots.Add(groom.GetPoints()[groom.GetCurveFirstPoint(curve)]);
            slotGroups.Add(groups[curve]);
            roles.Add(0u);
            pointCounts.Add(groom.GetCurvePointCount(curve));
            if (slot % keepEvery != 0u)
            {
                budget.GuideOfSlot.push_back(GroomNoGuide);
                continue;
            }
            budget.GuideOfSlot.push_back(static_cast<u32>(budget.SlotOfGuide.size()));
            budget.SlotOfGuide.push_back(slot);
            const u32 count = groom.GetCurvePointCount(curve);
            const f32 s = static_cast<f32>(slot);
            // Every fourth guide sags the other way, so some stand-ins sit
            // between opposed neighbours: KeepNeighbourLength's incoherent
            // branch is taken as well as its coherent one.
            const f32 side = (slot % 4u) == 3u ? -1.0f : 1.0f;
            for (u32 p = 0; p < count; ++p)
            {
                const f32 t = count > 1u ? static_cast<f32>(p) / static_cast<f32>(count - 1u) : 0.0f;
                budget.Current.emplace_back(side * 0.004f * (1.0f + std::fmod(s, 5.0f)) * t, -0.03f * t * t,
                                            0.002f * std::sin(1.3f * s + static_cast<f32>(p)));
                if (withPrevious)
                {
                    budget.Previous.emplace_back(side * 0.003f * (1.0f + std::fmod(s, 3.0f)) * t, -0.025f * t * t,
                                                 0.0015f * std::cos(0.7f * s + static_cast<f32>(p)));
                }
            }
            budget.Offsets.push_back(static_cast<u32>(budget.Current.size()));
        }

        TArray<GroomGuideWeights> standIns;
        BuildGroomGuideStandIns(std::span{ roots.GetData(), static_cast<sizet>(roots.Num()) },
                                std::span{ slotGroups.GetData(), static_cast<sizet>(slotGroups.Num()) },
                                std::span{ roles.GetData(), static_cast<sizet>(roles.Num()) },
                                std::span<const u32>(budget.GuideOfSlot), standIns);
        budget.StandIns = GroomGuideStandInSet::Create(std::move(standIns), std::move(pointCounts));
        EXPECT_TRUE(budget.StandIns);
        return budget;
    }

    /// The CPU reference: what every CPU reader of the coat samples.
    struct Reference
    {
        TArray<u32> Offsets;
        TArray<glm::vec3> Current;
        TArray<glm::vec3> Previous;
    };

    [[nodiscard]] inline Reference ExpandOnCpu(const Budget& budget)
    {
        Reference out;
        const auto standIns = budget.StandIns->GetStandIns();
        const auto counts = budget.StandIns->GetSlotPointCounts();
        EXPECT_TRUE(ExpandGroomGuideDisplacements(standIns, counts, budget.Offsets, budget.Current, out.Offsets,
                                                  out.Current));
        if (!budget.Previous.empty())
        {
            TArray<u32> prevOffsets;
            EXPECT_TRUE(ExpandGroomGuideDisplacements(standIns, counts, budget.Offsets, budget.Previous, prevOffsets,
                                                      out.Previous));
        }
        return out;
    }

    /// The buffer a coat whose guides the GPU expands is sent, packed from the
    /// sparse frame exactly as GroomRenderPass packs it, and the dispatch the
    /// buffer hands the pass.
    struct PackedFrame
    {
        GroomDeformBufferLayout Layout;
        GroomDeformFrameStats Frame;
        GroomGuideExpansionDispatch Dispatch;
        std::vector<u8> Bytes;
    };

    [[nodiscard]] inline PackedFrame PackSparseFrame(const GroomAsset& groom, const Budget& budget)
    {
        PackedFrame out;
        const GroomStrandSimulation view = budget.View();
        EXPECT_TRUE(view.IsUsable(groom.GetCurveCount()));
        EXPECT_TRUE(view.ExpandsStandIns());

        const u32 slots = budget.Table->GetGuideCount();
        out.Layout = GroomDeformBufferLayout::Make(0u, slots, GroomDeformDisplacementCapacity(groom, budget.Table.Raw()))
                         .WithGuideExpansion(static_cast<u32>(budget.Current.size()));
        EXPECT_TRUE(out.Layout.ExpandsGuides());

        GroomDeformBuffer buffer;
        buffer.Reset(out.Layout, {}, budget.Table.Raw());
        // No drawn roots: the displacement region alone is under test.
        const GroomBindingAsset binding;
        out.Frame = buffer.PackFrame({}, binding, {}, &view, groom.GetCurveCount());
        EXPECT_TRUE(out.Frame.Simulated);
        EXPECT_TRUE(out.Frame.GuidesExpandedOnGpu);
        EXPECT_EQ(out.Frame.DisplacementCount, budget.StandIns->GetPointCount());
        EXPECT_EQ(out.Frame.SourceCount, static_cast<u32>(budget.Current.size()));
        out.Dispatch = buffer.GuideExpansionDispatch();
        EXPECT_GT(out.Dispatch.Groups, 0u);
        out.Bytes.assign(buffer.GetBytes().begin(), buffer.GetBytes().end());
        return out;
    }

    /// What one dispatch of the kernel wrote: the dense displacement region.
    struct KernelRun
    {
        std::vector<GroomDeformDisplacementRecord> Records;
        GroomDeformFrameStats Frame;
        GroomDeformBufferLayout Layout;
    };

    [[nodiscard]] inline bool Agrees(const glm::vec3& gpu, const glm::vec3& cpu, f32& outError) noexcept
    {
        outError = glm::length(gpu - cpu);
        return outError <= kAbsoluteTolerance + kRelativeTolerance * glm::length(cpu);
    }

    /// Each slot whose expansion the kernel got wrong, this frame or last, and
    /// the largest disagreement anywhere.
    struct Comparison
    {
        std::vector<u32> WrongSlots;
        f32 MaxError = 0.0f;
        u32 Points = 0;
    };

    [[nodiscard]] inline Comparison Compare(const KernelRun& gpu, const Reference& cpu, bool withPrevious)
    {
        Comparison out;
        const u32 slots = static_cast<u32>(cpu.Offsets.Num()) - 1u;
        for (u32 slot = 0; slot < slots; ++slot)
        {
            bool wrong = false;
            for (u32 i = cpu.Offsets[static_cast<i32>(slot)]; i < cpu.Offsets[static_cast<i32>(slot) + 1]; ++i)
            {
                if (i >= gpu.Records.size())
                {
                    wrong = true;
                    break;
                }
                const glm::vec3 current = cpu.Current[static_cast<i32>(i)];
                // Without history the kernel writes this frame in both lanes.
                const glm::vec3 previous = withPrevious ? cpu.Previous[static_cast<i32>(i)] : current;
                f32 errorNow = 0.0f;
                f32 errorPrev = 0.0f;
                wrong |= !Agrees(glm::vec3(gpu.Records[i].Current), current, errorNow);
                wrong |= !Agrees(glm::vec3(gpu.Records[i].Previous), previous, errorPrev);
                out.MaxError = std::max({ out.MaxError, errorNow, errorPrev });
                ++out.Points;
            }
            if (wrong)
            {
                out.WrongSlots.push_back(slot);
            }
        }
        return out;
    }

    /// A left-out slot with two stand-ins of clearly different weight, or
    /// GroomNoGuide: the negative control breaks its weights.
    [[nodiscard]] inline u32 FindSlotWithTwoStandIns(const Budget& budget)
    {
        const auto standIns = budget.StandIns->GetStandIns();
        for (u32 slot = 0; slot < static_cast<u32>(standIns.size()); ++slot)
        {
            const GroomGuideWeights& s = standIns[slot];
            if (budget.GuideOfSlot[slot] == GroomNoGuide && s.Guides[1] != GroomNoGuide && s.Weights[1] > 0.2f)
            {
                return slot;
            }
        }
        return GroomNoGuide;
    }

    /// All of `slot`'s weight on its first stand-in, in the uploaded bytes only.
    inline void BreakStandInWeights(std::vector<u8>& bytes, const GroomDeformBufferLayout& layout, u32 slot)
    {
        const f32 allOnFirst[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        const sizet weights = (static_cast<sizet>(layout.StandInBase) + static_cast<sizet>(slot) * 2u + 1u) * 16u;
        ASSERT_LE(weights + sizeof(allOnFirst), bytes.size());
        std::memcpy(bytes.data() + weights, allOnFirst, sizeof(allOnFirst));
    }
} // namespace OloEngine::GroomExpandGuidesTest
