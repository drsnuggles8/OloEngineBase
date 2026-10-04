#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomGpuDeformationTest — issue #1427.
//
// A bound coat is no longer rebuilt on the CPU per frame: its stream is built
// once in each root's bind frame (BuildGroomStrandRestMesh) and the vertex
// shader moves it with a small per-frame buffer (GroomDeformBuffer). The CPU
// build is kept as the REFERENCE, and this file is the contract between the
// two: every vertex the GPU path would produce — reconstructed here exactly the
// way GroomStrand.glsl's mode-1 branch reconstructs it, from the same packed
// bytes, through EvaluateGroomDeformedPoint — must be the vertex the CPU path
// builds.
//
// EXACTLY, where the arithmetic is the same arithmetic. A strand whose root
// deformed this frame goes through `Origin + Rotation * local` with `local`
// taken once by the same expression ApplyGroomRootTransform uses, and its guide
// displacement through SampleGroomGuideDisplacement's steps in the same order,
// so the two agree bit for bit and a tolerance would only hide a change to one
// side. A strand HELD at rest is the one place the GPU path takes a different
// route to the same answer (its bind frame there and back), so that case gets a
// tolerance and says so.
//
// What the GPU itself does with those bytes is pinned by
// GroomGpuDeformationEvidenceTest, which renders both paths and compares.
// =============================================================================

#include <gtest/gtest.h>

#include "GroomBindingFixture.h"

#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCasterPose.h"
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomGpuDeformation.h"
#include "OloEngine/Groom/GroomGuideInfluence.h"
#include "OloEngine/Groom/GroomRayTracingProxy.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomShadowWidening.h"
#include "OloEngine/Groom/GroomStrandRequest.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::GroomBindingTest;

namespace
{
    [[nodiscard]] std::vector<glm::mat4> BentPalette(f32 degrees)
    {
        const glm::vec3 hinge{ 0.5f, 0.0f, 0.0f };
        glm::mat4 bend = glm::translate(glm::mat4(1.0f), hinge);
        bend = glm::rotate(bend, glm::radians(degrees), glm::vec3(0.0f, 0.0f, 1.0f));
        bend = glm::translate(bend, -hinge);
        return { glm::mat4(1.0f), bend };
    }

    // A bound coat on a hinged grid, bent this frame and less bent last frame,
    // so every strand on the moving half has a current AND a distinct previous
    // position — a test that compared only current positions would pass a GPU
    // path that got motion vectors wrong.
    struct BoundScene
    {
        GridSurface Grid;
        Ref<GroomAsset> Groom;
        Ref<GroomBindingAsset> Binding;
        std::vector<glm::mat4> Palette;
        std::vector<glm::mat4> PrevPalette;
        TArray<GroomRootTransform> Transforms;
    };

    // `degrees` is this frame's bend (last frame's is 15 less); `coat`, when
    // given, is the coat group's authored description (#1533).
    [[nodiscard]] BoundScene MakeBoundScene(u32 strands = 24u, u32 points = 6u, f32 degrees = 55.0f,
                                            const GroomCoatGroupDesc* coat = nullptr)
    {
        BoundScene scene;
        scene.Grid = MakeGrid(8u);
        WeightAsHinge(scene.Grid);
        scene.Groom = MakeCoat(strands, points, 0.1f, 0.0f, 1.0f, coat);
        EXPECT_TRUE(scene.Groom);
        std::string reason;
        GroomBindingBuildStats stats;
        EXPECT_TRUE(GroomBindingBuilder::Build(*scene.Groom, scene.Grid.View(2u), "TestBody",
                                               GroomBindingBuildSettings{}, scene.Binding, stats, reason))
            << reason;

        scene.Palette = BentPalette(degrees);
        scene.PrevPalette = BentPalette(degrees - 15.0f);
        GroomDeformationInputs inputs;
        inputs.Surface = scene.Grid.View(2u);
        inputs.Skinning = scene.Grid.Skinning(scene.Palette, scene.PrevPalette, true);
        inputs.HasHistory = true;
        (void)EvaluateGroomRootTransforms(*scene.Groom, *scene.Binding, inputs, std::nullopt, scene.Transforms);
        return scene;
    }

    // Every guide simulated, each point displaced by a different amount this
    // frame and last, so a sample taken at the wrong parameter, from the wrong
    // guide or from the wrong frame lands somewhere measurably different.
    struct Simulation
    {
        Ref<GroomGuideInfluenceTable> Table;
        std::vector<u32> Offsets;
        std::vector<u32> GuideOfSlot;
        std::vector<u32> SlotOfGuide;
        std::vector<glm::vec3> Displacements;
        std::vector<glm::vec3> PrevDisplacements;

        [[nodiscard]] GroomStrandSimulation View() const
        {
            GroomStrandSimulation sim;
            sim.Influence = Table.Raw();
            sim.GuideOfSlot = GuideOfSlot;
            sim.Displacements.GuideOffsets = Offsets;
            sim.Displacements.Displacements = Displacements;
            sim.Displacements.PrevDisplacements = PrevDisplacements;
            sim.Displacements.SlotOfGuide = SlotOfGuide;
            return sim;
        }
    };

    // `dropEvery` > 0 leaves every Nth guide slot out of the frame's budget,
    // which is the case the CPU renormalises the remaining weights for.
    [[nodiscard]] Simulation MakeSimulation(const GroomAsset& groom, u32 dropEvery = 0u, bool withPrevious = true)
    {
        Simulation sim;
        sim.Table = BuildGroomGuideInfluence(groom);
        sim.Offsets.push_back(0u);
        u32 guide = 0;
        for (u32 slot = 0; slot < sim.Table->GetGuideCount(); ++slot)
        {
            if (dropEvery > 0u && (slot % dropEvery) == 1u)
            {
                sim.GuideOfSlot.push_back(GroomNoGuide);
                continue;
            }
            const u32 curve = sim.Table->GetGuideCurves()[slot];
            const u32 count = groom.GetCurvePointCount(curve);
            for (u32 p = 0; p < count; ++p)
            {
                const f32 s = static_cast<f32>(slot) + 1.0f;
                const f32 t = static_cast<f32>(p);
                sim.Displacements.emplace_back(0.003f * s * t, -0.002f * t, 0.001f * std::sin(s + t));
                if (withPrevious)
                {
                    sim.PrevDisplacements.emplace_back(0.002f * s * t, -0.001f * t, 0.0015f * std::cos(s + t));
                }
            }
            sim.Offsets.push_back(static_cast<u32>(sim.Displacements.size()));
            sim.GuideOfSlot.push_back(guide);
            sim.SlotOfGuide.push_back(slot);
            ++guide;
        }
        return sim;
    }

    [[nodiscard]] bool BitwiseEqual(const glm::vec3& a, const glm::vec3& b) noexcept
    {
        return std::bit_cast<u32>(a.x) == std::bit_cast<u32>(b.x) && std::bit_cast<u32>(a.y) == std::bit_cast<u32>(b.y) &&
               std::bit_cast<u32>(a.z) == std::bit_cast<u32>(b.z);
    }

    // The comparison every case below makes: build the CPU reference and the
    // GPU path's rest stream + frame buffer from the same inputs, and require
    // every corner to agree. Returns the number of corners compared.
    u32 ExpectPathsAgree(const GroomBuildSource& source, const GroomStrandBuildSettings& settings,
                         const GroomBindingAsset& binding, std::span<const GroomRootTransform> transforms,
                         const GroomStrandSimulation* simulation, f32 heldTolerance = -1.0f,
                         const GroomCoatContext* coat = nullptr)
    {
        GroomStrandDeformation deformation;
        deformation.Binding = &binding;
        deformation.RootTransforms = transforms;

        std::vector<GroomStrandVertex> cpu;
        std::vector<u32> cpuIndices;
        std::vector<u32> cpuStrandFirst;
        std::vector<GroomCasterStrand> cpuCaster;
        const GroomStrandMeshStats cpuStats = BuildGroomStrandMesh(source, settings, cpu, cpuIndices, &deformation,
                                                                   coat, simulation, &cpuStrandFirst, &cpuCaster);

        std::vector<GroomStrandVertex> rest;
        std::vector<u32> restIndices;
        std::vector<u32> rootCurves;
        std::vector<u32> restStrandFirst;
        std::vector<GroomCasterStrand> restCaster;
        const GroomStrandMeshStats restStats =
            BuildGroomStrandRestMesh(source, settings, binding, rest, restIndices, rootCurves, coat, nullptr,
                                     &restStrandFirst, &restCaster);

        EXPECT_EQ(rest.size(), cpu.size()) << "the two paths must walk the same strands";
        EXPECT_EQ(restIndices, cpuIndices) << "and index them identically";
        // The shadow caster's order (#1533 E1) is built from these: both paths
        // must cast the same strands as the same subset.
        EXPECT_EQ(restStrandFirst, cpuStrandFirst) << "and group them into the same strands";
        EXPECT_EQ(restStrandFirst.size(), rootCurves.size()) << "one strand per root slot";
        // The caster's summaries are REST-space and come from the one walk, so
        // the two paths report them bit for bit, however the CPU path posed its
        // vertices (#1533).
        EXPECT_EQ(restCaster.size(), restStrandFirst.size()) << "one summary per emitted strand";
        EXPECT_EQ(cpuCaster.size(), cpuStrandFirst.size());
        for (sizet strand = 0; strand < std::min(restCaster.size(), cpuCaster.size()); ++strand)
        {
            EXPECT_EQ(restCaster[strand].Group, cpuCaster[strand].Group) << "strand " << strand;
            EXPECT_EQ(restCaster[strand].Role, cpuCaster[strand].Role) << "strand " << strand;
            EXPECT_EQ(std::bit_cast<u32>(restCaster[strand].Length), std::bit_cast<u32>(cpuCaster[strand].Length))
                << "strand " << strand;
            for (sizet m = 0; m < 6u; ++m)
            {
                EXPECT_EQ(std::bit_cast<u32>(restCaster[strand].Moments[m]), std::bit_cast<u32>(cpuCaster[strand].Moments[m]))
                    << "strand " << strand << " moment " << m;
            }
            EXPECT_TRUE(BitwiseEqual(restCaster[strand].BoundsMin, cpuCaster[strand].BoundsMin)) << "strand " << strand;
            EXPECT_TRUE(BitwiseEqual(restCaster[strand].BoundsMax, cpuCaster[strand].BoundsMax)) << "strand " << strand;
        }
        if (!rest.empty() && rest.size() == cpu.size())
        {
            const GroomCasterOrder restOrder = BuildGroomCasterOrder(rest, restIndices, restStrandFirst, restCaster);
            const GroomCasterOrder cpuOrder = BuildGroomCasterOrder(cpu, cpuIndices, cpuStrandFirst, cpuCaster);
            EXPECT_EQ(restOrder.Indices, cpuOrder.Indices);
            EXPECT_EQ(restOrder.Runs.size(), cpuOrder.Runs.size());
            for (sizet run = 0; run < std::min(restOrder.Runs.size(), cpuOrder.Runs.size()); ++run)
            {
                EXPECT_EQ(restOrder.Runs[run].FirstIndex, cpuOrder.Runs[run].FirstIndex) << "run " << run;
                EXPECT_EQ(restOrder.Runs[run].Prefix, cpuOrder.Runs[run].Prefix) << "run " << run;
            }
            // Lengths in the bind frame against lengths in the pose: a rotation
            // per root, so equal up to rounding -- and the simulation's
            // displacement, which stretches a strand by a hair at most.
            EXPECT_NEAR(restOrder.MeanRadius, cpuOrder.MeanRadius, 0.02f * cpuOrder.MeanRadius);
        }
        EXPECT_EQ(restStats.SegmentCount, cpuStats.SegmentCount);
        EXPECT_EQ(restStats.StrandsSelected, cpuStats.StrandsSelected);
        if (rest.size() != cpu.size())
        {
            return 0;
        }

        const GroomGuideInfluenceTable* table = simulation != nullptr ? simulation->Influence : nullptr;
        const u32 displacements =
            simulation != nullptr ? static_cast<u32>(simulation->Displacements.Displacements.size()) : 0u;
        GroomDeformBuffer buffer;
        buffer.Reset(GroomDeformBufferLayout::Make(static_cast<u32>(rootCurves.size()),
                                                   table != nullptr ? table->GetGuideCount() : 0u, displacements),
                     rootCurves, table);
        const GroomDeformFrameStats frame =
            buffer.PackFrame(rootCurves, binding, transforms, simulation, source.BaseCurveCount);
        EXPECT_EQ(frame.StrandsHeldAtRest, cpuStats.StrandsHeldAtRest);
        EXPECT_EQ(frame.StrandsSimulated, cpuStats.StrandsSimulated);
        EXPECT_EQ(frame.StrandsUnguided, cpuStats.StrandsUnguided);

        // The GPU path's stream, deformed on the CPU the way GroomStrand.glsl's
        // mode-1 branch deforms it -- the function the ray-traced proxy refits
        // from (#1533), so every case below also checks the proxy's geometry.
        std::vector<GroomStrandVertex> deformedRest;
        DeformGroomRestStream(buffer, rest, deformedRest);
        EXPECT_EQ(deformedRest.size(), rest.size());

        for (sizet i = 0; i < cpu.size(); ++i)
        {
            SCOPED_TRACE("corner " + std::to_string(i));
            const GroomStrandVertex& expected = cpu[i];
            const GroomStrandVertex& encoded = rest[i];
            // The lanes that mean the same thing on both paths.
            EXPECT_EQ(std::bit_cast<u32>(encoded.Side), std::bit_cast<u32>(expected.Side));
            EXPECT_EQ(std::bit_cast<u32>(encoded.Radius), std::bit_cast<u32>(expected.Radius));
            EXPECT_EQ(std::bit_cast<u32>(encoded.Coords.x), std::bit_cast<u32>(expected.Coords.x));
            EXPECT_EQ(std::bit_cast<u32>(encoded.Coords.y), std::bit_cast<u32>(expected.Coords.y));
            EXPECT_EQ(std::bit_cast<u32>(encoded.SegmentId), std::bit_cast<u32>(expected.SegmentId));
            EXPECT_EQ(std::bit_cast<u32>(encoded.Tint), std::bit_cast<u32>(expected.Tint));

            const GroomStrandVertex& gpu = deformedRest[i];
            const u32 root = static_cast<u32>(encoded.PrevPosition.x + 0.5f);
            const bool held = !transforms[rootCurves[root]].Valid;
            if (held && heldTolerance >= 0.0f)
            {
                EXPECT_LE(glm::length(gpu.Position - expected.Position), heldTolerance);
                EXPECT_LE(glm::length(gpu.Other - expected.Other), heldTolerance);
                EXPECT_LE(glm::length(gpu.PrevPosition - expected.PrevPosition), heldTolerance);
                continue;
            }
            EXPECT_TRUE(BitwiseEqual(gpu.Position, expected.Position))
                << "position (" << gpu.Position.x << ", " << gpu.Position.y << ", " << gpu.Position.z << ") vs ("
                << expected.Position.x << ", " << expected.Position.y << ", " << expected.Position.z << ")";
            EXPECT_TRUE(BitwiseEqual(gpu.Other, expected.Other)) << "other";
            EXPECT_TRUE(BitwiseEqual(gpu.PrevPosition, expected.PrevPosition)) << "previous position";
        }
        return static_cast<u32>(cpu.size());
    }
    // ── The caster pose's harness (#1533) ─────────────────────────────────────

    struct StrandSpec
    {
        std::vector<glm::vec3> Points;
        f32 Width = 1.0e-4f;
        u16 Group = 0;
    };

    [[nodiscard]] std::vector<glm::vec3> Line(const glm::vec3& root, const glm::vec3& direction, f32 length,
                                              u32 points = 6u)
    {
        std::vector<glm::vec3> out;
        for (u32 p = 0; p < points; ++p)
        {
            out.push_back(root + direction * (length * static_cast<f32>(p) / static_cast<f32>(points - 1u)));
        }
        return out;
    }

    [[nodiscard]] Ref<GroomAsset> BuildCoatFrom(const std::vector<StrandSpec>& strands,
                                                const std::vector<std::string>& groups)
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
        for (const StrandSpec& strand : strands)
        {
            const std::vector<f32> widths(strand.Points.size(), strand.Width);
            GroomCurveInput input;
            input.Points = strand.Points;
            input.Widths = widths;
            input.GroupId = ids[strand.Group];
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }
        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }

    // What GroomRenderPass builds a bound coat's caster from: the rest stream,
    // its caster order and the order's pose.
    struct CasterSetup
    {
        GroomStrandBuildSettings Settings;
        std::vector<GroomStrandVertex> Rest;
        std::vector<u32> RestIndices;
        std::vector<u32> RootCurves;
        std::vector<u32> RestFirst;
        std::vector<GroomCasterStrand> RestStrands;
        std::vector<u32> StrandOrder;
        GroomCasterOrder Order;
        GroomCasterPose Pose;
    };

    [[nodiscard]] CasterSetup MakeCasterSetup(const GroomAsset& groom, const GroomBindingAsset& binding,
                                              const GroomGuideInfluenceTable* influence = nullptr)
    {
        CasterSetup setup;
        setup.Settings.MaxStrands = groom.GetCurveCount();
        (void)BuildGroomStrandRestMesh(GroomBuildSource::FromAsset(groom), setup.Settings, binding, setup.Rest,
                                       setup.RestIndices, setup.RootCurves, nullptr, nullptr, &setup.RestFirst,
                                       &setup.RestStrands);
        setup.Order = BuildGroomCasterOrder(setup.Rest, setup.RestIndices, setup.RestFirst, setup.RestStrands,
                                            &setup.StrandOrder);
        setup.Pose = BuildGroomCasterPose(setup.Order.Runs, setup.StrandOrder, setup.RestStrands, setup.RootCurves,
                                          binding, CollectGroomCasterLocalBoxes(setup.Rest, setup.RestIndices, setup.RestFirst),
                                          influence);
        return setup;
    }

    // THE TRUTH, run by run: the coat the CPU deforms, segment by segment.
    struct RunTruth
    {
        std::array<f64, 6> Moments{};
        std::vector<glm::dvec3> Segments;
        f64 Length = 0.0;
        glm::vec3 Min{ std::numeric_limits<f32>::max() };
        glm::vec3 Max{ std::numeric_limits<f32>::lowest() };
    };

    [[nodiscard]] std::vector<RunTruth> MeasureTruth(const GroomAsset& groom, const GroomBindingAsset& binding,
                                                     const TArray<GroomRootTransform>& transforms,
                                                     const CasterSetup& setup,
                                                     const GroomStrandSimulation* simulation = nullptr)
    {
        GroomStrandDeformation deformation;
        deformation.Binding = &binding;
        deformation.RootTransforms = { transforms.GetData(), static_cast<sizet>(transforms.Num()) };
        std::vector<GroomStrandVertex> cpu;
        std::vector<u32> cpuIndices;
        std::vector<u32> cpuFirst;
        std::vector<u32> cpuCurves;
        (void)BuildGroomStrandMesh(GroomBuildSource::FromAsset(groom), setup.Settings, cpu, cpuIndices, &deformation,
                                   nullptr, simulation, &cpuFirst, nullptr, &cpuCurves);
        EXPECT_EQ(cpuFirst, setup.RestFirst) << "the two walks must emit the same strands";
        EXPECT_EQ(cpuCurves, setup.RootCurves) << "and report the same curve for each";
        std::vector<RunTruth> truth(setup.Order.Runs.size());
        for (sizet r = 0, first = 0; r < setup.Order.Runs.size(); first += setup.Order.Runs[r].Strands, ++r)
        {
            for (u32 k = 0; k < setup.Order.Runs[r].Strands; ++k)
            {
                const u32 strand = setup.StrandOrder[first + k];
                const u32 end = strand + 1u < cpuFirst.size() ? cpuFirst[strand + 1u] : static_cast<u32>(cpuIndices.size());
                for (u32 i = cpuFirst[strand]; i < end; i += 6u)
                {
                    const GroomStrandVertex& v = cpu[cpuIndices[i] / 4u * 4u];
                    const glm::dvec3 segment(v.Other - v.Position);
                    const f64 l = glm::length(segment);
                    if (l > 0.0)
                    {
                        const glm::dvec3 t = segment / l;
                        const std::array<f64, 6> outer{ t.x * t.x, t.y * t.y, t.z * t.z, t.x * t.y, t.x * t.z, t.y * t.z };
                        for (sizet m = 0; m < 6u; ++m)
                        {
                            truth[r].Moments[m] += l * outer[m];
                        }
                        truth[r].Segments.push_back(segment);
                        truth[r].Length += l;
                    }
                    truth[r].Min = glm::min(truth[r].Min, glm::min(v.Position, v.Other));
                    truth[r].Max = glm::max(truth[r].Max, glm::max(v.Position, v.Other));
                }
            }
        }
        return truth;
    }

    // What the truth projects to across a unit direction: each segment's part
    // perpendicular to it.
    [[nodiscard]] f64 ProjectedAcross(const RunTruth& truth, const glm::dvec3& d)
    {
        f64 sum = 0.0;
        for (const glm::dvec3& s : truth.Segments)
        {
            sum += glm::length(s - (glm::dot(s, d) * d));
        }
        return sum;
    }

    // What a posed run claims across a unit direction: DecideGroomCasterRun's
    // orthographic bound, its loss taken off.
    [[nodiscard]] f64 ClaimedAcross(const GroomCasterRun& run, const glm::vec3& d)
    {
        return std::max(0.0, static_cast<f64>(GroomShadowProjectedLengthLowerBound(run.TotalLength, run.Moments, d)) -
                                 static_cast<f64>(run.ProjectedLengthLoss));
    }

    [[nodiscard]] bool Holds(const GroomCasterRun& run, const RunTruth& truth, f32 slack = 1.0e-5f)
    {
        return glm::all(glm::lessThanEqual(run.BoundsMin, truth.Min + glm::vec3(slack))) &&
               glm::all(glm::greaterThanEqual(run.BoundsMax, truth.Max - glm::vec3(slack)));
    }

    [[nodiscard]] glm::vec3 UnitDirection(u32 a, u32 b)
    {
        const auto hash = [](u32 x, u32 y)
        {
            u32 h = x * 0x9E3779B9u ^ (y + 0x7F4A7C15u) * 0x85EBCA6Bu;
            h ^= h >> 16u;
            h *= 0x7FEB352Du;
            h ^= h >> 15u;
            return static_cast<f32>(h >> 8u) / static_cast<f32>(1u << 24u);
        };
        const f32 z = 2.0f * hash(a, b * 2u) - 1.0f;
        const f32 phi = 6.2831853f * hash(a, b * 2u + 1u);
        const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return { r * std::cos(phi), r * std::sin(phi), z };
    }

    [[nodiscard]] glm::mat4 About(const glm::vec3& pivot, f32 degrees, const glm::vec3& axis)
    {
        return glm::translate(glm::mat4(1.0f), pivot) * glm::rotate(glm::mat4(1.0f), glm::radians(degrees), axis) *
               glm::translate(glm::mat4(1.0f), -pivot);
    }

    // Both sources' posed runs: from the surface, as for a coat whose roots the
    // GPU evaluates, and from the CPU's root transforms.
    struct Posed
    {
        std::vector<GroomCasterRun> BySurface;
        std::vector<GroomCasterRun> ByRoots;
    };

    [[nodiscard]] Posed PoseBothWays(const CasterSetup& setup, const GroomDeformationInputs& inputs,
                                     const TArray<GroomRootTransform>& transforms,
                                     const GroomCasterPosePadding& padding = {})
    {
        Posed posed;
        const GroomCasterPoseSurface surface = BuildGroomCasterPoseSurface(setup.Pose, inputs.Surface);
        EXPECT_TRUE(PoseGroomCasterRunsBySurface(setup.Order.Runs, setup.Pose, surface, inputs, padding, posed.BySurface));
        EXPECT_TRUE(PoseGroomCasterRunsByRoots(setup.Order.Runs, setup.Pose,
                                               { transforms.GetData(), static_cast<sizet>(transforms.Num()) }, padding,
                                               posed.ByRoots));
        return posed;
    }
} // namespace

TEST(GroomGpuDeformation, ABentCoatReproducesTheCpuStreamExactly)
{
    const BoundScene scene = MakeBoundScene();
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };
    const u32 compared =
        ExpectPathsAgree(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{}, *scene.Binding,
                         transforms, nullptr);
    EXPECT_GT(compared, 0u);

    // NEGATIVE CONTROL: the bend really moved the coat, so the agreement above
    // is about deformed positions and not two copies of the bind pose.
    std::vector<GroomStrandVertex> unbent;
    std::vector<u32> indices;
    (void)BuildGroomStrandMesh(*scene.Groom, GroomStrandBuildSettings{}, unbent, indices);
    GroomStrandDeformation deformation{ scene.Binding.Raw(), transforms };
    std::vector<GroomStrandVertex> bent;
    (void)BuildGroomStrandMesh(*scene.Groom, GroomStrandBuildSettings{}, bent, indices, &deformation);
    u32 moved = 0;
    u32 withMotion = 0;
    for (sizet i = 0; i < bent.size(); ++i)
    {
        moved += glm::length(bent[i].Position - unbent[i].Position) > 1.0e-3f ? 1u : 0u;
        withMotion += glm::length(bent[i].Position - bent[i].PrevPosition) > 1.0e-3f ? 1u : 0u;
    }
    EXPECT_GT(moved, bent.size() / 4u) << "the fixture's bend did not move the coat";
    EXPECT_GT(withMotion, bent.size() / 4u) << "the fixture's previous pose is not a different pose";
}

TEST(GroomGpuDeformation, ASimulatedCoatReproducesTheCpuStreamExactly)
{
    const BoundScene scene = MakeBoundScene();
    const Simulation simulation = MakeSimulation(*scene.Groom);
    const GroomStrandSimulation view = simulation.View();
    ASSERT_TRUE(view.IsUsable(scene.Groom->GetCurveCount()));
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };
    EXPECT_GT(ExpectPathsAgree(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{}, *scene.Binding,
                               transforms, &view),
              0u);
}

namespace
{
    // A curled, waved, tip-tinted coat group (#1533): a little over two turns on
    // the fixture's 10 cm strands, which at 24 points is about ten points a turn.
    [[nodiscard]] GroomCoatGroupDesc CurledCoat()
    {
        GroomCoatGroupDesc desc;
        desc.CurlRadius = 0.004f;
        desc.CurlFrequency = 22.0f;
        desc.WaveAmplitude = 0.003f;
        desc.WaveFrequency = 15.0f;
        desc.TipTint = glm::vec3(0.5f, 0.4f, 0.3f);
        return desc;
    }

    [[nodiscard]] GroomCoatSettings CurledSettings()
    {
        GroomCoatSettings settings;
        settings.Enabled = true;
        settings.Seed = 3u;
        return settings;
    }
} // namespace

TEST(GroomGpuDeformation, ACurledCoatReproducesTheCpuStreamExactly)
{
    // #1533. Curl and wave are baked into the REST points by the one walk both
    // builds share, so the GPU path's bind-local stream carries them and the
    // per-frame deformation moves them rigidly with each root -- exactly what
    // the CPU path's root transform does to the same curled points. A curl
    // applied on one path only would fail every corner. The per-corner tint
    // rides the same way and is compared lane for lane.
    const GroomCoatGroupDesc curled = CurledCoat();
    const BoundScene scene = MakeBoundScene(24u, 24u, 55.0f, &curled);
    const GroomCoatSettings settings = CurledSettings();
    const GroomCoatContext coat{ &settings, scene.Groom->GetGroupCoats() };
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };
    EXPECT_GT(ExpectPathsAgree(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{}, *scene.Binding,
                               transforms, nullptr, -1.0f, &coat),
              0u);

    // NEGATIVE CONTROL: the curl really is in the GPU path's rest stream, so the
    // agreement above is about curled points.
    std::vector<GroomStrandVertex> plain;
    std::vector<GroomStrandVertex> curledRest;
    std::vector<u32> indices;
    std::vector<u32> roots;
    (void)BuildGroomStrandRestMesh(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{},
                                   *scene.Binding, plain, indices, roots);
    (void)BuildGroomStrandRestMesh(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{},
                                   *scene.Binding, curledRest, indices, roots, &coat);
    ASSERT_EQ(plain.size(), curledRest.size());
    u32 moved = 0;
    for (sizet i = 0; i < plain.size(); ++i)
    {
        moved += glm::length(curledRest[i].Position - plain[i].Position) > 1.0e-4f ? 1u : 0u;
    }
    EXPECT_GT(moved, static_cast<u32>(plain.size() / 2u)) << "the curl did not reach the rest stream";
}

TEST(GroomGpuDeformation, TheCurlRidesTheRootFrameUnderTwoPoses)
{
    // #1251's criterion 2 for the v4 shape terms: in each root's OWN frame a
    // curled strand is the same at every pose, because the curl was applied in
    // rest space and the body only ever moves the result rigidly. Two poses of
    // the hinge; per corner, conjugate(Rotation) * (P - Origin) agrees to float
    // precision while the world positions differ by centimetres.
    const GroomCoatGroupDesc curled = CurledCoat();
    const BoundScene bent = MakeBoundScene(24u, 24u, 55.0f, &curled);
    const BoundScene straighter = MakeBoundScene(24u, 24u, 20.0f, &curled);
    const GroomCoatSettings settings = CurledSettings();

    const auto build = [&settings](const BoundScene& scene)
    {
        const GroomCoatContext coat{ &settings, scene.Groom->GetGroupCoats() };
        const GroomStrandDeformation deformation{
            scene.Binding.Raw(),
            std::span<const GroomRootTransform>(scene.Transforms.GetData(), static_cast<sizet>(scene.Transforms.Num()))
        };
        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        (void)BuildGroomStrandMesh(*scene.Groom, GroomStrandBuildSettings{}, vertices, indices, &deformation, &coat);
        return vertices;
    };
    const std::vector<GroomStrandVertex> a = build(bent);
    const std::vector<GroomStrandVertex> b = build(straighter);
    ASSERT_EQ(a.size(), b.size());

    u32 compared = 0;
    u32 posesDiffer = 0;
    sizet corner = 0;
    for (u32 curve = 0; curve < bent.Groom->GetCurveCount(); ++curve)
    {
        const GroomRootTransform& ta = bent.Transforms[static_cast<i32>(curve)];
        const GroomRootTransform& tb = straighter.Transforms[static_cast<i32>(curve)];
        const u32 corners = (bent.Groom->GetCurvePointCount(curve) - 1u) * 4u;
        for (u32 c = 0; c < corners; ++c, ++corner)
        {
            if (!ta.Valid || !tb.Valid)
            {
                continue;
            }
            const glm::vec3 localA = glm::conjugate(ta.Rotation) * (a[corner].Position - ta.Origin);
            const glm::vec3 localB = glm::conjugate(tb.Rotation) * (b[corner].Position - tb.Origin);
            EXPECT_LT(glm::length(localA - localB), 1.0e-5f) << "curve " << curve << " corner " << c;
            posesDiffer += glm::length(a[corner].Position - b[corner].Position) > 1.0e-3f ? 1u : 0u;
            ++compared;
        }
    }
    EXPECT_EQ(corner, a.size());
    EXPECT_GT(compared, 0u);
    EXPECT_GT(posesDiffer, compared / 4u) << "the two poses must genuinely differ, or this compares one pose twice";
}

TEST(GroomGpuDeformation, ABudgetThatDropsGuidesRenormalisesExactlyAsTheCpuDoes)
{
    // Every third slot out of the frame's budget: the remaining guides must be
    // renormalised by the weight actually applied, on both paths, and a strand
    // whose only guide was dropped must be left on its bound rest shape.
    const BoundScene scene = MakeBoundScene(40u);
    const Simulation simulation = MakeSimulation(*scene.Groom, 3u, false);
    const GroomStrandSimulation view = simulation.View();
    ASSERT_TRUE(view.IsUsable(scene.Groom->GetCurveCount()));
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };
    EXPECT_GT(ExpectPathsAgree(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{}, *scene.Binding,
                               transforms, &view),
              0u);
}

TEST(GroomGpuDeformation, AStrandBudgetReproducesTheCpuStreamExactly)
{
    // A stride over the coat and a segment cap that lands mid-curve: the two
    // paths share one walk, so they must stop on the same segment of the same
    // strand and pack the same roots.
    const BoundScene scene = MakeBoundScene(40u, 8u);
    const Simulation simulation = MakeSimulation(*scene.Groom);
    const GroomStrandSimulation view = simulation.View();
    GroomStrandBuildSettings settings;
    settings.MaxStrands = 13u;
    settings.MaxSegments = 60u;
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };
    EXPECT_GT(ExpectPathsAgree(GroomBuildSource::FromAsset(*scene.Groom), settings, *scene.Binding, transforms, &view),
              0u);
}

TEST(GroomGpuDeformation, ARemappedCurveSetReadsItsSourceCurvesRootsAndGuides)
{
    // A card LOD level draws its OWN curves with the root frames and guides of
    // the base curves its source map names (#1252). The frame buffer is packed
    // by root slot -> base curve, so a map that is not the identity is what
    // catches a pack that indexed by the drawn curve instead.
    const BoundScene scene = MakeBoundScene();
    const Simulation simulation = MakeSimulation(*scene.Groom);
    const GroomStrandSimulation view = simulation.View();
    const u32 count = scene.Groom->GetCurveCount();
    std::vector<u32> reversed(count);
    for (u32 curve = 0; curve < count; ++curve)
    {
        reversed[curve] = count - 1u - curve;
    }
    GroomBuildSource source = GroomBuildSource::FromAsset(*scene.Groom);
    source.SourceCurves = reversed;
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };
    EXPECT_GT(ExpectPathsAgree(source, GroomStrandBuildSettings{}, *scene.Binding, transforms, &view), 0u);
}

// #1533: the ray-traced proxy of a bound coat refits each frame from its REST
// stream -- roots evaluated for its own root slots only, one frame packed, the
// stream deformed on the CPU -- instead of rebuilding the coat from the groom,
// which walked every curve several times a frame (~90 ms on the showcase dog).
// The proxy it converts must be the one the rebuild converted, bit for bit.
TEST(GroomGpuDeformation, TheRayTracedProxyRefitsFromItsRestStreamExactly)
{
    const BoundScene scene = MakeBoundScene(40u, 8u);
    const Simulation simulation = MakeSimulation(*scene.Groom);
    const GroomStrandSimulation view = simulation.View();
    GroomStrandBuildSettings build;
    build.MaxStrands = 13u; // a tier's budget: a subset of the coat
    build.MaxWidthCompensation = 1.0f;
    const GroomBuildSource source = GroomBuildSource::FromAsset(*scene.Groom);

    // The rebuild, from every root.
    GroomStrandDeformation deformation;
    deformation.Binding = scene.Binding.Raw();
    deformation.RootTransforms = { scene.Transforms.GetData(), static_cast<sizet>(scene.Transforms.Num()) };
    std::vector<GroomStrandVertex> rebuilt;
    std::vector<u32> rebuiltIndices;
    const GroomStrandMeshStats rebuiltStats =
        BuildGroomStrandMesh(source, build, rebuilt, rebuiltIndices, &deformation, nullptr, &view);
    ASSERT_FALSE(rebuilt.empty());
    ASSERT_LT(rebuiltStats.StrandsSelected, scene.Groom->GetCurveCount()) << "the budget must select a subset";

    // The refit: the rest stream, its root slots' roots only, one packed frame.
    std::vector<GroomStrandVertex> rest;
    std::vector<u32> restIndices;
    std::vector<u32> rootCurves;
    (void)BuildGroomStrandRestMesh(source, build, *scene.Binding, rest, restIndices, rootCurves);
    GroomStrandRequest request;
    request.Groom = scene.Groom;
    request.Binding = scene.Binding;
    request.GpuRootFrames = true;
    request.GpuRootInputs.Surface = scene.Grid.View(2u);
    request.GpuRootInputs.Skinning = scene.Grid.Skinning(scene.Palette, scene.PrevPalette, true);
    request.GpuRootInputs.HasHistory = true;
    TArray<GroomRootTransform> scratch;
    const std::span<const GroomRootTransform> roots =
        GroomCpuRootTransforms(request, std::span<const u32>{ rootCurves }, scratch);
    const auto evaluated = std::count_if(roots.begin(), roots.end(), [](const GroomRootTransform& t)
                                         { return t.Valid; });
    EXPECT_EQ(static_cast<sizet>(evaluated), rootCurves.size()) << "only the stream's own roots are evaluated";

    GroomDeformBuffer buffer;
    buffer.Reset(GroomDeformBufferLayout::Make(static_cast<u32>(rootCurves.size()), view.Influence->GetGuideCount(),
                                               static_cast<u32>(view.Displacements.Displacements.size())),
                 rootCurves, view.Influence);
    (void)buffer.PackFrame(rootCurves, *scene.Binding, roots, &view, scene.Groom->GetCurveCount());
    std::vector<GroomStrandVertex> refit;
    DeformGroomRestStream(buffer, rest, refit);

    GroomProxyConversionSettings conversion;
    std::vector<Vertex> rebuiltProxy;
    std::vector<Vertex> refitProxy;
    std::vector<u32> rebuiltProxyIndices;
    std::vector<u32> refitProxyIndices;
    (void)ConvertGroomStrandMeshToProxy(rebuilt, conversion, rebuiltProxy, rebuiltProxyIndices);
    (void)ConvertGroomStrandMeshToProxy(refit, conversion, refitProxy, refitProxyIndices);
    ASSERT_FALSE(rebuiltProxy.empty());
    ASSERT_EQ(refitProxy.size(), rebuiltProxy.size());
    EXPECT_EQ(std::memcmp(refitProxy.data(), rebuiltProxy.data(), rebuiltProxy.size() * sizeof(Vertex)), 0)
        << "the refit traces a different coat than the rebuild";
    EXPECT_EQ(refitProxyIndices, rebuiltProxyIndices);

    // #1533: the proxy's own path. Roots reset and written for the selection
    // alone are the same records; the two points a segment's ribbons need,
    // converted in parallel, are the same bytes as the whole stream deformed
    // and converted one segment at a time.
    TArray<GroomRootTransform> selectedScratch;
    const std::span<const GroomRootTransform> selectedRoots =
        GroomCpuRootTransforms(request, std::span<const u32>{ rootCurves }, selectedScratch, /*onlyCurvesDefined*/ true);
    for (const u32 curve : rootCurves)
    {
        // Field by field: the record's tail after its two flags is padding.
        const GroomRootTransform& a = selectedRoots[curve];
        const GroomRootTransform& b = roots[curve];
        EXPECT_TRUE(Math::BitwiseEqual(a.Origin, b.Origin) && Math::BitwiseEqual(a.Rotation, b.Rotation) &&
                    Math::BitwiseEqual(a.PrevOrigin, b.PrevOrigin) && Math::BitwiseEqual(a.PrevRotation, b.PrevRotation) &&
                    a.Valid == b.Valid && a.Held == b.Held)
            << "curve " << curve << " evaluated differently for its selection alone";
    }
    std::vector<GroomProxySegment> segments;
    DeformGroomRestStreamSegments(buffer, rest, segments);
    std::vector<Vertex> fusedProxy;
    std::vector<u32> fusedProxyIndices;
    const GroomProxyMeshStats fusedStats = ConvertGroomProxySegments(segments, conversion, fusedProxy, fusedProxyIndices);
    ASSERT_EQ(fusedProxy.size(), refitProxy.size());
    EXPECT_EQ(std::memcmp(fusedProxy.data(), refitProxy.data(), refitProxy.size() * sizeof(Vertex)), 0)
        << "the proxy's two-point deform traces a different coat than the whole stream";
    EXPECT_EQ(fusedProxyIndices, refitProxyIndices);
    EXPECT_EQ(fusedStats.SegmentCount, segments.size());

    // A CPU-only buffer (no byte image) gives the evaluator the same records.
    GroomDeformBuffer cpuOnly;
    cpuOnly.Reset(GroomDeformBufferLayout::Make(static_cast<u32>(rootCurves.size()), view.Influence->GetGuideCount(),
                                                static_cast<u32>(view.Displacements.Displacements.size())),
                  rootCurves, view.Influence, /*cpuOnly*/ true);
    (void)cpuOnly.PackFrame(rootCurves, *scene.Binding, selectedRoots, &view, scene.Groom->GetCurveCount());
    EXPECT_TRUE(cpuOnly.GetBytes().empty()) << "a CPU-only buffer still built a byte image";
    std::vector<GroomProxySegment> cpuOnlySegments;
    DeformGroomRestStreamSegments(cpuOnly, rest, cpuOnlySegments);
    ASSERT_EQ(cpuOnlySegments.size(), segments.size());
    EXPECT_EQ(std::memcmp(cpuOnlySegments.data(), segments.data(), segments.size() * sizeof(GroomProxySegment)), 0)
        << "the CPU-only buffer deforms the proxy differently";
}

TEST(GroomGpuDeformation, AHeldRootRestsWhereTheCpuPathHoldsIt)
{
    // A root with no deformed frame is held at REST. The CPU path returns the
    // rest point untouched; the GPU path carries the bind-local point through
    // the bind frame and back, which is the same point to within rounding —
    // the one comparison in this file that needs a tolerance.
    BoundScene scene = MakeBoundScene();
    u32 held = 0;
    for (i32 curve = 0; curve < scene.Transforms.Num(); curve += 3)
    {
        scene.Transforms[curve].Valid = false;
        scene.Transforms[curve].Held = true;
        ++held;
    }
    ASSERT_GT(held, 0u);
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };
    EXPECT_GT(ExpectPathsAgree(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{}, *scene.Binding,
                               transforms, nullptr, 1.0e-6f),
              0u);
}

TEST(GroomGpuDeformation, TheCoatBakeSeesThePoseTheGpuDraws)
{
    // The coat self-shadow bake (#1426) reads the drawn pose as centrelines.
    // On the GPU path those are evaluated from the frame buffer; they must be
    // the centrelines the CPU path's stream carries, or the shadow sits where
    // the coat is not.
    const BoundScene scene = MakeBoundScene();
    const Simulation simulation = MakeSimulation(*scene.Groom);
    const GroomStrandSimulation view = simulation.View();
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };

    GroomStrandDeformation deformation{ scene.Binding.Raw(), transforms };
    std::vector<GroomStrandVertex> cpu;
    std::vector<u32> indices;
    (void)BuildGroomStrandMesh(*scene.Groom, GroomStrandBuildSettings{}, cpu, indices, &deformation, nullptr, &view);
    std::vector<GroomCoatShadow::CoatSegment> cpuPose;
    GroomCoatShadow::CoatPoseFromStrandVertices(cpu, cpuPose);

    std::vector<GroomStrandVertex> rest;
    std::vector<u32> rootCurves;
    std::vector<GroomRestPoseSegment> poseSegments;
    (void)BuildGroomStrandRestMesh(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{},
                                   *scene.Binding, rest, indices, rootCurves, nullptr, &poseSegments);
    GroomDeformBuffer buffer;
    buffer.Reset(GroomDeformBufferLayout::Make(static_cast<u32>(rootCurves.size()), simulation.Table->GetGuideCount(),
                                               static_cast<u32>(simulation.Displacements.size())),
                 rootCurves, simulation.Table.Raw());
    (void)buffer.PackFrame(rootCurves, *scene.Binding, transforms, &view, scene.Groom->GetCurveCount());
    std::vector<GroomCoatShadow::CoatSegment> gpuPose;
    EvaluateGroomDeformedPose(buffer, poseSegments, gpuPose);

    ASSERT_EQ(gpuPose.size(), cpuPose.size());
    ASSERT_FALSE(gpuPose.empty());
    for (sizet s = 0; s < gpuPose.size(); ++s)
    {
        EXPECT_TRUE(BitwiseEqual(gpuPose[s].A, cpuPose[s].A) && BitwiseEqual(gpuPose[s].B, cpuPose[s].B) &&
                    std::bit_cast<u32>(gpuPose[s].RadiusA) == std::bit_cast<u32>(cpuPose[s].RadiusA) &&
                    std::bit_cast<u32>(gpuPose[s].RadiusB) == std::bit_cast<u32>(cpuPose[s].RadiusB))
            << "segment " << s;
    }
    // And the drift the bake measures against a pose captured from one path
    // is zero against the other.
    std::vector<glm::vec3> baked;
    GroomCoatShadow::CaptureCoatPose(std::span<const GroomCoatShadow::CoatSegment>{ cpuPose }, baked);
    const f32 drift = GroomCoatShadow::MaxCoatPoseDrift(baked, std::span<const GroomCoatShadow::CoatSegment>{ gpuPose });
    EXPECT_EQ(std::bit_cast<u32>(drift), 0u) << "drift " << drift;
}

// #1533 E1: the simulated and unguided strand counts are a lookup per drawn
// strand, so PackFrame keeps its last count until the guides it counted change
// -- and a frame whose budget drops guides is recounted, not served stale.
TEST(GroomGpuDeformation, TheDirectPoseBuilderIsTheRestStreamsPoseWithoutItsRibbons)
{
    // #1533. A rest stream is built WITHOUT its pose segments now, and the first
    // posed coat bake builds them straight from the walk (BuildGroomRestPoseSegments)
    // instead of building the whole four-corner ribbon mesh again and dropping it.
    // They must be the segments the rest builder emits beside its ribbons, bit for
    // bit -- one walk and one bind arithmetic -- on a curled, authored coat, whole
    // and under a strand budget; and the builder's one allocation of any size is
    // its output, reserved to the planned count and never regrown.
    const GroomCoatGroupDesc curled = CurledCoat();
    const BoundScene scene = MakeBoundScene(24u, 24u, 55.0f, &curled);
    const GroomCoatSettings coatSettings = CurledSettings();
    const GroomCoatContext coat{ &coatSettings, scene.Groom->GetGroupCoats() };
    const GroomBuildSource source = GroomBuildSource::FromAsset(*scene.Groom);
    for (const u32 budget : { GroomStrandBuildSettings{}.MaxStrands, 10u })
    {
        SCOPED_TRACE(budget);
        GroomStrandBuildSettings build;
        build.MaxStrands = budget;
        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        std::vector<u32> roots;
        std::vector<GroomRestPoseSegment> reference;
        const GroomStrandMeshStats full =
            BuildGroomStrandRestMesh(source, build, *scene.Binding, vertices, indices, roots, &coat, &reference);
        std::vector<GroomRestPoseSegment> direct;
        const GroomStrandMeshStats lean = BuildGroomRestPoseSegments(source, build, *scene.Binding, &coat, direct);

        ASSERT_FALSE(reference.empty());
        ASSERT_EQ(direct.size(), reference.size());
        for (sizet i = 0; i < direct.size(); ++i)
        {
            const GroomRestPoseSegment& a = direct[i];
            const GroomRestPoseSegment& b = reference[i];
            ASSERT_TRUE(a.RootSlot == b.RootSlot && BitwiseEqual(a.Local0, b.Local0) && BitwiseEqual(a.Local1, b.Local1) &&
                        std::bit_cast<u32>(a.T0) == std::bit_cast<u32>(b.T0) &&
                        std::bit_cast<u32>(a.T1) == std::bit_cast<u32>(b.T1) &&
                        std::bit_cast<u32>(a.Radius0) == std::bit_cast<u32>(b.Radius0) &&
                        std::bit_cast<u32>(a.Radius1) == std::bit_cast<u32>(b.Radius1))
                << "segment " << i;
        }
        EXPECT_EQ(lean.SegmentCount, full.SegmentCount);
        EXPECT_EQ(lean.StrandsSelected, full.StrandsSelected);
        EXPECT_EQ(lean.VertexBytes, 0u) << "the direct builder built ribbon vertices";
        EXPECT_EQ(lean.IndexBytes, 0u) << "the direct builder built ribbon indices";
        EXPECT_EQ(direct.capacity(), direct.size()) << "reserved to the planned count and never regrown";
        std::printf("[groom-gpu] %zu pose segments: %zu bytes direct; the rest builder also needed %zu + %zu bytes of "
                    "ribbons\n",
                    direct.size(), direct.capacity() * sizeof(GroomRestPoseSegment),
                    vertices.capacity() * sizeof(GroomStrandVertex), indices.capacity() * sizeof(u32));
    }

    // The rest builder's refusal, for its reason: a binding that does not span
    // the groom builds nothing.
    const BoundScene other = MakeBoundScene(12u, 12u, 55.0f);
    std::vector<GroomRestPoseSegment> none;
    const GroomStrandMeshStats refused =
        BuildGroomRestPoseSegments(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{}, *other.Binding,
                                   nullptr, none);
    EXPECT_TRUE(none.empty());
    EXPECT_EQ(refused.SegmentCount, 0u);
}

TEST(GroomGpuDeformation, TheSimulatedStrandCountIsRecountedOnlyWhenTheGuidesChange)
{
    const BoundScene scene = MakeBoundScene(40u, 12u);
    const Simulation simulation = MakeSimulation(*scene.Groom);
    std::vector<GroomStrandVertex> rest;
    std::vector<u32> indices;
    std::vector<u32> rootCurves;
    (void)BuildGroomStrandRestMesh(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{},
                                   *scene.Binding, rest, indices, rootCurves);
    const GroomDeformBufferLayout layout =
        GroomDeformBufferLayout::Make(static_cast<u32>(rootCurves.size()), simulation.Table->GetGuideCount(),
                                      static_cast<u32>(simulation.Displacements.size()));
    GroomDeformBuffer buffer;
    buffer.Reset(layout, rootCurves, simulation.Table.Raw());
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };
    const u32 baseCurves = scene.Groom->GetCurveCount();
    const auto direct = [&](const GroomStrandSimulation& view)
    {
        u32 simulated = 0;
        for (const u32 curve : rootCurves)
        {
            simulated += HasGroomGuideInfluence(view, curve) ? 1u : 0u;
        }
        return simulated;
    };

    const GroomStrandSimulation all = simulation.View();
    const GroomDeformFrameStats first = buffer.PackFrame(rootCurves, *scene.Binding, transforms, &all, baseCurves);
    const GroomDeformFrameStats again = buffer.PackFrame(rootCurves, *scene.Binding, transforms, &all, baseCurves);
    EXPECT_EQ(first.StrandsSimulated, direct(all));
    EXPECT_EQ(first.StrandsSimulated + first.StrandsUnguided, rootCurves.size());
    EXPECT_EQ(again.StrandsSimulated, first.StrandsSimulated);
    EXPECT_EQ(again.StrandsUnguided, first.StrandsUnguided);

    // The budget keeps only the first guide this frame: the same table, a
    // different slot-to-guide map. (A strand stays simulated while ANY of its
    // guides moved, so dropping a few could leave every count where it was.)
    Simulation thinned = simulation;
    for (sizet slot = 1; slot < thinned.GuideOfSlot.size(); ++slot)
    {
        thinned.GuideOfSlot[slot] = GroomNoGuide;
    }
    const GroomStrandSimulation fewer = thinned.View();
    const GroomDeformFrameStats dropped = buffer.PackFrame(rootCurves, *scene.Binding, transforms, &fewer, baseCurves);
    EXPECT_EQ(dropped.StrandsSimulated, direct(fewer)) << "a frame with fewer guides was served the last count";
    EXPECT_EQ(dropped.StrandsSimulated + dropped.StrandsUnguided, rootCurves.size());
    EXPECT_LT(dropped.StrandsSimulated, first.StrandsSimulated) << "the fixture's dropped guides moved no strand";
}

TEST(GroomGpuDeformation, AFrameSendsSixtyFourBytesAStrandNotAWholeStream)
{
    // The point of the issue, as a number: what a bound coat sends per frame
    // is its root records, its guide slots and its displacement samples — not
    // 256 bytes for every segment.
    const BoundScene scene = MakeBoundScene(40u, 12u);
    const Simulation simulation = MakeSimulation(*scene.Groom);
    const GroomStrandSimulation view = simulation.View();

    std::vector<GroomStrandVertex> rest;
    std::vector<u32> indices;
    std::vector<u32> rootCurves;
    const GroomStrandMeshStats stats =
        BuildGroomStrandRestMesh(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{},
                                 *scene.Binding, rest, indices, rootCurves);
    const u32 displacements = static_cast<u32>(simulation.Displacements.size());
    const GroomDeformBufferLayout layout = GroomDeformBufferLayout::Make(
        static_cast<u32>(rootCurves.size()), simulation.Table->GetGuideCount(), displacements);

    EXPECT_EQ(rootCurves.size(), stats.StrandsSelected);
    EXPECT_EQ(layout.DynamicBytes(), static_cast<u64>(rootCurves.size()) * sizeof(GroomDeformRootRecord) +
                                         static_cast<u64>(simulation.Table->GetGuideCount()) *
                                             sizeof(GroomDeformSlotRecord) +
                                         static_cast<u64>(displacements) * sizeof(GroomDeformDisplacementRecord));
    // Eleven segments a strand here, so the stream is 44 times the roots'
    // share; a real coat's strands are longer still.
    EXPECT_LT(layout.DynamicBytes() * 8u, stats.VertexBytes)
        << "a frame's deformation must be a small fraction of the stream it moves";
}

TEST(GroomGpuDeformation, AnUnusableSimulationLeavesTheCoatOnItsBoundRestShape)
{
    // An unusable simulation is ABSENT, never partly applied — the rule the
    // CPU build states. The pack must report it unsimulated, and every point
    // must be exactly the bound-only one.
    const BoundScene scene = MakeBoundScene();
    Simulation simulation = MakeSimulation(*scene.Groom);
    simulation.GuideOfSlot.pop_back(); // no longer spans the table
    const GroomStrandSimulation view = simulation.View();
    ASSERT_FALSE(view.IsUsable(scene.Groom->GetCurveCount()));
    const std::span<const GroomRootTransform> transforms{ scene.Transforms.GetData(),
                                                          static_cast<sizet>(scene.Transforms.Num()) };

    std::vector<GroomStrandVertex> rest;
    std::vector<u32> indices;
    std::vector<u32> rootCurves;
    (void)BuildGroomStrandRestMesh(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{},
                                   *scene.Binding, rest, indices, rootCurves);
    GroomDeformBuffer buffer;
    buffer.Reset(GroomDeformBufferLayout::Make(static_cast<u32>(rootCurves.size()), simulation.Table->GetGuideCount(),
                                               static_cast<u32>(simulation.Displacements.size())),
                 rootCurves, simulation.Table.Raw());
    const GroomDeformFrameStats frame =
        buffer.PackFrame(rootCurves, *scene.Binding, transforms, &view, scene.Groom->GetCurveCount());
    EXPECT_FALSE(frame.Simulated);
    EXPECT_EQ(frame.StrandsSimulated, 0u);

    // THROUGH BOTH PATHS WITH THE UNUSABLE VIEW, not with no simulation: the
    // CPU build must refuse it and the pack must refuse it, and then every
    // corner must still agree bit for bit — which it can only do if neither
    // path applied any of it.
    EXPECT_GT(ExpectPathsAgree(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{}, *scene.Binding,
                               transforms, &view),
              0u);
}

TEST(GroomGpuDeformation, ABindingThatDoesNotSpanTheGroomBuildsNothing)
{
    // The rest stream is written in the binding's bind frames. A binding for a
    // different curve count would place strands on someone else's triangles,
    // so the build refuses and the pass falls back to the CPU path — which
    // refuses the same binding and draws the coat at rest.
    const BoundScene scene = MakeBoundScene();
    const Ref<GroomAsset> other = MakeCoat(scene.Groom->GetCurveCount() + 3u);
    ASSERT_TRUE(other);
    std::vector<GroomStrandVertex> rest;
    std::vector<u32> indices;
    std::vector<u32> rootCurves;
    const GroomStrandMeshStats stats = BuildGroomStrandRestMesh(GroomBuildSource::FromAsset(*other),
                                                                GroomStrandBuildSettings{}, *scene.Binding, rest,
                                                                indices, rootCurves);
    EXPECT_TRUE(rest.empty());
    EXPECT_TRUE(rootCurves.empty());
    EXPECT_EQ(stats.SegmentCount, 0u);
}

TEST(GroomGpuDeformation, TheCasterRunsArePosedAsTheCpuDeformsTheCoat)
{
    // #1533: a bound coat's shadow views decide each run's share from its
    // moments and box, which the walk records at REST. They are re-posed every
    // frame -- from the root transforms when the CPU evaluated the roots, from
    // the surface when the GPU does -- and both must describe the coat the CPU
    // deforms: its posed segments' second moments, and a box holding every
    // posed point. Half the hinge bent 55 degrees, under a groom mapping that is
    // not the identity.
    BoundScene scene = MakeBoundScene(48u, 6u, 55.0f);
    ASSERT_TRUE(scene.Groom && scene.Binding);
    GroomDeformationInputs inputs;
    inputs.Surface = scene.Grid.View(2u);
    inputs.Skinning = scene.Grid.Skinning(scene.Palette, scene.PrevPalette, true);
    inputs.HasHistory = true;
    inputs.SurfaceToGroom = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.02f, 0.01f)), 0.3f,
                                        glm::vec3(0.0f, 1.0f, 0.0f));
    (void)EvaluateGroomRootTransforms(*scene.Groom, *scene.Binding, inputs, std::nullopt, scene.Transforms);
    const CasterSetup setup = MakeCasterSetup(*scene.Groom, *scene.Binding);
    ASSERT_TRUE(setup.Pose.IsUsable());
    const std::vector<RunTruth> truth = MeasureTruth(*scene.Groom, *scene.Binding, scene.Transforms, setup);
    const Posed posed = PoseBothWays(setup, inputs, scene.Transforms);
    for (sizet r = 0; r < setup.Order.Runs.size(); ++r)
    {
        SCOPED_TRACE("run " + std::to_string(r));
        const f64 trace = truth[r].Moments[0] + truth[r].Moments[1] + truth[r].Moments[2];
        f64 restError = 0.0;
        for (sizet m = 0; m < 6u; ++m)
        {
            EXPECT_NEAR(posed.ByRoots[r].Moments[m], truth[r].Moments[m], 1.0e-4 * trace) << "by roots, moment " << m;
            EXPECT_NEAR(posed.BySurface[r].Moments[m], truth[r].Moments[m], 1.0e-4 * trace) << "by surface, moment " << m;
            restError = std::max(restError, std::abs(setup.Order.Runs[r].Moments[m] - truth[r].Moments[m]));
        }
        // THE CONTROL: the rest moments are not the bent coat's.
        EXPECT_GT(restError, 0.1 * trace) << "the bend left the run's moments where they were: nothing was tested";
        EXPECT_TRUE(Holds(posed.ByRoots[r], truth[r])) << "the roots' posed box does not hold the posed coat";
        EXPECT_TRUE(Holds(posed.BySurface[r], truth[r])) << "the surface's posed box does not hold the posed coat";
        EXPECT_LT(posed.BySurface[r].ProjectedLengthLoss, 0.01f * posed.BySurface[r].TotalLength)
            << "healthy triangles should cost the claim almost nothing";
    }
}

TEST(GroomGpuDeformation, APosedRunsMomentsAreItsStrandsOwnNotASamplesEstimate)
{
    // #1533 review, finding 2. 128 short strands lying along x on the half of a
    // hinged grid that folds up 90 degrees, so they end along y, and one strand
    // nine times as long standing along y on the half that stays. Every posed
    // strand lies along y: across x the run projects to its whole length.
    // A 128-strand sample of the run that missed the long strand, its change
    // scaled from the sample's length to the run's, gave a moment matrix with
    // a negative eigenvalue and a "lower bound" across x above the run's whole
    // length (146 against 137 in the review's units). The pose now turns every
    // strand by its own root triangle's frame: its moments are the run's own.
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid);
    constexpr f32 kShort = 0.01f;
    std::vector<StrandSpec> strands;
    for (u32 i = 0; i < 128u; ++i)
    {
        const glm::vec3 root{ 0.56f + 0.36f * static_cast<f32>(i % 16u) / 15.0f, 0.0f,
                              0.06f + 0.88f * static_cast<f32>(i / 16u) / 7.0f };
        strands.push_back({ Line(root, glm::vec3(1.0f, 0.0f, 0.0f), kShort) });
    }
    strands.push_back({ Line(glm::vec3(0.25f, 0.0f, 0.5f), glm::vec3(0.0f, 1.0f, 0.0f), 9.0f * kShort) });
    const Ref<GroomAsset> groom = BuildCoatFrom(strands, { "leg_guard" });
    ASSERT_TRUE(groom);
    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(2u), "TestBody", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;
    const std::vector<glm::mat4> palette{ glm::mat4(1.0f),
                                          About(glm::vec3(0.5f, 0.0f, 0.0f), 90.0f, glm::vec3(0.0f, 0.0f, 1.0f)) };
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(palette, palette, true);
    TArray<GroomRootTransform> transforms;
    (void)EvaluateGroomRootTransforms(*groom, *binding, inputs, std::nullopt, transforms);
    const CasterSetup setup = MakeCasterSetup(*groom, *binding);
    ASSERT_EQ(setup.Order.Runs.size(), 1u);
    ASSERT_EQ(setup.Order.Runs[0].Strands, 129u);
    const std::vector<RunTruth> truth = MeasureTruth(*groom, *binding, transforms, setup);
    const Posed posed = PoseBothWays(setup, inputs, transforms);

    const glm::vec3 x(1.0f, 0.0f, 0.0f);
    const f64 actual = ProjectedAcross(truth[0], glm::dvec3(x));
    const f64 trace = truth[0].Moments[0] + truth[0].Moments[1] + truth[0].Moments[2];
    for (const auto* run : { &posed.BySurface[0], &posed.ByRoots[0] })
    {
        for (sizet m = 0; m < 6u; ++m)
        {
            EXPECT_NEAR(run->Moments[m], truth[0].Moments[m], 1.0e-4 * trace) << "moment " << m;
        }
        EXPECT_GE(run->Moments[0], -1.0e-6f * static_cast<f32>(trace)) << "a posed xx moment below zero";
        EXPECT_LE(ClaimedAcross(*run, x), actual * (1.0 + 1.0e-5)) << "the claim across x exceeds what the run projects to";
        EXPECT_GE(ClaimedAcross(*run, x), 0.98 * actual) << "the exact moments should claim nearly all of it";
    }

    // THE CONTROL: the sampled form the pose replaced, over the same run --
    // the 128 short strands as the sample, their change scaled by L / L_s.
    std::array<f64, 6> change{};
    f64 sampleLength = 0.0;
    for (u32 strand = 0; strand < static_cast<u32>(setup.RestStrands.size()); ++strand)
    {
        const GroomCasterStrand& summary = setup.RestStrands[strand];
        if (summary.Length > 2.0f * kShort)
        {
            continue; // the long strand: outside the sample
        }
        const u32 curve = setup.RootCurves[strand];
        const glm::mat3 turn = glm::mat3_cast(transforms[static_cast<i32>(curve)].Rotation *
                                              glm::conjugate(binding->GetRoot(curve).RestRotation));
        const std::array<f32, 6> turned = RotateGroomCasterMoments(summary.Moments, turn);
        for (sizet m = 0; m < 6u; ++m)
        {
            change[m] += static_cast<f64>(turned[m]) - summary.Moments[m];
        }
        sampleLength += summary.Length;
    }
    const f64 scale = static_cast<f64>(setup.Order.Runs[0].TotalLength) / sampleLength;
    const f64 sampledXX = setup.Order.Runs[0].Moments[0] + scale * change[0];
    const f64 sampledClaim = setup.Order.Runs[0].TotalLength - sampledXX;
    std::printf("[groom-gpu] posed run across x: actual %.5f, exact claim %.5f, sampled form %.5f (xx %.5f)\n", actual,
                ClaimedAcross(posed.BySurface[0], x), sampledClaim, sampledXX);
    EXPECT_LT(sampledXX, 0.0) << "the control: the sampled form should have a negative moment here";
    EXPECT_GT(sampledClaim, 1.05 * actual) << "the control: the sampled form should over-credit the run";
}

TEST(GroomGpuDeformation, APosedRunIsExactUnderMixedBonesAndUnequalStrands)
{
    // #1533 review, finding 2: unequal lengths, mixed orientations and
    // heterogeneous root motion -- three bones blended smoothly across the
    // grid, each turned about its own pivot and axis, under a groom mapping
    // that is neither a similarity nor the identity. Each run's posed moments
    // must be the deformed coat's, its claim across every direction at most
    // what it projects to, and its box must hold every deformed point.
    GridSurface grid = MakeGrid(16u);
    for (u32 v = 0; v < grid.VertexCount(); ++v)
    {
        const f32 x = grid.Positions[v].x;
        const f32 a = glm::smoothstep(0.2f, 0.5f, x);
        const f32 b = glm::smoothstep(0.5f, 0.8f, x);
        // Two of the three bones at any point, blended.
        if (x < 0.5f)
        {
            grid.SetInfluence(v, 0u, 1.0f - a, 1u, a);
        }
        else
        {
            grid.SetInfluence(v, 1u, 1.0f - b, 2u, b);
        }
    }
    const std::vector<glm::mat4> palette{
        About(glm::vec3(0.2f, 0.0f, 0.3f), 25.0f, glm::normalize(glm::vec3(0.3f, 0.2f, 1.0f))),
        About(glm::vec3(0.5f, 0.1f, 0.6f), -50.0f, glm::normalize(glm::vec3(1.0f, 0.4f, 0.1f))),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.02f, 0.05f, -0.01f)) *
            About(glm::vec3(0.8f, 0.0f, 0.2f), 70.0f, glm::normalize(glm::vec3(0.1f, 1.0f, 0.5f)))
    };
    std::vector<StrandSpec> strands;
    for (u32 i = 0; i < 600u; ++i)
    {
        const glm::vec3 root{ 0.05f + 0.9f * UnitDirection(11u, i).x * 0.5f + 0.45f, 0.0f,
                              0.05f + 0.9f * (UnitDirection(13u, i).y * 0.5f + 0.5f) };
        // Combed mostly along +x, out of the surface, with a spread: anisotropic,
        // so the bones' turns move the moments.
        const glm::vec3 u = UnitDirection(17u, i);
        const glm::vec3 direction{ 1.0f, 0.2f + 0.5f * std::abs(u.y), 0.4f * u.z };
        const f32 length = 0.005f + 0.055f * (UnitDirection(19u, i).z * 0.5f + 0.5f);
        strands.push_back({ Line(glm::clamp(root, glm::vec3(0.02f), glm::vec3(0.98f)), glm::normalize(direction), length),
                            1.0e-4f, static_cast<u16>(i % 2u) });
    }
    const Ref<GroomAsset> groom = BuildCoatFrom(strands, { "back_undercoat", "back_guard" });
    ASSERT_TRUE(groom);
    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(3u), "TestBody", GroomBindingBuildSettings{}, binding, stats,
                                           reason))
        << reason;
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(3u);
    inputs.Skinning = grid.Skinning(palette, palette, true);
    inputs.SurfaceToGroom = glm::translate(glm::mat4(1.0f), glm::vec3(0.01f, -0.02f, 0.03f)) *
                            glm::rotate(glm::mat4(1.0f), 0.4f, glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f))) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(1.2f, 0.9f, 1.05f));
    TArray<GroomRootTransform> transforms;
    (void)EvaluateGroomRootTransforms(*groom, *binding, inputs, std::nullopt, transforms);
    const CasterSetup setup = MakeCasterSetup(*groom, *binding);
    ASSERT_EQ(setup.Order.Runs.size(), 2u);
    const std::vector<RunTruth> truth = MeasureTruth(*groom, *binding, transforms, setup);
    const Posed posed = PoseBothWays(setup, inputs, transforms);
    for (sizet r = 0; r < truth.size(); ++r)
    {
        SCOPED_TRACE("run " + std::to_string(r));
        const f64 trace = truth[r].Moments[0] + truth[r].Moments[1] + truth[r].Moments[2];
        f64 worst = 0.0;
        f64 restError = 0.0;
        for (sizet m = 0; m < 6u; ++m)
        {
            restError = std::max(restError, std::abs(setup.Order.Runs[r].Moments[m] - truth[r].Moments[m]));
        }
        for (const auto* run : { &posed.BySurface[r], &posed.ByRoots[r] })
        {
            for (sizet m = 0; m < 6u; ++m)
            {
                EXPECT_NEAR(run->Moments[m], truth[r].Moments[m], 1.0e-4 * trace) << "moment " << m;
            }
            EXPECT_TRUE(Holds(*run, truth[r])) << "a posed box does not hold the deformed coat";
            for (u32 k = 0; k < 64u; ++k)
            {
                const glm::vec3 d = UnitDirection(23u, k);
                const f64 actual = ProjectedAcross(truth[r], glm::dvec3(d));
                EXPECT_LE(ClaimedAcross(*run, d), actual * (1.0 + 1.0e-5) + 1.0e-7) << "direction " << k;
                worst = std::max(worst, ClaimedAcross(*run, d) / actual);
            }
        }
        std::printf("[groom-gpu] mixed bones, run %zu: largest claim/actual %.4f, rest moments off by %.3f of the trace, "
                    "loss %.5f of %.4f\n",
                    r, worst, restError / trace, posed.BySurface[r].ProjectedLengthLoss, posed.BySurface[r].TotalLength);
        // THE CONTROL: the pose moved the moments, so the exactness is not the
        // rest numbers passing through.
        EXPECT_GT(restError, 0.05 * trace) << "the pose left the run's moments where they were: nothing was tested";
    }
}

TEST(GroomGpuDeformation, ABlendedRootTriangleKeepsItsFrameAndItsStrandsReach)
{
    // #1533 review, finding 3: a small root triangle in the yz-plane, every
    // corner weighted half to a bone turned +60 degrees about x and half to one
    // turned -60. The blend is diag(1, 0.5, 0.5): the triangle shrinks and its
    // frame does not turn, so a strand of reach l along y keeps its tip at
    // y = l, while each bone's image of the strand reaches only y = 0.5 l. The
    // posed box -- the triangle's hull plus its turned offsets -- holds it.
    GridSurface surface;
    constexpr f32 kSide = 1.0e-3f;
    surface.Positions = { { 0.0f, -kSide, -kSide }, { 0.0f, kSide, -kSide }, { 0.0f, 0.0f, kSide } };
    surface.Indices = { 0u, 1u, 2u };
    surface.Influences.assign(3u * 32u, std::byte{ 0 });
    for (u32 v = 0; v < 3u; ++v)
    {
        surface.SetInfluence(v, 0u, 0.5f, 1u, 0.5f);
    }
    const std::vector<glm::mat4> palette{ glm::rotate(glm::mat4(1.0f), glm::radians(60.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
                                          glm::rotate(glm::mat4(1.0f), glm::radians(-60.0f), glm::vec3(1.0f, 0.0f, 0.0f)) };
    constexpr f32 kReach = 0.1f;
    const glm::vec3 root = (surface.Positions[0] + surface.Positions[1] + surface.Positions[2]) / 3.0f;
    const Ref<GroomAsset> groom = BuildCoatFrom({ { Line(root, glm::vec3(0.0f, 1.0f, 0.0f), kReach), 1.0e-5f } },
                                                { "ear_longhair" });
    ASSERT_TRUE(groom);
    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, surface.View(2u), "TestBody", GroomBindingBuildSettings{}, binding,
                                           stats, reason))
        << reason;
    GroomDeformationInputs inputs;
    inputs.Surface = surface.View(2u);
    inputs.Skinning = surface.Skinning(palette, palette, true);
    TArray<GroomRootTransform> transforms;
    (void)EvaluateGroomRootTransforms(*groom, *binding, inputs, std::nullopt, transforms);
    ASSERT_TRUE(transforms[0].Valid) << "the shrunk triangle must still frame";
    const CasterSetup setup = MakeCasterSetup(*groom, *binding);
    const std::vector<RunTruth> truth = MeasureTruth(*groom, *binding, transforms, setup);
    const Posed posed = PoseBothWays(setup, inputs, transforms);
    std::printf("[groom-gpu] blended root: posed box y [%.4f, %.4f] (roots [%.4f, %.4f]), deformed strand y [%.4f, %.4f]\n",
                posed.BySurface[0].BoundsMin.y, posed.BySurface[0].BoundsMax.y, posed.ByRoots[0].BoundsMin.y,
                posed.ByRoots[0].BoundsMax.y, truth[0].Min.y, truth[0].Max.y);
    EXPECT_GT(truth[0].Max.y, 0.9f * kReach) << "the blended frame should keep the strand upright";
    EXPECT_TRUE(Holds(posed.BySurface[0], truth[0])) << "the surface's posed box does not hold the deformed strand";
    EXPECT_TRUE(Holds(posed.ByRoots[0], truth[0])) << "the roots' posed box does not hold the deformed strand";

    // THE CONTROL: the bones' images of the strand, padded by the 15% of its
    // reach the replaced policy allowed, fall short of where it is.
    f32 boneTipY = std::numeric_limits<f32>::lowest();
    for (const glm::mat4& bone : palette)
    {
        boneTipY = std::max(boneTipY, glm::vec3(bone * glm::vec4(root + glm::vec3(0.0f, kReach, 0.0f), 1.0f)).y);
    }
    EXPECT_LT(boneTipY + 0.15f * kReach, truth[0].Max.y - 0.1f * kReach)
        << "the control: the bones' images plus 15% should miss the blended strand's tip";
}

TEST(GroomGpuDeformation, AnIllConditionedRootTriangleEarnsNoCreditAndKeepsItsFullReach)
{
    // #1533 review: a root triangle that frames on the CPU but so thinly that
    // the GPU's float arithmetic could frame it another way round. Its strands
    // earn no projected length (their whole length is the run's loss) and its
    // box is its hull padded by their full reach, joined with the rest box --
    // while a healthy triangle beside it, in a run of its own, is credited.
    GridSurface surface;
    constexpr f32 kSliver = 2.0e-8f;
    surface.Positions = { { 0.0f, 0.0f, 0.0f }, { 0.01f, 0.0f, 0.0f }, { 0.005f, 0.0f, kSliver }, { 0.1f, 0.0f, 0.0f }, { 0.11f, 0.0f, 0.0f }, { 0.105f, 0.0f, 0.01f } };
    surface.Indices = { 0u, 2u, 1u, 3u, 5u, 4u };
    surface.Influences.assign(6u * 32u, std::byte{ 0 });
    for (u32 v = 0; v < 6u; ++v)
    {
        surface.SetInfluence(v, 0u, 1.0f);
    }
    const std::vector<glm::mat4> palette{ glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f)) };
    const glm::vec3 sliverRoot = (surface.Positions[0] + surface.Positions[1] + surface.Positions[2]) / 3.0f;
    const glm::vec3 healthyRoot = (surface.Positions[3] + surface.Positions[4] + surface.Positions[5]) / 3.0f;
    const glm::vec3 lean = glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f));
    const Ref<GroomAsset> groom =
        BuildCoatFrom({ { Line(sliverRoot, lean, 0.05f), 1.0e-4f, 0u }, { Line(healthyRoot, lean, 0.05f), 1.0e-4f, 1u } },
                      { "belly_longhair", "chest_longhair" });
    ASSERT_TRUE(groom);
    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats stats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, surface.View(1u), "TestBody", GroomBindingBuildSettings{}, binding,
                                           stats, reason))
        << reason;
    GroomDeformationInputs inputs;
    inputs.Surface = surface.View(1u);
    inputs.Skinning = surface.Skinning(palette, palette, true);
    TArray<GroomRootTransform> transforms;
    (void)EvaluateGroomRootTransforms(*groom, *binding, inputs, std::nullopt, transforms);
    const CasterSetup setup = MakeCasterSetup(*groom, *binding);
    ASSERT_EQ(setup.Order.Runs.size(), 2u);
    const std::vector<RunTruth> truth = MeasureTruth(*groom, *binding, transforms, setup);
    const Posed posed = PoseBothWays(setup, inputs, transforms);
    const auto runOf = [&](u16 group)
    {
        for (sizet r = 0; r < setup.Order.Runs.size(); ++r)
        {
            if (setup.Order.Runs[r].Group == group)
            {
                return r;
            }
        }
        return sizet{ 0 };
    };
    const sizet sliver = runOf(0u);
    const sizet healthy = runOf(1u);
    const glm::vec3 posedSliver[3] = { glm::vec3(palette[0] * glm::vec4(surface.Positions[0], 1.0f)),
                                       glm::vec3(palette[0] * glm::vec4(surface.Positions[1], 1.0f)),
                                       glm::vec3(palette[0] * glm::vec4(surface.Positions[2], 1.0f)) };
    EXPECT_GT(GroomCasterFrameTolerance(posedSliver[0], posedSliver[1], posedSliver[2], kGroomCasterPositionTolerance),
              kGroomCasterMaxFrameError)
        << "the sliver should be too ill-conditioned to trust";
    std::printf("[groom-gpu] sliver root: loss %.4f of %.4f; healthy root: loss %.6f of %.4f\n",
                posed.BySurface[sliver].ProjectedLengthLoss, posed.BySurface[sliver].TotalLength,
                posed.BySurface[healthy].ProjectedLengthLoss, posed.BySurface[healthy].TotalLength);
    EXPECT_GE(posed.BySurface[sliver].ProjectedLengthLoss, 0.999f * posed.BySurface[sliver].TotalLength)
        << "the sliver's strands earned projected length";
    EXPECT_TRUE(Holds(posed.BySurface[sliver], truth[sliver])) << "the sliver's box does not hold its strand";
    EXPECT_TRUE(glm::all(glm::lessThanEqual(posed.BySurface[sliver].BoundsMin, setup.Order.Runs[sliver].BoundsMin)) &&
                glm::all(glm::greaterThanEqual(posed.BySurface[sliver].BoundsMax, setup.Order.Runs[sliver].BoundsMax)))
        << "the sliver's box must hold its rest box, where the GPU may hold it";
    EXPECT_LT(posed.BySurface[healthy].ProjectedLengthLoss, 0.01f * posed.BySurface[healthy].TotalLength)
        << "the healthy triangle should be credited";
    EXPECT_TRUE(Holds(posed.BySurface[healthy], truth[healthy]));
}

TEST(GroomGpuDeformation, TheCasterRunsHoldEverySimulatedPointAndPayForTheShortening)
{
    // #1533 review: the simulation moves points after the root transform. The
    // posed box is padded by the farthest a guide was displaced, and the claim
    // gives up what the displacement can shorten: a segment's projected length
    // shrinks by at most the change in displacement between its ends. Two
    // ways, both held to the simulated coat: per strand the largest variation
    // of any guide (a pose built without the influence table), and per strand
    // its own slots' variation, blended by its weights (built with it).
    BoundScene scene = MakeBoundScene(40u, 8u);
    ASSERT_TRUE(scene.Groom && scene.Binding);
    const Simulation simulation = MakeSimulation(*scene.Groom);
    const GroomStrandSimulation view = simulation.View();
    ASSERT_TRUE(view.IsUsable(scene.Groom->GetCurveCount()));
    GroomDeformationInputs inputs;
    inputs.Surface = scene.Grid.View(2u);
    inputs.Skinning = scene.Grid.Skinning(scene.Palette, scene.PrevPalette, true);
    const CasterSetup setup = MakeCasterSetup(*scene.Groom, *scene.Binding);
    const std::vector<RunTruth> truth = MeasureTruth(*scene.Groom, *scene.Binding, scene.Transforms, setup, &view);
    const GroomCasterPosePadding padding =
        MeasureGroomCasterPosePadding(&view, scene.Groom->GetCurveGroupIds(), nullptr, 0.0f);
    const Posed posed = PoseBothWays(setup, inputs, scene.Transforms, padding);
    GroomCasterPosePadding noVariation = padding;
    noVariation.Variation.fill(0.0f);
    const Posed unpaid = PoseBothWays(setup, inputs, scene.Transforms, noVariation);
    for (sizet r = 0; r < truth.size(); ++r)
    {
        SCOPED_TRACE("run " + std::to_string(r));
        f64 unpaidWorst = 0.0;
        for (const auto* run : { &posed.BySurface[r], &posed.ByRoots[r] })
        {
            EXPECT_TRUE(Holds(*run, truth[r])) << "a posed box does not hold the simulated coat";
            for (u32 k = 0; k < 64u; ++k)
            {
                const glm::vec3 d = UnitDirection(29u, k);
                EXPECT_LE(ClaimedAcross(*run, d), ProjectedAcross(truth[r], glm::dvec3(d)) * (1.0 + 1.0e-5) + 1.0e-7)
                    << "direction " << k;
            }
        }
        for (u32 k = 0; k < 64u; ++k)
        {
            const glm::vec3 d = UnitDirection(29u, k);
            unpaidWorst = std::max(unpaidWorst, ClaimedAcross(unpaid.BySurface[r], d) / ProjectedAcross(truth[r], glm::dvec3(d)));
        }
        std::printf("[groom-gpu] simulated coat, run %zu: loss %.4f of %.4f; without the variation term the largest "
                    "claim/actual is %.4f\n",
                    r, posed.BySurface[r].ProjectedLengthLoss, posed.BySurface[r].TotalLength, unpaidWorst);
        EXPECT_GT(unpaidWorst, 1.0) << "the control: without the variation term the claim should exceed the coat somewhere";
    }

    // PER SLOT: the strands' own guides' variation, blended by their weights.
    // On a gentler copy of the same motion (a tenth of it), so the claim stays
    // above zero and holding it to the coat means something: at full strength
    // either loss exceeds the run's length and the claim is zero.
    Simulation gentle = simulation;
    for (glm::vec3& p : gentle.Displacements)
    {
        p *= 0.1f;
    }
    for (glm::vec3& p : gentle.PrevDisplacements)
    {
        p *= 0.1f;
    }
    const GroomStrandSimulation gentleView = gentle.View();
    const std::vector<RunTruth> gentleTruth = MeasureTruth(*scene.Groom, *scene.Binding, scene.Transforms, setup, &gentleView);
    const GroomCasterPosePadding gentlePadding =
        MeasureGroomCasterPosePadding(&gentleView, scene.Groom->GetCurveGroupIds(), nullptr, 0.0f);
    ASSERT_TRUE(gentlePadding.SlotsComplete) << "every guide is simulated: every slot has a displacement";
    const CasterSetup slotted = MakeCasterSetup(*scene.Groom, *scene.Binding, simulation.Table.Raw());
    ASSERT_TRUE(slotted.Pose.SlotWeightsKnown);
    const Posed byRole = PoseBothWays(setup, inputs, scene.Transforms, gentlePadding);
    const Posed bySlot = PoseBothWays(slotted, inputs, scene.Transforms, gentlePadding);
    GroomCasterPosePadding halfSlots = gentlePadding;
    for (f32& v : halfSlots.SlotVariation)
    {
        v *= 0.5f;
    }
    const Posed halfPaid = PoseBothWays(slotted, inputs, scene.Transforms, halfSlots);
    f64 slotTotal = 0.0;
    f64 roleTotal = 0.0;
    f64 halfWorst = 0.0;
    for (sizet r = 0; r < gentleTruth.size(); ++r)
    {
        SCOPED_TRACE("per slot, run " + std::to_string(r));
        ASSERT_LT(bySlot.BySurface[r].ProjectedLengthLoss, bySlot.BySurface[r].TotalLength)
            << "the per-slot claim should stay above zero, or holding it to the coat proves nothing";
        for (const auto* run : { &bySlot.BySurface[r], &bySlot.ByRoots[r] })
        {
            EXPECT_TRUE(Holds(*run, gentleTruth[r])) << "a per-slot posed box does not hold the simulated coat";
            for (u32 k = 0; k < 64u; ++k)
            {
                const glm::vec3 d = UnitDirection(29u, k);
                EXPECT_LE(ClaimedAcross(*run, d), ProjectedAcross(gentleTruth[r], glm::dvec3(d)) * (1.0 + 1.0e-5) + 1.0e-7)
                    << "direction " << k;
            }
        }
        for (u32 k = 0; k < 64u; ++k)
        {
            const glm::vec3 d = UnitDirection(29u, k);
            halfWorst =
                std::max(halfWorst, ClaimedAcross(halfPaid.BySurface[r], d) / ProjectedAcross(gentleTruth[r], glm::dvec3(d)));
        }
        slotTotal += bySlot.BySurface[r].ProjectedLengthLoss;
        roleTotal += byRole.BySurface[r].ProjectedLengthLoss;
        std::printf("[groom-gpu] gentle coat, run %zu: per-slot loss %.4f, per-role %.4f, of %.4f\n", r,
                    bySlot.BySurface[r].ProjectedLengthLoss, byRole.BySurface[r].ProjectedLengthLoss,
                    bySlot.BySurface[r].TotalLength);
        EXPECT_LE(bySlot.BySurface[r].ProjectedLengthLoss, byRole.BySurface[r].ProjectedLengthLoss * 1.0001f)
            << "the per-slot loss should never exceed the per-role one";
    }
    std::printf("[groom-gpu] gentle coat: per-slot loss %.4f against per-role %.4f; with half the per-slot "
                "variation the largest claim/actual is %.4f\n",
                slotTotal, roleTotal, halfWorst);
    // Each guide here is displaced by a different amount, so a strand's own
    // guides vary less than the largest of the coat's: a per-slot pose that
    // pays the per-role loss has fallen back without saying so.
    EXPECT_LT(slotTotal, roleTotal) << "the per-slot bound should be tighter than the per-role one on this coat";
    EXPECT_GT(halfWorst, 1.0) << "the control: half the per-slot variation should let the claim exceed the coat somewhere";

    // A slot the budget left out renormalises its strands' weights: the
    // per-slot bound no longer describes the blend, and the pose pays the
    // per-role bound instead.
    const Simulation dropped = MakeSimulation(*scene.Groom, 3u);
    const GroomStrandSimulation droppedView = dropped.View();
    ASSERT_TRUE(droppedView.IsUsable(scene.Groom->GetCurveCount()));
    const GroomCasterPosePadding droppedPadding =
        MeasureGroomCasterPosePadding(&droppedView, scene.Groom->GetCurveGroupIds(), nullptr, 0.0f);
    EXPECT_FALSE(droppedPadding.SlotsComplete);
    const std::vector<RunTruth> droppedTruth =
        MeasureTruth(*scene.Groom, *scene.Binding, scene.Transforms, slotted, &droppedView);
    const Posed droppedBySlot = PoseBothWays(slotted, inputs, scene.Transforms, droppedPadding);
    const Posed droppedByRole = PoseBothWays(setup, inputs, scene.Transforms, droppedPadding);
    for (sizet r = 0; r < droppedTruth.size(); ++r)
    {
        SCOPED_TRACE("dropped slots, run " + std::to_string(r));
        for (const auto* run : { &droppedBySlot.BySurface[r], &droppedBySlot.ByRoots[r] })
        {
            EXPECT_TRUE(Holds(*run, droppedTruth[r])) << "a posed box does not hold the renormalised coat";
            for (u32 k = 0; k < 64u; ++k)
            {
                const glm::vec3 d = UnitDirection(29u, k);
                EXPECT_LE(ClaimedAcross(*run, d), ProjectedAcross(droppedTruth[r], glm::dvec3(d)) * (1.0 + 1.0e-5) + 1.0e-7)
                    << "direction " << k;
            }
        }
        EXPECT_FLOAT_EQ(droppedBySlot.BySurface[r].ProjectedLengthLoss, droppedByRole.BySurface[r].ProjectedLengthLoss)
            << "with a slot left out the pose should pay the per-role bound";
    }
}

TEST(GroomGpuDeformation, TheSkeletonBoundsEveryRootTheKernelWrites)
{
    // #1533 E1: while GroomRootFrames.comp evaluates a coat's drawn roots, the
    // CPU holds none of them, so the posed box the shadow pass culls the coat
    // by comes from the bones: each bone's rest box of the corners it moves,
    // posed. Every root the reference evaluates must lie inside it -- for a bent
    // pose and a groom mapping that is not the identity.
    const BoundScene scene = MakeBoundScene(48u, 6u, 55.0f);
    ASSERT_TRUE(scene.Groom && scene.Binding);
    const u32 curves = scene.Groom->GetCurveCount();
    std::vector<u32> rootCurves(curves);
    std::iota(rootCurves.begin(), rootCurves.end(), 0u);

    GroomDeformationInputs inputs;
    inputs.Surface = scene.Grid.View(2u);
    inputs.Skinning = scene.Grid.Skinning(scene.Palette, scene.PrevPalette, true);
    inputs.HasHistory = true;
    inputs.SurfaceToGroom = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.02f, 0.01f)), 0.3f,
                                        glm::vec3(0.0f, 1.0f, 0.0f));
    TArray<GroomRootTransform> posed;
    (void)EvaluateGroomRootTransforms(*scene.Groom, *scene.Binding, inputs, std::nullopt, posed);
    ASSERT_EQ(posed.Num(), static_cast<i32>(curves));

    const GroomRootBoneBounds bounds =
        BuildGroomRootBoneBounds(rootCurves, *scene.Binding, inputs.Surface, inputs.Skinning,
                                 static_cast<u32>(scene.Palette.size()), curves);
    glm::vec3 lo(0.0f);
    glm::vec3 hi(0.0f);
    ASSERT_TRUE(PoseGroomRootBoneBounds(bounds, scene.Palette, inputs.SurfaceToGroom, lo, hi));

    // Two float orderings of the same blend, so a root on a face of the box may
    // land a rounding error outside it.
    constexpr f32 kSlack = 1.0e-5f;
    const auto inside = [kSlack](const glm::vec3& p, const glm::vec3& boxMin, const glm::vec3& boxMax)
    {
        return glm::all(glm::greaterThanEqual(p, boxMin - glm::vec3(kSlack))) &&
               glm::all(glm::lessThanEqual(p, boxMax + glm::vec3(kSlack)));
    };
    for (i32 curve = 0; curve < posed.Num(); ++curve)
    {
        SCOPED_TRACE("curve " + std::to_string(curve));
        ASSERT_TRUE(posed[curve].Valid) << "the reference held a root on a healthy triangle";
        EXPECT_TRUE(inside(posed[curve].Origin, lo, hi));
    }

    // THE CONTROL: the same boxes at rest do not hold the bent coat. A box that
    // ignored the palette -- or one so loose it held anything -- would pass the
    // loop above just as well.
    const std::vector<glm::mat4> rest(scene.Palette.size(), glm::mat4(1.0f));
    glm::vec3 restLo(0.0f);
    glm::vec3 restHi(0.0f);
    ASSERT_TRUE(PoseGroomRootBoneBounds(bounds, rest, inputs.SurfaceToGroom, restLo, restHi));
    u32 outsideRest = 0;
    for (const GroomRootTransform& root : posed)
    {
        outsideRest += inside(root.Origin, restLo, restHi) ? 0u : 1u;
    }
    EXPECT_GT(outsideRest, 0u);
}
