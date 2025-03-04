#pragma once

#include <glm/glm.hpp>

namespace OloEngine
{
	class Camera
	{
	public:
		Camera() = default;
		explicit Camera(const glm::mat4& projection)
			: m_Projection(projection) {}

		virtual ~Camera() = default;

		[[nodiscard("Store this!")]] const glm::mat4& GetProjection() const { return m_Projection; }
	protected:
		glm::mat4 m_Projection = glm::mat4(1.0f);
	};

}
