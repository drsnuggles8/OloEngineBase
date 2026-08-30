#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// InputActionManager's Steam Input routing (#893) — "Steam Input wins when available", see
// docs/agent-rules/steamworks-platform-integration.md §11.
//
// Drives InputActionManager::Update() with SteamManager pointed at a FakeSteamBackend, so the
// routing decision (activate the right action set, prefer Steam digital/analog state for
// gamepad-origin bindings, never touch keyboard/mouse bindings, don't clobber a real Steam
// analog magnitude with the "snap partial axis to full scale" fallback meant for the raw-gamepad
// path) is exercised without a Steam client or a physical controller.

#include "FakeSteamBackend.h"

#include "OloEngine/Core/IInputProvider.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "Platform/Steam/SteamManager.h"

using OloEngine::GamepadAxis;
using OloEngine::GamepadButton;
using OloEngine::InputAction;
using OloEngine::InputActionManager;
using OloEngine::InputActionMap;
using OloEngine::InputBinding;
using OloEngine::InputContextType;
namespace Key = OloEngine::Key;
using OloEngine::SteamManager;
using OloEngine::Testing::FakeSteamBackend;

namespace
{
    // A minimal provider that reports nothing pressed — isolates the tests below to Steam
    // Input's contribution rather than whatever GLFW/default gamepad state happens to be live.
    class InertInputProvider final : public OloEngine::IInputProvider
    {
      public:
        [[nodiscard]] bool IsKeyPressed(OloEngine::KeyCode) const override
        {
            return false;
        }
        [[nodiscard]] bool IsMouseButtonPressed(OloEngine::MouseCode) const override
        {
            return false;
        }
    };

    class SteamInputRoutingTest : public ::testing::Test
    {
      protected:
        InertInputProvider m_Provider;
        FakeSteamBackend* m_Fake = nullptr;

        void SetUp() override
        {
            auto backend = OloEngine::CreateScope<FakeSteamBackend>();
            m_Fake = backend.get();
            SteamManager::SetBackendForTesting(std::move(backend));
            SteamManager::Initialize();
            ASSERT_TRUE(SteamManager::IsInputAvailable());

            InputActionManager::Init();
            InputActionManager::SetInputProvider(&m_Provider);
        }

        void TearDown() override
        {
            InputActionManager::SetInputProvider(nullptr);
            InputActionManager::Shutdown();
            SteamManager::ResetForTesting();
            m_Fake = nullptr;
        }
    };

    TEST_F(SteamInputRoutingTest, NoConnectedControllerFallsBackToEngineBindingsUnchanged)
    {
        InputActionMap map;
        map.AddAction({ "Jump", { InputBinding::GamepadBtn(GamepadButton::South) } });
        InputActionManager::SetActionMap(map);

        // No controllers connected on the fake — Steam must contribute nothing, and the
        // GamepadButton binding falls back to the (inert) provider, i.e. not pressed.
        InputActionManager::Update();
        EXPECT_FALSE(InputActionManager::IsActionPressed("Jump"));
    }

    TEST_F(SteamInputRoutingTest, ConnectedControllerActivatesTheContextsActionSet)
    {
        m_Fake->ConnectedControllers = { 1 };
        InputActionManager::SetActionMap(InputActionMap{});

        InputActionManager::Update();

        EXPECT_GE(m_Fake->ActivateActionSetCalls, 1u);
    }

    TEST_F(SteamInputRoutingTest, SteamDigitalActionDrivesAGamepadOriginAction)
    {
        m_Fake->ConnectedControllers = { 1 };
        m_Fake->DigitalActionHandles["Jump"] = 42;
        m_Fake->DigitalActionStates[{ 1, 42 }] = { .Pressed = true, .Active = true };

        InputActionMap map;
        map.AddAction({ "Jump", { InputBinding::GamepadBtn(GamepadButton::South) } });
        InputActionManager::SetActionMap(map);

        InputActionManager::Update();
        EXPECT_TRUE(InputActionManager::IsActionPressed("Jump"));
    }

    // Active=false ("no origin bound in the current action set") must not be treated as
    // "not pressed" in a way that overrides a genuinely-pressed keyboard/mouse binding on the
    // same action — it must simply contribute nothing.
    TEST_F(SteamInputRoutingTest, UnboundSteamActionDoesNotSuppressAKeyboardBinding)
    {
        m_Fake->ConnectedControllers = { 1 };
        // No digital state configured for "Jump" -> Active=false from GetDigitalActionState.

        InputActionMap map;
        map.AddAction({ "Jump", { InputBinding::Key(Key::Space), InputBinding::GamepadBtn(GamepadButton::South) } });
        InputActionManager::SetActionMap(map);

        // Simulate the keyboard press through a provider that reports Space pressed.
        class SpacePressedProvider final : public OloEngine::IInputProvider
        {
          public:
            [[nodiscard]] bool IsKeyPressed(OloEngine::KeyCode key) const override
            {
                return key == Key::Space;
            }
            [[nodiscard]] bool IsMouseButtonPressed(OloEngine::MouseCode) const override
            {
                return false;
            }
        } provider;
        InputActionManager::SetInputProvider(&provider);

        InputActionManager::Update();
        EXPECT_TRUE(InputActionManager::IsActionPressed("Jump"));

        InputActionManager::SetInputProvider(&m_Provider);
    }

    TEST_F(SteamInputRoutingTest, SteamAnalogMagnitudeIsNotSnappedToFullScale)
    {
        m_Fake->ConnectedControllers = { 1 };
        m_Fake->AnalogActionHandles["Throttle"] = 43;
        // A real 40% pull, alongside a digitally-bound "pressed past threshold" origin on the
        // SAME action — the shape the review flagged: without the fix, the pre-existing
        // "snap partial axis to full scale when also pressed" fallback (meant for a weak/absent
        // raw-gamepad axis) would clobber this into 1.0.
        m_Fake->DigitalActionHandles["Throttle"] = 44;
        m_Fake->DigitalActionStates[{ 1, 44 }] = { .Pressed = true, .Active = true };
        m_Fake->AnalogActionStates[{ 1, 43 }] = { .X = 0.4f, .Y = 0.0f, .Active = true };

        InputActionMap map;
        map.AddAction({ "Throttle", { InputBinding::GamepadAx(GamepadAxis::RightTrigger) } });
        InputActionManager::SetActionMap(map);

        InputActionManager::Update();
        EXPECT_TRUE(InputActionManager::IsActionPressed("Throttle"));
        EXPECT_FLOAT_EQ(InputActionManager::GetActionAxisValue("Throttle"), 0.4f);
    }

    // The action's own AxisPositive constraint (the split-into-two-actions convention, e.g.
    // "MoveLeft" bound to the negative half of an axis) must still gate the Steam-sourced value.
    TEST_F(SteamInputRoutingTest, SteamAnalogValueRespectsTheBindingsAxisDirection)
    {
        m_Fake->ConnectedControllers = { 1 };
        m_Fake->AnalogActionHandles["MoveLeft"] = 45;
        // Steam reports a POSITIVE deflection; the engine binding constrains this action to the
        // negative half of the axis, so it must clamp to 0, not report a positive value for a
        // "left" action.
        m_Fake->AnalogActionStates[{ 1, 45 }] = { .X = 0.7f, .Y = 0.0f, .Active = true };

        InputActionMap map;
        map.AddAction({ "MoveLeft", { InputBinding::GamepadAx(GamepadAxis::LeftX, 0.5f, /*positive*/ false) } });
        InputActionManager::SetActionMap(map);

        InputActionManager::Update();
        EXPECT_FLOAT_EQ(InputActionManager::GetActionAxisValue("MoveLeft"), 0.0f);
    }

    TEST_F(SteamInputRoutingTest, DisconnectingTheControllerFallsBackCleanly)
    {
        m_Fake->ConnectedControllers = { 1 };
        m_Fake->DigitalActionHandles["Jump"] = 42;
        m_Fake->DigitalActionStates[{ 1, 42 }] = { .Pressed = true, .Active = true };

        InputActionMap map;
        map.AddAction({ "Jump", { InputBinding::GamepadBtn(GamepadButton::South) } });
        InputActionManager::SetActionMap(map);

        InputActionManager::Update();
        ASSERT_TRUE(InputActionManager::IsActionPressed("Jump"));

        // Controller disconnects mid-session.
        m_Fake->ConnectedControllers.clear();
        InputActionManager::Update();
        EXPECT_FALSE(InputActionManager::IsActionPressed("Jump"))
            << "action stayed pressed after the Steam Input controller disconnected";
    }
} // namespace
