#pragma once

#include "Camera.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Events/Event.h"
#include "OloEngine/Events/MouseEvent.h"

#include <glm/glm.hpp>

namespace OloEngine {

	class EditorCamera : public Camera
	{
	public:
		EditorCamera() = default;
		EditorCamera(float fov, float aspectRatio, float nearClip, float farClip);

		void OnUpdate(Timestep ts);
		void OnEvent(Event& e);

		[[nodiscard("Store this!")]] float GetDistance() const { return m_Distance; }
		void SetDistance(const float distance) { m_Distance = distance; }

		void SetViewportSize(const float width, const float height) { m_ViewportWidth = width; m_ViewportHeight = height; UpdateProjection(); }

		[[nodiscard("Store this!")]] const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
		[[nodiscard("Store this!")]] glm::mat4 GetViewProjection() const { return m_Projection * m_ViewMatrix; }

		[[nodiscard("Store this!")]] glm::vec3 GetUpDirection() const;
		[[nodiscard("Store this!")]] glm::vec3 GetRightDirection() const;
		[[nodiscard("Store this!")]] glm::vec3 GetForwardDirection() const;
		[[nodiscard("Store this!")]] const glm::vec3& GetPosition() const { return m_Position; }
		[[nodiscard("Store this!")]] glm::quat GetOrientation() const;

		[[nodiscard("Store this!")]] float GetPitch() const { return m_Pitch; }
		[[nodiscard("Store this!")]] float GetYaw() const { return m_Yaw; }
	private:
		void UpdateProjection();
		void UpdateView();

		bool OnMouseScroll(const MouseScrolledEvent& e);

		void MousePan(const glm::vec2& delta);
		void MouseRotate(const glm::vec2& delta);
		void MouseZoom(float delta);

		[[nodiscard("Store this!")]] glm::vec3 CalculatePosition() const;

		[[nodiscard("Store this!")]] std::pair<float, float> PanSpeed() const;
		static float RotationSpeed() ;
		[[nodiscard("Store this!")]] float ZoomSpeed() const;

		float m_FOV = 45.0F;
		float m_AspectRatio = 1.778F;
		float m_NearClip = 0.1F;
		float m_FarClip = 1000.0F;

		glm::mat4 m_ViewMatrix{};
		glm::vec3 m_Position = { 0.0F, 0.0F, 0.0F };
		glm::vec3 m_FocalPoint = { 0.0F, 0.0F, 0.0F };

		glm::vec2 m_InitialMousePosition = { 0.0F, 0.0F };

		float m_Distance = 10.0F;
		float m_Pitch = 0.0F;
		float m_Yaw = 0.0F;

		float m_ViewportWidth = 1280.0F;
		float m_ViewportHeight = 720.0F;
	};

}
