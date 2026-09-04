// OLO_TEST_LAYER: plumbing
//
// #978 — the acceleration-structure scene manager's device-free half.
//
// WHAT THIS FILE COVERS, and it is deliberately the whole of what a machine
// with no ray-tracing device can honestly cover:
//
//   * the policy — geometry classification, the refit-vs-rebuild heuristic,
//     the TLAS rebuild-vs-update decision;
//   * the packing arithmetic that silently truncates if it is wrong — the
//     8-bit instance mask and the 24-bit instanceCustomIndex;
//   * the GPU Scene seam, driven through a REAL GPUScene rather than a fake of
//     one: extraction, commit, slot walk, and the stale/reused-slot rule.
//
// SUBSTITUTION, named rather than left implicit (substituted-seams-compound.md):
// the backend below is a recording fake, so nothing here can see a wrong
// VkAccelerationStructureInstanceKHR layout, a wrong scratch or AS alignment, a
// compaction that retires storage still referenced, a missing AS-build ->
// AS-read barrier, or a ray that hits the wrong triangle. Those are pinned ONLY
// by RayTracingDeviceTest.cpp, which builds real structures and traces real
// rays, and which SKIPs on every machine without a ray-tracing device — which
// is every CI runner this project has.
//
// The GPUScene here is real and is driven WITHOUT InitializeGPU: extraction and
// commit are pure CPU, so the records, the generations and the retirement clock
// are the production ones. That is the seam that matters, and faking it is what
// would have hidden the reused-slot bug this file's last test is about.

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace RT = OloEngine::RayTracing;

    namespace
    {
        // A recording backend. It answers IsBlasResident from what it was
        // asked to build, so the scene's "drop an instance whose BLAS is
        // missing" rule is exercised for real rather than short-circuited.
        class FakeRayTracingBackend final : public RT::IRayTracingBackend
        {
          public:
            RT::Capabilities Caps;
            std::vector<RT::BlasBuildRequest> Builds;
            std::vector<RT::GeometryKey> Retires;
            std::vector<RT::InstanceRecord> LastInstances;
            RT::TlasBuildReason LastRequestedReason = RT::TlasBuildReason::FirstBuild;
            u32 TlasBuildCalls = 0;
            u32 BarrierCalls = 0;
            // Set to fail the Nth build so the "instance whose BLAS never
            // landed must not reach the TLAS" path can be driven.
            bool FailAllBuilds = false;

            FakeRayTracingBackend()
            {
                Caps.Supported = true;
                Caps.Reason = RT::UnsupportedReason::None;
                Caps.Properties.MinScratchOffsetAlignment = 128;
                Caps.Properties.MaxInstanceCount = 1u << 24;
            }

            [[nodiscard]] RT::Capabilities GetCapabilities() const override
            {
                return Caps;
            }

            u32 RecordBlasBuilds(std::span<const RT::BlasBuildRequest> requests) override
            {
                if (FailAllBuilds)
                {
                    return 0;
                }
                Builds.insert(Builds.end(), requests.begin(), requests.end());
                for (const RT::BlasBuildRequest& request : requests)
                {
                    m_Resident.push_back(request.Key);
                }
                return static_cast<u32>(requests.size());
            }

            void RetireBlas(const RT::GeometryKey& key) override
            {
                Retires.push_back(key);
                std::erase(m_Resident, key);
            }

            [[nodiscard]] bool IsBlasResident(const RT::GeometryKey& key) const override
            {
                return std::ranges::find(m_Resident, key) != m_Resident.end();
            }

            RT::TlasBuildReason RecordTlasBuild(std::span<const RT::InstanceRecord> instances,
                                                RT::TlasBuildReason requested) override
            {
                ++TlasBuildCalls;
                LastRequestedReason = requested;
                LastInstances.assign(instances.begin(), instances.end());
                return requested;
            }

            [[nodiscard]] u64 GetTlasDeviceAddress() const override
            {
                return TlasBuildCalls > 0 ? 0xDEADBEEFull : 0ull;
            }

            void RecordBuildToReadBarrier() override
            {
                ++BarrierCalls;
            }

            void PublishStats(RT::SceneStats&) const override
            {
            }

            void Shutdown() override
            {
                m_Resident.clear();
            }

            void ClearRecording()
            {
                Builds.clear();
                Retires.clear();
                LastInstances.clear();
                TlasBuildCalls = 0;
            }

          private:
            std::vector<RT::GeometryKey> m_Resident;
        };

        // --- GPU Scene fixtures ------------------------------------------

        constexpr u64 kOwnerToken = 0x9781234u;

        [[nodiscard]] GPUSceneGeometryKey MakeGeometryKey(u64 vertexBuffer, u64 indexBuffer, u32 submesh = 0)
        {
            return GPUSceneGeometryKey{ .m_VertexBuffer = vertexBuffer, .m_IndexBuffer = indexBuffer, .m_SubmeshIndex = submesh };
        }

        // A geometry the classifier will accept: real (non-zero) addresses,
        // the one vertex/index format the builder understands, and a whole
        // number of triangles.
        [[nodiscard]] GPUSceneGeometryInput MakeTraceableGeometry(u64 vertexAddress = 0x1000, u64 indexAddress = 0x2000,
                                                                  u32 indexCount = 3, u32 vertexCount = 3)
        {
            GPUSceneGeometryInput input{};
            input.m_VertexAddress = vertexAddress;
            input.m_IndexAddress = indexAddress;
            input.m_VertexFormat = static_cast<u32>(GPUSceneVertexFormat::OloVertex);
            input.m_IndexFormat = static_cast<u32>(GPUSceneIndexFormat::UInt32);
            input.m_IndexCount = indexCount;
            input.m_VertexCount = vertexCount;
            return input;
        }

        [[nodiscard]] GPUSceneMaterialKey MakeMaterialKey(u64 owner)
        {
            return GPUSceneMaterialKey{ .m_Owner = owner, .m_Slot = 0, .m_Source = 0 };
        }

        [[nodiscard]] GPUSceneMaterialInput MakeMaterial(AlphaMode mode = AlphaMode::Opaque, f32 cutoff = 0.5f)
        {
            GPUSceneMaterialInput input{};
            input.m_AlphaMode = static_cast<u32>(mode);
            input.m_AlphaCutoff = cutoff;
            return input;
        }

        [[nodiscard]] GPUSceneInstanceKey MakeInstanceKey(u64 entity, const GPUSceneGeometryKey& geometry)
        {
            return GPUSceneInstanceKey{ .m_EntityId = entity, .m_Geometry = geometry, .m_InstanceId = 0 };
        }
    } // namespace

    // =========================================================================
    // Packing arithmetic — the two conversions that TRUNCATE rather than fail
    // =========================================================================

    TEST(RayTracingPacking, InstanceMaskFoldsEveryByteRatherThanTruncating)
    {
        // GPU Scene's VisibilityMask is a u32; VkAccelerationStructureInstanceKHR's
        // mask is 8 bits. A straight cast would make an effect that only ever
        // sets a high bit invisible to every ray, silently.
        EXPECT_EQ(RT::PackInstanceMask(0xFFFFFFFFu), RT::kInstanceMaskAll);
        EXPECT_EQ(RT::PackInstanceMask(0x000000FFu), 0xFFu);
        // A bit that lives ONLY above the low byte must survive.
        EXPECT_EQ(RT::PackInstanceMask(0x01000000u), 0x01u);
        EXPECT_EQ(RT::PackInstanceMask(0x00020000u), 0x02u);
        EXPECT_EQ(RT::PackInstanceMask(0x00000400u), 0x04u);
        // Folding is an OR, so two bits in different bytes both land.
        EXPECT_EQ(RT::PackInstanceMask(0x01000002u), 0x03u);
        // A deliberate zero stays zero — that instance is hittable by nothing,
        // which is a legitimate thing to ask for.
        EXPECT_EQ(RT::PackInstanceMask(0u), 0u);
    }

    TEST(RayTracingPacking, CustomIndexBoundaryIsTheHardwares24Bits)
    {
        EXPECT_TRUE(RT::FitsInstanceCustomIndex(0u));
        EXPECT_TRUE(RT::FitsInstanceCustomIndex(RT::kMaxInstanceCustomIndex));
        EXPECT_FALSE(RT::FitsInstanceCustomIndex(RT::kMaxInstanceCustomIndex + 1u));
        EXPECT_EQ(RT::kMaxInstanceCustomIndex, 16777215u);
    }

    // =========================================================================
    // Class invariants
    // =========================================================================

    TEST(RayTracingGeometryClass, CompactionAndRefitAreMutuallyExclusive)
    {
        // A compacted acceleration structure cannot be refitted, so no class
        // may claim both. This is the invariant that keeps the builder from
        // ever asking for ALLOW_COMPACTION and ALLOW_UPDATE together.
        for (u32 i = 0; i < static_cast<u32>(RT::GeometryClass::Count); ++i)
        {
            const auto geometryClass = static_cast<RT::GeometryClass>(i);
            const bool refits = RT::UpdatePolicyFor(geometryClass) == RT::UpdatePolicy::RefitOrRebuild;
            EXPECT_FALSE(refits && RT::AllowsCompaction(geometryClass))
                << "class " << RT::ToString(geometryClass) << " claims both refit and compaction";
        }
    }

    TEST(RayTracingGeometryClass, OnlyMaskedNeedsCandidateConfirmation)
    {
        EXPECT_TRUE(RT::RequiresCandidateConfirmation(RT::GeometryClass::Masked));
        EXPECT_FALSE(RT::RequiresCandidateConfirmation(RT::GeometryClass::Static));
        EXPECT_FALSE(RT::RequiresCandidateConfirmation(RT::GeometryClass::RigidDynamic));
        EXPECT_FALSE(RT::RequiresCandidateConfirmation(RT::GeometryClass::Deformed));
        EXPECT_FALSE(RT::RequiresCandidateConfirmation(RT::GeometryClass::Unsupported));
    }

    TEST(RayTracingGeometryClass, UnsupportedGetsNoAccelerationStructure)
    {
        EXPECT_EQ(RT::UpdatePolicyFor(RT::GeometryClass::Unsupported), RT::UpdatePolicy::Never);
    }

    // =========================================================================
    // The BLAS build decision
    // =========================================================================

    using RT::BuildReason;
    using RT::GeometryClass;
    using RT::RayTracingScene;

    TEST(RayTracingBuildDecision, FirstBuildWhenNoStructureExists)
    {
        const auto reason = RayTracingScene::DecideBuild(GeometryClass::Static, GeometryClass::Static, false, false, 0);
        ASSERT_TRUE(reason.has_value());
        EXPECT_EQ(*reason, BuildReason::FirstBuild);
    }

    TEST(RayTracingBuildDecision, UnchangedStaticGeometryIsNotRebuilt)
    {
        // The acceptance criterion "rigid transform changes update TLAS state
        // without rebuilding static BLASes", from the BLAS side: a moved rigid
        // instance changes its INSTANCE record, never its GEOMETRY record, so
        // it arrives here with geometryChanged false and must produce nothing.
        EXPECT_FALSE(
            RayTracingScene::DecideBuild(GeometryClass::Static, GeometryClass::Static, false, true, 0).has_value());
    }

    TEST(RayTracingBuildDecision, ChangedGeometryRebuilds)
    {
        const auto reason = RayTracingScene::DecideBuild(GeometryClass::Static, GeometryClass::Static, true, true, 0);
        ASSERT_TRUE(reason.has_value());
        EXPECT_EQ(*reason, BuildReason::GeometryChanged);
    }

    TEST(RayTracingBuildDecision, ClassChangeRebuildsEvenWithIdenticalGeometry)
    {
        // A structure built build-once cannot become refittable, and vice
        // versa — the flags are fixed at build time.
        const auto reason = RayTracingScene::DecideBuild(GeometryClass::Static, GeometryClass::Deformed, false, true, 0);
        ASSERT_TRUE(reason.has_value());
        EXPECT_EQ(*reason, BuildReason::ClassChanged);
    }

    TEST(RayTracingBuildDecision, DeformedRefitsUntilTheRunBudgetThenRebuilds)
    {
        // The documented heuristic. A refit reuses the tree built for the
        // ORIGINAL vertex positions, so quality decays as the vertices drift;
        // the run is broken with a full rebuild at kMaxConsecutiveRefits.
        for (u32 run = 0; run < RayTracingScene::kMaxConsecutiveRefits; ++run)
        {
            const auto reason =
                RayTracingScene::DecideBuild(GeometryClass::Deformed, GeometryClass::Deformed, false, true, run);
            ASSERT_TRUE(reason.has_value()) << "run " << run;
            EXPECT_EQ(*reason, BuildReason::DeformedRefit) << "run " << run;
        }
        const auto atBudget = RayTracingScene::DecideBuild(GeometryClass::Deformed, GeometryClass::Deformed, false, true,
                                                           RayTracingScene::kMaxConsecutiveRefits);
        ASSERT_TRUE(atBudget.has_value());
        EXPECT_EQ(*atBudget, BuildReason::DeformedRefitBudget);
    }

    TEST(RayTracingBuildDecision, UnsupportedNeverBuilds)
    {
        EXPECT_FALSE(RayTracingScene::DecideBuild(GeometryClass::Unsupported, GeometryClass::Unsupported, true, false, 0)
                         .has_value());
    }

    // =========================================================================
    // The TLAS build decision
    // =========================================================================

    using RT::TlasBuildReason;

    TEST(RayTracingTlasDecision, OrdinaryTransformChurnRefits)
    {
        // "Rebuild only when topology/capacity requires it; update/refit for
        // ordinary transform changes."
        EXPECT_EQ(RayTracingScene::DecideTlasBuild(10, 10, false, false, true), TlasBuildReason::Update);
    }

    TEST(RayTracingTlasDecision, GrowthTopologyAndRebaseAllForceARebuild)
    {
        EXPECT_EQ(RayTracingScene::DecideTlasBuild(10, 11, false, false, true), TlasBuildReason::InstanceCountGrew);
        EXPECT_EQ(RayTracingScene::DecideTlasBuild(10, 10, true, false, true), TlasBuildReason::TopologyChanged);
        EXPECT_EQ(RayTracingScene::DecideTlasBuild(10, 10, false, true, true), TlasBuildReason::RenderOriginRebased);
    }

    TEST(RayTracingTlasDecision, FirstBuildBeatsEveryOtherReason)
    {
        EXPECT_EQ(RayTracingScene::DecideTlasBuild(0, 0, false, false, false), TlasBuildReason::FirstBuild);
    }

    TEST(RayTracingTlasDecision, ASmallShrinkRefitsAndALargeOneRebuilds)
    {
        // Refitting into a tree sized for many more instances leaves dead
        // weight, but rebuilding on every departure is worse. The line is a
        // halving.
        EXPECT_EQ(RayTracingScene::DecideTlasBuild(10, 8, false, false, true), TlasBuildReason::Update);
        EXPECT_EQ(RayTracingScene::DecideTlasBuild(10, 4, false, false, true), TlasBuildReason::InstanceCountShrank);
    }

    // =========================================================================
    // Classification, against real GPU Scene records
    // =========================================================================

    class RayTracingSceneFixture : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            auto backend = std::make_unique<FakeRayTracingBackend>();
            m_Backend = backend.get();
            m_Scene.SetBackendForTesting(std::move(backend));
        }

        // One extraction frame. Everything here is CPU-only — InitializeGPU is
        // deliberately never called, so no GL/Vulkan context is needed.
        void BeginFrame(const glm::vec3& renderOrigin = glm::vec3(0.0f))
        {
            m_GPUScene.BeginExtraction(kOwnerToken, renderOrigin);
        }
        void EndFrame()
        {
            static_cast<void>(m_GPUScene.EndExtraction());
        }

        // Stage one traceable instance with its geometry and material.
        void StageInstance(u64 entity, const GPUSceneGeometryKey& geometryKey, const GPUSceneGeometryInput& geometry,
                           const GPUSceneMaterialInput& material, const glm::mat4& transform = glm::mat4(1.0f),
                           u32 visibilityMask = std::numeric_limits<u32>::max())
        {
            const GPUSceneMaterialKey materialKey = MakeMaterialKey(entity);
            m_GPUScene.ExtractGeometry(geometryKey, geometry);
            m_GPUScene.ExtractMaterial(materialKey, material);
            GPUSceneInstanceInput instance{};
            instance.m_WorldTransform = transform;
            instance.m_Material = materialKey;
            instance.m_VisibilityMask = visibilityMask;
            m_GPUScene.ExtractInstance(MakeInstanceKey(entity, geometryKey), instance);
        }

        GPUScene m_GPUScene;
        RT::RayTracingScene m_Scene;
        FakeRayTracingBackend* m_Backend = nullptr;
    };

    TEST_F(RayTracingSceneFixture, AnOpaqueStaticMeshBecomesOneBlasAndOneInstance)
    {
        BeginFrame();
        StageInstance(1, MakeGeometryKey(10, 20), MakeTraceableGeometry(), MakeMaterial());
        EndFrame();

        m_Scene.Update(m_GPUScene);

        ASSERT_EQ(m_Backend->Builds.size(), 1u);
        EXPECT_EQ(m_Backend->Builds[0].Reason, BuildReason::FirstBuild);
        EXPECT_EQ(m_Backend->Builds[0].IndexCount, 3u);
        ASSERT_EQ(m_Backend->LastInstances.size(), 1u);
        EXPECT_TRUE(m_Backend->LastInstances[0].ForceOpaque);
        EXPECT_EQ(m_Backend->LastInstances[0].Mask, RT::kInstanceMaskAll);
        EXPECT_EQ(m_Scene.GetStats().Frame.InstancesTraced, 1u);
        EXPECT_EQ(m_Scene.GetStats().Resident.BlasByClass[static_cast<sizet>(GeometryClass::Static)], 1u);
    }

    TEST_F(RayTracingSceneFixture, AMaskedMaterialMakesItsInstanceNonOpaque)
    {
        // The cutout-foliage criterion, from the acceleration-structure side:
        // the ray must stop on this instance as a CANDIDATE so the shared
        // alpha helper can reject it. Opacity rides the INSTANCE, not the
        // shared BLAS.
        BeginFrame();
        StageInstance(1, MakeGeometryKey(10, 20), MakeTraceableGeometry(), MakeMaterial(AlphaMode::Mask, 0.25f));
        EndFrame();

        m_Scene.Update(m_GPUScene);

        ASSERT_EQ(m_Backend->LastInstances.size(), 1u);
        EXPECT_FALSE(m_Backend->LastInstances[0].ForceOpaque);
        EXPECT_EQ(m_Scene.GetStats().Resident.BlasByClass[static_cast<sizet>(GeometryClass::Masked)], 1u);
        EXPECT_EQ(m_Scene.GetStats().Resident.TotalBlas(), 1u);
    }

    TEST_F(RayTracingSceneFixture, TwoInstancesOfOneMeshShareOneBlas)
    {
        // A build list with two entries writing the same destination
        // acceleration structure is invalid usage, and the natural shape of a
        // per-instance loop produces exactly that.
        const GPUSceneGeometryKey shared = MakeGeometryKey(10, 20);
        BeginFrame();
        StageInstance(1, shared, MakeTraceableGeometry(), MakeMaterial());
        StageInstance(2, shared, MakeTraceableGeometry(), MakeMaterial());
        EndFrame();

        m_Scene.Update(m_GPUScene);

        EXPECT_EQ(m_Backend->Builds.size(), 1u) << "one geometry must produce one BLAS build, not one per instance";
        EXPECT_EQ(m_Backend->LastInstances.size(), 2u);
        // ...and the COUNTER must agree with the structure count. Incrementing
        // it in the instance walk instead reports a mesh drawn N times as N
        // acceleration structures, which is the number a memory budget would
        // then be sized against.
        EXPECT_EQ(m_Scene.GetStats().Resident.TotalBlas(), 1u)
            << "BlasByClass counts acceleration structures, not instances";
        EXPECT_EQ(m_Scene.GetStats().Resident.TlasInstances, 2u);
    }

    TEST_F(RayTracingSceneFixture, OneMeshUsedOpaqueAndMaskedStillSharesOneBlas)
    {
        // Opacity is an instance flag precisely so this needs one structure.
        const GPUSceneGeometryKey shared = MakeGeometryKey(10, 20);
        BeginFrame();
        StageInstance(1, shared, MakeTraceableGeometry(), MakeMaterial(AlphaMode::Opaque));
        StageInstance(2, shared, MakeTraceableGeometry(), MakeMaterial(AlphaMode::Mask));
        EndFrame();

        m_Scene.Update(m_GPUScene);

        EXPECT_EQ(m_Backend->Builds.size(), 1u);
        ASSERT_EQ(m_Backend->LastInstances.size(), 2u);
        const bool anyOpaque = std::ranges::any_of(m_Backend->LastInstances,
                                                   [](const RT::InstanceRecord& r)
                                                   { return r.ForceOpaque; });
        const bool anyNonOpaque = std::ranges::any_of(m_Backend->LastInstances,
                                                      [](const RT::InstanceRecord& r)
                                                      { return !r.ForceOpaque; });
        EXPECT_TRUE(anyOpaque);
        EXPECT_TRUE(anyNonOpaque);
    }

    TEST_F(RayTracingSceneFixture, UntraceableGeometryIsCountedNotDropped)
    {
        // "Unsupported: counted and reported, with the raster fallback
        // remaining visible." A zero-address geometry is the shape a record
        // takes on a backend with no device addresses at all.
        BeginFrame();
        GPUSceneGeometryInput noAddresses = MakeTraceableGeometry();
        noAddresses.m_VertexAddress = 0;
        noAddresses.m_IndexAddress = 0;
        StageInstance(1, MakeGeometryKey(10, 20), noAddresses, MakeMaterial());
        EndFrame();

        m_Scene.Update(m_GPUScene);

        EXPECT_TRUE(m_Backend->Builds.empty());
        EXPECT_TRUE(m_Backend->LastInstances.empty());
        EXPECT_EQ(m_Scene.GetStats().Frame.InstancesSkipped, 1u);
        EXPECT_EQ(m_Scene.GetStats().Resident.UnsupportedInstances, 1u);
        EXPECT_EQ(m_Scene.GetStats().Resident.TotalBlas(), 0u);
    }

    TEST_F(RayTracingSceneFixture, AMovedInstanceRebuildsNoBlas)
    {
        // The acceptance criterion end to end: move a rigid instance and the
        // BLAS must not be rebuilt, only the TLAS updated.
        const GPUSceneGeometryKey key = MakeGeometryKey(10, 20);
        BeginFrame();
        StageInstance(1, key, MakeTraceableGeometry(), MakeMaterial());
        EndFrame();
        m_Scene.Update(m_GPUScene);
        ASSERT_EQ(m_Backend->Builds.size(), 1u);
        m_Backend->ClearRecording();

        BeginFrame();
        StageInstance(1, key, MakeTraceableGeometry(), MakeMaterial(),
                      glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f)));
        EndFrame();
        m_Scene.Update(m_GPUScene);

        EXPECT_TRUE(m_Backend->Builds.empty()) << "a transform change must not rebuild a static BLAS";
        EXPECT_EQ(m_Backend->LastInstances.size(), 1u);
        EXPECT_EQ(m_Scene.GetStats().Frame.BlasBuilds, 0u);
    }

    TEST_F(RayTracingSceneFixture, AnInstanceThatStopsBeingStagedLeavesTheTlasAndItsBlasRetires)
    {
        // THE STALE-RECORD CRITERION: "removed/reused GPU Scene instances
        // cannot be hit through stale TLAS records."
        //
        // GPU Scene has no explicit remove — a record dies by not being
        // re-staged — so this drives the real mechanism rather than calling a
        // removal API that does not exist.
        const GPUSceneGeometryKey key = MakeGeometryKey(10, 20);
        BeginFrame();
        StageInstance(1, key, MakeTraceableGeometry(), MakeMaterial());
        EndFrame();
        m_Scene.Update(m_GPUScene);
        ASSERT_EQ(m_Backend->LastInstances.size(), 1u);
        m_Backend->ClearRecording();

        // Frame two stages nothing at all.
        BeginFrame();
        EndFrame();
        m_Scene.Update(m_GPUScene);

        EXPECT_TRUE(m_Backend->LastInstances.empty()) << "a removed instance must not survive in the TLAS";
        ASSERT_EQ(m_Backend->Retires.size(), 1u);
        EXPECT_EQ(m_Scene.GetStats().Frame.BlasRetired, 1u);
        EXPECT_EQ(m_Scene.GetStats().Frame.InstancesTraced, 0u);
    }

    TEST_F(RayTracingSceneFixture, AReusedGeometrySlotDoesNotInheritTheDeadMeshsBlas)
    {
        // A GPU Scene slot is recycled two frames after its record dies, and
        // the generation is what distinguishes the new occupant. Keying a BLAS
        // on the slot alone would hand the new mesh the old one's structure —
        // a hit on geometry that is no longer there.
        BeginFrame();
        StageInstance(1, MakeGeometryKey(10, 20), MakeTraceableGeometry(), MakeMaterial());
        EndFrame();
        m_Scene.Update(m_GPUScene);
        ASSERT_EQ(m_Backend->Builds.size(), 1u);
        const RT::GeometryKey firstKey = m_Backend->Builds[0].Key;
        m_Backend->ClearRecording();

        // Two empty frames retire the record and release its slot.
        for (int i = 0; i < 3; ++i)
        {
            BeginFrame();
            EndFrame();
            m_Scene.Update(m_GPUScene);
        }
        m_Backend->ClearRecording();

        // A different mesh now takes the recycled slot.
        BeginFrame();
        StageInstance(2, MakeGeometryKey(30, 40), MakeTraceableGeometry(0x5000, 0x6000, 6, 4), MakeMaterial());
        EndFrame();
        m_Scene.Update(m_GPUScene);

        ASSERT_EQ(m_Backend->Builds.size(), 1u) << "the recycled slot must be built afresh, not reused";
        const RT::GeometryKey secondKey = m_Backend->Builds[0].Key;
        if (secondKey.Slot == firstKey.Slot)
        {
            EXPECT_NE(secondKey.Generation, firstKey.Generation)
                << "a reused slot must not carry the dead record's generation";
        }
        EXPECT_EQ(m_Backend->Builds[0].IndexCount, 6u);
    }

    TEST_F(RayTracingSceneFixture, AFailedBuildKeepsItsInstanceOutOfTheTlas)
    {
        // An instance whose acceleration structure never landed must not reach
        // the TLAS: its accelerationStructureReference would be zero, which is
        // a GPU fault rather than a miss.
        m_Backend->FailAllBuilds = true;
        BeginFrame();
        StageInstance(1, MakeGeometryKey(10, 20), MakeTraceableGeometry(), MakeMaterial());
        EndFrame();

        m_Scene.Update(m_GPUScene);

        EXPECT_TRUE(m_Backend->LastInstances.empty());
        EXPECT_EQ(m_Scene.GetStats().Frame.InstancesTraced, 0u);
    }

    TEST_F(RayTracingSceneFixture, ARenderOriginRebaseForcesAWholeTlasRebuild)
    {
        // Crossing a camera-relative grid cell re-encodes every transform in
        // one frame, so a refit would be re-fitting a tree whose every leaf
        // moved.
        const GPUSceneGeometryKey key = MakeGeometryKey(10, 20);
        BeginFrame(glm::vec3(0.0f));
        StageInstance(1, key, MakeTraceableGeometry(), MakeMaterial());
        EndFrame();
        m_Scene.Update(m_GPUScene);
        m_Backend->ClearRecording();

        // Same content, new origin.
        BeginFrame(glm::vec3(1024.0f, 0.0f, 0.0f));
        StageInstance(1, key, MakeTraceableGeometry(), MakeMaterial());
        EndFrame();
        m_Scene.Update(m_GPUScene);

        EXPECT_EQ(m_Backend->LastRequestedReason, TlasBuildReason::RenderOriginRebased);
    }

    TEST_F(RayTracingSceneFixture, TheTransformReachesTheInstanceAsGPUScenesOwnThreeRows)
    {
        // GPUSceneTransform IS VkTransformMatrixKHR's layout. Round-tripping
        // it through a glm::mat4 is how a transpose gets in, so the records
        // are compared row for row against what GPU Scene encoded.
        const GPUSceneGeometryKey key = MakeGeometryKey(10, 20);
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 4.0f, 5.0f));
        BeginFrame();
        StageInstance(1, key, MakeTraceableGeometry(), MakeMaterial(), transform);
        EndFrame();
        m_Scene.Update(m_GPUScene);

        ASSERT_EQ(m_Backend->LastInstances.size(), 1u);
        const GPUSceneInstance* record = m_GPUScene.GetLiveInstanceRecordBySlot(0);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(Math::BitwiseEqual(m_Backend->LastInstances[0].Transform[0], record->CurrentTransform.Row0));
        EXPECT_TRUE(Math::BitwiseEqual(m_Backend->LastInstances[0].Transform[1], record->CurrentTransform.Row1));
        EXPECT_TRUE(Math::BitwiseEqual(m_Backend->LastInstances[0].Transform[2], record->CurrentTransform.Row2));
        // The translation lives in the fourth column of the three rows.
        EXPECT_NEAR(m_Backend->LastInstances[0].Transform[0].w, 3.0f, 1e-4f);
        EXPECT_NEAR(m_Backend->LastInstances[0].Transform[1].w, 4.0f, 1e-4f);
        EXPECT_NEAR(m_Backend->LastInstances[0].Transform[2].w, 5.0f, 1e-4f);
    }

    // =========================================================================
    // The unsupported-hardware arm
    // =========================================================================

    TEST(RayTracingUnsupported, AnUnsupportedSceneBuildsNothingAndSaysWhy)
    {
        // "Unsupported RT hardware keeps the raster renderer usable and
        // reports the capability reason." Asserted from BOTH directions off
        // one branch, because every machine that can run the device tests has
        // ray tracing and would never exercise this arm otherwise.
        RT::RayTracingScene scene;
        auto backend = std::make_unique<FakeRayTracingBackend>();
        backend->Caps.Supported = false;
        backend->Caps.Reason = RT::UnsupportedReason::ExtensionMissing;
        auto* raw = backend.get();
        scene.SetBackendForTesting(std::move(backend));

        EXPECT_FALSE(scene.IsAvailable());
        EXPECT_FALSE(scene.GetCapabilities().Supported);
        EXPECT_EQ(scene.GetCapabilities().Reason, RT::UnsupportedReason::ExtensionMissing);
        EXPECT_FALSE(scene.GetCapabilities().ReasonText().empty());
        EXPECT_EQ(scene.GetTlasDeviceAddress(), 0u);

        GPUScene gpuScene;
        gpuScene.BeginExtraction(kOwnerToken, glm::vec3(0.0f));
        static_cast<void>(gpuScene.EndExtraction());
        scene.Update(gpuScene);
        scene.RecordBuildToReadBarrier();

        EXPECT_TRUE(raw->Builds.empty());
        EXPECT_EQ(raw->TlasBuildCalls, 0u);
        EXPECT_EQ(raw->BarrierCalls, 0u) << "an unavailable scene must not touch the command buffer at all";
    }

    TEST(RayTracingUnsupported, EveryReasonHasDistinctHumanReadableText)
    {
        // A reason enum whose ToString falls through to "unknown" would report
        // a capability failure the user cannot act on.
        const RT::UnsupportedReason reasons[] = {
            RT::UnsupportedReason::None,
            RT::UnsupportedReason::BackendNotVulkan,
            RT::UnsupportedReason::NoDevice,
            RT::UnsupportedReason::ExtensionMissing,
            RT::UnsupportedReason::FeatureUnsupported,
            RT::UnsupportedReason::EntryPointMissing,
            RT::UnsupportedReason::DisabledByLever,
        };
        // The sweep is hand-written, so it can silently stop being exhaustive.
        // Pinned with the repo's growth-tripwire idiom (RHIEnumLoweringTest)
        // rather than by adding a Count enumerator: UnsupportedReason is not an
        // array index, and a Count member would need a case in ToString's
        // deliberately default-less switch — weakening the exhaustiveness this
        // very test exists to protect.
        static_assert(static_cast<u32>(RT::UnsupportedReason::DisabledByLever) == 6u,
                      "UnsupportedReason gained or lost a member — add it to reasons[] below");
        static_assert(std::size(reasons) == static_cast<sizet>(RT::UnsupportedReason::DisabledByLever) + 1u,
                      "reasons[] no longer covers every UnsupportedReason");
        std::vector<std::string_view> seen;
        for (const RT::UnsupportedReason reason : reasons)
        {
            const std::string_view text = RT::ToString(reason);
            EXPECT_NE(text, "unknown") << "reason " << static_cast<u32>(reason) << " has no text";
            EXPECT_FALSE(text.empty());
            EXPECT_EQ(std::ranges::find(seen, text), seen.end()) << "duplicate reason text: " << text;
            seen.push_back(text);
        }
    }
} // namespace OloEngine::Tests
