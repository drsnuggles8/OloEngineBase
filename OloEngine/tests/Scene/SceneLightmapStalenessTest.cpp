// OLO_TEST_LAYER: unit
// =============================================================================
// SceneLightmapStalenessTest.cpp
//
// The bake key IS the staleness gate (issue #439): a lightmap whose stored key
// no longer matches the live scene is never sampled. These tests pin the key's
// semantics — what MUST change it (anything the bake captured) and what must
// NOT (things the bake never saw) — because both failure directions are silent:
// a key that misses an input renders confidently from stale data (the feature's
// signature failure), and a key that includes a volatile input reads as
// permanently stale.
//
// Headless, no GL: ComputeBakeKey walks the ECS and mesh CPU data only.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneLightmap.h"

#include <glm/glm.hpp>

namespace OloEngine::Tests
{
    namespace
    {
        struct StalenessFixture
        {
            Ref<Scene> World;
            Entity StaticWall;
            Entity DynamicSphere;
            Entity Light;
        };

        StalenessFixture MakeFixture()
        {
            StalenessFixture f;
            f.World = Ref<Scene>::Create();

            f.StaticWall = f.World->CreateEntity("Static Wall");
            f.StaticWall.GetComponent<TransformComponent>().Translation = { -3.0f, 2.0f, 0.0f };
            auto& wallMesh = f.StaticWall.AddComponent<MeshComponent>();
            wallMesh.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                wallMesh.m_MeshSource = cube->GetMeshSource();
            wallMesh.m_LightmapStatic = true;
            auto& wallMaterial = f.StaticWall.AddComponent<MaterialComponent>();
            wallMaterial.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.1f, 0.1f, 1.0f));

            f.DynamicSphere = f.World->CreateEntity("Dynamic Sphere");
            f.DynamicSphere.GetComponent<TransformComponent>().Translation = { 1.0f, 1.0f, 0.0f };
            auto& sphereMesh = f.DynamicSphere.AddComponent<MeshComponent>();
            sphereMesh.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                sphereMesh.m_MeshSource = cube->GetMeshSource();
            // deliberately NOT lightmap-static

            f.Light = f.World->CreateEntity("Key Light");
            f.Light.GetComponent<TransformComponent>().Translation = { 0.0f, 3.0f, 0.0f };
            auto& point = f.Light.AddComponent<PointLightComponent>();
            point.m_Intensity = 8.0f;
            point.m_Range = 20.0f;

            return f;
        }
    } // namespace

    TEST(SceneLightmapStaleness, KeyIsDeterministicAcrossRecomputation)
    {
        StalenessFixture f = MakeFixture();
        const SceneLightmapSettings settings = f.World->GetLightmapSettings();
        const u64 a = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);
        const u64 b = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);
        EXPECT_EQ(a, b) << "the same scene must hash to the same key — a volatile input leaked in";
        EXPECT_NE(a, 0u);
    }

    TEST(SceneLightmapStaleness, MovingAStaticEntityChangesTheKeyAndMovingItBackRestoresIt)
    {
        StalenessFixture f = MakeFixture();
        const SceneLightmapSettings settings = f.World->GetLightmapSettings();
        const u64 original = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);

        auto& transform = f.StaticWall.GetComponent<TransformComponent>();
        const glm::vec3 originalPos = transform.Translation;
        transform.Translation = originalPos + glm::vec3(0.5f, 0.0f, 0.0f);
        const u64 moved = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);
        EXPECT_NE(moved, original) << "moving baked geometry MUST stale the bake";

        // Exact restoration must restore the key bit-exactly — the key hashes
        // raw float bits, so an entity moved back to precisely its baked pose
        // legitimately un-stales the bake.
        transform.Translation = originalPos;
        const u64 restored = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);
        EXPECT_EQ(restored, original);
    }

    TEST(SceneLightmapStaleness, MovingADynamicEntityDoesNotChangeTheKey)
    {
        StalenessFixture f = MakeFixture();
        const SceneLightmapSettings settings = f.World->GetLightmapSettings();
        const u64 original = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);

        f.DynamicSphere.GetComponent<TransformComponent>().Translation += glm::vec3(2.0f, 0.0f, 1.0f);
        const u64 afterMove = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);
        EXPECT_EQ(afterMove, original)
            << "a non-lightmap-static entity is not in the bake; moving it must not stale the lightmap";
    }

    TEST(SceneLightmapStaleness, LightAndMaterialChangesChangeTheKey)
    {
        StalenessFixture f = MakeFixture();
        const SceneLightmapSettings settings = f.World->GetLightmapSettings();
        const u64 original = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);

        f.Light.GetComponent<PointLightComponent>().m_Intensity = 9.0f;
        const u64 afterLight = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);
        EXPECT_NE(afterLight, original) << "the bake integrated this light; its intensity is a key input";

        f.Light.GetComponent<PointLightComponent>().m_Intensity = 8.0f;
        f.StaticWall.GetComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.1f, 0.7f, 0.1f, 1.0f));
        const u64 afterMaterial = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);
        EXPECT_NE(afterMaterial, original) << "bounce colour comes from material factors; they are key inputs";
    }

    TEST(SceneLightmapStaleness, BakeSettingsAreKeyInputs)
    {
        StalenessFixture f = MakeFixture();
        SceneLightmapSettings settings = f.World->GetLightmapSettings();
        const u64 original = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);

        settings.SamplesPerTexel *= 2;
        EXPECT_NE(SceneLightmapRuntime::ComputeBakeKey(*f.World, settings), original);

        settings = f.World->GetLightmapSettings();
        settings.TexelsPerMeter += 1.0f;
        EXPECT_NE(SceneLightmapRuntime::ComputeBakeKey(*f.World, settings), original);

        // Enabled and Intensity are RUNTIME knobs, not bake inputs — toggling
        // them must not stale a bake (the GPU visual test relies on this to
        // A/B the same bake ON and OFF).
        settings = f.World->GetLightmapSettings();
        settings.Enabled = false;
        settings.Intensity = 0.5f;
        EXPECT_EQ(SceneLightmapRuntime::ComputeBakeKey(*f.World, settings), original);
    }

    TEST(SceneLightmapStaleness, TogglingTheStaticFlagChangesTheKey)
    {
        StalenessFixture f = MakeFixture();
        const SceneLightmapSettings settings = f.World->GetLightmapSettings();
        const u64 original = SceneLightmapRuntime::ComputeBakeKey(*f.World, settings);

        // Marking ANOTHER entity static changes the baked set — new key.
        f.DynamicSphere.GetComponent<MeshComponent>().m_LightmapStatic = true;
        EXPECT_NE(SceneLightmapRuntime::ComputeBakeKey(*f.World, settings), original);

        // And un-marking the wall empties/changes the set again.
        f.DynamicSphere.GetComponent<MeshComponent>().m_LightmapStatic = false;
        f.StaticWall.GetComponent<MeshComponent>().m_LightmapStatic = false;
        EXPECT_NE(SceneLightmapRuntime::ComputeBakeKey(*f.World, settings), original);
    }
} // namespace OloEngine::Tests
