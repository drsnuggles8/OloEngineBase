#pragma once

#include "OloEngine/Renderer/Camera/PerspectiveCamera.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Events/MouseEvent.h"

namespace OloEngine
{
	class PerspectiveCameraController
	{
	public:
		PerspectiveCameraController(f32 fov, f32 aspectRatio, f32 nearClip, f32 farClip);

		void OnUpdate(Timestep ts);
		void OnEvent(Event& e);

		void OnResize(f32 width, f32 height);

		PerspectiveCamera& GetCamera() { return m_Camera; }
		const PerspectiveCamera& GetCamera() const { return m_Camera; }

	private:
		bool OnMouseScrolled(MouseScrolledEvent& e);
		bool OnWindowResized(WindowResizeEvent& e);
		void UpdateCameraView();

	private:
		f32 m_AspectRatio;
		PerspectiveCamera m_Camera;

		glm::vec3 m_CameraPosition = { 0.0f, 0.0f, 0.0f };
		glm::quat m_CameraRotation = { 1.0f, 0.0f, 0.0f, 0.0f };
		f32 m_CameraTranslationSpeed = 5.0f;
		f32 m_CameraRotationSpeed = 0.1f;

		bool m_MouseLookEnabled = true;
		glm::vec2 m_LastMousePosition = { 0.0f, 0.0f };
	};
}
