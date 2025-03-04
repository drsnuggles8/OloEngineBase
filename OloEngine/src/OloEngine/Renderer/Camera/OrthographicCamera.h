#pragma once

#include <glm/glm.hpp>

namespace OloEngine
{
	class OrthographicCamera
	{
	public:
		OrthographicCamera(f32 left, f32 right, f32 bottom, f32 top);

		void SetProjection(f32 left, f32 right, f32 bottom, f32 top);

		[[nodiscard("Store this!")]] const glm::vec3& GetPosition() const { return m_Position; }
		void SetPosition(const glm::vec3& position) { m_Position = position; RecalculateViewMatrix(); }

		[[nodiscard("Store this!")]] f32 GetRotation() const { return m_Rotation; }
		void SetRotation(const f32 rotation) { m_Rotation = rotation; RecalculateViewMatrix(); }

		[[nodiscard("Store this!")]] const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
		[[nodiscard("Store this!")]] const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
		[[nodiscard("Store this!")]] const glm::mat4& GetViewProjectionMatrix() const { return m_ViewProjectionMatrix; }
	private:
		void RecalculateViewMatrix();
	private:
		glm::mat4 m_ProjectionMatrix;
		glm::mat4 m_ViewMatrix;
		glm::mat4 m_ViewProjectionMatrix{};

		glm::vec3 m_Position = { 0.0f, 0.0f, 0.0f };
		f32 m_Rotation = 0.0f;
	};
}
