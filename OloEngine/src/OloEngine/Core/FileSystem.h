#pragma once

#include "OloEngine/Core/Buffer.h"

#include <filesystem>

namespace OloEngine {

	class FileSystem
	{
	public:
		// TODO: move to FileSystem class
		static Buffer ReadFileBinary(const std::filesystem::path& filepath);
	};

}
