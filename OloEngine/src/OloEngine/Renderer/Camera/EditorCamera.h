#pragma once

#include "Camera.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Events/Event.h"
#include "OloEngine/Events/MouseEvent.h"

#include <glm/glm.hpp>

#include <algorithm> // std::clamp (SetFOV), std::max (SetFlySpeed) — keep the header self-contained

namespace OloEngine
{
    class Gamepad;

    class EditorCamera : public Camera
    {
      public:
        EditorCamera() = default;
        EditorCamera(f32 fov, f32 aspectRatio, f32 nearClip, f32 farClip);

        void OnUpdate(Timestep ts);
        void OnEvent(Event& e);

        void SetViewportSize(const f32 width, const f32 height)
        {
            m_ViewportWidth = width;
            m_ViewportHeight = height;
            UpdateProjection();
        }
        // EVERY setter below rebuilds the view matrix. They used to only stash the
        // member, which made them silent no-ops for any caller that does not also
        // drive OnUpdate — the whole editor-preferences bookmark restore, the
        // editor's own default 3D pose, and every screenshot test that posed a
        // camera this way. A capture test that did so rendered its whole set from
        // the constructor's default orbit view: identical frames, sky and water
        // but no subject, and nothing anywhere reporting a problem (issue #931).
        void SetDistance(const f32 distance)
        {
            m_Distance = distance;
            UpdateView();
        }
        // The orbit model derives the eye from the focal point, so an eye position
        // is stored by moving the FOCAL POINT to keep the current orbit distance
        // ahead of the requested eye. Assigning m_Position directly (what this did
        // before) is discarded by the very next UpdateView.
        void SetPosition(const glm::vec3& position)
        {
            m_FocalPoint = position + (GetForwardDirection() * m_Distance);
            UpdateView();
        }
        void SetYaw(const f32 yaw)
        {
            m_Yaw = yaw;
            UpdateView();
        }
        void SetPitch(const f32 pitch)
        {
            m_Pitch = pitch;
            UpdateView();
        }

        // Pose the camera at an explicit eye position looking along (yaw, pitch)
        // and rebuild the view matrix immediately. This collapses the orbit (focal
        // point = eye, distance 0) so the requested pose is exactly what gets
        // rendered. Used for deterministic captures where there is no live input.
        // Positive pitch tilts the view down.
        void SetPose(const glm::vec3& eyePosition, const f32 yaw, const f32 pitch)
        {
            SetPose(eyePosition, yaw, pitch, 0.0f);
        }

        // As above, but keeping `orbitDistance` as the pivot distance so a later
        // orbit/dolly turns about a point in front of the camera rather than about
        // the eye. One atomic call rather than four setters whose ORDER decides
        // where the eye ends up — restoring a saved (position, yaw, pitch,
        // distance) camera bookmark is exactly this and nothing else.
        void SetPose(const glm::vec3& eyePosition, const f32 yaw, const f32 pitch, const f32 orbitDistance)
        {
            m_Yaw = yaw;
            m_Pitch = pitch;
            m_Distance = orbitDistance;
            // GetForwardDirection() reads m_Yaw/m_Pitch, which are already the
            // final ones — so the eye lands exactly on `eyePosition`.
            m_FocalPoint = eyePosition + (GetForwardDirection() * orbitDistance);
            UpdateView();
        }

        // Frame an orbit view centred on `point`: pivot there at the given
        // distance and look-angles, rebuilding the view immediately. Unlike
        // SetPose (which collapses the orbit to distance 0), this keeps a real
        // focal point so subsequent orbit/dolly behave normally. Used to frame
        // the camera on scene content when a scene is opened. Positive pitch
        // tilts the view down.
        void Focus(const glm::vec3& point, const f32 distance, const f32 yaw, const f32 pitch)
        {
            m_FocalPoint = point;
            m_Distance = distance;
            m_Yaw = yaw;
            m_Pitch = pitch;
            UpdateView();
        }

        // Vertical field of view in degrees. Setting it rebuilds the projection
        // immediately (used by the MCP camera tools for deterministic captures).
        [[nodiscard("Store this!")]] f32 GetFOV() const
        {
            return m_FOV;
        }
        void SetFOV(const f32 fovDegrees)
        {
            m_FOV = std::clamp(fovDegrees, 1.0f, 170.0f);
            UpdateProjection();
        }

        [[nodiscard("Store this!")]] const glm::vec3& GetFocalPoint() const
        {
            return m_FocalPoint;
        }

        [[nodiscard("Store this!")]] const glm::mat4& GetViewMatrix() const
        {
            return m_ViewMatrix;
        }
        [[nodiscard("Store this!")]] const glm::mat4 GetViewProjection() const
        {
            return m_Projection * m_ViewMatrix;
        }
        [[nodiscard("Store this!")]] const f32 GetDistance() const
        {
            return m_Distance;
        }
        [[nodiscard("Store this!")]] const glm::vec3& GetPosition() const
        {
            return m_Position;
        }
        [[nodiscard("Store this!")]] const f32 GetPitch() const
        {
            return m_Pitch;
        }
        [[nodiscard("Store this!")]] const f32 GetYaw() const
        {
            return m_Yaw;
        }

        [[nodiscard("Store this!")]] f32 GetNearClip() const
        {
            return m_NearClip;
        }
        [[nodiscard("Store this!")]] f32 GetFarClip() const
        {
            return m_FarClip;
        }

        [[nodiscard("Store this!")]] glm::vec3 GetUpDirection() const;
        [[nodiscard("Store this!")]] glm::vec3 GetRightDirection() const;
        [[nodiscard("Store this!")]] glm::vec3 GetForwardDirection() const;
        [[nodiscard("Store this!")]] glm::quat GetOrientation() const;

        [[nodiscard("Store this!")]] bool IsFlying() const
        {
            return m_Flying;
        }
        void SetFlySpeed(const f32 speed)
        {
            m_FlySpeed = std::max(0.5f, speed);
        }
        [[nodiscard("Store this!")]] f32 GetFlySpeed() const
        {
            return m_FlySpeed;
        }

        void SetGamepadEnabled(bool enabled)
        {
            m_GamepadEnabled = enabled;
        }
        [[nodiscard]] bool IsGamepadEnabled() const
        {
            return m_GamepadEnabled;
        }
        void SetGamepadSensitivity(f32 sensitivity)
        {
            m_GamepadSensitivity = sensitivity;
        }
        [[nodiscard]] f32 GetGamepadSensitivity() const
        {
            return m_GamepadSensitivity;
        }

      private:
        void UpdateProjection();
        void UpdateView();

        bool OnMouseScroll(const MouseScrolledEvent& e);

        void MousePan(const glm::vec2& delta);
        void MouseRotate(const glm::vec2& delta);
        void MouseZoom(f32 delta);

        [[nodiscard("Store this!")]] glm::vec3 CalculatePosition() const;

        [[nodiscard("Store this!")]] std::pair<f32, f32> PanSpeed() const;
        static f32 RotationSpeed();
        [[nodiscard("Store this!")]] f32 ZoomSpeed() const;

      private:
        f32 m_FOV = 45.0F;
        f32 m_AspectRatio = 1.778F;
        f32 m_NearClip = 0.1F;
        f32 m_FarClip = 1000.0F;

        glm::mat4 m_ViewMatrix{};
        glm::vec3 m_Position = { 0.0F, 0.0F, 0.0F };
        glm::vec3 m_FocalPoint = { 0.0F, 0.0F, 0.0F };

        glm::vec2 m_InitialMousePosition = { 0.0F, 0.0F };

        f32 m_Distance = 10.0F;
        f32 m_Pitch = 0.0F;
        f32 m_Yaw = 0.0F;

        f32 m_FlySpeed = 5.0F;
        bool m_Flying = false;

        // Gamepad control
        bool m_GamepadEnabled = true;
        f32 m_GamepadSensitivity = 2.0F;

        f32 m_ViewportWidth = 1280.0F;
        f32 m_ViewportHeight = 720.0F;
    };

} // namespace OloEngine
