#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>
#include <string>

namespace OloEngine
{
    // One foliage type within a foliage system (grass, flowers, bushes, trees, etc.)
    struct FoliageLayer
    {
        std::string Name = "Grass";

        // Mesh asset path (e.g. a quad or low-poly plant mesh)
        std::string MeshPath;

        // Albedo texture for the foliage (with alpha channel for cutout)
        std::string AlbedoPath;

        // Density / placement
        f32 Density = 1.0f;        // Instances per world unit squared
        i32 SplatmapChannel = -1;  // Splatmap channel to read density from (-1 = uniform)
        f32 MinSlopeAngle = 0.0f;  // Minimum slope angle (degrees) — 0 = flat
        f32 MaxSlopeAngle = 45.0f; // Maximum slope angle (degrees) — reject if steeper

        // Randomization
        f32 MinScale = 0.8f;
        f32 MaxScale = 1.2f;
        f32 MinHeight = 0.5f;       // Min instance height
        f32 MaxHeight = 1.5f;       // Max instance height
        bool RandomRotation = true; // Random Y-axis rotation

        // LOD distances
        f32 ViewDistance = 100.0f;     // Max view distance for this layer
        f32 FadeStartDistance = 80.0f; // Distance where fade-out begins

        // Wind
        f32 WindStrength = 0.3f; // Wind sway amplitude
        f32 WindSpeed = 1.0f;    // Wind animation speed

        // Rendering
        glm::vec3 BaseColor{ 0.3f, 0.5f, 0.1f }; // Tint color
        f32 Roughness = 0.8f;
        f32 AlphaCutoff = 0.5f; // Alpha test threshold

        // Runtime (not serialized)
        Ref<Texture2D> AlbedoTexture;

        bool Enabled = true;
    };

    // Per-instance data for GPU (must match shader layout)
    struct FoliageInstanceData
    {
        glm::vec4 PositionScale;  // xyz = world pos, w = uniform scale
        glm::vec4 RotationHeight; // x = Y-axis rotation (radians), y = height, z = fade, w = unused
        glm::vec4 ColorAlpha;     // rgb = tint color, a = alpha cutoff
    };
} // namespace OloEngine
