// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "SceneCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
	SceneCamera::SceneCamera()
	{
		RecalculateProjection();
	}

	void SceneCamera::SetPerspective(const f32 verticalFOV, const f32 nearClip, const f32 farClip)
	{
		m_ProjectionType = ProjectionType::Perspective;
		m_PerspectiveFOV = verticalFOV;
		m_PerspectiveNear = nearClip;
		m_PerspectiveFar = farClip;
		RecalculateProjection();
	}

	void SceneCamera::SetOrthographic(const f32 size, const f32 nearClip, const f32 farClip)
	{
		m_ProjectionType = ProjectionType::Orthographic;
		m_OrthographicSize = size;
		m_OrthographicNear = nearClip;
		m_OrthographicFar = farClip;
		RecalculateProjection();
	}

	void SceneCamera::SetViewportSize(const u32 width, const u32 height)
	{
		OLO_CORE_ASSERT(width > 0 && height > 0);
		m_AspectRatio = static_cast<f32>(width) / static_cast<f32>(height);
		RecalculateProjection();
	}

	void SceneCamera::RecalculateProjection()
	{
		if (m_ProjectionType == ProjectionType::Perspective)
		{
			m_Projection = glm::perspective(m_PerspectiveFOV, m_AspectRatio, m_PerspectiveNear, m_PerspectiveFar);
		}
		else
		{
			const f32 orthoLeft = -m_OrthographicSize * m_AspectRatio * 0.5f;
			const f32 orthoRight = m_OrthographicSize * m_AspectRatio * 0.5f;
			const f32 orthoBottom = -m_OrthographicSize * 0.5f;
			const f32 orthoTop = m_OrthographicSize * 0.5f;

			m_Projection = glm::ortho(orthoLeft, orthoRight, orthoBottom, orthoTop, m_OrthographicNear, m_OrthographicFar);
		}
	}

}
