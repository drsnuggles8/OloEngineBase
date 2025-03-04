#include "OloEngine/Renderer/Camera/PerspectiveCameraController.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

namespace OloEngine
{
	PerspectiveCameraController::PerspectiveCameraController(float fov, float aspectRatio, float nearClip, float farClip)
		: m_AspectRatio(aspectRatio), m_Camera(fov, aspectRatio, nearClip, farClip)
	{}

	void PerspectiveCameraController::OnUpdate(Timestep ts)
	{
		if (Input::IsKeyPressed(Key::A))
		{
			m_CameraPosition.x -= m_CameraTranslationSpeed * ts;
		}
		else if (Input::IsKeyPressed(Key::D))
		{
			m_CameraPosition.x += m_CameraTranslationSpeed * ts;
		}

		if (Input::IsKeyPressed(Key::W))
		{
			m_CameraPosition.z -= m_CameraTranslationSpeed * ts;
		}
		else if (Input::IsKeyPressed(Key::S))
		{
			m_CameraPosition.z += m_CameraTranslationSpeed * ts;
		}

		if (m_MouseLookEnabled)
		{
			const glm::vec2 mousePosition = { Input::GetMouseX(), Input::GetMouseY() };
			const glm::vec2 delta = (mousePosition - m_LastMousePosition) * m_CameraRotationSpeed;
			m_LastMousePosition = mousePosition;

			m_CameraRotation = glm::rotate(m_CameraRotation, glm::radians(-delta.x), glm::vec3(0.0f, 1.0f, 0.0f));
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

	void PerspectiveCameraController::OnResize(float width, float height)
	{
		m_AspectRatio = width / height;
		m_Camera.SetViewportSize(width, height);
	}

	bool PerspectiveCameraController::OnMouseScrolled(MouseScrolledEvent& e)
	{
		m_CameraTranslationSpeed -= e.GetYOffset() * 0.25f;
		m_CameraTranslationSpeed = std::max(m_CameraTranslationSpeed, 0.25f);
		return false;
	}

	bool PerspectiveCameraController::OnWindowResized(WindowResizeEvent& e)
	{
		OnResize(static_cast<float>(e.GetWidth()), static_cast<float>(e.GetHeight()));
		return false;
	}

	void PerspectiveCameraController::UpdateCameraView()
	{
		m_Camera.SetPosition(m_CameraPosition);
		m_Camera.SetRotation(m_CameraRotation);
	}
}
