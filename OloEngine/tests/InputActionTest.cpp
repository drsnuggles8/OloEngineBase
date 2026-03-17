#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Core/IInputProvider.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputActionSerializer.h"

#include <filesystem>
#include <fstream>
#include <unordered_set>

using namespace OloEngine;

// ============================================================================
// InputBinding tests
// ============================================================================

TEST(InputBindingTest, KeyFactory)
{
    auto binding = InputBinding::Key(Key::W);
    EXPECT_EQ(binding.Type, InputBindingType::Keyboard);
    EXPECT_EQ(binding.Code, Key::W);
}

TEST(InputBindingTest, MouseFactory)
{
    auto binding = InputBinding::MouseButton(Mouse::ButtonLeft);
    EXPECT_EQ(binding.Type, InputBindingType::Mouse);
    EXPECT_EQ(binding.Code, Mouse::ButtonLeft);
}

TEST(InputBindingTest, Equality)
{
    auto a = InputBinding::Key(Key::W);
    auto b = InputBinding::Key(Key::W);
    auto c = InputBinding::Key(Key::S);
    auto d = InputBinding::MouseButton(Mouse::ButtonLeft);

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
}

TEST(InputBindingTest, DisplayNameKeyboard)
{
    EXPECT_EQ(InputBinding::Key(Key::W).GetDisplayName(), "Keyboard: W");
    EXPECT_EQ(InputBinding::Key(Key::Space).GetDisplayName(), "Keyboard: Space");
    EXPECT_EQ(InputBinding::Key(Key::Escape).GetDisplayName(), "Keyboard: Escape");
    EXPECT_EQ(InputBinding::Key(Key::F1).GetDisplayName(), "Keyboard: F1");
    EXPECT_EQ(InputBinding::Key(Key::F12).GetDisplayName(), "Keyboard: F12");
    EXPECT_EQ(InputBinding::Key(Key::D0).GetDisplayName(), "Keyboard: 0");
    EXPECT_EQ(InputBinding::Key(Key::KP0).GetDisplayName(), "Keyboard: KP0");
    EXPECT_EQ(InputBinding::Key(Key::LeftShift).GetDisplayName(), "Keyboard: LeftShift");
}

TEST(InputBindingTest, DisplayNameMouse)
{
    EXPECT_EQ(InputBinding::MouseButton(Mouse::ButtonLeft).GetDisplayName(), "Mouse: Left");
    EXPECT_EQ(InputBinding::MouseButton(Mouse::ButtonRight).GetDisplayName(), "Mouse: Right");
    EXPECT_EQ(InputBinding::MouseButton(Mouse::ButtonMiddle).GetDisplayName(), "Mouse: Middle");
    EXPECT_EQ(InputBinding::MouseButton(Mouse::Button4).GetDisplayName(), "Mouse: Button4");
}

// ============================================================================
// InputActionMap tests
// ============================================================================

TEST(InputActionMapTest, AddAndRetrieve)
{
    InputActionMap map;
    map.Name = "TestMap";

    InputAction action;
    action.Name = "Jump";
    action.Bindings.push_back(InputBinding::Key(Key::Space));
    map.AddAction(action);

    EXPECT_TRUE(map.HasAction("Jump"));

    auto* retrieved = map.GetAction("Jump");
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->Name, "Jump");
    ASSERT_EQ(retrieved->Bindings.size(), 1u);
    EXPECT_EQ(retrieved->Bindings[0], InputBinding::Key(Key::Space));
}

TEST(InputActionMapTest, RemoveAction)
{
    InputActionMap map;
    map.AddAction({ "Jump", { InputBinding::Key(Key::Space) } });

    EXPECT_TRUE(map.HasAction("Jump"));
    map.RemoveAction("Jump");
    EXPECT_FALSE(map.HasAction("Jump"));
    EXPECT_EQ(map.GetAction("Jump"), nullptr);
}

TEST(InputActionMapTest, HasAction)
{
    InputActionMap map;
    EXPECT_FALSE(map.HasAction("NonExistent"));

    map.AddAction({ "Fire", { InputBinding::MouseButton(Mouse::ButtonLeft) } });
    EXPECT_TRUE(map.HasAction("Fire"));
    EXPECT_FALSE(map.HasAction("fire")); // case-sensitive
}

TEST(InputActionMapTest, DuplicateAddOverwrites)
{
    InputActionMap map;
    map.AddAction({ "Jump", { InputBinding::Key(Key::Space) } });
    map.AddAction({ "Jump", { InputBinding::Key(Key::W) } });

    auto* action = map.GetAction("Jump");
    ASSERT_NE(action, nullptr);
    ASSERT_EQ(action->Bindings.size(), 1u);
    EXPECT_EQ(action->Bindings[0], InputBinding::Key(Key::W));
}

TEST(InputActionMapTest, EmptyMap)
{
    InputActionMap map;
    EXPECT_EQ(map.GetAction("Anything"), nullptr);
    EXPECT_FALSE(map.HasAction("Anything"));
}

TEST(InputActionMapTest, MultipleBindings)
{
    InputActionMap map;
    map.AddAction({ "MoveUp", { InputBinding::Key(Key::W), InputBinding::Key(Key::Up) } });

    auto* action = map.GetAction("MoveUp");
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->Bindings.size(), 2u);
}

// ============================================================================
// InputActionManager tests
// ============================================================================

TEST(InputActionManagerTest, UnknownActionReturnsFalse)
{
    InputActionManager::Init();
    EXPECT_FALSE(InputActionManager::IsActionPressed("NonExistent"));
    EXPECT_FALSE(InputActionManager::IsActionJustPressed("NonExistent"));
    EXPECT_FALSE(InputActionManager::IsActionJustReleased("NonExistent"));
    InputActionManager::Shutdown();
}

TEST(InputActionManagerTest, SetActionMapClearsState)
{
    InputActionManager::Init();

    InputActionMap map;
    map.Name = "Test";
    map.AddAction({ "Jump", { InputBinding::Key(Key::Space) } });
    InputActionManager::SetActionMap(map);

    // After setting a new map, state should be clear
    EXPECT_FALSE(InputActionManager::IsActionPressed("Jump"));
    EXPECT_FALSE(InputActionManager::IsActionJustPressed("Jump"));
    EXPECT_FALSE(InputActionManager::IsActionJustReleased("Jump"));

    InputActionManager::Shutdown();
}

TEST(InputActionManagerTest, GetActionMapReturnsReference)
{
    InputActionManager::Init();

    InputActionMap map;
    map.Name = "EditorTest";
    map.AddAction({ "Fire", { InputBinding::MouseButton(Mouse::ButtonLeft) } });
    InputActionManager::SetActionMap(map);

    auto& ref = InputActionManager::GetActionMap();
    EXPECT_EQ(ref.Name, "EditorTest");
    EXPECT_TRUE(ref.HasAction("Fire"));

    InputActionManager::Shutdown();
}

// ============================================================================
// CreateDefaultGameActions tests
// ============================================================================

TEST(DefaultGameActionsTest, HasExpectedActions)
{
    auto map = CreateDefaultGameActions();
    EXPECT_EQ(map.Name, "DefaultGameActions");
    EXPECT_TRUE(map.HasAction("MoveUp"));
    EXPECT_TRUE(map.HasAction("MoveDown"));
    EXPECT_TRUE(map.HasAction("MoveLeft"));
    EXPECT_TRUE(map.HasAction("MoveRight"));
    EXPECT_TRUE(map.HasAction("Jump"));
    EXPECT_TRUE(map.HasAction("Interact"));
}

TEST(DefaultGameActionsTest, MoveUpHasWAndArrowAndDPad)
{
    auto map = CreateDefaultGameActions();
    auto* action = map.GetAction("MoveUp");
    ASSERT_NE(action, nullptr);
    ASSERT_EQ(action->Bindings.size(), 3u);
    EXPECT_EQ(action->Bindings[0], InputBinding::Key(Key::W));
    EXPECT_EQ(action->Bindings[1], InputBinding::Key(Key::Up));
    EXPECT_EQ(action->Bindings[2], InputBinding::GamepadBtn(GamepadButton::DPadUp));
}

// ============================================================================
// Serialization round-trip tests
// ============================================================================

class InputActionSerializerTest : public ::testing::Test
{
  protected:
    std::filesystem::path m_TempDir;

    void SetUp() override
    {
        m_TempDir = std::filesystem::temp_directory_path() / "OloEngine_InputActionTest";
        std::filesystem::create_directories(m_TempDir);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
    }
};

TEST_F(InputActionSerializerTest, RoundTrip)
{
    InputActionMap original;
    original.Name = "TestMap";
    original.AddAction({ "MoveUp", { InputBinding::Key(Key::W), InputBinding::Key(Key::Up) } });
    original.AddAction({ "Fire", { InputBinding::MouseButton(Mouse::ButtonLeft) } });
    original.AddAction({ "Interact", { InputBinding::Key(Key::E) } });

    auto filepath = m_TempDir / "test_actions.yaml";
    ASSERT_TRUE(InputActionSerializer::Serialize(original, filepath));

    auto result = InputActionSerializer::Deserialize(filepath);
    ASSERT_TRUE(result.has_value());

    const auto& loaded = *result;
    EXPECT_EQ(loaded.Name, original.Name);
    EXPECT_EQ(loaded.Actions.size(), original.Actions.size());

    // Verify each action
    for (const auto& [name, action] : original.Actions)
    {
        auto* loadedAction = loaded.GetAction(name);
        ASSERT_NE(loadedAction, nullptr) << "Missing action: " << name;
        EXPECT_EQ(loadedAction->Name, action.Name);
        ASSERT_EQ(loadedAction->Bindings.size(), action.Bindings.size());
        for (sizet i = 0; i < action.Bindings.size(); ++i)
        {
            EXPECT_EQ(loadedAction->Bindings[i], action.Bindings[i])
                << "Binding mismatch at index " << i << " for action " << name;
        }
    }
}

TEST_F(InputActionSerializerTest, EmptyMap)
{
    InputActionMap original;
    original.Name = "Empty";

    auto filepath = m_TempDir / "empty.yaml";
    ASSERT_TRUE(InputActionSerializer::Serialize(original, filepath));

    auto result = InputActionSerializer::Deserialize(filepath);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->Name, "Empty");
    EXPECT_TRUE(result->Actions.empty());
}

TEST_F(InputActionSerializerTest, InvalidFile)
{
    auto result = InputActionSerializer::Deserialize(m_TempDir / "nonexistent.yaml");
    EXPECT_FALSE(result.has_value());
}

TEST_F(InputActionSerializerTest, MalformedYAML)
{
    auto filepath = m_TempDir / "malformed.yaml";
    {
        std::ofstream fout(filepath);
        fout << "{{{{ not valid yaml !@#$";
    }

    auto result = InputActionSerializer::Deserialize(filepath);
    EXPECT_FALSE(result.has_value());
}

TEST_F(InputActionSerializerTest, MissingFields)
{
    // YAML with a binding missing the Code field
    auto filepath = m_TempDir / "missing_fields.yaml";
    {
        std::ofstream fout(filepath);
        fout << R"(
InputActionMap:
  Name: Partial
  Actions:
    - Name: TestAction
      Bindings:
        - Type: Keyboard
        - Type: Mouse
          Code: 0
    - Bindings:
        - Type: Keyboard
          Code: 32
)";
    }

    auto result = InputActionSerializer::Deserialize(filepath);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->Name, "Partial");

    // Action with missing Name should be skipped
    // TestAction should exist but only have the valid binding (Mouse:0)
    auto* action = result->GetAction("TestAction");
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->Bindings.size(), 1u); // only the one with both Type and Code
    EXPECT_EQ(action->Bindings[0].Type, InputBindingType::Mouse);
    EXPECT_EQ(action->Bindings[0].Code, 0);
}

TEST_F(InputActionSerializerTest, UnknownBindingType)
{
    auto filepath = m_TempDir / "unknown_type.yaml";
    {
        std::ofstream fout(filepath);
        fout << R"(
InputActionMap:
  Name: TestUnknown
  Actions:
    - Name: TestAction
      Bindings:
        - Type: Gamepad
          Code: 1
        - Type: Keyboard
          Code: 87
)";
    }

    auto result = InputActionSerializer::Deserialize(filepath);
    ASSERT_TRUE(result.has_value());

    auto* action = result->GetAction("TestAction");
    ASSERT_NE(action, nullptr);
    // Only the Keyboard binding should survive
    EXPECT_EQ(action->Bindings.size(), 1u);
    EXPECT_EQ(action->Bindings[0].Type, InputBindingType::Keyboard);
    EXPECT_EQ(action->Bindings[0].Code, Key::W);
}

// ============================================================================
// MockInputProvider for state-transition tests
// ============================================================================

class MockInputProvider final : public OloEngine::IInputProvider
{
  public:
    [[nodiscard]] bool IsKeyPressed(OloEngine::KeyCode key) const override
    {
        return m_PressedKeys.contains(key);
    }

    [[nodiscard]] bool IsMouseButtonPressed(OloEngine::MouseCode button) const override
    {
        return m_PressedMouseButtons.contains(button);
    }

    void PressKey(OloEngine::KeyCode key)
    {
        m_PressedKeys.insert(key);
    }
    void ReleaseKey(OloEngine::KeyCode key)
    {
        m_PressedKeys.erase(key);
    }
    void PressMouseButton(OloEngine::MouseCode button)
    {
        m_PressedMouseButtons.insert(button);
    }
    void ReleaseMouseButton(OloEngine::MouseCode button)
    {
        m_PressedMouseButtons.erase(button);
    }
    void ReleaseAll()
    {
        m_PressedKeys.clear();
        m_PressedMouseButtons.clear();
    }

  private:
    std::unordered_set<OloEngine::KeyCode> m_PressedKeys;
    std::unordered_set<OloEngine::MouseCode> m_PressedMouseButtons;
};

// ============================================================================
// State-transition tests (using MockInputProvider)
// ============================================================================

class InputStateTransitionTest : public ::testing::Test
{
  protected:
    MockInputProvider m_Mock;

    void SetUp() override
    {
        InputActionManager::Init();
        InputActionManager::SetInputProvider(&m_Mock);

        InputActionMap map;
        map.Name = "TestMap";
        map.AddAction({ "Jump", { InputBinding::Key(Key::Space) } });
        map.AddAction({ "Fire", { InputBinding::MouseButton(Mouse::ButtonLeft) } });
        map.AddAction({ "MoveUp", { InputBinding::Key(Key::W), InputBinding::Key(Key::Up) } });
        InputActionManager::SetActionMap(map);
    }

    void TearDown() override
    {
        InputActionManager::SetInputProvider(nullptr); // restore default
        InputActionManager::Shutdown();
    }
};

TEST_F(InputStateTransitionTest, KeyPressedOnFirstFrame)
{
    // Frame 1: key down
    m_Mock.PressKey(Key::Space);
    InputActionManager::Update();

    EXPECT_TRUE(InputActionManager::IsActionPressed("Jump"));
    EXPECT_TRUE(InputActionManager::IsActionJustPressed("Jump"));
    EXPECT_FALSE(InputActionManager::IsActionJustReleased("Jump"));
}

TEST_F(InputStateTransitionTest, KeyHeldAcrossFrames)
{
    // Frame 1: press
    m_Mock.PressKey(Key::Space);
    InputActionManager::Update();

    // Frame 2: still held
    InputActionManager::Update();

    EXPECT_TRUE(InputActionManager::IsActionPressed("Jump"));
    EXPECT_FALSE(InputActionManager::IsActionJustPressed("Jump")); // not "just" anymore
    EXPECT_FALSE(InputActionManager::IsActionJustReleased("Jump"));
}

TEST_F(InputStateTransitionTest, KeyReleasedAfterHeld)
{
    // Frame 1: press
    m_Mock.PressKey(Key::Space);
    InputActionManager::Update();

    // Frame 2: release
    m_Mock.ReleaseKey(Key::Space);
    InputActionManager::Update();

    EXPECT_FALSE(InputActionManager::IsActionPressed("Jump"));
    EXPECT_FALSE(InputActionManager::IsActionJustPressed("Jump"));
    EXPECT_TRUE(InputActionManager::IsActionJustReleased("Jump"));
}

TEST_F(InputStateTransitionTest, JustReleasedOnlyOneFrame)
{
    // Frame 1: press
    m_Mock.PressKey(Key::Space);
    InputActionManager::Update();

    // Frame 2: release
    m_Mock.ReleaseKey(Key::Space);
    InputActionManager::Update();

    // Frame 3: still released
    InputActionManager::Update();

    EXPECT_FALSE(InputActionManager::IsActionPressed("Jump"));
    EXPECT_FALSE(InputActionManager::IsActionJustPressed("Jump"));
    EXPECT_FALSE(InputActionManager::IsActionJustReleased("Jump")); // gone after one frame
}

TEST_F(InputStateTransitionTest, MouseButtonTransitions)
{
    // Frame 1: click
    m_Mock.PressMouseButton(Mouse::ButtonLeft);
    InputActionManager::Update();

    EXPECT_TRUE(InputActionManager::IsActionPressed("Fire"));
    EXPECT_TRUE(InputActionManager::IsActionJustPressed("Fire"));

    // Frame 2: release
    m_Mock.ReleaseMouseButton(Mouse::ButtonLeft);
    InputActionManager::Update();

    EXPECT_FALSE(InputActionManager::IsActionPressed("Fire"));
    EXPECT_TRUE(InputActionManager::IsActionJustReleased("Fire"));
}

TEST_F(InputStateTransitionTest, MultipleBindingsSameAction)
{
    // MoveUp is bound to both W and Up arrow — pressing either should trigger it
    m_Mock.PressKey(Key::Up);
    InputActionManager::Update();

    EXPECT_TRUE(InputActionManager::IsActionPressed("MoveUp"));
    EXPECT_TRUE(InputActionManager::IsActionJustPressed("MoveUp"));

    // Switch to W (release Up, press W) — still pressed, not "just pressed"
    m_Mock.ReleaseKey(Key::Up);
    m_Mock.PressKey(Key::W);
    InputActionManager::Update();

    EXPECT_TRUE(InputActionManager::IsActionPressed("MoveUp"));
    EXPECT_FALSE(InputActionManager::IsActionJustPressed("MoveUp"));
    EXPECT_FALSE(InputActionManager::IsActionJustReleased("MoveUp"));
}

TEST_F(InputStateTransitionTest, RapidPressReleaseCycle)
{
    // Simulate rapid tap: press → release → press across 3 frames
    m_Mock.PressKey(Key::Space);
    InputActionManager::Update();
    EXPECT_TRUE(InputActionManager::IsActionJustPressed("Jump"));

    m_Mock.ReleaseKey(Key::Space);
    InputActionManager::Update();
    EXPECT_TRUE(InputActionManager::IsActionJustReleased("Jump"));

    m_Mock.PressKey(Key::Space);
    InputActionManager::Update();
    EXPECT_TRUE(InputActionManager::IsActionJustPressed("Jump")); // should fire again
}

TEST_F(InputStateTransitionTest, UnboundActionStaysIdle)
{
    m_Mock.PressKey(Key::Space);
    InputActionManager::Update();

    // "Fire" is mouse-only — keyboard presses shouldn't affect it
    EXPECT_FALSE(InputActionManager::IsActionPressed("Fire"));
    EXPECT_FALSE(InputActionManager::IsActionJustPressed("Fire"));
}

TEST_F(InputStateTransitionTest, StaleStateCleanedAfterActionRemoval)
{
    // Frame 1: press Jump
    m_Mock.PressKey(Key::Space);
    InputActionManager::Update();
    EXPECT_TRUE(InputActionManager::IsActionPressed("Jump"));

    // Remove the action from the map
    InputActionManager::GetActionMap().RemoveAction("Jump");

    // Frame 2: update should prune stale state
    InputActionManager::Update();
    EXPECT_FALSE(InputActionManager::IsActionPressed("Jump"));
    EXPECT_FALSE(InputActionManager::IsActionJustReleased("Jump"));
}
