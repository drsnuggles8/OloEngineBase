#pragma once

#include "OloEngine/Renderer/Camera/OrthographicCamera.h"
#include "OloEngine/Core/Timestep.h"

#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Events/MouseEvent.h"

namespace OloEngine
{
	class OrthographicCameraController
	{
	public:
		explicit OrthographicCameraController(f32 aspectRatio, bool rotation = true);

		void OnUpdate(Timestep ts);
		void OnEvent(Event& e);

		void OnResize(f32 width, f32 height);

		[[nodiscard("Store this!")]] OrthographicCamera& GetCamera() { return m_Camera; }
		[[nodiscard("Store this!")]] const OrthographicCamera& GetCamera() const { return m_Camera; }

		[[nodiscard("Store this!")]] f32 GetZoomLevel() const { return m_ZoomLevel; }
		void SetZoomLevel(f32 level) { m_ZoomLevel = level; }
	private:
		bool OnMouseScrolled(MouseScrolledEvent& e);
		bool OnWindowResized(WindowResizeEvent& e);
	private:
		f32 m_AspectRatio;
		f32 m_ZoomLevel = 1.0f;
		OrthographicCamera m_Camera;

		bool m_Rotation;

		glm::vec3 m_CameraPosition = { 0.0f, 0.0f, 0.0f };
		f32 m_CameraRotation = 0.0f; //In degrees, in the anti-clockwise direction
		f32 m_CameraTranslationSpeed = 5.0f;
		f32 m_CameraRotationSpeed = 180.0f;
	};
}
