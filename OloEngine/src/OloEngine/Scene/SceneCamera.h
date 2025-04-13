#pragma once

#include "OloEngine/Renderer/Camera/Camera.h"

namespace OloEngine
{
	class SceneCamera : public Camera
	{
	public:
		enum class ProjectionType { Perspective = 0, Orthographic = 1 };
	public:
		SceneCamera();
		~SceneCamera() override = default;

		void SetPerspective(f32 verticalFOV, f32 nearClip, f32 farClip);
		void SetOrthographic(f32 size, f32 nearClip, f32 farClip);

		void SetViewportSize(u32 width, u32 height);

		[[nodiscard("Store this!")]] f32 GetPerspectiveVerticalFOV() const { return m_PerspectiveFOV; }
		void SetPerspectiveVerticalFOV(const f32 verticalFov) { m_PerspectiveFOV = verticalFov; RecalculateProjection(); }
		[[nodiscard("Store this!")]] f32 GetPerspectiveNearClip() const { return m_PerspectiveNear; }
		void SetPerspectiveNearClip(const f32 nearClip) { m_PerspectiveNear = nearClip; RecalculateProjection(); }
		[[nodiscard("Store this!")]] f32 GetPerspectiveFarClip() const { return m_PerspectiveFar; }
		void SetPerspectiveFarClip(const f32 farClip) { m_PerspectiveFar = farClip; RecalculateProjection(); }

		[[nodiscard("Store this!")]] f32 GetOrthographicSize() const { return m_OrthographicSize; }
		void SetOrthographicSize(const f32 size) { m_OrthographicSize = size; RecalculateProjection(); }
		[[nodiscard("Store this!")]] f32 GetOrthographicNearClip() const { return m_OrthographicNear; }
		void SetOrthographicNearClip(const f32 nearClip) { m_OrthographicNear = nearClip; RecalculateProjection(); }
		[[nodiscard("Store this!")]] f32 GetOrthographicFarClip() const { return m_OrthographicFar; }
		void SetOrthographicFarClip(const f32 farClip) { m_OrthographicFar = farClip; RecalculateProjection(); }

		[[nodiscard("Store this!")]] ProjectionType GetProjectionType() const { return m_ProjectionType; }
		void SetProjectionType(const ProjectionType type) { m_ProjectionType = type; RecalculateProjection(); }
	private:
		void RecalculateProjection();
	private:
		ProjectionType m_ProjectionType = ProjectionType::Orthographic;

		f32 m_PerspectiveFOV = glm::radians(45.0f);
		f32 m_PerspectiveNear = 0.01f;
		f32 m_PerspectiveFar = 1000.0f;
		f32 m_OrthographicSize = 10.0f;
		f32 m_OrthographicNear = -1.0f;
		f32 m_OrthographicFar = 1.0f;

		f32 m_AspectRatio = 0.0f;
	};

}
