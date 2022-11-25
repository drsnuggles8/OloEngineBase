#pragma once

#include "OloEngine/Renderer/Camera.h"

namespace OloEngine
{
	class SceneCamera : public Camera
	{
	public:
		enum class ProjectionType { Perspective = 0, Orthographic = 1 };
	public:
		SceneCamera();
		~SceneCamera() override = default;

		void SetPerspective(float verticalFOV, float nearClip, float farClip);
		void SetOrthographic(float size, float nearClip, float farClip);

		void SetViewportSize(uint32_t width, uint32_t height);

		[[nodiscard("Store this!")]] float GetPerspectiveVerticalFOV() const { return m_PerspectiveFOV; }
		void SetPerspectiveVerticalFOV(const float verticalFov) { m_PerspectiveFOV = verticalFov; RecalculateProjection(); }
		[[nodiscard("Store this!")]] float GetPerspectiveNearClip() const { return m_PerspectiveNear; }
		void SetPerspectiveNearClip(const float nearClip) { m_PerspectiveNear = nearClip; RecalculateProjection(); }
		[[nodiscard("Store this!")]] float GetPerspectiveFarClip() const { return m_PerspectiveFar; }
		void SetPerspectiveFarClip(const float farClip) { m_PerspectiveFar = farClip; RecalculateProjection(); }

		[[nodiscard("Store this!")]] float GetOrthographicSize() const { return m_OrthographicSize; }
		void SetOrthographicSize(const float size) { m_OrthographicSize = size; RecalculateProjection(); }
		[[nodiscard("Store this!")]] float GetOrthographicNearClip() const { return m_OrthographicNear; }
		void SetOrthographicNearClip(const float nearClip) { m_OrthographicNear = nearClip; RecalculateProjection(); }
		[[nodiscard("Store this!")]] float GetOrthographicFarClip() const { return m_OrthographicFar; }
		void SetOrthographicFarClip(const float farClip) { m_OrthographicFar = farClip; RecalculateProjection(); }

		[[nodiscard("Store this!")]] ProjectionType GetProjectionType() const { return m_ProjectionType; }
		void SetProjectionType(const ProjectionType type) { m_ProjectionType = type; RecalculateProjection(); }
	private:
		void RecalculateProjection();
	private:
		ProjectionType m_ProjectionType = ProjectionType::Orthographic;

		float m_PerspectiveFOV = glm::radians(45.0f);
		float m_PerspectiveNear = 0.01f;
		float m_PerspectiveFar = 1000.0f;
		float m_OrthographicSize = 10.0f;
		float m_OrthographicNear = -1.0f;
		float m_OrthographicFar = 1.0f;

		float m_AspectRatio = 0.0f;
	};

}
