#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>
#include <span>

namespace OloEngine
{
    class Scene;
    class TextureCubemap;
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
        static Ref<TextureCubemap> CaptureSceneCubemap(Ref<Scene>& scene,
                                                       const glm::vec3& position,
                                                       u32 resolution);
    };
} // namespace OloEngine
