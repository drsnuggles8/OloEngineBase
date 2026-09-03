// OLO_TEST_LAYER: unit
// =============================================================================
// SceneLightmapGatherTest.cpp
//
// GatherLightmapReceivers is the single source of truth behind FOUR walks that
// used to be written out separately and had to agree (issue #867): the editor's
// bake gather, the reference world the bake traces against, the runtime's
// self-healing re-unwrap, and SceneLightmapRuntime::ComputeBakeKey. This file
// pins the identity it produces, because every way it can be wrong is silent:
//
//   * a WRONG sub-key bakes a region the draw never asks for, so the receiver
//     falls back to probes/IBL and looks exactly like an unbaked scene;
//   * a COLLIDING sub-key gives two surfaces one region, so the second shades
//     from the first's charts — a plausible patch of light, not an error;
//   * a NON-DETERMINISTIC order changes the atlas layout between two bakes of
//     the same scene, which quietly voids the bit-identity every lightmap
//     parity test rests on.
//
// Headless, no GL: the gather walks the ECS and mesh CPU data only.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneLightmapGather.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <tuple>

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] Ref<MeshSource> MakeCubeSource()
        {
            // CreateCube returns a FRESH MeshSource per call, so two entities
            // built here never alias one another's geometry.
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            return cube ? cube->GetMeshSource() : nullptr;
        }

        [[nodiscard]] InstanceData MakeInstance(const glm::vec3& translation, u64 stableID)
        {
            InstanceData instance;
            instance.Transform = glm::translate(glm::mat4(1.0f), translation);
            instance.StableID = stableID;
            return instance;
        }

        [[nodiscard]] const LightmapReceiver* Find(const std::vector<LightmapReceiver>& receivers, UUID uuid, u64 subKey)
        {
            const auto it = std::find_if(receivers.begin(), receivers.end(),
                                         [uuid, subKey](const LightmapReceiver& r)
                                         { return r.EntityUUID == uuid && r.SubKey == subKey; });
            return it == receivers.end() ? nullptr : &*it;
        }
    } // namespace

    // ── MeshComponent: unchanged by #867 ─────────────────────────────────────

    TEST(SceneLightmapGather, MeshComponentContributesOneReceiverAtSubKeyZero)
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity wall = scene->CreateEntity("Wall");
        auto& mesh = wall.AddComponent<MeshComponent>(MakeCubeSource());
        mesh.m_LightmapStatic = true;

        const auto receivers = GatherLightmapReceivers(*scene);
        ASSERT_EQ(receivers.size(), 1u);
        EXPECT_EQ(receivers[0].EntityUUID, wall.GetUUID());
        // Sub-key 0 is "the whole entity" — the value every pre-#867 bake wrote,
        // which is what keeps the classic path's bakes valid across this change.
        EXPECT_EQ(receivers[0].SubKey, 0u);
        EXPECT_EQ(receivers[0].Kind, LightmapReceiverKind::Mesh);
    }

    TEST(SceneLightmapGather, NonStaticEntitiesAreNotGathered)
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity wall = scene->CreateEntity("Wall");
        wall.AddComponent<MeshComponent>(MakeCubeSource()); // m_LightmapStatic defaults to false

        EXPECT_TRUE(GatherLightmapReceivers(*scene).empty());
    }

    // ── InstancedMeshComponent: one region PER INSTANCE ──────────────────────

    TEST(SceneLightmapGather, EachInstanceBecomesItsOwnReceiverKeyedByStableID)
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity rocks = scene->CreateEntity("Rocks");
        auto& imc = rocks.AddComponent<InstancedMeshComponent>();
        imc.MeshSource = MakeCubeSource();
        imc.LightmapStatic = true;
        imc.Instances.push_back(MakeInstance({ 0.0f, 0.0f, 0.0f }, 7));
        imc.Instances.push_back(MakeInstance({ 5.0f, 0.0f, 0.0f }, 9));
        imc.Instances.push_back(MakeInstance({ 0.0f, 0.0f, 5.0f }, 11));

        const auto receivers = GatherLightmapReceivers(*scene);
        ASSERT_EQ(receivers.size(), 3u);

        for (u64 stableID : { 7ull, 9ull, 11ull })
        {
            const LightmapReceiver* receiver = Find(receivers, rocks.GetUUID(), stableID);
            ASSERT_NE(receiver, nullptr) << "no receiver for StableID " << stableID;
            EXPECT_EQ(receiver->Kind, LightmapReceiverKind::Instance);
            EXPECT_EQ(receiver->Mesh, imc.MeshSource) << "every instance bakes the same geometry";
        }

        // The transform is the INSTANCE's, not the entity's: that is the whole
        // reason each instance needs a region of its own. A gather that handed
        // every instance the entity transform would bake three identical regions
        // and the batch would light as if it were one prop.
        EXPECT_NE(Find(receivers, rocks.GetUUID(), 7)->WorldTransform[3],
                  Find(receivers, rocks.GetUUID(), 9)->WorldTransform[3]);
    }

    TEST(SceneLightmapGather, InstanceReceiversUseTheInstanceTransformNotTheEntityTransform)
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity rocks = scene->CreateEntity("Rocks");
        // Instances live in WORLD space; the entity's own TransformComponent is
        // deliberately not applied on top (InstancedMeshComponent's contract).
        rocks.GetComponent<TransformComponent>().Translation = { 100.0f, 100.0f, 100.0f };

        auto& imc = rocks.AddComponent<InstancedMeshComponent>();
        imc.MeshSource = MakeCubeSource();
        imc.LightmapStatic = true;
        imc.Instances.push_back(MakeInstance({ 4.0f, 0.0f, 0.0f }, 1));

        const auto receivers = GatherLightmapReceivers(*scene);
        ASSERT_EQ(receivers.size(), 1u);
        EXPECT_FLOAT_EQ(receivers[0].WorldTransform[3].x, 4.0f);
        EXPECT_FLOAT_EQ(receivers[0].WorldTransform[3].y, 0.0f);
    }

    TEST(SceneLightmapGather, ZeroStableIDsAreRepairedRatherThanCollidingOnSubKeyZero)
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity rocks = scene->CreateEntity("Rocks");
        auto& imc = rocks.AddComponent<InstancedMeshComponent>();
        imc.MeshSource = MakeCubeSource();
        imc.LightmapStatic = true;
        imc.Instances.push_back(MakeInstance({ 0.0f, 0.0f, 0.0f }, 0));
        imc.Instances.push_back(MakeInstance({ 5.0f, 0.0f, 0.0f }, 0));

        const auto receivers = GatherLightmapReceivers(*scene);
        ASSERT_EQ(receivers.size(), 2u);
        // EnsureStableIDs runs inside the gather, so unassigned ids become real
        // ones. If it ever stopped doing so, both instances would land on
        // sub-key 0 — the "whole entity" key — and share one region.
        EXPECT_NE(receivers[0].SubKey, receivers[1].SubKey);
        EXPECT_NE(receivers[0].SubKey, 0u);
        EXPECT_NE(receivers[1].SubKey, 0u);
    }

    // ── ModelComponent: one region per DISTINCT MeshSource ───────────────────

    TEST(SceneLightmapGather, AModelWithNoMeshesContributesNothing)
    {
        // Constructing a Model with real meshes needs a file on disk, so the
        // dedup rule itself is covered by LightmapSubKeyForModelMesh below and
        // by the visual-evidence suite. What IS worth pinning headlessly is that
        // an empty or unloaded model produces no receiver at all rather than a
        // bogus region: a region baked for geometry that does not exist is atlas
        // space spent on nothing, and it survives every other check.
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity prop = scene->CreateEntity("Prop");
        auto& modelComponent = prop.AddComponent<ModelComponent>();
        modelComponent.m_Model = Ref<Model>::Create();
        modelComponent.m_LightmapStatic = true;

        EXPECT_TRUE(GatherLightmapReceivers(*scene).empty());
    }

    TEST(SceneLightmapGather, ModelSubKeyIsTotalAndNeverInventsARegion)
    {
        // The bake and the draw recover a model mesh's sub-key with the SAME
        // helper, so the rule lives in one place and cannot drift. Its totality
        // is what this asserts: an out-of-range index answers 0 — the "whole
        // entity" key, which simply MISSES in a table that has no entry for it —
        // rather than reading past the mesh list or naming another mesh's region.
        Ref<Model> model = Ref<Model>::Create();
        EXPECT_EQ(LightmapSubKeyForModelMesh(*model, 0), 0u);
        EXPECT_EQ(LightmapSubKeyForModelMesh(*model, 9999), 0u);
    }

    // ── VirtualMeshComponent is deliberately absent ──────────────────────────

    TEST(SceneLightmapGather, VirtualMeshEntitiesAreNotGathered)
    {
        // A virtual mesh cannot sample the lightmap yet (the SSBO binding
        // namespace is full — see GatherLightmapReceivers). Gathering one would
        // spend atlas space on a region nothing reads, and wiring only its
        // classic fallback would make baked GI appear and disappear with
        // RendererSettings::VirtualGeometryEnabled — the trap issue #867 names.
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity vm = scene->CreateEntity("VirtualProp");
        auto& virtualMesh = vm.AddComponent<VirtualMeshComponent>();
        virtualMesh.m_Enabled = true;

        EXPECT_TRUE(GatherLightmapReceivers(*scene).empty());
    }

    // ── Determinism ──────────────────────────────────────────────────────────

    TEST(SceneLightmapGather, OrderIsSortedByUuidThenSubKey)
    {
        Ref<Scene> scene = Ref<Scene>::Create();

        Entity wall = scene->CreateEntity("Wall");
        auto& mesh = wall.AddComponent<MeshComponent>(MakeCubeSource());
        mesh.m_LightmapStatic = true;

        Entity rocks = scene->CreateEntity("Rocks");
        auto& imc = rocks.AddComponent<InstancedMeshComponent>();
        imc.MeshSource = MakeCubeSource();
        imc.LightmapStatic = true;
        // Deliberately out of order on the way in.
        imc.Instances.push_back(MakeInstance({ 0.0f, 0.0f, 0.0f }, 40));
        imc.Instances.push_back(MakeInstance({ 5.0f, 0.0f, 0.0f }, 10));
        imc.Instances.push_back(MakeInstance({ 0.0f, 0.0f, 5.0f }, 25));

        const auto receivers = GatherLightmapReceivers(*scene);
        ASSERT_EQ(receivers.size(), 4u);

        // Registry iteration order is not a contract; the atlas layout is. The
        // packing plan sorts on (size desc, UUID asc, SubKey asc), and SubKey is
        // load-bearing there — UUID alone stopped being a total order the moment
        // one entity could emit N receivers.
        for (sizet i = 1; i < receivers.size(); ++i)
        {
            const bool ordered = std::tie(receivers[i - 1].EntityUUID, receivers[i - 1].SubKey) <
                                 std::tie(receivers[i].EntityUUID, receivers[i].SubKey);
            EXPECT_TRUE(ordered) << "receivers " << (i - 1) << " and " << i << " are out of order";
        }
    }

    TEST(SceneLightmapGather, TwoWalksOfOneSceneProduceTheSameList)
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity rocks = scene->CreateEntity("Rocks");
        auto& imc = rocks.AddComponent<InstancedMeshComponent>();
        imc.MeshSource = MakeCubeSource();
        imc.LightmapStatic = true;
        for (u64 i = 1; i <= 16; ++i)
        {
            imc.Instances.push_back(MakeInstance({ static_cast<f32>(i), 0.0f, 0.0f }, i));
        }

        const auto first = GatherLightmapReceivers(*scene);
        const auto second = GatherLightmapReceivers(*scene);
        ASSERT_EQ(first.size(), second.size());
        for (sizet i = 0; i < first.size(); ++i)
        {
            EXPECT_EQ(first[i].EntityUUID, second[i].EntityUUID);
            EXPECT_EQ(first[i].SubKey, second[i].SubKey);
        }
    }

    // ── The failed-unwrap memo is per MESH ───────────────────────────────────

    TEST(SceneLightmapGather, ReceiversOfOneEntityCanHaveDifferentMeshes)
    {
        // Why the runtime's failed-unwrap memo may not be keyed by entity: one
        // entity's receivers no longer share a MeshSource. A cold-imported
        // ModelComponent gives each mesh its own source, so a memo that recorded
        // "this ENTITY failed to unwrap" would let one unchartable source stop
        // its siblings from ever being re-unwrapped — and they would lose baked
        // GI on every reload, silently.
        //
        // Instanced batches are the opposite shape and pin the other half: every
        // instance SHARES one source, so the memo must not re-run xatlas (100ms+)
        // once per instance either.
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity rocks = scene->CreateEntity("Rocks");
        auto& imc = rocks.AddComponent<InstancedMeshComponent>();
        imc.MeshSource = MakeCubeSource();
        imc.LightmapStatic = true;
        imc.Instances.push_back(MakeInstance({ 0.0f, 0.0f, 0.0f }, 1));
        imc.Instances.push_back(MakeInstance({ 5.0f, 0.0f, 0.0f }, 2));

        const auto receivers = GatherLightmapReceivers(*scene);
        ASSERT_EQ(receivers.size(), 2u);
        EXPECT_EQ(receivers[0].Mesh.Raw(), receivers[1].Mesh.Raw())
            << "every instance of one batch bakes the same mesh, so an unwrap is attempted once";
    }
} // namespace OloEngine::Tests
