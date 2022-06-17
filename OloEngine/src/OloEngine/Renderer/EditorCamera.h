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

		[[nodiscard]] inline float GetDistance() const { return m_Distance; }
		inline void SetDistance(float distance) { m_Distance = distance; }

		inline void SetViewportSize(float width, float height) { m_ViewportWidth = width; m_ViewportHeight = height; UpdateProjection(); }

		[[nodiscard]] const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
		[[nodiscard]] glm::mat4 GetViewProjection() const { return m_Projection * m_ViewMatrix; }

		[[nodiscard]] glm::vec3 GetUpDirection() const;
		[[nodiscard]] glm::vec3 GetRightDirection() const;
		[[nodiscard]] glm::vec3 GetForwardDirection() const;
		[[nodiscard]] const glm::vec3& GetPosition() const { return m_Position; }
		[[nodiscard]] glm::quat GetOrientation() const;

		[[nodiscard]] float GetPitch() const { return m_Pitch; }
		[[nodiscard]] float GetYaw() const { return m_Yaw; }
	private:
		void UpdateProjection();
		void UpdateView();

		bool OnMouseScroll(MouseScrolledEvent& e);

		void MousePan(const glm::vec2& delta);
		void MouseRotate(const glm::vec2& delta);
		void MouseZoom(float delta);

		[[nodiscard]] glm::vec3 CalculatePosition() const;

		[[nodiscard]] std::pair<float, float> PanSpeed() const;
		static float RotationSpeed() ;
		[[nodiscard]] float ZoomSpeed() const;
	private:
		float m_FOV = 45.0F, m_AspectRatio = 1.778F, m_NearClip = 0.1F, m_FarClip = 1000.0F;

		glm::mat4 m_ViewMatrix{};
		glm::vec3 m_Position = { 0.0F, 0.0F, 0.0F };
		glm::vec3 m_FocalPoint = { 0.0F, 0.0F, 0.0F };

		glm::vec2 m_InitialMousePosition = { 0.0F, 0.0F };

		float m_Distance = 10.0F;
		float m_Pitch = 0.0F, m_Yaw = 0.0F;

		float m_ViewportWidth = 1280.0F, m_ViewportHeight = 720.0F;
	};

}
