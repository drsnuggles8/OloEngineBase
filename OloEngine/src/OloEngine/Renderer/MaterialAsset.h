#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Core/Assert.h"

#include <map>

namespace OloEngine
{

	class MaterialAsset : public Asset
	{
	public:
		explicit MaterialAsset(bool transparent = false);
		explicit MaterialAsset(Ref<Material> material);
		virtual ~MaterialAsset() override;

		virtual void OnDependencyUpdated(AssetHandle handle) override;

		glm::vec3 GetAlbedoColor() const;
		void SetAlbedoColor(const glm::vec3& color);

		float GetMetalness() const;
		void SetMetalness(float value);

		float GetRoughness() const;
		void SetRoughness(float value);

		float GetEmission() const;
		void SetEmission(float value);

		// Textures
		Ref<Texture2D> GetAlbedoMap() const;
		void SetAlbedoMap(AssetHandle handle);
		void ClearAlbedoMap();

		Ref<Texture2D> GetNormalMap() const;
		void SetNormalMap(AssetHandle handle);
		bool IsUsingNormalMap() const;
		void SetUseNormalMap(bool value);
		void ClearNormalMap();

		Ref<Texture2D> GetMetalnessMap() const;
		void SetMetalnessMap(AssetHandle handle);
		void ClearMetalnessMap();

		Ref<Texture2D> GetRoughnessMap() const;
		void SetRoughnessMap(AssetHandle handle);
		void ClearRoughnessMap();

		float GetTransparency() const;
		void SetTransparency(float transparency);

		bool IsShadowCasting() const { return m_Material ? !m_Material->GetFlag(MaterialFlag::DisableShadowCasting) : false; }
		void SetShadowCasting(bool castsShadows) { if (m_Material) m_Material->SetFlag(MaterialFlag::DisableShadowCasting, !castsShadows); }

		static AssetType GetStaticType() { return AssetType::Material; }
		virtual AssetType GetAssetType() const override { return GetStaticType(); }

		Ref<Material> GetMaterial() const { return m_Material; }
		void SetMaterial(Ref<Material> material) { OLO_CORE_ASSERT(material, "Material cannot be null"); m_Material = material; }

		bool IsTransparent() const { return m_Transparent; }
	private:
		void SetDefaults();
	private:
		Ref<Material> m_Material;

		struct MapAssets
		{
			AssetHandle AlbedoMap = 0;
			AssetHandle NormalMap = 0;
			AssetHandle MetalnessMap = 0;
			AssetHandle RoughnessMap = 0;
		} m_Maps;

		bool m_Transparent = false;

		friend class MaterialEditor;
	};

	class MaterialTable : public RefCounted
	{
	public:
		MaterialTable(u32 materialCount = 1);
		explicit MaterialTable(const Ref<MaterialTable>& other);
		~MaterialTable() = default;

		bool HasMaterial(u32 materialIndex) const { return m_Materials.find(materialIndex) != m_Materials.end(); }
		void SetMaterial(u32 index, AssetHandle material);
		void ClearMaterial(u32 index);

		AssetHandle GetMaterial(u32 materialIndex) const
		{
			auto it = m_Materials.find(materialIndex);
			if (it == m_Materials.end())
			{
				OLO_CORE_ERROR("MaterialTable::GetMaterial: Material index {} not found", materialIndex);
				return 0; // Return invalid handle instead of terminating
			}
			return it->second;
		}
		
		std::map<u32, AssetHandle>& GetMaterials() { return m_Materials; }
		const std::map<u32, AssetHandle>& GetMaterials() const { return m_Materials; }

		u32 GetMaterialCount() const { return static_cast<u32>(m_Materials.size()); }

		void Clear();
	private:
		std::map<u32, AssetHandle> m_Materials;
	};

}
