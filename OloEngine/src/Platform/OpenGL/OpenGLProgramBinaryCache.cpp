#include "OloEnginePCH.h"

#include "Platform/OpenGL/OpenGLProgramBinaryCache.h"

#include <istream>
#include <ostream>

namespace OloEngine
{
    std::optional<ProgramBinary> ReadProgramBinary(std::istream& in)
    {
        // Determine total size from the stream itself rather than trusting a
        // caller-supplied length — that decoupling is exactly what the buggy
        // AMD path got wrong.
        in.clear();
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < static_cast<std::streamoff>(sizeof(u32)))
        {
            // Too small to even hold the format header.
            return std::nullopt;
        }
        in.seekg(0, std::ios::beg);

        u32 format = 0;
        in.read(reinterpret_cast<char*>(&format), sizeof(u32));
        if (!in || in.gcount() != static_cast<std::streamsize>(sizeof(u32)))
        {
            return std::nullopt;
        }

        const std::streamoff dataSize = size - static_cast<std::streamoff>(sizeof(u32));

        ProgramBinary result;
        result.Format = format;
        result.Data.resize(static_cast<sizet>(dataSize));
        if (dataSize > 0)
        {
            in.read(result.Data.data(), dataSize);
            if (!in || in.gcount() != static_cast<std::streamsize>(dataSize))
            {
                return std::nullopt;
            }
        }
        return result;
    }

    bool WriteProgramBinary(std::ostream& out, u32 format, const char* data, sizet dataSize)
    {
        out.write(reinterpret_cast<const char*>(&format), sizeof(u32));
        if (dataSize > 0 && data != nullptr)
        {
            out.write(data, static_cast<std::streamsize>(dataSize));
        }
        return static_cast<bool>(out);
    }
} // namespace OloEngine
