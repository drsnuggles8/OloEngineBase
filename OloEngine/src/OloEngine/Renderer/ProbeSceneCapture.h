#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

#include <functional>
#include <span>

namespace OloEngine
{
    class Scene;
    struct DDGIMeshCaster;

    // The lit scene as seen from one point, six faces: the capture both probe
    // bakers start from (ReflectionProbeBaker's environment, LightProbeBaker's
    // spherical harmonics).
    //
    // The scene renders through the renderer's own graph, which writes into its
    // OWN targets and never into a framebuffer the caller bound. So a capture
    // resizes the main view's graph to the probe's resolution, renders a face,
    // and reads the graph's HDR SceneColor back: linear radiance before tone
    // mapping and post. A baker that binds a framebuffer of its own and reads
    // that reads the clear colour; both bakers did at one time.
    //
    // A class of statics, not a namespace: Scene names it a friend for the
    // camera-and-transform render entry point the bakers use.
    class ProbeSceneCapture
    {
      public:
        static constexpr u32 kFaceCount = 6u;

        // Face order and orientation of a GL cube map (+X, -X, +Y, -Y, +Z, -Z),
        // the one IBLPrecompute::RenderToCubemap binds, so a captured face
        // uploads into the cube map face of the same index unchanged.
        [[nodiscard]] static glm::vec3 FaceTarget(u32 face);
        [[nodiscard]] static glm::vec3 FaceUp(u32 face);
        // Camera-to-world transform of a capture camera at `position` looking
        // along `face`.
        [[nodiscard]] static glm::mat4 FaceTransform(const glm::vec3& position, u32 face);

        // One face: `resolution` x `resolution` RGBA32F texels, rows BOTTOM-UP
        // on every backend (the OpenGL readback order the face orientations
        // above are chosen for; a Vulkan target stores its rows top-down and
        // the capture turns them over). `readOk` false means the readback
        // failed and `rgba` is unspecified. Return false to abandon the
        // remaining faces.
        using FaceSink = std::function<bool(u32 face, bool readOk, std::span<const f32> rgba)>;

        // Puts one face's rows into the order FaceSink promises, in place.
        // `rgba` is `resolution` rows of `resolution` RGBA texels as read back;
        // `rowsAreBottomUp` is how the target they were read from stores its
        // rows (RHI::RenderTargetRowsAreBottomUp()). Bottom-up input is left as
        // it is, top-down input has its rows reversed, and the texels within a
        // row never move. A span that is not `resolution` x `resolution` RGBA
        // is left untouched.
        static void NormalizeFaceRows(std::span<f32> rgba, u32 resolution, bool rowsAreBottomUp);

        // Renders the six faces through the MAIN view (ADR 0026, section 6) at
        // render scale 1 with no lighting tap, hands each to `onFace`, and puts
        // the main view's size, scale and tap back on every exit. `casterSink`, when given,
        // collects the scene's opaque mesh casters during the warm-up render
        // (Renderer3D::SetAuxCasterSink). Returns false when the scene is null,
        // the graph produced no SceneColor target, or `onFace` abandoned.
        // `resolution` is clamped to [16, 2048].
        [[nodiscard]] static bool CaptureFaces(Ref<Scene>& scene, const glm::vec3& position, u32 resolution,
                                               TArray<DDGIMeshCaster>* casterSink, const FaceSink& onFace);
    };
} // namespace OloEngine
