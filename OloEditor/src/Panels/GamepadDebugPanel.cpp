#include "OloEnginePCH.h"
#include "GamepadDebugPanel.h"

#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Core/GamepadCodes.h"
#include "OloEngine/Core/GamepadManager.h"

#include <imgui.h>

namespace OloEngine
{
    void GamepadDebugPanel::OnImGuiRender(bool* p_open)
    {
        if (!ImGui::Begin("Gamepad Debug", p_open))
        {
            ImGui::End();
            return;
        }

        // Active input device
        InputDevice activeDevice = GamepadManager::GetActiveDevice();
        ImGui::Text("Active Device: %s", activeDevice == InputDevice::GamepadDevice ? "Gamepad" : "Keyboard/Mouse");
        ImGui::Text("Connected Gamepads: %d", GamepadManager::GetConnectedCount());
        ImGui::Separator();

        for (i32 i = 0; i < GamepadManager::MaxGamepads; ++i)
        {
            auto* gp = GamepadManager::GetGamepad(i);
            if (!gp || !gp->IsConnected())
            {
                continue;
            }

            ImGui::PushID(i);
            if (ImGui::TreeNodeEx("##Gamepad", ImGuiTreeNodeFlags_DefaultOpen, "Gamepad %d: %s", i, gp->GetName().c_str()))
            {
                // Buttons
                ImGui::Text("Buttons:");
                ImGui::Indent();

                for (u32 b = 0; b < static_cast<u32>(GamepadButton::Count); ++b)
                {
                    auto btn = static_cast<GamepadButton>(b);
                    bool pressed = gp->IsButtonPressed(btn);
                    bool justPressed = gp->IsButtonJustPressed(btn);

                    ImVec4 color = pressed ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                    if (justPressed)
                    {
                        color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
                    }

                    ImGui::TextColored(color, "%s: %s", GamepadButtonToString(btn), pressed ? "PRESSED" : "---");
                }
                ImGui::Unindent();

                ImGui::Spacing();

                // Axes
                ImGui::Text("Axes:");
                ImGui::Indent();
                for (u32 a = 0; a < static_cast<u32>(GamepadAxis::Count); ++a)
                {
                    auto axis = static_cast<GamepadAxis>(a);
                    f32 value = gp->GetAxis(axis);
                    ImGui::Text("%-14s: %+.3f", GamepadAxisToString(axis), static_cast<double>(value));
                    ImGui::SameLine();
                    ImGui::ProgressBar((value + 1.0f) * 0.5f, ImVec2(100.0f, 0.0f), "");
                }
                ImGui::Unindent();

                ImGui::Spacing();

                // Stick visualizer
                ImGui::Text("Sticks (with deadzone):");
                glm::vec2 leftStick = gp->GetLeftStickDeadzone(0.15f);
                glm::vec2 rightStick = gp->GetRightStickDeadzone(0.15f);

                auto DrawStickWidget = [](const char* label, glm::vec2 stick)
                {
                    ImGui::Text("%s: (%.2f, %.2f)", label, static_cast<double>(stick.x), static_cast<double>(stick.y));

                    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                    constexpr f32 radius = 50.0f;
                    ImVec2 center = ImVec2(canvasPos.x + radius, canvasPos.y + radius);

                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddCircleFilled(center, radius, IM_COL32(40, 40, 40, 255));
                    drawList->AddCircle(center, radius, IM_COL32(100, 100, 100, 255));

                    // Dead zone ring
                    drawList->AddCircle(center, radius * 0.15f, IM_COL32(80, 80, 80, 128));

                    // Stick position
                    ImVec2 dotPos = ImVec2(
                        center.x + stick.x * radius,
                        center.y + stick.y * radius);
                    drawList->AddCircleFilled(dotPos, 6.0f, IM_COL32(255, 80, 80, 255));

                    ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f));
                };

                ImGui::Columns(2, "StickColumns", false);
                DrawStickWidget("Left Stick", leftStick);
                ImGui::NextColumn();
                DrawStickWidget("Right Stick", rightStick);
                ImGui::Columns(1);

                // Triggers
                ImGui::Text("Triggers:");
                f32 lt = gp->GetAxis(GamepadAxis::LeftTrigger);
                f32 rt = gp->GetAxis(GamepadAxis::RightTrigger);
                ImGui::ProgressBar((lt + 1.0f) * 0.5f, ImVec2(150.0f, 0.0f), "LT");
                ImGui::SameLine();
                ImGui::ProgressBar((rt + 1.0f) * 0.5f, ImVec2(150.0f, 0.0f), "RT");

                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        if (GamepadManager::GetConnectedCount() == 0)
        {
            ImGui::TextWrapped("No gamepads connected. Connect a controller and it will appear here.");
        }

        ImGui::End();
    }

} // namespace OloEngine
