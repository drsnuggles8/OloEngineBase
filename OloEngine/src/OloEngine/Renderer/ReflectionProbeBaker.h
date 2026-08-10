#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>
#include <span>
#include <vector>

namespace OloEngine
{
    class Scene;
    class TextureCubemap;
    class ReflectionProbeDistanceField;
    struct DDGIMeshCaster;
    struct ReflectionProbeComponent;

    // Geometry-only probe description for the dominant-probe lookup.
    // Decoupled from ReflectionProbeComponent so the selection function can be
    // unit-tested without a scene or render context.
    struct ReflectionProbeRef
    {
        glm::vec3 Position;
        f32 InfluenceRadius;
    };

    // Returns the index of the probe whose influence sphere contains
    // `cameraPosition` and whose center is closest to it. Returns -1 if no
    // probe applies. Pure / testable; identical math to the runtime selection
    // path in Scene::ApplyReflectionProbeOverride.
    [[nodiscard]] i32 SelectDominantReflectionProbe(const glm::vec3& cameraPosition,
                                                    std::span<const ReflectionProbeRef> probes);

    // Bakes a local reflection probe by rendering the scene from a probe
    // position into a cubemap, then generating the irradiance / prefilter /
    // BRDF LUT chain via EnvironmentMap::CreateFromCubemap. The result is
    // attached to probe.m_BakedEnvironment.
    //
    // Editor-driven; runs synchronously and assumes a live GL context (the
    // editor "Bake" button on the inspector panel).
    class ReflectionProbeBaker
    {
      public:
        // Render scene at `position` into a fresh cubemap + IBL chain and
        // store the result on the component. Clears `m_NeedsBake` on success.
        // Returns true if the bake produced a usable EnvironmentMap.
        static bool BakeProbe(Ref<Scene>& scene,
                              const glm::vec3& position,
                              ReflectionProbeComponent& probe);

      private:
        // Friend of Scene (via Scene.h's friend list) so this can call the
        // otherwise-private RenderScene3D. Kept as a class member rather than
        // a free function for exactly that reason.
        //
        // `casterSink` (optional): collects the scene's opaque mesh casters
        // during the warm-up render via Renderer3D::SetAuxCasterSink, so the
        // distance capture below can re-rasterize the same geometry without
        // a second scene traversal.
        static Ref<TextureCubemap> CaptureSceneCubemap(Ref<Scene>& scene,
                                                       const glm::vec3& position,
                                                       u32 resolution,
                                                       std::vector<DDGIMeshCaster>* casterSink = nullptr);

        // Rasterizes `casters` into a kProbeDistanceResolution RG32F cube-face
        // target around `position` with ReflectionProbe_Distance.glsl (six
        // faces, own FBO — the DDGIProbeUpdatePass::CaptureProbe shape), reads
        // each face back and builds the CPU distance field (max-mips + dMax).
        // Encoding contract: ReflectionProbeDistanceField.h. Returns nullptr
        // when the capture cannot run (no GL context resources).
        static Ref<ReflectionProbeDistanceField> CaptureDistanceField(const std::vector<DDGIMeshCaster>& casters,
                                                                      const glm::vec3& position);
    };
} // namespace OloEngine
