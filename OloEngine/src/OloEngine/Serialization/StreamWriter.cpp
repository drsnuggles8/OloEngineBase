#include "OloEnginePCH.h"
#include "StreamWriter.h"

#include <array>
#include <algorithm>
#include <type_traits>

namespace OloEngine
{
	void StreamWriter::WriteBuffer(Buffer buffer, bool writeSize)
	{
		// Verify Buffer::Size is the expected type for serialization consistency
		static_assert(std::is_same_v<decltype(buffer.Size), u64>, "Buffer::Size must be u64 for consistent serialization format");

		if (writeSize)
		{
			// Normalize buffer size to u64 for consistent serialization format
			u64 normalizedSize = static_cast<u64>(buffer.Size);
			WriteData(reinterpret_cast<const char*>(&normalizedSize), sizeof(u64));
		}

		// Assert data is non-null when size > 0
		if (buffer.Size > 0)
		{
			OLO_CORE_ASSERT(buffer.Data != nullptr, "Buffer data is null but size is {}", buffer.Size);
		}

		// Use normalized size for consistency (though Buffer::Size is already u64)
		u64 normalizedSize = static_cast<u64>(buffer.Size);
		WriteData(reinterpret_cast<const char*>(buffer.Data), normalizedSize);
	}

	void StreamWriter::WriteZero(u64 size)
	{
		constexpr u64 ChunkSize = 1024;
		static const std::array<char, ChunkSize> zeros{};
		
		u64 remaining = size;
		while (remaining > 0)
		{
			u64 chunk = std::min(remaining, ChunkSize);
			WriteData(zeros.data(), chunk);
			remaining -= chunk;
		}
	}

	void StreamWriter::WriteString(const std::string& string)
	{
		// Serialize string length as u64 in little-endian format (consistent with asset format)
		// This matches StreamReader::ReadString which expects a u64 length
		u64 size = static_cast<u64>(string.size());
		WriteData(reinterpret_cast<const char*>(&size), sizeof(size));
		
		// Write string data directly - string.data() already returns const char*
		WriteData(string.data(), static_cast<u64>(string.size()));
	}

} // namespace OloEngine
