#include "OloEnginePCH.h"
#include "StreamWriter.h"

namespace OloEngine
{
	void StreamWriter::WriteBuffer(Buffer buffer, bool writeSize)
	{
		if (writeSize)
			WriteData((char*)&buffer.Size, sizeof(u64));

		WriteData((char*)buffer.Data, buffer.Size);
	}

	void StreamWriter::WriteZero(u64 size)
	{
		char zero = 0;
		for (u64 i = 0; i < size; i++)
			WriteData(&zero, 1);
	}

	void StreamWriter::WriteString(const std::string& string)
	{
		sizet size = string.size();
		WriteData((char*)&size, sizeof(sizet));
		WriteData((char*)string.data(), sizeof(char) * string.size());
	}

} // namespace OloEngine