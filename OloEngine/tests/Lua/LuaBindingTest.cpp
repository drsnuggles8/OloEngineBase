#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Particle/ParticleEmitter.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Scripting/Lua/LuaScriptGlue.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Lua Binding Test Fixture
// =============================================================================
// Uses the real production bindings from LuaScriptGlue::RegisterAllTypes().
// Engine services (ScriptEngine, Application, AssetManager) are not initialised,
// so only component-property round-trips and table-existence checks are safe.
// Runtime-only lambdas (entity_utils helpers, Input, etc.) are registered but
// never invoked — they guard with null checks at call time.

class LuaBindingTest : public ::testing::Test
{
  protected:
    sol::state lua;

    void SetUp() override
    {
        lua.open_libraries(sol::lib::base, sol::lib::math);
        LuaScriptGlue::RegisterAllTypes(lua);
    }
};

// =============================================================================
// GLM vector constructors and access
// =============================================================================

TEST_F(LuaBindingTest, Vec2_ConstructAndAccess)
{
    auto result = lua.script("local v = vec2.new(4.0, 5.0); return v.x, v.y");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 4.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 5.0f);
}

TEST_F(LuaBindingTest, Vec3_ConstructAndAccess)
{
    auto result = lua.script("local v = vec3.new(1.0, 2.0, 3.0); return v.x, v.y, v.z");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 1.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 2.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(2), 3.0f);
}

TEST_F(LuaBindingTest, Vec4_ConstructAndAccess)
{
    auto result = lua.script("local v = vec4.new(1.0, 2.0, 3.0, 4.0); return v.x, v.y, v.z, v.w");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 1.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 2.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(2), 3.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(3), 4.0f);
}

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

    lua.script("bc.density = 2.0; bc.friction = 0.3; bc.restitution = 0.7; bc.restitutionThreshold = 1.0");
    EXPECT_FLOAT_EQ(bc.Density, 2.0f);
    EXPECT_FLOAT_EQ(bc.Friction, 0.3f);
    EXPECT_FLOAT_EQ(bc.Restitution, 0.7f);
    EXPECT_FLOAT_EQ(bc.RestitutionThreshold, 1.0f);
}

// =============================================================================
// CircleCollider2DComponent
// =============================================================================

TEST_F(LuaBindingTest, CircleCollider2D_PropertyRoundTrip)
{
    CircleCollider2DComponent cc;
    lua["cc"] = &cc;

    lua.script("cc.radius = 2.0; cc.offset = vec2.new(0.1, 0.2)");
    EXPECT_FLOAT_EQ(cc.Radius, 2.0f);
    EXPECT_FLOAT_EQ(cc.Offset.x, 0.1f);
    EXPECT_FLOAT_EQ(cc.Offset.y, 0.2f);

    lua.script("cc.density = 1.5; cc.friction = 0.8");
    EXPECT_FLOAT_EQ(cc.Density, 1.5f);
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

    lua.script("cam.camera.perspectiveNearClip = 0.1; cam.camera.perspectiveFarClip = 500.0");
    EXPECT_FLOAT_EQ(cam.Camera.GetPerspectiveNearClip(), 0.1f);
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

    lua.script("cr.thickness = 0.5; cr.fade = 0.01");
    EXPECT_FLOAT_EQ(cr.Thickness, 0.5f);
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

    lua.script("tc.kerning = 1.5; tc.lineSpacing = 2.0; tc.maxWidth = 300.0");
    EXPECT_FLOAT_EQ(tc.Kerning, 1.5f);
    EXPECT_FLOAT_EQ(tc.LineSpacing, 2.0f);
    EXPECT_FLOAT_EQ(tc.MaxWidth, 300.0f);

    lua.script("tc.dropShadow = true; tc.shadowDistance = 0.05");
    EXPECT_TRUE(tc.DropShadow);
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
// UI Components
// =============================================================================

TEST_F(LuaBindingTest, UICanvasComponent_PropertyRoundTrip)
{
    UICanvasComponent canvas;
    lua["c"] = &canvas;

    lua.script("c.sortOrder = 5");
    EXPECT_EQ(canvas.m_SortOrder, 5);

    lua.script("c.referenceResolution = vec2.new(1280.0, 720.0)");
    EXPECT_FLOAT_EQ(canvas.m_ReferenceResolution.x, 1280.0f);
    EXPECT_FLOAT_EQ(canvas.m_ReferenceResolution.y, 720.0f);
}

TEST_F(LuaBindingTest, UIRectTransformComponent_PropertyRoundTrip)
{
    UIRectTransformComponent rect;
    lua["r"] = &rect;

    lua.script("r.anchorMin = vec2.new(0.0, 0.0); r.anchorMax = vec2.new(1.0, 1.0)");
    EXPECT_FLOAT_EQ(rect.m_AnchorMin.x, 0.0f);
    EXPECT_FLOAT_EQ(rect.m_AnchorMax.x, 1.0f);
    EXPECT_FLOAT_EQ(rect.m_AnchorMax.y, 1.0f);

    lua.script("r.anchoredPosition = vec2.new(10.0, 20.0)");
    EXPECT_FLOAT_EQ(rect.m_AnchoredPosition.x, 10.0f);

    lua.script("r.sizeDelta = vec2.new(200.0, 50.0)");
    EXPECT_FLOAT_EQ(rect.m_SizeDelta.x, 200.0f);
    EXPECT_FLOAT_EQ(rect.m_SizeDelta.y, 50.0f);

    lua.script("r.pivot = vec2.new(0.0, 1.0)");
    EXPECT_FLOAT_EQ(rect.m_Pivot.x, 0.0f);
    EXPECT_FLOAT_EQ(rect.m_Pivot.y, 1.0f);

    lua.script("r.rotation = 45.0; r.scale = vec2.new(2.0, 2.0)");
    EXPECT_FLOAT_EQ(rect.m_Rotation, 45.0f);
    EXPECT_FLOAT_EQ(rect.m_Scale.x, 2.0f);
}

TEST_F(LuaBindingTest, UIImageComponent_PropertyRoundTrip)
{
    UIImageComponent img;
    lua["i"] = &img;

    lua.script("i.color = vec4.new(1.0, 0.0, 0.0, 0.5)");
    EXPECT_FLOAT_EQ(img.m_Color.r, 1.0f);
    EXPECT_FLOAT_EQ(img.m_Color.a, 0.5f);

    lua.script("i.borderInsets = vec4.new(1.0, 2.0, 3.0, 4.0)");
    EXPECT_FLOAT_EQ(img.m_BorderInsets.x, 1.0f);
    EXPECT_FLOAT_EQ(img.m_BorderInsets.w, 4.0f);
}

TEST_F(LuaBindingTest, UIPanelComponent_PropertyRoundTrip)
{
    UIPanelComponent panel;
    lua["p"] = &panel;

    lua.script("p.backgroundColor = vec4.new(0.1, 0.2, 0.3, 0.9)");
    EXPECT_FLOAT_EQ(panel.m_BackgroundColor.r, 0.1f);
    EXPECT_FLOAT_EQ(panel.m_BackgroundColor.a, 0.9f);
}

TEST_F(LuaBindingTest, UITextComponent_PropertyRoundTrip)
{
    UITextComponent text;
    lua["t"] = &text;

    lua.script("t.text = 'UI Label'; t.fontSize = 24.0");
    EXPECT_EQ(text.m_Text, "UI Label");
    EXPECT_FLOAT_EQ(text.m_FontSize, 24.0f);

    lua.script("t.color = vec4.new(0.0, 0.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(text.m_Color.r, 0.0f);

    lua.script("t.kerning = 0.5; t.lineSpacing = 1.2");
    EXPECT_FLOAT_EQ(text.m_Kerning, 0.5f);
    EXPECT_FLOAT_EQ(text.m_LineSpacing, 1.2f);
}

TEST_F(LuaBindingTest, UIButtonComponent_PropertyRoundTrip)
{
    UIButtonComponent btn;
    lua["b"] = &btn;

    lua.script("b.interactable = false");
    EXPECT_FALSE(btn.m_Interactable);

    lua.script("b.normalColor = vec4.new(0.5, 0.5, 0.5, 1.0)");
    EXPECT_FLOAT_EQ(btn.m_NormalColor.r, 0.5f);

    lua.script("b.hoveredColor = vec4.new(0.6, 0.6, 0.6, 1.0)");
    EXPECT_FLOAT_EQ(btn.m_HoveredColor.r, 0.6f);

    // state is readonly
    auto result = lua.script("return b.state");
    EXPECT_EQ(result.get<UIButtonState>(), UIButtonState::Normal);
}

TEST_F(LuaBindingTest, UISliderComponent_PropertyRoundTrip)
{
    UISliderComponent slider;
    lua["s"] = &slider;

    lua.script("s.value = 0.5; s.minValue = 0.0; s.maxValue = 100.0");
    EXPECT_FLOAT_EQ(slider.m_Value, 0.5f);
    EXPECT_FLOAT_EQ(slider.m_MinValue, 0.0f);
    EXPECT_FLOAT_EQ(slider.m_MaxValue, 100.0f);

    lua.script("s.interactable = false");
    EXPECT_FALSE(slider.m_Interactable);

    lua.script("s.fillColor = vec4.new(0.0, 1.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(slider.m_FillColor.g, 1.0f);
}

TEST_F(LuaBindingTest, UICheckboxComponent_PropertyRoundTrip)
{
    UICheckboxComponent cb;
    lua["c"] = &cb;

    lua.script("c.isChecked = true");
    EXPECT_TRUE(cb.m_IsChecked);

    lua.script("c.interactable = false");
    EXPECT_FALSE(cb.m_Interactable);

    lua.script("c.checkedColor = vec4.new(0.0, 0.8, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(cb.m_CheckedColor.g, 0.8f);
}

TEST_F(LuaBindingTest, UIProgressBarComponent_PropertyRoundTrip)
{
    UIProgressBarComponent pb;
    lua["p"] = &pb;

    lua.script("p.value = 75.0; p.minValue = 0.0; p.maxValue = 100.0");
    EXPECT_FLOAT_EQ(pb.m_Value, 75.0f);
    EXPECT_FLOAT_EQ(pb.m_MinValue, 0.0f);
    EXPECT_FLOAT_EQ(pb.m_MaxValue, 100.0f);

    lua.script("p.fillColor = vec4.new(0.2, 0.8, 0.2, 1.0)");
    EXPECT_FLOAT_EQ(pb.m_FillColor.g, 0.8f);
}

TEST_F(LuaBindingTest, UIInputFieldComponent_PropertyRoundTrip)
{
    UIInputFieldComponent input;
    lua["i"] = &input;

    lua.script("i.text = 'typed text'; i.placeholder = 'Enter...'");
    EXPECT_EQ(input.m_Text, "typed text");
    EXPECT_EQ(input.m_Placeholder, "Enter...");

    lua.script("i.fontSize = 16.0; i.characterLimit = 50; i.interactable = false");
    EXPECT_FLOAT_EQ(input.m_FontSize, 16.0f);
    EXPECT_EQ(input.m_CharacterLimit, 50);
    EXPECT_FALSE(input.m_Interactable);
}

TEST_F(LuaBindingTest, UIScrollViewComponent_PropertyRoundTrip)
{
    UIScrollViewComponent sv;
    lua["s"] = &sv;

    lua.script("s.scrollPosition = vec2.new(10.0, 20.0)");
    EXPECT_FLOAT_EQ(sv.m_ScrollPosition.x, 10.0f);
    EXPECT_FLOAT_EQ(sv.m_ScrollPosition.y, 20.0f);

    lua.script("s.contentSize = vec2.new(800.0, 2000.0)");
    EXPECT_FLOAT_EQ(sv.m_ContentSize.x, 800.0f);

    lua.script("s.scrollSpeed = 2.5");
    EXPECT_FLOAT_EQ(sv.m_ScrollSpeed, 2.5f);

    lua.script("s.showHorizontalScrollbar = false; s.showVerticalScrollbar = true");
    EXPECT_FALSE(sv.m_ShowHorizontalScrollbar);
    EXPECT_TRUE(sv.m_ShowVerticalScrollbar);
}

TEST_F(LuaBindingTest, UIDropdownComponent_PropertyRoundTrip)
{
    UIDropdownComponent dd;
    lua["d"] = &dd;

    lua.script("d.selectedIndex = 3; d.fontSize = 18.0; d.itemHeight = 30.0");
    EXPECT_EQ(dd.m_SelectedIndex, 3);
    EXPECT_FLOAT_EQ(dd.m_FontSize, 18.0f);
    EXPECT_FLOAT_EQ(dd.m_ItemHeight, 30.0f);

    lua.script("d.interactable = false");
    EXPECT_FALSE(dd.m_Interactable);
}

TEST_F(LuaBindingTest, UIGridLayoutComponent_PropertyRoundTrip)
{
    UIGridLayoutComponent grid;
    lua["g"] = &grid;

    lua.script("g.cellSize = vec2.new(64.0, 64.0); g.spacing = vec2.new(4.0, 4.0)");
    EXPECT_FLOAT_EQ(grid.m_CellSize.x, 64.0f);
    EXPECT_FLOAT_EQ(grid.m_Spacing.x, 4.0f);

    lua.script("g.constraintCount = 4");
    EXPECT_EQ(grid.m_ConstraintCount, 4);
}

TEST_F(LuaBindingTest, UIToggleComponent_PropertyRoundTrip)
{
    UIToggleComponent toggle;
    lua["t"] = &toggle;

    lua.script("t.isOn = true; t.interactable = false");
    EXPECT_TRUE(toggle.m_IsOn);
    EXPECT_FALSE(toggle.m_Interactable);

    lua.script("t.onColor = vec4.new(0.0, 1.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(toggle.m_OnColor.g, 1.0f);

    lua.script("t.knobColor = vec4.new(1.0, 1.0, 1.0, 1.0)");
    EXPECT_FLOAT_EQ(toggle.m_KnobColor.r, 1.0f);
}

// =============================================================================
// ParticleSystem + ParticleEmitter
// =============================================================================

TEST_F(LuaBindingTest, ParticleSystem_PropertyRoundTrip)
{
    ParticleSystem ps;
    lua["ps"] = &ps;

    lua.script("ps.playing = false; ps.looping = false; ps.duration = 10.0");
    EXPECT_FALSE(ps.Playing);
    EXPECT_FALSE(ps.Looping);
    EXPECT_FLOAT_EQ(ps.Duration, 10.0f);

    lua.script("ps.playbackSpeed = 2.0; ps.windInfluence = 0.5");
    EXPECT_FLOAT_EQ(ps.PlaybackSpeed, 2.0f);
    EXPECT_FLOAT_EQ(ps.WindInfluence, 0.5f);
}

TEST_F(LuaBindingTest, ParticleEmitter_PropertyRoundTrip)
{
    ParticleEmitter emitter;
    lua["e"] = &emitter;

    lua.script("e.rateOverTime = 50.0; e.initialSpeed = 10.0; e.speedVariance = 2.0");
    EXPECT_FLOAT_EQ(emitter.RateOverTime, 50.0f);
    EXPECT_FLOAT_EQ(emitter.InitialSpeed, 10.0f);
    EXPECT_FLOAT_EQ(emitter.SpeedVariance, 2.0f);

    lua.script("e.lifetimeMin = 0.5; e.lifetimeMax = 3.0");
    EXPECT_FLOAT_EQ(emitter.LifetimeMin, 0.5f);
    EXPECT_FLOAT_EQ(emitter.LifetimeMax, 3.0f);

    lua.script("e.initialSize = 2.0; e.sizeVariance = 0.5");
    EXPECT_FLOAT_EQ(emitter.InitialSize, 2.0f);
    EXPECT_FLOAT_EQ(emitter.SizeVariance, 0.5f);

    lua.script("e.initialColor = vec4.new(1.0, 0.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(emitter.InitialColor.r, 1.0f);
    EXPECT_FLOAT_EQ(emitter.InitialColor.g, 0.0f);
}

TEST_F(LuaBindingTest, ParticleSystemComponent_SystemAccess)
{
    ParticleSystemComponent psc;
    lua["psc"] = &psc;

    lua.script("psc.system.duration = 7.0; psc.system.looping = false");
    EXPECT_FLOAT_EQ(psc.System.Duration, 7.0f);
    EXPECT_FALSE(psc.System.Looping);
}

// =============================================================================
// LightProbeComponent + LightProbeVolumeComponent
// =============================================================================

TEST_F(LuaBindingTest, LightProbeComponent_PropertyRoundTrip)
{
    LightProbeComponent lp;
    lua["lp"] = &lp;

    lua.script("lp.influenceRadius = 25.0; lp.intensity = 0.8; lp.active = false");
    EXPECT_FLOAT_EQ(lp.m_InfluenceRadius, 25.0f);
    EXPECT_FLOAT_EQ(lp.m_Intensity, 0.8f);
    EXPECT_FALSE(lp.m_Active);
}

TEST_F(LuaBindingTest, LightProbeVolumeComponent_PropertyRoundTrip)
{
    LightProbeVolumeComponent lpv;
    lua["lpv"] = &lpv;

    lua.script("lpv.boundsMin = vec3.new(-10.0, -10.0, -10.0)");
    EXPECT_FLOAT_EQ(lpv.m_BoundsMin.x, -10.0f);

    lua.script("lpv.boundsMax = vec3.new(10.0, 10.0, 10.0)");
    EXPECT_FLOAT_EQ(lpv.m_BoundsMax.x, 10.0f);

    lua.script("lpv.spacing = 5.0");
    EXPECT_FLOAT_EQ(lpv.m_Spacing, 5.0f);

    lua.script("lpv.intensity = 1.5; lpv.active = false; lpv.dirty = true");
    EXPECT_FLOAT_EQ(lpv.m_Intensity, 1.5f);
    EXPECT_FALSE(lpv.m_Active);
    EXPECT_TRUE(lpv.m_Dirty);

    auto result = lua.script("return lpv:getTotalProbeCount()");
    EXPECT_GE(result.get<i32>(), 0);
}

// =============================================================================
// UIWorldAnchorComponent
// =============================================================================

TEST_F(LuaBindingTest, UIWorldAnchorComponent_PropertyRoundTrip)
{
    UIWorldAnchorComponent anchor;
    lua["a"] = &anchor;

    lua.script("a.targetEntity = 42");
    EXPECT_EQ(static_cast<u64>(anchor.m_TargetEntity), 42u);

    lua.script("a.worldOffset = vec3.new(0.0, 5.0, 0.0)");
    EXPECT_FLOAT_EQ(anchor.m_WorldOffset.y, 5.0f);

    auto result = lua.script("return a.targetEntity");
    EXPECT_EQ(result.get<u64>(), 42u);
}

// =============================================================================
// NameplateComponent
// =============================================================================

TEST_F(LuaBindingTest, NameplateComponent_PropertyRoundTrip)
{
    NameplateComponent np;
    lua["np"] = &np;

    lua.script("np.enabled = false; np.showHealthBar = false; np.showManaBar = true");
    EXPECT_FALSE(np.m_Enabled);
    EXPECT_FALSE(np.m_ShowHealthBar);
    EXPECT_TRUE(np.m_ShowManaBar);

    lua.script("np.worldOffset = vec3.new(0.0, 3.0, 0.0)");
    EXPECT_FLOAT_EQ(np.m_WorldOffset.y, 3.0f);

    lua.script("np.barSize = vec2.new(200.0, 16.0)");
    EXPECT_FLOAT_EQ(np.m_BarSize.x, 200.0f);

    lua.script("np.manaBarGap = 4.0");
    EXPECT_FLOAT_EQ(np.m_ManaBarGap, 4.0f);

    lua.script("np.healthBarColor = vec4.new(1.0, 0.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(np.m_HealthBarColor.r, 1.0f);
}

// =============================================================================
// IKTargetComponent
// =============================================================================

TEST_F(LuaBindingTest, IKTargetComponent_AimProperties)
{
    IKTargetComponent ik;
    lua["ik"] = &ik;

    lua.script("ik.aimIKEnabled = true; ik.aimBoneIndex = 5");
    EXPECT_TRUE(ik.AimIKEnabled);
    EXPECT_EQ(ik.AimBoneIndex, 5);

    lua.script("ik.aimTarget = vec3.new(10.0, 5.0, 0.0)");
    EXPECT_FLOAT_EQ(ik.AimTarget.x, 10.0f);
    EXPECT_FLOAT_EQ(ik.AimTarget.y, 5.0f);

    lua.script("ik.aimWeight = 0.8; ik.aimChainLength = 3; ik.aimChainFactor = 0.7");
    EXPECT_FLOAT_EQ(ik.AimWeight, 0.8f);
    EXPECT_EQ(ik.AimChainLength, 3);
    EXPECT_FLOAT_EQ(ik.AimChainFactor, 0.7f);

    lua.script("ik.aimTargetEntity = 99");
    EXPECT_EQ(static_cast<u64>(ik.AimTargetEntity), 99u);
}

TEST_F(LuaBindingTest, IKTargetComponent_LimbProperties)
{
    IKTargetComponent ik;
    lua["ik"] = &ik;

    lua.script("ik.limbIKEnabled = true; ik.limbBoneIndex = 12");
    EXPECT_TRUE(ik.LimbIKEnabled);
    EXPECT_EQ(ik.LimbBoneIndex, 12);

    lua.script("ik.limbTarget = vec3.new(1.0, 0.0, 0.0)");
    EXPECT_FLOAT_EQ(ik.LimbTarget.x, 1.0f);

    lua.script("ik.limbChainLength = 4; ik.limbWeight = 0.9");
    EXPECT_EQ(ik.LimbChainLength, 4);
    EXPECT_FLOAT_EQ(ik.LimbWeight, 0.9f);

    lua.script("ik.limbTargetEntity = 200");
    EXPECT_EQ(static_cast<u64>(ik.LimbTargetEntity), 200u);
}

// =============================================================================
// WindSettings
// =============================================================================

TEST_F(LuaBindingTest, WindSettings_PropertyRoundTrip)
{
    WindSettings ws;
    lua["ws"] = &ws;

    lua.script("ws.enabled = true; ws.speed = 5.0; ws.gustStrength = 2.0");
    EXPECT_TRUE(ws.Enabled);
    EXPECT_FLOAT_EQ(ws.Speed, 5.0f);
    EXPECT_FLOAT_EQ(ws.GustStrength, 2.0f);

    lua.script("ws.gustFrequency = 0.5; ws.turbulenceIntensity = 0.3; ws.turbulenceScale = 10.0");
    EXPECT_FLOAT_EQ(ws.GustFrequency, 0.5f);
    EXPECT_FLOAT_EQ(ws.TurbulenceIntensity, 0.3f);
    EXPECT_FLOAT_EQ(ws.TurbulenceScale, 10.0f);

    lua.script("ws.direction = vec3.new(1.0, 0.0, 0.0)");
    EXPECT_FLOAT_EQ(ws.Direction.x, 1.0f);

    lua.script("ws.gridWorldSize = 500.0; ws.gridResolution = 64");
    EXPECT_FLOAT_EQ(ws.GridWorldSize, 500.0f);
    EXPECT_EQ(ws.GridResolution, 64);
}

// =============================================================================
// StreamingVolumeComponent + StreamingSettings
// =============================================================================

TEST_F(LuaBindingTest, StreamingVolumeComponent_PropertyRoundTrip)
{
    StreamingVolumeComponent sv;
    lua["sv"] = &sv;

    lua.script("sv.loadRadius = 300.0; sv.unloadRadius = 400.0");
    EXPECT_FLOAT_EQ(sv.LoadRadius, 300.0f);
    EXPECT_FLOAT_EQ(sv.UnloadRadius, 400.0f);

    // isLoaded is readonly
    auto result = lua.script("return sv.isLoaded");
    EXPECT_FALSE(result.get<bool>());
}

TEST_F(LuaBindingTest, StreamingSettings_PropertyRoundTrip)
{
    StreamingSettings ss;
    lua["ss"] = &ss;

    lua.script("ss.enabled = true; ss.defaultLoadRadius = 150.0; ss.defaultUnloadRadius = 200.0");
    EXPECT_TRUE(ss.Enabled);
    EXPECT_FLOAT_EQ(ss.DefaultLoadRadius, 150.0f);
    EXPECT_FLOAT_EQ(ss.DefaultUnloadRadius, 200.0f);

    lua.script("ss.maxLoadedRegions = 8");
    EXPECT_EQ(ss.MaxLoadedRegions, 8u);

    lua.script("ss.regionDirectory = 'Regions/World1'");
    EXPECT_EQ(ss.RegionDirectory, "Regions/World1");
}

// =============================================================================
// NetworkIdentityComponent
// =============================================================================

TEST_F(LuaBindingTest, NetworkIdentityComponent_PropertyRoundTrip)
{
    NetworkIdentityComponent net;
    lua["n"] = &net;

    lua.script("n.ownerClientID = 42; n.isReplicated = false");
    EXPECT_EQ(net.OwnerClientID, 42u);
    EXPECT_FALSE(net.IsReplicated);
}

// =============================================================================
// AudioSourceComponent (including spatial properties)
// =============================================================================

TEST_F(LuaBindingTest, AudioSourceComponent_BasicProperties)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    lua.script("a.volume = 0.5");
    EXPECT_FLOAT_EQ(audio.Config.VolumeMultiplier, 0.5f);

    lua.script("a.pitch = 1.5");
    EXPECT_FLOAT_EQ(audio.Config.PitchMultiplier, 1.5f);

    lua.script("a.playOnAwake = false; a.looping = true; a.spatialization = true");
    EXPECT_FALSE(audio.Config.PlayOnAwake);
    EXPECT_TRUE(audio.Config.Looping);
    EXPECT_TRUE(audio.Config.Spatialization);

    lua.script("a.useEventSystem = true");
    EXPECT_TRUE(audio.UseEventSystem);

    lua.script("a.startEvent = 'PlayFootsteps'");
    EXPECT_EQ(audio.StartEvent, "PlayFootsteps");
}

TEST_F(LuaBindingTest, AudioSourceComponent_VolumeClamping)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    lua.script("a.volume = 5.0");
    EXPECT_FLOAT_EQ(audio.Config.VolumeMultiplier, 2.0f); // clamped to max

    lua.script("a.volume = -1.0");
    EXPECT_FLOAT_EQ(audio.Config.VolumeMultiplier, 0.0f); // clamped to min
}

TEST_F(LuaBindingTest, AudioSourceComponent_PitchClamping)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    lua.script("a.pitch = 10.0");
    EXPECT_FLOAT_EQ(audio.Config.PitchMultiplier, 3.0f); // clamped to max

    lua.script("a.pitch = 0.01");
    EXPECT_FLOAT_EQ(audio.Config.PitchMultiplier, 0.1f); // clamped to min
}

TEST_F(LuaBindingTest, AudioSourceComponent_SpatialProperties)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    // AttenuationModel (enum as int)
    lua.script("a.attenuationModel = 2"); // Linear
    EXPECT_EQ(audio.Config.AttenuationModel, AttenuationModelType::Linear);

    auto result = lua.script("return a.attenuationModel");
    EXPECT_EQ(result.get<int>(), 2);

    // RollOff
    lua.script("a.rollOff = 2.5");
    EXPECT_FLOAT_EQ(audio.Config.RollOff, 2.5f);

    // Gain range
    lua.script("a.minGain = 0.1; a.maxGain = 0.9");
    EXPECT_FLOAT_EQ(audio.Config.MinGain, 0.1f);
    EXPECT_FLOAT_EQ(audio.Config.MaxGain, 0.9f);

    // Distance range
    lua.script("a.minDistance = 1.0; a.maxDistance = 500.0");
    EXPECT_FLOAT_EQ(audio.Config.MinDistance, 1.0f);
    EXPECT_FLOAT_EQ(audio.Config.MaxDistance, 500.0f);

    // Cone angles
    lua.script("a.coneInnerAngle = 1.57; a.coneOuterAngle = 3.14; a.coneOuterGain = 0.2");
    EXPECT_NEAR(audio.Config.ConeInnerAngle, 1.57f, 1e-5f);
    EXPECT_NEAR(audio.Config.ConeOuterAngle, 3.14f, 1e-5f);
    EXPECT_FLOAT_EQ(audio.Config.ConeOuterGain, 0.2f);

    // Doppler
    lua.script("a.dopplerFactor = 2.0");
    EXPECT_FLOAT_EQ(audio.Config.DopplerFactor, 2.0f);
}

TEST_F(LuaBindingTest, AudioSourceComponent_NaNSafety)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    // NaN should reset to defaults
    lua.script("a.volume = 0/0"); // NaN
    EXPECT_FLOAT_EQ(audio.Config.VolumeMultiplier, 1.0f);

    lua.script("a.pitch = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.PitchMultiplier, 1.0f);

    lua.script("a.rollOff = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.RollOff, 1.0f);

    lua.script("a.dopplerFactor = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.DopplerFactor, 1.0f);

    lua.script("a.minDistance = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.MinDistance, 0.3f);

    lua.script("a.maxDistance = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.MaxDistance, 1000.0f);
}

// --- AudioListenerComponent ---

TEST_F(LuaBindingTest, AudioListenerComponent_PropertyRoundTrip)
{
    AudioListenerComponent al;
    lua["al"] = &al;

    lua.script("al.active = false");
    EXPECT_FALSE(al.Active);

    lua.script("al.active = true");
    EXPECT_TRUE(al.Active);
}

// =============================================================================
// DialogueComponent
// =============================================================================

TEST_F(LuaBindingTest, DialogueComponent_PropertyRoundTrip)
{
    DialogueComponent dc;
    lua["dc"] = &dc;

    lua.script("dc.autoTrigger = true; dc.triggerRadius = 5.0; dc.triggerOnce = false");
    EXPECT_TRUE(dc.m_AutoTrigger);
    EXPECT_FLOAT_EQ(dc.m_TriggerRadius, 5.0f);
    EXPECT_FALSE(dc.m_TriggerOnce);

    lua.script("dc.hasTriggered = true");
    EXPECT_TRUE(dc.m_HasTriggered);
}

// =============================================================================
// NavAgentComponent
// =============================================================================

TEST_F(LuaBindingTest, NavAgentComponent_PropertyRoundTrip)
{
    NavAgentComponent nav;
    lua["n"] = &nav;

    lua.script("n.radius = 1.0; n.height = 3.0; n.maxSpeed = 7.0; n.acceleration = 12.0");
    EXPECT_FLOAT_EQ(nav.m_Radius, 1.0f);
    EXPECT_FLOAT_EQ(nav.m_Height, 3.0f);
    EXPECT_FLOAT_EQ(nav.m_MaxSpeed, 7.0f);
    EXPECT_FLOAT_EQ(nav.m_Acceleration, 12.0f);

    lua.script("n.stoppingDistance = 0.5; n.avoidancePriority = 10; n.lockYAxis = true");
    EXPECT_FLOAT_EQ(nav.m_StoppingDistance, 0.5f);
    EXPECT_EQ(nav.m_AvoidancePriority, 10);
    EXPECT_TRUE(nav.m_LockYAxis);

    // hasTarget and hasPath are readonly
    auto r1 = lua.script("return n.hasTarget");
    EXPECT_FALSE(r1.get<bool>());
    auto r2 = lua.script("return n.hasPath");
    EXPECT_FALSE(r2.get<bool>());
}

// =============================================================================
// ItemPickupComponent + ItemContainerComponent
// =============================================================================

TEST_F(LuaBindingTest, ItemPickupComponent_PropertyRoundTrip)
{
    ItemPickupComponent ip;
    lua["ip"] = &ip;

    lua.script("ip.pickupRadius = 3.0; ip.autoPickup = true; ip.despawnTimer = 30.0");
    EXPECT_FLOAT_EQ(ip.PickupRadius, 3.0f);
    EXPECT_TRUE(ip.AutoPickup);
    EXPECT_FLOAT_EQ(ip.DespawnTimer, 30.0f);
}

TEST_F(LuaBindingTest, ItemContainerComponent_PropertyRoundTrip)
{
    ItemContainerComponent ic;
    lua["ic"] = &ic;

    lua.script("ic.isShop = true; ic.lootTableID = 'dungeon_chest'; ic.hasBeenLooted = true");
    EXPECT_TRUE(ic.IsShop);
    EXPECT_EQ(ic.LootTableID, "dungeon_chest");
    EXPECT_TRUE(ic.HasBeenLooted);
}

// =============================================================================
// QuestGiverComponent
// =============================================================================

TEST_F(LuaBindingTest, QuestGiverComponent_PropertyRoundTrip)
{
    QuestGiverComponent qg;
    lua["qg"] = &qg;

    lua.script("qg.questMarkerIcon = '!'");
    EXPECT_EQ(qg.QuestMarkerIcon, "!");
}

// =============================================================================
// MaterialComponent
// =============================================================================

TEST_F(LuaBindingTest, MaterialComponent_AlbedoColorRoundTrip)
{
    MaterialComponent mc;
    lua["mc"] = &mc;

    lua.script("mc.albedoColor = vec4.new(0.2, 0.4, 0.6, 1.0)");
    auto color = mc.m_Material.GetBaseColorFactor();
    EXPECT_FLOAT_EQ(color.r, 0.2f);
    EXPECT_FLOAT_EQ(color.g, 0.4f);
    EXPECT_FLOAT_EQ(color.b, 0.6f);
    EXPECT_FLOAT_EQ(color.a, 1.0f);

    auto result = lua.script("local c = mc.albedoColor; return c.x, c.y, c.z, c.w");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 0.2f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 0.4f);
    EXPECT_FLOAT_EQ(result.get<f32>(2), 0.6f);
    EXPECT_FLOAT_EQ(result.get<f32>(3), 1.0f);
}

TEST_F(LuaBindingTest, MaterialComponent_ShaderGraphHandleRoundTrip)
{
    MaterialComponent mc;
    lua["mc"] = &mc;

    // Without an active project/AssetManager, non-zero handles are rejected by
    // the production setter (asset validation). Verify the clear path (handle=0)
    // and read-back work correctly.
    mc.m_ShaderGraphHandle = 99;
    lua.script("mc.shaderGraphHandle = 0");
    EXPECT_EQ(static_cast<u64>(mc.m_ShaderGraphHandle), 0u);

    auto result = lua.script("return mc.shaderGraphHandle");
    EXPECT_EQ(result.get<u64>(), 0u);
}

// =============================================================================
// QuestJournalComponent
// =============================================================================

TEST_F(LuaBindingTest, QuestJournalComponent_ItemCountRoundTrip)
{
    QuestJournalComponent qj;
    lua["qj"] = &qj;

    lua.script("qj:SetItemCount('sword', 5)");
    EXPECT_EQ(qj.Journal.GetItemCount("sword"), 5);

    auto result = lua.script("return qj:GetItemCount('sword')");
    EXPECT_EQ(result.get<i32>(), 5);
}

TEST_F(LuaBindingTest, QuestJournalComponent_StatRoundTrip)
{
    QuestJournalComponent qj;
    lua["qj"] = &qj;

    lua.script("qj:SetStat('kills', 10)");
    EXPECT_EQ(qj.Journal.GetStat("kills"), 10);

    auto result = lua.script("return qj:GetStat('kills')");
    EXPECT_EQ(result.get<i32>(), 10);
}

TEST_F(LuaBindingTest, QuestJournalComponent_PlayerClassRoundTrip)
{
    QuestJournalComponent qj;
    lua["qj"] = &qj;

    lua.script("qj:SetPlayerClass('Warrior')");
    EXPECT_EQ(qj.Journal.GetPlayerClass(), "Warrior");

    auto result = lua.script("return qj:GetPlayerClass()");
    EXPECT_EQ(result.get<std::string>(), "Warrior");
}

TEST_F(LuaBindingTest, QuestJournalComponent_PlayerFactionRoundTrip)
{
    QuestJournalComponent qj;
    lua["qj"] = &qj;

    lua.script("qj:SetPlayerFaction('Alliance')");
    EXPECT_EQ(qj.Journal.GetPlayerFaction(), "Alliance");

    auto result = lua.script("return qj:GetPlayerFaction()");
    EXPECT_EQ(result.get<std::string>(), "Alliance");
}

TEST_F(LuaBindingTest, QuestJournalComponent_TagRoundTrip)
{
    QuestJournalComponent qj;
    lua["qj"] = &qj;

    auto r1 = lua.script("return qj:HasTag('visited_town')");
    EXPECT_FALSE(r1.get<bool>());

    lua.script("qj:AddTag('visited_town')");
    auto r2 = lua.script("return qj:HasTag('visited_town')");
    EXPECT_TRUE(r2.get<bool>());
}

TEST_F(LuaBindingTest, QuestJournalComponent_PlayerLevelRoundTrip)
{
    QuestJournalComponent qj;
    lua["qj"] = &qj;

    lua.script("qj:SetPlayerLevel(15)");
    EXPECT_EQ(qj.Journal.GetPlayerLevel(), 15);

    auto result = lua.script("return qj:GetPlayerLevel()");
    EXPECT_EQ(result.get<i32>(), 15);
}

TEST_F(LuaBindingTest, QuestJournalComponent_ReputationRoundTrip)
{
    QuestJournalComponent qj;
    lua["qj"] = &qj;

    lua.script("qj:SetReputation('Stormwind', 500)");
    EXPECT_EQ(qj.Journal.GetReputation("Stormwind"), 500);

    auto result = lua.script("return qj:GetReputation('Stormwind')");
    EXPECT_EQ(result.get<i32>(), 500);
}

// =============================================================================
// Log table
// =============================================================================

TEST_F(LuaBindingTest, LogTable_FunctionsExist)
{
    // Verify the Log table exists and all four entries are callable
    auto r1 = lua.script("return type(Log)");
    EXPECT_EQ(r1.get<std::string>(), "table");

    // sol2 wraps C++ lambdas as userdata, so check they're non-nil and callable
    EXPECT_NO_THROW(lua.script("assert(Log.Trace ~= nil)"));
    EXPECT_NO_THROW(lua.script("assert(Log.Info ~= nil)"));
    EXPECT_NO_THROW(lua.script("assert(Log.Warn ~= nil)"));
    EXPECT_NO_THROW(lua.script("assert(Log.Error ~= nil)"));
}

TEST_F(LuaBindingTest, LogTable_CallsDoNotThrow)
{
    // Verify calling each log function doesn't throw
    EXPECT_NO_THROW(lua.script("Log.Trace('test trace')"));
    EXPECT_NO_THROW(lua.script("Log.Info('test info')"));
    EXPECT_NO_THROW(lua.script("Log.Warn('test warn')"));
    EXPECT_NO_THROW(lua.script("Log.Error('test error')"));
}

// =============================================================================
// KeyCode constants
// =============================================================================

TEST_F(LuaBindingTest, KeyCode_TableExists)
{
    auto r = lua.script("return type(KeyCode)");
    EXPECT_EQ(r.get<std::string>(), "table");
}

TEST_F(LuaBindingTest, KeyCode_LetterValues)
{
    auto rA = lua.script("return KeyCode.A");
    EXPECT_EQ(rA.get<u16>(), 65);
    auto rW = lua.script("return KeyCode.W");
    EXPECT_EQ(rW.get<u16>(), 87);
    auto rS = lua.script("return KeyCode.S");
    EXPECT_EQ(rS.get<u16>(), 83);
    auto rD = lua.script("return KeyCode.D");
    EXPECT_EQ(rD.get<u16>(), 68);
}

TEST_F(LuaBindingTest, KeyCode_FunctionKeyValues)
{
    auto rEsc = lua.script("return KeyCode.Escape");
    EXPECT_EQ(rEsc.get<u16>(), 256);
    auto rEnter = lua.script("return KeyCode.Enter");
    EXPECT_EQ(rEnter.get<u16>(), 257);
    auto rTab = lua.script("return KeyCode.Tab");
    EXPECT_EQ(rTab.get<u16>(), 258);
    auto rF1 = lua.script("return KeyCode.F1");
    EXPECT_EQ(rF1.get<u16>(), 290);
}

TEST_F(LuaBindingTest, KeyCode_ArrowKeys)
{
    auto rUp = lua.script("return KeyCode.Up");
    EXPECT_EQ(rUp.get<u16>(), 265);
    auto rDown = lua.script("return KeyCode.Down");
    EXPECT_EQ(rDown.get<u16>(), 264);
    auto rLeft = lua.script("return KeyCode.Left");
    EXPECT_EQ(rLeft.get<u16>(), 263);
    auto rRight = lua.script("return KeyCode.Right");
    EXPECT_EQ(rRight.get<u16>(), 262);
}

TEST_F(LuaBindingTest, KeyCode_ModifierKeys)
{
    auto rShift = lua.script("return KeyCode.LeftShift");
    EXPECT_EQ(rShift.get<u16>(), 340);
    auto rCtrl = lua.script("return KeyCode.LeftControl");
    EXPECT_EQ(rCtrl.get<u16>(), 341);
}

// =============================================================================
// MouseButton constants
// =============================================================================

TEST_F(LuaBindingTest, MouseButton_TableExists)
{
    auto r = lua.script("return type(MouseButton)");
    EXPECT_EQ(r.get<std::string>(), "table");
}

TEST_F(LuaBindingTest, MouseButton_Values)
{
    // New canonical names (from X-MACRO)
    auto rBtnLeft = lua.script("return MouseButton.ButtonLeft");
    EXPECT_EQ(rBtnLeft.get<u16>(), 0);
    auto rBtnRight = lua.script("return MouseButton.ButtonRight");
    EXPECT_EQ(rBtnRight.get<u16>(), 1);
    auto rBtnMiddle = lua.script("return MouseButton.ButtonMiddle");
    EXPECT_EQ(rBtnMiddle.get<u16>(), 2);
}

// =============================================================================
// Global table smoke tests (entity_utils, Application, Scene, Damage)
// =============================================================================

TEST_F(LuaBindingTest, EntityUtils_TableExists)
{
    auto r = lua.script("return type(entity_utils)");
    EXPECT_EQ(r.get<std::string>(), "table");
}

TEST_F(LuaBindingTest, EntityUtils_HasExpectedFunctions)
{
    auto r = lua.script(R"(
        return type(entity_utils.get_component),
               type(entity_utils.has_component),
               type(entity_utils.find_by_name),
               type(entity_utils.get_translation),
               type(entity_utils.set_translation),
               type(entity_utils.get_name)
    )");
    EXPECT_EQ(r.get<std::string>(0), "function");
    EXPECT_EQ(r.get<std::string>(1), "function");
    EXPECT_EQ(r.get<std::string>(2), "function");
    EXPECT_EQ(r.get<std::string>(3), "function");
    EXPECT_EQ(r.get<std::string>(4), "function");
    EXPECT_EQ(r.get<std::string>(5), "function");
}

TEST_F(LuaBindingTest, Application_TableExists)
{
    auto r = lua.script("return type(Application)");
    EXPECT_EQ(r.get<std::string>(), "table");
}

TEST_F(LuaBindingTest, Application_HasExpectedFunctions)
{
    auto r = lua.script(R"(
        return type(Application.GetTimeScale),
               type(Application.SetTimeScale),
               type(Application.QuitGame)
    )");
    EXPECT_EQ(r.get<std::string>(0), "function");
    EXPECT_EQ(r.get<std::string>(1), "function");
    EXPECT_EQ(r.get<std::string>(2), "function");
}

TEST_F(LuaBindingTest, Scene_TableExists)
{
    auto r = lua.script("return type(Scene)");
    EXPECT_EQ(r.get<std::string>(), "table");
}

TEST_F(LuaBindingTest, Damage_TableExists)
{
    auto r = lua.script("return type(Damage)");
    EXPECT_EQ(r.get<std::string>(), "table");
}

TEST_F(LuaBindingTest, Damage_HasExpectedFunctions)
{
    auto r = lua.script(R"(
        return type(Damage.TryActivateAbilityOnTarget),
               type(Damage.TryActivateAbility)
    )");
    EXPECT_EQ(r.get<std::string>(0), "function");
    EXPECT_EQ(r.get<std::string>(1), "function");
}
