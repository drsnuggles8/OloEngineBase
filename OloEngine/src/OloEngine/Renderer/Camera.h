#pragma once

#include <glm/glm.hpp>

namespace OloEngine {

	class Camera
	{
	public:
		Camera() = default;
		explicit Camera(const glm::mat4& projection)
			: m_Projection(projection) {}

		virtual ~Camera() = default;

		[[nodiscard("This returns m_Projection, you probably wanted another function!")]] const glm::mat4& GetProjection() const { return m_Projection; }
	protected:
		glm::mat4 m_Projection = glm::mat4(1.0f);
	};

}
