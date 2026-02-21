#include "OloEnginePCH.h"
#include "OloEngine/Core/FileSystem.h"

namespace OloEngine
{
    Buffer FileSystem::ReadFileBinary(const std::filesystem::path& filepath)
    {
        std::ifstream stream(filepath, std::ios::binary | std::ios::ate);

        if (!stream)
        {
            return {};
        }

        std::streampos end = stream.tellg();
        stream.seekg(0, std::ios::beg);
        auto size = static_cast<u64>(end - stream.tellg());

        if (size == 0)
        {
            return {};
        }

        Buffer buffer(size);
        stream.read(buffer.As<char>(), static_cast<std::streamsize>(size));
        stream.close();
        return buffer;
    }

    std::string FileSystem::ReadFileText(const std::filesystem::path& filepath)
    {
        std::string result;
        std::ifstream in(filepath, std::ios::in | std::ios::binary);
        if (!in)
        {
            OLO_CORE_ERROR("Could not open file '{0}'", filepath.string());
            return result;
        }

        in.seekg(0, std::ios::end);
        const auto size = in.tellg();
        if (size != std::streampos(-1))
        {
            result.resize(static_cast<sizet>(size));
            in.seekg(0, std::ios::beg);
            in.read(result.data(), size);
        }
        else
        {
            OLO_CORE_ERROR("Could not read file '{0}'", filepath.string());
        }
        return result;
    }

} // namespace OloEngine
