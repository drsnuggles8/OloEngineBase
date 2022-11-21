#include "OloEnginePCH.h"
#include "OloEngine/Core/FileSystem.h"

namespace OloEngine {

	Buffer FileSystem::ReadFileBinary(const std::filesystem::path& filepath)
	{
		std::ifstream stream(filepath, std::ios::binary | std::ios::ate);

		if (!stream)
		{
			return {};
		}

		std::streampos end = stream.tellg();
		stream.seekg(0, std::ios::beg);
		auto size = static_cast<uint64_t>(end - stream.tellg());

		if (size == 0)
		{
			return {};
		}

		Buffer buffer(size);
		stream.read(buffer.As<char>(), static_cast<std::streamsize>(size));
		stream.close();
		return buffer;
	}

}
