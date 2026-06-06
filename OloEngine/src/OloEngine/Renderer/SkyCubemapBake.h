#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    class TextureCubemap;
    class Shader;
    class UniformBuffer;

    namespace SkyBake
    {
        // Bake a self-generating sky shader into all six faces of `cubemap`.
        //
        // The shader is expected to synthesise radiance purely from `skyUBO`
        // (no source cubemap is bound) using the per-face sample direction
        // supplied through the engine's standard CameraUBO (UBO_CAMERA): the
        // vertex stage draws a unit cube and forwards the local position as the
        // ray direction, exactly like Skybox.glsl. Mirrors the single-mip path
        // of IBLPrecompute::RenderToCubemap.
        //
        // Shared by ProceduralSky::Generate (Preetham analytic sky) and
        // StarNestSky::Generate (Star Nest raymarched nebula) — they differ only
        // in which shader + sky UBO contents they hand in. Returns false on
        // framebuffer / mesh allocation failure (the bake is a no-op then).
        //
        // The Refs are taken by value (a cheap refcount bump): Bind()/SetData()
        // are non-const and Ref<T> propagates const through operator->, so a
        // const-ref parameter would reject those calls.
        [[nodiscard]] bool RenderSkyToCubemap(Ref<TextureCubemap> cubemap,
                                              Ref<Shader> shader,
                                              Ref<UniformBuffer> cameraUBO,
                                              Ref<UniformBuffer> skyUBO);
    } // namespace SkyBake
} // namespace OloEngine
