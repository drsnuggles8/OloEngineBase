#pragma once

#include "OloEngine/Renderer/Camera/Camera.h"
#include <glm/glm.hpp>

namespace OloEngine
{
	class PerspectiveCamera : public Camera
	{
	public:
		PerspectiveCamera(float fov, float aspectRatio, float nearClip, float farClip);

		void SetViewportSize(float width, float height);
		void SetPosition(const glm::vec3& position);
		void SetRotation(const glm::quat& rotation);

		const glm::mat4& GetViewProjection() const { return m_ViewProjection; }
		const glm::vec3& GetPosition() const { return m_Position; }

	private:
		void UpdateProjection();
		void UpdateView();

	private:
		float m_FOV;
		float m_AspectRatio;
		float m_NearClip;
		float m_FarClip;
		glm::mat4 m_Projection{ 1.0f };
		glm::mat4 m_ViewProjection{ 1.0f };
		glm::vec3 m_Position{ 0.0f };
		glm::quat m_Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
	};
}
