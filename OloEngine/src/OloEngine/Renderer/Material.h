#pragma once

#include <glm/glm.hpp>
#include "OloEngine/Renderer/Texture.h"

namespace OloEngine
{
	struct Material
	{
		glm::vec3 Ambient;
		glm::vec3 Diffuse;
		glm::vec3 Specular;
		float Shininess;
		bool UseTextureMaps = false;
		Ref<Texture2D> DiffuseMap;
		Ref<Texture2D> SpecularMap;
	};
}