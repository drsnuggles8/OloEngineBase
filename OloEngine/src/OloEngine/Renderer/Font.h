#pragma once

#include <filesystem>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"

namespace OloEngine
{
	struct MSDFData;

	class Font
	{
	public:
		Font(const std::filesystem::path& font);
		~Font();

		Ref<Texture2D> GetAtlasTexture() const { return m_AtlasTexture; }
	private:
		MSDFData* m_Data;
		Ref<Texture2D> m_AtlasTexture;

	};
}
