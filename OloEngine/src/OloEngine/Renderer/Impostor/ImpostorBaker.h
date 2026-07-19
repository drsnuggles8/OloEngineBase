#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class Mesh;
    class Texture2D;

    // Result of an octahedral impostor bake (issue #433). Two atlases sharing
    // one N*N tile grid: Albedo (rgb + coverage) and NormalDepth (object-space
    // normal + card-relative depth). Center/Radius are the object-space framing
    // used per tile; the runtime card shader needs Radius (and centre.y) to size
    // and place the billboard.
    struct ImpostorAtlas
    {
        Ref<Texture2D> Albedo;      // RGBA8: rgb = albedo, a = coverage
        Ref<Texture2D> NormalDepth; // RGBA8: rgb = obj normal *0.5+0.5, a = depth (0.5 = card plane)
        u32 FramesPerAxis = 0;      // N (N*N captured views)
        bool Hemi = true;           // hemi-octahedron vs full sphere
        f32 Radius = 1.0f;          // object-space bounding-sphere radius the tiles were framed to
        glm::vec3 Center{ 0.0f };   // object-space bounding-sphere centre

        [[nodiscard]] bool IsValid() const
        {
            return Albedo && NormalDepth && FramesPerAxis >= 2u;
        }
    };

    // Bakes a mesh into an octahedral impostor atlas by rendering it from N*N
    // view angles with an orthographic camera per tile. Requires a live GL 4.6
    // context (call from the render thread). Modelled on IBLPrecompute /
    // SkyCubemapBake.
    class ImpostorBaker
    {
      public:
        // albedoTexture may be null (bakes tint-only via a white fallback).
        // tint multiplies the sampled albedo. framesPerAxis and atlasResolution
        // are clamped to sane ranges. Returns an invalid ImpostorAtlas on failure.
        [[nodiscard]] static ImpostorAtlas Bake(
            const Ref<Mesh>& mesh,
            const Ref<Texture2D>& albedoTexture,
            const glm::vec3& tint,
            u32 framesPerAxis,
            u32 atlasResolution,
            bool hemi,
            f32 alphaCutoff);
    };
} // namespace OloEngine
