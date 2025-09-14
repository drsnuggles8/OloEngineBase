#include "OloEnginePCH.h"
#include "StreamReader.h"

#include <limits>

namespace OloEngine
{
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
			constexpr u64 MaxBufferSize = 1024ULL * 1024ULL * 1024ULL; // 1GB
			if (bufferSize > MaxBufferSize)
			{
				OLO_CORE_ERROR("Buffer size {} exceeds maximum allowed size of {} bytes", bufferSize, MaxBufferSize);
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
		constexpr u64 MaxStringSize = 256ULL * 1024ULL * 1024ULL; // 256MB
		if (size > MaxStringSize)
		{
			OLO_CORE_ERROR("String size {} exceeds maximum allowed size of {} bytes", size, MaxStringSize);
			return;
		}

		string.resize(static_cast<size_t>(size));
		ReadData(reinterpret_cast<char*>(string.data()), static_cast<size_t>(size));
	}

} // namespace OloEngine