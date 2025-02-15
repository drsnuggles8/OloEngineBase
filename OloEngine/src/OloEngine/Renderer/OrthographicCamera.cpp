#include "OloEnginePCH.h"
#include "OloEngine/Renderer/OrthographicCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
	OrthographicCamera::OrthographicCamera(const f32 left, const f32 right, const f32 bottom, const f32 top)
		: m_ProjectionMatrix(glm::ortho(left, right, bottom, top, -1.0f, 1.0f)), m_ViewMatrix(1.0f)
	{
		OLO_PROFILE_FUNCTION();

		m_ViewProjectionMatrix = m_ProjectionMatrix * m_ViewMatrix;
	}

	void OrthographicCamera::SetProjection(const f32 left, const f32 right, const f32 bottom, const f32 top)
	{
		OLO_PROFILE_FUNCTION();

		m_ProjectionMatrix = glm::ortho(left, right, bottom, top, -1.0f, 1.0f);
		m_ViewProjectionMatrix = m_ProjectionMatrix * m_ViewMatrix;
	}

	void OrthographicCamera::RecalculateViewMatrix()
	{
		OLO_PROFILE_FUNCTION();

		glm::mat4 const transform = glm::translate(glm::mat4(1.0f), m_Position) *
			glm::rotate(glm::mat4(1.0f), glm::radians(m_Rotation), glm::vec3(0, 0, 1));

		m_ViewMatrix = glm::inverse(transform);
		m_ViewProjectionMatrix = m_ProjectionMatrix * m_ViewMatrix;
	}
}
