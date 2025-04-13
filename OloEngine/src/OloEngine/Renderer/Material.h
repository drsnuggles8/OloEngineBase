#pragma once

#include <glm/glm.hpp>
// Add these headers for GLM hash support
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
	struct Material
	{
		glm::vec3 Ambient;
		glm::vec3 Diffuse;
		glm::vec3 Specular;
		f32 Shininess;
		bool UseTextureMaps = false;
		Ref<Texture2D> DiffuseMap;
		Ref<Texture2D> SpecularMap;
		Ref<Shader> Shader;
		glm::vec3 LightPosition = {0.0f, 0.0f, 0.0f};
   		glm::vec3 ViewPosition = {0.0f, 0.0f, 0.0f};

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

		[[nodiscard]] u64 CalculateKey() const
		{
			u64 key = 0;
			// Simple hash combination of material properties
			HashCombine(key, std::hash<glm::vec3>()(Ambient));
			HashCombine(key, std::hash<glm::vec3>()(Diffuse));
			HashCombine(key, std::hash<glm::vec3>()(Specular));
			HashCombine(key, std::hash<float>()(Shininess));
			
			// Include shader ID if available
			if (Shader)
				HashCombine(key, Shader->GetRendererID());
			
			// Include texture IDs if used
			if (UseTextureMaps)
			{
				if (DiffuseMap)
					HashCombine(key, DiffuseMap->GetRendererID());
				if (SpecularMap)
					HashCombine(key, SpecularMap->GetRendererID());
			}
			
			return key;
		}

		private:
		// Hash combine helper
		template <typename T>
		static void HashCombine(u64& seed, const T& v)
		{
			std::hash<T> hasher;
			seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		}
	};
}
