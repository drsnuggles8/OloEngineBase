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
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomGpuDeformation.h"
#include "OloEngine/Groom/GroomGuideInfluence.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
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

    [[nodiscard]] BoundScene MakeBoundScene(u32 strands = 24u, u32 points = 6u)
    {
        BoundScene scene;
        scene.Grid = MakeGrid(8u);
        WeightAsHinge(scene.Grid);
        scene.Groom = MakeCoat(strands, points, 0.1f);
        EXPECT_TRUE(scene.Groom);
        std::string reason;
        GroomBindingBuildStats stats;
        EXPECT_TRUE(GroomBindingBuilder::Build(*scene.Groom, scene.Grid.View(2u), "TestBody",
                                               GroomBindingBuildSettings{}, scene.Binding, stats, reason))
            << reason;

        scene.Palette = BentPalette(55.0f);
        scene.PrevPalette = BentPalette(40.0f);
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

    // One corner of the GPU path's stream, reconstructed the way
    // GroomStrand.glsl's mode-1 branch reconstructs it.
    struct GpuCorner
    {
        glm::vec3 Position;
        glm::vec3 Other;
        glm::vec3 PrevPosition;
    };

    [[nodiscard]] GpuCorner ReconstructCorner(const GroomDeformBuffer& buffer, const GroomStrandVertex& rest)
    {
        const u32 root = static_cast<u32>(rest.PrevPosition.x + 0.5f);
        const f32 tSelf = rest.Coords.x;
        const f32 tOther = rest.PrevPosition.y;
        const bool atP1 = rest.PrevPosition.z > 0.5f;
        const glm::vec3 self = EvaluateGroomDeformedPoint(buffer, root, rest.Position, tSelf, false);
        const glm::vec3 otherEnd = EvaluateGroomDeformedPoint(buffer, root, rest.Other, tOther, false);
        const glm::vec3 delta = atP1 ? (self - otherEnd) : (otherEnd - self);
        return { self, self + delta, EvaluateGroomDeformedPoint(buffer, root, rest.Position, tSelf, true) };
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
                         const GroomStrandSimulation* simulation, f32 heldTolerance = -1.0f)
    {
        GroomStrandDeformation deformation;
        deformation.Binding = &binding;
        deformation.RootTransforms = transforms;

        std::vector<GroomStrandVertex> cpu;
        std::vector<u32> cpuIndices;
        const GroomStrandMeshStats cpuStats =
            BuildGroomStrandMesh(source, settings, cpu, cpuIndices, &deformation, nullptr, simulation);

        std::vector<GroomStrandVertex> rest;
        std::vector<u32> restIndices;
        std::vector<u32> rootCurves;
        const GroomStrandMeshStats restStats =
            BuildGroomStrandRestMesh(source, settings, binding, rest, restIndices, rootCurves);

        EXPECT_EQ(rest.size(), cpu.size()) << "the two paths must walk the same strands";
        EXPECT_EQ(restIndices, cpuIndices) << "and index them identically";
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

            const GpuCorner gpu = ReconstructCorner(buffer, encoded);
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

    EXPECT_GT(ExpectPathsAgree(GroomBuildSource::FromAsset(*scene.Groom), GroomStrandBuildSettings{}, *scene.Binding,
                               transforms, nullptr),
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
