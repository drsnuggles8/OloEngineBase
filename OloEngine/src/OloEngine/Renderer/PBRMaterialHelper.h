#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include <glm/glm.hpp>

namespace OloEngine
{
	/**
	 * @brief PBR Material Helper Class
	 * 
	 * This class provides utility functions for creating and managing PBR materials
	 * following the glTF 2.0 metallic-roughness workflow.
	 */
	class PBRMaterialHelper
	{
	public:
		/**
		 * @brief Create a basic PBR material with default values
		 * @param baseColor Base color (albedo) of the material
		 * @param metallic Metallic factor (0.0 = dielectric, 1.0 = metallic)
		 * @param roughness Roughness factor (0.0 = mirror, 1.0 = completely rough)
		 * @return Configured Material with PBR properties
		 */
		static Material CreateBasicPBRMaterial(const glm::vec3& baseColor, float metallic = 0.0f, float roughness = 0.5f)
		{
			Material material;
			
			// Enable PBR
			material.EnablePBR = true;
			material.EnableIBL = false; // Can be enabled if IBL textures are available
			
			// Set PBR properties
			material.BaseColorFactor = glm::vec4(baseColor, 1.0f);
			material.MetallicFactor = glm::clamp(metallic, 0.0f, 1.0f);
			material.RoughnessFactor = glm::clamp(roughness, 0.0f, 1.0f);
			material.NormalScale = 1.0f;
			material.OcclusionStrength = 1.0f;
			material.EmissiveFactor = glm::vec4(0.0f);
			
			// Set legacy properties for backward compatibility
			material.Ambient = baseColor * 0.1f;
			material.Diffuse = baseColor;
			material.Specular = glm::mix(glm::vec3(0.04f), baseColor, metallic);
			material.Shininess = (1.0f - roughness) * 128.0f;
			material.UseTextureMaps = false;
			
			return material;
		}
		
		/**
		 * @brief Create a textured PBR material
		 * @param baseColor Base color factor (will be multiplied with albedo texture)
		 * @param metallic Metallic factor
		 * @param roughness Roughness factor
		 * @param albedoMap Base color texture (optional)
		 * @param metallicRoughnessMap Metallic-roughness texture (optional)
		 * @param normalMap Normal map texture (optional)
		 * @return Configured Material with PBR textures
		 */
		static Material CreateTexturedPBRMaterial(
			const glm::vec3& baseColor,
			float metallic,
			float roughness,
			const Ref<Texture2D>& albedoMap = nullptr,
			const Ref<Texture2D>& metallicRoughnessMap = nullptr,
			const Ref<Texture2D>& normalMap = nullptr)
		{
			Material material = CreateBasicPBRMaterial(baseColor, metallic, roughness);
			
			// Set textures
			material.AlbedoMap = albedoMap;
			material.MetallicRoughnessMap = metallicRoughnessMap;
			material.NormalMap = normalMap;
			
			return material;
		}
		
		/**
		 * @brief Create a metal material preset
		 * @param baseColor Metal color (e.g., gold, silver, copper)
		 * @param roughness Surface roughness
		 * @return Configured metallic Material
		 */
		static Material CreateMetalMaterial(const glm::vec3& baseColor, float roughness = 0.1f)
		{
			return CreateBasicPBRMaterial(baseColor, 1.0f, roughness);
		}
		
		/**
		 * @brief Create a dielectric material preset
		 * @param baseColor Dielectric color
		 * @param roughness Surface roughness
		 * @return Configured dielectric Material
		 */
		static Material CreateDielectricMaterial(const glm::vec3& baseColor, float roughness = 0.5f)
		{
			return CreateBasicPBRMaterial(baseColor, 0.0f, roughness);
		}
		
		/**
		 * @brief Create common material presets
		 */
		static Material CreateGoldMaterial()
		{
			return CreateMetalMaterial(glm::vec3(1.0f, 0.765f, 0.336f), 0.1f);
		}
		
		static Material CreateSilverMaterial()
		{
			return CreateMetalMaterial(glm::vec3(0.972f, 0.960f, 0.915f), 0.1f);
		}
		
		static Material CreateCopperMaterial()
		{
			return CreateMetalMaterial(glm::vec3(0.955f, 0.637f, 0.538f), 0.1f);
		}
		
		static Material CreatePlasticMaterial(const glm::vec3& color)
		{
			return CreateDielectricMaterial(color, 0.5f);
		}
		
		static Material CreateRubberMaterial(const glm::vec3& color)
		{
			return CreateDielectricMaterial(color, 0.9f);
		}
		
		/**
		 * @brief Configure IBL (Image-Based Lighting) for a material
		 * @param material Material to configure
		 * @param environmentMap Environment cubemap
		 * @param irradianceMap Irradiance cubemap
		 * @param prefilterMap Prefiltered environment map
		 * @param brdfLutMap BRDF lookup table
		 */
		static void ConfigureIBL(Material& material, 
			const Ref<TextureCubemap>& environmentMap,
			const Ref<TextureCubemap>& irradianceMap = nullptr,
			const Ref<TextureCubemap>& prefilterMap = nullptr,
			const Ref<Texture2D>& brdfLutMap = nullptr)
		{
			material.EnableIBL = true;
			material.EnvironmentMap = environmentMap;
			material.IrradianceMap = irradianceMap;
			material.PrefilterMap = prefilterMap;
			material.BRDFLutMap = brdfLutMap;
		}
	};
}
