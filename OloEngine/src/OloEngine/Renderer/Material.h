#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RendererResource.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"

namespace OloEngine {

	enum class MaterialFlag
	{
		None       = 1 << 0,
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
	 * TODO: REFACTOR MATERIAL CLASS ARCHITECTURE
	 * 
	 * This class currently uses a hybrid approach with both:
	 * 1. RefCounted class-based interface for asset management
	 * 2. Public member variables for legacy struct-like access
	 * 
	 * This is TECHNICAL DEBT and should be refactored to use a consistent approach:
	 * - Either make it a proper class with encapsulated properties and methods
	 * - Or convert Model.cpp and Renderer3D.cpp to use the class-based interface
	 * 
	 * Current compatibility requirements:
	 * - MaterialAsset.cpp expects Set/Get/TryGet methods and uniform system
	 * - Model.cpp expects struct-like public members (AlbedoMap, etc.)
	 * - Renderer3D.cpp expects all PBR/legacy properties as public members
	 * 
	 * Recommended solution: Update Model.cpp and Renderer3D.cpp to use proper
	 * getter/setter methods and remove all public member variables.
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
		
		// Static factory method for PBR materials (for Model.cpp compatibility)
		static Material CreatePBR(const std::string& name, const glm::vec3& baseColor, float metallic = 0.0f, float roughness = 0.5f);
		
		// Static factory methods for common materials (for Sandbox3D compatibility)
		static Material CreateGoldMaterial(const std::string& name = "Gold");
		static Material CreateSilverMaterial(const std::string& name = "Silver");
		static Material CreateCopperMaterial(const std::string& name = "Copper");
		static Material CreatePlasticMaterial(const std::string& name = "Plastic", const glm::vec3& color = glm::vec3(0.1f, 0.1f, 0.8f));
		
		virtual ~Material() = default;

		virtual void Invalidate() {}
		virtual void OnShaderReloaded() {}

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

		virtual float& GetFloat(const std::string& name);
		virtual int32_t& GetInt(const std::string& name);
		virtual uint32_t& GetUInt(const std::string& name);
		virtual bool& GetBool(const std::string& name);
		virtual glm::vec2& GetVector2(const std::string& name);
		virtual glm::vec3& GetVector3(const std::string& name);
		virtual glm::vec4& GetVector4(const std::string& name);
		virtual glm::mat3& GetMatrix3(const std::string& name);
		virtual glm::mat4& GetMatrix4(const std::string& name);

		virtual Ref<Texture2D> GetTexture2D(const std::string& name);
		virtual Ref<TextureCubemap> GetTextureCube(const std::string& name);

		virtual Ref<Texture2D> TryGetTexture2D(const std::string& name);
		virtual Ref<TextureCubemap> TryGetTextureCube(const std::string& name);

		virtual uint32_t GetFlags() const { return m_MaterialFlags; }
		virtual void SetFlags(uint32_t flags) { m_MaterialFlags = flags; }

		virtual bool GetFlag(MaterialFlag flag) const { return (static_cast<uint32_t>(flag) & m_MaterialFlags) != 0; }
		virtual void SetFlag(MaterialFlag flag, bool value = true);

		virtual Ref<OloEngine::Shader> GetShader() { return m_Shader; }
		virtual const std::string& GetName() const { return m_Name; }
		
		// IBL configuration method (for Sandbox3D compatibility)
		void ConfigureIBL(const Ref<TextureCubemap>& environmentMap, 
		                  const Ref<TextureCubemap>& irradianceMap,
		                  const Ref<TextureCubemap>& prefilterMap,
		                  const Ref<Texture2D>& brdfLutMap);

		// TODO: REMOVE ALL PUBLIC MEMBERS BELOW - TECHNICAL DEBT
		// These public member variables exist only for compatibility with Model.cpp and Renderer3D.cpp
		// They should be replaced with proper getter/setter methods once those files are refactored
		// Public member variables for compatibility with Model.cpp and Renderer3D.cpp (struct-like access)
		
		// Material identification
		std::string Name = "Material";
		
		// Material type and shader
		MaterialType Type = MaterialType::PBR;
		Ref<OloEngine::Shader> Shader;
		
		// Legacy material properties (for backward compatibility)
		glm::vec3 Ambient = glm::vec3(0.2f);
		glm::vec3 Diffuse = glm::vec3(0.8f);
		glm::vec3 Specular = glm::vec3(1.0f);
		float Shininess = 32.0f;
		bool UseTextureMaps = false;
		Ref<Texture2D> DiffuseMap;
		Ref<Texture2D> SpecularMap;
		
		// PBR material properties
		glm::vec4 BaseColorFactor = glm::vec4(1.0f);     // Base color (albedo) with alpha
		glm::vec4 EmissiveFactor = glm::vec4(0.0f);      // Emissive color
		float MetallicFactor = 0.0f;                     // Metallic factor
		float RoughnessFactor = 1.0f;                    // Roughness factor
		float NormalScale = 1.0f;                        // Normal map scale
		float OcclusionStrength = 1.0f;                  // AO strength
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

		// Material properties storage
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
	};

}
