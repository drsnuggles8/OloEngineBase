#pragma once

#include <filesystem>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
	struct MSDFData;

	class Font : public RefCounted
	{
	public:
		explicit Font(const std::filesystem::path& font);
		~Font();

		[[nodiscard("Store this!")]] const MSDFData* GetMSDFData() const { return m_Data; }
		Ref<Texture2D> GetAtlasTexture() const { return m_AtlasTexture; }

		static Ref<Font> GetDefault();
		static Ref<Font> Create(const std::filesystem::path& font);
	private:
		MSDFData* m_Data;
		Ref<Texture2D> m_AtlasTexture;
	};
}
