#include "OloEnginePCH.h"
#include "MaterialAsset.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Asset/AssetManager.h"

namespace OloEngine {

	static const std::string s_AlbedoColorUniform = "u_MaterialUniforms.AlbedoColor";
	static const std::string s_UseNormalMapUniform = "u_MaterialUniforms.UseNormalMap";
	static const std::string s_MetalnessUniform = "u_MaterialUniforms.Metalness";
	static const std::string s_RoughnessUniform = "u_MaterialUniforms.Roughness";
	static const std::string s_EmissionUniform = "u_MaterialUniforms.Emission";
	static const std::string s_TransparencyUniform = "u_MaterialUniforms.Transparency";

	static const std::string s_AlbedoMapUniform = "u_AlbedoTexture";
	static const std::string s_NormalMapUniform = "u_NormalTexture";
	static const std::string s_MetalnessMapUniform = "u_MetalnessTexture";
	static const std::string s_RoughnessMapUniform = "u_RoughnessTexture";

	MaterialAsset::MaterialAsset(bool transparent)
		: m_Transparent(transparent)
	{
		SetHandle(AssetHandle{});
		
		// Try to get the desired shader, fallback to a simpler shader if not available
		Ref<Shader> shader;
		ShaderLibrary& shaderLibrary = Renderer3D::GetShaderLibrary();
		
		if (transparent)
		{
			if (shaderLibrary.Exists("DefaultPBR_Transparent"))
			{
				shader = shaderLibrary.Get("DefaultPBR_Transparent");
			}
			else
			{
				OLO_CORE_WARN("MaterialAsset: DefaultPBR_Transparent shader not found, falling back to DefaultPBR");
				if (shaderLibrary.Exists("DefaultPBR"))
				{
					shader = shaderLibrary.Get("DefaultPBR");
				}
				else
				{
					OLO_CORE_ERROR("MaterialAsset: DefaultPBR shader not found, falling back to Basic3D");
					if (shaderLibrary.Exists("Basic3D"))
					{
						shader = shaderLibrary.Get("Basic3D");
					}
					else
					{
						OLO_CORE_ASSERT(false, "MaterialAsset: No fallback shader available! Basic3D shader is missing.");
						return; // Cannot create material without a shader
					}
				}
			}
		}
		else
		{
			if (shaderLibrary.Exists("DefaultPBR"))
			{
				shader = shaderLibrary.Get("DefaultPBR");
			}
			else
			{
				OLO_CORE_ERROR("MaterialAsset: DefaultPBR shader not found, falling back to Basic3D");
				if (shaderLibrary.Exists("Basic3D"))
				{
					shader = shaderLibrary.Get("Basic3D");
				}
				else
				{
					OLO_CORE_ASSERT(false, "MaterialAsset: No fallback shader available! Basic3D shader is missing.");
					return; // Cannot create material without a shader
				}
			}
		}
		
		// Verify we have a valid shader before creating the material
		if (!shader)
		{
			OLO_CORE_ASSERT(false, "MaterialAsset: Failed to obtain a valid shader");
			return;
		}
		
		m_Material = Material::Create(shader);

		SetDefaults();
	}

	MaterialAsset::MaterialAsset(Ref<Material> material)
	{
		SetHandle(AssetHandle{});
		m_Material = Material::Copy(material);
	}

	MaterialAsset::~MaterialAsset()
	{
	}

	void MaterialAsset::OnDependencyUpdated(AssetHandle handle)
	{
		// Handle material dependency updates if needed
	}

	glm::vec3 MaterialAsset::GetAlbedoColor() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		return m_Material->GetVector3(s_AlbedoColorUniform);
	}

	void MaterialAsset::SetAlbedoColor(const glm::vec3& color)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		m_Material->Set(s_AlbedoColorUniform, color);
	}

	float MaterialAsset::GetMetalness() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		return m_Material->GetFloat(s_MetalnessUniform);
	}

	void MaterialAsset::SetMetalness(float value)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		m_Material->Set(s_MetalnessUniform, value);
	}

	float MaterialAsset::GetRoughness() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		return m_Material->GetFloat(s_RoughnessUniform);
	}

	void MaterialAsset::SetRoughness(float value)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		m_Material->Set(s_RoughnessUniform, value);
	}

	float MaterialAsset::GetEmission() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		return m_Material->GetFloat(s_EmissionUniform);
	}

	void MaterialAsset::SetEmission(float value)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		m_Material->Set(s_EmissionUniform, value);
	}

	Ref<Texture2D> MaterialAsset::GetAlbedoMap() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		auto texture = m_Material->TryGetTexture2D(s_AlbedoMapUniform);
		if (texture)
			return texture;
		
		// Fall back to placeholder using centralized helper logic
		return GetTextureOrPlaceholder(AssetHandle{});
	}

	void MaterialAsset::SetAlbedoMap(AssetHandle handle)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		auto texture = GetTextureOrPlaceholder(handle);
		m_Material->Set(s_AlbedoMapUniform, texture);
	}

	Ref<Texture2D> MaterialAsset::GetNormalMap() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		auto texture = m_Material->TryGetTexture2D(s_NormalMapUniform);
		if (texture)
			return texture;
		
		// Fall back to placeholder using centralized helper logic
		return GetTextureOrPlaceholder(AssetHandle{});
	}

	void MaterialAsset::SetNormalMap(AssetHandle handle)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		auto texture = GetTextureOrPlaceholder(handle);
		m_Material->Set(s_NormalMapUniform, texture);
		
		// Enable normal mapping only if we got a real texture, not the placeholder
		bool isRealTexture = false;
		if (handle != 0 && texture)
		{
			// Check if the returned texture is different from the placeholder
			auto placeholderAsset = AssetManager::GetPlaceholderAsset(AssetType::Texture2D);
			if (placeholderAsset)
			{
				AssetHandle placeholderHandle = placeholderAsset->GetHandle();
				auto placeholderTexture = AssetManager::GetAsset<Texture2D>(placeholderHandle);
				isRealTexture = (texture != placeholderTexture);
			}
			else
			{
				// If no placeholder exists, any non-null texture is real
				isRealTexture = true;
			}
		}
		
		SetUseNormalMap(isRealTexture);
	}

	bool MaterialAsset::IsUsingNormalMap() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		return m_Material->GetBool(s_UseNormalMapUniform);
	}

	void MaterialAsset::SetUseNormalMap(bool value)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		m_Material->Set(s_UseNormalMapUniform, value);
	}

	Ref<Texture2D> MaterialAsset::GetMetalnessMap() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		auto texture = m_Material->TryGetTexture2D(s_MetalnessMapUniform);
		if (texture)
			return texture;
		
		// Fall back to placeholder using centralized helper logic
		return GetTextureOrPlaceholder(AssetHandle{});
	}

	void MaterialAsset::SetMetalnessMap(AssetHandle handle)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		auto texture = GetTextureOrPlaceholder(handle);
		m_Material->Set(s_MetalnessMapUniform, texture);
	}

	Ref<Texture2D> MaterialAsset::GetRoughnessMap() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		auto texture = m_Material->TryGetTexture2D(s_RoughnessMapUniform);
		if (texture)
			return texture;
		
		// Fall back to placeholder using centralized helper logic
		return GetTextureOrPlaceholder(AssetHandle{});
	}

	void MaterialAsset::SetRoughnessMap(AssetHandle handle)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		auto texture = GetTextureOrPlaceholder(handle);
		m_Material->Set(s_RoughnessMapUniform, texture);
	}

	float MaterialAsset::GetTransparency() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		return m_Material->GetFloat(s_TransparencyUniform);
	}

	void MaterialAsset::SetTransparency(float transparency)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		m_Material->Set(s_TransparencyUniform, transparency);
	}

	void MaterialAsset::SetDefaults()
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		if (m_Transparent)
		{
			// Set defaults for transparent materials
			SetAlbedoColor(glm::vec3(0.8f));
			SetTransparency(1.0f);
		}
		else
		{
			// Set defaults for opaque materials
			SetAlbedoColor(glm::vec3(0.8f));
			SetEmission(0.0f);
			SetUseNormalMap(false);
			SetMetalness(0.0f);
			SetRoughness(0.4f);
		}
	}

	Ref<Texture2D> MaterialAsset::GetTextureOrPlaceholder(AssetHandle handle) const
	{
		if (handle)
		{
			auto texture = AssetManager::GetAsset<Texture2D>(handle);
			if (texture)
				return texture;
		}
		
		// Fall back to placeholder texture when handle is invalid or texture couldn't be loaded
		auto placeholderAsset = AssetManager::GetPlaceholderAsset(AssetType::Texture2D);
		if (!placeholderAsset)
		{
			OLO_CORE_ERROR("MaterialAsset::GetTextureOrPlaceholder: No placeholder asset available for Texture2D");
			return nullptr; // Return null if no placeholder is available
		}
		
		AssetHandle placeholderHandle = placeholderAsset->GetHandle();
		auto placeholderTexture = AssetManager::GetAsset<Texture2D>(placeholderHandle);
		if (!placeholderTexture)
		{
			OLO_CORE_ERROR("MaterialAsset::GetTextureOrPlaceholder: Failed to load placeholder texture");
			return nullptr; // Return null if placeholder texture failed to load
		}
		
		return placeholderTexture;
	}

	// MaterialTable implementation
	MaterialTable::MaterialTable(u32 materialCount)
		: m_MaterialCount(materialCount)
	{
	}

	MaterialTable::MaterialTable(const Ref<MaterialTable>& other)
		: m_MaterialCount(other->m_MaterialCount)
	{
		const auto& meshMaterials = other->GetMaterials();
		for (const auto& [index, materialAsset] : meshMaterials)
			SetMaterial(index, materialAsset);
	}

	void MaterialTable::SetMaterial(u32 index, AssetHandle materialHandle)
	{
		// Strict bounds validation - prevent out-of-bounds writes
		if (index >= m_MaterialCount)
		{
			OLO_CORE_ERROR("MaterialTable::SetMaterial: Material index {} exceeds capacity {}. Use SetMaterialCapacity() to resize first.", index, m_MaterialCount);
			OLO_CORE_ASSERT(false, "MaterialTable::SetMaterial: Index out of bounds");
			return; // Fail gracefully in release builds
		}
		
		m_Materials[index] = materialHandle;
	}

	void MaterialTable::ClearMaterial(u32 index)
	{
		// Strict bounds validation - prevent out-of-bounds operations
		if (index >= m_MaterialCount)
		{
			OLO_CORE_ERROR("MaterialTable::ClearMaterial: Material index {} exceeds capacity {}.", index, m_MaterialCount);
			OLO_CORE_ASSERT(false, "MaterialTable::ClearMaterial: Index out of bounds");
			return; // Fail gracefully in release builds
		}
		
		m_Materials.erase(index);
	}

	void MaterialTable::Clear()
	{
		m_Materials.clear();
		// Note: Don't reset m_MaterialCount to 0 - this maintains the current capacity
		// Users can call SetMaterialCapacity(0) explicitly if they want to reduce capacity
	}

}
