// OLO_TEST_LAYER: L1
// =============================================================================
// GPUSceneAnimatedRecordPropertyTests.cpp
//
// Issue #1228: animated meshes in GPU Scene with stable surface identity.
//
// What these tests are for, and what they deliberately are NOT for.
//
// The identity half of this feature is almost entirely INHERITED. A skinned
// mesh's vertex buffer holds a rest surface that the vertex stage deforms, and
// the CPU morph pass writes that same buffer in place, so the geometry key
// (vertex buffer, index buffer, submesh) was already a stable per-submesh
// identity for an animated surface before this issue existed -- what was
// missing was a path that USED it. Re-asserting the key rules here would be
// testing GPUScenePropertyTests.cpp a second time.
//
// So what is pinned here is exactly the part that is new, and the part whose
// failure mode is silent:
//
//   * The deformation revision pair means one thing, in one place. A record
//     claiming continuity that the bone palettes do not have is not a crash and
//     not a test failure anywhere else -- it is a velocity measured across a
//     seam, which TAA and motion blur smear faithfully, several subsystems from
//     the cause.
//   * Invalidation is NARROW. A too-wide invalidation is the defect nothing
//     catches: every picture is still correct, every test still passes, and the
//     cost is temporal quality and throughput. So the assertions are about
//     which records did NOT move, which is the half a happy-path test omits.
//   * A rigid instance never claims a deformation history it does not have.
//     0 == 0 reads as "dropped", so the Animated FLAG, not the values, has to
//     be what a consumer keys on.
//
// Pure CPU: the registry with no GL context.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "GPUSceneRecordTestHelpers.h"

#include "OloEngine/Animation/SkeletalDeformation.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneDrawLink.h"

#include <glm/gtc/matrix_transform.hpp>

#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using GPUSceneRecordTesting::kGeometry;
        using GPUSceneRecordTesting::kGeometryKey;

        // A second geometry, standing in for what an LOD switch or a topology
        // replacement produces: a DIFFERENT vertex buffer, therefore a
        // different geometry key, therefore a different instance key.
        const GPUSceneGeometryKey kOtherGeometryKey{ .m_VertexBuffer = 21, .m_IndexBuffer = 22 };
        const GPUSceneGeometryInput kOtherGeometry{ .m_VertexBuffer = RHI::ResourceHandle{ 3, 1 },
                                                    .m_IndexBuffer = RHI::ResourceHandle{ 4, 1 },
                                                    .m_IndexCount = 3,
                                                    .m_VertexCount = 3 };

        [[nodiscard]] GPUSceneInstanceKey AnimatedKey(u64 entityId, const GPUSceneGeometryKey& geometry)
        {
            return GPUSceneInstanceKey{ .m_EntityId = entityId, .m_Geometry = geometry };
        }

        [[nodiscard]] GPUSceneInstanceInput AnimatedInput(const glm::mat4& transform, u32 revision,
                                                          u32 prevRevision)
        {
            return GPUSceneInstanceInput{
                .m_WorldTransform = transform,
                .m_Flags = GPUSceneInstanceFlagAnimated,
                .m_DeformationRevision = revision,
                .m_PrevDeformationRevision = prevRevision,
            };
        }

        // One frame of extraction for a fixed set of (key, input) pairs.
        void ExtractFrame(GPUScene& scene, u64 ownerToken,
                          const std::vector<std::pair<GPUSceneInstanceKey, GPUSceneInstanceInput>>& instances,
                          bool includeOtherGeometry = false)
        {
            scene.BeginExtraction(ownerToken, glm::vec3(0.0f));
            scene.ExtractGeometry(kGeometryKey, kGeometry);
            if (includeOtherGeometry)
            {
                scene.ExtractGeometry(kOtherGeometryKey, kOtherGeometry);
            }
            for (const auto& [key, input] : instances)
            {
                scene.ExtractInstance(key, input);
            }
            scene.EndExtraction();
        }

        [[nodiscard]] const GPUSceneInstance* Record(const GPUScene& scene, const GPUSceneInstanceKey& key)
        {
            return scene.GetInstanceRecord(scene.FindInstance(key));
        }
    } // namespace

    // ---------------------------------------------------------------- AC 1 ---

    // Identity is per (entity, geometry, SUBMESH), and the deformation revision
    // is per ENTITY. Both halves matter: a character with three submeshes is
    // three records sharing one pose, and collapsing either direction is a real
    // defect. Keying the pose per submesh would let two submeshes of one
    // character disagree about whether their shared palette is continuous;
    // keying identity per entity would give every submesh after the first the
    // previous one's transform history, which is the exact defect the
    // per-entity previous-transform cache had before #994.
    TEST(GPUSceneAnimated, OneEntityKeepsOneRevisionAcrossManySubmeshIdentities)
    {
        GPUScene scene;
        const auto submesh = [](u32 index)
        { return GPUSceneGeometryKey{ .m_VertexBuffer = 11, .m_IndexBuffer = 12, .m_SubmeshIndex = index }; };

        scene.BeginExtraction(1, glm::vec3(0.0f));
        for (u32 i = 0; i < 3u; ++i)
        {
            scene.ExtractGeometry(submesh(i), kGeometry);
            scene.ExtractInstance(AnimatedKey(7, submesh(i)),
                                  AnimatedInput(glm::translate(glm::mat4(1.0f), glm::vec3(f32(i), 0.0f, 0.0f)),
                                                /*revision*/ 5u, /*prev*/ 4u));
        }
        scene.EndExtraction();

        std::vector<u32> slots;
        for (u32 i = 0; i < 3u; ++i)
        {
            const GPUSceneInstance* record = Record(scene, AnimatedKey(7, submesh(i)));
            ASSERT_NE(record, nullptr) << "submesh " << i << " of an animated entity produced no record";
            slots.push_back(record->StableIndex);

            EXPECT_NE(record->Flags & GPUSceneInstanceFlagAnimated, 0u);
            EXPECT_EQ(record->DeformationRevision, 5u) << "submesh " << i << " disagrees about the shared pose";
            EXPECT_EQ(record->PreviousDeformationRevision, 4u);
        }
        EXPECT_NE(slots[0], slots[1]);
        EXPECT_NE(slots[1], slots[2]);
        EXPECT_NE(slots[0], slots[2]) << "three submeshes of one animated entity collapsed onto fewer records, so "
                                         "they share a transform history that is only correct for one of them";
    }

    // The identity does not move when the POSE moves, which is the whole claim
    // "stable surface identity" makes. A skinned mesh deforms in the vertex
    // stage, so its buffers -- and therefore its key and its slot -- are the
    // same object from frame to frame; only the revision advances.
    TEST(GPUSceneAnimated, AdvancingThePoseKeepsTheSlotAndTheGeneration)
    {
        GPUScene scene;
        const GPUSceneInstanceKey key = AnimatedKey(7, kGeometryKey);

        ExtractFrame(scene, 1, { { key, AnimatedInput(glm::mat4(1.0f), 5u, 4u) } });
        const GPUSceneHandle first = scene.FindInstance(key);
        ASSERT_TRUE(first.IsValid());

        for (u32 revision = 6u; revision < 12u; ++revision)
        {
            ExtractFrame(scene, 1, { { key, AnimatedInput(glm::mat4(1.0f), revision, revision - 1u) } });
            const GPUSceneHandle handle = scene.FindInstance(key);
            EXPECT_EQ(handle, first) << "the pose advancing to revision " << revision
                                     << " moved the surface's identity; a consumer holding the handle across "
                                        "frames would have to rebuild for a character that merely animated";

            const GPUSceneInstance* record = Record(scene, key);
            ASSERT_NE(record, nullptr);
            EXPECT_EQ(record->DeformationRevision, revision);
            EXPECT_EQ(record->PreviousDeformationRevision, revision - 1u);
        }
    }

    // A rigid instance leaves both lanes at zero, and zero-equals-zero is the
    // DISCONTINUOUS reading. Without the flag the raster path would read every
    // static prop as an animated surface whose history had just been thrown
    // away -- the opposite verdict from the true one.
    TEST(GPUSceneAnimated, ARigidInstanceIsNotAnAnimatedOneWithoutHistory)
    {
        GPUScene scene;
        const GPUSceneInstanceKey rigidKey = AnimatedKey(1, kGeometryKey);
        const GPUSceneInstanceKey animatedKey = AnimatedKey(2, kGeometryKey);

        ExtractFrame(scene, 1,
                     { { rigidKey, GPUSceneInstanceInput{ .m_WorldTransform = glm::mat4(1.0f) } },
                       { animatedKey, AnimatedInput(glm::mat4(1.0f), /*revision*/ 3u, /*prev*/ 3u) } });

        const GPUSceneInstance* rigid = Record(scene, rigidKey);
        const GPUSceneInstance* animated = Record(scene, animatedKey);
        ASSERT_NE(rigid, nullptr);
        ASSERT_NE(animated, nullptr);

        EXPECT_EQ(rigid->DeformationRevision, 0u);
        EXPECT_EQ(rigid->PreviousDeformationRevision, 0u);
        EXPECT_EQ(rigid->Flags & GPUSceneInstanceFlagAnimated, 0u)
            << "a rigid instance carries the animated flag, so its 0/0 revisions read as a dropped deformation "
               "history rather than as 'this surface does not deform'";
        EXPECT_NE(animated->Flags & GPUSceneInstanceFlagAnimated, 0u);
    }

    // ---------------------------------------------------------------- AC 2 ---

    // The narrowness claim, stated as the thing a happy-path test omits: what
    // did NOT change. A global revision bump, a whole-registry reset on an LOD
    // switch, or an invalidation keyed on the entity rather than the surface
    // would all still draw a correct picture -- and would cost every other
    // character on screen its temporal history, invisibly.
    TEST(GPUSceneAnimated, OneSurfacesTopologyChangeLeavesEveryOtherRecordUntouched)
    {
        GPUScene scene;
        const GPUSceneInstanceKey stayerA = AnimatedKey(1, kGeometryKey);
        const GPUSceneInstanceKey stayerB = AnimatedKey(2, kGeometryKey);
        const GPUSceneInstanceKey switcherBefore = AnimatedKey(3, kGeometryKey);
        const GPUSceneInstanceKey switcherAfter = AnimatedKey(3, kOtherGeometryKey);

        ExtractFrame(scene, 1,
                     { { stayerA, AnimatedInput(glm::mat4(1.0f), 5u, 4u) },
                       { stayerB, AnimatedInput(glm::mat4(1.0f), 5u, 4u) },
                       { switcherBefore, AnimatedInput(glm::mat4(1.0f), 5u, 4u) } },
                     /*includeOtherGeometry*/ true);

        const GPUSceneHandle stayerAHandle = scene.FindInstance(stayerA);
        const GPUSceneHandle stayerBHandle = scene.FindInstance(stayerB);
        ASSERT_TRUE(stayerAHandle.IsValid());
        ASSERT_TRUE(stayerBHandle.IsValid());

        // Entity 3 switches level. Its surface really is a different surface,
        // so its previous pose is not comparable and the producer holds the
        // revision equal -- while the other two animate on undisturbed.
        ExtractFrame(scene, 1,
                     { { stayerA, AnimatedInput(glm::mat4(1.0f), 6u, 5u) },
                       { stayerB, AnimatedInput(glm::mat4(1.0f), 6u, 5u) },
                       { switcherAfter, AnimatedInput(glm::mat4(1.0f), 6u, 6u) } },
                     /*includeOtherGeometry*/ true);

        EXPECT_EQ(scene.FindInstance(stayerA), stayerAHandle)
            << "another entity's LOD switch moved this record's identity";
        EXPECT_EQ(scene.FindInstance(stayerB), stayerBHandle);

        for (const GPUSceneInstanceKey& key : { stayerA, stayerB })
        {
            const GPUSceneInstance* record = Record(scene, key);
            ASSERT_NE(record, nullptr);
            EXPECT_EQ(record->DeformationRevision, 6u);
            EXPECT_EQ(record->PreviousDeformationRevision, 5u)
                << "an unrelated entity's topology change cost this surface its deformation history";
        }

        // The switcher's OLD identity is gone, and its new one starts without a
        // history rather than inheriting the old surface's.
        EXPECT_EQ(Record(scene, switcherBefore), nullptr)
            << "the pre-switch surface still resolves, so a draw could still name geometry that is no longer "
               "what the deformation pass wrote";
        const GPUSceneInstance* switched = Record(scene, switcherAfter);
        ASSERT_NE(switched, nullptr);
        EXPECT_EQ(switched->DeformationRevision, switched->PreviousDeformationRevision)
            << "the surface after an LOD switch claims a previous pose measured on the surface before it";
    }

    // Spawn and removal, same claim from the other direction. A new character
    // appearing must not disturb the records of the ones already on screen, and
    // a character leaving must retire only its own slot.
    TEST(GPUSceneAnimated, SpawnAndRemovalTouchOnlyTheirOwnRecords)
    {
        GPUScene scene;
        const GPUSceneInstanceKey resident = AnimatedKey(1, kGeometryKey);
        const GPUSceneInstanceKey newcomer = AnimatedKey(2, kGeometryKey);

        ExtractFrame(scene, 1, { { resident, AnimatedInput(glm::mat4(1.0f), 5u, 4u) } });
        const GPUSceneHandle residentHandle = scene.FindInstance(resident);
        ASSERT_TRUE(residentHandle.IsValid());

        // Spawn.
        ExtractFrame(scene, 1,
                     { { resident, AnimatedInput(glm::mat4(1.0f), 6u, 5u) },
                       { newcomer, AnimatedInput(glm::mat4(1.0f), 1u, 1u) } });
        EXPECT_EQ(scene.FindInstance(resident), residentHandle) << "a spawn moved a resident record's identity";
        const GPUSceneInstance* residentRecord = Record(scene, resident);
        ASSERT_NE(residentRecord, nullptr);
        EXPECT_EQ(residentRecord->PreviousDeformationRevision, 5u)
            << "a spawn cost an unrelated surface its deformation history";

        const GPUSceneInstance* newcomerRecord = Record(scene, newcomer);
        ASSERT_NE(newcomerRecord, nullptr);
        EXPECT_EQ(newcomerRecord->DeformationRevision, newcomerRecord->PreviousDeformationRevision)
            << "a freshly spawned surface claims a previous pose it was never in";

        // Removal.
        ExtractFrame(scene, 1, { { resident, AnimatedInput(glm::mat4(1.0f), 7u, 6u) } });
        EXPECT_EQ(scene.FindInstance(resident), residentHandle) << "a removal moved a survivor's identity";
        EXPECT_EQ(Record(scene, newcomer), nullptr);
        residentRecord = Record(scene, resident);
        ASSERT_NE(residentRecord, nullptr);
        EXPECT_EQ(residentRecord->PreviousDeformationRevision, 6u)
            << "a removal cost a surviving surface its deformation history";
    }

    // ---------------------------------------------------------------- AC 4 ---

    // The record, the resolved draw link and the census must all read the same
    // verdict off one set of numbers. Three spellings of "does this surface have
    // a usable previous pose" is three chances to drift, and the drift is
    // invisible: each is individually plausible.
    TEST(GPUSceneAnimated, TheLinkAndTheRecordAgreeOnContinuity)
    {
        GPUScene scene;
        const GPUSceneInstanceKey continuous = AnimatedKey(1, kGeometryKey);
        const GPUSceneInstanceKey dropped = AnimatedKey(2, kGeometryKey);
        const GPUSceneInstanceKey rigid = AnimatedKey(3, kGeometryKey);

        ExtractFrame(scene, 1,
                     { { continuous, AnimatedInput(glm::mat4(1.0f), 9u, 8u) },
                       { dropped, AnimatedInput(glm::mat4(1.0f), 9u, 9u) },
                       { rigid, GPUSceneInstanceInput{ .m_WorldTransform = glm::mat4(1.0f) } } });

        const auto resolve = [&scene](const GPUSceneInstanceKey& key)
        {
            GPUSceneDrawLink link{ .m_InstanceKey = key };
            const GPUSceneHandle handle = scene.FindInstance(key);
            const GPUSceneInstance* record = scene.GetInstanceRecord(handle);
            EXPECT_NE(record, nullptr);
            if (record != nullptr)
            {
                link.m_Resolved = true;
                link.m_Instance = handle;
                link.m_Animated = (record->Flags & GPUSceneInstanceFlagAnimated) != 0u;
                link.m_DeformationRevision = record->DeformationRevision;
                link.m_PreviousDeformationRevision = record->PreviousDeformationRevision;
            }
            return link;
        };

        EXPECT_TRUE(resolve(continuous).HasContinuousDeformation());
        EXPECT_FALSE(resolve(dropped).HasContinuousDeformation())
            << "a surface whose history was dropped reads as continuous through the link, so the raster path "
               "would emit a velocity across the discontinuity the producer declared";
        EXPECT_FALSE(resolve(rigid).HasContinuousDeformation())
            << "a rigid draw reads as an animated surface with a usable previous pose";
    }

    // An unresolved link must be indistinguishable from no link at all, in the
    // deformation half as well as the transform half. A link that resolved
    // nothing but still claimed continuity would hand the consumer a velocity
    // derived from a record that does not exist.
    TEST(GPUSceneAnimated, AnUnresolvedLinkClaimsNoDeformationHistory)
    {
        GPUSceneDrawLink unresolved{ .m_InstanceKey = AnimatedKey(1, kGeometryKey) };
        unresolved.m_Animated = true;
        unresolved.m_DeformationRevision = 9u;
        unresolved.m_PreviousDeformationRevision = 8u;

        EXPECT_FALSE(unresolved.HasContinuousDeformation());
        EXPECT_EQ(unresolved.Ref(), glm::uvec4(0u));
    }

    // ------------------------------------------- the producer/record contract -

    // The record's verdict is the palettes' verdict. SkeletonData owns when a
    // deformation history exists -- HasBoneHistory() is what every shader path
    // is gated on -- and the revision pair is a second spelling of that fact
    // travelling to consumers that cannot see the skeleton. Two spellings are
    // only safe while something checks they agree, over the transitions, not
    // just in the steady state.
    TEST(GPUSceneAnimated, TheRevisionPairNeverDisagreesWithTheBonePalettes)
    {
        SkeletonData skeleton(4);

        const auto check = [&skeleton](const char* what)
        {
            EXPECT_EQ(skeleton.HasBoneHistory(), skeleton.HasContinuousDeformation())
                << what << ": the revision pair and the bone palettes disagree about whether a previous pose "
                           "exists, so a record and the shaders reading it would emit different motion";
        };

        check("construction");

        // Construction arms a pending reset, so the first advance covers it.
        skeleton.AdvanceBoneHistory();
        check("first advance");

        skeleton.AdvanceBoneHistory();
        check("second advance");
        EXPECT_TRUE(skeleton.HasContinuousDeformation());

        // A discontinuity declared AFTER this frame's advance -- the morph and
        // LOD cases -- must still read as discontinuous.
        skeleton.AdvanceBoneHistory();
        ASSERT_TRUE(skeleton.HasContinuousDeformation());
        skeleton.ResetBoneHistory();
        check("reset after the advance");
        EXPECT_FALSE(skeleton.HasContinuousDeformation())
            << "a reset that lands after the frame's advance did not cancel the continuity that advance "
               "declared, which is exactly the case a slot-derived previous revision would miss";

        skeleton.AdvanceBoneHistory();
        check("advance covering the reset");

        skeleton.AdvanceBoneHistory();
        check("recovery");
        EXPECT_TRUE(skeleton.HasContinuousDeformation());

        // A bone-count change resizes the palette: not comparable.
        skeleton.m_FinalBoneMatrices.resize(6, glm::mat4(1.0f));
        skeleton.AdvanceBoneHistory();
        check("bone count change");
        EXPECT_FALSE(skeleton.HasContinuousDeformation());
    }

    // The revision is monotonic across a pause, which is what separates "no
    // motion because nothing moved" from "no motion because the history was
    // thrown away". A paused character advances into prev == current POSES and
    // therefore emits zero motion -- but its history is intact, and a consumer
    // that treated the pause as a discontinuity would refuse it a velocity
    // forever rather than give it a zero one.
    TEST(GPUSceneAnimated, APausedSurfaceKeepsAContinuousRevision)
    {
        SkeletonData skeleton(4);
        skeleton.AdvanceBoneHistory();
        skeleton.AdvanceBoneHistory();
        ASSERT_TRUE(skeleton.HasContinuousDeformation());

        const u32 before = skeleton.m_DeformationRevision;
        for (u32 i = 0; i < 5u; ++i)
        {
            // No pose is written between advances: this is a paused character.
            skeleton.AdvanceBoneHistory();
            EXPECT_TRUE(skeleton.HasContinuousDeformation())
                << "pause frame " << i << " read as a discontinuity, so a standing character is permanently "
                                          "denied a motion vector instead of being given a zero one";
        }
        EXPECT_EQ(skeleton.m_DeformationRevision, before + 5u)
            << "the revision stopped advancing while paused, so a consumer asking 'has this surface deformed "
               "since I last saw it' cannot tell a pause from a stall";
    }
} // namespace OloEngine::Tests
