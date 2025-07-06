#pragma once

#include <glm/glm.hpp>
// Add these headers for GLM hash support
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"
#include "Platform/OpenGL/OpenGLShader.h"

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

		/**
		 * @brief Template method for setting shader resources with automatic type deduction
		 * @tparam T Resource type (UniformBuffer, Texture2D, etc.)
		 * @param name Resource name as defined in shader
		 * @param resource Resource to bind
		 * @return true if resource was set successfully, false otherwise
		 */
		template<typename T>
		bool SetResource(const std::string& name, const Ref<T>& resource)
		{
			if (!Shader)
			{
				OLO_CORE_WARN("Material::SetResource: No shader associated with material");
				return false;
			}

			// Try to get the shader registry (for OpenGL shaders)
			if (auto* openglShader = dynamic_cast<OpenGLShader*>(Shader.get()))
			{
				return openglShader->SetShaderResource(name, resource);
			}
			
			OLO_CORE_WARN("Material::SetResource: Shader type does not support resource registry");
			return false;
		}

		/**
		 * @brief Set a shader resource by name with type-safe input
		 * @param name Resource name as defined in shader
		 * @param input Type-safe resource input
		 * @return true if resource was set successfully, false otherwise
		 */
		bool SetResource(const std::string& name, const ShaderResourceInput& input)
		{
			if (!Shader)
			{
				OLO_CORE_WARN("Material::SetResource: No shader associated with material");
				return false;
			}

			// Try to get the shader registry (for OpenGL shaders)
			if (auto* openglShader = dynamic_cast<OpenGLShader*>(Shader.get()))
			{
				return openglShader->SetShaderResource(name, input);
			}
			
			OLO_CORE_WARN("Material::SetResource: Shader type does not support resource registry");
			return false;
		}

		/**
		 * @brief Apply all bound resources to the shader's registry
		 * This should be called before rendering with this material
		 */
		void ApplyToShader()
		{
			if (!Shader)
			{
				OLO_CORE_WARN("Material::ApplyToShader: No shader associated with material");
				return;
			}

			// Try to get the shader registry (for OpenGL shaders)
			if (auto* openglShader = dynamic_cast<OpenGLShader*>(Shader.get()))
			{
				openglShader->GetResourceRegistry().ApplyBindings();
			}
		}

		/**
		 * @brief Get the resource registry associated with this material's shader
		 * @return Pointer to registry if available, nullptr otherwise
		 */
		UniformBufferRegistry* GetResourceRegistry()
		{
			if (!Shader)
				return nullptr;

			if (auto* openglShader = dynamic_cast<OpenGLShader*>(Shader.get()))
			{
				return &openglShader->GetResourceRegistry();
			}
			
			return nullptr;
		}

		/**
		 * @brief Get the resource registry associated with this material's shader (const version)
		 * @return Pointer to registry if available, nullptr otherwise
		 */
		const UniformBufferRegistry* GetResourceRegistry() const
		{
			if (!Shader)
				return nullptr;

			if (auto* openglShader = dynamic_cast<const OpenGLShader*>(Shader.get()))
			{
				return &openglShader->GetResourceRegistry();
			}
			
			return nullptr;
		}

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
