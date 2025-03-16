#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"

#include <string>

namespace OloEngine
{
	
	class TextureCubemap : public Texture
	{
	public:
		static Ref<TextureCubemap> Create(const TextureSpecification& specification);
		
		// Creates a cubemap from 6 individual texture files
		static Ref<TextureCubemap> Create(const std::array<std::string, 6>& facePaths);
		
		// Creates a cubemap from a single directory containing the 6 faces with standardized names
		static Ref<TextureCubemap> Create(const std::string& folderPath);
	};
}
