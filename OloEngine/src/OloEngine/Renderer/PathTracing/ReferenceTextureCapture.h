#pragma once

// =============================================================================
// ReferenceTextureCapture.h — live GPU textures -> the reference tracer's own
// images (issue #869, ADR 0022)
//
// The reference path tracer models textures and a sky cubemap, but
// `ReferenceScene` and `ReferenceSceneBuilder` are GL-free by contract: they
// must run in a headless unit test with no Application, no asset manager and
// no GPU. Reading a `Texture2D`'s texels, on the other hand, is a readback.
//
// This file is where those two facts meet. It is the ONLY piece of the
// reference tracer that needs a live graphics device, it is called once per
// bake at capture time (never per ray), and everything it produces is a plain
// CPU buffer the integrator can then trace without touching the GPU again.
//
// WHAT IT IS NOT. It does not decide the sampling convention — `ReferenceTexture`
// and `ReferenceEnvironmentCubemap` own that (level 0, bilinear, REPEAT for
// materials; level 0, GL face selection, clamped bilinear for the sky). This
// file only moves bytes into those, decoding whatever pixel format the texture
// happens to carry.
//
// EVERY FAILURE IS COUNTED, NEVER SILENT. A texture that cannot be read back —
// no device, a compressed format, a readback failure — leaves its slot empty,
// which traces as "factor only". That is a real fidelity change and it is
// exactly the kind of thing that reads as a transport bug three weeks later, so
// `CaptureStats` tallies it and the callers log it.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"

#include <map>
#include <memory>

namespace OloEngine
{
    class Material;
    class Scene;
    class Texture2D;
    class TextureCubemap;
} // namespace OloEngine

namespace OloEngine::PathTracing
{
    // Decodes live textures once each and hands the results to a
    // `ReferenceSceneBuilder`. Caches by texture pointer, so one image shared
    // by twenty materials is read back once.
    //
    // THREADING / LIFETIME: construct, use and destroy on the thread that owns
    // the graphics context, before the bake's background stage starts. The
    // `ReferenceScene` it fills holds `shared_ptr`s to the decoded images, so
    // the captor itself does not need to outlive the bake.
    class ReferenceTextureCaptor
    {
      public:
        struct CaptureStats
        {
            // Distinct images successfully decoded.
            u32 Captured = 0;
            // Texture slots a material referenced that could not be read back.
            // Those slots trace factor-only; see the header's note on why this
            // is counted rather than shrugged off.
            u32 Failed = 0;
        };

        // Reads back one Texture2D into the reference's own image, or null if
        // it cannot be read. Cached.
        [[nodiscard]] std::shared_ptr<const ReferenceTexture> Capture(const Ref<Texture2D>& texture);

        // A provider for `ReferenceSceneBuildOptions::MaterialMapProvider`,
        // bound to this captor. The captor must outlive the `Build()` call.
        [[nodiscard]] ReferenceMaterialMapProvider MakeMaterialMapProvider();

        [[nodiscard]] const CaptureStats& GetStats() const
        {
            return m_Stats;
        }

      private:
        std::map<const Texture2D*, std::shared_ptr<const ReferenceTexture>> m_Cache;
        CaptureStats m_Stats;
    };

    // Reads back a sky cubemap as radiance. Returns null when the cubemap is
    // null, unreadable, or in a format this cannot decode — never a black cube,
    // which would trace as "there is a sky and it is dark" rather than "there
    // is no sky".
    [[nodiscard]] std::shared_ptr<const ReferenceEnvironmentCubemap>
    CaptureEnvironmentCubemap(const Ref<TextureCubemap>& cubemap);

    // The scene's active sky, resolved with the SAME precedence
    // `Scene::LoadAndRenderSkybox` applies (StarNestSky > ProceduralSky >
    // EnvironmentMap) and gated on the same `m_EnableIBL` flag — a sky the lit
    // passes do not take light from must not light the bake either.
    //
    // Returns null (leaving `outIntensity` untouched) when the scene has no
    // sky, when IBL is disabled on it, or when its cubemap has not been baked
    // yet. Reads the CACHED environment map: it does not generate one, so call
    // it from the editor after at least one frame has run.
    struct CapturedSky
    {
        std::shared_ptr<const ReferenceEnvironmentCubemap> Cubemap;
        f32 Intensity = 1.0f;
    };
    [[nodiscard]] CapturedSky CaptureSceneSky(Scene& scene);
} // namespace OloEngine::PathTracing
