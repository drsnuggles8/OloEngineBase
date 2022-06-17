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

		[[nodiscard("This returns m_Distance, you probably wanted some other function!")]] float GetDistance() const { return m_Distance; }
		void SetDistance(const float distance) { m_Distance = distance; }

		void SetViewportSize(const float width, const float height) { m_ViewportWidth = width; m_ViewportHeight = height; UpdateProjection(); }

		[[nodiscard("This returns m_ViewMatrix, you probably wanted some other function!")]] const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
		[[nodiscard("This returns m_Projection * m_ViewMatrix, you probably wanted some other function!")]] glm::mat4 GetViewProjection() const { return m_Projection * m_ViewMatrix; }

		[[nodiscard("This returns the Up direction, you probably wanted some other function!")]] glm::vec3 GetUpDirection() const;
		[[nodiscard("This returns the Right direction, you probably wanted some other function!")]] glm::vec3 GetRightDirection() const;
		[[nodiscard("This returns the Forward direction, you probably wanted some other function!")]] glm::vec3 GetForwardDirection() const;
		[[nodiscard("This returns m_Position, you probably wanted some other function!")]] const glm::vec3& GetPosition() const { return m_Position; }
		[[nodiscard("This returns the Orientation, you probably wanted some other function!")]] glm::quat GetOrientation() const;

		[[nodiscard("This returns m_Pitch, you probably wanted some other function!")]] float GetPitch() const { return m_Pitch; }
		[[nodiscard("This returns m_Yaw, you probably wanted some other function!")]] float GetYaw() const { return m_Yaw; }
	private:
		void UpdateProjection();
		void UpdateView();

		bool OnMouseScroll(const MouseScrolledEvent& e);

		void MousePan(const glm::vec2& delta);
		void MouseRotate(const glm::vec2& delta);
		void MouseZoom(float delta);

		[[nodiscard("This calculates the position, you probably wanted some other function!")]] glm::vec3 CalculatePosition() const;

		[[nodiscard("This returns a pair of xFactor, yFactor as speed, you probably wanted some other function!")]] std::pair<float, float> PanSpeed() const;
		static float RotationSpeed() ;
		[[nodiscard("This returns the calculated zoom speed, you probably wanted some other function!")]] float ZoomSpeed() const;

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
