#pragma once

#include "OloEngine/Renderer/OrthographicCamera.h"
#include "OloEngine/Core/Timestep.h"

#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Events/MouseEvent.h"

namespace OloEngine {

	class OrthographicCameraController
	{
	public:
		explicit OrthographicCameraController(float aspectRatio, bool rotation = true);

		void OnUpdate(Timestep ts);
		void OnEvent(Event& e);

		void OnResize(float width, float height);

		[[nodiscard("Store this!")]] OrthographicCamera& GetCamera() { return m_Camera; }
		[[nodiscard("Store this!")]] const OrthographicCamera& GetCamera() const { return m_Camera; }

		[[nodiscard("Store this!")]] float GetZoomLevel() const { return m_ZoomLevel; }
		void SetZoomLevel(float level) { m_ZoomLevel = level; }
	private:
		bool OnMouseScrolled(MouseScrolledEvent& e);
		bool OnWindowResized(WindowResizeEvent& e);
	private:
		float m_AspectRatio;
		float m_ZoomLevel = 1.0f;
		OrthographicCamera m_Camera;

		bool m_Rotation;

		glm::vec3 m_CameraPosition = { 0.0f, 0.0f, 0.0f };
		float m_CameraRotation = 0.0f; //In degrees, in the anti-clockwise direction
		float m_CameraTranslationSpeed = 5.0f;
		float m_CameraRotationSpeed = 180.0f;
	};

}
