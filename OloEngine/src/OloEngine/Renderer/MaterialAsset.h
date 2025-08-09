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

		glm::vec3& GetAlbedoColor();
		void SetAlbedoColor(const glm::vec3& color);

		float& GetMetalness();
		void SetMetalness(float value);

		float& GetRoughness();
		void SetRoughness(float value);

		float& GetEmission();
		void SetEmission(float value);

		// Textures
		Ref<Texture2D> GetAlbedoMap();
		void SetAlbedoMap(AssetHandle handle);
		void ClearAlbedoMap();

		Ref<Texture2D> GetNormalMap();
		void SetNormalMap(AssetHandle handle);
		bool IsUsingNormalMap();
		void SetUseNormalMap(bool value);
		void ClearNormalMap();

		Ref<Texture2D> GetMetalnessMap();
		void SetMetalnessMap(AssetHandle handle);
		void ClearMetalnessMap();

		Ref<Texture2D> GetRoughnessMap();
		void SetRoughnessMap(AssetHandle handle);
		void ClearRoughnessMap();

		float& GetTransparency();
		void SetTransparency(float transparency);

		bool IsShadowCasting() const { return !m_Material->GetFlag(MaterialFlag::DisableShadowCasting); }
		void SetShadowCasting(bool castsShadows) { m_Material->SetFlag(MaterialFlag::DisableShadowCasting, !castsShadows); }

		static AssetType GetStaticType() { return AssetType::Material; }
		virtual AssetType GetAssetType() const override { return GetStaticType(); }

		Ref<Material> GetMaterial() const { return m_Material; }
		void SetMaterial(Ref<Material> material) { m_Material = material; }

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
		MaterialTable(Ref<MaterialTable> other);
		~MaterialTable() = default;

		bool HasMaterial(u32 materialIndex) const { return m_Materials.find(materialIndex) != m_Materials.end(); }
		void SetMaterial(u32 index, AssetHandle material);
		void ClearMaterial(u32 index);

		AssetHandle GetMaterial(u32 materialIndex) const
		{
			OLO_CORE_VERIFY(HasMaterial(materialIndex));
			return m_Materials.at(materialIndex);
		}
		std::map<u32, AssetHandle>& GetMaterials() { return m_Materials; }
		const std::map<u32, AssetHandle>& GetMaterials() const { return m_Materials; }

		u32 GetMaterialCount() const { return m_MaterialCount; }
		void SetMaterialCount(u32 materialCount) { m_MaterialCount = materialCount; }

		void Clear();
	private:
		std::map<u32, AssetHandle> m_Materials;
		u32 m_MaterialCount;
	};

}
