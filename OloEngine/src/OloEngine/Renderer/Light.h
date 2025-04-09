#pragma once

#include <glm/glm.hpp>

namespace OloEngine
{
	enum class LightType
	{
		Directional,
		Point,
		Spot
	};

	class Light
    {
    public:
        LightType Type = LightType::Point;
        glm::vec3 Position = glm::vec3(0.0f);
        glm::vec3 Direction = glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 Ambient = glm::vec3(0.2f);
        glm::vec3 Diffuse = glm::vec3(0.5f);
        glm::vec3 Specular = glm::vec3(1.0f);
        f32 Constant = 1.0f;
        f32 Linear = 0.09f;
        f32 Quadratic = 0.032f;
        f32 CutOff = glm::cos(glm::radians(12.5f));
        f32 OuterCutOff = glm::cos(glm::radians(17.5f));
    };
}