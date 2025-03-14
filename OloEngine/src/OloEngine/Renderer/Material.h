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

		bool operator==(const Material& other) const
		{
			// Compare basic properties
			if (Ambient != other.Ambient ||
				Diffuse != other.Diffuse ||
				Specular != other.Specular ||
				Shininess != other.Shininess ||
				UseTextureMaps != other.UseTextureMaps)
			{
				return false;
			}

			// Compare texture maps if they are used
			if (UseTextureMaps)
			{
				if (DiffuseMap && other.DiffuseMap)
				{
					if (*DiffuseMap != *other.DiffuseMap)
						return false;
				}
				else if (DiffuseMap || other.DiffuseMap)
				{
					return false;
				}

				if (SpecularMap && other.SpecularMap)
				{
					if (*SpecularMap != *other.SpecularMap)
						return false;
				}
				else if (SpecularMap || other.SpecularMap)
				{
					return false;
				}
			}

			return true;
		}
	};
}