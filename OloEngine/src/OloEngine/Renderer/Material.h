#pragma once

#include <glm/glm.hpp>
// Add these headers for GLM hash support
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "Platform/OpenGL/OpenGLShader.h"

namespace OloEngine
{
	struct Material
	{
		// Legacy material properties (for backward compatibility)
		glm::vec3 Ambient;
		glm::vec3 Diffuse;
		glm::vec3 Specular;
		f32 Shininess;
		bool UseTextureMaps = false;
		Ref<Texture2D> DiffuseMap;
		Ref<Texture2D> SpecularMap;
		
		// PBR material properties
		glm::vec4 BaseColorFactor = glm::vec4(1.0f);     // Base color (albedo) with alpha
		glm::vec4 EmissiveFactor = glm::vec4(0.0f);      // Emissive color
		f32 MetallicFactor = 0.0f;                       // Metallic factor
		f32 RoughnessFactor = 1.0f;                      // Roughness factor
		f32 NormalScale = 1.0f;                          // Normal map scale
		f32 OcclusionStrength = 1.0f;                    // AO strength
		bool EnablePBR = false;                          // Enable PBR rendering
		bool EnableIBL = false;                          // Enable IBL
		
		// PBR texture maps
		Ref<Texture2D> AlbedoMap;                        // Base color texture
		Ref<Texture2D> MetallicRoughnessMap;             // Metallic-roughness texture (glTF format)
		Ref<Texture2D> NormalMap;                        // Normal map
		Ref<Texture2D> AOMap;                            // Ambient occlusion map
		Ref<Texture2D> EmissiveMap;                      // Emissive map
		Ref<TextureCubemap> EnvironmentMap;              // Environment cubemap
		Ref<TextureCubemap> IrradianceMap;               // Irradiance cubemap
		Ref<TextureCubemap> PrefilterMap;                // Prefiltered environment map
		Ref<Texture2D> BRDFLutMap;                       // BRDF lookup table
		
		// General material properties
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
		 * @brief Configure PBR textures for this material
		 * Automatically sets up PBR texture bindings based on available texture maps
		 */
		void ConfigurePBRTextures()
		{
			if (!Shader)
			{
				OLO_CORE_WARN("Material::ConfigurePBRTextures: No shader associated with material");
				return;
			}

			// Try to get the shader registry (for OpenGL shaders)
			if (auto* openglShader = dynamic_cast<OpenGLShader*>(Shader.get()))
			{
				auto& registry = openglShader->GetResourceRegistry();
				
				// Bind PBR textures using standard binding layout
				if (AlbedoMap)
					registry.SetTexture("u_AlbedoMap", AlbedoMap);
				if (MetallicRoughnessMap)
					registry.SetTexture("u_MetallicRoughnessMap", MetallicRoughnessMap);
				if (NormalMap)
					registry.SetTexture("u_NormalMap", NormalMap);
				if (AOMap)
					registry.SetTexture("u_AOMap", AOMap);
				if (EmissiveMap)
					registry.SetTexture("u_EmissiveMap", EmissiveMap);
				if (EnvironmentMap)
					registry.SetTexture("u_EnvironmentMap", EnvironmentMap);
				if (IrradianceMap)
					registry.SetTexture("u_IrradianceMap", IrradianceMap);
				if (PrefilterMap)
					registry.SetTexture("u_PrefilterMap", PrefilterMap);
				if (BRDFLutMap)
					registry.SetTexture("u_BRDFLutMap", BRDFLutMap);
			}
		}

		/**
		 * @brief Get the resource registry associated with this material's shader
		 * @return Pointer to registry if available, nullptr otherwise
		 */
		ShaderResourceRegistry* GetResourceRegistry()
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
		const ShaderResourceRegistry* GetResourceRegistry() const
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
			// Compare basic legacy properties
			if (Ambient != other.Ambient ||
				Diffuse != other.Diffuse ||
				Specular != other.Specular ||
				Shininess != other.Shininess ||
				UseTextureMaps != other.UseTextureMaps)
			{
				return false;
			}
			
			// Compare PBR properties
			if (EnablePBR != other.EnablePBR ||
				BaseColorFactor != other.BaseColorFactor ||
				EmissiveFactor != other.EmissiveFactor ||
				MetallicFactor != other.MetallicFactor ||
				RoughnessFactor != other.RoughnessFactor ||
				NormalScale != other.NormalScale ||
				OcclusionStrength != other.OcclusionStrength ||
				EnableIBL != other.EnableIBL)
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
			
			// Compare PBR texture maps if PBR is enabled
			if (EnablePBR)
			{
				// Compare all PBR texture maps
				if ((AlbedoMap && other.AlbedoMap && *AlbedoMap != *other.AlbedoMap) ||
					(AlbedoMap && !other.AlbedoMap) || (!AlbedoMap && other.AlbedoMap))
					return false;
					
				if ((MetallicRoughnessMap && other.MetallicRoughnessMap && *MetallicRoughnessMap != *other.MetallicRoughnessMap) ||
					(MetallicRoughnessMap && !other.MetallicRoughnessMap) || (!MetallicRoughnessMap && other.MetallicRoughnessMap))
					return false;
					
				if ((NormalMap && other.NormalMap && *NormalMap != *other.NormalMap) ||
					(NormalMap && !other.NormalMap) || (!NormalMap && other.NormalMap))
					return false;
					
				if ((AOMap && other.AOMap && *AOMap != *other.AOMap) ||
					(AOMap && !other.AOMap) || (!AOMap && other.AOMap))
					return false;
					
				if ((EmissiveMap && other.EmissiveMap && *EmissiveMap != *other.EmissiveMap) ||
					(EmissiveMap && !other.EmissiveMap) || (!EmissiveMap && other.EmissiveMap))
					return false;
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
			HashCombine(key, std::hash<bool>()(UseTextureMaps));
			
			// Include PBR properties
			HashCombine(key, std::hash<bool>()(EnablePBR));
			if (EnablePBR)
			{
				HashCombine(key, std::hash<glm::vec4>()(BaseColorFactor));
				HashCombine(key, std::hash<glm::vec4>()(EmissiveFactor));
				HashCombine(key, std::hash<float>()(MetallicFactor));
				HashCombine(key, std::hash<float>()(RoughnessFactor));
				HashCombine(key, std::hash<float>()(NormalScale));
				HashCombine(key, std::hash<float>()(OcclusionStrength));
				HashCombine(key, std::hash<bool>()(EnableIBL));
			}
			
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
			
			// Include PBR texture IDs if PBR is enabled
			if (EnablePBR)
			{
				if (AlbedoMap)
					HashCombine(key, AlbedoMap->GetRendererID());
				if (MetallicRoughnessMap)
					HashCombine(key, MetallicRoughnessMap->GetRendererID());
				if (NormalMap)
					HashCombine(key, NormalMap->GetRendererID());
				if (AOMap)
					HashCombine(key, AOMap->GetRendererID());
				if (EmissiveMap)
					HashCombine(key, EmissiveMap->GetRendererID());
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
