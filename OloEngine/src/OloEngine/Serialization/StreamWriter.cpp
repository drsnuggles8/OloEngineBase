#include "OloEnginePCH.h"
#include "StreamWriter.h"

#include <array>

namespace OloEngine
{
	void StreamWriter::WriteBuffer(Buffer buffer, bool writeSize)
	{
		if (writeSize)
			WriteData(reinterpret_cast<const char*>(&buffer.Size), sizeof(u64));

		WriteData(reinterpret_cast<const char*>(buffer.Data), buffer.Size);
	}

	void StreamWriter::WriteZero(u64 size)
	{
		constexpr sizet ChunkSize = 1024;
		std::array<char, ChunkSize> zeros{};
		
		u64 remaining = size;
		while (remaining > 0)
		{
			sizet chunk = std::min(remaining, static_cast<u64>(ChunkSize));
			WriteData(zeros.data(), chunk);
			remaining -= chunk;
		}
	}

	void StreamWriter::WriteString(const std::string& string)
	{
		u64 size = string.size();
		WriteData(reinterpret_cast<const char*>(&size), sizeof(u64));
		WriteData(reinterpret_cast<const char*>(string.data()), string.size());
	}

} // namespace OloEngine