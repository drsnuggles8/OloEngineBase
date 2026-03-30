#pragma once

#include "OloEngine/Core/Base.h"

#include <array>
#include <string_view>

namespace OloEngine::Audio
{
    /// Deterministic 32-bit identifier generated from a string event name via CRC32.
    /// Used as the key for looking up trigger commands in the AudioCommandRegistry.
    struct CommandID
    {
        u32 ID = 0;

        constexpr CommandID() = default;
        constexpr explicit CommandID(u32 id) : ID(id) {}

        /// CRC32 hash of the event name (case-insensitive, deterministic).
        [[nodiscard]] static CommandID FromString(std::string_view name);

        [[nodiscard]] constexpr bool IsValid() const
        {
            return ID != 0;
        }
        constexpr auto operator<=>(const CommandID&) const = default;
        constexpr auto operator==(const CommandID&) const -> bool = default;
    };

    namespace Detail
    {
        /// Compile-time CRC32 lookup table (IEEE 802.3 polynomial).
        consteval auto MakeCRC32Table()
        {
            std::array<u32, 256> table{};
            for (u32 i = 0; i < 256; ++i)
            {
                u32 crc = i;
                for (u32 j = 0; j < 8; ++j)
                {
                    crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
                }
                table[i] = crc;
            }
            return table;
        }

        inline constexpr auto s_CRC32Table = MakeCRC32Table();
    } // namespace Detail

} // namespace OloEngine::Audio
