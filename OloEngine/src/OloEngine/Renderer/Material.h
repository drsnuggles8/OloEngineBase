#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RendererResource.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"

namespace OloEngine {

	enum class MaterialFlag
	{
		None       = 0,
		DepthTest  = 1 << 1,
		Blend      = 1 << 2,
		TwoSided   = 1 << 3,
		DisableShadowCasting = 1 << 4
	};

	enum class MaterialType
	{
		Legacy = 0,		// Legacy Phong-style material
		PBR = 1			// Physically Based Rendering material
	};

	/**
	 * @brief Material class for handling PBR and legacy material properties
	 * 
	 * This class uses a consistent encapsulated design with getter/setter methods.
	 * All material properties are accessed through typed methods that handle
	 * both the uniform system and direct property access efficiently.
	 * 
	 * Features:
	 * - Unified interface for both PBR and legacy materials
	 * - Automatic uniform management for shader binding
	 * - Type-safe property access with validation
	 * - Efficient texture and parameter caching
	 * - Asset dependency tracking integration
	 */
	class Material : public RendererResource
	{
	public:
		// Default constructor for struct-like usage
		Material();
		
		// Copy constructor for value semantics (Model.cpp uses Material as value type)
		Material(const Material& other);
		
		// Assignment operator for value semantics
		Material& operator=(const Material& other);
		
		static Ref<Material> Create(const Ref<OloEngine::Shader>& shader, const std::string& name = "");
		static Ref<Material> Copy(const Ref<Material>& other, const std::string& name = "");
		
		// Static factory method for PBR materials - returns Ref<Material> for consistency
		static Ref<Material> CreatePBR(const std::string& name, const glm::vec3& baseColor, float metallic = 0.0f, float roughness = 0.5f);
		
		// Static factory methods for common materials (for Sandbox3D compatibility)
		static Material CreateGoldMaterial(const std::string& name = "Gold");
		static Material CreateSilverMaterial(const std::string& name = "Silver");
		static Material CreateCopperMaterial(const std::string& name = "Copper");
		static Material CreatePlasticMaterial(const std::string& name = "Plastic", const glm::vec3& color = glm::vec3(0.1f, 0.1f, 0.8f));
		
		virtual ~Material() = default;

		virtual void Invalidate() {}
		virtual void OnShaderReloaded() {}

		// Material property accessors
		void SetName(const std::string& name) { m_MaterialName = name; }
		const std::string& GetName() const { return m_MaterialName; }
		
		void SetType(MaterialType type) { m_MaterialType = type; }
		MaterialType GetType() const { return m_MaterialType; }
		
		void SetShader(const Ref<Shader>& shader) { m_MaterialShader = shader; }

		virtual void Set(const std::string& name, float value);
		virtual void Set(const std::string& name, int value);
		virtual void Set(const std::string& name, uint32_t value);
		virtual void Set(const std::string& name, bool value);
		virtual void Set(const std::string& name, const glm::vec2& value);
		virtual void Set(const std::string& name, const glm::vec3& value);
		virtual void Set(const std::string& name, const glm::vec4& value);
		virtual void Set(const std::string& name, const glm::ivec2& value);
		virtual void Set(const std::string& name, const glm::ivec3& value);
		virtual void Set(const std::string& name, const glm::ivec4& value);

		virtual void Set(const std::string& name, const glm::mat3& value);
		virtual void Set(const std::string& name, const glm::mat4& value);

		virtual void Set(const std::string& name, const Ref<Texture2D>& texture);
		virtual void Set(const std::string& name, const Ref<Texture2D>& texture, uint32_t arrayIndex);
		virtual void Set(const std::string& name, const Ref<TextureCubemap>& texture);

		virtual float GetFloat(const std::string& name) const;
		virtual int32_t GetInt(const std::string& name) const;
		virtual uint32_t GetUInt(const std::string& name) const;
		virtual bool GetBool(const std::string& name) const;
		virtual const glm::vec2& GetVector2(const std::string& name) const;
		virtual const glm::vec3& GetVector3(const std::string& name) const;
		virtual const glm::vec4& GetVector4(const std::string& name) const;
		virtual const glm::mat3& GetMatrix3(const std::string& name) const;
		virtual const glm::mat4& GetMatrix4(const std::string& name) const;

		virtual Ref<Texture2D> GetTexture2D(const std::string& name);
		virtual Ref<TextureCubemap> GetTextureCube(const std::string& name);

		virtual Ref<Texture2D> TryGetTexture2D(const std::string& name);
		virtual Ref<TextureCubemap> TryGetTextureCube(const std::string& name);

		virtual uint32_t GetFlags() const { return m_MaterialFlags; }
		virtual void SetFlags(uint32_t flags) { m_MaterialFlags = flags; }

		virtual bool GetFlag(MaterialFlag flag) const { return (static_cast<uint32_t>(flag) & m_MaterialFlags) != 0; }
		virtual void SetFlag(MaterialFlag flag, bool value = true);

		virtual Ref<OloEngine::Shader> GetShader() { return m_Shader; }
		
		// IBL configuration method (for Sandbox3D compatibility)
		void ConfigureIBL(const Ref<TextureCubemap>& environmentMap, 
		                  const Ref<TextureCubemap>& irradianceMap,
		                  const Ref<TextureCubemap>& prefilterMap,
		                  const Ref<Texture2D>& brdfLutMap);

		// =====================================================================
		// TYPED PROPERTY ACCESSORS (Replacement for public member variables)
		// =====================================================================
		
		// Material identification
		void SetMaterialName(const std::string& name) { m_MaterialName = name; }
		const std::string& GetMaterialName() const { return m_MaterialName; }
		
		// Material type and shader management
		MaterialType GetMaterialType() const { return m_MaterialType; }
		void SetMaterialType(MaterialType type) { m_MaterialType = type; }
		void SetMaterialShader(const Ref<Shader>& shader) { m_MaterialShader = shader; }
		Ref<Shader> GetMaterialShader() const { return m_MaterialShader; }
		
		// Legacy material properties (for backward compatibility)
		const glm::vec3& GetAmbient() const { return m_Ambient; }
		void SetAmbient(const glm::vec3& ambient) { m_Ambient = ambient; }
		const glm::vec3& GetDiffuse() const { return m_Diffuse; }
		void SetDiffuse(const glm::vec3& diffuse) { m_Diffuse = diffuse; }
		const glm::vec3& GetSpecular() const { return m_Specular; }
		void SetSpecular(const glm::vec3& specular) { m_Specular = specular; }
		float GetShininess() const { return m_Shininess; }
		void SetShininess(float shininess) { m_Shininess = shininess; }
		bool IsUsingTextureMaps() const { return m_UseTextureMaps; }
		void SetUseTextureMaps(bool use) { m_UseTextureMaps = use; }
		Ref<Texture2D> GetDiffuseMap() const { return m_DiffuseMap; }
		void SetDiffuseMap(const Ref<Texture2D>& texture) { m_DiffuseMap = texture; }
		Ref<Texture2D> GetSpecularMap() const { return m_SpecularMap; }
		void SetSpecularMap(const Ref<Texture2D>& texture) { m_SpecularMap = texture; }
		
		// PBR material properties
		const glm::vec4& GetBaseColorFactor() const { return m_BaseColorFactor; }
		void SetBaseColorFactor(const glm::vec4& color) { m_BaseColorFactor = color; }
		const glm::vec4& GetEmissiveFactor() const { return m_EmissiveFactor; }
		void SetEmissiveFactor(const glm::vec4& emissive) { m_EmissiveFactor = emissive; }
		float GetMetallicFactor() const { return m_MetallicFactor; }
		void SetMetallicFactor(float metallic) { m_MetallicFactor = metallic; }
		float GetRoughnessFactor() const { return m_RoughnessFactor; }
		void SetRoughnessFactor(float roughness) { m_RoughnessFactor = roughness; }
		float GetNormalScale() const { return m_NormalScale; }
		void SetNormalScale(float scale) { m_NormalScale = scale; }
		float GetOcclusionStrength() const { return m_OcclusionStrength; }
		void SetOcclusionStrength(float strength) { m_OcclusionStrength = strength; }
		bool IsIBLEnabled() const { return m_EnableIBL; }
		void SetEnableIBL(bool enable) { m_EnableIBL = enable; }
		
		// PBR texture maps
		Ref<Texture2D> GetAlbedoMap() const { return m_AlbedoMap; }
		void SetAlbedoMap(const Ref<Texture2D>& texture) { m_AlbedoMap = texture; }
		Ref<Texture2D> GetMetallicRoughnessMap() const { return m_MetallicRoughnessMap; }
		void SetMetallicRoughnessMap(const Ref<Texture2D>& texture) { m_MetallicRoughnessMap = texture; }
		Ref<Texture2D> GetNormalMap() const { return m_NormalMap; }
		void SetNormalMap(const Ref<Texture2D>& texture) { m_NormalMap = texture; }
		Ref<Texture2D> GetAOMap() const { return m_AOMap; }
		void SetAOMap(const Ref<Texture2D>& texture) { m_AOMap = texture; }
		Ref<Texture2D> GetEmissiveMap() const { return m_EmissiveMap; }
		void SetEmissiveMap(const Ref<Texture2D>& texture) { m_EmissiveMap = texture; }
		Ref<TextureCubemap> GetEnvironmentMap() const { return m_EnvironmentMap; }
		void SetEnvironmentMap(const Ref<TextureCubemap>& texture) { m_EnvironmentMap = texture; }
		Ref<TextureCubemap> GetIrradianceMap() const { return m_IrradianceMap; }
		void SetIrradianceMap(const Ref<TextureCubemap>& texture) { m_IrradianceMap = texture; }
		Ref<TextureCubemap> GetPrefilterMap() const { return m_PrefilterMap; }
		void SetPrefilterMap(const Ref<TextureCubemap>& texture) { m_PrefilterMap = texture; }
		Ref<Texture2D> GetBRDFLutMap() const { return m_BRDFLutMap; }
		void SetBRDFLutMap(const Ref<Texture2D>& texture) { m_BRDFLutMap = texture; }

		// =====================================================================
		// LEGACY COMPATIBILITY: Public reference aliases to private members
		// =====================================================================
		// These properties provide reference access to private members for compatibility
		
		// Material identification (aliases to private members)
		std::string& Name = m_MaterialName;
		
		// Material type and shader (aliases to private members)
		MaterialType& Type = m_MaterialType;
		Ref<OloEngine::Shader>& Shader = m_MaterialShader;
		
		// Legacy material properties (aliases to private members)
		glm::vec3& Ambient = m_Ambient;
		glm::vec3& Diffuse = m_Diffuse;
		glm::vec3& Specular = m_Specular;
		float& Shininess = m_Shininess;
		bool& UseTextureMaps = m_UseTextureMaps;
		Ref<Texture2D>& DiffuseMap = m_DiffuseMap;
		Ref<Texture2D>& SpecularMap = m_SpecularMap;
		
		// PBR material properties (aliases to private members)
		glm::vec4& BaseColorFactor = m_BaseColorFactor;     // Base color (albedo) with alpha
		glm::vec4& EmissiveFactor = m_EmissiveFactor;       // Emissive color
		float& MetallicFactor = m_MetallicFactor;           // Metallic factor
		float& RoughnessFactor = m_RoughnessFactor;         // Roughness factor
		float& NormalScale = m_NormalScale;                 // Normal map scale
		float& OcclusionStrength = m_OcclusionStrength;     // AO strength
		bool& EnableIBL = m_EnableIBL;                      // Enable IBL
		
		// PBR texture maps (aliases to private members)
		Ref<Texture2D>& AlbedoMap = m_AlbedoMap;                        // Base color texture
		Ref<Texture2D>& MetallicRoughnessMap = m_MetallicRoughnessMap;  // Metallic-roughness texture (glTF format)
		Ref<Texture2D>& NormalMap = m_NormalMap;                        // Normal map
		Ref<Texture2D>& AOMap = m_AOMap;                                // Ambient occlusion map
		Ref<Texture2D>& EmissiveMap = m_EmissiveMap;                    // Emissive map
		Ref<TextureCubemap>& EnvironmentMap = m_EnvironmentMap;         // Environment cubemap
		Ref<TextureCubemap>& IrradianceMap = m_IrradianceMap;           // Irradiance cubemap
		Ref<TextureCubemap>& PrefilterMap = m_PrefilterMap;             // Prefiltered environment map
		Ref<Texture2D>& BRDFLutMap = m_BRDFLutMap;                      // BRDF lookup table

		// Asset interface
		static AssetType GetStaticType() { return AssetType::Material; }
		virtual AssetType GetAssetType() const override { return GetStaticType(); }

		// Accessors for serialization
		const std::unordered_map<std::string, Ref<Texture2D>>& GetTexture2DUniforms() const { return m_Texture2DUniforms; }
		const std::unordered_map<std::string, Ref<TextureCubemap>>& GetTextureCubeUniforms() const { return m_TextureCubeUniforms; }
		const std::unordered_map<std::string, float>& GetFloatUniforms() const { return m_FloatUniforms; }
		const std::unordered_map<std::string, int>& GetIntUniforms() const { return m_IntUniforms; }
		const std::unordered_map<std::string, glm::vec3>& GetVec3Uniforms() const { return m_Vec3Uniforms; }
		const std::unordered_map<std::string, glm::vec4>& GetVec4Uniforms() const { return m_Vec4Uniforms; }

	protected:
		Material(const Ref<OloEngine::Shader>& shader, const std::string& name = "");

	protected:
		Ref<OloEngine::Shader> m_Shader;
		std::string m_Name;
		uint32_t m_MaterialFlags = static_cast<uint32_t>(MaterialFlag::DepthTest);

		// Material properties storage (uniform system)
		std::unordered_map<std::string, float> m_FloatUniforms;
		std::unordered_map<std::string, int> m_IntUniforms;
		std::unordered_map<std::string, uint32_t> m_UIntUniforms;
		std::unordered_map<std::string, bool> m_BoolUniforms;
		std::unordered_map<std::string, glm::vec2> m_Vec2Uniforms;
		std::unordered_map<std::string, glm::vec3> m_Vec3Uniforms;
		std::unordered_map<std::string, glm::vec4> m_Vec4Uniforms;
		std::unordered_map<std::string, glm::mat3> m_Mat3Uniforms;
		std::unordered_map<std::string, glm::mat4> m_Mat4Uniforms;
		std::unordered_map<std::string, Ref<Texture2D>> m_Texture2DUniforms;
		std::unordered_map<std::string, Ref<TextureCubemap>> m_TextureCubeUniforms;

		// =====================================================================
		// PRIVATE MATERIAL PROPERTIES (Encapsulated)
		// =====================================================================
		
		// Material identification
		std::string m_MaterialName = "Material";
		
		// Material type and shader
		MaterialType m_MaterialType = MaterialType::PBR;
		Ref<OloEngine::Shader> m_MaterialShader;
		
		// Legacy material properties (for backward compatibility)
		glm::vec3 m_Ambient = glm::vec3(0.2f);
		glm::vec3 m_Diffuse = glm::vec3(0.8f);
		glm::vec3 m_Specular = glm::vec3(1.0f);
		float m_Shininess = 32.0f;
		bool m_UseTextureMaps = false;
		Ref<Texture2D> m_DiffuseMap;
		Ref<Texture2D> m_SpecularMap;
		
		// PBR material properties
		glm::vec4 m_BaseColorFactor = glm::vec4(1.0f);     // Base color (albedo) with alpha
		glm::vec4 m_EmissiveFactor = glm::vec4(0.0f);      // Emissive color
		float m_MetallicFactor = 0.0f;                     // Metallic factor
		float m_RoughnessFactor = 1.0f;                    // Roughness factor
		float m_NormalScale = 1.0f;                        // Normal map scale
		float m_OcclusionStrength = 1.0f;                  // AO strength
		bool m_EnableIBL = false;                          // Enable IBL
		
		// PBR texture maps
		Ref<Texture2D> m_AlbedoMap;                        // Base color texture
		Ref<Texture2D> m_MetallicRoughnessMap;             // Metallic-roughness texture (glTF format)
		Ref<Texture2D> m_NormalMap;                        // Normal map
		Ref<Texture2D> m_AOMap;                            // Ambient occlusion map
		Ref<Texture2D> m_EmissiveMap;                      // Emissive map
		Ref<TextureCubemap> m_EnvironmentMap;              // Environment cubemap
		Ref<TextureCubemap> m_IrradianceMap;               // Irradiance cubemap
		Ref<TextureCubemap> m_PrefilterMap;                // Prefiltered environment map
		Ref<Texture2D> m_BRDFLutMap;                       // BRDF lookup table
	};

}
