#pragma once

#include "OloEngine/Renderer/Camera/Camera.h"
#include <glm/glm.hpp>

namespace OloEngine
{
	class PerspectiveCamera : public Camera
	{
	public:
		PerspectiveCamera(f32 fov, f32 aspectRatio, f32 nearClip, f32 farClip);

		void SetViewportSize(f32 width, f32 height);
		void SetPosition(const glm::vec3& position);
		void SetRotation(const glm::quat& rotation);

		const glm::mat4& GetViewProjection() const { return m_ViewProjection; }
		const glm::vec3& GetPosition() const { return m_Position; }

	private:
		void UpdateProjection();
		void UpdateView();

	private:
		f32 m_FOV;
		f32 m_AspectRatio;
		f32 m_NearClip;
		f32 m_FarClip;
		glm::mat4 m_Projection{ 1.0f };
		glm::mat4 m_ViewProjection{ 1.0f };
		glm::vec3 m_Position{ 0.0f };
		glm::quat m_Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
	};
}
