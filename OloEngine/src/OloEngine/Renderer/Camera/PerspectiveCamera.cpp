#include "OloEngine/Renderer/Camera/PerspectiveCamera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
	PerspectiveCamera::PerspectiveCamera(float fov, float aspectRatio, float nearClip, float farClip)
		: m_FOV(fov), m_AspectRatio(aspectRatio), m_NearClip(nearClip), m_FarClip(farClip)
	{
		UpdateProjection();
		UpdateView();
	}

	void PerspectiveCamera::SetViewportSize(float width, float height)
	{
		m_AspectRatio = width / height;
		UpdateProjection();
	}

	void PerspectiveCamera::SetPosition(const glm::vec3& position)
	{
		m_Position = position;
		UpdateView();
	}

	void PerspectiveCamera::SetRotation(const glm::quat& rotation)
	{
		m_Rotation = rotation;
		UpdateView();
	}

	void PerspectiveCamera::UpdateProjection()
	{
		m_Projection = glm::perspective(glm::radians(m_FOV), m_AspectRatio, m_NearClip, m_FarClip);
		UpdateView();
	}

	void PerspectiveCamera::UpdateView()
	{
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), m_Position) * glm::mat4_cast(m_Rotation);
		m_ViewProjection = m_Projection * glm::inverse(transform);
	}
}
