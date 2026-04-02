#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

#include <glm/glm.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Lua Binding Test Fixture
// =============================================================================
// Mirrors the subset of LuaScriptGlue::RegisterAllTypes() needed for
// component property round-trip verification. No rendering or Mono required.

class LuaBindingTest : public ::testing::Test
{
  protected:
    sol::state lua;

    void SetUp() override
    {
        lua.open_libraries(sol::lib::base, sol::lib::math);

        // --- GLM vector types ---
        lua.new_usertype<glm::vec2>("vec2",
                                    sol::constructors<glm::vec2(), glm::vec2(float), glm::vec2(float, float)>(),
                                    "x", &glm::vec2::x,
                                    "y", &glm::vec2::y);

        lua.new_usertype<glm::vec3>("vec3",
                                    sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(),
                                    "x", &glm::vec3::x,
                                    "y", &glm::vec3::y,
                                    "z", &glm::vec3::z);

        lua.new_usertype<glm::vec4>("vec4",
                                    sol::constructors<glm::vec4(), glm::vec4(float), glm::vec4(float, float, float, float)>(),
                                    "x", &glm::vec4::x,
                                    "y", &glm::vec4::y,
                                    "z", &glm::vec4::z,
                                    "w", &glm::vec4::w);

        // --- TransformComponent ---
        lua.new_usertype<TransformComponent>("TransformComponent",
                                             "translation", &TransformComponent::Translation,
                                             "scale", &TransformComponent::Scale,
                                             "rotation", sol::property(&TransformComponent::GetRotationEuler, &TransformComponent::SetRotationEuler));

        // --- Rigidbody2DComponent ---
        lua.new_usertype<Rigidbody2DComponent>("Rigidbody2DComponent",
                                               "type", &Rigidbody2DComponent::Type,
                                               "fixedRotation", &Rigidbody2DComponent::FixedRotation,
                                               "linearVelocity", &Rigidbody2DComponent::LinearVelocity,
                                               "angularVelocity", &Rigidbody2DComponent::AngularVelocity);

        // --- BoxCollider2DComponent ---
        lua.new_usertype<BoxCollider2DComponent>("BoxCollider2DComponent",
                                                 "offset", &BoxCollider2DComponent::Offset,
                                                 "size", &BoxCollider2DComponent::Size,
                                                 "density", &BoxCollider2DComponent::Density,
                                                 "friction", &BoxCollider2DComponent::Friction,
                                                 "restitution", &BoxCollider2DComponent::Restitution,
                                                 "restitutionThreshold", &BoxCollider2DComponent::RestitutionThreshold);

        // --- CircleCollider2DComponent ---
        lua.new_usertype<CircleCollider2DComponent>("CircleCollider2DComponent",
                                                    "offset", &CircleCollider2DComponent::Offset,
                                                    "radius", &CircleCollider2DComponent::Radius,
                                                    "density", &CircleCollider2DComponent::Density,
                                                    "friction", &CircleCollider2DComponent::Friction,
                                                    "restitution", &CircleCollider2DComponent::Restitution,
                                                    "restitutionThreshold", &CircleCollider2DComponent::RestitutionThreshold);

        // --- SceneCamera ---
        lua.new_usertype<SceneCamera>("SceneCamera",
                                      "projectionType", sol::property(&SceneCamera::GetProjectionType, &SceneCamera::SetProjectionType),
                                      "perspectiveFOV", sol::property(&SceneCamera::GetPerspectiveVerticalFOV, &SceneCamera::SetPerspectiveVerticalFOV),
                                      "perspectiveNearClip", sol::property(&SceneCamera::GetPerspectiveNearClip, &SceneCamera::SetPerspectiveNearClip),
                                      "perspectiveFarClip", sol::property(&SceneCamera::GetPerspectiveFarClip, &SceneCamera::SetPerspectiveFarClip),
                                      "orthographicSize", sol::property(&SceneCamera::GetOrthographicSize, &SceneCamera::SetOrthographicSize),
                                      "orthographicNearClip", sol::property(&SceneCamera::GetOrthographicNearClip, &SceneCamera::SetOrthographicNearClip),
                                      "orthographicFarClip", sol::property(&SceneCamera::GetOrthographicFarClip, &SceneCamera::SetOrthographicFarClip));

        // --- CameraComponent ---
        lua.new_usertype<CameraComponent>("CameraComponent",
                                          "camera", &CameraComponent::Camera,
                                          "primary", &CameraComponent::Primary,
                                          "fixedAspectRatio", &CameraComponent::FixedAspectRatio);

        // --- SpriteRendererComponent ---
        lua.new_usertype<SpriteRendererComponent>("SpriteRendererComponent",
                                                  "color", &SpriteRendererComponent::Color,
                                                  "tilingFactor", &SpriteRendererComponent::TilingFactor);

        // --- CircleRendererComponent ---
        lua.new_usertype<CircleRendererComponent>("CircleRendererComponent",
                                                  "color", &CircleRendererComponent::Color,
                                                  "thickness", &CircleRendererComponent::Thickness,
                                                  "fade", &CircleRendererComponent::Fade);

        // --- TextComponent ---
        lua.new_usertype<TextComponent>("TextComponent",
                                        "text", &TextComponent::TextString,
                                        "color", &TextComponent::Color,
                                        "kerning", &TextComponent::Kerning,
                                        "lineSpacing", &TextComponent::LineSpacing,
                                        "maxWidth", &TextComponent::MaxWidth,
                                        "dropShadow", &TextComponent::DropShadow,
                                        "shadowDistance", &TextComponent::ShadowDistance,
                                        "shadowColor", &TextComponent::ShadowColor);

        // --- MeshComponent ---
        lua.new_usertype<MeshComponent>("MeshComponent",
                                        "primitive", &MeshComponent::m_Primitive);
    }
};

// =============================================================================
// TransformComponent
// =============================================================================

TEST_F(LuaBindingTest, TransformComponent_TranslationRoundTrip)
{
    TransformComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.translation = vec3.new(1.0, 2.0, 3.0)");
    EXPECT_FLOAT_EQ(tc.Translation.x, 1.0f);
    EXPECT_FLOAT_EQ(tc.Translation.y, 2.0f);
    EXPECT_FLOAT_EQ(tc.Translation.z, 3.0f);

    auto result = lua.script("return tc.translation.x, tc.translation.y, tc.translation.z");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 1.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 2.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(2), 3.0f);
}

TEST_F(LuaBindingTest, TransformComponent_ScaleRoundTrip)
{
    TransformComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.scale = vec3.new(2.0, 3.0, 4.0)");
    EXPECT_FLOAT_EQ(tc.Scale.x, 2.0f);
    EXPECT_FLOAT_EQ(tc.Scale.y, 3.0f);
    EXPECT_FLOAT_EQ(tc.Scale.z, 4.0f);
}

TEST_F(LuaBindingTest, TransformComponent_RotationRoundTrip)
{
    TransformComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.rotation = vec3.new(0.1, 0.2, 0.3)");
    auto euler = tc.GetRotationEuler();
    EXPECT_NEAR(euler.x, 0.1f, 1e-5f);
    EXPECT_NEAR(euler.y, 0.2f, 1e-5f);
    EXPECT_NEAR(euler.z, 0.3f, 1e-5f);
}

// =============================================================================
// Rigidbody2DComponent
// =============================================================================

TEST_F(LuaBindingTest, Rigidbody2D_PropertyRoundTrip)
{
    Rigidbody2DComponent rb;
    lua["rb"] = &rb;

    lua.script("rb.fixedRotation = true");
    EXPECT_TRUE(rb.FixedRotation);

    lua.script("rb.angularVelocity = 5.0");
    EXPECT_FLOAT_EQ(rb.AngularVelocity, 5.0f);

    lua.script("rb.linearVelocity = vec2.new(1.0, 2.0)");
    EXPECT_FLOAT_EQ(rb.LinearVelocity.x, 1.0f);
    EXPECT_FLOAT_EQ(rb.LinearVelocity.y, 2.0f);
}

// =============================================================================
// BoxCollider2DComponent
// =============================================================================

TEST_F(LuaBindingTest, BoxCollider2D_PropertyRoundTrip)
{
    BoxCollider2DComponent bc;
    lua["bc"] = &bc;

    lua.script("bc.offset = vec2.new(0.5, 0.5)");
    EXPECT_FLOAT_EQ(bc.Offset.x, 0.5f);
    EXPECT_FLOAT_EQ(bc.Offset.y, 0.5f);

    lua.script("bc.size = vec2.new(1.0, 2.0)");
    EXPECT_FLOAT_EQ(bc.Size.x, 1.0f);
    EXPECT_FLOAT_EQ(bc.Size.y, 2.0f);

    lua.script("bc.density = 2.0");
    EXPECT_FLOAT_EQ(bc.Density, 2.0f);

    lua.script("bc.friction = 0.3");
    EXPECT_FLOAT_EQ(bc.Friction, 0.3f);

    lua.script("bc.restitution = 0.7");
    EXPECT_FLOAT_EQ(bc.Restitution, 0.7f);

    lua.script("bc.restitutionThreshold = 1.0");
    EXPECT_FLOAT_EQ(bc.RestitutionThreshold, 1.0f);
}

// =============================================================================
// CircleCollider2DComponent
// =============================================================================

TEST_F(LuaBindingTest, CircleCollider2D_PropertyRoundTrip)
{
    CircleCollider2DComponent cc;
    lua["cc"] = &cc;

    lua.script("cc.radius = 2.0");
    EXPECT_FLOAT_EQ(cc.Radius, 2.0f);

    lua.script("cc.offset = vec2.new(0.1, 0.2)");
    EXPECT_FLOAT_EQ(cc.Offset.x, 0.1f);
    EXPECT_FLOAT_EQ(cc.Offset.y, 0.2f);

    lua.script("cc.density = 1.5");
    EXPECT_FLOAT_EQ(cc.Density, 1.5f);

    lua.script("cc.friction = 0.8");
    EXPECT_FLOAT_EQ(cc.Friction, 0.8f);
}

// =============================================================================
// CameraComponent + SceneCamera
// =============================================================================

TEST_F(LuaBindingTest, CameraComponent_PrimaryRoundTrip)
{
    CameraComponent cam;
    lua["cam"] = &cam;

    lua.script("cam.primary = false");
    EXPECT_FALSE(cam.Primary);

    lua.script("cam.primary = true");
    EXPECT_TRUE(cam.Primary);
}

TEST_F(LuaBindingTest, CameraComponent_SceneCameraProperties)
{
    CameraComponent cam;
    lua["cam"] = &cam;

    lua.script("cam.camera.perspectiveFOV = 1.0");
    EXPECT_FLOAT_EQ(cam.Camera.GetPerspectiveVerticalFOV(), 1.0f);

    lua.script("cam.camera.orthographicSize = 20.0");
    EXPECT_FLOAT_EQ(cam.Camera.GetOrthographicSize(), 20.0f);

    lua.script("cam.camera.perspectiveNearClip = 0.1");
    EXPECT_FLOAT_EQ(cam.Camera.GetPerspectiveNearClip(), 0.1f);

    lua.script("cam.camera.perspectiveFarClip = 500.0");
    EXPECT_FLOAT_EQ(cam.Camera.GetPerspectiveFarClip(), 500.0f);
}

// =============================================================================
// SpriteRendererComponent
// =============================================================================

TEST_F(LuaBindingTest, SpriteRenderer_PropertyRoundTrip)
{
    SpriteRendererComponent sr;
    lua["sr"] = &sr;

    lua.script("sr.color = vec4.new(0.5, 0.6, 0.7, 0.8)");
    EXPECT_FLOAT_EQ(sr.Color.r, 0.5f);
    EXPECT_FLOAT_EQ(sr.Color.g, 0.6f);
    EXPECT_FLOAT_EQ(sr.Color.b, 0.7f);
    EXPECT_FLOAT_EQ(sr.Color.a, 0.8f);

    lua.script("sr.tilingFactor = 3.0");
    EXPECT_FLOAT_EQ(sr.TilingFactor, 3.0f);
}

// =============================================================================
// CircleRendererComponent
// =============================================================================

TEST_F(LuaBindingTest, CircleRenderer_PropertyRoundTrip)
{
    CircleRendererComponent cr;
    lua["cr"] = &cr;

    lua.script("cr.color = vec4.new(1.0, 0.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(cr.Color.r, 1.0f);
    EXPECT_FLOAT_EQ(cr.Color.g, 0.0f);

    lua.script("cr.thickness = 0.5");
    EXPECT_FLOAT_EQ(cr.Thickness, 0.5f);

    lua.script("cr.fade = 0.01");
    EXPECT_FLOAT_EQ(cr.Fade, 0.01f);
}

// =============================================================================
// TextComponent
// =============================================================================

TEST_F(LuaBindingTest, TextComponent_StringRoundTrip)
{
    TextComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.text = 'Hello, World!'");
    EXPECT_EQ(tc.TextString, "Hello, World!");

    auto result = lua.script("return tc.text");
    EXPECT_EQ(result.get<std::string>(), "Hello, World!");
}

TEST_F(LuaBindingTest, TextComponent_PropertyRoundTrip)
{
    TextComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.color = vec4.new(0.0, 1.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(tc.Color.g, 1.0f);

    lua.script("tc.kerning = 1.5");
    EXPECT_FLOAT_EQ(tc.Kerning, 1.5f);

    lua.script("tc.lineSpacing = 2.0");
    EXPECT_FLOAT_EQ(tc.LineSpacing, 2.0f);

    lua.script("tc.maxWidth = 300.0");
    EXPECT_FLOAT_EQ(tc.MaxWidth, 300.0f);

    lua.script("tc.dropShadow = true");
    EXPECT_TRUE(tc.DropShadow);

    lua.script("tc.shadowDistance = 0.05");
    EXPECT_FLOAT_EQ(tc.ShadowDistance, 0.05f);
}

// =============================================================================
// MeshComponent
// =============================================================================

TEST_F(LuaBindingTest, MeshComponent_PrimitiveRoundTrip)
{
    MeshComponent mc;
    lua["mc"] = &mc;
    mc.m_Primitive = MeshPrimitive::Cube;

    auto result = lua.script("return mc.primitive");
    EXPECT_EQ(result.get<MeshPrimitive>(), MeshPrimitive::Cube);
}

// =============================================================================
// GLM vector constructors and access
// =============================================================================

TEST_F(LuaBindingTest, Vec3_ConstructAndAccess)
{
    auto result = lua.script("local v = vec3.new(1.0, 2.0, 3.0); return v.x, v.y, v.z");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 1.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 2.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(2), 3.0f);
}

TEST_F(LuaBindingTest, Vec2_ConstructAndAccess)
{
    auto result = lua.script("local v = vec2.new(4.0, 5.0); return v.x, v.y");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 4.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 5.0f);
}

TEST_F(LuaBindingTest, Vec4_ConstructAndAccess)
{
    auto result = lua.script("local v = vec4.new(1.0, 2.0, 3.0, 4.0); return v.x, v.y, v.z, v.w");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 1.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 2.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(2), 3.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(3), 4.0f);
}
