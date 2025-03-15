#include "OloEngine/Renderer/Camera/PerspectiveCameraController.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"
#include <glm/gtx/quaternion.hpp>

namespace OloEngine
{
	PerspectiveCameraController::PerspectiveCameraController(f32 fov, f32 aspectRatio, f32 nearClip, f32 farClip)
		: m_AspectRatio(aspectRatio), m_Camera(fov, aspectRatio, nearClip, farClip)
	{
		 // Start with a position that gives a good view of the scene
		m_CameraPosition = glm::vec3(0.0f, 2.0f, 6.0f);
		
		// Start with identity rotation
		m_CameraRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		
		// Then add a slight downward tilt by rotating around the local X axis
		glm::quat pitchRotation = glm::angleAxis(glm::radians(-15.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		m_CameraRotation = pitchRotation * m_CameraRotation;
		
		UpdateCameraView();
	}

	void PerspectiveCameraController::OnUpdate(Timestep ts)
	{
		const glm::vec3 forward = glm::normalize(m_CameraRotation * glm::vec3(0.0f, 0.0f, -1.0f));
		const glm::vec3 right = glm::normalize(m_CameraRotation * glm::vec3(1.0f, 0.0f, 0.0f));
		const glm::vec3 up = glm::normalize(m_CameraRotation * glm::vec3(0.0f, 1.0f, 0.0f));

		if (Input::IsKeyPressed(Key::A))
			m_CameraPosition -= right * (m_CameraTranslationSpeed * ts);
		else if (Input::IsKeyPressed(Key::D))
			m_CameraPosition += right * (m_CameraTranslationSpeed * ts);

		if (Input::IsKeyPressed(Key::W))
			m_CameraPosition += forward * (m_CameraTranslationSpeed * ts);
		else if (Input::IsKeyPressed(Key::S))
			m_CameraPosition -= forward * (m_CameraTranslationSpeed * ts);

		if (Input::IsKeyPressed(Key::LeftShift))
			m_CameraPosition += up * (m_CameraTranslationSpeed * ts);
		else if (Input::IsKeyPressed(Key::LeftControl))
			m_CameraPosition -= up * (m_CameraTranslationSpeed * ts);

		if (m_MouseLookEnabled)
		{
			glm::vec2 mousePosition = { Input::GetMouseX(), Input::GetMouseY() };
			glm::vec2 delta = (mousePosition - m_LastMousePosition) * m_CameraRotationSpeed;
			m_LastMousePosition = mousePosition;

			// Yaw (around world Y)
			m_CameraRotation = glm::rotate(m_CameraRotation, glm::radians(-delta.x), glm::vec3(0.0f, 1.0f, 0.0f));
			// Pitch (around local X)
			m_CameraRotation = glm::rotate(m_CameraRotation, glm::radians(-delta.y), glm::vec3(1.0f, 0.0f, 0.0f));
		}

		UpdateCameraView();
	}

	void PerspectiveCameraController::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<MouseScrolledEvent>(OLO_BIND_EVENT_FN(PerspectiveCameraController::OnMouseScrolled));
		dispatcher.Dispatch<WindowResizeEvent>(OLO_BIND_EVENT_FN(PerspectiveCameraController::OnWindowResized));
	}

	void PerspectiveCameraController::OnResize(f32 width, f32 height)
	{
		m_AspectRatio = width / height;
		m_Camera.SetViewportSize(width, height);
	}

	bool PerspectiveCameraController::OnMouseScrolled(MouseScrolledEvent& e)
	{
		m_CameraTranslationSpeed = std::max(m_CameraTranslationSpeed - e.GetYOffset() * 0.25f, 0.25f);
		return false;
	}

	bool PerspectiveCameraController::OnWindowResized(WindowResizeEvent& e)
	{
		OnResize(static_cast<f32>(e.GetWidth()), static_cast<f32>(e.GetHeight()));
		return false;
	}

	void PerspectiveCameraController::UpdateCameraView()
	{
		m_Camera.SetPosition(m_CameraPosition);
		m_Camera.SetRotation(m_CameraRotation);
	}
}
