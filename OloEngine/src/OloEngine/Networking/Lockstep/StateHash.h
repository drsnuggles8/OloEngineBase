#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    // Computes a hash of serialized snapshot data for desync detection in lockstep mode.
    class StateHash
    {
      public:
        // Compute a CRC32 hash of the given data buffer.
        [[nodiscard]] static u32 Compute(const u8* data, u32 size);
        [[nodiscard]] static u32 Compute(const std::vector<u8>& data);
    };
} // namespace OloEngine
