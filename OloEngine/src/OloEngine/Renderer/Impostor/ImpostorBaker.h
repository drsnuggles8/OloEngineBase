#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/AtlasAllocator.h"

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

        // Shared atlas-allocator budget claim (issue #718). Every bake must
        // reserve a square region of a process-wide OloEngine::AtlasAllocator
        // sized to (a power-of-two rounding of) its own atlas footprint before
        // ImpostorBaker::Bake proceeds — this is what bounds TOTAL impostor
        // VRAM across every foliage layer, which nothing did before (any
        // number of layers could each demand a full 4096x4096 pair with no
        // shared ceiling). Deliberately NOT spatial packing: Albedo/
        // NormalDepth stay dedicated per-bake textures rather than sub-rects
        // of one shared texture — see ImpostorBaker.cpp for why. Opaque to
        // callers; owner must call ImpostorBaker::Free(atlas) before letting
        // an ImpostorAtlas go (rebake or teardown) or the budget leaks.
        u32 BudgetNode = OloEngine::AtlasAllocator::kInvalidNode;

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
        // Release the lazily-created 1x1 white fallback texture. Must run
        // before the graphics context dies — a static Ref surviving into
        // vmaDestroyAllocator is the Vulkan close-crash shape (#691).
        static void Shutdown();

        // albedoTexture may be null (bakes tint-only via a white fallback).
        // tint multiplies the sampled albedo. framesPerAxis and atlasResolution
        // are clamped to sane ranges. Returns an invalid ImpostorAtlas when the
        // mesh/shader are unusable OR the shared VRAM budget (issue #718) has
        // no room left for this bake's footprint.
        [[nodiscard]] static ImpostorAtlas Bake(
            const Ref<Mesh>& mesh,
            const Ref<Texture2D>& albedoTexture,
            const glm::vec3& tint,
            u32 framesPerAxis,
            u32 atlasResolution,
            bool hemi,
            f32 alphaCutoff);

        // Releases atlas's claim on the shared VRAM budget (issue #718) and
        // clears BudgetNode. Idempotent (safe on an already-freed or
        // never-baked atlas). Callers must call this before dropping/
        // overwriting an ImpostorAtlas — a rebake or teardown that skips it
        // leaks the budget claim, eventually starving every future bake.
        static void Free(ImpostorAtlas& atlas);
    };
} // namespace OloEngine
