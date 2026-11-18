// OLO_TEST_LAYER: unit
// =============================================================================
// LocalLightWorldPoseTest.cpp — a parented local light shines from its world
// pose (issue #1707).
//
// THE BUG. The light collection packed TransformComponent::Translation, the
// PARENT-RELATIVE position, for point, spot and sphere area lights. A lantern
// mesh under a tower at (-322, 60, -462) drew its bulb at (-322, 80.3, -462)
// while its light shone from (0, 20.3, 0).
//
// WHAT THIS PINS. Scene::GetLocalLightWorldPose, the one answer every consumer
// places a local light with:
//   * a light under a translated AND rotated parent answers the same world
//     position and spot direction as a root light authored at that world pose;
//   * a root light answers its raw Translation and its authored direction,
//     bit for bit,
//     so no unparented light moves;
//   * the light entity's OWN rotation stays unused, as it always was.
// And the two CPU consumers outside the raster collection:
//   * ReferenceSceneBuilder (the bake and the path-traced reference) builds
//     the same lights from the parented scene as from the root one;
//   * the lightmap bake key follows a parent that moves its light.
// The raster collection itself (canonical record, frame, local shadows) is
// pinned on the GPU by ParentedLocalLightEvidenceTest.
//
// Headless, no GL.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneLightmap.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <span>

namespace OloEngine::Tests
{
    using PathTracing::ReferenceLight;
    using PathTracing::ReferenceScene;
    using PathTracing::ReferenceSceneBuilder;
    using PathTracing::ReferenceSceneBuildOptions;

    namespace
    {
        // The parent: translated off the origin, yawed 90 degrees and pitched
        // 30, so a light that ignores either part lands visibly elsewhere.
        const glm::vec3 kParentTranslation{ -12.0f, 6.0f, 9.0f };
        const glm::vec3 kParentEuler{ glm::radians(30.0f), glm::radians(90.0f), 0.0f };

        // The light's pose inside the parent.
        const glm::vec3 kLocalTranslation{ 1.5f, 2.0f, -0.5f };
        const glm::vec3 kLocalDirection{ 0.3f, -1.0f, 0.2f };

        [[nodiscard]] glm::mat4 ParentMatrix()
        {
            TransformComponent parent;
            parent.Translation = kParentTranslation;
            parent.SetRotationEuler(kParentEuler);
            return parent.GetTransform();
        }

        [[nodiscard]] glm::vec3 ExpectedWorldPosition()
        {
            return glm::vec3(ParentMatrix() * glm::vec4(kLocalTranslation, 1.0f));
        }

        [[nodiscard]] glm::vec3 ExpectedWorldDirection()
        {
            return glm::quat(kParentEuler) * kLocalDirection;
        }

        void ExpectVec3Near(const glm::vec3& actual, const glm::vec3& expected, f32 tolerance, const char* what)
        {
            EXPECT_NEAR(actual.x, expected.x, tolerance) << what;
            EXPECT_NEAR(actual.y, expected.y, tolerance) << what;
            EXPECT_NEAR(actual.z, expected.z, tolerance) << what;
        }

        // One light of each local type, either under the parent at the local
        // pose or at the root at the equivalent world pose.
        struct LightRig
        {
            Ref<Scene> World;
            Entity Parent;
            Entity Point;
            Entity Spot;
            Entity Sphere;
        };

        [[nodiscard]] LightRig MakeRig(bool parented)
        {
            LightRig rig;
            rig.World = Ref<Scene>::Create();
            rig.Parent = rig.World->CreateEntity("Tower");
            auto& parentTransform = rig.Parent.GetComponent<TransformComponent>();
            parentTransform.Translation = kParentTranslation;
            parentTransform.SetRotationEuler(kParentEuler);

            const glm::vec3 position = parented ? kLocalTranslation : ExpectedWorldPosition();
            const auto place = [&](const char* name)
            {
                Entity light = rig.World->CreateEntity(name);
                light.GetComponent<TransformComponent>().Translation = position;
                if (parented)
                    light.SetParent(rig.Parent);
                return light;
            };

            rig.Point = place("Point");
            auto& point = rig.Point.AddComponent<PointLightComponent>();
            point.m_Intensity = 6.0f;
            point.m_Range = 15.0f;

            rig.Spot = place("Spot");
            auto& spot = rig.Spot.AddComponent<SpotLightComponent>();
            spot.m_Intensity = 6.0f;
            spot.m_Range = 15.0f;
            spot.m_Direction = parented ? kLocalDirection : ExpectedWorldDirection();

            rig.Sphere = place("Sphere");
            auto& sphere = rig.Sphere.AddComponent<SphereAreaLightComponent>();
            sphere.m_Intensity = 6.0f;
            sphere.m_Range = 15.0f;
            sphere.m_Radius = 0.4f;

            rig.World->PropagateWorldTransforms();
            return rig;
        }
    } // namespace

    TEST(LocalLightWorldPose, AParentedLightAnswersTheWorldPoseOfItsParentChain)
    {
        LightRig rig = MakeRig(true);
        for (Entity light : { rig.Point, rig.Spot, rig.Sphere })
        {
            SCOPED_TRACE(light.GetComponent<TagComponent>().Tag);
            const Scene::LocalLightWorldPose pose = rig.World->GetLocalLightWorldPose(light);
            ExpectVec3Near(pose.Position, ExpectedWorldPosition(), 1.0e-4f,
                           "the light must shine from its world position, not its parent-local one");
        }
        const Scene::LocalLightWorldPose spotPose = rig.World->GetLocalLightWorldPose(rig.Spot);
        ExpectVec3Near(spotPose.ToWorldDirection(rig.Spot.GetComponent<SpotLightComponent>().m_Direction),
                       ExpectedWorldDirection(), 1.0e-5f, "the spot must aim along its parent's rotated frame");
    }

    TEST(LocalLightWorldPose, ARootLightAnswersItsRawTranslationAndAuthoredDirectionBitForBit)
    {
        LightRig rig = MakeRig(false);
        const glm::vec3 authored = rig.Spot.GetComponent<SpotLightComponent>().m_Direction;
        for (Entity light : { rig.Point, rig.Spot, rig.Sphere })
        {
            SCOPED_TRACE(light.GetComponent<TagComponent>().Tag);
            const Scene::LocalLightWorldPose pose = rig.World->GetLocalLightWorldPose(light);
            const glm::vec3 raw = light.GetComponent<TransformComponent>().Translation;
            EXPECT_EQ(std::memcmp(&pose.Position, &raw, sizeof(raw)), 0) << "an unparented light must not move";
            const glm::vec3 world = pose.ToWorldDirection(authored);
            EXPECT_EQ(std::memcmp(&world, &authored, sizeof(world)), 0) << "an unparented spot must not turn";
        }
    }

    // Bit for bit means every bit: a signed zero or an infinity an author
    // typed is handed back as it was, so the packing and the bake key of an
    // unparented spot cannot move by a rounding of the identity.
    TEST(LocalLightWorldPose, ARootSpotKeepsEvenASignedZeroAndAnInfinity)
    {
        LightRig rig = MakeRig(false);
        const glm::vec3 authored{ -0.0f, -1.0f, std::numeric_limits<f32>::infinity() };
        const glm::vec3 world = rig.World->GetLocalLightWorldPose(rig.Spot).ToWorldDirection(authored);
        EXPECT_EQ(std::memcmp(&world, &authored, sizeof(world)), 0);
    }

    // The direction goes through the parent the way its children's meshes do.
    // A mirrored lamp (scale -1 on X) aims its cone into the mirrored shade,
    // and a stretched parent bends it as it bends the shade; the authored
    // length is kept, since the packing never normalises.
    TEST(LocalLightWorldPose, AMirroredOrStretchedParentCarriesTheSpotLikeItsMesh)
    {
        const auto worldPose = [](const glm::vec3& parentScale)
        {
            auto world = Ref<Scene>::Create();
            Entity parent = world->CreateEntity("Lamp");
            parent.GetComponent<TransformComponent>().Scale = parentScale;
            Entity spot = world->CreateEntity("Spot");
            spot.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
            spot.AddComponent<SpotLightComponent>();
            spot.SetParent(parent);
            world->PropagateWorldTransforms();
            const Scene::LocalLightWorldPose pose = world->GetLocalLightWorldPose(spot);
            return std::pair{ pose.Position, pose.ToWorldDirection({ 1.0f, -1.0f, 0.0f }) };
        };

        const auto [mirroredAt, mirroredAim] = worldPose({ -1.0f, 1.0f, 1.0f });
        ExpectVec3Near(mirroredAt, { -1.0f, 0.0f, 0.0f }, 1.0e-6f, "the mirrored lamp's light is not where its mesh is");
        ExpectVec3Near(mirroredAim, { -1.0f, -1.0f, 0.0f }, 1.0e-6f,
                       "the mirrored lamp's cone does not point into its mirrored shade");

        const auto [stretchedAt, stretchedAim] = worldPose({ 2.0f, 1.0f, 1.0f });
        ExpectVec3Near(stretchedAt, { 2.0f, 0.0f, 0.0f }, 1.0e-6f, "the stretched lamp's light is not where its mesh is");
        const glm::vec3 bent = glm::vec3(2.0f, -1.0f, 0.0f) * (std::sqrt(2.0f) / std::sqrt(5.0f));
        ExpectVec3Near(stretchedAim, bent, 1.0e-6f, "the stretched parent did not bend the cone as it bends the shade");
    }

    TEST(LocalLightWorldPose, TheLightEntitysOwnRotationDoesNotTurnItsSpot)
    {
        LightRig rig = MakeRig(true);
        const glm::vec3 before = rig.World->GetLocalLightWorldPose(rig.Spot).ToWorldDirection(kLocalDirection);

        // As before #1707, turning the light entity itself does not aim a spot:
        // m_Direction is authored in the parent's frame, like Translation.
        Entity spot = rig.Spot;
        spot.GetComponent<TransformComponent>().SetRotationEuler({ 0.0f, 0.0f, glm::radians(70.0f) });
        rig.World->PropagateWorldTransforms();
        const Scene::LocalLightWorldPose after = rig.World->GetLocalLightWorldPose(rig.Spot);
        ExpectVec3Near(after.ToWorldDirection(kLocalDirection), before, 1.0e-5f,
                       "the light entity's own rotation leaked into the spot direction");
        ExpectVec3Near(after.Position, ExpectedWorldPosition(), 1.0e-4f, "a rotation about its own origin moved the light");
    }

    TEST(LocalLightWorldPose, AParentedLightFeedsTheBakeTheSameLightAsItsRootTwin)
    {
        const auto build = [](bool parented)
        {
            LightRig rig = MakeRig(parented);
            ReferenceSceneBuilder builder;
            builder.AddScene(*rig.World, {});
            return builder.Build(ReferenceSceneBuildOptions{});
        };
        const ReferenceScene parented = build(true);
        const ReferenceScene root = build(false);
        ASSERT_TRUE(parented.IsBuilt());
        ASSERT_TRUE(root.IsBuilt());

        const std::span<const ReferenceLight> parentedLights = parented.GetLights();
        const std::span<const ReferenceLight> rootLights = root.GetLights();
        ASSERT_EQ(parentedLights.size(), 3u);
        ASSERT_EQ(rootLights.size(), parentedLights.size());
        for (sizet i = 0; i < rootLights.size(); ++i)
        {
            SCOPED_TRACE(i);
            EXPECT_EQ(parentedLights[i].Type, rootLights[i].Type);
            ExpectVec3Near(parentedLights[i].Position, rootLights[i].Position, 1.0e-4f,
                           "the bake would light the scene from the parent-local position");
            ExpectVec3Near(parentedLights[i].Direction, rootLights[i].Direction, 1.0e-5f,
                           "the bake would aim the spot along its unrotated local direction");
        }
    }

    TEST(LocalLightWorldPose, MovingAParentStalesTheBakeItsLightMovedIn)
    {
        LightRig rig = MakeRig(true);
        const SceneLightmapSettings settings = rig.World->GetLightmapSettings();
        const u64 original = SceneLightmapRuntime::ComputeBakeKey(*rig.World, settings);

        // The lights' own components are untouched: only the parent moves, and
        // with it the place every light shines from.
        Entity tower = rig.Parent;
        auto& parent = tower.GetComponent<TransformComponent>();
        parent.Translation += glm::vec3(0.0f, 0.0f, 3.0f);
        rig.World->PropagateWorldTransforms();
        EXPECT_NE(SceneLightmapRuntime::ComputeBakeKey(*rig.World, settings), original)
            << "the parent moved every light the bake integrated; the bake must read as stale";

        parent.Translation -= glm::vec3(0.0f, 0.0f, 3.0f);
        parent.SetRotationEuler(kParentEuler + glm::vec3(0.0f, glm::radians(45.0f), 0.0f));
        rig.World->PropagateWorldTransforms();
        EXPECT_NE(SceneLightmapRuntime::ComputeBakeKey(*rig.World, settings), original)
            << "turning the parent moved and re-aimed its lights; the bake must read as stale";
    }
} // namespace OloEngine::Tests
