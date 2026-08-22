// =============================================================================
// ComponentRoundTripTest.cpp
//
// Catches the OloEditor breakage class where a developer adds a field
// to a component struct but forgets to wire it into either the
// SceneSerializer's emit-side or its YAML-read-side. The bug is silent
// at runtime: the field gets default-initialised on load, and the user
// sees "I set this value, saved the scene, and now it's reset" — only
// the value the deserializer can recover is preserved.
//
// What this test does
// -------------------
//   For each component type covered below:
//     1. Construct a Scene + Entity with the component set to recognisable
//        non-default values (chosen to differ from the engine's defaults
//        on every field so a missing read or write is detectable).
//     2. Call `SceneSerializer::SerializeToYAML()` to get a full-scene
//        YAML string — exercises the same code path the editor uses
//        when saving to disk.
//     3. Construct a fresh Scene + SceneSerializer and call
//        `DeserializeFromYAML(string)`. This exercises the same code
//        path the editor uses when loading a scene.
//     4. Look up the round-tripped entity by name and assert every
//        component field matches the originals.
//
// Why this isn't a Functional test
// --------------------------------
//   No `Scene::OnUpdateRuntime` ticks happen here — we're testing the
//   serializer's symmetry, not any subsystem's runtime behaviour. The
//   Functional test axis is for cross-subsystem state contracts driven
//   by real ticks; this is a simpler property-style check on the
//   serialiser code path.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Core/YAMLConverters.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <regex>
#include <string>
#include <unordered_set>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kFloatEpsilon = 1e-5f;

        // Tag used to identify the entity on both ends of the round-trip.
        // Distinctive enough that the wrong entity won't accidentally
        // pass on a multi-entity load.
        constexpr const char* kTestTag = "RoundTripEntity_uniqueA72F";

        Entity FindByTag(Scene& scene, const char* tag)
        {
            for (auto e : scene.GetAllEntitiesWith<TagComponent>())
            {
                Entity ent{ e, &scene };
                if (ent.GetComponent<TagComponent>().Tag == tag)
                    return ent;
            }
            return {};
        }
    } // namespace

    // -------------------------------------------------------------------------
    // TransformComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, TransformComponentSurvivesYAMLRoundTrip)
    {
        // Distinctive values: each axis different, each field non-default.
        const glm::vec3 expectedTranslation{ 1.5f, -2.25f, 7.125f };
        const glm::vec3 expectedRotationEuler{ 0.2f, -0.3f, 1.1f };
        const glm::vec3 expectedScale{ 0.5f, 2.0f, 3.75f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& tc = entity.GetComponent<TransformComponent>();
            tc.Translation = expectedTranslation;
            tc.SetRotationEuler(expectedRotationEuler);
            tc.Scale = expectedScale;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty()) << "SerializeToYAML produced an empty string.";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "DeserializeFromYAML rejected the just-serialised scene.";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored))
            << "Round-tripped entity '" << kTestTag << "' not found on the reloaded scene.";

        const auto& tc = restored.GetComponent<TransformComponent>();
        EXPECT_NEAR(tc.Translation.x, expectedTranslation.x, kFloatEpsilon);
        EXPECT_NEAR(tc.Translation.y, expectedTranslation.y, kFloatEpsilon);
        EXPECT_NEAR(tc.Translation.z, expectedTranslation.z, kFloatEpsilon);

        const glm::vec3 restoredEuler = tc.GetRotationEuler();
        EXPECT_NEAR(restoredEuler.x, expectedRotationEuler.x, kFloatEpsilon);
        EXPECT_NEAR(restoredEuler.y, expectedRotationEuler.y, kFloatEpsilon);
        EXPECT_NEAR(restoredEuler.z, expectedRotationEuler.z, kFloatEpsilon);

        EXPECT_NEAR(tc.Scale.x, expectedScale.x, kFloatEpsilon);
        EXPECT_NEAR(tc.Scale.y, expectedScale.y, kFloatEpsilon);
        EXPECT_NEAR(tc.Scale.z, expectedScale.z, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // DestructibleComponent (issue #459)
    //
    // All-trivial, so its scene (de)serialize is fully OloHeaderTool-generated —
    // this pins that the codegen actually round-trips every authored field
    // (including the AssetHandle chunk-mesh reference), and that the two
    // OLO_SERIALIZE(Skip) runtime flags are DROPPED (a saved scene must not carry
    // a mid-play "broken"/"pending" state).
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, DestructibleComponentSurvivesYAMLRoundTrip)
    {
        const AssetHandle expectedChunkMesh{ 0x0123456789ABCDEFull };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& dc = entity.AddComponent<DestructibleComponent>();
            dc.m_Health = 42.5f;
            dc.m_MaxHealth = 250.0f;
            dc.m_DamageThreshold = 5.5f;
            dc.m_ChunkMesh = expectedChunkMesh;
            dc.m_ChunkCount = 12;
            dc.m_ChunkScale = 0.4f;
            dc.m_ChunkMass = 2.5f;
            dc.m_ExplosionImpulse = 9.0f;
            dc.m_DebrisLifetime = 3.5f;
            dc.m_BreakOnJointBreak = false;
            dc.m_DestroyOnBreak = false;
            // Runtime flags — must NOT survive the round-trip (Skip).
            dc.m_Broken = true;
            dc.m_PendingBreak = true;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty()) << "SerializeToYAML produced an empty string.";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "DeserializeFromYAML rejected the just-serialised scene.";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored))
            << "Round-tripped entity '" << kTestTag << "' not found on the reloaded scene.";
        ASSERT_TRUE(restored.HasComponent<DestructibleComponent>());

        const auto& dc = restored.GetComponent<DestructibleComponent>();
        EXPECT_NEAR(dc.m_Health, 42.5f, kFloatEpsilon);
        EXPECT_NEAR(dc.m_MaxHealth, 250.0f, kFloatEpsilon);
        EXPECT_NEAR(dc.m_DamageThreshold, 5.5f, kFloatEpsilon);
        EXPECT_EQ(static_cast<u64>(dc.m_ChunkMesh), static_cast<u64>(expectedChunkMesh));
        EXPECT_EQ(dc.m_ChunkCount, 12u);
        EXPECT_NEAR(dc.m_ChunkScale, 0.4f, kFloatEpsilon);
        EXPECT_NEAR(dc.m_ChunkMass, 2.5f, kFloatEpsilon);
        EXPECT_NEAR(dc.m_ExplosionImpulse, 9.0f, kFloatEpsilon);
        EXPECT_NEAR(dc.m_DebrisLifetime, 3.5f, kFloatEpsilon);
        EXPECT_FALSE(dc.m_BreakOnJointBreak);
        EXPECT_FALSE(dc.m_DestroyOnBreak);
        // Skip fields reload at their constructor defaults.
        EXPECT_FALSE(dc.m_Broken) << "m_Broken is OLO_SERIALIZE(Skip); it must not persist in scene YAML.";
        EXPECT_FALSE(dc.m_PendingBreak) << "m_PendingBreak is OLO_SERIALIZE(Skip); it must not persist.";
    }

    // -------------------------------------------------------------------------
    // CameraComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, CameraComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedFov = 1.2345f;
        const f32 expectedNear = 0.07f;
        const f32 expectedFar = 412.5f;
        const f32 expectedOrthoSize = 12.5f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& cc = entity.AddComponent<CameraComponent>();
            cc.Primary = true;
            cc.FixedAspectRatio = true;
            cc.Camera.SetPerspectiveVerticalFOV(expectedFov);
            cc.Camera.SetPerspectiveNearClip(expectedNear);
            cc.Camera.SetPerspectiveFarClip(expectedFar);
            cc.Camera.SetOrthographicSize(expectedOrthoSize);
            cc.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<CameraComponent>())
            << "CameraComponent was dropped during round-trip.";

        const auto& cc = restored.GetComponent<CameraComponent>();
        EXPECT_TRUE(cc.Primary);
        EXPECT_TRUE(cc.FixedAspectRatio);
        EXPECT_EQ(cc.Camera.GetProjectionType(), SceneCamera::ProjectionType::Perspective);
        EXPECT_NEAR(cc.Camera.GetPerspectiveVerticalFOV(), expectedFov, kFloatEpsilon);
        EXPECT_NEAR(cc.Camera.GetPerspectiveNearClip(), expectedNear, kFloatEpsilon);
        EXPECT_NEAR(cc.Camera.GetPerspectiveFarClip(), expectedFar, kFloatEpsilon);
        EXPECT_NEAR(cc.Camera.GetOrthographicSize(), expectedOrthoSize, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // SpriteRendererComponent — minimal struct (a colour), but exercises
    // a different serializer path (2D-renderer family) so still useful
    // as a separate guard.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, SpriteRendererComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec4 expectedColor{ 0.25f, 0.75f, 0.125f, 0.875f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& sr = entity.AddComponent<SpriteRendererComponent>();
            sr.Color = expectedColor;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<SpriteRendererComponent>())
            << "SpriteRendererComponent was dropped during round-trip.";

        const auto& sr = restored.GetComponent<SpriteRendererComponent>();
        EXPECT_NEAR(sr.Color.r, expectedColor.r, kFloatEpsilon);
        EXPECT_NEAR(sr.Color.g, expectedColor.g, kFloatEpsilon);
        EXPECT_NEAR(sr.Color.b, expectedColor.b, kFloatEpsilon);
        EXPECT_NEAR(sr.Color.a, expectedColor.a, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // CircleRendererComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, CircleRendererComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec4 expectedColor{ 0.3f, 0.6f, 0.9f, 0.5f };
        const f32 expectedThickness = 0.7f;
        const f32 expectedFade = 0.123f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& cr = entity.AddComponent<CircleRendererComponent>();
            cr.Color = expectedColor;
            cr.Thickness = expectedThickness;
            cr.Fade = expectedFade;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<CircleRendererComponent>())
            << "CircleRendererComponent dropped during round-trip.";

        const auto& cr = restored.GetComponent<CircleRendererComponent>();
        EXPECT_NEAR(cr.Color.r, expectedColor.r, kFloatEpsilon);
        EXPECT_NEAR(cr.Color.a, expectedColor.a, kFloatEpsilon);
        EXPECT_NEAR(cr.Thickness, expectedThickness, kFloatEpsilon);
        EXPECT_NEAR(cr.Fade, expectedFade, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // DirectionalLightComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, DirectionalLightComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec3 expectedColor{ 0.9f, 0.4f, 0.1f };
        const f32 expectedIntensity = 2.5f;
        const bool expectedCastShadows = false; // non-default

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& dl = entity.AddComponent<DirectionalLightComponent>();
            dl.m_Color = expectedColor;
            dl.m_Intensity = expectedIntensity;
            dl.m_CastShadows = expectedCastShadows;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<DirectionalLightComponent>())
            << "DirectionalLightComponent dropped during round-trip.";

        const auto& dl = restored.GetComponent<DirectionalLightComponent>();
        EXPECT_NEAR(dl.m_Color.r, expectedColor.r, kFloatEpsilon);
        EXPECT_NEAR(dl.m_Color.g, expectedColor.g, kFloatEpsilon);
        EXPECT_NEAR(dl.m_Color.b, expectedColor.b, kFloatEpsilon);
        EXPECT_NEAR(dl.m_Intensity, expectedIntensity, kFloatEpsilon);
        EXPECT_EQ(dl.m_CastShadows, expectedCastShadows);
    }

    // -------------------------------------------------------------------------
    // PointLightComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, PointLightComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec3 expectedColor{ 0.2f, 0.8f, 0.4f };
        const f32 expectedIntensity = 3.0f;
        const f32 expectedRange = 25.5f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& pl = entity.AddComponent<PointLightComponent>();
            pl.m_Color = expectedColor;
            pl.m_Intensity = expectedIntensity;
            pl.m_Range = expectedRange;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<PointLightComponent>());

        const auto& pl = restored.GetComponent<PointLightComponent>();
        EXPECT_NEAR(pl.m_Color.r, expectedColor.r, kFloatEpsilon);
        EXPECT_NEAR(pl.m_Color.g, expectedColor.g, kFloatEpsilon);
        EXPECT_NEAR(pl.m_Color.b, expectedColor.b, kFloatEpsilon);
        EXPECT_NEAR(pl.m_Intensity, expectedIntensity, kFloatEpsilon);
        EXPECT_NEAR(pl.m_Range, expectedRange, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // SpotLightComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, SpotLightComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec3 expectedColor{ 0.7f, 0.2f, 0.6f };
        const f32 expectedIntensity = 1.75f;
        const f32 expectedRange = 18.0f;
        const f32 expectedInner = 9.0f;
        const f32 expectedOuter = 22.5f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& sl = entity.AddComponent<SpotLightComponent>();
            sl.m_Color = expectedColor;
            sl.m_Intensity = expectedIntensity;
            sl.m_Range = expectedRange;
            sl.m_InnerCutoff = expectedInner;
            sl.m_OuterCutoff = expectedOuter;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<SpotLightComponent>());

        const auto& sl = restored.GetComponent<SpotLightComponent>();
        EXPECT_NEAR(sl.m_Color.r, expectedColor.r, kFloatEpsilon);
        EXPECT_NEAR(sl.m_Intensity, expectedIntensity, kFloatEpsilon);
        EXPECT_NEAR(sl.m_Range, expectedRange, kFloatEpsilon);
        EXPECT_NEAR(sl.m_InnerCutoff, expectedInner, kFloatEpsilon);
        EXPECT_NEAR(sl.m_OuterCutoff, expectedOuter, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // SphereAreaLightComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, SphereAreaLightComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec3 expectedColor{ 0.85f, 0.12f, 0.93f };
        const f32 expectedIntensity = 2.5f;
        const f32 expectedRadius = 0.42f;
        const f32 expectedRange = 17.0f;
        const bool expectedCastShadows = true;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& al = entity.AddComponent<SphereAreaLightComponent>();
            al.m_Color = expectedColor;
            al.m_Intensity = expectedIntensity;
            al.m_Radius = expectedRadius;
            al.m_Range = expectedRange;
            al.m_CastShadows = expectedCastShadows;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<SphereAreaLightComponent>());

        const auto& al = restored.GetComponent<SphereAreaLightComponent>();
        EXPECT_NEAR(al.m_Color.r, expectedColor.r, kFloatEpsilon);
        EXPECT_NEAR(al.m_Color.g, expectedColor.g, kFloatEpsilon);
        EXPECT_NEAR(al.m_Color.b, expectedColor.b, kFloatEpsilon);
        EXPECT_NEAR(al.m_Intensity, expectedIntensity, kFloatEpsilon);
        EXPECT_NEAR(al.m_Radius, expectedRadius, kFloatEpsilon);
        EXPECT_NEAR(al.m_Range, expectedRange, kFloatEpsilon);
        EXPECT_EQ(al.m_CastShadows, expectedCastShadows);
    }

    // -------------------------------------------------------------------------
    // TextComponent — has a defaulted operator==() so we can compare in one
    // shot after round-trip.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, TextComponentSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        TextComponent expected;
        expected.TextString = "Hello round-trip — αβγ ✓";
        expected.Color = { 0.5f, 0.6f, 0.7f, 0.8f };
        expected.Kerning = 0.125f;
        expected.LineSpacing = 0.0625f;

        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& tc = entity.AddComponent<TextComponent>();
            tc.TextString = expected.TextString;
            tc.Color = expected.Color;
            tc.Kerning = expected.Kerning;
            tc.LineSpacing = expected.LineSpacing;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<TextComponent>());

        const auto& tc = restored.GetComponent<TextComponent>();
        EXPECT_EQ(tc.TextString, expected.TextString);
        EXPECT_NEAR(tc.Color.r, expected.Color.r, kFloatEpsilon);
        EXPECT_NEAR(tc.Color.a, expected.Color.a, kFloatEpsilon);
        EXPECT_NEAR(tc.Kerning, expected.Kerning, kFloatEpsilon);
        EXPECT_NEAR(tc.LineSpacing, expected.LineSpacing, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // LocalizedTextComponent — keys-only tag component that auto-localizes
    // the entity's TextComponent. Round-trip must preserve the lookup key.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, LocalizedTextComponentSurvivesYAMLRoundTrip)
    {
        const std::string expectedKey = "ui.main_menu.play";
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            entity.AddComponent<LocalizedTextComponent>(expectedKey);
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<LocalizedTextComponent>());
        EXPECT_EQ(restored.GetComponent<LocalizedTextComponent>().LocalizationKey, expectedKey);
    }

    // -------------------------------------------------------------------------
    // Rigidbody2DComponent — exercises the physics-flavor serializer.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, Rigidbody2DComponentSurvivesYAMLRoundTrip)
    {
        const auto expectedType = Rigidbody2DComponent::BodyType::Dynamic;
        const bool expectedFixedRotation = true;
        const glm::vec2 expectedLinearVelocity{ 3.0f, -4.0f }; // was silently dropped before the fix
        const f32 expectedAngularVelocity = 2.5f;              // was silently dropped before the fix

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& rb = entity.AddComponent<Rigidbody2DComponent>();
            rb.Type = expectedType;
            rb.FixedRotation = expectedFixedRotation;
            rb.LinearVelocity = expectedLinearVelocity;
            rb.AngularVelocity = expectedAngularVelocity;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<Rigidbody2DComponent>());

        const auto& rb = restored.GetComponent<Rigidbody2DComponent>();
        EXPECT_EQ(rb.Type, expectedType);
        EXPECT_EQ(rb.FixedRotation, expectedFixedRotation);
        EXPECT_NEAR(rb.LinearVelocity.x, expectedLinearVelocity.x, kFloatEpsilon);
        EXPECT_NEAR(rb.LinearVelocity.y, expectedLinearVelocity.y, kFloatEpsilon);
        EXPECT_NEAR(rb.AngularVelocity, expectedAngularVelocity, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // Rigidbody3DComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, Rigidbody3DComponentSurvivesYAMLRoundTrip)
    {
        const auto expectedType = BodyType3D::Dynamic;
        const f32 expectedMass = 7.5f;
        const f32 expectedLinearDrag = 0.25f;
        const f32 expectedAngularDrag = 0.6f;
        const bool expectedDisableGravity = true;
        const bool expectedIsTrigger = true;
        // The following six were all silently dropped by the serializer before the fix.
        const u32 expectedLayerID = 3;
        const EActorAxis expectedLockedAxes = EActorAxis::Translation;
        const glm::vec3 expectedInitialLinearVelocity{ 1.5f, -2.0f, 0.5f };
        const glm::vec3 expectedInitialAngularVelocity{ 0.25f, 0.5f, -0.75f };
        const f32 expectedMaxLinearVelocity = 250.0f;
        const f32 expectedMaxAngularVelocity = 30.0f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& rb = entity.AddComponent<Rigidbody3DComponent>();
            rb.m_Type = expectedType;
            rb.m_Mass = expectedMass;
            rb.m_LinearDrag = expectedLinearDrag;
            rb.m_AngularDrag = expectedAngularDrag;
            rb.m_DisableGravity = expectedDisableGravity;
            rb.m_IsTrigger = expectedIsTrigger;
            rb.m_LayerID = expectedLayerID;
            rb.m_LockedAxes = expectedLockedAxes;
            rb.m_InitialLinearVelocity = expectedInitialLinearVelocity;
            rb.m_InitialAngularVelocity = expectedInitialAngularVelocity;
            rb.m_MaxLinearVelocity = expectedMaxLinearVelocity;
            rb.m_MaxAngularVelocity = expectedMaxAngularVelocity;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<Rigidbody3DComponent>());

        const auto& rb = restored.GetComponent<Rigidbody3DComponent>();
        EXPECT_EQ(rb.m_Type, expectedType);
        EXPECT_NEAR(rb.m_Mass, expectedMass, kFloatEpsilon);
        EXPECT_NEAR(rb.m_LinearDrag, expectedLinearDrag, kFloatEpsilon);
        EXPECT_NEAR(rb.m_AngularDrag, expectedAngularDrag, kFloatEpsilon);
        EXPECT_EQ(rb.m_DisableGravity, expectedDisableGravity);
        EXPECT_EQ(rb.m_IsTrigger, expectedIsTrigger);
        EXPECT_EQ(rb.m_LayerID, expectedLayerID);
        EXPECT_EQ(rb.m_LockedAxes, expectedLockedAxes);
        EXPECT_NEAR(rb.m_InitialLinearVelocity.x, expectedInitialLinearVelocity.x, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialLinearVelocity.y, expectedInitialLinearVelocity.y, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialLinearVelocity.z, expectedInitialLinearVelocity.z, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialAngularVelocity.x, expectedInitialAngularVelocity.x, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialAngularVelocity.y, expectedInitialAngularVelocity.y, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialAngularVelocity.z, expectedInitialAngularVelocity.z, kFloatEpsilon);
        EXPECT_NEAR(rb.m_MaxLinearVelocity, expectedMaxLinearVelocity, kFloatEpsilon);
        EXPECT_NEAR(rb.m_MaxAngularVelocity, expectedMaxAngularVelocity, kFloatEpsilon);
    }

    // A corrupted/hand-authored `Mass: 0` (or negative, or NaN) would otherwise
    // produce an infinite inverse mass in JoltBody::CreateBodySettings, NaN-
    // propagating across the whole solver island (issue #541).
    TEST(ComponentRoundTrip, Rigidbody3DComponentInvalidMassIsSanitizedOnLoad)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& rb = entity.AddComponent<Rigidbody3DComponent>();
            rb.m_Mass = 7.5f;
            rb.m_LinearDrag = 0.25f;
            rb.m_AngularDrag = 0.6f;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        yaml = std::regex_replace(yaml, std::regex(R"(Mass: [0-9.e+-]+)"), "Mass: .nan");
        yaml = std::regex_replace(yaml, std::regex(R"(LinearDrag: [0-9.e+-]+)"), "LinearDrag: -5");
        yaml = std::regex_replace(yaml, std::regex(R"(AngularDrag: [0-9.e+-]+)"), "AngularDrag: .inf");

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "Deserialize rejected the (structurally valid) NaN/Inf-injected scene.";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<Rigidbody3DComponent>());

        const auto& rb = restored.GetComponent<Rigidbody3DComponent>();
        EXPECT_TRUE(std::isfinite(rb.m_Mass));
        EXPECT_GT(rb.m_Mass, 0.0f) << "NaN mass must not become a zero/negative inverse-mass divisor";
        EXPECT_TRUE(std::isfinite(rb.m_LinearDrag));
        EXPECT_GE(rb.m_LinearDrag, 0.0f) << "Negative drag should fall back to a non-negative value";
        EXPECT_TRUE(std::isfinite(rb.m_AngularDrag));
        EXPECT_GE(rb.m_AngularDrag, 0.0f) << "Inf drag should be sanitized to a finite value";
    }

    // -------------------------------------------------------------------------
    // BoxCollider2DComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, BoxCollider2DComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec2 expectedOffset{ 0.4f, -0.8f };
        const glm::vec2 expectedSize{ 1.5f, 2.25f };
        const f32 expectedDensity = 2.5f;
        const f32 expectedFriction = 0.75f;
        const f32 expectedRestitution = 0.6f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& bc = entity.AddComponent<BoxCollider2DComponent>();
            bc.Offset = expectedOffset;
            bc.Size = expectedSize;
            bc.Density = expectedDensity;
            bc.Friction = expectedFriction;
            bc.Restitution = expectedRestitution;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<BoxCollider2DComponent>());

        const auto& bc = restored.GetComponent<BoxCollider2DComponent>();
        EXPECT_NEAR(bc.Offset.x, expectedOffset.x, kFloatEpsilon);
        EXPECT_NEAR(bc.Offset.y, expectedOffset.y, kFloatEpsilon);
        EXPECT_NEAR(bc.Size.x, expectedSize.x, kFloatEpsilon);
        EXPECT_NEAR(bc.Size.y, expectedSize.y, kFloatEpsilon);
        EXPECT_NEAR(bc.Density, expectedDensity, kFloatEpsilon);
        EXPECT_NEAR(bc.Friction, expectedFriction, kFloatEpsilon);
        EXPECT_NEAR(bc.Restitution, expectedRestitution, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // CircleCollider2DComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, CircleCollider2DComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec2 expectedOffset{ 0.2f, -0.15f };
        const f32 expectedRadius = 0.875f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& cc = entity.AddComponent<CircleCollider2DComponent>();
            cc.Offset = expectedOffset;
            cc.Radius = expectedRadius;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<CircleCollider2DComponent>());

        const auto& cc = restored.GetComponent<CircleCollider2DComponent>();
        EXPECT_NEAR(cc.Offset.x, expectedOffset.x, kFloatEpsilon);
        EXPECT_NEAR(cc.Offset.y, expectedOffset.y, kFloatEpsilon);
        EXPECT_NEAR(cc.Radius, expectedRadius, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // SphereCollider3DComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, SphereCollider3DComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedRadius = 1.625f;
        const glm::vec3 expectedOffset{ 0.5f, -0.25f, 0.125f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& sc = entity.AddComponent<SphereCollider3DComponent>();
            sc.m_Material.SetDensity(3300.0f); // non-default — guards the Density round-trip
            sc.m_Radius = expectedRadius;
            sc.m_Offset = expectedOffset;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<SphereCollider3DComponent>());

        const auto& sc = restored.GetComponent<SphereCollider3DComponent>();
        EXPECT_NEAR(sc.m_Radius, expectedRadius, kFloatEpsilon);
        EXPECT_NEAR(sc.m_Offset.x, expectedOffset.x, kFloatEpsilon);
        EXPECT_NEAR(sc.m_Offset.y, expectedOffset.y, kFloatEpsilon);
        EXPECT_NEAR(sc.m_Offset.z, expectedOffset.z, kFloatEpsilon);
        EXPECT_NEAR(sc.m_Material.GetDensity(), 3300.0f, kFloatEpsilon); // was silently dropped before the fix
    }

    // -------------------------------------------------------------------------
    // AudioListenerComponent — Active flag is the user-visible toggle.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, AudioListenerComponentSurvivesYAMLRoundTrip)
    {
        const bool expectedActive = false; // non-default

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& al = entity.AddComponent<AudioListenerComponent>();
            al.Active = expectedActive;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<AudioListenerComponent>());

        const auto& al = restored.GetComponent<AudioListenerComponent>();
        EXPECT_EQ(al.Active, expectedActive);
    }

    // -------------------------------------------------------------------------
    // BoxCollider3DComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, BoxCollider3DComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec3 expectedHalfExtents{ 1.25f, 0.75f, 2.5f };
        const glm::vec3 expectedOffset{ 0.1f, 0.2f, -0.3f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& bc = entity.AddComponent<BoxCollider3DComponent>();
            bc.m_Material.SetDensity(2500.0f); // non-default (default is 1000) — guards the Density round-trip
            bc.m_HalfExtents = expectedHalfExtents;
            bc.m_Offset = expectedOffset;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<BoxCollider3DComponent>());

        const auto& bc = restored.GetComponent<BoxCollider3DComponent>();
        EXPECT_NEAR(bc.m_HalfExtents.x, expectedHalfExtents.x, kFloatEpsilon);
        EXPECT_NEAR(bc.m_HalfExtents.y, expectedHalfExtents.y, kFloatEpsilon);
        EXPECT_NEAR(bc.m_HalfExtents.z, expectedHalfExtents.z, kFloatEpsilon);
        EXPECT_NEAR(bc.m_Offset.x, expectedOffset.x, kFloatEpsilon);
        EXPECT_NEAR(bc.m_Offset.y, expectedOffset.y, kFloatEpsilon);
        EXPECT_NEAR(bc.m_Offset.z, expectedOffset.z, kFloatEpsilon);
        EXPECT_NEAR(bc.m_Material.GetDensity(), 2500.0f, kFloatEpsilon); // was silently dropped before the fix
    }

    // -------------------------------------------------------------------------
    // MeshCollider3DComponent — exercises AssetHandle round-trip + scalar
    // fields. The ColliderMaterial sub-struct serialises through
    // StaticFriction/DynamicFriction/Restitution keys; covered separately
    // by the existing physics tests, not here.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, MeshCollider3DComponentSurvivesYAMLRoundTrip)
    {
        const AssetHandle expectedHandle{ 5544332211009988ULL };
        const glm::vec3 expectedOffset{ 0.3f, -0.7f, 1.4f };
        const glm::vec3 expectedScale{ 1.25f, 0.5f, 2.0f };
        const bool expectedUseComplexAsSimple = true; // non-default

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& mc = entity.AddComponent<MeshCollider3DComponent>();
            mc.m_Material.SetDensity(4200.0f); // non-default — guards the Density round-trip
            mc.m_ColliderAsset = expectedHandle;
            mc.m_Offset = expectedOffset;
            mc.m_Scale = expectedScale;
            mc.m_UseComplexAsSimple = expectedUseComplexAsSimple;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<MeshCollider3DComponent>());

        const auto& mc = restored.GetComponent<MeshCollider3DComponent>();
        EXPECT_EQ(static_cast<u64>(mc.m_ColliderAsset), static_cast<u64>(expectedHandle));
        EXPECT_NEAR(mc.m_Offset.x, expectedOffset.x, kFloatEpsilon);
        EXPECT_NEAR(mc.m_Offset.y, expectedOffset.y, kFloatEpsilon);
        EXPECT_NEAR(mc.m_Offset.z, expectedOffset.z, kFloatEpsilon);
        EXPECT_NEAR(mc.m_Scale.x, expectedScale.x, kFloatEpsilon);
        EXPECT_NEAR(mc.m_Scale.y, expectedScale.y, kFloatEpsilon);
        EXPECT_NEAR(mc.m_Scale.z, expectedScale.z, kFloatEpsilon);
        EXPECT_EQ(mc.m_UseComplexAsSimple, expectedUseComplexAsSimple);
        EXPECT_NEAR(mc.m_Material.GetDensity(), 4200.0f, kFloatEpsilon); // was silently dropped before the fix
    }

    // -------------------------------------------------------------------------
    // ConvexMeshCollider3DComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, ConvexMeshCollider3DComponentSurvivesYAMLRoundTrip)
    {
        const AssetHandle expectedHandle{ 9988776655443322ULL };
        const glm::vec3 expectedOffset{ -0.1f, 0.4f, 0.9f };
        const glm::vec3 expectedScale{ 2.0f, 1.5f, 0.5f };
        const f32 expectedConvexRadius = 0.125f;
        const u32 expectedMaxVertices = 128;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& cc = entity.AddComponent<ConvexMeshCollider3DComponent>();
            cc.m_Material.SetDensity(2200.0f); // non-default — guards the Density round-trip
            cc.m_ColliderAsset = expectedHandle;
            cc.m_Offset = expectedOffset;
            cc.m_Scale = expectedScale;
            cc.m_ConvexRadius = expectedConvexRadius;
            cc.m_MaxVertices = expectedMaxVertices;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<ConvexMeshCollider3DComponent>());

        const auto& cc = restored.GetComponent<ConvexMeshCollider3DComponent>();
        EXPECT_EQ(static_cast<u64>(cc.m_ColliderAsset), static_cast<u64>(expectedHandle));
        EXPECT_NEAR(cc.m_Offset.x, expectedOffset.x, kFloatEpsilon);
        EXPECT_NEAR(cc.m_Scale.y, expectedScale.y, kFloatEpsilon);
        EXPECT_NEAR(cc.m_ConvexRadius, expectedConvexRadius, kFloatEpsilon);
        EXPECT_EQ(cc.m_MaxVertices, expectedMaxVertices);
        EXPECT_NEAR(cc.m_Material.GetDensity(), 2200.0f, kFloatEpsilon); // was silently dropped before the fix
    }

    // -------------------------------------------------------------------------
    // TriangleMeshCollider3DComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, TriangleMeshCollider3DComponentSurvivesYAMLRoundTrip)
    {
        const AssetHandle expectedHandle{ 1122334455667788ULL };
        const glm::vec3 expectedOffset{ 0.2f, 0.4f, -0.6f };
        const glm::vec3 expectedScale{ 1.1f, 1.2f, 1.3f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& tc = entity.AddComponent<TriangleMeshCollider3DComponent>();
            tc.m_Material.SetDensity(5100.0f); // non-default — guards the Density round-trip
            tc.m_ColliderAsset = expectedHandle;
            tc.m_Offset = expectedOffset;
            tc.m_Scale = expectedScale;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<TriangleMeshCollider3DComponent>());

        const auto& tc = restored.GetComponent<TriangleMeshCollider3DComponent>();
        EXPECT_EQ(static_cast<u64>(tc.m_ColliderAsset), static_cast<u64>(expectedHandle));
        EXPECT_NEAR(tc.m_Offset.x, expectedOffset.x, kFloatEpsilon);
        EXPECT_NEAR(tc.m_Offset.y, expectedOffset.y, kFloatEpsilon);
        EXPECT_NEAR(tc.m_Scale.z, expectedScale.z, kFloatEpsilon);
        EXPECT_NEAR(tc.m_Material.GetDensity(), 5100.0f, kFloatEpsilon); // was silently dropped before the fix
    }

    // -------------------------------------------------------------------------
    // CapsuleCollider3DComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, CapsuleCollider3DComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedRadius = 0.625f;
        const f32 expectedHalfHeight = 1.875f;
        const glm::vec3 expectedOffset{ 0.1f, 0.5f, -0.25f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& cc = entity.AddComponent<CapsuleCollider3DComponent>();
            cc.m_Material.SetDensity(1750.0f); // non-default — guards the Density round-trip
            cc.m_Radius = expectedRadius;
            cc.m_HalfHeight = expectedHalfHeight;
            cc.m_Offset = expectedOffset;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<CapsuleCollider3DComponent>());

        const auto& cc = restored.GetComponent<CapsuleCollider3DComponent>();
        EXPECT_NEAR(cc.m_Radius, expectedRadius, kFloatEpsilon);
        EXPECT_NEAR(cc.m_HalfHeight, expectedHalfHeight, kFloatEpsilon);
        EXPECT_NEAR(cc.m_Offset.x, expectedOffset.x, kFloatEpsilon);
        EXPECT_NEAR(cc.m_Offset.y, expectedOffset.y, kFloatEpsilon);
        EXPECT_NEAR(cc.m_Offset.z, expectedOffset.z, kFloatEpsilon);
        EXPECT_NEAR(cc.m_Material.GetDensity(), 1750.0f, kFloatEpsilon); // was silently dropped before the fix
    }

    // -------------------------------------------------------------------------
    // CharacterController3DComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, CharacterController3DComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedSlope = 32.5f;
        const f32 expectedStep = 0.6f;
        const f32 expectedJump = 11.25f;
        const bool expectedDisableGravity = true;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& cc = entity.AddComponent<CharacterController3DComponent>();
            cc.m_SlopeLimitDeg = expectedSlope;
            cc.m_StepOffset = expectedStep;
            cc.m_JumpPower = expectedJump;
            cc.m_DisableGravity = expectedDisableGravity;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<CharacterController3DComponent>());

        const auto& cc = restored.GetComponent<CharacterController3DComponent>();
        EXPECT_NEAR(cc.m_SlopeLimitDeg, expectedSlope, kFloatEpsilon);
        EXPECT_NEAR(cc.m_StepOffset, expectedStep, kFloatEpsilon);
        EXPECT_NEAR(cc.m_JumpPower, expectedJump, kFloatEpsilon);
        EXPECT_EQ(cc.m_DisableGravity, expectedDisableGravity);
    }

    // -------------------------------------------------------------------------
    // PhysicsJoint3DComponent — every field set non-default (and inside the
    // serializer's sanitize/clamp ranges so it round-trips exactly), including
    // the UUID connected-entity reference and the local-space anchors/axis.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, PhysicsJoint3DComponentSurvivesYAMLRoundTrip)
    {
        const auto expectedType = JointType3D::Slider; // non-default (default Fixed)
        const UUID expectedConnected{ 0x1234'5678'9ABCULL };
        const glm::vec3 expectedAnchorA{ 1.5f, -2.25f, 0.75f };
        const glm::vec3 expectedAnchorB{ -0.5f, 3.25f, -1.125f };
        const glm::vec3 expectedAxis{ 0.0f, 0.0f, 1.0f };
        const f32 expectedMinDistance = 0.25f;
        const f32 expectedMaxDistance = 4.5f;
        const f32 expectedHingeMin = -45.0f; // within [-180, 0]
        const f32 expectedHingeMax = 90.0f;  // within [0, 180]
        const f32 expectedSliderMin = -2.5f;
        const f32 expectedSliderMax = 3.5f;
        const f32 expectedConeHalf = 60.0f; // within [0, 180]
        const f32 expectedBreakForce = 250.0f;
        const f32 expectedBreakTorque = 75.0f;
        const auto expectedHingeMotorMode = JointMotorMode::Position; // non-default (default Off)
        const f32 expectedHingeMotorVel = -150.0f;                    // signed target, within sanitize range
        const f32 expectedHingeMotorAngle = 35.0f;                    // within [-360, 360]
        const f32 expectedHingeMaxTorque = 42.0f;
        const f32 expectedHingeFriction = 6.5f;
        const auto expectedSliderMotorMode = JointMotorMode::Velocity;
        const f32 expectedSliderMotorVel = 3.25f;
        const f32 expectedSliderMotorPos = -1.75f; // within [-10000, 10000]
        const f32 expectedSliderMaxForce = 64.0f;
        const f32 expectedSliderFriction = 9.0f;
        const f32 expectedHingeSpringFreq = 1.5f; // > 0 → soft hinge limits
        const f32 expectedHingeSpringDamping = 0.7f;
        const f32 expectedSliderSpringFreq = 3.0f;
        const f32 expectedSliderSpringDamping = 1.1f; // overdamped is valid
        // Pulley + Gear/RackAndPinion (issue #308 item 4).
        const glm::vec3 expectedPulleyFixedA{ 2.5f, -1.0f, 0.5f };
        const glm::vec3 expectedPulleyFixedB{ -3.0f, 4.0f, -2.5f };
        const f32 expectedPulleyRatio = 2.5f;
        const f32 expectedPulleyMinLength = 0.75f; // within [-1, 1e9]
        const f32 expectedPulleyMaxLength = 12.0f;
        const glm::vec3 expectedConnectedAxis{ 0.25f, -0.5f, 0.75f }; // distinct components catch any axis-order bug
        const f32 expectedGearRatio = -1.5f;                          // signed: reversed coupling

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& j = entity.AddComponent<PhysicsJoint3DComponent>();
            j.m_Type = expectedType;
            j.m_ConnectedEntity = expectedConnected;
            j.m_LocalAnchorA = expectedAnchorA;
            j.m_LocalAnchorB = expectedAnchorB;
            j.m_Axis = expectedAxis;
            j.m_MinDistance = expectedMinDistance;
            j.m_MaxDistance = expectedMaxDistance;
            j.m_HingeMinAngleDeg = expectedHingeMin;
            j.m_HingeMaxAngleDeg = expectedHingeMax;
            j.m_SliderMinLimit = expectedSliderMin;
            j.m_SliderMaxLimit = expectedSliderMax;
            j.m_ConeHalfAngleDeg = expectedConeHalf;
            j.m_BreakForce = expectedBreakForce;
            j.m_BreakTorque = expectedBreakTorque;
            j.m_HingeMotorMode = expectedHingeMotorMode;
            j.m_HingeMotorTargetVelocityDeg = expectedHingeMotorVel;
            j.m_HingeMotorTargetAngleDeg = expectedHingeMotorAngle;
            j.m_HingeMaxMotorTorque = expectedHingeMaxTorque;
            j.m_HingeMaxFrictionTorque = expectedHingeFriction;
            j.m_SliderMotorMode = expectedSliderMotorMode;
            j.m_SliderMotorTargetVelocity = expectedSliderMotorVel;
            j.m_SliderMotorTargetPosition = expectedSliderMotorPos;
            j.m_SliderMaxMotorForce = expectedSliderMaxForce;
            j.m_SliderMaxFrictionForce = expectedSliderFriction;
            j.m_HingeLimitSpringFrequency = expectedHingeSpringFreq;
            j.m_HingeLimitSpringDamping = expectedHingeSpringDamping;
            j.m_SliderLimitSpringFrequency = expectedSliderSpringFreq;
            j.m_SliderLimitSpringDamping = expectedSliderSpringDamping;
            j.m_PulleyFixedPointA = expectedPulleyFixedA;
            j.m_PulleyFixedPointB = expectedPulleyFixedB;
            j.m_PulleyRatio = expectedPulleyRatio;
            j.m_PulleyMinLength = expectedPulleyMinLength;
            j.m_PulleyMaxLength = expectedPulleyMaxLength;
            j.m_ConnectedAxis = expectedConnectedAxis;
            j.m_GearRatio = expectedGearRatio;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<PhysicsJoint3DComponent>())
            << "PhysicsJoint3DComponent dropped during round-trip.";

        const auto& j = restored.GetComponent<PhysicsJoint3DComponent>();
        EXPECT_EQ(j.m_Type, expectedType);
        EXPECT_EQ(static_cast<u64>(j.m_ConnectedEntity), static_cast<u64>(expectedConnected));
        EXPECT_NEAR(j.m_LocalAnchorA.x, expectedAnchorA.x, kFloatEpsilon);
        EXPECT_NEAR(j.m_LocalAnchorA.y, expectedAnchorA.y, kFloatEpsilon);
        EXPECT_NEAR(j.m_LocalAnchorA.z, expectedAnchorA.z, kFloatEpsilon);
        EXPECT_NEAR(j.m_LocalAnchorB.x, expectedAnchorB.x, kFloatEpsilon);
        EXPECT_NEAR(j.m_LocalAnchorB.y, expectedAnchorB.y, kFloatEpsilon);
        EXPECT_NEAR(j.m_LocalAnchorB.z, expectedAnchorB.z, kFloatEpsilon);
        EXPECT_NEAR(j.m_Axis.z, expectedAxis.z, kFloatEpsilon);
        EXPECT_NEAR(j.m_MinDistance, expectedMinDistance, kFloatEpsilon);
        EXPECT_NEAR(j.m_MaxDistance, expectedMaxDistance, kFloatEpsilon);
        EXPECT_NEAR(j.m_HingeMinAngleDeg, expectedHingeMin, kFloatEpsilon);
        EXPECT_NEAR(j.m_HingeMaxAngleDeg, expectedHingeMax, kFloatEpsilon);
        EXPECT_NEAR(j.m_SliderMinLimit, expectedSliderMin, kFloatEpsilon);
        EXPECT_NEAR(j.m_SliderMaxLimit, expectedSliderMax, kFloatEpsilon);
        EXPECT_NEAR(j.m_ConeHalfAngleDeg, expectedConeHalf, kFloatEpsilon);
        EXPECT_NEAR(j.m_BreakForce, expectedBreakForce, kFloatEpsilon);
        EXPECT_NEAR(j.m_BreakTorque, expectedBreakTorque, kFloatEpsilon);
        EXPECT_EQ(j.m_HingeMotorMode, expectedHingeMotorMode);
        EXPECT_NEAR(j.m_HingeMotorTargetVelocityDeg, expectedHingeMotorVel, kFloatEpsilon);
        EXPECT_NEAR(j.m_HingeMotorTargetAngleDeg, expectedHingeMotorAngle, kFloatEpsilon);
        EXPECT_NEAR(j.m_HingeMaxMotorTorque, expectedHingeMaxTorque, kFloatEpsilon);
        EXPECT_NEAR(j.m_HingeMaxFrictionTorque, expectedHingeFriction, kFloatEpsilon);
        EXPECT_EQ(j.m_SliderMotorMode, expectedSliderMotorMode);
        EXPECT_NEAR(j.m_SliderMotorTargetVelocity, expectedSliderMotorVel, kFloatEpsilon);
        EXPECT_NEAR(j.m_SliderMotorTargetPosition, expectedSliderMotorPos, kFloatEpsilon);
        EXPECT_NEAR(j.m_SliderMaxMotorForce, expectedSliderMaxForce, kFloatEpsilon);
        EXPECT_NEAR(j.m_SliderMaxFrictionForce, expectedSliderFriction, kFloatEpsilon);
        EXPECT_NEAR(j.m_HingeLimitSpringFrequency, expectedHingeSpringFreq, kFloatEpsilon);
        EXPECT_NEAR(j.m_HingeLimitSpringDamping, expectedHingeSpringDamping, kFloatEpsilon);
        EXPECT_NEAR(j.m_SliderLimitSpringFrequency, expectedSliderSpringFreq, kFloatEpsilon);
        EXPECT_NEAR(j.m_SliderLimitSpringDamping, expectedSliderSpringDamping, kFloatEpsilon);
        EXPECT_NEAR(j.m_PulleyFixedPointA.x, expectedPulleyFixedA.x, kFloatEpsilon);
        EXPECT_NEAR(j.m_PulleyFixedPointA.y, expectedPulleyFixedA.y, kFloatEpsilon);
        EXPECT_NEAR(j.m_PulleyFixedPointA.z, expectedPulleyFixedA.z, kFloatEpsilon);
        EXPECT_NEAR(j.m_PulleyFixedPointB.x, expectedPulleyFixedB.x, kFloatEpsilon);
        EXPECT_NEAR(j.m_PulleyFixedPointB.y, expectedPulleyFixedB.y, kFloatEpsilon);
        EXPECT_NEAR(j.m_PulleyFixedPointB.z, expectedPulleyFixedB.z, kFloatEpsilon);
        EXPECT_NEAR(j.m_PulleyRatio, expectedPulleyRatio, kFloatEpsilon);
        EXPECT_NEAR(j.m_PulleyMinLength, expectedPulleyMinLength, kFloatEpsilon);
        EXPECT_NEAR(j.m_PulleyMaxLength, expectedPulleyMaxLength, kFloatEpsilon);
        EXPECT_NEAR(j.m_ConnectedAxis.x, expectedConnectedAxis.x, kFloatEpsilon);
        EXPECT_NEAR(j.m_ConnectedAxis.y, expectedConnectedAxis.y, kFloatEpsilon);
        EXPECT_NEAR(j.m_ConnectedAxis.z, expectedConnectedAxis.z, kFloatEpsilon);
        EXPECT_NEAR(j.m_GearRatio, expectedGearRatio, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // LightProbeComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, LightProbeComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedRadius = 7.5f;
        const f32 expectedIntensity = 1.75f;
        const bool expectedActive = false; // non-default

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& lp = entity.AddComponent<LightProbeComponent>();
            lp.m_InfluenceRadius = expectedRadius;
            lp.m_Intensity = expectedIntensity;
            lp.m_Active = expectedActive;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<LightProbeComponent>());

        const auto& lp = restored.GetComponent<LightProbeComponent>();
        EXPECT_NEAR(lp.m_InfluenceRadius, expectedRadius, kFloatEpsilon);
        EXPECT_NEAR(lp.m_Intensity, expectedIntensity, kFloatEpsilon);
        EXPECT_EQ(lp.m_Active, expectedActive);
    }

    // -------------------------------------------------------------------------
    // LightProbeVolumeComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, LightProbeVolumeComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec3 expectedMin{ -5.0f, -1.0f, -7.5f };
        const glm::vec3 expectedMax{ 5.0f, 3.0f, 7.5f };
        const f32 expectedSpacing = 2.5f;
        const f32 expectedIntensity = 1.5f;
        const bool expectedActive = false; // non-default
        // Realtime DDGI fields (issue #632) — all non-default
        const auto expectedMode = LightProbeVolumeComponent::Mode::Realtime;
        const i32 expectedRaysPerProbe = 1024;
        const f32 expectedHysteresis = 0.75f;
        const i32 expectedCaptureBudget = 8;
        const i32 expectedRelightBudget = 64;
        const f32 expectedSelfShadowBias = 0.5f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& lp = entity.AddComponent<LightProbeVolumeComponent>();
            lp.m_BoundsMin = expectedMin;
            lp.m_BoundsMax = expectedMax;
            lp.m_Spacing = expectedSpacing;
            lp.m_Intensity = expectedIntensity;
            lp.m_Active = expectedActive;
            lp.m_Mode = expectedMode;
            lp.m_RaysPerProbe = expectedRaysPerProbe;
            lp.m_Hysteresis = expectedHysteresis;
            lp.m_ProbeCaptureBudget = expectedCaptureBudget;
            lp.m_RelightBudget = expectedRelightBudget;
            lp.m_SelfShadowBias = expectedSelfShadowBias;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<LightProbeVolumeComponent>());

        const auto& lp = restored.GetComponent<LightProbeVolumeComponent>();
        EXPECT_NEAR(lp.m_BoundsMin.x, expectedMin.x, kFloatEpsilon);
        EXPECT_NEAR(lp.m_BoundsMin.y, expectedMin.y, kFloatEpsilon);
        EXPECT_NEAR(lp.m_BoundsMin.z, expectedMin.z, kFloatEpsilon);
        EXPECT_NEAR(lp.m_BoundsMax.x, expectedMax.x, kFloatEpsilon);
        EXPECT_NEAR(lp.m_BoundsMax.y, expectedMax.y, kFloatEpsilon);
        EXPECT_NEAR(lp.m_BoundsMax.z, expectedMax.z, kFloatEpsilon);
        EXPECT_NEAR(lp.m_Spacing, expectedSpacing, kFloatEpsilon);
        EXPECT_NEAR(lp.m_Intensity, expectedIntensity, kFloatEpsilon);
        EXPECT_EQ(lp.m_Active, expectedActive);
        EXPECT_EQ(lp.m_Mode, expectedMode);
        EXPECT_EQ(lp.m_RaysPerProbe, expectedRaysPerProbe);
        EXPECT_NEAR(lp.m_Hysteresis, expectedHysteresis, kFloatEpsilon);
        EXPECT_EQ(lp.m_ProbeCaptureBudget, expectedCaptureBudget);
        EXPECT_EQ(lp.m_RelightBudget, expectedRelightBudget);
        EXPECT_NEAR(lp.m_SelfShadowBias, expectedSelfShadowBias, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // ReflectionProbeComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, ReflectionProbeComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedRadius = 8.25f;
        const f32 expectedBlend = 0.75f;
        const u32 expectedResolution = 512u;
        const f32 expectedIntensity = 1.4f;
        const bool expectedActive = false; // non-default

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& rp = entity.AddComponent<ReflectionProbeComponent>();
            rp.m_InfluenceRadius = expectedRadius;
            rp.m_BlendDistance = expectedBlend;
            rp.m_Resolution = expectedResolution;
            rp.m_Intensity = expectedIntensity;
            rp.m_Active = expectedActive;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<ReflectionProbeComponent>());

        const auto& rp = restored.GetComponent<ReflectionProbeComponent>();
        EXPECT_NEAR(rp.m_InfluenceRadius, expectedRadius, kFloatEpsilon);
        EXPECT_NEAR(rp.m_BlendDistance, expectedBlend, kFloatEpsilon);
        EXPECT_EQ(rp.m_Resolution, expectedResolution);
        EXPECT_NEAR(rp.m_Intensity, expectedIntensity, kFloatEpsilon);
        EXPECT_EQ(rp.m_Active, expectedActive);
        // m_BakedEnvironment is runtime — must come back null with rebake pending.
        EXPECT_FALSE(static_cast<bool>(rp.m_BakedEnvironment));
        EXPECT_TRUE(rp.m_NeedsBake);
    }

    // -------------------------------------------------------------------------
    // EnvironmentMapComponent — exercises the scalar/bool fields (Rotation,
    // Exposure, BlurAmount, EnableSkybox, EnableIBL, Tint). The
    // m_FilePath / m_EnvironmentMapAsset fields are paths/handles that
    // production code resolves through the asset manager; here we only
    // verify the scalar settings survive — those are the user-tweakable
    // values in the editor's skybox panel.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, EnvironmentMapComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedRotation = 73.5f;
        const f32 expectedExposure = 2.5f;
        const f32 expectedBlur = 0.4f;
        const bool expectedEnableSkybox = false; // non-default
        const bool expectedEnableIBL = false;    // non-default
        const f32 expectedIBLIntensity = 1.75f;
        // Flip to non-default so a missing serializer entry would surface as
        // the field reverting to false on reload.
        const bool expectedUseSH = true;
        const glm::vec3 expectedTint{ 0.8f, 0.9f, 1.0f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& em = entity.AddComponent<EnvironmentMapComponent>();
            em.m_Rotation = expectedRotation;
            em.m_Exposure = expectedExposure;
            em.m_BlurAmount = expectedBlur;
            em.m_EnableSkybox = expectedEnableSkybox;
            em.m_EnableIBL = expectedEnableIBL;
            em.m_IBLIntensity = expectedIBLIntensity;
            em.m_UseSphericalHarmonics = expectedUseSH;
            em.m_Tint = expectedTint;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<EnvironmentMapComponent>());

        const auto& em = restored.GetComponent<EnvironmentMapComponent>();
        EXPECT_NEAR(em.m_Rotation, expectedRotation, kFloatEpsilon);
        EXPECT_NEAR(em.m_Exposure, expectedExposure, kFloatEpsilon);
        EXPECT_NEAR(em.m_BlurAmount, expectedBlur, kFloatEpsilon);
        EXPECT_EQ(em.m_EnableSkybox, expectedEnableSkybox);
        EXPECT_EQ(em.m_EnableIBL, expectedEnableIBL);
        EXPECT_NEAR(em.m_IBLIntensity, expectedIBLIntensity, kFloatEpsilon);
        EXPECT_EQ(em.m_UseSphericalHarmonics, expectedUseSH);
        EXPECT_NEAR(em.m_Tint.r, expectedTint.r, kFloatEpsilon);
        EXPECT_NEAR(em.m_Tint.g, expectedTint.g, kFloatEpsilon);
        EXPECT_NEAR(em.m_Tint.b, expectedTint.b, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // UIRectTransformComponent — UI-equivalent of TransformComponent.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIRectTransformComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec2 expectedAnchorMin{ 0.0f, 0.25f };
        const glm::vec2 expectedAnchorMax{ 1.0f, 0.75f };
        const glm::vec2 expectedAnchoredPos{ 12.5f, -8.0f };
        const glm::vec2 expectedSizeDelta{ 320.0f, 80.0f };
        const glm::vec2 expectedPivot{ 0.0f, 1.0f };
        const f32 expectedRotation = 17.5f;
        const glm::vec2 expectedScale{ 1.25f, 0.75f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& rt = entity.AddComponent<UIRectTransformComponent>();
            rt.m_AnchorMin = expectedAnchorMin;
            rt.m_AnchorMax = expectedAnchorMax;
            rt.m_AnchoredPosition = expectedAnchoredPos;
            rt.m_SizeDelta = expectedSizeDelta;
            rt.m_Pivot = expectedPivot;
            rt.m_Rotation = expectedRotation;
            rt.m_Scale = expectedScale;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIRectTransformComponent>());

        const auto& rt = restored.GetComponent<UIRectTransformComponent>();
        EXPECT_NEAR(rt.m_AnchorMin.x, expectedAnchorMin.x, kFloatEpsilon);
        EXPECT_NEAR(rt.m_AnchorMin.y, expectedAnchorMin.y, kFloatEpsilon);
        EXPECT_NEAR(rt.m_AnchorMax.x, expectedAnchorMax.x, kFloatEpsilon);
        EXPECT_NEAR(rt.m_AnchorMax.y, expectedAnchorMax.y, kFloatEpsilon);
        EXPECT_NEAR(rt.m_AnchoredPosition.x, expectedAnchoredPos.x, kFloatEpsilon);
        EXPECT_NEAR(rt.m_AnchoredPosition.y, expectedAnchoredPos.y, kFloatEpsilon);
        EXPECT_NEAR(rt.m_SizeDelta.x, expectedSizeDelta.x, kFloatEpsilon);
        EXPECT_NEAR(rt.m_SizeDelta.y, expectedSizeDelta.y, kFloatEpsilon);
        EXPECT_NEAR(rt.m_Pivot.x, expectedPivot.x, kFloatEpsilon);
        EXPECT_NEAR(rt.m_Pivot.y, expectedPivot.y, kFloatEpsilon);
        EXPECT_NEAR(rt.m_Rotation, expectedRotation, kFloatEpsilon);
        EXPECT_NEAR(rt.m_Scale.x, expectedScale.x, kFloatEpsilon);
        EXPECT_NEAR(rt.m_Scale.y, expectedScale.y, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // UIPanelComponent — background colour (texture handle skipped to
    // avoid the Ref<Texture2D> initialisation that's covered by the
    // MaterialComponent limitation).
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIPanelComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec4 expectedColor{ 0.15f, 0.35f, 0.55f, 0.85f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& p = entity.AddComponent<UIPanelComponent>();
            p.m_BackgroundColor = expectedColor;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIPanelComponent>());

        const auto& p = restored.GetComponent<UIPanelComponent>();
        EXPECT_NEAR(p.m_BackgroundColor.r, expectedColor.r, kFloatEpsilon);
        EXPECT_NEAR(p.m_BackgroundColor.g, expectedColor.g, kFloatEpsilon);
        EXPECT_NEAR(p.m_BackgroundColor.b, expectedColor.b, kFloatEpsilon);
        EXPECT_NEAR(p.m_BackgroundColor.a, expectedColor.a, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // UITextComponent — text + size + colour + kerning + line spacing.
    // Default-constructed `m_FontAsset` calls Font::GetDefault(), which
    // searches well-known paths and returns a loaded fallback font (no
    // renderer init required).
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UITextComponentSurvivesYAMLRoundTrip)
    {
        const std::string expectedText = "Round-trip UI text — αβγ ✓";
        const f32 expectedFontSize = 36.0f;
        const glm::vec4 expectedColor{ 0.95f, 0.7f, 0.2f, 1.0f };
        const f32 expectedKerning = 0.25f;
        const f32 expectedLineSpacing = 0.125f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& t = entity.AddComponent<UITextComponent>();
            t.m_Text = expectedText;
            t.m_FontSize = expectedFontSize;
            t.m_Color = expectedColor;
            t.m_Kerning = expectedKerning;
            t.m_LineSpacing = expectedLineSpacing;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UITextComponent>());

        const auto& t = restored.GetComponent<UITextComponent>();
        EXPECT_EQ(t.m_Text, expectedText);
        EXPECT_NEAR(t.m_FontSize, expectedFontSize, kFloatEpsilon);
        EXPECT_NEAR(t.m_Color.r, expectedColor.r, kFloatEpsilon);
        EXPECT_NEAR(t.m_Color.a, expectedColor.a, kFloatEpsilon);
        EXPECT_NEAR(t.m_Kerning, expectedKerning, kFloatEpsilon);
        EXPECT_NEAR(t.m_LineSpacing, expectedLineSpacing, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // UIButtonComponent — 4-state colour palette + Interactable. Auto-generated
    // via the OLO_SERIALIZE(Skip) slice (#451): the runtime-only m_State is marked
    // Skip, so it must round-trip every authored field but NOT persist m_State
    // (which stays at its constructor default on load even when set before save).
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIButtonComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec4 normal{ 0.3f, 0.5f, 0.7f, 1.0f };
        const glm::vec4 hover{ 0.4f, 0.6f, 0.8f, 1.0f };
        const glm::vec4 pressed{ 0.2f, 0.4f, 0.6f, 1.0f };
        const glm::vec4 disabled{ 0.1f, 0.1f, 0.1f, 0.5f };
        const bool expectedInteractable = false;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& b = entity.AddComponent<UIButtonComponent>();
            b.m_NormalColor = normal;
            b.m_HoveredColor = hover;
            b.m_PressedColor = pressed;
            b.m_DisabledColor = disabled;
            b.m_Interactable = expectedInteractable;
            // Runtime-only field set to a non-default value — OLO_SERIALIZE(Skip)
            // must keep it OUT of the YAML so it reloads at the ctor default.
            b.m_State = UIButtonState::Pressed;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        // The skipped runtime field must never appear in the serialized scene.
        EXPECT_EQ(yaml.find("State:"), std::string::npos)
            << "UIButtonComponent::m_State (OLO_SERIALIZE(Skip)) leaked into scene YAML.";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIButtonComponent>());

        const auto& b = restored.GetComponent<UIButtonComponent>();
        EXPECT_NEAR(b.m_NormalColor.r, normal.r, kFloatEpsilon);
        EXPECT_NEAR(b.m_HoveredColor.g, hover.g, kFloatEpsilon);
        EXPECT_NEAR(b.m_PressedColor.b, pressed.b, kFloatEpsilon);
        EXPECT_NEAR(b.m_DisabledColor.a, disabled.a, kFloatEpsilon);
        EXPECT_EQ(b.m_Interactable, expectedInteractable);
        // Skipped: not serialized, so it comes back as the constructor default.
        EXPECT_EQ(b.m_State, UIButtonState::Normal)
            << "m_State was persisted despite OLO_SERIALIZE(Skip).";
    }

    // -------------------------------------------------------------------------
    // UISliderComponent — auto-generated via the OLO_SERIALIZE(Skip) slice (#451).
    // Covers the Direction enum round-trip and asserts the runtime-only m_IsDragging
    // (marked Skip) is not persisted.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UISliderComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedValue = 0.625f;
        const f32 expectedMin = 0.0f;
        const f32 expectedMax = 100.0f;
        const auto expectedDirection = UISliderDirection::RightToLeft; // non-default
        const bool expectedInteractable = false;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& s = entity.AddComponent<UISliderComponent>();
            s.m_Value = expectedValue;
            s.m_MinValue = expectedMin;
            s.m_MaxValue = expectedMax;
            s.m_Direction = expectedDirection;
            s.m_Interactable = expectedInteractable;
            // Runtime-only drag flag set true — OLO_SERIALIZE(Skip) must drop it.
            s.m_IsDragging = true;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        EXPECT_EQ(yaml.find("IsDragging:"), std::string::npos)
            << "UISliderComponent::m_IsDragging (OLO_SERIALIZE(Skip)) leaked into scene YAML.";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UISliderComponent>());

        const auto& s = restored.GetComponent<UISliderComponent>();
        EXPECT_NEAR(s.m_Value, expectedValue, kFloatEpsilon);
        EXPECT_NEAR(s.m_MinValue, expectedMin, kFloatEpsilon);
        EXPECT_NEAR(s.m_MaxValue, expectedMax, kFloatEpsilon);
        EXPECT_EQ(s.m_Direction, expectedDirection);
        EXPECT_EQ(s.m_Interactable, expectedInteractable);
        // Skipped: reloads at the constructor default (false).
        EXPECT_FALSE(s.m_IsDragging)
            << "m_IsDragging was persisted despite OLO_SERIALIZE(Skip).";
    }

    // -------------------------------------------------------------------------
    // UICheckboxComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UICheckboxComponentSurvivesYAMLRoundTrip)
    {
        const bool expectedChecked = true;
        const bool expectedInteractable = false;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& c = entity.AddComponent<UICheckboxComponent>();
            c.m_IsChecked = expectedChecked;
            c.m_Interactable = expectedInteractable;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UICheckboxComponent>());

        const auto& c = restored.GetComponent<UICheckboxComponent>();
        EXPECT_EQ(c.m_IsChecked, expectedChecked);
        EXPECT_EQ(c.m_Interactable, expectedInteractable);
    }

    // -------------------------------------------------------------------------
    // UIProgressBarComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIProgressBarComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedValue = 42.5f;
        const f32 expectedMin = 0.0f;
        const f32 expectedMax = 100.0f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& p = entity.AddComponent<UIProgressBarComponent>();
            p.m_Value = expectedValue;
            p.m_MinValue = expectedMin;
            p.m_MaxValue = expectedMax;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIProgressBarComponent>());

        const auto& p = restored.GetComponent<UIProgressBarComponent>();
        EXPECT_NEAR(p.m_Value, expectedValue, kFloatEpsilon);
        EXPECT_NEAR(p.m_MinValue, expectedMin, kFloatEpsilon);
        EXPECT_NEAR(p.m_MaxValue, expectedMax, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // UIInputFieldComponent — text + placeholder + font size + interactable.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIInputFieldComponentSurvivesYAMLRoundTrip)
    {
        const std::string expectedText = "User typed this";
        const std::string expectedPlaceholder = "Type something…";
        const f32 expectedFontSize = 18.0f;
        const bool expectedInteractable = false;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& f = entity.AddComponent<UIInputFieldComponent>();
            f.m_Text = expectedText;
            f.m_Placeholder = expectedPlaceholder;
            f.m_FontSize = expectedFontSize;
            f.m_Interactable = expectedInteractable;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIInputFieldComponent>());

        const auto& f = restored.GetComponent<UIInputFieldComponent>();
        EXPECT_EQ(f.m_Text, expectedText);
        EXPECT_EQ(f.m_Placeholder, expectedPlaceholder);
        EXPECT_NEAR(f.m_FontSize, expectedFontSize, kFloatEpsilon);
        EXPECT_EQ(f.m_Interactable, expectedInteractable);
    }

    // -------------------------------------------------------------------------
    // UIScrollViewComponent — scroll position + content size + speed.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIScrollViewComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec2 expectedScrollPos{ 25.0f, 100.0f };
        const glm::vec2 expectedContentSize{ 500.0f, 1200.0f };
        const f32 expectedScrollSpeed = 35.0f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& s = entity.AddComponent<UIScrollViewComponent>();
            s.m_ScrollPosition = expectedScrollPos;
            s.m_ContentSize = expectedContentSize;
            s.m_ScrollSpeed = expectedScrollSpeed;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIScrollViewComponent>());

        const auto& s = restored.GetComponent<UIScrollViewComponent>();
        EXPECT_NEAR(s.m_ScrollPosition.x, expectedScrollPos.x, kFloatEpsilon);
        EXPECT_NEAR(s.m_ScrollPosition.y, expectedScrollPos.y, kFloatEpsilon);
        EXPECT_NEAR(s.m_ContentSize.x, expectedContentSize.x, kFloatEpsilon);
        EXPECT_NEAR(s.m_ContentSize.y, expectedContentSize.y, kFloatEpsilon);
        EXPECT_NEAR(s.m_ScrollSpeed, expectedScrollSpeed, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // UICanvasComponent — sort order + reference resolution.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UICanvasComponentSurvivesYAMLRoundTrip)
    {
        // RenderMode / ScaleMode are u8-backed enums — they exercise the
        // OloHeaderTool-generated enum serializer path (issue #451 enum slice):
        // both round-trip as ints through Scene{Serialize,Deserialize}Components
        // .Generated.inl, cast back via decltype. Set them non-default so a wrong
        // key / lost cast / dropped field surfaces here.
        const auto expectedRenderMode = UICanvasRenderMode::WorldSpace;        // default ScreenSpaceOverlay
        const auto expectedScaleMode = UICanvasScaleMode::ScaleWithScreenSize; // default ConstantPixelSize
        const i32 expectedSortOrder = 42;
        const glm::vec2 expectedReferenceResolution{ 1280.0f, 720.0f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& c = entity.AddComponent<UICanvasComponent>();
            c.m_RenderMode = expectedRenderMode;
            c.m_ScaleMode = expectedScaleMode;
            c.m_SortOrder = expectedSortOrder;
            c.m_ReferenceResolution = expectedReferenceResolution;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UICanvasComponent>());

        const auto& c = restored.GetComponent<UICanvasComponent>();
        EXPECT_EQ(c.m_RenderMode, expectedRenderMode);
        EXPECT_EQ(c.m_ScaleMode, expectedScaleMode);
        EXPECT_EQ(c.m_SortOrder, expectedSortOrder);
        EXPECT_NEAR(c.m_ReferenceResolution.x, expectedReferenceResolution.x, kFloatEpsilon);
        EXPECT_NEAR(c.m_ReferenceResolution.y, expectedReferenceResolution.y, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // UIGridLayoutComponent — cell + spacing + constraint count.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIGridLayoutComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec2 expectedCellSize{ 75.0f, 50.0f };
        const glm::vec2 expectedSpacing{ 10.0f, 8.0f };
        const i32 expectedConstraintCount = 4;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& g = entity.AddComponent<UIGridLayoutComponent>();
            g.m_CellSize = expectedCellSize;
            g.m_Spacing = expectedSpacing;
            g.m_ConstraintCount = expectedConstraintCount;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIGridLayoutComponent>());

        const auto& g = restored.GetComponent<UIGridLayoutComponent>();
        EXPECT_NEAR(g.m_CellSize.x, expectedCellSize.x, kFloatEpsilon);
        EXPECT_NEAR(g.m_CellSize.y, expectedCellSize.y, kFloatEpsilon);
        EXPECT_NEAR(g.m_Spacing.x, expectedSpacing.x, kFloatEpsilon);
        EXPECT_NEAR(g.m_Spacing.y, expectedSpacing.y, kFloatEpsilon);
        EXPECT_EQ(g.m_ConstraintCount, expectedConstraintCount);
    }

    // -------------------------------------------------------------------------
    // UIDropdownComponent — selected index + interactable.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIDropdownComponentSurvivesYAMLRoundTrip)
    {
        const i32 expectedSelectedIndex = 3;
        const bool expectedInteractable = false;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& d = entity.AddComponent<UIDropdownComponent>();
            d.m_SelectedIndex = expectedSelectedIndex;
            d.m_Interactable = expectedInteractable;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIDropdownComponent>());

        const auto& d = restored.GetComponent<UIDropdownComponent>();
        EXPECT_EQ(d.m_SelectedIndex, expectedSelectedIndex);
        EXPECT_EQ(d.m_Interactable, expectedInteractable);
    }

    // -------------------------------------------------------------------------
    // UIToggleComponent — IsOn + Interactable.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIToggleComponentSurvivesYAMLRoundTrip)
    {
        const bool expectedIsOn = true;
        const bool expectedInteractable = false;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& t = entity.AddComponent<UIToggleComponent>();
            t.m_IsOn = expectedIsOn;
            t.m_Interactable = expectedInteractable;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIToggleComponent>());

        const auto& t = restored.GetComponent<UIToggleComponent>();
        EXPECT_EQ(t.m_IsOn, expectedIsOn);
        EXPECT_EQ(t.m_Interactable, expectedInteractable);
    }

    // -------------------------------------------------------------------------
    // UIWorldAnchorComponent — UUID target + world-offset. As of issue #451 this
    // component is fully AUTO-GENERATED by OloHeaderTool (its only non-primitive
    // field is the UUID m_TargetEntity, now a SceneSerType), so this round-trip is
    // the end-to-end guard that the generated AssetHandle/UUID serialize/deserialize
    // blocks (static_cast<u64> on write, .as<u64> + implicit UUID(u64) on read)
    // preserve a non-trivial handle value — not just the previous hand-written path.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UIWorldAnchorComponentSurvivesYAMLRoundTrip)
    {
        const UUID expectedTarget{ 5500006677889900ULL };
        const glm::vec3 expectedOffset{ 0.5f, 3.0f, -1.25f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& a = entity.AddComponent<UIWorldAnchorComponent>();
            a.m_TargetEntity = expectedTarget;
            a.m_WorldOffset = expectedOffset;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UIWorldAnchorComponent>());

        const auto& a = restored.GetComponent<UIWorldAnchorComponent>();
        EXPECT_EQ(static_cast<u64>(a.m_TargetEntity), static_cast<u64>(expectedTarget));
        EXPECT_NEAR(a.m_WorldOffset.x, expectedOffset.x, kFloatEpsilon);
        EXPECT_NEAR(a.m_WorldOffset.y, expectedOffset.y, kFloatEpsilon);
        EXPECT_NEAR(a.m_WorldOffset.z, expectedOffset.z, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // StreamingVolumeComponent — RegionAssetHandle + LoadRadius/UnloadRadius.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, StreamingVolumeComponentSurvivesYAMLRoundTrip)
    {
        const AssetHandle expectedRegion{ 9999111122223333ULL };
        const f32 expectedLoad = 150.0f;
        const f32 expectedUnload = 200.0f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& s = entity.AddComponent<StreamingVolumeComponent>();
            s.RegionAssetHandle = expectedRegion;
            s.LoadRadius = expectedLoad;
            s.UnloadRadius = expectedUnload;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<StreamingVolumeComponent>());

        const auto& s = restored.GetComponent<StreamingVolumeComponent>();
        EXPECT_EQ(static_cast<u64>(s.RegionAssetHandle), static_cast<u64>(expectedRegion));
        EXPECT_NEAR(s.LoadRadius, expectedLoad, kFloatEpsilon);
        EXPECT_NEAR(s.UnloadRadius, expectedUnload, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // BehaviorTreeComponent — AssetHandle only (blackboard is a runtime
    // struct without a simple round-trippable scalar set).
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, BehaviorTreeComponentSurvivesYAMLRoundTrip)
    {
        const AssetHandle expectedAsset{ 1357913579135791ULL };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& b = entity.AddComponent<BehaviorTreeComponent>();
            b.BehaviorTreeAssetHandle = expectedAsset;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<BehaviorTreeComponent>());

        const auto& b = restored.GetComponent<BehaviorTreeComponent>();
        EXPECT_EQ(static_cast<u64>(b.BehaviorTreeAssetHandle),
                  static_cast<u64>(expectedAsset));
    }

    // -------------------------------------------------------------------------
    // StateMachineComponent — AssetHandle only.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, StateMachineComponentSurvivesYAMLRoundTrip)
    {
        const AssetHandle expectedAsset{ 2468024680246802ULL };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& sm = entity.AddComponent<StateMachineComponent>();
            sm.StateMachineAssetHandle = expectedAsset;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<StateMachineComponent>());

        const auto& sm = restored.GetComponent<StateMachineComponent>();
        EXPECT_EQ(static_cast<u64>(sm.StateMachineAssetHandle),
                  static_cast<u64>(expectedAsset));
    }

    // -------------------------------------------------------------------------
    // InventoryComponent — capacity + currency. The Items vector is
    // covered by the SaveGame round-trip tests under Functional/SaveGame
    // and the InventoryTest unit test; here we cover the lightweight
    // header fields the serializer writes.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, InventoryComponentSurvivesYAMLRoundTrip)
    {
        const u32 expectedCapacity = 64;
        const i32 expectedCurrency = 1337;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& inv = entity.AddComponent<InventoryComponent>();
            // Inventory's default constructor took capacity=40; we
            // rebuild with the expected capacity. Assigning to the
            // PlayerInventory directly via a new Inventory(capacity)
            // would require knowing Inventory's constructors — instead
            // we use Reset/Resize-equivalent semantics if the
            // serializer reads back Capacity correctly.
            inv.PlayerInventory = Inventory{ expectedCapacity };
            inv.Currency = expectedCurrency;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<InventoryComponent>());

        const auto& inv = restored.GetComponent<InventoryComponent>();
        EXPECT_EQ(inv.PlayerInventory.GetCapacity(), expectedCapacity);
        EXPECT_EQ(inv.Currency, expectedCurrency);
    }

    // -------------------------------------------------------------------------
    // ItemPickupComponent — pickup parameters + item-instance scalars.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, ItemPickupComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedRadius = 3.5f;
        const bool expectedAutoPickup = true;
        const f32 expectedDespawnTimer = 45.0f;
        const std::string expectedDefId = "health_potion";
        const u32 expectedStackCount = 5;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& pc = entity.AddComponent<ItemPickupComponent>();
            pc.PickupRadius = expectedRadius;
            pc.AutoPickup = expectedAutoPickup;
            pc.DespawnTimer = expectedDespawnTimer;
            pc.Item.ItemDefinitionID = expectedDefId;
            pc.Item.StackCount = expectedStackCount;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<ItemPickupComponent>());

        const auto& pc = restored.GetComponent<ItemPickupComponent>();
        EXPECT_NEAR(pc.PickupRadius, expectedRadius, kFloatEpsilon);
        EXPECT_EQ(pc.AutoPickup, expectedAutoPickup);
        EXPECT_NEAR(pc.DespawnTimer, expectedDespawnTimer, kFloatEpsilon);
        EXPECT_EQ(pc.Item.ItemDefinitionID, expectedDefId);
        EXPECT_EQ(pc.Item.StackCount, expectedStackCount);
    }

    // -------------------------------------------------------------------------
    // ItemContainerComponent — shop / loot-table flags.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, ItemContainerComponentSurvivesYAMLRoundTrip)
    {
        const u32 expectedCapacity = 12;
        const bool expectedIsShop = true;
        const std::string expectedLootTable = "tavern_chest_tier3";
        const bool expectedHasBeenLooted = false;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& cc = entity.AddComponent<ItemContainerComponent>();
            cc.Contents = Inventory{ expectedCapacity };
            cc.IsShop = expectedIsShop;
            cc.LootTableID = expectedLootTable;
            cc.HasBeenLooted = expectedHasBeenLooted;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<ItemContainerComponent>());

        const auto& cc = restored.GetComponent<ItemContainerComponent>();
        EXPECT_EQ(cc.Contents.GetCapacity(), expectedCapacity);
        EXPECT_EQ(cc.IsShop, expectedIsShop);
        EXPECT_EQ(cc.LootTableID, expectedLootTable);
        EXPECT_EQ(cc.HasBeenLooted, expectedHasBeenLooted);
    }

    // -------------------------------------------------------------------------
    // QuestGiverComponent — string vectors + marker icon.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, QuestGiverComponentSurvivesYAMLRoundTrip)
    {
        const std::vector<std::string> expectedOffered = { "herb_gathering", "wolf_hunt" };
        const std::vector<std::string> expectedTurnIn = { "the_crossroads" };
        const std::string expectedMarker = "?";

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& qg = entity.AddComponent<QuestGiverComponent>();
            qg.OfferedQuestIDs = expectedOffered;
            qg.TurnInQuestIDs = expectedTurnIn;
            qg.QuestMarkerIcon = expectedMarker;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<QuestGiverComponent>());

        const auto& qg = restored.GetComponent<QuestGiverComponent>();
        EXPECT_EQ(qg.OfferedQuestIDs, expectedOffered);
        EXPECT_EQ(qg.TurnInQuestIDs, expectedTurnIn);
        EXPECT_EQ(qg.QuestMarkerIcon, expectedMarker);
    }

    // -------------------------------------------------------------------------
    // PrefabComponent — exercises the hand-written serializer path (issue #444
    // hot/cold split moved the three override-tracking sets behind a private,
    // lazily-allocated PrefabOverrideSets pointer, so the component is no
    // longer codegen-trivial). Sets are populated with MULTIPLE entries each
    // (rather than a single one) so a bug that only appends/reads the first
    // inserted element would be caught; the hand-written serializer still
    // sorts before emit, so this also exercises that insertion order doesn't
    // matter for round-trip equality.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, PrefabComponentSurvivesYAMLRoundTrip)
    {
        const UUID expectedPrefabID{ 0x1234ULL };
        const UUID expectedPrefabEntityID{ 0x5678ULL };
        const std::unordered_set<std::string> expectedOverridden = { "TransformComponent", "SpriteRendererComponent" };
        const std::unordered_set<std::string> expectedAdded = { "ScriptComponent" };
        const std::unordered_set<std::string> expectedRemoved = { "CameraComponent", "AudioSourceComponent" };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& pc = entity.AddComponent<PrefabComponent>();
            pc.m_PrefabID = expectedPrefabID;
            pc.m_PrefabEntityID = expectedPrefabEntityID;
            pc.SetOverriddenComponents(expectedOverridden);
            pc.SetAddedComponents(expectedAdded);
            pc.SetRemovedComponents(expectedRemoved);
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<PrefabComponent>());

        const auto& pc = restored.GetComponent<PrefabComponent>();
        EXPECT_EQ(static_cast<u64>(pc.m_PrefabID), static_cast<u64>(expectedPrefabID));
        EXPECT_EQ(static_cast<u64>(pc.m_PrefabEntityID), static_cast<u64>(expectedPrefabEntityID));
        EXPECT_EQ(pc.GetOverriddenComponents(), expectedOverridden);
        EXPECT_EQ(pc.GetAddedComponents(), expectedAdded);
        EXPECT_EQ(pc.GetRemovedComponents(), expectedRemoved);
    }

    // -------------------------------------------------------------------------
    // InstancePortalComponent — exercises the GENERATED small-int (u8) serializer
    // path (#451 small-int slice). InstanceType is a u8; the codegen widens it to
    // u32 on emit so yaml-cpp writes the number, not a raw char. Use a value > 127
    // so a signed-char misread would corrupt it.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, InstancePortalComponentSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& ipc = entity.AddComponent<InstancePortalComponent>();
            ipc.TargetZoneID = 4242u;
            ipc.InstanceType = static_cast<u8>(200); // > 127: catches a signed-char misread
            ipc.MaxPlayers = 32u;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<InstancePortalComponent>());

        const auto& ipc = restored.GetComponent<InstancePortalComponent>();
        EXPECT_EQ(ipc.TargetZoneID, 4242u);
        EXPECT_EQ(ipc.InstanceType, static_cast<u8>(200));
        EXPECT_EQ(ipc.MaxPlayers, 32u);
    }

    // -------------------------------------------------------------------------
    // ParticleSystemComponent — header playback fields + a handful of
    // emitter scalars. The component's Texture / ParticleMesh refs and
    // ChildSystems sub-emitters are out of scope (Refs would couple to
    // renderer state); the full nested-emitter coverage is the job of
    // PrecipitationSystemTest / particle-specific tests.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, ParticleSystemComponentSurvivesYAMLRoundTrip)
    {
        const bool expectedPlaying = false; // non-default (default is true-ish, but mostly: deterministic)
        const bool expectedLooping = false; // non-default
        const f32 expectedDuration = 7.5f;
        const f32 expectedPlaybackSpeed = 1.75f;
        const f32 expectedRate = 25.0f;
        const f32 expectedSpeed = 4.5f;
        const f32 expectedLifetimeMin = 0.5f;
        const f32 expectedLifetimeMax = 2.5f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& ps = entity.AddComponent<ParticleSystemComponent>();
            ps.System.Playing = expectedPlaying;
            ps.System.Looping = expectedLooping;
            ps.System.Duration = expectedDuration;
            ps.System.PlaybackSpeed = expectedPlaybackSpeed;
            ps.System.Emitter.RateOverTime = expectedRate;
            ps.System.Emitter.InitialSpeed = expectedSpeed;
            ps.System.Emitter.LifetimeMin = expectedLifetimeMin;
            ps.System.Emitter.LifetimeMax = expectedLifetimeMax;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<ParticleSystemComponent>());

        const auto& ps = restored.GetComponent<ParticleSystemComponent>();
        EXPECT_EQ(ps.System.Playing, expectedPlaying);
        EXPECT_EQ(ps.System.Looping, expectedLooping);
        EXPECT_NEAR(ps.System.Duration, expectedDuration, kFloatEpsilon);
        EXPECT_NEAR(ps.System.PlaybackSpeed, expectedPlaybackSpeed, kFloatEpsilon);
        EXPECT_NEAR(ps.System.Emitter.RateOverTime, expectedRate, kFloatEpsilon);
        EXPECT_NEAR(ps.System.Emitter.InitialSpeed, expectedSpeed, kFloatEpsilon);
        EXPECT_NEAR(ps.System.Emitter.LifetimeMin, expectedLifetimeMin, kFloatEpsilon);
        EXPECT_NEAR(ps.System.Emitter.LifetimeMax, expectedLifetimeMax, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // CinematicComponent — authoring fields, with a NEGATIVE PlaybackSpeed to
    // prove reverse playback survives the scene round-trip. The deserializer
    // historically rejected speed < 0 (forward-only); reverse playback widened
    // the valid range, so the read path must accept (only) finite negatives.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, CinematicComponentReverseSpeedSurvivesYAMLRoundTrip)
    {
        const u64 expectedSequence = 4242u;
        const bool expectedPlayOnStart = true;
        const bool expectedLoop = true;
        const f32 expectedPlaybackSpeed = -1.5f; // reverse

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& cine = entity.AddComponent<CinematicComponent>();
            cine.Sequence = AssetHandle{ expectedSequence };
            cine.PlayOnStart = expectedPlayOnStart;
            cine.Loop = expectedLoop;
            cine.PlaybackSpeed = expectedPlaybackSpeed;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<CinematicComponent>());

        const auto& cine = restored.GetComponent<CinematicComponent>();
        EXPECT_EQ(static_cast<u64>(cine.Sequence), expectedSequence);
        EXPECT_EQ(cine.PlayOnStart, expectedPlayOnStart);
        EXPECT_EQ(cine.Loop, expectedLoop);
        EXPECT_NEAR(cine.PlaybackSpeed, expectedPlaybackSpeed, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // NetworkIdentityComponent — owner / authority / replicated flag.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, NetworkIdentityComponentSurvivesYAMLRoundTrip)
    {
        const u32 expectedOwnerID = 42;
        const ENetworkAuthority expectedAuthority = ENetworkAuthority::Client; // non-default
        const bool expectedReplicated = false;                                 // non-default

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& ni = entity.AddComponent<NetworkIdentityComponent>();
            ni.OwnerClientID = expectedOwnerID;
            ni.Authority = expectedAuthority;
            ni.IsReplicated = expectedReplicated;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<NetworkIdentityComponent>());

        const auto& ni = restored.GetComponent<NetworkIdentityComponent>();
        EXPECT_EQ(ni.OwnerClientID, expectedOwnerID);
        EXPECT_EQ(ni.Authority, expectedAuthority);
        EXPECT_EQ(ni.IsReplicated, expectedReplicated);
    }

    // -------------------------------------------------------------------------
    // NetworkInterestComponent — relevance radius + interest group.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, NetworkInterestComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedRadius = 75.0f;
        const u32 expectedGroup = 3;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& ni = entity.AddComponent<NetworkInterestComponent>();
            ni.RelevanceRadius = expectedRadius;
            ni.InterestGroup = expectedGroup;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<NetworkInterestComponent>());

        const auto& ni = restored.GetComponent<NetworkInterestComponent>();
        EXPECT_NEAR(ni.RelevanceRadius, expectedRadius, kFloatEpsilon);
        EXPECT_EQ(ni.InterestGroup, expectedGroup);
    }

    // -------------------------------------------------------------------------
    // MaterialComponent round-trip is intentionally absent here:
    // MaterialComponent carries a `Material m_Material;` value that holds
    // internal `Ref<Shader>` / `Ref<Texture>` handles initialised lazily by
    // Renderer3D. Touching those handles (which both
    // `SetBaseColorFactor()` and the deserializer's mirror calls do) on a
    // default-constructed Material with no renderer init SEH-crashes.
    // The right home for a MaterialComponent round-trip is the
    // RendererAttachedTest fixture family, where Renderer3D::Init has
    // run. Tracking as a follow-up alongside the renderer-attached
    // Scene-tick blocker.
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // AudioSourceComponent — exercises a nested Config struct + bool flags.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, AudioSourceComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedVolume = 0.4f;
        const f32 expectedPitch = 1.25f;
        const bool expectedLooping = true;
        const f32 expectedMinDistance = 2.5f;
        const f32 expectedMaxDistance = 50.0f;
        // Full 64-bit, non-default — catches a missing emit/read of the SoundConfig
        // (.olosoundc) preset link, or u64 precision loss in the AssetHandle path.
        const AssetHandle expectedPreset = 0x0123456789ABCDEFULL;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& as = entity.AddComponent<AudioSourceComponent>();
            as.GetConfig().VolumeMultiplier = expectedVolume;
            as.GetConfig().PitchMultiplier = expectedPitch;
            as.GetConfig().Looping = expectedLooping;
            as.GetConfig().MinDistance = expectedMinDistance;
            as.GetConfig().MaxDistance = expectedMaxDistance;
            as.SetSoundConfigHandle(expectedPreset);
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<AudioSourceComponent>());

        const auto& as = restored.GetComponent<AudioSourceComponent>();
        EXPECT_NEAR(as.GetConfig().VolumeMultiplier, expectedVolume, kFloatEpsilon);
        EXPECT_NEAR(as.GetConfig().PitchMultiplier, expectedPitch, kFloatEpsilon);
        EXPECT_EQ(as.GetConfig().Looping, expectedLooping);
        EXPECT_NEAR(as.GetConfig().MinDistance, expectedMinDistance, kFloatEpsilon);
        EXPECT_NEAR(as.GetConfig().MaxDistance, expectedMaxDistance, kFloatEpsilon);
        EXPECT_EQ(as.GetSoundConfigHandle(), expectedPreset)
            << "SoundConfigHandle dropped on scene YAML round-trip — check SceneSerializer emit/read.";
    }

    // -------------------------------------------------------------------------
    // SerializeLoadSerializeProducesIdenticalYAML
    //
    // Stronger invariant than per-component round-trips: a scene
    // serialized → loaded → serialized AGAIN must produce byte-identical
    // YAML on both serializations. Catches:
    //   - Component fields whose default values get re-emitted on the
    //     second pass (asymmetric defaults — one side emits, the other
    //     reads as missing-with-default).
    //   - Non-deterministic ordering of entity sequences or component
    //     fields.
    //   - Float precision drift that depends on which read path is taken
    //     (e.g. a vec3 emitted as `[1.5, 2.5, 3.5]` but read as
    //     `[1.5, 2.500000001, 3.5]`).
    //
    // Builds a representative scene with several components covering the
    // major serializer branches (transform, camera, sprite, light,
    // physics) so any of those code paths' asymmetries surface here
    // instead of waiting for a real scene file to drift.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, SerializeLoadSerializeProducesIdenticalYAML)
    {
        auto buildScene = []
        {
            auto scene = Scene::Create();

            Entity camera = scene->CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0, 0, 5 };
            auto& cc = camera.AddComponent<CameraComponent>();
            cc.Primary = true;
            cc.Camera.SetPerspectiveVerticalFOV(1.05f);

            Entity sprite = scene->CreateEntity("Sprite");
            sprite.AddComponent<SpriteRendererComponent>().Color = { 0.5f, 0.25f, 0.125f, 1.0f };

            Entity light = scene->CreateEntity("Sun");
            auto& dl = light.AddComponent<DirectionalLightComponent>();
            dl.m_Color = { 1.0f, 0.95f, 0.85f };
            dl.m_Intensity = 2.5f;

            Entity body = scene->CreateEntity("PhysicsBox");
            body.AddComponent<Rigidbody3DComponent>().m_Mass = 4.0f;
            body.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 1.0f, 0.5f, 0.5f };

            // Exercises the GENERATED std::unordered_set<std::string> serializer
            // path's sort-before-emit determinism (#451 unordered_map/set slice):
            // an unordered_set's own iteration order is not guaranteed stable, so
            // without the generator's sort step this entity alone could make
            // firstYaml/secondYaml differ.
            Entity prefabInstance = scene->CreateEntity("PrefabInstance");
            auto& pc = prefabInstance.AddComponent<PrefabComponent>(UUID(0xAAAAULL), UUID(0xBBBBULL));
            pc.SetOverriddenComponents({ "TransformComponent", "SpriteRendererComponent" });

            return scene;
        };

        // First serialization: builds the scene, dumps to YAML.
        const std::string firstYaml = SceneSerializer(buildScene()).SerializeToYAML();
        ASSERT_FALSE(firstYaml.empty());

        // Round-trip: load the YAML into a fresh Scene, dump again.
        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(firstYaml));
        const std::string secondYaml = SceneSerializer(reloaded).SerializeToYAML();

        EXPECT_EQ(firstYaml, secondYaml)
            << "Scene YAML drifted after a serialize→load→serialize cycle. "
               "Most likely cause: a component has an asymmetric default "
               "(one side emits the default, the other reads-with-default), "
               "or a non-deterministic ordering of entities / fields.\n"
               "First pass length:  "
            << firstYaml.size() << "\n"
                                   "Second pass length: "
            << secondYaml.size();
    }

    // -------------------------------------------------------------------------
    // SerializeLoadSerializeProducesIdenticalYAML — wider component set
    //
    // The existing `SerializeLoadSerializeProducesIdenticalYAML` covers
    // Camera + Sprite + DirectionalLight + Rigidbody3D + BoxCollider3D.
    // This variant adds entities exercising the components added in
    // later round-trip batches: PointLight, SpotLight, CircleRenderer,
    // Text, AudioSource, AudioListener, LightProbe, LightProbeVolume,
    // EnvironmentMap, mesh colliders, character controller, capsule
    // collider. If any of those components' serializers later develops
    // an asymmetric default, this test catches it.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, SerializeLoadSerializeWithWideComponentSetIsIdempotent)
    {
        auto buildScene = []
        {
            auto scene = Scene::Create();

            // Lights
            Entity pl = scene->CreateEntity("Point");
            {
                auto& l = pl.AddComponent<PointLightComponent>();
                l.m_Color = { 0.7f, 0.2f, 0.4f };
                l.m_Intensity = 3.0f;
                l.m_Range = 15.0f;
            }
            Entity sl = scene->CreateEntity("Spot");
            {
                auto& l = sl.AddComponent<SpotLightComponent>();
                l.m_Color = { 0.5f, 0.5f, 0.9f };
                l.m_InnerCutoff = 12.0f;
                l.m_OuterCutoff = 22.0f;
            }
            Entity al = scene->CreateEntity("SphereArea");
            {
                auto& l = al.AddComponent<SphereAreaLightComponent>();
                l.m_Color = { 0.95f, 0.7f, 0.3f };
                l.m_Intensity = 2.25f;
                l.m_Radius = 0.6f;
                l.m_Range = 12.0f;
            }

            // 2D renderers
            Entity circle = scene->CreateEntity("Circle");
            {
                auto& c = circle.AddComponent<CircleRendererComponent>();
                c.Color = { 0.1f, 0.9f, 0.5f, 0.8f };
                c.Thickness = 0.6f;
                c.Fade = 0.05f;
            }

            // Text
            Entity text = scene->CreateEntity("Text");
            {
                auto& t = text.AddComponent<TextComponent>();
                t.TextString = "Determinism check";
                t.Color = { 0.3f, 0.5f, 0.7f, 1.0f };
                t.Kerning = 0.125f;
                t.LineSpacing = 0.0625f;
            }

            // Audio
            Entity src = scene->CreateEntity("AudioSrc");
            {
                auto& s = src.AddComponent<AudioSourceComponent>();
                s.GetConfig().VolumeMultiplier = 0.6f;
                s.GetConfig().Looping = true;
                s.GetConfig().MinDistance = 2.0f;
                s.GetConfig().MaxDistance = 25.0f;
            }
            Entity listener = scene->CreateEntity("AudioListen");
            {
                auto& l = listener.AddComponent<AudioListenerComponent>();
                l.Active = false;
            }

            // Light probes
            Entity probe = scene->CreateEntity("Probe");
            {
                auto& p = probe.AddComponent<LightProbeComponent>();
                p.m_InfluenceRadius = 8.0f;
                p.m_Intensity = 1.25f;
                p.m_Active = true;
            }
            Entity probeVol = scene->CreateEntity("ProbeVolume");
            {
                auto& v = probeVol.AddComponent<LightProbeVolumeComponent>();
                v.m_BoundsMin = { -4, -1, -4 };
                v.m_BoundsMax = { 4, 3, 4 };
                v.m_Spacing = 2.0f;
                v.m_Intensity = 1.5f;
                v.m_Active = true;
                v.m_Mode = LightProbeVolumeComponent::Mode::Hybrid;
                v.m_RaysPerProbe = 1024;
                v.m_Hysteresis = 0.75f;
                v.m_ProbeCaptureBudget = 8;
                v.m_RelightBudget = 64;
                v.m_SelfShadowBias = 0.5f;
            }

            // Skybox / env map
            Entity env = scene->CreateEntity("Sky");
            {
                auto& e = env.AddComponent<EnvironmentMapComponent>();
                e.m_Rotation = 30.0f;
                e.m_Exposure = 1.8f;
                e.m_BlurAmount = 0.2f;
                e.m_EnableSkybox = false;
                e.m_EnableIBL = true;
                e.m_IBLIntensity = 1.5f;
            }

            // Mesh colliders
            Entity mc = scene->CreateEntity("MeshCollider");
            {
                auto& c = mc.AddComponent<MeshCollider3DComponent>();
                c.m_ColliderAsset = AssetHandle{ 111ULL };
                c.m_Offset = { 0, 0.5f, 0 };
                c.m_UseComplexAsSimple = true;
            }
            Entity cv = scene->CreateEntity("ConvexCollider");
            {
                auto& c = cv.AddComponent<ConvexMeshCollider3DComponent>();
                c.m_ColliderAsset = AssetHandle{ 222ULL };
                c.m_ConvexRadius = 0.1f;
                c.m_MaxVertices = 100;
            }
            Entity tm = scene->CreateEntity("TriMeshCollider");
            {
                auto& c = tm.AddComponent<TriangleMeshCollider3DComponent>();
                c.m_ColliderAsset = AssetHandle{ 333ULL };
                c.m_Scale = { 1.5f, 1.5f, 1.5f };
            }
            Entity cap = scene->CreateEntity("Capsule");
            {
                auto& c = cap.AddComponent<CapsuleCollider3DComponent>();
                c.m_Radius = 0.5f;
                c.m_HalfHeight = 1.0f;
            }

            // Character controller
            Entity cc = scene->CreateEntity("CharCtl");
            {
                auto& c = cc.AddComponent<CharacterController3DComponent>();
                c.m_SlopeLimitDeg = 50.0f;
                c.m_StepOffset = 0.3f;
                c.m_JumpPower = 9.5f;
            }

            return scene;
        };

        const std::string firstYaml = SceneSerializer(buildScene()).SerializeToYAML();
        ASSERT_FALSE(firstYaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(firstYaml));
        const std::string secondYaml = SceneSerializer(reloaded).SerializeToYAML();

        EXPECT_EQ(firstYaml, secondYaml)
            << "Scene YAML drifted after a serialize→load→serialize cycle with a "
               "wide component set. A component added since the original "
               "determinism test was written has introduced an asymmetric default.\n"
               "First pass length:  "
            << firstYaml.size() << "\n"
                                   "Second pass length: "
            << secondYaml.size();
    }

    // -------------------------------------------------------------------------
    // Entity hierarchy: parent → two children — round-trips parent UUID
    // and child UUID list, in the same order.
    //
    // Hierarchy is stored in RelationshipComponent and is a separate code
    // path from the per-entity component data: corruption can leave
    // entities orphaned ("my child object jumped to world origin after
    // reload because its parent reference vanished") even if every other
    // component's data survives.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, EntityHierarchyParentAndChildrenSurviveYAMLRoundTrip)
    {
        constexpr const char* parentTag = "Parent_uniqueP41C";
        constexpr const char* child1Tag = "ChildA_uniqueC73E";
        constexpr const char* child2Tag = "ChildB_uniqueC74F";

        UUID expectedParentUUID{};
        UUID expectedChild1UUID{};
        UUID expectedChild2UUID{};

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity parent = scene->CreateEntity(parentTag);
            Entity child1 = scene->CreateEntity(child1Tag);
            Entity child2 = scene->CreateEntity(child2Tag);

            child1.SetParent(parent);
            child2.SetParent(parent);

            expectedParentUUID = parent.GetUUID();
            expectedChild1UUID = child1.GetUUID();
            expectedChild2UUID = child2.GetUUID();

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restoredParent = FindByTag(*reloaded, parentTag);
        Entity restoredChild1 = FindByTag(*reloaded, child1Tag);
        Entity restoredChild2 = FindByTag(*reloaded, child2Tag);
        ASSERT_TRUE(static_cast<bool>(restoredParent));
        ASSERT_TRUE(static_cast<bool>(restoredChild1));
        ASSERT_TRUE(static_cast<bool>(restoredChild2));

        // UUIDs must round-trip — production code uses them as stable
        // identifiers between sessions.
        EXPECT_EQ(restoredParent.GetUUID(), expectedParentUUID);
        EXPECT_EQ(restoredChild1.GetUUID(), expectedChild1UUID);
        EXPECT_EQ(restoredChild2.GetUUID(), expectedChild2UUID);

        // Children must report the parent as their parent.
        ASSERT_TRUE(restoredChild1.HasComponent<RelationshipComponent>());
        ASSERT_TRUE(restoredChild2.HasComponent<RelationshipComponent>());
        EXPECT_EQ(restoredChild1.GetComponent<RelationshipComponent>().m_ParentHandle,
                  expectedParentUUID);
        EXPECT_EQ(restoredChild2.GetComponent<RelationshipComponent>().m_ParentHandle,
                  expectedParentUUID);

        // Parent must list both children, in the order they were attached.
        ASSERT_TRUE(restoredParent.HasComponent<RelationshipComponent>());
        const auto& parentRel = restoredParent.GetComponent<RelationshipComponent>();
        ASSERT_EQ(parentRel.m_Children.size(), 2u);
        EXPECT_EQ(parentRel.m_Children[0], expectedChild1UUID);
        EXPECT_EQ(parentRel.m_Children[1], expectedChild2UUID);
    }

    // -------------------------------------------------------------------------
    // WaterComponent — underwater rendering fields (WATER_FUTURE_IMPROVEMENTS.md §7.2)
    //
    // Focused on the newly-added underwater fields rather than the full
    // WaterComponent surface: those three fields are the only ones added in
    // this change, so a missing read/write on any of them is what this test
    // is here to catch.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, WaterComponentUnderwaterFieldsSurviveYAMLRoundTrip)
    {
        // Non-default, recognisable values so a dropped read or write is visible.
        const glm::vec3 expectedFogColor{ 0.12f, 0.34f, 0.56f };
        const f32 expectedFogDensity = 0.275f;
        const bool expectedRenderFromBelow = false; // default is true
        // Refraction (§7.2) + caustics (§7.1) fields. Values chosen inside each
        // field's sanitize range so the serializer doesn't clamp them away.
        const f32 expectedRefractionStrength = 0.042f;
        const f32 expectedRefractionScale = 27.5f;
        const f32 expectedRefractionSpeed = 2.25f;
        const f32 expectedChromaticStrength = 0.66f;
        const f32 expectedCausticsIntensity = 1.75f;
        const f32 expectedCausticsScale = 0.42f;
        const f32 expectedCausticsSpeed = 0.85f;
        const f32 expectedCausticsMaxDepth = 33.0f;
        const glm::vec3 expectedCausticsColor{ 0.65f, 0.8f, 0.95f };
        // God rays (§3.3). Values inside each field's sanitize range.
        const f32 expectedGodRayIntensity = 0.85f;
        const f32 expectedGodRayDecay = 0.94f;
        const f32 expectedGodRayDensity = 1.1f;
        const f32 expectedGodRayWeight = 0.12f;
        const glm::vec3 expectedGodRayColor{ 0.95f, 0.9f, 0.75f };
        const u32 expectedGodRaySamples = 72u;
        const f32 expectedGodRayDappleFloor = 0.4f;
        const f32 expectedGodRaySunFalloff = 24.0f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& water = entity.AddComponent<WaterComponent>();
            water.m_UnderwaterFogColor = expectedFogColor;
            water.m_UnderwaterFogDensity = expectedFogDensity;
            water.m_RenderFromBelow = expectedRenderFromBelow;
            water.m_UnderwaterRefractionStrength = expectedRefractionStrength;
            water.m_UnderwaterRefractionScale = expectedRefractionScale;
            water.m_UnderwaterRefractionSpeed = expectedRefractionSpeed;
            water.m_UnderwaterChromaticStrength = expectedChromaticStrength;
            water.m_CausticsIntensity = expectedCausticsIntensity;
            water.m_CausticsScale = expectedCausticsScale;
            water.m_CausticsSpeed = expectedCausticsSpeed;
            water.m_CausticsMaxDepth = expectedCausticsMaxDepth;
            water.m_CausticsColor = expectedCausticsColor;
            water.m_GodRayIntensity = expectedGodRayIntensity;
            water.m_GodRayDecay = expectedGodRayDecay;
            water.m_GodRayDensity = expectedGodRayDensity;
            water.m_GodRayWeight = expectedGodRayWeight;
            water.m_GodRayColor = expectedGodRayColor;
            water.m_GodRaySamples = expectedGodRaySamples;
            water.m_GodRayDappleFloor = expectedGodRayDappleFloor;
            water.m_GodRaySunFalloff = expectedGodRaySunFalloff;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<WaterComponent>());

        const auto& water = restored.GetComponent<WaterComponent>();
        EXPECT_NEAR(water.m_UnderwaterFogColor.r, expectedFogColor.r, kFloatEpsilon);
        EXPECT_NEAR(water.m_UnderwaterFogColor.g, expectedFogColor.g, kFloatEpsilon);
        EXPECT_NEAR(water.m_UnderwaterFogColor.b, expectedFogColor.b, kFloatEpsilon);
        EXPECT_NEAR(water.m_UnderwaterFogDensity, expectedFogDensity, kFloatEpsilon);
        EXPECT_EQ(water.m_RenderFromBelow, expectedRenderFromBelow);
        EXPECT_NEAR(water.m_UnderwaterRefractionStrength, expectedRefractionStrength, kFloatEpsilon);
        EXPECT_NEAR(water.m_UnderwaterRefractionScale, expectedRefractionScale, kFloatEpsilon);
        EXPECT_NEAR(water.m_UnderwaterRefractionSpeed, expectedRefractionSpeed, kFloatEpsilon);
        EXPECT_NEAR(water.m_UnderwaterChromaticStrength, expectedChromaticStrength, kFloatEpsilon);
        EXPECT_NEAR(water.m_CausticsIntensity, expectedCausticsIntensity, kFloatEpsilon);
        EXPECT_NEAR(water.m_CausticsScale, expectedCausticsScale, kFloatEpsilon);
        EXPECT_NEAR(water.m_CausticsSpeed, expectedCausticsSpeed, kFloatEpsilon);
        EXPECT_NEAR(water.m_CausticsMaxDepth, expectedCausticsMaxDepth, kFloatEpsilon);
        EXPECT_NEAR(water.m_CausticsColor.r, expectedCausticsColor.r, kFloatEpsilon);
        EXPECT_NEAR(water.m_CausticsColor.g, expectedCausticsColor.g, kFloatEpsilon);
        EXPECT_NEAR(water.m_CausticsColor.b, expectedCausticsColor.b, kFloatEpsilon);
        EXPECT_NEAR(water.m_GodRayIntensity, expectedGodRayIntensity, kFloatEpsilon);
        EXPECT_NEAR(water.m_GodRayDecay, expectedGodRayDecay, kFloatEpsilon);
        EXPECT_NEAR(water.m_GodRayDensity, expectedGodRayDensity, kFloatEpsilon);
        EXPECT_NEAR(water.m_GodRayWeight, expectedGodRayWeight, kFloatEpsilon);
        EXPECT_NEAR(water.m_GodRayColor.r, expectedGodRayColor.r, kFloatEpsilon);
        EXPECT_NEAR(water.m_GodRayColor.g, expectedGodRayColor.g, kFloatEpsilon);
        EXPECT_NEAR(water.m_GodRayColor.b, expectedGodRayColor.b, kFloatEpsilon);
        EXPECT_EQ(water.m_GodRaySamples, expectedGodRaySamples);
        EXPECT_NEAR(water.m_GodRayDappleFloor, expectedGodRayDappleFloor, kFloatEpsilon);
        EXPECT_NEAR(water.m_GodRaySunFalloff, expectedGodRaySunFalloff, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // WaterComponent — FFT ocean + spectrum-selection fields (§1, §1.4)
    //
    // Covers the Tessendorf FFT block, including the JONSWAP spectrum selector
    // and its gamma/fetch params. A dropped read/write on the spectrum fields
    // would silently revert a JONSWAP scene to Phillips on reload.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, WaterComponentFFTSpectrumFieldsSurviveYAMLRoundTrip)
    {
        const bool expectedUseFFT = true;
        const u32 expectedResolution = 256u;
        const f32 expectedPatchSize = 120.0f;
        const f32 expectedWindSpeed = 22.0f;
        const glm::vec2 expectedWindDir{ 0.6f, 0.8f };
        const f32 expectedAmplitude = 3.5f;
        const f32 expectedChoppiness = 1.5f;
        const f32 expectedHeightScale = 1.3f;
        const u32 expectedSeed = 4242u;
        const bool expectedGpuCompute = false; // default is true
        const Ocean::SpectrumType expectedSpectrum = Ocean::SpectrumType::JONSWAP;
        const f32 expectedGamma = 4.2f;
        const f32 expectedFetch = 50000.0f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& water = entity.AddComponent<WaterComponent>();
            water.m_UseFFT = expectedUseFFT;
            water.m_FFTResolution = expectedResolution;
            water.m_FFTPatchSize = expectedPatchSize;
            water.m_FFTWindSpeed = expectedWindSpeed;
            water.m_FFTWindDirection = expectedWindDir;
            water.m_FFTAmplitude = expectedAmplitude;
            water.m_FFTChoppiness = expectedChoppiness;
            water.m_FFTHeightScale = expectedHeightScale;
            water.m_FFTSeed = expectedSeed;
            water.m_FFTUseGpuCompute = expectedGpuCompute;
            water.m_FFTSpectrumType = expectedSpectrum;
            water.m_FFTJonswapGamma = expectedGamma;
            water.m_FFTJonswapFetch = expectedFetch;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<WaterComponent>());

        const auto& water = restored.GetComponent<WaterComponent>();
        EXPECT_EQ(water.m_UseFFT, expectedUseFFT);
        EXPECT_EQ(water.m_FFTResolution, expectedResolution);
        EXPECT_NEAR(water.m_FFTPatchSize, expectedPatchSize, kFloatEpsilon);
        EXPECT_NEAR(water.m_FFTWindSpeed, expectedWindSpeed, kFloatEpsilon);
        EXPECT_NEAR(water.m_FFTWindDirection.x, expectedWindDir.x, kFloatEpsilon);
        EXPECT_NEAR(water.m_FFTWindDirection.y, expectedWindDir.y, kFloatEpsilon);
        EXPECT_NEAR(water.m_FFTAmplitude, expectedAmplitude, kFloatEpsilon);
        EXPECT_NEAR(water.m_FFTChoppiness, expectedChoppiness, kFloatEpsilon);
        EXPECT_NEAR(water.m_FFTHeightScale, expectedHeightScale, kFloatEpsilon);
        EXPECT_EQ(water.m_FFTSeed, expectedSeed);
        EXPECT_EQ(water.m_FFTUseGpuCompute, expectedGpuCompute);
        EXPECT_EQ(water.m_FFTSpectrumType, expectedSpectrum);
        EXPECT_NEAR(water.m_FFTJonswapGamma, expectedGamma, kFloatEpsilon);
        EXPECT_NEAR(water.m_FFTJonswapFetch, expectedFetch, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // BuoyancyComponent — every serialized field must survive a YAML round-trip
    // (one of the five component touch-points; a dropped read/write here would
    // silently desync a saved scene from its in-editor setup). See
    // docs/design/WATER_FUTURE_IMPROVEMENTS.md §5.1.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, BuoyancyComponentSurvivesYAMLRoundTrip)
    {
        // Non-default, recognisable values so a dropped field is visible.
        const bool expectedEnabled = false; // default is true
        const glm::vec3 expectedExtents{ 0.7f, 0.4f, 1.3f };
        const f32 expectedDensity = 1025.0f;
        const f32 expectedScale = 1.4f;
        const f32 expectedLinearDrag = 2.5f;
        const f32 expectedAngularDrag = 1.1f;
        const f32 expectedRamp = 0.6f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& b = entity.AddComponent<BuoyancyComponent>();
            b.m_Enabled = expectedEnabled;
            b.m_ProbeExtents = expectedExtents;
            b.m_FluidDensity = expectedDensity;
            b.m_BuoyancyScale = expectedScale;
            b.m_LinearDrag = expectedLinearDrag;
            b.m_AngularDrag = expectedAngularDrag;
            b.m_SubmergenceRamp = expectedRamp;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<BuoyancyComponent>());

        const auto& b = restored.GetComponent<BuoyancyComponent>();
        EXPECT_EQ(b.m_Enabled, expectedEnabled);
        EXPECT_NEAR(b.m_ProbeExtents.x, expectedExtents.x, kFloatEpsilon);
        EXPECT_NEAR(b.m_ProbeExtents.y, expectedExtents.y, kFloatEpsilon);
        EXPECT_NEAR(b.m_ProbeExtents.z, expectedExtents.z, kFloatEpsilon);
        EXPECT_NEAR(b.m_FluidDensity, expectedDensity, kFloatEpsilon);
        EXPECT_NEAR(b.m_BuoyancyScale, expectedScale, kFloatEpsilon);
        EXPECT_NEAR(b.m_LinearDrag, expectedLinearDrag, kFloatEpsilon);
        EXPECT_NEAR(b.m_AngularDrag, expectedAngularDrag, kFloatEpsilon);
        EXPECT_NEAR(b.m_SubmergenceRamp, expectedRamp, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // VehicleComponent — issue #438 both ADDED the drive-mode/differential
    // fields and MIGRATED the whole component off its hand-written serializer
    // onto the generated one (OLO_SERIALIZE(Skip) for the runtime token, a
    // per-field OLO_SERIALIZE(Clamp, ...) for each authored float). This
    // round-trip is what pins that migration: an emitted key the generator
    // spells differently, or a Clamp range narrower than the value being
    // authored, would silently rewrite a designer's tuning on the next load.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, VehicleComponentSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& v = entity.AddComponent<VehicleComponent>();
            v.m_HalfTrackWidth = 1.15f;
            v.m_FrontAxleOffset = 1.45f;
            v.m_RearAxleOffset = 1.65f;
            v.m_WheelAttachmentHeight = -0.55f;
            v.m_WheelRadius = 0.42f;
            v.m_WheelWidth = 0.31f;
            v.m_SuspensionMinLength = 0.22f;
            v.m_SuspensionMaxLength = 0.62f;
            v.m_SuspensionFrequency = 2.1f;
            v.m_SuspensionDamping = 0.72f;
            v.m_MaxEngineTorque = 655.0f;
            v.m_MaxSteerAngleDeg = 35.0f;
            v.m_MaxBrakeTorque = 1850.0f;
            v.m_DriveMode = VehicleDriveMode::AllWheelDrive;
            v.m_FrontTorqueSplit = 0.35f;
            v.m_LeftRightSplit = 0.45f;
            v.m_LimitedSlipRatio = 2.2f;
            v.m_CenterLimitedSlipRatio = 3.1f;
            v.m_DifferentialRatio = 4.11f;
            v.m_ThrottleInput = 0.5f;
            v.m_SteerInput = -0.25f;
            v.m_BrakeInput = 0.1f;
            // Runtime-only: OLO_SERIALIZE(Skip) must keep this OFF disk, so it
            // has to come back as 0 no matter what it held at save time.
            v.m_RuntimeVehicleToken = 12345u;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<VehicleComponent>());

        const auto& v = restored.GetComponent<VehicleComponent>();
        EXPECT_NEAR(v.m_HalfTrackWidth, 1.15f, kFloatEpsilon);
        EXPECT_NEAR(v.m_FrontAxleOffset, 1.45f, kFloatEpsilon);
        EXPECT_NEAR(v.m_RearAxleOffset, 1.65f, kFloatEpsilon);
        EXPECT_NEAR(v.m_WheelAttachmentHeight, -0.55f, kFloatEpsilon);
        EXPECT_NEAR(v.m_WheelRadius, 0.42f, kFloatEpsilon);
        EXPECT_NEAR(v.m_WheelWidth, 0.31f, kFloatEpsilon);
        EXPECT_NEAR(v.m_SuspensionMinLength, 0.22f, kFloatEpsilon);
        EXPECT_NEAR(v.m_SuspensionMaxLength, 0.62f, kFloatEpsilon);
        EXPECT_NEAR(v.m_SuspensionFrequency, 2.1f, kFloatEpsilon);
        EXPECT_NEAR(v.m_SuspensionDamping, 0.72f, kFloatEpsilon);
        EXPECT_NEAR(v.m_MaxEngineTorque, 655.0f, kFloatEpsilon);
        EXPECT_NEAR(v.m_MaxSteerAngleDeg, 35.0f, kFloatEpsilon);
        EXPECT_NEAR(v.m_MaxBrakeTorque, 1850.0f, kFloatEpsilon);
        EXPECT_EQ(v.m_DriveMode, VehicleDriveMode::AllWheelDrive);
        EXPECT_NEAR(v.m_FrontTorqueSplit, 0.35f, kFloatEpsilon);
        EXPECT_NEAR(v.m_LeftRightSplit, 0.45f, kFloatEpsilon);
        EXPECT_NEAR(v.m_LimitedSlipRatio, 2.2f, kFloatEpsilon);
        EXPECT_NEAR(v.m_CenterLimitedSlipRatio, 3.1f, kFloatEpsilon);
        EXPECT_NEAR(v.m_DifferentialRatio, 4.11f, kFloatEpsilon);
        EXPECT_NEAR(v.m_ThrottleInput, 0.5f, kFloatEpsilon);
        EXPECT_NEAR(v.m_SteerInput, -0.25f, kFloatEpsilon);
        EXPECT_NEAR(v.m_BrakeInput, 0.1f, kFloatEpsilon);
        EXPECT_EQ(v.m_RuntimeVehicleToken, 0u)
            << "the runtime Jolt token was persisted despite OLO_SERIALIZE(Skip)";
    }

    // -------------------------------------------------------------------------
    // m_DriveMode is OLO_SERIALIZE(Reject, …), not Clamp — a corrupted or
    // hand-edited DriveMode must fall back to the RearWheelDrive DEFAULT.
    //
    // This is the whole point of the Reject slice. Under the previous
    // Clamp(0, 2) the same corrupt `7` saturated to 2 = AllWheelDrive: a
    // perfectly valid mode, silently driving the wrong axles, and disagreeing
    // with both SaveGameComponentSerializer and JoltScene::CreateVehicle, which
    // map anything that isn't Front/AllWheelDrive back to RearWheelDrive.
    // A clamp-shaped regression here is invisible without this test, because
    // the loaded value is still a legal enumerator and the car still drives.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, VehicleDriveModeRejectsOutOfRangeToTheDefault)
    {
        // The two setup steps are ASSERT (fatal), not EXPECT: a missing DriveMode
        // key would make yaml.replace(npos, …) throw, and a missing entity would
        // make GetComponent read a component that isn't there — both must stop
        // before the damage, not merely record a failure and carry on.
        //
        // ASSERT_* expands to a bare `return`, so it is only valid in a
        // void-returning callable; the loader therefore writes through an
        // out-param and a thin wrapper keeps the call sites below reading as
        // plain expressions.
        const auto loadDriveModeInto = [](const std::string& rawValue, VehicleDriveMode& out)
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            entity.AddComponent<VehicleComponent>().m_DriveMode = VehicleDriveMode::FrontWheelDrive;
            std::string yaml = SceneSerializer(scene).SerializeToYAML();

            // Rewrite just the DriveMode scalar, leaving the rest of the document
            // untouched — this is what a corrupted or hand-edited scene looks like.
            const auto pos = yaml.find("DriveMode: ");
            ASSERT_NE(pos, std::string::npos) << "serialized VehicleComponent has no DriveMode key to corrupt";
            const auto eol = yaml.find('\n', pos);
            yaml.replace(pos, eol - pos, "DriveMode: " + rawValue);

            auto reloaded = Scene::Create();
            ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));
            Entity restored = FindByTag(*reloaded, kTestTag);
            ASSERT_TRUE(static_cast<bool>(restored));
            out = restored.GetComponent<VehicleComponent>().m_DriveMode;
        };
        const auto loadDriveMode = [&loadDriveModeInto](const std::string& rawValue)
        {
            // A fatal failure inside the loader leaves `mode` untouched, but the
            // ASSERT has already failed the test — this value can never turn a
            // broken setup into a pass.
            VehicleDriveMode mode = VehicleDriveMode::AllWheelDrive;
            loadDriveModeInto(rawValue, mode);
            return mode;
        };

        // Valid enumerators still round-trip untouched.
        EXPECT_EQ(loadDriveMode("0"), VehicleDriveMode::RearWheelDrive);
        EXPECT_EQ(loadDriveMode("1"), VehicleDriveMode::FrontWheelDrive);
        EXPECT_EQ(loadDriveMode("2"), VehicleDriveMode::AllWheelDrive);

        // Out of range in either direction -> the constructor default, NOT the
        // nearest bound. "7 -> AllWheelDrive" is precisely the clamp bug.
        EXPECT_EQ(loadDriveMode("7"), VehicleDriveMode::RearWheelDrive)
            << "an above-range DriveMode saturated to AllWheelDrive instead of "
               "falling back to the default — Reject has regressed to Clamp";
        EXPECT_EQ(loadDriveMode("-3"), VehicleDriveMode::RearWheelDrive);
        EXPECT_EQ(loadDriveMode("2147483647"), VehicleDriveMode::RearWheelDrive);

        // And the scene path now agrees with the save-game path's rule.
        for (const char* raw : { "3", "7", "-1", "99" })
        {
            const auto mode = loadDriveMode(raw);
            EXPECT_TRUE(mode == VehicleDriveMode::RearWheelDrive)
                << "scene load of DriveMode " << raw << " disagrees with "
                                                        "SaveGameComponentSerializer, which maps every unrecognised "
                                                        "value to RearWheelDrive";
        }
    }

    // -------------------------------------------------------------------------
    // Rigidbody3DComponent's INITIAL velocities (issue #438 follow-up). These are
    // applied once, at body creation, and were previously runtime-only — so a
    // scene could not author a body that is already moving when Play starts, and
    // "an aircraft in cruise" was simply unexpressible. They are now serialized.
    //
    // The round-trip matters more than usual here because the field is write-once:
    // if it silently reverted to zero on load, everything would still simulate
    // perfectly well — the vehicle would just start from a standstill, which reads
    // as a design choice rather than a dropped field.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, Rigidbody3DInitialVelocitiesSurviveYAMLRoundTrip)
    {
        const glm::vec3 expectedLinear{ 12.5f, -3.25f, 71.0f };
        const glm::vec3 expectedAngular{ 0.25f, -1.5f, 0.75f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& rb = entity.AddComponent<Rigidbody3DComponent>();
            rb.m_Type = BodyType3D::Dynamic;
            rb.m_InitialLinearVelocity = expectedLinear;
            rb.m_InitialAngularVelocity = expectedAngular;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<Rigidbody3DComponent>());

        const auto& rb = restored.GetComponent<Rigidbody3DComponent>();
        EXPECT_NEAR(rb.m_InitialLinearVelocity.x, expectedLinear.x, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialLinearVelocity.y, expectedLinear.y, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialLinearVelocity.z, expectedLinear.z, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialAngularVelocity.x, expectedAngular.x, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialAngularVelocity.y, expectedAngular.y, kFloatEpsilon);
        EXPECT_NEAR(rb.m_InitialAngularVelocity.z, expectedAngular.z, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // A scene written BEFORE the initial-velocity keys existed must still load,
    // with the body simply starting at rest. This is the backward-compatibility
    // half of the change above, and it is what makes adding the keys safe for
    // every scene already on disk.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, Rigidbody3DWithoutInitialVelocityKeysLoadsAtRest)
    {
        // Hand-written YAML with the pre-#438 Rigidbody3DComponent key set only.
        const std::string legacyYaml = R"(Scene: Legacy
Entities:
  - Entity: 1234567890123456789
    TagComponent:
      Tag: )" + std::string(kTestTag) + R"(
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    Rigidbody3DComponent:
      BodyType: 1
      Mass: 5
      LinearDrag: 0.02
      AngularDrag: 0.04
      DisableGravity: false
      IsTrigger: false
)";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(legacyYaml))
            << "a scene predating the initial-velocity keys failed to load";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<Rigidbody3DComponent>());

        const auto& rb = restored.GetComponent<Rigidbody3DComponent>();
        EXPECT_NEAR(rb.m_Mass, 5.0f, kFloatEpsilon) << "the rest of the block did not survive";
        EXPECT_NEAR(glm::length(rb.m_InitialLinearVelocity), 0.0f, kFloatEpsilon)
            << "a legacy scene picked up a non-zero initial velocity";
        EXPECT_NEAR(glm::length(rb.m_InitialAngularVelocity), 0.0f, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // BoatComponent (issue #438) — fully generated, so this round-trip is the
    // guard that its authored hull tuning reaches disk and comes back. A boat
    // whose thrust offsets or drag coefficients silently reverted to defaults
    // on load would still float and still drive; only the handling would be
    // wrong, which is exactly the kind of drift a round-trip test catches and
    // a behavioural one does not.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, BoatComponentSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& b = entity.AddComponent<BoatComponent>();
            b.m_Enabled = false; // default is true
            b.m_MaxThrust = 7500.0f;
            b.m_ThrustOffsetZ = -3.25f;
            b.m_ThrustOffsetY = -0.65f;
            b.m_MaxRudderTorque = 12000.0f;
            b.m_RudderAuthoritySpeed = 6.5f;
            b.m_LateralDrag = 4.25f;
            b.m_ForwardDrag = 0.45f;
            b.m_YawDrag = 2.75f;
            b.m_ImmersionDepth = 0.85f;
            b.m_ThrottleInput = 0.5f;
            b.m_SteerInput = -0.25f;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<BoatComponent>());

        const auto& b = restored.GetComponent<BoatComponent>();
        EXPECT_FALSE(b.m_Enabled);
        EXPECT_NEAR(b.m_MaxThrust, 7500.0f, kFloatEpsilon);
        EXPECT_NEAR(b.m_ThrustOffsetZ, -3.25f, kFloatEpsilon);
        EXPECT_NEAR(b.m_ThrustOffsetY, -0.65f, kFloatEpsilon);
        EXPECT_NEAR(b.m_MaxRudderTorque, 12000.0f, kFloatEpsilon);
        EXPECT_NEAR(b.m_RudderAuthoritySpeed, 6.5f, kFloatEpsilon);
        EXPECT_NEAR(b.m_LateralDrag, 4.25f, kFloatEpsilon);
        EXPECT_NEAR(b.m_ForwardDrag, 0.45f, kFloatEpsilon);
        EXPECT_NEAR(b.m_YawDrag, 2.75f, kFloatEpsilon);
        EXPECT_NEAR(b.m_ImmersionDepth, 0.85f, kFloatEpsilon);
        EXPECT_NEAR(b.m_ThrottleInput, 0.5f, kFloatEpsilon);
        EXPECT_NEAR(b.m_SteerInput, -0.25f, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // AircraftComponent (issue #438) — same reasoning as the boat, with more at
    // stake: the aerodynamic coefficients ARE the flight model, so a field that
    // quietly reverted to its default on load would change how the aircraft
    // flies without changing whether it flies.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, AircraftComponentSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& a = entity.AddComponent<AircraftComponent>();
            a.m_Enabled = false; // default is true
            a.m_MaxThrust = 8200.0f;
            a.m_WingArea = 22.5f;
            a.m_AirDensity = 0.9f;
            a.m_LiftSlope = 6.1f;
            a.m_ZeroLiftCoefficient = 0.15f;
            a.m_StallAngleDeg = 18.0f;
            a.m_DragCoefficient = 0.045f;
            a.m_InducedDragFactor = 0.07f;
            a.m_PitchTorque = 26000.0f;
            a.m_RollTorque = 31000.0f;
            a.m_YawTorque = 13000.0f;
            a.m_ControlAuthoritySpeed = 55.0f;
            a.m_PitchDamping = 5.5f;
            a.m_RollDamping = 3.5f;
            a.m_YawDamping = 4.5f;
            a.m_WeathervaneStrength = 2.25f;
            a.m_ThrottleInput = 0.75f;
            a.m_PitchInput = -0.4f;
            a.m_RollInput = 0.6f;
            a.m_YawInput = -0.2f;

            // Landing gear (issue #438 follow-up). These decide whether the
            // aircraft can rotate on the ground at all, and a dropped one still
            // simulates perfectly — it just silently reverts to the belly-landed
            // behaviour the gear exists to fix. Every value below is off-default
            // and inside the field's OLO_SERIALIZE(Clamp) range, so a value that
            // comes back changed means the round trip lost or clamped it.
            a.m_HasLandingGear = true; // default is false
            a.m_MainGearOffsetZ = -0.85f;
            a.m_MainGearHalfTrack = 1.4f;
            a.m_NoseGearOffsetZ = 3.1f;
            a.m_GearLength = 0.95f;
            a.m_GearStiffness = 9.5f;
            a.m_GearDamping = 0.65f;
            a.m_GearRollingResistance = 0.045f;
            a.m_GearLateralGrip = 2.25f;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<AircraftComponent>());

        const auto& a = restored.GetComponent<AircraftComponent>();
        EXPECT_FALSE(a.m_Enabled);
        EXPECT_NEAR(a.m_MaxThrust, 8200.0f, kFloatEpsilon);
        EXPECT_NEAR(a.m_WingArea, 22.5f, kFloatEpsilon);
        EXPECT_NEAR(a.m_AirDensity, 0.9f, kFloatEpsilon);
        EXPECT_NEAR(a.m_LiftSlope, 6.1f, kFloatEpsilon);
        EXPECT_NEAR(a.m_ZeroLiftCoefficient, 0.15f, kFloatEpsilon);
        EXPECT_NEAR(a.m_StallAngleDeg, 18.0f, kFloatEpsilon);
        EXPECT_NEAR(a.m_DragCoefficient, 0.045f, kFloatEpsilon);
        EXPECT_NEAR(a.m_InducedDragFactor, 0.07f, kFloatEpsilon);
        EXPECT_NEAR(a.m_PitchTorque, 26000.0f, kFloatEpsilon);
        EXPECT_NEAR(a.m_RollTorque, 31000.0f, kFloatEpsilon);
        EXPECT_NEAR(a.m_YawTorque, 13000.0f, kFloatEpsilon);
        EXPECT_NEAR(a.m_ControlAuthoritySpeed, 55.0f, kFloatEpsilon);
        EXPECT_NEAR(a.m_PitchDamping, 5.5f, kFloatEpsilon);
        EXPECT_NEAR(a.m_RollDamping, 3.5f, kFloatEpsilon);
        EXPECT_NEAR(a.m_YawDamping, 4.5f, kFloatEpsilon);
        EXPECT_NEAR(a.m_WeathervaneStrength, 2.25f, kFloatEpsilon);
        EXPECT_NEAR(a.m_ThrottleInput, 0.75f, kFloatEpsilon);
        EXPECT_NEAR(a.m_PitchInput, -0.4f, kFloatEpsilon);
        EXPECT_NEAR(a.m_RollInput, 0.6f, kFloatEpsilon);
        EXPECT_NEAR(a.m_YawInput, -0.2f, kFloatEpsilon);

        EXPECT_TRUE(a.m_HasLandingGear);
        EXPECT_NEAR(a.m_MainGearOffsetZ, -0.85f, kFloatEpsilon);
        EXPECT_NEAR(a.m_MainGearHalfTrack, 1.4f, kFloatEpsilon);
        EXPECT_NEAR(a.m_NoseGearOffsetZ, 3.1f, kFloatEpsilon);
        EXPECT_NEAR(a.m_GearLength, 0.95f, kFloatEpsilon);
        EXPECT_NEAR(a.m_GearStiffness, 9.5f, kFloatEpsilon);
        EXPECT_NEAR(a.m_GearDamping, 0.65f, kFloatEpsilon);
        EXPECT_NEAR(a.m_GearRollingResistance, 0.045f, kFloatEpsilon);
        EXPECT_NEAR(a.m_GearLateralGrip, 2.25f, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // InstancedMeshComponent — each instance's Transform (16 floats) and Color
    // (4 floats) are serialized as flat float sequences. This flat-array path
    // was the only authored round-trip data with no direct coverage. A
    // distinctive, fully-populated transform (every one of the 16 floats
    // different) makes a column/row transposition or off-by-one in the flat
    // (de)serialization detectable. Also exercises the emit-only-when-non-
    // default branches for EntityID (-1) and Custom (0), which must still
    // recover their defaults on load.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, InstancedMeshComponentInstancesSurviveYAMLRoundTrip)
    {
        // Distinct value per matrix element so any ordering bug is visible.
        // Uses glm's operator[] (column-major [col][row]) rather than the
        // serializer's flat-pointer access, keeping the check independent.
        auto makeTransform = [](f32 seed)
        {
            glm::mat4 m(1.0f);
            for (glm::length_t c = 0; c < 4; ++c)
                for (glm::length_t r = 0; r < 4; ++r)
                    m[c][r] = seed + static_cast<f32>(c * 4 + r) * 0.5f - 3.0f;
            return m;
        };

        const glm::mat4 expectedTransforms[2] = { makeTransform(1.0f), makeTransform(40.0f) };
        const glm::vec4 expectedColors[2] = { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.9f, 0.8f, 0.7f, 0.6f } };
        const i32 expectedIDs[2] = { 7, -1 };           // 7 is emitted; -1 is skipped and must default back to -1
        const f32 expectedCustoms[2] = { 2.75f, 0.0f }; // 2.75 is emitted; 0.0 is skipped and must default back to 0

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& imc = entity.AddComponent<InstancedMeshComponent>();
            for (sizet inst = 0; inst < 2; ++inst)
            {
                InstanceData data;
                data.Transform = expectedTransforms[inst];
                data.Color = expectedColors[inst];
                data.EntityID = expectedIDs[inst];
                data.Custom = expectedCustoms[inst];
                imc.Instances.push_back(data);
            }

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<InstancedMeshComponent>())
            << "InstancedMeshComponent was dropped during round-trip.";

        const auto& imc = restored.GetComponent<InstancedMeshComponent>();
        ASSERT_EQ(imc.Instances.size(), 2u) << "Instance count changed across the round-trip.";

        for (sizet inst = 0; inst < 2; ++inst)
        {
            const auto& data = imc.Instances[inst];
            for (glm::length_t c = 0; c < 4; ++c)
                for (glm::length_t r = 0; r < 4; ++r)
                    EXPECT_NEAR(data.Transform[c][r], expectedTransforms[inst][c][r], kFloatEpsilon)
                        << "instance " << inst << " transform [" << c << "][" << r << "]";
            for (glm::length_t i = 0; i < 4; ++i)
                EXPECT_NEAR(data.Color[i], expectedColors[inst][i], kFloatEpsilon)
                    << "instance " << inst << " color [" << i << "]";
            EXPECT_EQ(data.EntityID, expectedIDs[inst]) << "instance " << inst << " EntityID";
            EXPECT_NEAR(data.Custom, expectedCustoms[inst], kFloatEpsilon) << "instance " << inst << " Custom";
        }
    }

    // -------------------------------------------------------------------------
    // InstancedMeshComponent — non-finite instance floats (NaN/Inf) injected
    // into a saved scene (corrupt file, bad authoring tool, hostile input) must
    // be sanitized on load, never uploaded to the instance SSBO. Guards the
    // Math::IsFinite checks on the instance read path. Injection goes through
    // the YAML *parser* (which maps .nan/.inf to NaN/Inf) rather than emitting
    // a NaN, since MSVC's float→text for NaN is not valid YAML.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, InstancedMeshComponentNonFiniteInstanceDataIsSanitizedOnLoad)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& imc = entity.AddComponent<InstancedMeshComponent>();
            InstanceData data;
            data.Transform = glm::mat4(2.0f); // distinctive finite values
            data.Color = glm::vec4(0.25f, 0.5f, 0.75f, 1.0f);
            data.Custom = 3.0f;
            imc.Instances.push_back(data);
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        // Replace the flow-style instance arrays with ones carrying .nan/.inf.
        // The block-mapped "TransformComponent:" key is not followed by '[', so
        // only the instance's "Transform: [..]" / "Color: [..]" seqs are hit.
        yaml = std::regex_replace(yaml, std::regex(R"(Transform: \[[^\]]*\])"),
                                  "Transform: [.nan, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]");
        yaml = std::regex_replace(yaml, std::regex(R"(Color: \[[^\]]*\])"),
                                  "Color: [.inf, 0.5, 0.5, 1]");

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "Deserialize rejected the (structurally valid) NaN/Inf-injected scene.";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<InstancedMeshComponent>());

        const auto& imc = restored.GetComponent<InstancedMeshComponent>();
        ASSERT_EQ(imc.Instances.size(), 1u);

        // Non-finite transform → reset to identity; non-finite color → white.
        const auto& d = imc.Instances[0];
        const glm::mat4 identity{ 1.0f };
        for (glm::length_t c = 0; c < 4; ++c)
            for (glm::length_t r = 0; r < 4; ++r)
            {
                EXPECT_TRUE(std::isfinite(d.Transform[c][r]))
                    << "transform [" << c << "][" << r << "] left non-finite";
                EXPECT_NEAR(d.Transform[c][r], identity[c][r], kFloatEpsilon)
                    << "transform [" << c << "][" << r << "] should be reset to identity";
            }
        for (glm::length_t i = 0; i < 4; ++i)
        {
            EXPECT_TRUE(std::isfinite(d.Color[i])) << "color [" << i << "] left non-finite";
            EXPECT_NEAR(d.Color[i], 1.0f, kFloatEpsilon) << "color [" << i << "] should be reset to white";
        }
    }

    // -------------------------------------------------------------------------
    // SpringBoneComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, SpringBoneComponentSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& spring = entity.AddComponent<SpringBoneComponent>();
            // Distinctive non-default values on every field.
            spring.Enabled = false;
            spring.EndBoneIndex = 7;
            spring.ChainLength = 5;
            spring.Stiffness = 33.5f;
            spring.Damping = 4.25f;
            spring.Gravity = glm::vec3(0.5f, -3.0f, 1.25f);
            spring.Weight = 0.625f;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty()) << "SerializeToYAML produced an empty string.";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "DeserializeFromYAML rejected the just-serialised scene.";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<SpringBoneComponent>());

        const auto& spring = restored.GetComponent<SpringBoneComponent>();
        EXPECT_FALSE(spring.Enabled);
        EXPECT_EQ(spring.EndBoneIndex, 7u);
        EXPECT_EQ(spring.ChainLength, 5u);
        EXPECT_NEAR(spring.Stiffness, 33.5f, kFloatEpsilon);
        EXPECT_NEAR(spring.Damping, 4.25f, kFloatEpsilon);
        EXPECT_NEAR(spring.Gravity.x, 0.5f, kFloatEpsilon);
        EXPECT_NEAR(spring.Gravity.y, -3.0f, kFloatEpsilon);
        EXPECT_NEAR(spring.Gravity.z, 1.25f, kFloatEpsilon);
        EXPECT_NEAR(spring.Weight, 0.625f, kFloatEpsilon);
    }

    TEST(ComponentRoundTrip, SpringBoneComponentNonFiniteFieldsAreSanitizedOnLoad)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& spring = entity.AddComponent<SpringBoneComponent>();
            spring.Stiffness = 33.5f;
            spring.Damping = 4.25f;
            spring.Weight = 0.625f;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        // Inject non-finite values into the SpringBoneComponent fields.
        yaml = std::regex_replace(yaml, std::regex(R"(Stiffness: [0-9.e+-]+)"), "Stiffness: .nan");
        yaml = std::regex_replace(yaml, std::regex(R"(Damping: [0-9.e+-]+)"), "Damping: .inf");
        yaml = std::regex_replace(yaml, std::regex(R"(Weight: [0-9.e+-]+)"), "Weight: -.inf");

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "Deserialize rejected the (structurally valid) NaN/Inf-injected scene.";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<SpringBoneComponent>());

        const auto& spring = restored.GetComponent<SpringBoneComponent>();
        EXPECT_TRUE(std::isfinite(spring.Stiffness));
        EXPECT_TRUE(std::isfinite(spring.Damping));
        EXPECT_TRUE(std::isfinite(spring.Weight));
        EXPECT_NEAR(spring.Stiffness, 80.0f, kFloatEpsilon) << "NaN stiffness should fall back to the default";
        EXPECT_NEAR(spring.Damping, 12.0f, kFloatEpsilon) << "Inf damping should fall back to the default";
        EXPECT_GE(spring.Weight, 0.0f);
        EXPECT_LE(spring.Weight, 1.0f);
    }

    // -------------------------------------------------------------------------
    // NoiseAnimationComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, NoiseAnimationComponentSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& noise = entity.AddComponent<NoiseAnimationComponent>();
            // Distinctive non-default values on every field.
            noise.Enabled = false;
            noise.EndBoneIndex = 9;
            noise.ChainLength = 4;
            noise.Frequency = 2.75f;
            noise.RotationAmplitude = glm::vec3(0.11f, 0.22f, 0.33f);
            noise.TranslationAmplitude = glm::vec3(0.01f, 0.02f, 0.03f);
            noise.Octaves = 4;
            noise.Lacunarity = 2.5f;
            noise.Gain = 0.4f;
            noise.Seed = 1234;
            noise.Weight = 0.625f;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty()) << "SerializeToYAML produced an empty string.";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "DeserializeFromYAML rejected the just-serialised scene.";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<NoiseAnimationComponent>());

        const auto& noise = restored.GetComponent<NoiseAnimationComponent>();
        EXPECT_FALSE(noise.Enabled);
        EXPECT_EQ(noise.EndBoneIndex, 9u);
        EXPECT_EQ(noise.ChainLength, 4u);
        EXPECT_NEAR(noise.Frequency, 2.75f, kFloatEpsilon);
        EXPECT_NEAR(noise.RotationAmplitude.x, 0.11f, kFloatEpsilon);
        EXPECT_NEAR(noise.RotationAmplitude.y, 0.22f, kFloatEpsilon);
        EXPECT_NEAR(noise.RotationAmplitude.z, 0.33f, kFloatEpsilon);
        EXPECT_NEAR(noise.TranslationAmplitude.x, 0.01f, kFloatEpsilon);
        EXPECT_NEAR(noise.TranslationAmplitude.y, 0.02f, kFloatEpsilon);
        EXPECT_NEAR(noise.TranslationAmplitude.z, 0.03f, kFloatEpsilon);
        EXPECT_EQ(noise.Octaves, 4u);
        EXPECT_NEAR(noise.Lacunarity, 2.5f, kFloatEpsilon);
        EXPECT_NEAR(noise.Gain, 0.4f, kFloatEpsilon);
        EXPECT_EQ(noise.Seed, 1234u);
        EXPECT_NEAR(noise.Weight, 0.625f, kFloatEpsilon);
    }

    TEST(ComponentRoundTrip, NoiseAnimationComponentNonFiniteFieldsAreSanitizedOnLoad)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& noise = entity.AddComponent<NoiseAnimationComponent>();
            noise.Frequency = 2.75f;
            noise.Gain = 0.4f;
            noise.Weight = 0.625f;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        // Inject non-finite values into the NoiseAnimationComponent fields.
        yaml = std::regex_replace(yaml, std::regex(R"(Frequency: [0-9.e+-]+)"), "Frequency: .nan");
        yaml = std::regex_replace(yaml, std::regex(R"(Gain: [0-9.e+-]+)"), "Gain: .inf");
        yaml = std::regex_replace(yaml, std::regex(R"(Weight: [0-9.e+-]+)"), "Weight: -.inf");

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "Deserialize rejected the (structurally valid) NaN/Inf-injected scene.";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<NoiseAnimationComponent>());

        const auto& noise = restored.GetComponent<NoiseAnimationComponent>();
        EXPECT_TRUE(std::isfinite(noise.Frequency));
        EXPECT_TRUE(std::isfinite(noise.Gain));
        EXPECT_TRUE(std::isfinite(noise.Weight));
        EXPECT_NEAR(noise.Frequency, 1.0f, kFloatEpsilon) << "NaN frequency should fall back to the default";
        EXPECT_NEAR(noise.Gain, 0.5f, kFloatEpsilon) << "Inf gain should fall back to the default";
        EXPECT_NEAR(noise.Weight, 1.0f, kFloatEpsilon) << "-Inf weight should fall back to the default";
    }

    // -------------------------------------------------------------------------
    // NavMeshBoundsComponent — off-mesh links (vector-of-structs YAML sequence).
    // Guards the SceneSerializer emit/read sides for the m_Links field added with
    // the off-mesh-link feature: a forgotten Start/End/Radius/Bidirectional key,
    // or a dropped sequence, vanishes silently from saved scenes otherwise.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, NavMeshBoundsOffMeshLinksSurviveYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& nmb = entity.AddComponent<NavMeshBoundsComponent>();
            nmb.m_Min = { -7.0f, -2.0f, -7.0f };
            nmb.m_Max = { 7.0f, 12.0f, 7.0f };
            nmb.m_Links.emplace_back(glm::vec3{ -3.0f, 0.25f, 1.0f }, glm::vec3{ 3.5f, 0.5f, -1.5f },
                                     /*radius=*/0.8f, /*bidirectional=*/false);
            nmb.m_Links.emplace_back(glm::vec3{ 1.0f, 0.0f, 2.0f }, glm::vec3{ -1.0f, 1.0f, -2.0f },
                                     /*radius=*/1.25f, /*bidirectional=*/true);
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<NavMeshBoundsComponent>())
            << "NavMeshBoundsComponent was dropped during round-trip.";

        const auto& nmb = restored.GetComponent<NavMeshBoundsComponent>();
        ASSERT_EQ(nmb.m_Links.size(), 2u) << "off-mesh link list lost entries during round-trip.";

        EXPECT_NEAR(nmb.m_Links[0].m_Start.x, -3.0f, kFloatEpsilon);
        EXPECT_NEAR(nmb.m_Links[0].m_End.z, -1.5f, kFloatEpsilon);
        EXPECT_NEAR(nmb.m_Links[0].m_Radius, 0.8f, kFloatEpsilon);
        EXPECT_FALSE(nmb.m_Links[0].m_Bidirectional);

        EXPECT_NEAR(nmb.m_Links[1].m_Start.z, 2.0f, kFloatEpsilon);
        EXPECT_NEAR(nmb.m_Links[1].m_Radius, 1.25f, kFloatEpsilon);
        EXPECT_TRUE(nmb.m_Links[1].m_Bidirectional);
    }

    // -------------------------------------------------------------------------
    // The components below are serialized by the OloHeaderTool-generated
    // Scene{Serialize,Deserialize}Components.Generated.inl (issue #380) rather
    // than a hand-written block. Their round-trips exercise the generated
    // string / i32 / mixed-vec read paths that the lighting tests above (vec3 /
    // f32 / bool) don't, so a regression in the codegen's per-type emit fails here.
    // -------------------------------------------------------------------------

    // LuaScriptComponent — the lone std::string field. Guards the generated
    // `.as<std::string>(default)` deserialize path.
    TEST(ComponentRoundTrip, LuaScriptComponentSurvivesYAMLRoundTrip)
    {
        const std::string expectedScriptFile = "Scripts/Enemies/patrol_guard.lua";

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            entity.AddComponent<LuaScriptComponent>().ScriptFile = expectedScriptFile;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<LuaScriptComponent>())
            << "LuaScriptComponent dropped during round-trip.";
        EXPECT_EQ(restored.GetComponent<LuaScriptComponent>().ScriptFile, expectedScriptFile);
    }

    // PerceptibleComponent — an i32 + a bool. Guards the generated
    // `.as<i32>(default)` read path (distinct from the u32 LayerID path).
    TEST(ComponentRoundTrip, PerceptibleComponentSurvivesYAMLRoundTrip)
    {
        const i32 expectedTeam = -7;              // non-default, negative to catch sign loss
        const bool expectedIsPerceptible = false; // non-default

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& perc = entity.AddComponent<PerceptibleComponent>();
            perc.Team = expectedTeam;
            perc.IsPerceptible = expectedIsPerceptible;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<PerceptibleComponent>())
            << "PerceptibleComponent dropped during round-trip.";

        const auto& perc = restored.GetComponent<PerceptibleComponent>();
        EXPECT_EQ(perc.Team, expectedTeam);
        EXPECT_EQ(perc.IsPerceptible, expectedIsPerceptible);
    }

    // NameplateComponent — the widest generated component: bools, a vec2, a vec3,
    // three vec4 colours and an f32. A per-type emit bug (wrong key, dropped
    // field, vec arity) surfaces here.
    TEST(ComponentRoundTrip, NameplateComponentSurvivesYAMLRoundTrip)
    {
        const glm::vec3 expectedWorldOffset{ 0.25f, 3.5f, -1.5f };
        const glm::vec2 expectedBarSize{ 200.0f, 18.0f };
        const glm::vec4 expectedHealthColor{ 0.9f, 0.1f, 0.2f, 0.95f };
        const glm::vec4 expectedManaColor{ 0.15f, 0.35f, 0.85f, 0.9f };
        const glm::vec4 expectedBgColor{ 0.05f, 0.06f, 0.07f, 0.8f };
        const f32 expectedManaBarGap = 5.5f;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& nc = entity.AddComponent<NameplateComponent>();
            nc.m_Enabled = false;       // non-default
            nc.m_ShowHealthBar = false; // non-default
            nc.m_ShowManaBar = true;    // non-default
            nc.m_WorldOffset = expectedWorldOffset;
            nc.m_BarSize = expectedBarSize;
            nc.m_HealthBarColor = expectedHealthColor;
            nc.m_ManaBarColor = expectedManaColor;
            nc.m_BarBackgroundColor = expectedBgColor;
            nc.m_ManaBarGap = expectedManaBarGap;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<NameplateComponent>())
            << "NameplateComponent dropped during round-trip.";

        const auto& nc = restored.GetComponent<NameplateComponent>();
        EXPECT_FALSE(nc.m_Enabled);
        EXPECT_FALSE(nc.m_ShowHealthBar);
        EXPECT_TRUE(nc.m_ShowManaBar);
        EXPECT_NEAR(nc.m_WorldOffset.y, expectedWorldOffset.y, kFloatEpsilon);
        EXPECT_NEAR(nc.m_WorldOffset.z, expectedWorldOffset.z, kFloatEpsilon);
        EXPECT_NEAR(nc.m_BarSize.x, expectedBarSize.x, kFloatEpsilon);
        EXPECT_NEAR(nc.m_BarSize.y, expectedBarSize.y, kFloatEpsilon);
        EXPECT_NEAR(nc.m_HealthBarColor.r, expectedHealthColor.r, kFloatEpsilon);
        EXPECT_NEAR(nc.m_HealthBarColor.a, expectedHealthColor.a, kFloatEpsilon);
        EXPECT_NEAR(nc.m_ManaBarColor.b, expectedManaColor.b, kFloatEpsilon);
        EXPECT_NEAR(nc.m_BarBackgroundColor.g, expectedBgColor.g, kFloatEpsilon);
        EXPECT_NEAR(nc.m_ManaBarGap, expectedManaBarGap, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // AnimationStateComponent
    // -------------------------------------------------------------------------
    // A corrupted/hand-authored `CurrentTime: .nan` would otherwise flow through
    // AnimationSystem::Update's LoopTime (a NaN passes the `while` guards
    // unchanged) into SampleBonePosition/Rotation/Scale, NaN-propagating forward
    // kinematics across the whole skeleton (issue #541).
    TEST(ComponentRoundTrip, AnimationStateComponentInvalidTimingIsSanitizedOnLoad)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& anim = entity.AddComponent<AnimationStateComponent>();
            anim.m_CurrentTime = 1.5f;
            anim.m_BlendDuration = 0.2f;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        yaml = std::regex_replace(yaml, std::regex(R"(CurrentTime: [0-9.e+-]+)"), "CurrentTime: .nan");
        yaml = std::regex_replace(yaml, std::regex(R"(BlendDuration: [0-9.e+-]+)"), "BlendDuration: -.inf");

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml))
            << "Deserialize rejected the (structurally valid) NaN/Inf-injected scene.";

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<AnimationStateComponent>());

        const auto& anim = restored.GetComponent<AnimationStateComponent>();
        EXPECT_TRUE(std::isfinite(anim.m_CurrentTime)) << "NaN CurrentTime must not propagate into forward kinematics";
        EXPECT_TRUE(std::isfinite(anim.m_BlendDuration)) << "Inf BlendDuration should be sanitized to a finite value";
    }

    // -------------------------------------------------------------------------
    // ProgressionComponent (issue #635) — fully GENERATED serializer blocks.
    // This is the first real component exercising the generated
    // std::unordered_map<std::string, i32> scene-YAML path (AllocatedPoints,
    // emitted as a sorted-key YAML mapping) alongside the generated
    // std::unordered_set<std::string> path (UnlockedNodes, emitted as a
    // sorted sequence). PendingXP and RuntimeInitialized carry
    // OLO_SERIALIZE(Skip): they must never appear in the YAML and must come
    // back at their constructor defaults.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, ProgressionComponentSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& prog = entity.AddComponent<ProgressionComponent>();
            // Non-default values for every SERIALIZED field, inside the load
            // clamps (Level >= 1, the point pools / XP / bounty >= 0).
            prog.Level = 7;
            prog.CurrentXP = 350;
            prog.AttributePoints = 12;
            prog.SkillPoints = 4;
            prog.XPBounty = 250;
            prog.HealOnLevelUp = false;
            prog.UnlockedNodes = { "toughness", "power_strike", "battle_focus" };
            prog.AllocatedPoints["AttackPower"] = 3;
            prog.AllocatedPoints["Defense"] = 2;
            prog.ExperienceCurveHandle = AssetHandle{ 111 };
            prog.ClassDatabaseHandle = AssetHandle{ 222 };
            prog.SkillTreeHandle = AssetHandle{ 333 };
            prog.ClassID = "warrior";
            // Runtime-only fields, set non-default to prove the Skip:
            prog.PendingXP = 999;
            prog.RuntimeInitialized = true;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());
        EXPECT_EQ(yaml.find("PendingXP"), std::string::npos)
            << "PendingXP carries OLO_SERIALIZE(Skip) — it must not be emitted into scene YAML.";
        EXPECT_EQ(yaml.find("RuntimeInitialized"), std::string::npos)
            << "RuntimeInitialized carries OLO_SERIALIZE(Skip) — it must not be emitted into scene YAML.";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<ProgressionComponent>())
            << "ProgressionComponent was dropped during round-trip.";

        const auto& prog = restored.GetComponent<ProgressionComponent>();
        EXPECT_EQ(prog.Level, 7);
        EXPECT_EQ(prog.CurrentXP, 350);
        EXPECT_EQ(prog.AttributePoints, 12);
        EXPECT_EQ(prog.SkillPoints, 4);
        EXPECT_EQ(prog.XPBounty, 250);
        EXPECT_FALSE(prog.HealOnLevelUp);

        EXPECT_EQ(prog.UnlockedNodes.size(), 3u)
            << "the generated unordered_set sequence lost entries.";
        EXPECT_TRUE(prog.UnlockedNodes.contains("toughness"));
        EXPECT_TRUE(prog.UnlockedNodes.contains("power_strike"));
        EXPECT_TRUE(prog.UnlockedNodes.contains("battle_focus"));

        ASSERT_EQ(prog.AllocatedPoints.size(), 2u)
            << "the generated string-keyed unordered_map lost entries.";
        ASSERT_TRUE(prog.AllocatedPoints.contains("AttackPower"));
        EXPECT_EQ(prog.AllocatedPoints.at("AttackPower"), 3);
        ASSERT_TRUE(prog.AllocatedPoints.contains("Defense"));
        EXPECT_EQ(prog.AllocatedPoints.at("Defense"), 2);

        EXPECT_EQ(static_cast<u64>(prog.ExperienceCurveHandle), 111u);
        EXPECT_EQ(static_cast<u64>(prog.ClassDatabaseHandle), 222u);
        EXPECT_EQ(static_cast<u64>(prog.SkillTreeHandle), 333u);
        EXPECT_EQ(prog.ClassID, "warrior");

        EXPECT_EQ(prog.PendingXP, 0)
            << "PendingXP must come back at its constructor default (0) after a scene load.";
        EXPECT_FALSE(prog.RuntimeInitialized)
            << "RuntimeInitialized must come back false so the first tick re-applies runtime state.";
    }

    // The generated writer must emit the unordered containers DETERMINISTICALLY
    // (sorted set sequence, sorted map keys): serialize -> load -> serialize
    // must be byte-identical or two saves of the same scene would diff.
    TEST(ComponentRoundTrip, ProgressionComponentSerializeLoadSerializeIsStable)
    {
        auto scene = Scene::Create();
        Entity entity = scene->CreateEntity(kTestTag);
        auto& prog = entity.AddComponent<ProgressionComponent>();
        prog.UnlockedNodes = { "zeta", "alpha", "mid" };
        prog.AllocatedPoints["Strength"] = 1;
        prog.AllocatedPoints["Agility"] = 2;
        prog.AllocatedPoints["Wisdom"] = 3;

        const std::string first = SceneSerializer(scene).SerializeToYAML();

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(first));
        const std::string second = SceneSerializer(reloaded).SerializeToYAML();

        EXPECT_EQ(first, second)
            << "serialize -> load -> serialize must produce identical YAML — the unordered "
               "set/map emission is not deterministic (missing sort before emit?).";
    }

    // =========================================================================
    // YAML converters for the glm integer-vector / quaternion / matrix types
    // (#451 glm slice). No shipping component is all-trivial-plus-glm-math (the
    // only candidates — TransformComponent / LightProbeVolumeComponent — are kept
    // hand-written), so the GENERATED emit path for these types isn't exercised by
    // a component round-trip. These tests cover the converters the generated code
    // would call, so the machinery is not dead/untested: each value must survive an
    // Encode → string → parse → Decode round-trip, and Decode must reject NaN/Inf
    // for the float-backed types (quat / mat) per the finiteness mandate.
    // =========================================================================
    namespace
    {
        // Emit a single value as YAML via the Emitter<< overload, parse it back, and
        // Decode through convert<T>. Mirrors how the generated serializer writes
        // (out << value) and reads (node.as<T>()).
        template<typename T>
        T GlmYamlRoundTrip(const T& value)
        {
            YAML::Emitter out;
            out << value;
            return YAML::Load(out.c_str()).as<T>();
        }
    } // namespace

    TEST(YAMLConverters, GlmQuatSurvivesRoundTrip)
    {
        const glm::quat q = glm::normalize(glm::quat(0.5f, -0.25f, 0.75f, 0.1f));
        const glm::quat r = GlmYamlRoundTrip(q);
        EXPECT_NEAR(r.w, q.w, kFloatEpsilon);
        EXPECT_NEAR(r.x, q.x, kFloatEpsilon);
        EXPECT_NEAR(r.y, q.y, kFloatEpsilon);
        EXPECT_NEAR(r.z, q.z, kFloatEpsilon);
    }

    TEST(YAMLConverters, GlmIVec2SurvivesRoundTrip)
    {
        const glm::ivec2 v{ -7, 123456 };
        const glm::ivec2 r = GlmYamlRoundTrip(v);
        EXPECT_EQ(r, v);
    }

    TEST(YAMLConverters, GlmIVec4SurvivesRoundTrip)
    {
        const glm::ivec4 v{ -7, 0, 123456, -987654 };
        const glm::ivec4 r = GlmYamlRoundTrip(v);
        EXPECT_EQ(r, v);
    }

    TEST(YAMLConverters, GlmMat3SurvivesRoundTrip)
    {
        glm::mat3 m{ 0.0f };
        for (i32 i = 0; i < 3; ++i)
            for (i32 j = 0; j < 3; ++j)
                m[i][j] = static_cast<f32>(i * 3 + j) + 0.5f;
        const glm::mat3 r = GlmYamlRoundTrip(m);
        for (i32 i = 0; i < 3; ++i)
            for (i32 j = 0; j < 3; ++j)
                EXPECT_NEAR(r[i][j], m[i][j], kFloatEpsilon);
    }

    TEST(YAMLConverters, GlmMat4SurvivesRoundTrip)
    {
        glm::mat4 m{ 0.0f };
        for (i32 i = 0; i < 4; ++i)
            for (i32 j = 0; j < 4; ++j)
                m[i][j] = static_cast<f32>(i * 4 + j) - 3.25f;
        const glm::mat4 r = GlmYamlRoundTrip(m);
        for (i32 i = 0; i < 4; ++i)
            for (i32 j = 0; j < 4; ++j)
                EXPECT_NEAR(r[i][j], m[i][j], kFloatEpsilon);
    }

    // Decode must fail-closed on a non-finite float component (per the finiteness
    // mandate) so a corrupt scene value keeps the field's prior/default value via
    // the .as<T>(fallback) overload rather than poisoning it with NaN/Inf.
    TEST(YAMLConverters, GlmQuatDecodeRejectsNonFinite)
    {
        const glm::quat fallback(1.0f, 0.0f, 0.0f, 0.0f);
        const YAML::Node node = YAML::Load("[.nan, 0.0, 0.0, 1.0]");
        const glm::quat r = node.as<glm::quat>(fallback);
        EXPECT_EQ(r.w, fallback.w);
        EXPECT_EQ(r.x, fallback.x);
    }

    TEST(YAMLConverters, GlmMat4DecodeRejectsNonFinite)
    {
        glm::mat4 fallback{ 1.0f };
        YAML::Node node;
        node.SetStyle(YAML::EmitterStyle::Flow);
        for (i32 k = 0; k < 16; ++k)
            node.push_back(k == 5 ? std::string(".inf") : std::string("0.0"));
        const glm::mat4 r = node.as<glm::mat4>(fallback);
        EXPECT_NEAR(r[0][0], fallback[0][0], kFloatEpsilon); // unchanged → fallback used
    }

    // -------------------------------------------------------------------------
    // PlayerRigComponent / CameraRigComponent (issue #645)
    //
    // Both are fully OloHeaderTool-generated: every authored field is trivial,
    // and the per-tick runtime fields carry OLO_SERIALIZE(Skip). That makes the
    // round-trip a two-sided check — the authored tuning must survive, and the
    // runtime state must NOT be in the file at all. The second half is the one
    // that matters: nothing else notices if a per-tick field starts leaking
    // into scene YAML, it just quietly begins persisting a held key or a
    // mid-pull-in boom length into the saved scene.
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, PlayerRigComponentSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            PlayerRigComponent rig;
            rig.m_LookSensitivity = 0.275f;
            rig.m_InvertLookY = true;
            rig.m_MinPitchDeg = -63.5f;
            rig.m_MaxPitchDeg = 71.25f;
            rig.m_WalkSpeed = 6.125f;
            rig.m_SprintMultiplier = 2.375f;
            rig.m_AirControl = 0.625f;
            rig.m_MoveRelativeToLook = false;
            rig.m_YawBodyWithLook = false;
            rig.m_FaceMoveDirection = true;
            rig.m_TurnRateDeg = 512.5f;
            rig.m_UseDeviceInput = false;
            rig.m_CaptureCursor = false;
            rig.m_YawDeg = 123.75f;
            rig.m_PitchDeg = -22.5f;

            // Runtime state, deliberately non-default so a leak is visible.
            rig.m_MoveInput = { 0.5f, -0.5f };
            rig.m_LookInput = { 9.0f, -9.0f };
            rig.m_SprintInput = true;
            rig.m_JumpInput = true;
            rig.m_PlanarSpeed = 4.25f;
            rig.m_Grounded = true;

            entity.AddComponent<PlayerRigComponent>(rig);
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        // The Skip-tagged runtime fields must not appear anywhere in the file.
        for (const char* runtimeKey : { "MoveInput", "LookInput", "SprintInput", "JumpInput",
                                        "LastMousePos", "HasLastMousePos", "PlanarSpeed", "Grounded" })
        {
            EXPECT_EQ(yaml.find(runtimeKey), std::string::npos)
                << "per-tick runtime field '" << runtimeKey
                << "' leaked into scene YAML — it needs OLO_SERIALIZE(Skip)";
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<PlayerRigComponent>());

        const auto& rig = restored.GetComponent<PlayerRigComponent>();
        EXPECT_NEAR(rig.m_LookSensitivity, 0.275f, kFloatEpsilon);
        EXPECT_TRUE(rig.m_InvertLookY);
        EXPECT_NEAR(rig.m_MinPitchDeg, -63.5f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_MaxPitchDeg, 71.25f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_WalkSpeed, 6.125f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_SprintMultiplier, 2.375f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_AirControl, 0.625f, kFloatEpsilon);
        EXPECT_FALSE(rig.m_MoveRelativeToLook);
        EXPECT_FALSE(rig.m_YawBodyWithLook);
        EXPECT_TRUE(rig.m_FaceMoveDirection);
        EXPECT_NEAR(rig.m_TurnRateDeg, 512.5f, kFloatEpsilon);
        EXPECT_FALSE(rig.m_UseDeviceInput);
        EXPECT_FALSE(rig.m_CaptureCursor);
        // Look angles ARE authored state — a scene sets the initial facing, and
        // a reloaded save must restore where the player was looking.
        EXPECT_NEAR(rig.m_YawDeg, 123.75f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_PitchDeg, -22.5f, kFloatEpsilon);

        // Runtime state comes back at its constructor default, not the value
        // that was live when the scene was saved.
        EXPECT_NEAR(rig.m_MoveInput.x, 0.0f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_LookInput.y, 0.0f, kFloatEpsilon);
        EXPECT_FALSE(rig.m_SprintInput);
        EXPECT_FALSE(rig.m_JumpInput);
        EXPECT_FALSE(rig.m_HasLastMousePos);
        EXPECT_NEAR(rig.m_PlanarSpeed, 0.0f, kFloatEpsilon);
        EXPECT_FALSE(rig.m_Grounded);
    }

    TEST(ComponentRoundTrip, CameraRigComponentSurvivesYAMLRoundTrip)
    {
        constexpr u64 kTargetId = 0x0123456789ABCDEFULL;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            CameraRigComponent rig;
            rig.m_Target = UUID(kTargetId);
            rig.m_PivotOffset = { 0.375f, 1.625f, -0.25f };
            rig.m_BoomLength = 5.5f;
            rig.m_CollisionEnabled = false;
            rig.m_ProbeRadius = 0.375f;
            rig.m_MinBoomLength = 0.875f;
            rig.m_BoomReturnSpeed = 8.25f;
            rig.m_PositionSmoothTime = 0.125f;
            rig.m_HeadBobAmplitude = 0.0625f;
            rig.m_HeadBobFrequency = 2.25f;
            rig.m_FallbackPitchDeg = -33.5f;

            rig.m_CurrentBoomLength = 2.0f;
            rig.m_SmoothedPosition = { 9.0f, 9.0f, 9.0f };
            rig.m_BobPhase = 1.5f;
            rig.m_PrevTargetPosition = { 3.0f, 3.0f, 3.0f };
            rig.m_Initialized = true;

            entity.AddComponent<CameraRigComponent>(rig);
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        for (const char* runtimeKey : { "CurrentBoomLength", "SmoothedPosition", "BobPhase",
                                        "PrevTargetPosition", "Initialized" })
        {
            EXPECT_EQ(yaml.find(runtimeKey), std::string::npos)
                << "per-tick runtime field '" << runtimeKey
                << "' leaked into scene YAML — it needs OLO_SERIALIZE(Skip)";
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<CameraRigComponent>());

        const auto& rig = restored.GetComponent<CameraRigComponent>();
        // The target reference is a UUID: losing it silently turns a follow
        // camera into a static one on load.
        EXPECT_EQ(static_cast<u64>(rig.m_Target), kTargetId);
        EXPECT_NEAR(rig.m_PivotOffset.x, 0.375f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_PivotOffset.y, 1.625f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_PivotOffset.z, -0.25f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_BoomLength, 5.5f, kFloatEpsilon);
        EXPECT_FALSE(rig.m_CollisionEnabled);
        EXPECT_NEAR(rig.m_ProbeRadius, 0.375f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_MinBoomLength, 0.875f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_BoomReturnSpeed, 8.25f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_PositionSmoothTime, 0.125f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_HeadBobAmplitude, 0.0625f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_HeadBobFrequency, 2.25f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_FallbackPitchDeg, -33.5f, kFloatEpsilon);

        EXPECT_NEAR(rig.m_CurrentBoomLength, 0.0f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_BobPhase, 0.0f, kFloatEpsilon);
        EXPECT_FALSE(rig.m_Initialized);
    }

    // The generated deserialize gets its bounds from the OLO_SERIALIZE(Clamp)
    // annotations; a corrupt scene must land in range rather than producing a
    // camera that can never see anything (a pitch past the pole would make the
    // look basis degenerate).
    TEST(ComponentRoundTrip, PlayerRigOutOfRangeValuesAreClampedOnLoad)
    {
        auto scene = Scene::Create();
        const std::string yaml =
            "Scene: Untitled\n"
            "Entities:\n"
            "  - Entity: 12345\n"
            "    TagComponent:\n"
            "      Tag: " +
            std::string(kTestTag) + "\n"
                                    "    TransformComponent:\n"
                                    "      Translation: [0, 0, 0]\n"
                                    "      Rotation: [0, 0, 0]\n"
                                    "      Scale: [1, 1, 1]\n"
                                    "    PlayerRigComponent:\n"
                                    "      LookSensitivity: -5\n"
                                    "      MaxPitchDeg: 500\n"
                                    "      MinPitchDeg: -500\n"
                                    "      AirControl: 9\n"
                                    "      SprintMultiplier: 0.1\n"
                                    "      PitchDeg: 720\n"
                                    "      WalkSpeed: .nan\n";

        ASSERT_TRUE(SceneSerializer(scene).DeserializeFromYAML(yaml));
        Entity restored = FindByTag(*scene, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<PlayerRigComponent>());

        const auto& rig = restored.GetComponent<PlayerRigComponent>();
        EXPECT_NEAR(rig.m_LookSensitivity, 0.0f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_MaxPitchDeg, 89.9f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_MinPitchDeg, -89.9f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_AirControl, 1.0f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_SprintMultiplier, 1.0f, kFloatEpsilon);
        EXPECT_NEAR(rig.m_PitchDeg, 89.9f, kFloatEpsilon);
        // A non-finite value keeps the constructor default (TryReadFiniteF32
        // rejects it before the clamp ever runs).
        EXPECT_NEAR(rig.m_WalkSpeed, PlayerRigComponent{}.m_WalkSpeed, kFloatEpsilon);
    }

    // -------------------------------------------------------------------------
    // TerrainComponent — the voxel mesher selector (issue #727)
    // -------------------------------------------------------------------------
    //
    // TerrainComponent's serializer is hand-written (it is in the generator's
    // kComponentsCustomSerialize set), so nothing derives its YAML from the
    // struct and a new field is exactly the silent-scene-data-loss case this
    // file exists for.
    TEST(ComponentRoundTrip, TerrainVoxelMesherSurvivesYAMLRoundTrip)
    {
        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& terrain = entity.AddComponent<TerrainComponent>();
            terrain.m_VoxelEnabled = true;
            terrain.m_VoxelSize = 2.0f;
            terrain.m_VoxelMesher = VoxelMesherKind::GreedyCubic;

            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<TerrainComponent>());

        const auto& terrain = restored.GetComponent<TerrainComponent>();
        EXPECT_TRUE(terrain.m_VoxelEnabled);
        EXPECT_NEAR(terrain.m_VoxelSize, 2.0f, kFloatEpsilon);
        EXPECT_EQ(terrain.m_VoxelMesher, VoxelMesherKind::GreedyCubic);
    }

    TEST(ComponentRoundTrip, TerrainVoxelMesherRejectsAnOutOfRangeValue)
    {
        // Reject, not clamp: a corrupt index must fall back to the DEFAULT
        // mesher, not saturate to the other valid one. Saturating would give a
        // corrupt scene a different, plausible, wrong silhouette — the same
        // reasoning behind VehicleComponent::m_DriveMode.
        auto scene = Scene::Create();
        const std::string yaml = std::string("Scene: Untitled\n") +
                                 "Entities:\n" +
                                 "  - Entity: 424242\n" +
                                 "    TagComponent:\n" +
                                 "      Tag: " + std::string(kTestTag) + "\n" +
                                 "    TransformComponent:\n" +
                                 "      Translation: [0, 0, 0]\n" +
                                 "      Rotation: [0, 0, 0]\n" +
                                 "      Scale: [1, 1, 1]\n" +
                                 "    TerrainComponent:\n" +
                                 "      VoxelEnabled: true\n" +
                                 "      VoxelMesher: 99\n";

        ASSERT_TRUE(SceneSerializer(scene).DeserializeFromYAML(yaml));
        Entity restored = FindByTag(*scene, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<TerrainComponent>());

        EXPECT_EQ(restored.GetComponent<TerrainComponent>().m_VoxelMesher, VoxelMesherKind::MarchingCubes);
    }

    // -------------------------------------------------------------------------
    // TerrainComponent — the OTHER two destinations a new field has to reach
    // -------------------------------------------------------------------------
    //
    // This file's breakage class is "added a field, forgot to wire it up", and
    // the serializer is only one of the places that wiring lands.
    // TerrainComponent hand-writes its copy constructor, its copy-assignment
    // and its operator== (it holds Ref<T> runtime state that must NOT be
    // copied, so it cannot use the defaulted forms), which makes each of them
    // a per-field list someone has to remember. Both failures are silent and
    // neither is a serializer bug:
    //
    //   * missing from the copy path -> Scene::Copy runs on every Play/Simulate
    //     entry and DuplicateEntity on every duplicate, so an authored value
    //     reverts to its default the moment you press Play. The scene file on
    //     disk is correct the whole time, which is what makes it baffling.
    //   * missing from operator==    -> SceneHierarchyPanel's DrawComponent<T>
    //     picks the equality tier for this type, so an inspector edit to that
    //     field is invisible to undo — no change is recorded at all.
    //
    // Both were live for all eleven virtual-texturing fields (issue #715
    // slices 1-4) until CodeRabbit caught it on the slice 3+4 PR.
    TEST(ComponentRoundTrip, TerrainVirtualTextureFieldsSurviveSceneCopy)
    {
        auto scene = Scene::Create();
        {
            Entity entity = scene->CreateEntity(kTestTag);
            auto& terrain = entity.AddComponent<TerrainComponent>();
            // Every one deliberately different from its default, so a field
            // dropped from the copy list shows up as its default below.
            terrain.m_VirtualTextureEnabled = true;
            terrain.m_VTVirtualPagesWide = 512;
            terrain.m_VTPageTexels = 64;
            terrain.m_VTBorderTexels = 2;
            terrain.m_VTCacheTilesWide = 32;
            terrain.m_VTMaxTileBakesPerFrame = 4;
            terrain.m_VTAdaptiveEnabled = false;
            terrain.m_VTSectorsWide = 4;
            terrain.m_VTMaxImagePagesWide = 32;
            terrain.m_VTTrilinearEnabled = false;
            terrain.m_VTCompressedCache = false;
        }

        // The exact call Scene::OnRuntimeStart makes.
        Ref<Scene> copy = Scene::Copy(scene);
        ASSERT_TRUE(static_cast<bool>(copy));

        Entity copied = FindByTag(*copy, kTestTag);
        ASSERT_TRUE(static_cast<bool>(copied));
        ASSERT_TRUE(copied.HasComponent<TerrainComponent>());

        const auto& terrain = copied.GetComponent<TerrainComponent>();
        EXPECT_TRUE(terrain.m_VirtualTextureEnabled);
        EXPECT_EQ(terrain.m_VTVirtualPagesWide, 512u);
        EXPECT_EQ(terrain.m_VTPageTexels, 64u);
        EXPECT_EQ(terrain.m_VTBorderTexels, 2u);
        EXPECT_EQ(terrain.m_VTCacheTilesWide, 32u);
        EXPECT_EQ(terrain.m_VTMaxTileBakesPerFrame, 4u);
        EXPECT_FALSE(terrain.m_VTAdaptiveEnabled);
        EXPECT_EQ(terrain.m_VTSectorsWide, 4u);
        EXPECT_EQ(terrain.m_VTMaxImagePagesWide, 32u);
        EXPECT_FALSE(terrain.m_VTTrilinearEnabled);
        EXPECT_FALSE(terrain.m_VTCompressedCache);
    }

    TEST(ComponentRoundTrip, TerrainVirtualTextureFieldsAreVisibleToUndoEquality)
    {
        // One mutation per field, each asserted to break equality. A field
        // missing from operator== passes every other test in this file and
        // simply cannot be undone in the editor.
        const auto differsInEveryWay = [](auto&& mutate)
        {
            TerrainComponent original;
            TerrainComponent edited = original;
            EXPECT_TRUE(original == edited) << "a plain copy must compare equal";
            mutate(edited);
            EXPECT_FALSE(original == edited);
        };

        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VirtualTextureEnabled = !c.m_VirtualTextureEnabled; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTVirtualPagesWide += 1u; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTPageTexels += 1u; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTBorderTexels += 1u; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTCacheTilesWide += 1u; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTMaxTileBakesPerFrame += 1u; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTAdaptiveEnabled = !c.m_VTAdaptiveEnabled; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTSectorsWide += 1u; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTMaxImagePagesWide += 1u; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTTrilinearEnabled = !c.m_VTTrilinearEnabled; });
        differsInEveryWay([](TerrainComponent& c)
                          { c.m_VTCompressedCache = !c.m_VTCompressedCache; });
    }

} // namespace OloEngine::Tests
