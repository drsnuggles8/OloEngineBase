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
		SetHandle(0);
		
		if (transparent)
			m_Material = Material::Create(Renderer3D::GetShaderLibrary().Get("DefaultPBR_Transparent"));
		else
			m_Material = Material::Create(Renderer3D::GetShaderLibrary().Get("DefaultPBR"));

		SetDefaults();
	}

	MaterialAsset::MaterialAsset(Ref<Material> material)
	{
		SetHandle(0);
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
		// Remove const from this to call non-const TryGetTexture2D
		return const_cast<MaterialAsset*>(this)->m_Material->TryGetTexture2D(s_AlbedoMapUniform);
	}

	void MaterialAsset::SetAlbedoMap(AssetHandle handle)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		
		if (handle)
		{
			auto texture = AssetManager::GetAsset<Texture2D>(handle);
			if (texture)
			{
				m_Material->Set(s_AlbedoMapUniform, texture);
			}
		}
		else
		{
			auto placeholderTexture = AssetManager::GetAsset<Texture2D>(AssetManager::GetPlaceholderAsset(AssetType::Texture2D)->GetHandle());
			m_Material->Set(s_AlbedoMapUniform, placeholderTexture);
		}
	}

	Ref<Texture2D> MaterialAsset::GetNormalMap() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		return const_cast<MaterialAsset*>(this)->m_Material->TryGetTexture2D(s_NormalMapUniform);
	}

	void MaterialAsset::SetNormalMap(AssetHandle handle)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		
		if (handle)
		{
			auto texture = AssetManager::GetAsset<Texture2D>(handle);
			if (texture)
			{
				m_Material->Set(s_NormalMapUniform, texture);
				SetUseNormalMap(true);
			}
		}
		else
		{
			auto placeholderTexture = AssetManager::GetAsset<Texture2D>(AssetManager::GetPlaceholderAsset(AssetType::Texture2D)->GetHandle());
			m_Material->Set(s_NormalMapUniform, placeholderTexture);
			SetUseNormalMap(false);
		}
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
		return const_cast<MaterialAsset*>(this)->m_Material->TryGetTexture2D(s_MetalnessMapUniform);
	}

	void MaterialAsset::SetMetalnessMap(AssetHandle handle)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		
		if (handle)
		{
			auto texture = AssetManager::GetAsset<Texture2D>(handle);
			if (texture)
			{
				m_Material->Set(s_MetalnessMapUniform, texture);
			}
		}
		else
		{
			auto placeholderTexture = AssetManager::GetAsset<Texture2D>(AssetManager::GetPlaceholderAsset(AssetType::Texture2D)->GetHandle());
			m_Material->Set(s_MetalnessMapUniform, placeholderTexture);
		}
	}

	Ref<Texture2D> MaterialAsset::GetRoughnessMap() const
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		return const_cast<MaterialAsset*>(this)->m_Material->TryGetTexture2D(s_RoughnessMapUniform);
	}

	void MaterialAsset::SetRoughnessMap(AssetHandle handle)
	{
		OLO_CORE_VERIFY(m_Material, "Material instance is null");
		
		if (handle)
		{
			auto texture = AssetManager::GetAsset<Texture2D>(handle);
			if (texture)
			{
				m_Material->Set(s_RoughnessMapUniform, texture);
			}
		}
		else
		{
			auto placeholderTexture = AssetManager::GetAsset<Texture2D>(AssetManager::GetPlaceholderAsset(AssetType::Texture2D)->GetHandle());
			m_Material->Set(s_RoughnessMapUniform, placeholderTexture);
		}
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

	// MaterialTable implementation
	MaterialTable::MaterialTable(u32 materialCount)
		: m_MaterialCount(materialCount)
	{
	}

	MaterialTable::MaterialTable(const Ref<MaterialTable>& other)
		: m_MaterialCount(other->m_MaterialCount)
	{
		const auto& meshMaterials = other->GetMaterials();
		for (auto [index, materialAsset] : meshMaterials)
			SetMaterial(index, materialAsset);
	}

	void MaterialTable::SetMaterial(u32 index, AssetHandle materialHandle)
	{
		m_Materials[index] = materialHandle;
	}

	void MaterialTable::ClearMaterial(u32 index)
	{
		m_Materials.erase(index);
	}

	void MaterialTable::Clear()
	{
		m_Materials.clear();
	}

}
