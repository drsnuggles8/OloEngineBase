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

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <regex>
#include <string>

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

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& rb = entity.AddComponent<Rigidbody2DComponent>();
            rb.Type = expectedType;
            rb.FixedRotation = expectedFixedRotation;
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
    // UIButtonComponent — 4-state colour palette + Interactable.
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
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

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
    }

    // -------------------------------------------------------------------------
    // UISliderComponent
    // -------------------------------------------------------------------------
    TEST(ComponentRoundTrip, UISliderComponentSurvivesYAMLRoundTrip)
    {
        const f32 expectedValue = 0.625f;
        const f32 expectedMin = 0.0f;
        const f32 expectedMax = 100.0f;
        const bool expectedInteractable = false;

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& s = entity.AddComponent<UISliderComponent>();
            s.m_Value = expectedValue;
            s.m_MinValue = expectedMin;
            s.m_MaxValue = expectedMax;
            s.m_Interactable = expectedInteractable;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<UISliderComponent>());

        const auto& s = restored.GetComponent<UISliderComponent>();
        EXPECT_NEAR(s.m_Value, expectedValue, kFloatEpsilon);
        EXPECT_NEAR(s.m_MinValue, expectedMin, kFloatEpsilon);
        EXPECT_NEAR(s.m_MaxValue, expectedMax, kFloatEpsilon);
        EXPECT_EQ(s.m_Interactable, expectedInteractable);
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
        const i32 expectedSortOrder = 42;
        const glm::vec2 expectedReferenceResolution{ 1280.0f, 720.0f };

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& c = entity.AddComponent<UICanvasComponent>();
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
    // UIWorldAnchorComponent — UUID target + world-offset.
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

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity entity = scene->CreateEntity(kTestTag);
            auto& as = entity.AddComponent<AudioSourceComponent>();
            as.Config.VolumeMultiplier = expectedVolume;
            as.Config.PitchMultiplier = expectedPitch;
            as.Config.Looping = expectedLooping;
            as.Config.MinDistance = expectedMinDistance;
            as.Config.MaxDistance = expectedMaxDistance;
            yaml = SceneSerializer(scene).SerializeToYAML();
        }

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, kTestTag);
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<AudioSourceComponent>());

        const auto& as = restored.GetComponent<AudioSourceComponent>();
        EXPECT_NEAR(as.Config.VolumeMultiplier, expectedVolume, kFloatEpsilon);
        EXPECT_NEAR(as.Config.PitchMultiplier, expectedPitch, kFloatEpsilon);
        EXPECT_EQ(as.Config.Looping, expectedLooping);
        EXPECT_NEAR(as.Config.MinDistance, expectedMinDistance, kFloatEpsilon);
        EXPECT_NEAR(as.Config.MaxDistance, expectedMaxDistance, kFloatEpsilon);
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
                s.Config.VolumeMultiplier = 0.6f;
                s.Config.Looping = true;
                s.Config.MinDistance = 2.0f;
                s.Config.MaxDistance = 25.0f;
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
    }

    // -------------------------------------------------------------------------
    // BuoyancyComponent — every serialized field must survive a YAML round-trip
    // (one of the five component touch-points; a dropped read/write here would
    // silently desync a saved scene from its in-editor setup). See
    // docs/WATER_FUTURE_IMPROVEMENTS.md §5.1.
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
} // namespace OloEngine::Tests
