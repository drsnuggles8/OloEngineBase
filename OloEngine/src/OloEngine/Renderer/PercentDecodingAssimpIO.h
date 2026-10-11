#pragma once

#include <assimp/DefaultIOSystem.h>

#include <cctype>
#include <string>

namespace OloEngine
{
    // glTF names its external files by URI, so a buffer called "editable
    // buffer.bin" is written "editable%20buffer.bin". Assimp opens that URI as
    // a literal path and fails ("could not open referenced file"), so a valid
    // glTF whose buffers or images have a space -- or any other escaped byte --
    // in their names did not import. This IO system tries the literal path
    // first, so a file whose name really contains '%' still opens, and then the
    // percent-decoded one. MeshCache fingerprints the same buffers through the
    // same decoding.
    class PercentDecodingAssimpIO final : public Assimp::IOSystem
    {
      public:
        // The std::string overloads, not hidden by the overrides below.
        using Assimp::IOSystem::Exists;
        using Assimp::IOSystem::Open;

        bool Exists(const char* path) const override
        {
            if (m_System.Exists(path))
                return true;
            const std::string decoded = Decode(path);
            return decoded != path && m_System.Exists(decoded.c_str());
        }

        char getOsSeparator() const override
        {
            return m_System.getOsSeparator();
        }

        Assimp::IOStream* Open(const char* path, const char* mode = "rb") override
        {
            if (m_System.Exists(path))
                return m_System.Open(path, mode);
            const std::string decoded = Decode(path);
            return m_System.Open(decoded.c_str(), mode);
        }

        void Close(Assimp::IOStream* stream) override
        {
            m_System.Close(stream);
        }

        bool ComparePaths(const char* one, const char* two) const override
        {
            return m_System.ComparePaths(one, two);
        }

        [[nodiscard]] static std::string Decode(const char* path)
        {
            const std::string in = path ? path : "";
            std::string out;
            out.reserve(in.size());
            for (std::size_t i = 0; i < in.size(); ++i)
            {
                if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(in[i + 2])))
                {
                    out += static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else
                {
                    out += in[i];
                }
            }
            return out;
        }

      private:
        // DefaultIOSystem is final, so it is wrapped rather than derived from.
        Assimp::DefaultIOSystem m_System;
    };
} // namespace OloEngine
