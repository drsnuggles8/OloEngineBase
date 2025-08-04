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
	enum class MaterialType
	{
		Legacy = 0,		// Legacy Phong-style material
		PBR = 1			// Physically Based Rendering material
	};

	struct Material
	{
		// Material type and identification
		MaterialType Type = MaterialType::Legacy;
		std::string Name = "Material";

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
		AssetRef<Shader> Shader;
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
			if (auto* openglShader = dynamic_cast<OpenGLShader*>(Shader.Raw()))
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
			if (auto* openglShader = dynamic_cast<OpenGLShader*>(Shader.Raw()))
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
			if (auto* openglShader = dynamic_cast<OpenGLShader*>(Shader.Raw()))
			{
				openglShader->GetResourceRegistry()->ApplyBindings();
			}
		}

		// PBR-specific convenience methods
		/**
		 * @brief Set base color (albedo) for PBR materials
		 */
		void SetBaseColor(const glm::vec3& color) 
		{ 
			BaseColorFactor = glm::vec4(color, BaseColorFactor.a); 
			Type = MaterialType::PBR;
		}
		void SetBaseColor(const glm::vec4& color) 
		{ 
			BaseColorFactor = color; 
			Type = MaterialType::PBR;
		}

		/**
		 * @brief Set metallic and roughness factors for PBR materials
		 */
		void SetMetallicRoughness(float metallic, float roughness) 
		{ 
			MetallicFactor = metallic; 
			RoughnessFactor = roughness; 
			Type = MaterialType::PBR;
		}

		/**
		 * @brief Set emissive color for PBR materials
		 */
		void SetEmissive(const glm::vec3& emissive) 
		{ 
			EmissiveFactor = glm::vec4(emissive, 0.0f); 
			Type = MaterialType::PBR;
		}

		/**
		 * @brief Check if texture maps are available
		 */
		bool HasAlbedoMap() const { return AlbedoMap != nullptr; }
		bool HasMetallicRoughnessMap() const { return MetallicRoughnessMap != nullptr; }
		bool HasNormalMap() const { return NormalMap != nullptr; }
		bool HasAOMap() const { return AOMap != nullptr; }
		bool HasEmissiveMap() const { return EmissiveMap != nullptr; }
		bool HasIBLMaps() const { return IrradianceMap != nullptr && PrefilterMap != nullptr && BRDFLutMap != nullptr; }

		/**
		 * @brief Validate the material configuration
		 * @return true if material is valid and ready for rendering
		 */
		bool Validate() const
		{
			// Check if shader is available
			if (!Shader)
			{
				OLO_CORE_WARN("Material validation failed: No shader associated with material");
				return false;
			}

			// Validate based on material type
			if (Type == MaterialType::PBR)
			{
				// Check factor ranges for PBR materials
				if (MetallicFactor < 0.0f || MetallicFactor > 1.0f)
				{
					OLO_CORE_WARN("Material validation failed: MetallicFactor out of range [0,1]: {}", MetallicFactor);
					return false;
				}
				
				if (RoughnessFactor < 0.0f || RoughnessFactor > 1.0f)
				{
					OLO_CORE_WARN("Material validation failed: RoughnessFactor out of range [0,1]: {}", RoughnessFactor);
					return false;
				}
				
				if (NormalScale < 0.0f)
				{
					OLO_CORE_WARN("Material validation failed: NormalScale cannot be negative: {}", NormalScale);
					return false;
				}
				
				if (OcclusionStrength < 0.0f || OcclusionStrength > 1.0f)
				{
					OLO_CORE_WARN("Material validation failed: OcclusionStrength out of range [0,1]: {}", OcclusionStrength);
					return false;
				}

				// Validate IBL setup if enabled
				if (EnableIBL)
				{
					if (!IrradianceMap || !PrefilterMap || !BRDFLutMap)
					{
						OLO_CORE_WARN("Material validation failed: IBL enabled but missing required IBL textures");
						return false;
					}
				}
			}
			else // Legacy material validation
			{
				// Validate legacy material properties
				if (Shininess < 0.0f)
				{
					OLO_CORE_WARN("Material validation failed: Shininess cannot be negative: {}", Shininess);
					return false;
				}
			}

			return true;
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

			// Use the shader's resource registry directly (safer approach)
			auto* registry = GetResourceRegistry();
			if (!registry)
			{
				OLO_CORE_WARN("Material::ConfigurePBRTextures: No resource registry available for shader");
				return;
			}
			
			// Bind textures individually due to type differences (Texture2D vs TextureCubemap)
			if (AlbedoMap)
				registry->SetTexture("u_AlbedoMap", AlbedoMap);
			if (MetallicRoughnessMap)
				registry->SetTexture("u_MetallicRoughnessMap", MetallicRoughnessMap);
			if (NormalMap)
				registry->SetTexture("u_NormalMap", NormalMap);
			if (AOMap)
				registry->SetTexture("u_AOMap", AOMap);
			if (EmissiveMap)
				registry->SetTexture("u_EmissiveMap", EmissiveMap);
			if (EnvironmentMap)
				registry->SetTexture("u_EnvironmentMap", EnvironmentMap);
			if (IrradianceMap)
				registry->SetTexture("u_IrradianceMap", IrradianceMap);
			if (PrefilterMap)
				registry->SetTexture("u_PrefilterMap", PrefilterMap);
			if (BRDFLutMap)
				registry->SetTexture("u_BRDFLutMap", BRDFLutMap);
		}

		/**
		 * @brief Get the resource registry associated with this material's shader
		 * @return Pointer to registry if available, nullptr otherwise
		 */
		ShaderResourceRegistry* GetResourceRegistry()
		{
			return Shader ? Shader->GetResourceRegistry() : nullptr;
		}

		/**
		 * @brief Get the resource registry associated with this material's shader (const version)
		 * @return Pointer to registry if available, nullptr otherwise
		 */
		const ShaderResourceRegistry* GetResourceRegistry() const
		{
			return Shader ? Shader->GetResourceRegistry() : nullptr;
		}

		/**
		 * @brief Get the material name
		 * @return Material name string
		 */
		const std::string& GetName() const
		{
			return Name;
		}

		/**
		 * @brief Get the shader associated with this material
		 * @return Shader reference, or nullptr if no shader is set
		 */
		AssetRef<OloEngine::Shader> GetShader() const
		{
			return Shader;
		}

		bool operator==(const Material& other) const
		{
			// Compare material type first
			if (Type != other.Type || Name != other.Name)
			{
				return false;
			}

			// Compare basic legacy properties
			if (Ambient != other.Ambient ||
				Diffuse != other.Diffuse ||
				Specular != other.Specular ||
				Shininess != other.Shininess ||
				UseTextureMaps != other.UseTextureMaps)
			{
				return false;
			}
			
			// Compare PBR properties if this is a PBR material
			if (Type == MaterialType::PBR)
			{
				if (BaseColorFactor != other.BaseColorFactor ||
					EmissiveFactor != other.EmissiveFactor ||
					MetallicFactor != other.MetallicFactor ||
					RoughnessFactor != other.RoughnessFactor ||
					NormalScale != other.NormalScale ||
					OcclusionStrength != other.OcclusionStrength ||
					EnableIBL != other.EnableIBL)
				{
					return false;
				}
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
			
			// Compare PBR texture maps if this is a PBR material
			if (Type == MaterialType::PBR)
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
			
			// Include material type and name
			HashCombine(key, static_cast<u32>(Type));
			HashCombine(key, std::hash<std::string>()(Name));
			
			// Simple hash combination of material properties
			HashCombine(key, std::hash<glm::vec3>()(Ambient));
			HashCombine(key, std::hash<glm::vec3>()(Diffuse));
			HashCombine(key, std::hash<glm::vec3>()(Specular));
			HashCombine(key, std::hash<float>()(Shininess));
			HashCombine(key, std::hash<bool>()(UseTextureMaps));
			
			// Include PBR properties if this is a PBR material
			if (Type == MaterialType::PBR)
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
			
			// Include PBR texture IDs if this is a PBR material
			if (Type == MaterialType::PBR)
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

		// Static factory methods
		/**
		 * @brief Create a PBR material with specified properties
		 */
		static Material CreatePBR(const std::string& name, const glm::vec3& baseColor, float metallic = 0.0f, float roughness = 0.5f)
		{
			Material material;
			material.Type = MaterialType::PBR;
			material.Name = name;
			material.BaseColorFactor = glm::vec4(baseColor, 1.0f);
			material.MetallicFactor = metallic;
			material.RoughnessFactor = roughness;
			material.NormalScale = 1.0f;
			material.OcclusionStrength = 1.0f;
			material.EmissiveFactor = glm::vec4(0.0f);
			material.EnableIBL = false;
			return material;
		}

		/**
		 * @brief Create a legacy Phong material with specified properties
		 */
		static Material CreateLegacy(const std::string& name, const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& specular, float shininess = 32.0f)
		{
			Material material;
			material.Type = MaterialType::Legacy;
			material.Name = name;
			material.Ambient = ambient;
			material.Diffuse = diffuse;
			material.Specular = specular;
			material.Shininess = shininess;
			material.UseTextureMaps = false;
			material.EnableIBL = false;
			return material;
		}

		// PBR preset materials
		static Material CreateGoldMaterial(const std::string& name = "Gold")
		{
			return CreatePBR(name, glm::vec3(1.0f, 0.765557f, 0.336057f), 1.0f, 0.1f);
		}

		static Material CreateSilverMaterial(const std::string& name = "Silver")
		{
			return CreatePBR(name, glm::vec3(0.950f, 0.930f, 0.880f), 1.0f, 0.05f);
		}

		static Material CreateCopperMaterial(const std::string& name = "Copper")
		{
			return CreatePBR(name, glm::vec3(0.95f, 0.64f, 0.54f), 1.0f, 0.15f);
		}

		static Material CreatePlasticMaterial(const std::string& name, const glm::vec3& color)
		{
			return CreatePBR(name, color, 0.0f, 0.5f);
		}

		static Material CreateMetalMaterial(const std::string& name, const glm::vec3& color, float roughness = 0.1f)
		{
			return CreatePBR(name, color, 1.0f, roughness);
		}

		/**
		 * @brief Configure IBL (Image-Based Lighting) for this material
		 */
		void ConfigureIBL(const Ref<TextureCubemap>& environmentMap,
			const Ref<TextureCubemap>& irradianceMap = nullptr,
			const Ref<TextureCubemap>& prefilterMap = nullptr,
			const Ref<Texture2D>& brdfLutMap = nullptr)
		{
			EnableIBL = true;
			EnvironmentMap = environmentMap;
			IrradianceMap = irradianceMap;
			PrefilterMap = prefilterMap;
			BRDFLutMap = brdfLutMap;
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
