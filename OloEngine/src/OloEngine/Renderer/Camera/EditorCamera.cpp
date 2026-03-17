#include "OloEnginePCH.h"
#include "EditorCamera.h"

#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace OloEngine
{
    EditorCamera::EditorCamera(const f32 fov, const f32 aspectRatio, const f32 nearClip, const f32 farClip)
        : Camera(glm::perspective(glm::radians(fov), aspectRatio, nearClip, farClip)), m_FOV(fov), m_AspectRatio(aspectRatio), m_NearClip(nearClip), m_FarClip(farClip)
    {
        UpdateView();
    }

    void EditorCamera::UpdateProjection()
    {
        m_AspectRatio = m_ViewportWidth / m_ViewportHeight;
        m_Projection = glm::perspective(glm::radians(m_FOV), m_AspectRatio, m_NearClip, m_FarClip);
    }

    void EditorCamera::UpdateView()
    {
        // m_Yaw = m_Pitch = 0.0f; // Lock the camera's rotation
        m_Position = CalculatePosition();

        glm::quat const orientation = GetOrientation();
        m_ViewMatrix = glm::translate(glm::mat4(1.0f), m_Position) * glm::toMat4(orientation);
        m_ViewMatrix = glm::inverse(m_ViewMatrix);
    }

    std::pair<f32, f32> EditorCamera::PanSpeed() const
    {
        const f32 x = std::min(m_ViewportWidth / 1000.0f, 2.4f);
        f32 xFactor = ((0.0366f * (x * x)) - (0.1778f * x)) + 0.3021f;

        const f32 y = std::min(m_ViewportHeight / 1000.0f, 2.4f);
        f32 yFactor = ((0.0366f * (y * y)) - (0.1778f * y)) + 0.3021f;

        return { xFactor, yFactor };
    }

    [[nodiscard("Store this!")]] f32 EditorCamera::RotationSpeed()
    {
        return 0.8f;
    }

    f32 EditorCamera::ZoomSpeed() const
    {
        f32 distance = m_Distance * 0.2f;
        distance = std::max(distance, 0.0f);
        f32 speed = distance * distance;
        speed = std::min(speed, 100.0f);
        return speed;
    }

    void EditorCamera::OnUpdate(Timestep const ts)
    {
        OLO_PROFILE_FUNCTION();

        const glm::vec2 mouse{ Input::GetMouseX(), Input::GetMouseY() };
        const glm::vec2 delta = (mouse - m_InitialMousePosition) * 0.003f;
        m_InitialMousePosition = mouse;

        if (Input::IsKeyPressed(Key::LeftAlt))
        {
            m_Flying = false;

            if (Input::IsMouseButtonPressed(Mouse::ButtonMiddle))
            {
                MousePan(delta);
            }
            else if (Input::IsMouseButtonPressed(Mouse::ButtonLeft))
            {
                MouseRotate(delta);
            }
            else if (Input::IsMouseButtonPressed(Mouse::ButtonRight))
            {
                MouseZoom(delta.y);
            }
        }
        else if (Input::IsMouseButtonPressed(Mouse::ButtonRight))
        {
            m_Flying = true;

            // Mouse look
            MouseRotate(delta);

            // Keep focal point in front of camera so rotation doesn't cause orbiting
            m_FocalPoint = m_Position + GetForwardDirection() * m_Distance;

            // WASD + QE movement
            f32 speed = m_FlySpeed * ts;
            if (Input::IsKeyPressed(Key::LeftShift))
            {
                speed *= 3.0f;
            }

            glm::vec3 movement(0.0f);
            if (Input::IsKeyPressed(Key::W))
            {
                movement += GetForwardDirection();
            }
            if (Input::IsKeyPressed(Key::S))
            {
                movement -= GetForwardDirection();
            }
            if (Input::IsKeyPressed(Key::A))
            {
                movement -= GetRightDirection();
            }
            if (Input::IsKeyPressed(Key::D))
            {
                movement += GetRightDirection();
            }
            if (Input::IsKeyPressed(Key::Q))
            {
                movement -= glm::vec3(0.0f, 1.0f, 0.0f);
            }
            if (Input::IsKeyPressed(Key::E))
            {
                movement += glm::vec3(0.0f, 1.0f, 0.0f);
            }

            if (glm::length(movement) > 0.0f)
            {
                m_FocalPoint += glm::normalize(movement) * speed;
            }
        }
        else
        {
            m_Flying = false;
        }

        // Gamepad fly mode: left stick moves, right stick looks, bumpers for up/down
        if (m_GamepadEnabled)
        {
            if (auto* gp = GamepadManager::GetGamepad(); gp && gp->IsConnected())
            {
                glm::vec2 leftStick = gp->GetLeftStickDeadzone(0.15f);
                glm::vec2 rightStick = gp->GetRightStickDeadzone(0.15f);

                bool hasStickInput = glm::length(leftStick) > 0.0f || glm::length(rightStick) > 0.0f;
                bool hasBumperInput = gp->IsButtonPressed(GamepadButton::LeftBumper) || gp->IsButtonPressed(GamepadButton::RightBumper);

                if (hasStickInput || hasBumperInput)
                {
                    m_Flying = true;

                    // Right stick look
                    if (glm::length(rightStick) > 0.0f)
                    {
                        const f32 yawSign = GetUpDirection().y < 0 ? -1.0f : 1.0f;
                        m_Yaw += yawSign * rightStick.x * m_GamepadSensitivity * ts;
                        m_Pitch += rightStick.y * m_GamepadSensitivity * ts;

                        m_FocalPoint = m_Position + GetForwardDirection() * m_Distance;
                    }

                    // Left stick movement
                    f32 speed = m_FlySpeed * ts;
                    if (gp->GetAxis(GamepadAxis::RightTrigger) > 0.5f)
                    {
                        speed *= 3.0f; // Sprint with right trigger
                    }

                    glm::vec3 movement(0.0f);
                    if (std::abs(leftStick.y) > 0.0f)
                    {
                        movement -= GetForwardDirection() * leftStick.y; // Forward/backward (Y is inverted)
                    }
                    if (std::abs(leftStick.x) > 0.0f)
                    {
                        movement += GetRightDirection() * leftStick.x; // Strafe
                    }
                    if (gp->IsButtonPressed(GamepadButton::RightBumper))
                    {
                        movement += glm::vec3(0.0f, 1.0f, 0.0f); // Up
                    }
                    if (gp->IsButtonPressed(GamepadButton::LeftBumper))
                    {
                        movement -= glm::vec3(0.0f, 1.0f, 0.0f); // Down
                    }

                    if (glm::length(movement) > 0.0f)
                    {
                        // Clamp length to 1 (prevents diagonal speed boost) but preserve analog magnitude
                        glm::vec3 dir = glm::length(movement) > 1.0f ? glm::normalize(movement) : movement;
                        m_FocalPoint += dir * speed;
                    }
                }
            }
        }

        UpdateView();
    }

    void EditorCamera::OnEvent(Event& e)
    {
        EventDispatcher dispatcher(e);
        // TODO(olbu): Only dispatch when hovering viewport?
        dispatcher.Dispatch<MouseScrolledEvent>(OLO_BIND_EVENT_FN(EditorCamera::OnMouseScroll));
    }

    bool EditorCamera::OnMouseScroll(const MouseScrolledEvent& e)
    {
        OLO_PROFILE_FUNCTION();

        const f32 delta = e.GetYOffset() * 0.1f;
        if (m_Flying)
        {
            // Adjust fly speed with scroll wheel while in fly mode
            m_FlySpeed = std::max(0.5f, m_FlySpeed + e.GetYOffset() * 0.5f);
        }
        else
        {
            MouseZoom(delta);
        }
        UpdateView();
        return false;
    }

    void EditorCamera::MousePan(const glm::vec2& delta)
    {
        const auto [xSpeed, ySpeed] = PanSpeed();
        m_FocalPoint += -GetRightDirection() * delta.x * xSpeed * m_Distance;
        m_FocalPoint += GetUpDirection() * delta.y * ySpeed * m_Distance;
    }

    void EditorCamera::MouseRotate(const glm::vec2& delta)
    {
        const f32 yawSign = GetUpDirection().y < 0 ? -1.0f : 1.0f;
        m_Yaw += yawSign * delta.x * RotationSpeed();
        m_Pitch += delta.y * RotationSpeed();
    }

    void EditorCamera::MouseZoom(const f32 delta)
    {
        m_Distance -= delta * ZoomSpeed();
        if (m_Distance < 1.0f)
        {
            m_FocalPoint += GetForwardDirection();
            m_Distance = 1.0f;
        }
    }

    glm::vec3 EditorCamera::GetUpDirection() const
    {
        return glm::rotate(GetOrientation(), glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::vec3 EditorCamera::GetRightDirection() const
    {
        return glm::rotate(GetOrientation(), glm::vec3(1.0f, 0.0f, 0.0f));
    }

    glm::vec3 EditorCamera::GetForwardDirection() const
    {
        return glm::rotate(GetOrientation(), glm::vec3(0.0f, 0.0f, -1.0f));
    }

    glm::vec3 EditorCamera::CalculatePosition() const
    {
        return m_FocalPoint - (GetForwardDirection() * m_Distance);
    }

    glm::quat EditorCamera::GetOrientation() const
    {
        return { (glm::vec3(-m_Pitch, -m_Yaw, 0.0f)) };
    }

} // namespace OloEngine
