#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// AnimalPathFollowsTerrainTest — Functional Test, issue #1259.
//
// Seams under test:
//   AnimalPathComponent::m_FollowTerrain × TerrainComponent's CPU height field
//   × the entity transforms of both, and the benchmark capture step
//   (Benchmark::ApplyEntityMotion) × Scene::AdvanceAnimalPaths.
//
// The acceptance scene walks a herd across the 30 m relief of the
// meadow-to-woodland terrain. A path is a closed-form figure at a fixed height,
// so without ground-following every animal floats over the dips and sinks into
// the rises. And a capture is an edit-mode tick, which never runs the gameplay
// scheduler's "AnimalPaths" system, so before #1259 the herd in every benchmark
// capture walked on the spot. Each case below fails if its wiring is absent.
// =============================================================================

#include "Rendering/PropertyTests/RenderPropertyTest.h"

#include "OloEngine/Renderer/Benchmark/BenchmarkCapture.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Terrain/TerrainData.h"

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <string>

using namespace OloEngine;

namespace
{
    constexpr u32 kResolution = 65;
    constexpr f32 kWorldSize = 64.0f;
    constexpr f32 kHeightScale = 8.0f;
    const glm::vec3 kTerrainOrigin{ 10.0f, -3.0f, 20.0f };
    constexpr f32 kDt = 1.0f / 60.0f;

    // A field that rises along X and ripples along Z, so an animal on an
    // ellipse crosses different heights and a constant answer cannot pass.
    TArray<f32> MakeField()
    {
        TArray<f32> heights;
        heights.SetNum(static_cast<i32>(kResolution * kResolution));
        for (u32 z = 0; z < kResolution; ++z)
        {
            for (u32 x = 0; x < kResolution; ++x)
            {
                const f32 fx = static_cast<f32>(x) / static_cast<f32>(kResolution - 1);
                const f32 fz = static_cast<f32>(z) / static_cast<f32>(kResolution - 1);
                heights[static_cast<i32>(z * kResolution + x)] = 0.2f + 0.6f * fx + 0.1f * std::sin(fz * 9.0f);
            }
        }
        return heights;
    }

    // World height of the field above, computed independently of the scene.
    f32 ExpectedGround(const TArray<f32>& heights, f32 worldX, f32 worldZ)
    {
        const f32 nx = (worldX - kTerrainOrigin.x) / kWorldSize;
        const f32 nz = (worldZ - kTerrainOrigin.z) / kWorldSize;
        return TerrainData::SampleHeight(heights, kResolution, nx, nz) * kHeightScale + kTerrainOrigin.y;
    }

    struct TerrainScene
    {
        Ref<Scene> SceneRef = Scene::Create();
        TArray<f32> Heights = MakeField();

        TerrainScene()
        {
            Entity terrainEntity = SceneRef->CreateEntity("Ground");
            terrainEntity.GetComponent<TransformComponent>().Translation = kTerrainOrigin;
            auto& terrain = terrainEntity.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = false;
            terrain.m_WorldSizeX = kWorldSize;
            terrain.m_WorldSizeZ = kWorldSize;
            terrain.m_HeightScale = kHeightScale;
            terrain.m_TerrainData = Ref<TerrainData>::Create();
            terrain.m_TerrainData->SetHeights(kResolution, Heights);
        }

        Entity AddAnimal(const char* name, glm::vec3 origin, bool followTerrain)
        {
            Entity animal = SceneRef->CreateEntity(name);
            animal.GetComponent<TransformComponent>().Translation = origin;
            auto& path = animal.AddComponent<AnimalPathComponent>();
            path.m_RadiusX = 9.0f;
            path.m_RadiusZ = 6.0f;
            path.m_FollowTerrain = followTerrain;
            return animal;
        }
    };
} // namespace

// The animal stands on the surface at every step of its figure, through the
// terrain entity's translation, and the height it reads changes as it moves.
TEST(AnimalPathFollowsTerrain, TheAnimalStandsOnTheSurfaceAsItWalks)
{
    OLO_ENSURE_GPU_OR_SKIP();
    TerrainScene world;
    Entity animal = world.AddAnimal("Walker", { 42.0f, 50.0f, 52.0f }, /*followTerrain=*/true);

    f32 lowest = 1.0e9f;
    f32 highest = -1.0e9f;
    for (u32 step = 0; step < 240; ++step)
    {
        world.SceneRef->AdvanceAnimalPaths(Timestep{ kDt });
        const glm::vec3 p = animal.GetComponent<TransformComponent>().Translation;
        const f32 expected = ExpectedGround(world.Heights, p.x, p.z);
        ASSERT_NEAR(p.y, expected, 1.0e-3f) << "step " << step << " at (" << p.x << ", " << p.z << ")";
        lowest = std::min(lowest, p.y);
        highest = std::max(highest, p.y);
    }
    // The figure spans 18 m of a slope that rises 4.8 m over 64 m; a path that
    // stayed at one height would read a range of zero.
    EXPECT_GT(highest - lowest, 0.5f);
}

// Control arm: the same path without the flag keeps its authored height, and
// an animal off the terrain keeps its height instead of dropping to zero.
TEST(AnimalPathFollowsTerrain, WithoutTheFlagOrOffTheTerrainTheHeightStays)
{
    OLO_ENSURE_GPU_OR_SKIP();
    TerrainScene world;
    Entity floating = world.AddAnimal("Authored", { 42.0f, 50.0f, 52.0f }, /*followTerrain=*/false);
    Entity offTerrain = world.AddAnimal("Outside", { -200.0f, 7.5f, -200.0f }, /*followTerrain=*/true);

    for (u32 step = 0; step < 60; ++step)
        world.SceneRef->AdvanceAnimalPaths(Timestep{ kDt });

    EXPECT_NEAR(floating.GetComponent<TransformComponent>().Translation.y, 50.0f, 1.0e-5f);
    EXPECT_NEAR(offTerrain.GetComponent<TransformComponent>().Translation.y, 7.5f, 1.0e-5f);
}

// A benchmark capture step advances the scene's animal paths by exactly one
// fixed step per rendered frame, even for a manifest with no EntityMotion.
// Headless: no terrain, no GPU.
TEST(AnimalPathFollowsTerrain, ACaptureStepWalksTheHerdOneFixedStepPerFrame)
{
    Ref<Scene> scene = Scene::Create();
    Entity animal = scene->CreateEntity("Herd 0");
    animal.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
    auto& path = animal.AddComponent<AnimalPathComponent>();
    path.m_RadiusX = 12.0f;
    path.m_RadiusZ = 8.0f;

    Benchmark::BenchmarkManifest manifest;
    manifest.FixedDtSeconds = kDt;
    constexpr u32 kFrames = 90;
    for (u32 frame = 0; frame < kFrames; ++frame)
    {
        std::string error;
        ASSERT_TRUE(Benchmark::ApplyEntityMotion(*scene, manifest, frame, error)) << error;
    }

    const auto& advanced = animal.GetComponent<AnimalPathComponent>();
    EXPECT_NEAR(advanced.m_ElapsedSeconds, static_cast<f32>(kFrames) * kDt, 1.0e-4f);
    const glm::vec3 expected = AnimalPathOffsetAt(advanced, advanced.m_ElapsedSeconds);
    const glm::vec3 actual = animal.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(actual.x, expected.x, 1.0e-4f);
    EXPECT_NEAR(actual.z, expected.z, 1.0e-4f);
    EXPECT_GT(glm::length(glm::vec2(actual.x, actual.z) - glm::vec2(AnimalPathOffsetAt(advanced, 0.0f).x,
                                                                    AnimalPathOffsetAt(advanced, 0.0f).z)),
              0.1f)
        << "the animal did not move: the capture step is not advancing the path";
}

// A parented animal stands on the ground under where it IS. The path writes
// the LOCAL translation; sampling the terrain at it and writing the world
// height back into local Y left an animal under an offset group floating or
// sunk by the group's offset.
TEST(AnimalPathFollowsTerrain, AParentedAnimalStandsOnTheGroundUnderItsWorldPosition)
{
    OLO_ENSURE_GPU_OR_SKIP();
    TerrainScene world;
    Entity group = world.SceneRef->CreateEntity("Herd group");
    group.GetComponent<TransformComponent>().Translation = { 8.0f, 5.0f, -6.0f };
    Entity animal = world.AddAnimal("Grouped", { 34.0f, 0.0f, 58.0f }, /*followTerrain=*/true);
    animal.SetParent(group);

    for (u32 step = 0; step < 120; ++step)
    {
        world.SceneRef->AdvanceAnimalPaths(Timestep{ kDt });
        const glm::mat4 parentWorld = group.GetComponent<TransformComponent>().GetTransform();
        const glm::vec3 p = glm::vec3(parentWorld * glm::vec4(animal.GetComponent<TransformComponent>().Translation, 1.0f));
        ASSERT_NEAR(p.y, ExpectedGround(world.Heights, p.x, p.z), 1.0e-3f)
            << "step " << step << ": the parented animal is not on the ground at (" << p.x << ", " << p.z << ")";
    }
}
