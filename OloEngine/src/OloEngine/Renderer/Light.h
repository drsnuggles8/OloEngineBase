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
		f32 Constant;
		f32 Linear;
		f32 Quadratic;
		f32 CutOff;
		f32 OuterCutOff;
	};
}