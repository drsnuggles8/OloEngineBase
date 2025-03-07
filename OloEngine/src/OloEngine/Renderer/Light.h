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

	struct Light
	{
		LightType Type;
		glm::vec3 Position;
		glm::vec3 Direction;
		glm::vec3 Ambient;
		glm::vec3 Diffuse;
		glm::vec3 Specular;
		float Constant;
		float Linear;
		float Quadratic;
		float CutOff;
		float OuterCutOff;
	};
}