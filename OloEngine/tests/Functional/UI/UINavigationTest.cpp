#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// UINavigationTest — Functional Test.
//
// Cross-subsystem seam under test:
//   UINavigationSystem (focus traversal + widget event delegates) ×
//   UIResolvedRectComponent (screen rects, normally produced by
//   UILayoutSystem) × the interactive UI widget components × the runtime-only
//   UINavigation state owned by Scene.
//
// This is the gamepad/keyboard menu-navigation half of issue #457: a menu must
// be fully operable without a mouse — navigate between widgets by direction,
// activate the focused one, adjust a focused slider, and fire the same event
// delegates a mouse would, all without per-frame polling by gameplay code.
//
// The tests drive UINavigationSystem::Update directly with a hand-built
// UINavInput (mirroring how UIInputSystem is unit-driven), against real Scene
// components and real resolved rects, so the focus/activation/event contract is
// pinned on CPU with no GL context required.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/UI/UINavigationSystem.h"

#include <glm/glm.hpp>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Give an entity a navigable button occupying a screen-space rect.
    template<typename Widget>
    Entity MakeWidget(Scene& scene, const char* name, const glm::vec2& pos, const glm::vec2& size)
    {
        Entity e = scene.CreateEntity(name);
        e.AddComponent<Widget>();
        auto& rect = e.AddComponent<UIResolvedRectComponent>();
        rect.m_Position = pos;
        rect.m_Size = size;
        return e;
    }
} // namespace

class UINavigationTest : public FunctionalTest
{
  protected:
    // A vertical menu: three buttons, a slider, and a checkbox stacked top-down.
    void BuildScene() override
    {
        Scene& scene = GetScene();
        m_Button0 = MakeWidget<UIButtonComponent>(scene, "Button0", { 100.0f, 100.0f }, { 200.0f, 50.0f });
        m_Button1 = MakeWidget<UIButtonComponent>(scene, "Button1", { 100.0f, 200.0f }, { 200.0f, 50.0f });
        m_Button2 = MakeWidget<UIButtonComponent>(scene, "Button2", { 100.0f, 300.0f }, { 200.0f, 50.0f });
        m_Slider = MakeWidget<UISliderComponent>(scene, "Slider", { 100.0f, 400.0f }, { 200.0f, 50.0f });
        m_Checkbox = MakeWidget<UICheckboxComponent>(scene, "Checkbox", { 100.0f, 500.0f }, { 50.0f, 50.0f });

        auto& slider = m_Slider.GetComponent<UISliderComponent>();
        slider.m_MinValue = 0.0f;
        slider.m_MaxValue = 1.0f;
        slider.m_Value = 0.5f;
    }

    static UINavInput NavDown()
    {
        UINavInput in;
        in.NavDown = true;
        return in;
    }
    static UINavInput NavUp()
    {
        UINavInput in;
        in.NavUp = true;
        return in;
    }

    Entity m_Button0;
    Entity m_Button1;
    Entity m_Button2;
    Entity m_Slider;
    Entity m_Checkbox;
};

// The first directional input seeds focus on the top-most widget; subsequent
// presses walk the vertical order; NavUp reverses it.
TEST_F(UINavigationTest, DirectionalInputSeedsThenTraversesFocus)
{
    UINavigation& nav = GetScene().GetUINavigation();
    ASSERT_FALSE(nav.HasFocus());

    UINavigationSystem::Update(GetScene(), NavDown());
    EXPECT_EQ(nav.GetFocus(), m_Button0.GetUUID()) << "first NavDown should seed focus on the top-most widget";

    UINavigationSystem::Update(GetScene(), NavDown());
    EXPECT_EQ(nav.GetFocus(), m_Button1.GetUUID());

    UINavigationSystem::Update(GetScene(), NavDown());
    EXPECT_EQ(nav.GetFocus(), m_Button2.GetUUID());

    UINavigationSystem::Update(GetScene(), NavDown());
    EXPECT_EQ(nav.GetFocus(), m_Slider.GetUUID());

    UINavigationSystem::Update(GetScene(), NavUp());
    EXPECT_EQ(nav.GetFocus(), m_Button2.GetUUID()) << "NavUp should walk back up the menu";
}

// The focused button reflects onto the existing hover state so the renderer
// highlights it without any renderer change.
TEST_F(UINavigationTest, FocusReflectsOnButtonHoverState)
{
    UINavigationSystem::Update(GetScene(), NavDown()); // focus Button0
    EXPECT_EQ(m_Button0.GetComponent<UIButtonComponent>().m_State, UIButtonState::Hovered);
}

// Activate on a focused button fires both OnClick and OnSubmit exactly once.
TEST_F(UINavigationTest, ActivateFiresButtonClickAndSubmit)
{
    int clicks = 0;
    int submits = 0;
    UINavigation& nav = GetScene().GetUINavigation();
    nav.OnClick(m_Button1.GetUUID(), [&clicks]
                { ++clicks; });
    nav.OnSubmit(m_Button1.GetUUID(), [&submits]
                 { ++submits; });
    nav.SetFocus(m_Button1.GetUUID());

    UINavInput activate;
    activate.Activate = true;
    UINavigationSystem::Update(GetScene(), activate);

    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(submits, 1);
}

// When input is suppressed (e.g. the rebind menu is capturing a key/button for
// itself), Update ignores directional/activate/cancel input so the press is not
// double-handled as navigation. Clearing suppression restores normal behaviour.
TEST_F(UINavigationTest, InputSuppressionIgnoresNavigationAndActivation)
{
    int clicks = 0;
    UINavigation& nav = GetScene().GetUINavigation();
    nav.OnClick(m_Button1.GetUUID(), [&clicks]
                { ++clicks; });
    nav.SetFocus(m_Button1.GetUUID());
    nav.SetInputSuppressed(true);

    UINavInput activate;
    activate.Activate = true;
    UINavigationSystem::Update(GetScene(), activate);
    EXPECT_EQ(clicks, 0);                           // activation ignored while suppressed
    EXPECT_EQ(nav.GetFocus(), m_Button1.GetUUID()); // focus unchanged

    // A directional press is likewise ignored (focus does not move).
    UINavigationSystem::Update(GetScene(), NavDown());
    EXPECT_EQ(nav.GetFocus(), m_Button1.GetUUID());

    // Unsuppress: activation works again.
    nav.SetInputSuppressed(false);
    UINavigationSystem::Update(GetScene(), activate);
    EXPECT_EQ(clicks, 1);
}

// Left/Right on a focused slider nudges its value by one step and fires
// OnValueChanged with the new value — not a focus move.
TEST_F(UINavigationTest, SliderRightAdjustsValueAndFiresValueChanged)
{
    f32 reported = -1.0f;
    UINavigation& nav = GetScene().GetUINavigation();
    nav.OnValueChanged(m_Slider.GetUUID(), [&reported](f32 v)
                       { reported = v; });
    nav.SetFocus(m_Slider.GetUUID());

    UINavInput right;
    right.NavRight = true;
    UINavigationSystem::Update(GetScene(), right);

    const f32 expected = 0.5f + UINavigationSystem::kSliderNavStep; // 0.5 + 0.1*range(=1)
    EXPECT_NEAR(m_Slider.GetComponent<UISliderComponent>().m_Value, expected, 1e-5f);
    EXPECT_NEAR(reported, expected, 1e-5f) << "OnValueChanged should carry the new value";
    EXPECT_EQ(nav.GetFocus(), m_Slider.GetUUID()) << "Left/Right on a slider adjusts, it does not move focus";

    UINavInput left;
    left.NavLeft = true;
    UINavigationSystem::Update(GetScene(), left);
    EXPECT_NEAR(m_Slider.GetComponent<UISliderComponent>().m_Value, 0.5f, 1e-5f);
}

// Activate on a focused checkbox flips it and fires OnValueChanged (1 = checked).
TEST_F(UINavigationTest, CheckboxActivateTogglesAndFiresValueChanged)
{
    f32 reported = -1.0f;
    UINavigation& nav = GetScene().GetUINavigation();
    nav.OnValueChanged(m_Checkbox.GetUUID(), [&reported](f32 v)
                       { reported = v; });
    nav.SetFocus(m_Checkbox.GetUUID());

    UINavInput activate;
    activate.Activate = true;
    UINavigationSystem::Update(GetScene(), activate);

    EXPECT_TRUE(m_Checkbox.GetComponent<UICheckboxComponent>().m_IsChecked);
    EXPECT_NEAR(reported, 1.0f, 1e-5f);
}

// A mouse press-then-release-over a button (Pressed -> Hovered across frames)
// fires the same OnClick delegate — the "binding fires from any source" contract.
TEST_F(UINavigationTest, MousePressReleaseTransitionFiresOnClick)
{
    int clicks = 0;
    UINavigation& nav = GetScene().GetUINavigation();
    nav.OnClick(m_Button0.GetUUID(), [&clicks]
                { ++clicks; });

    auto& button = m_Button0.GetComponent<UIButtonComponent>();

    UINavInput none;
    UINavigationSystem::Update(GetScene(), none); // baseline snapshot (Normal)

    button.m_State = UIButtonState::Pressed; // mouse down over the button
    UINavigationSystem::Update(GetScene(), none);
    EXPECT_EQ(clicks, 0) << "still held — no click yet";

    button.m_State = UIButtonState::Hovered; // released while still over
    UINavigationSystem::Update(GetScene(), none);
    EXPECT_EQ(clicks, 1) << "release-over should fire OnClick";
}

// Cancel clears focus (menu-close policy is the app's; navigation just drops it).
TEST_F(UINavigationTest, CancelClearsFocus)
{
    UINavigation& nav = GetScene().GetUINavigation();
    nav.SetFocus(m_Button1.GetUUID());
    ASSERT_TRUE(nav.HasFocus());

    UINavInput cancel;
    cancel.Cancel = true;
    UINavigationSystem::Update(GetScene(), cancel);

    EXPECT_FALSE(nav.HasFocus());
}

// Focus pointing at an entity destroyed since last frame must not crash: the
// stale-UUID lookup has to be find-or-null, not the asserting GetEntityByUUID.
TEST_F(UINavigationTest, StaleFocusAfterEntityDestroyedDoesNotCrash)
{
    UINavigation& nav = GetScene().GetUINavigation();
    nav.SetFocus(m_Button1.GetUUID());
    ASSERT_TRUE(nav.HasFocus());

    GetScene().DestroyEntity(m_Button1); // focus now dangles

    UINavInput activate; // exercises the ActivateFocused stale path too
    activate.Activate = true;
    UINavigationSystem::Update(GetScene(), activate); // must not assert / crash

    EXPECT_FALSE(nav.HasFocus()) << "focus on a destroyed widget should be dropped";
}

// A value-changed delegate that destroys another widget must be safe: delegates
// fire after view iteration completes, so the registry mutation can't invalidate
// a live entt iterator mid-dispatch.
TEST_F(UINavigationTest, DelegateMutatingSceneDuringDispatchIsSafe)
{
    UINavigation& nav = GetScene().GetUINavigation();
    Entity victim = m_Button2;
    const UUID victimId = victim.GetUUID(); // capture before destruction (handle goes stale)
    nav.OnValueChanged(m_Slider.GetUUID(), [this, victim]([[maybe_unused]] f32 v) mutable
                       { GetScene().DestroyEntity(victim); });
    nav.SetFocus(m_Slider.GetUUID());

    UINavInput right;
    right.NavRight = true;
    UINavigationSystem::Update(GetScene(), right); // slider change -> handler destroys Button2

    EXPECT_FALSE(GetScene().TryGetEntityWithUUID(victimId).has_value())
        << "the delegate should have run and destroyed the victim widget";
}
