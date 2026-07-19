#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Math/Math.h"
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

        // Octahedral impostor LOD (issue #433) — bakes MeshPath into a view-angle
        // atlas and swaps the flat billboard for a camera-facing impostor card
        // beyond ImpostorStartDistance, cross-fading so distant trees/bushes read
        // as 3D from any azimuth instead of a flat card. Requires a valid MeshPath.
        bool UseImpostor = false;           // Enable octahedral impostor rendering for this layer
        f32 ImpostorStartDistance = 40.0f;  // Distance where the billboard cross-fades into the impostor card
        f32 ImpostorTransitionBand = 15.0f; // Cross-fade band width (world units) above the start distance
        u32 ImpostorFramesPerAxis = 8;      // Octahedral atlas grid: N frames per axis (N*N captured views)
        u32 ImpostorAtlasResolution = 1024; // Total atlas texture resolution per axis (tile res = this / N)
        bool ImpostorHemiOctahedral = true; // Hemi-octahedron (upper hemisphere) vs full sphere layout

        // Runtime (not serialized)
        Ref<Texture2D> AlbedoTexture;

        bool Enabled = true;

        // Manual operator== — excludes the runtime AlbedoTexture (a re-load of
        // the same asset hands out a different Ref pointer, which would
        // spuriously flag layers as changed). Float / glm::vec3 fields use
        // Math::BitwiseEqual per cpp-coding-quality §2a; AlbedoPath identifies
        // the texture authoritatively for equality purposes.
        auto operator==(const FoliageLayer& other) const -> bool
        {
            return Name == other.Name && MeshPath == other.MeshPath && AlbedoPath == other.AlbedoPath && Math::BitwiseEqual(Density, other.Density) && SplatmapChannel == other.SplatmapChannel && Math::BitwiseEqual(MinSlopeAngle, other.MinSlopeAngle) && Math::BitwiseEqual(MaxSlopeAngle, other.MaxSlopeAngle) && Math::BitwiseEqual(MinScale, other.MinScale) && Math::BitwiseEqual(MaxScale, other.MaxScale) && Math::BitwiseEqual(MinHeight, other.MinHeight) && Math::BitwiseEqual(MaxHeight, other.MaxHeight) && RandomRotation == other.RandomRotation && Math::BitwiseEqual(ViewDistance, other.ViewDistance) && Math::BitwiseEqual(FadeStartDistance, other.FadeStartDistance) && Math::BitwiseEqual(WindStrength, other.WindStrength) && Math::BitwiseEqual(WindSpeed, other.WindSpeed) && Math::BitwiseEqual(BaseColor, other.BaseColor) && Math::BitwiseEqual(Roughness, other.Roughness) && Math::BitwiseEqual(AlphaCutoff, other.AlphaCutoff) && UseImpostor == other.UseImpostor && Math::BitwiseEqual(ImpostorStartDistance, other.ImpostorStartDistance) && Math::BitwiseEqual(ImpostorTransitionBand, other.ImpostorTransitionBand) && ImpostorFramesPerAxis == other.ImpostorFramesPerAxis && ImpostorAtlasResolution == other.ImpostorAtlasResolution && ImpostorHemiOctahedral == other.ImpostorHemiOctahedral && Enabled == other.Enabled;
        }
    };

    // Per-instance data for GPU (must match shader layout)
    struct FoliageInstanceData
    {
        glm::vec4 PositionScale;  // xyz = world pos, w = uniform scale
        glm::vec4 RotationHeight; // x = Y-axis rotation (radians), y = height, z = fade, w = unused
        glm::vec4 ColorAlpha;     // rgb = tint color, a = alpha cutoff
    };
} // namespace OloEngine
