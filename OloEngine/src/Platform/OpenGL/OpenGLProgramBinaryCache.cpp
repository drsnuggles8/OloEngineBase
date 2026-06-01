#include "OloEnginePCH.h"

#include "Platform/OpenGL/OpenGLProgramBinaryCache.h"

#include <cstdint>
#include <istream>
#include <limits>
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

        // dataSize >= 0 is already guaranteed (size >= sizeof(u32) was checked above). Before
        // narrowing it to the buffer's size type and to read()'s std::streamsize, reject a file
        // large enough that either conversion would overflow — a corrupt/oversized cache must
        // fail cleanly rather than truncate the size or over-allocate.
        if (static_cast<std::uintmax_t>(dataSize) > static_cast<std::uintmax_t>(std::numeric_limits<sizet>::max()) ||
            dataSize > static_cast<std::streamoff>(std::numeric_limits<std::streamsize>::max()))
        {
            return std::nullopt;
        }

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
        // A non-empty payload with no buffer is a caller error; bail before writing anything so
        // we never emit a header that promises bytes the file doesn't contain.
        if (dataSize > 0 && data == nullptr)
        {
            return false;
        }

        out.write(reinterpret_cast<const char*>(&format), sizeof(u32));
        if (dataSize > 0)
        {
            out.write(data, static_cast<std::streamsize>(dataSize));
        }
        return static_cast<bool>(out);
    }
} // namespace OloEngine
