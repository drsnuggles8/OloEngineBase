#pragma once

#include <glm/glm.hpp>

namespace OloEngine
{
	class Camera
	{
	public:
		Camera() = default;
		explicit Camera(const glm::mat4& projection)
			: m_Projection(projection)
		{}

		virtual ~Camera() = default;

		[[nodiscard("Store this!")]] const glm::mat4& GetProjection() const { return m_Projection; }
		[[nodiscard("Store this!")]] const glm::mat4& GetViewProjectionMatrix() const { return m_ViewProjectionMatrix; }

	protected:
		glm::mat4 m_Projection = glm::mat4(1.0f);
		glm::mat4 m_ViewProjectionMatrix = glm::mat4(1.0f);
	};
}
