// OLO_TEST_LAYER: L1
// =============================================================================
// GPUSceneRasterMigrationTest.cpp
//
// The CPU contract behind the raster migration (issue #994): what a draw link
// promises the dispatcher, and what happens to it across a scene reload or a
// backend switch.
//
// The pixel side of this change is verified separately, by multi-angle evidence
// and a live editor run — a wrong transform lane still renders *something*
// plausible, which is exactly why the math is pinned here where a mistake is
// loud rather than merely visible.
//
// Classification: L1 (pure CPU registry + decode math, no GL context).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneDrawLink.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u64 kOwner = 0x9940u;

        [[nodiscard]] GPUSceneGeometryKey GeometryKey(u32 submeshIndex)
        {
            return GPUSceneGeometryKey{ .m_VertexBuffer = 101, .m_IndexBuffer = 202, .m_SubmeshIndex = submeshIndex };
        }

        [[nodiscard]] GPUSceneGeometryInput GeometryInput()
        {
            return GPUSceneGeometryInput{ .m_VertexBuffer = RHI::ResourceHandle{ 1, 1 },
                                          .m_IndexBuffer = RHI::ResourceHandle{ 2, 1 },
                                          .m_IndexCount = 36,
                                          .m_VertexCount = 24 };
        }

        [[nodiscard]] GPUSceneInstanceKey InstanceKey(u64 entityId, u32 submeshIndex)
        {
            return GPUSceneInstanceKey{ .m_EntityId = entityId, .m_Geometry = GeometryKey(submeshIndex) };
        }

        // What the renderer does per frame for one entity's submeshes: stage the
        // geometry, stage an instance per submesh at `transform`, commit.
        void ExtractFrame(GPUScene& scene, const glm::vec3& renderOrigin, u64 entityId, u32 submeshCount,
                          const glm::mat4& transform, u64 ownerToken = kOwner)
        {
            scene.BeginExtraction(ownerToken, renderOrigin);
            for (u32 submesh = 0; submesh < submeshCount; ++submesh)
            {
                scene.ExtractGeometry(GeometryKey(submesh), GeometryInput());
                scene.ExtractInstance(InstanceKey(entityId, submesh), GPUSceneInstanceInput{ .m_WorldTransform = transform });
            }
            (void)scene.EndExtraction();
        }

        [[nodiscard]] const GPUSceneInstance* Record(const GPUScene& scene, const GPUSceneInstanceKey& key)
        {
            return scene.GetInstanceRecord(scene.FindInstance(key));
        }

        [[nodiscard]] ::testing::AssertionResult MatricesEqual(const glm::mat4& actual, const glm::mat4& expected)
        {
            for (int column = 0; column < 4; ++column)
            {
                for (int row = 0; row < 4; ++row)
                {
                    if (actual[column][row] != expected[column][row])
                    {
                        return ::testing::AssertionFailure()
                               << "matrices differ at [" << column << "][" << row << "]: " << actual[column][row]
                               << " vs " << expected[column][row];
                    }
                }
            }
            return ::testing::AssertionSuccess();
        }
    } // namespace

    // The dispatcher uploads the record's transform AS IS, without shifting it
    // again — the record was encoded with this frame's render origin. If decode
    // and MakeModelRelative ever disagree, every migrated mesh moves, so the
    // comparison is bitwise rather than approximate.
    TEST(GPUSceneRasterMigration, DecodedTransformIsExactlyTheRenderRelativeMatrix)
    {
        const glm::vec3 renderOrigin(1024.0f, -64.0f, 4096.0f);
        const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(1100.5f, 3.25f, 4000.75f)) *
                                glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f))) *
                                glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 0.5f, 1.25f));

        GPUScene scene;
        ExtractFrame(scene, renderOrigin, 7, 1, world);

        const GPUSceneInstance* record = Record(scene, InstanceKey(7, 0));
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(MatricesEqual(DecodeGPUSceneTransform(record->CurrentTransform),
                                  MakeModelRelative(world, renderOrigin)))
            << "the decoded record must be the same render-relative matrix the legacy path uploads; a mismatch "
               "either transposes the mesh or offsets it by the render origin twice";
    }

    // Motion vectors and the future RT consumer must read ONE previous-frame
    // truth. The per-entity cache this migration replaces could not provide it:
    // keyed on entity id and written on every DrawMesh call, a multi-submesh
    // mesh overwrote its own history between submeshes, so submeshes 1..N-1
    // reported zero velocity while submesh 0 reported the real motion. The
    // record is keyed per (entity, geometry, submesh) and does not.
    TEST(GPUSceneRasterMigration, EverySubmeshKeepsItsOwnPreviousTransform)
    {
        const glm::vec3 renderOrigin(0.0f);
        const glm::mat4 first = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));
        const glm::mat4 second = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f));

        GPUScene scene;
        constexpr u32 submeshCount = 3;
        ExtractFrame(scene, renderOrigin, 7, submeshCount, first);

        // First sight: no history, so the instance starts static and velocity
        // reads zero. A newly spawned mesh must not streak.
        for (u32 submesh = 0; submesh < submeshCount; ++submesh)
        {
            const GPUSceneInstance* record = Record(scene, InstanceKey(7, submesh));
            ASSERT_NE(record, nullptr) << "submesh " << submesh;
            EXPECT_TRUE(MatricesEqual(DecodeGPUSceneTransform(record->PreviousTransform), first)) << "submesh " << submesh;
        }

        ExtractFrame(scene, renderOrigin, 7, submeshCount, second);
        for (u32 submesh = 0; submesh < submeshCount; ++submesh)
        {
            const GPUSceneInstance* record = Record(scene, InstanceKey(7, submesh));
            ASSERT_NE(record, nullptr) << "submesh " << submesh;
            EXPECT_TRUE(MatricesEqual(DecodeGPUSceneTransform(record->CurrentTransform), second)) << "submesh " << submesh;
            EXPECT_TRUE(MatricesEqual(DecodeGPUSceneTransform(record->PreviousTransform), first))
                << "submesh " << submesh
                << " lost its own previous transform — this is the exact defect the per-entity cache had";
        }
    }

    // The link the shader receives is (slot, generation) twice over, and the
    // generation is what makes it safe. An unresolved link must be
    // indistinguishable from "no link", because generation zero is the invalid
    // generation on both sides of the mirror.
    TEST(GPUSceneRasterMigration, UnresolvedLinkIsIndistinguishableFromNoLink)
    {
        GPUSceneDrawLink link;
        EXPECT_FALSE(link.m_Resolved);
        EXPECT_EQ(link.Ref(), glm::uvec4(0u));
        EXPECT_EQ(link.Ref().y, GPUSceneDrawRefUnlinked);
        EXPECT_EQ(link.Ref().w, GPUSceneDrawRefUnlinked);

        // And the same value a default InstanceData carries, so a draw that was
        // never linked reads as unlinked rather than as slot 0 generation 0.
        const InstanceData instance{};
        EXPECT_EQ(instance.GPUSceneRef, link.Ref());
    }

    TEST(GPUSceneRasterMigration, ResolvedLinkCarriesBothSlotAndGeneration)
    {
        GPUScene scene;
        const GPUSceneMaterialKey materialKey{ .m_Owner = 3, .m_Slot = 0, .m_Source = std::to_underlying(GPUSceneMaterialSource::Imported) };
        scene.BeginExtraction(kOwner, glm::vec3(0.0f));
        scene.ExtractGeometry(GeometryKey(0), GeometryInput());
        scene.ExtractMaterial(materialKey, GPUSceneMaterialInput{});
        scene.ExtractInstance(InstanceKey(7, 0), GPUSceneInstanceInput{ .m_Material = materialKey });
        (void)scene.EndExtraction();

        const GPUSceneHandle instance = scene.FindInstance(InstanceKey(7, 0));
        const GPUSceneInstance* record = scene.GetInstanceRecord(instance);
        ASSERT_NE(record, nullptr);

        GPUSceneDrawLink link;
        link.m_Resolved = true;
        link.m_Instance = instance;
        link.m_Material = GPUSceneHandle{ .m_Index = record->MaterialIndex, .m_Generation = record->MaterialGeneration };

        const glm::uvec4 ref = link.Ref();
        EXPECT_EQ(ref.x, instance.m_Index);
        EXPECT_EQ(ref.y, instance.m_Generation);
        EXPECT_NE(ref.y, 0u) << "a live instance must never present the invalid generation";
        EXPECT_EQ(ref.z, scene.FindMaterial(materialKey).m_Index);
        EXPECT_EQ(ref.w, scene.FindMaterial(materialKey).m_Generation);
        EXPECT_NE(ref.w, 0u);

        // What the shader does with the link: the record it lands on must agree
        // with the generation it was handed, or it falls back.
        const GPUSceneMaterial* material = scene.GetMaterialRecord(scene.FindMaterial(materialKey));
        ASSERT_NE(material, nullptr);
        EXPECT_EQ(material->Generation, ref.w);
        EXPECT_TRUE((material->Flags & GPUSceneMaterialFlagActive) != 0u);
    }

    // Lifecycle. A scene reload tombstones every record; a link taken before it
    // must not resolve afterwards, and must not resolve to a RECYCLED slot
    // either — that is the difference between "stale binding" and "wrong mesh".
    TEST(GPUSceneRasterMigration, SceneReloadLeavesNoResolvableLink)
    {
        GPUScene scene;
        ExtractFrame(scene, glm::vec3(0.0f), 7, 1, glm::mat4(1.0f));

        const GPUSceneHandle before = scene.FindInstance(InstanceKey(7, 0));
        ASSERT_TRUE(before.IsValid());
        ASSERT_NE(scene.GetInstanceRecord(before), nullptr);

        scene.Reset();
        EXPECT_FALSE(scene.FindInstance(InstanceKey(7, 0)).IsValid())
            << "a link taken before a reload must not resolve afterwards";
        EXPECT_FALSE(scene.IsInstanceHandleLive(before));
        EXPECT_EQ(scene.GetInstanceRecord(before), nullptr);

        // Re-extraction after the reload gives the same key a DIFFERENT handle:
        // the retired slot cannot be recycled for RetirementFrameCount frames,
        // so the replacement appends elsewhere. Either half of the handle may
        // move — what matters is that the pre-reload handle is not the one the
        // key now owns, and is still dead.
        ExtractFrame(scene, glm::vec3(0.0f), 7, 1, glm::mat4(1.0f));
        const GPUSceneHandle after = scene.FindInstance(InstanceKey(7, 0));
        ASSERT_TRUE(after.IsValid());
        EXPECT_NE(after, before);
        EXPECT_FALSE(scene.IsInstanceHandleLive(before));
    }

    // A backend switch changes the owner token, which resets every kind at once
    // for the same reason a reload does: every RHI handle and heap offset in
    // the records belongs to the old device.
    TEST(GPUSceneRasterMigration, BackendSwitchInvalidatesLinksTakenOnTheOldDevice)
    {
        GPUScene scene;
        ExtractFrame(scene, glm::vec3(0.0f), 7, 2, glm::mat4(1.0f), 0xAAAAu);
        const GPUSceneHandle before = scene.FindInstance(InstanceKey(7, 0));
        ASSERT_TRUE(before.IsValid());

        ExtractFrame(scene, glm::vec3(0.0f), 7, 2, glm::mat4(1.0f), 0xBBBBu);
        EXPECT_FALSE(scene.IsInstanceHandleLive(before))
            << "a handle from the previous device must not become live again just because the same key came back";

        const GPUSceneHandle after = scene.FindInstance(InstanceKey(7, 0));
        ASSERT_TRUE(after.IsValid());
        EXPECT_NE(after, before);

        // And the freshly extracted instance starts static: the history from
        // the old device is not carried across the switch, so nothing streaks
        // on the first frame of the new backend.
        const GPUSceneInstance* record = scene.GetInstanceRecord(after);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(MatricesEqual(DecodeGPUSceneTransform(record->PreviousTransform),
                                  DecodeGPUSceneTransform(record->CurrentTransform)));
    }
} // namespace OloEngine::Tests
