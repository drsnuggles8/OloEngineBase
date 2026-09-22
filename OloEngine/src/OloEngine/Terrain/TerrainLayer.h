#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include "OloEngine/Containers/String.h"

namespace OloEngine
{
    // Describes a single terrain material layer (grass, rock, sand, snow, etc.)
    // Each layer has PBR textures and blending parameters.
    struct TerrainLayer
    {
        // Texture paths (relative to asset directory)
        FString AlbedoPath; // Albedo/diffuse texture
        FString NormalPath; // Normal map
        FString ARMPath;    // AO(R) + Roughness(G) + Metallic(B) packed

        // Tiling and blending
        f32 TilingScale = 10.0f;         // UV tiling factor for this layer
        f32 HeightBlendSharpness = 4.0f; // Sharpness of height-based blending at transitions
        f32 TriplanarSharpness = 8.0f;   // Sharpness of triplanar projection (higher = sharper)

        // Default PBR values when no texture is assigned
        glm::vec3 BaseColor = glm::vec3(0.5f);
        f32 Roughness = 0.8f;
        f32 Metallic = 0.0f;

        // Layer name for editor display
        FString Name = "Unnamed";
    };

    // Four FStrings own external buffers; remaining members are scalar values and glm vectors.
    template<>
    struct TIsTriviallyRelocatable<TerrainLayer>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(TerrainLayer::AlbedoPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainLayer::NormalPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainLayer::ARMPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainLayer::TilingScale)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainLayer::HeightBlendSharpness)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainLayer::TriplanarSharpness)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainLayer::BaseColor)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainLayer::Roughness)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainLayer::Metallic)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TerrainLayer::Name)>::Value;
    };

    // Maximum layers supported by the splatmap system
    // 2 RGBA8 splatmaps × 4 channels = 8 layers
    static constexpr u32 MAX_TERRAIN_LAYERS = 8;

} // namespace OloEngine
