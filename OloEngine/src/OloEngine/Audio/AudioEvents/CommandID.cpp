#include "OloEnginePCH.h"
#include "OloEngine/Audio/AudioEvents/CommandID.h"

namespace OloEngine::Audio
{
    CommandID CommandID::FromString(std::string_view name)
    {
        if (name.empty())
        {
            return CommandID{};
        }

        u32 crc = 0xFFFFFFFF;
        for (const char c : name)
        {
            // Case-insensitive: fold to lower before hashing
            const auto byte = static_cast<u8>(c >= 'A' && c <= 'Z' ? c + 32 : c);
            crc = Detail::s_CRC32Table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
        }
        return CommandID{ crc ^ 0xFFFFFFFF };
    }

} // namespace OloEngine::Audio
