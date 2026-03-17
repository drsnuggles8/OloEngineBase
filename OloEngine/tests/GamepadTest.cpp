#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Core/GamepadCodes.h"
#include "OloEngine/Core/IInputProvider.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputActionSerializer.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine;

// ============================================================================
// Dead zone tests
// ============================================================================

TEST(GamepadDeadzoneTest, InsideDeadzoneReturnsZero)
{
    glm::vec2 input(0.05f, 0.05f);
    glm::vec2 result = ApplyRadialDeadzone(input, 0.15f);
    EXPECT_FLOAT_EQ(result.x, 0.0f);
    EXPECT_FLOAT_EQ(result.y, 0.0f);
}

TEST(GamepadDeadzoneTest, ZeroInputReturnsZero)
{
    glm::vec2 result = ApplyRadialDeadzone({ 0.0f, 0.0f }, 0.15f);
    EXPECT_FLOAT_EQ(result.x, 0.0f);
    EXPECT_FLOAT_EQ(result.y, 0.0f);
}

TEST(GamepadDeadzoneTest, FullDeflectionReturnsOne)
{
    glm::vec2 input(1.0f, 0.0f);
    glm::vec2 result = ApplyRadialDeadzone(input, 0.15f);
    // Full deflection should map to magnitude ~1.0
    EXPECT_NEAR(glm::length(result), 1.0f, 0.001f);
}

TEST(GamepadDeadzoneTest, ExactlyAtDeadzoneReturnsZero)
{
    // Right at the deadzone boundary should return zero
    glm::vec2 input(0.15f, 0.0f);
    glm::vec2 result = ApplyRadialDeadzone(input, 0.15f);
    EXPECT_NEAR(glm::length(result), 0.0f, 0.001f);
}

TEST(GamepadDeadzoneTest, JustOutsideDeadzoneReturnsSmallValue)
{
    glm::vec2 input(0.2f, 0.0f);
    glm::vec2 result = ApplyRadialDeadzone(input, 0.15f);
    f32 mag = glm::length(result);
    EXPECT_GT(mag, 0.0f);
    EXPECT_LT(mag, 0.2f); // Should be remapped to a small value
}

TEST(GamepadDeadzoneTest, DirectionIsPreserved)
{
    glm::vec2 input(0.5f, 0.5f);
    glm::vec2 result = ApplyRadialDeadzone(input, 0.15f);
    // Direction should be the same
    glm::vec2 inputDir = glm::normalize(input);
    glm::vec2 resultDir = glm::normalize(result);
    EXPECT_NEAR(inputDir.x, resultDir.x, 0.001f);
    EXPECT_NEAR(inputDir.y, resultDir.y, 0.001f);
}

// ============================================================================
// GamepadButton/Axis enum string conversion tests
// ============================================================================

TEST(GamepadCodesTest, ButtonToString)
{
    EXPECT_STREQ(GamepadButtonToString(GamepadButton::South), "South");
    EXPECT_STREQ(GamepadButtonToString(GamepadButton::East), "East");
    EXPECT_STREQ(GamepadButtonToString(GamepadButton::DPadUp), "DPadUp");
    EXPECT_STREQ(GamepadButtonToString(GamepadButton::Start), "Start");
}

TEST(GamepadCodesTest, AxisToString)
{
    EXPECT_STREQ(GamepadAxisToString(GamepadAxis::LeftX), "LeftX");
    EXPECT_STREQ(GamepadAxisToString(GamepadAxis::RightTrigger), "RightTrigger");
}

TEST(GamepadCodesTest, StringToButton)
{
    EXPECT_EQ(StringToGamepadButton("South"), GamepadButton::South);
    EXPECT_EQ(StringToGamepadButton("DPadLeft"), GamepadButton::DPadLeft);
    EXPECT_EQ(StringToGamepadButton("Invalid"), std::nullopt);
}

TEST(GamepadCodesTest, StringToAxis)
{
    EXPECT_EQ(StringToGamepadAxis("LeftX"), GamepadAxis::LeftX);
    EXPECT_EQ(StringToGamepadAxis("RightTrigger"), GamepadAxis::RightTrigger);
    EXPECT_EQ(StringToGamepadAxis("Invalid"), std::nullopt);
}

// ============================================================================
// InputBinding gamepad factory tests
// ============================================================================

TEST(InputBindingTest, GamepadButtonFactory)
{
    auto binding = InputBinding::GamepadBtn(GamepadButton::South);
    EXPECT_EQ(binding.Type, InputBindingType::GamepadButton);
    EXPECT_EQ(binding.GPButton, GamepadButton::South);
}

TEST(InputBindingTest, GamepadAxisFactory)
{
    auto binding = InputBinding::GamepadAx(GamepadAxis::LeftX, 0.3f, false);
    EXPECT_EQ(binding.Type, InputBindingType::GamepadAxis);
    EXPECT_EQ(binding.GPAxis, GamepadAxis::LeftX);
    EXPECT_FLOAT_EQ(binding.AxisThreshold, 0.3f);
    EXPECT_FALSE(binding.AxisPositive);
}

TEST(InputBindingTest, GamepadButtonDisplayName)
{
    auto binding = InputBinding::GamepadBtn(GamepadButton::South);
    EXPECT_EQ(binding.GetDisplayName(), "Gamepad: South");
}

TEST(InputBindingTest, GamepadAxisDisplayName)
{
    auto binding = InputBinding::GamepadAx(GamepadAxis::LeftX, 0.5f, true);
    EXPECT_EQ(binding.GetDisplayName(), "Gamepad Axis: LeftX +");
}

// ============================================================================
// InputActionManager with gamepad bindings (mock provider)
// ============================================================================

class MockGamepadInputProvider : public IInputProvider
{
public:
    [[nodiscard]] bool IsKeyPressed([[maybe_unused]] KeyCode key) const override { return false; }
    [[nodiscard]] bool IsMouseButtonPressed([[maybe_unused]] MouseCode button) const override { return false; }

    [[nodiscard]] bool IsGamepadButtonPressed(GamepadButton button, [[maybe_unused]] i32 gamepadIndex) const override
    {
        auto idx = static_cast<u32>(button);
        return idx < m_Buttons.size() && m_Buttons[idx];
    }

    [[nodiscard]] f32 GetGamepadAxis(GamepadAxis axis, [[maybe_unused]] i32 gamepadIndex) const override
    {
        auto idx = static_cast<u32>(axis);
        return (idx < m_Axes.size()) ? m_Axes[idx] : 0.0f;
    }

    std::array<bool, 15> m_Buttons{};
    std::array<f32, 6> m_Axes{};
};

class GamepadActionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        InputActionManager::Init();
        InputActionManager::SetInputProvider(&m_Provider);
    }

    void TearDown() override
    {
        InputActionManager::SetInputProvider(nullptr);
        InputActionManager::Shutdown();
    }

    MockGamepadInputProvider m_Provider;
};

TEST_F(GamepadActionTest, GamepadButtonActionPressed)
{
    InputActionMap map;
    map.Name = "TestMap";
    map.AddAction({ "Jump", { InputBinding::GamepadBtn(GamepadButton::South) } });
    InputActionManager::SetActionMap(map);

    m_Provider.m_Buttons[static_cast<u32>(GamepadButton::South)] = true;
    InputActionManager::Update();

    EXPECT_TRUE(InputActionManager::IsActionPressed("Jump"));
    EXPECT_TRUE(InputActionManager::IsActionJustPressed("Jump"));
}

TEST_F(GamepadActionTest, GamepadButtonActionReleased)
{
    InputActionMap map;
    map.Name = "TestMap";
    map.AddAction({ "Jump", { InputBinding::GamepadBtn(GamepadButton::South) } });
    InputActionManager::SetActionMap(map);

    // Press
    m_Provider.m_Buttons[static_cast<u32>(GamepadButton::South)] = true;
    InputActionManager::Update();

    // Release
    m_Provider.m_Buttons[static_cast<u32>(GamepadButton::South)] = false;
    InputActionManager::Update();

    EXPECT_FALSE(InputActionManager::IsActionPressed("Jump"));
    EXPECT_TRUE(InputActionManager::IsActionJustReleased("Jump"));
}

TEST_F(GamepadActionTest, GamepadAxisPositiveThreshold)
{
    InputActionMap map;
    map.Name = "TestMap";
    map.AddAction({ "MoveRight", { InputBinding::GamepadAx(GamepadAxis::LeftX, 0.5f, true) } });
    InputActionManager::SetActionMap(map);

    // Below threshold
    m_Provider.m_Axes[static_cast<u32>(GamepadAxis::LeftX)] = 0.3f;
    InputActionManager::Update();
    EXPECT_FALSE(InputActionManager::IsActionPressed("MoveRight"));

    // Above threshold
    m_Provider.m_Axes[static_cast<u32>(GamepadAxis::LeftX)] = 0.7f;
    InputActionManager::Update();
    EXPECT_TRUE(InputActionManager::IsActionPressed("MoveRight"));
}

TEST_F(GamepadActionTest, GamepadAxisNegativeThreshold)
{
    InputActionMap map;
    map.Name = "TestMap";
    map.AddAction({ "MoveLeft", { InputBinding::GamepadAx(GamepadAxis::LeftX, 0.5f, false) } });
    InputActionManager::SetActionMap(map);

    // Positive direction shouldnt trigger
    m_Provider.m_Axes[static_cast<u32>(GamepadAxis::LeftX)] = 0.8f;
    InputActionManager::Update();
    EXPECT_FALSE(InputActionManager::IsActionPressed("MoveLeft"));

    // Negative direction above threshold
    m_Provider.m_Axes[static_cast<u32>(GamepadAxis::LeftX)] = -0.7f;
    InputActionManager::Update();
    EXPECT_TRUE(InputActionManager::IsActionPressed("MoveLeft"));
}

TEST_F(GamepadActionTest, MixedKeyboardAndGamepadBindings)
{
    InputActionMap map;
    map.Name = "TestMap";
    map.AddAction({ "Jump", {
        InputBinding::Key(Key::Space),
        InputBinding::GamepadBtn(GamepadButton::South)
    } });
    InputActionManager::SetActionMap(map);

    // Only gamepad pressed
    m_Provider.m_Buttons[static_cast<u32>(GamepadButton::South)] = true;
    InputActionManager::Update();
    EXPECT_TRUE(InputActionManager::IsActionPressed("Jump"));
}

// ============================================================================
// Serialization round-trip with gamepad bindings
// ============================================================================

TEST(GamepadSerializationTest, RoundTrip)
{
    InputActionMap map;
    map.Name = "GamepadTestMap";
    map.AddAction({ "Jump", {
        InputBinding::Key(Key::Space),
        InputBinding::GamepadBtn(GamepadButton::South)
    } });
    map.AddAction({ "MoveX", {
        InputBinding::GamepadAx(GamepadAxis::LeftX, 0.4f, true)
    } });

    std::filesystem::path tempPath = std::filesystem::temp_directory_path() / "gamepad_test_actions.yaml";
    ASSERT_TRUE(InputActionSerializer::Serialize(map, tempPath));

    auto loaded = InputActionSerializer::Deserialize(tempPath);
    ASSERT_TRUE(loaded.has_value());

    EXPECT_EQ(loaded->Name, "GamepadTestMap");

    auto* jumpAction = loaded->GetAction("Jump");
    ASSERT_NE(jumpAction, nullptr);
    ASSERT_EQ(jumpAction->Bindings.size(), 2u);
    EXPECT_EQ(jumpAction->Bindings[0].Type, InputBindingType::Keyboard);
    EXPECT_EQ(jumpAction->Bindings[0].Code, Key::Space);
    EXPECT_EQ(jumpAction->Bindings[1].Type, InputBindingType::GamepadButton);
    EXPECT_EQ(jumpAction->Bindings[1].GPButton, GamepadButton::South);

    auto* moveXAction = loaded->GetAction("MoveX");
    ASSERT_NE(moveXAction, nullptr);
    ASSERT_EQ(moveXAction->Bindings.size(), 1u);
    EXPECT_EQ(moveXAction->Bindings[0].Type, InputBindingType::GamepadAxis);
    EXPECT_EQ(moveXAction->Bindings[0].GPAxis, GamepadAxis::LeftX);
    EXPECT_FLOAT_EQ(moveXAction->Bindings[0].AxisThreshold, 0.4f);
    EXPECT_TRUE(moveXAction->Bindings[0].AxisPositive);

    // Cleanup
    std::filesystem::remove(tempPath);
}
