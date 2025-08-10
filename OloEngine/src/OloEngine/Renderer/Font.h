#pragma once

#include <filesystem>
#include <string>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/RendererResource.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
	struct MSDFData;

	class Font : public RendererResource
	{
	public:
		explicit Font(const std::filesystem::path& font);
		~Font() override;

		[[nodiscard("Store this!")]] const MSDFData* GetMSDFData() const { return m_Data.get(); }
		Ref<Texture2D> GetAtlasTexture() const { return m_AtlasTexture; }
		
		const std::string& GetName() const { return m_Name; }
		const std::string& GetPath() const { return m_Path; }

		// Asset interface
		static AssetType GetStaticType() { return AssetType::Font; }
		virtual AssetType GetAssetType() const override { return GetStaticType(); }

		static Ref<Font> GetDefault();
		static Ref<Font> Create(const std::filesystem::path& font);
	private:
		Scope<MSDFData> m_Data;
		Ref<Texture2D> m_AtlasTexture;
		std::string m_Name;
		std::string m_Path;
	};
}
