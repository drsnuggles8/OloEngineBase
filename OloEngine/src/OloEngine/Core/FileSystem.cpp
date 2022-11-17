#include "OloEnginePCH.h"
#include "FileSystem.h"

namespace OloEngine {

	Buffer FileSystem::ReadFileBinary(const std::filesystem::path& filepath)
	{
		std::ifstream stream(filepath, std::ios::binary | std::ios::ate);

		if (!stream)
		{
			// Failed to open the file
			return {};
		}


		std::streampos end = stream.tellg();
		stream.seekg(0, std::ios::beg);
		auto size = static_cast<uint64_t>(end - stream.tellg());

		if (size == 0)
		{
			// File is empty
			return {};
		}

		Buffer buffer(size);
		stream.read(buffer.As<char>(), static_cast<std::streamsize>(size));
		stream.close();
		return buffer;
	}

}
