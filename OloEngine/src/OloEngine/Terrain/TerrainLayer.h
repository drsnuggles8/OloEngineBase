#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <string>

namespace OloEngine
{
    // Describes a single terrain material layer (grass, rock, sand, snow, etc.)
    // Each layer has PBR textures and blending parameters.
    struct TerrainLayer
    {
        // Texture paths (relative to asset directory)
        std::string AlbedoPath;        // Albedo/diffuse texture
        std::string NormalPath;        // Normal map
        std::string ARMPath;           // AO(R) + Roughness(G) + Metallic(B) packed

        // Tiling and blending
        f32 TilingScale = 10.0f;       // UV tiling factor for this layer
        f32 HeightBlendSharpness = 4.0f; // Sharpness of height-based blending at transitions
        f32 TriplanarSharpness = 8.0f;   // Sharpness of triplanar projection (higher = sharper)

        // Default PBR values when no texture is assigned
        glm::vec3 BaseColor = glm::vec3(0.5f);
        f32 Roughness = 0.8f;
        f32 Metallic = 0.0f;

        // Layer name for editor display
        std::string Name = "Unnamed";
    };

    // Maximum layers supported by the splatmap system
    // 2 RGBA8 splatmaps × 4 channels = 8 layers
    static constexpr u32 MAX_TERRAIN_LAYERS = 8;

} // namespace OloEngine
