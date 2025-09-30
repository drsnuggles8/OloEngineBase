#include "OloEnginePCH.h"
#include "StreamReader.h"

#include <limits>
#include <span>

namespace OloEngine
{
	constexpr sizet OLO_MAX_BUFFER_SIZE = 1024ull * 1024ull * 1024ull; // 1GB
	constexpr sizet OLO_MAX_STRING_SIZE = 256ull * 1024ull * 1024ull;  // 256MB
	bool StreamReader::ReadData(std::span<std::byte> destination)
	{
		// Convert std::span<std::byte> to char* and size for the virtual method
		return ReadData(reinterpret_cast<char*>(destination.data()), destination.size());
	}

	void StreamReader::ReadBuffer(Buffer& buffer, u32 size)
	{
		u64 bufferSize = size;
		
		if (size == 0)
		{
			// Read the size from stream into local variable
			ReadData(reinterpret_cast<char*>(&bufferSize), sizeof(u64));
			
			// Validate size fits into size_t
			if (bufferSize > std::numeric_limits<size_t>::max())
			{
				OLO_CORE_ERROR("Buffer size {} exceeds maximum size_t value", bufferSize);
				return;
			}
			
			// Check for reasonable maximum (1GB limit to prevent excessive allocations)
			if (bufferSize > OLO_MAX_BUFFER_SIZE)
			{
				OLO_CORE_ERROR("Buffer size {} exceeds maximum allowed size of {} bytes", bufferSize, OLO_MAX_BUFFER_SIZE);
				return;
			}
		}
		
		// Set validated size and allocate
		buffer.Size = static_cast<size_t>(bufferSize);
		buffer.Allocate(buffer.Size);
		ReadData(reinterpret_cast<char*>(buffer.Data), buffer.Size);
	}

	void StreamReader::ReadString(std::string& string)
	{
		u64 size;
		ReadData(reinterpret_cast<char*>(&size), sizeof(u64));

		// Validate size fits into size_t
		if (size > std::numeric_limits<size_t>::max())
		{
			OLO_CORE_ERROR("String size {} exceeds maximum size_t value", size);
			return;
		}
		
		// Check for reasonable maximum (256MB limit for strings)
		if (size > OLO_MAX_STRING_SIZE)
		{
			OLO_CORE_ERROR("String size {} exceeds maximum allowed size of {} bytes", size, OLO_MAX_STRING_SIZE);
			return;
		}

		string.resize(static_cast<size_t>(size));
		
		// Use safe ReadData overload with std::span to avoid reinterpret_cast
		auto stringBytes = std::as_writable_bytes(std::span<char>(string.data(), string.size()));
		ReadData(stringBytes);
	}

} // namespace OloEngine