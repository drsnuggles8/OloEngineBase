#include "OloEnginePCH.h"
#include "StreamReader.h"

namespace OloEngine
{
	void StreamReader::ReadBuffer(Buffer& buffer, u32 size)
	{
		buffer.Size = size;
		if (size == 0)
			ReadData((char*)&buffer.Size, sizeof(u64));

		buffer.Allocate(buffer.Size);
		ReadData((char*)buffer.Data, buffer.Size);
	}

	void StreamReader::ReadString(std::string& string)
	{
		sizet size;
		ReadData((char*)&size, sizeof(sizet));

		string.resize(size);
		ReadData((char*)string.data(), sizeof(char) * size);
	}

} // namespace OloEngine