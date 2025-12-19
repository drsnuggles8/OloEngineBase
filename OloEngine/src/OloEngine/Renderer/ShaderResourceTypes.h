#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // @brief Enum defining all supported shader resource types
    enum class ShaderResourceType : u8
    {
        None = 0,
        UniformBuffer,
        StorageBuffer,
        Texture2D,
        TextureCube,
        Image2D,
        // Array resource types (Phase 1.2)
        UniformBufferArray,
        StorageBufferArray,
        Texture2DArray,
        TextureCubeArray
    };
}
