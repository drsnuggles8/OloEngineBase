#pragma once

#include <glm/glm.hpp>

namespace OloEngine {

	class OrthographicCamera
	{
	public:
		OrthographicCamera(float left, float right, float bottom, float top);

		void SetProjection(float left, float right, float bottom, float top);

		[[nodiscard("This returns m_Position, you probably wanted another function!")]] const glm::vec3& GetPosition() const { return m_Position; }
		void SetPosition(const glm::vec3& position) { m_Position = position; RecalculateViewMatrix(); }

		[[nodiscard("This returns m_Rotation, you probably wanted another function!")]] float GetRotation() const { return m_Rotation; }
		void SetRotation(const float rotation) { m_Rotation = rotation; RecalculateViewMatrix(); }

		[[nodiscard("This returns m_ProjectionMatrix, you probably wanted another function!")]] const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
		[[nodiscard("This returns m_ViewMatrix, you probably wanted another function!")]] const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
		[[nodiscard("This returns m_ViewProjectionMatrix, you probably wanted another function!")]] const glm::mat4& GetViewProjectionMatrix() const { return m_ViewProjectionMatrix; }
	private:
		void RecalculateViewMatrix();
	private:
		glm::mat4 m_ProjectionMatrix;
		glm::mat4 m_ViewMatrix;
		glm::mat4 m_ViewProjectionMatrix{};

		glm::vec3 m_Position = { 0.0f, 0.0f, 0.0f };
		float m_Rotation = 0.0f;
	};

}
