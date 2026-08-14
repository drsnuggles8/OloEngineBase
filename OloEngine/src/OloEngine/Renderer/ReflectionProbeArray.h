#pragma once

// =============================================================================
// Distance-impostor reflection probes (issue #705) — GPU array manager.
//
// Owns the two shared cubemap arrays every baked probe uploads into (layer i
// belongs to probe i of the frame's set):
//   - radiance:  RGBA32F prefilter chains (GPU-copied from each probe's
//                EnvironmentMap prefilter cubemap, roughness mips intact)
//   - distance:  R32F radial-distance fields + hand-built max-mips (uploaded
//                from the CPU ReflectionProbeDistanceField)
// plus the per-frame probe UBO (UBO_REFLECTION_PROBES), the per-cluster probe
// bitmask SSBO (SSBO_REFLECTION_PROBE_GRID) and the cull compute that fills
// it. Driven exactly like TiledForwardPlus: the Scene submits the frame's
// probe set during RenderScene3D, SceneRenderPass::Execute calls
// PrepareFrame + BindForShading, and DeferredLightingPass re-binds before its
// fullscreen draw.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/TextureCubemapArray.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class ComputeShader;
    class StorageBuffer;
    class UniformBuffer;

    // One active probe as gathered by the Scene (Scene::GatherReflectionProbes).
    struct ReflectionProbeRenderData
    {
        glm::vec3 Position{ 0.0f }; // ABSOLUTE world (the manager applies the render-origin shift)
        f32 InfluenceRadius = 10.0f;
        f32 BlendDistance = 1.0f;
        f32 Intensity = 1.0f;
        Ref<EnvironmentMap> Environment; // must HasIBL(); parallax additionally needs HasProbeDistanceField()
    };

    class ReflectionProbeArray
    {
      public:
        void Init();
        void Shutdown();

        // Per-frame probe set from the Scene (already camera-sorted; entries
        // beyond ReflectionProbeUBO::MAX_PROBES are dropped). Cleared by
        // PrepareFrame — a frame with no submission shades with zero probes.
        void SetProbes(std::vector<ReflectionProbeRenderData>&& probes);

        // Uploads new/changed layers, fills + uploads the probe UBO, and
        // dispatches the per-cluster cull. Call once per frame from
        // SceneRenderPass::Execute (GL thread), before the colour sub-pass.
        void PrepareFrame(const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                          u32 viewportWidth, u32 viewportHeight);

        // Publishes the arrays at TEX_REFLECTION_PROBE_* (offset + real bind,
        // so slot-based and bindless consumers both work) and binds the UBO +
        // grid SSBO. Callable repeatedly (DeferredLightingPass re-binds).
        void BindForShading() const;

        [[nodiscard]] bool HasProbes() const
        {
            return m_UploadedCount > 0;
        }
        [[nodiscard]] bool IsInitialized() const
        {
            return m_Initialized;
        }

      private:
        // `referencePrefilter` supplies the radiance array's face size /
        // format on (re)creation — the caller passes the first validated
        // probe's prefilter map (PrepareFrame has already consumed and
        // cleared m_Submitted by the time this runs).
        bool EnsureArrays(u32 requiredLayers, const Ref<TextureCubemap>& referencePrefilter);
        bool UploadLayer(u32 layer, const EnvironmentMap& environment);

        struct LayerSlot
        {
            Ref<EnvironmentMap> Environment; // null = free; identity keys the layer (a re-bake mints a new object)
        };

        std::vector<LayerSlot> m_Layers;
        Ref<TextureCubemapArray> m_RadianceArray;
        Ref<TextureCubemapArray> m_DistanceArray;
        // 1-layer stand-ins published while no probe is uploaded, so the
        // samplerCubeArray declarations never sit on an empty binding.
        Ref<TextureCubemapArray> m_PlaceholderRadiance;
        Ref<TextureCubemapArray> m_PlaceholderDistance;

        Ref<UniformBuffer> m_ProbeUBO;
        Ref<StorageBuffer> m_GridSSBO;
        Ref<ComputeShader> m_CullShader;
        // The cull dispatch's former bare uniforms (#691 Phase 8) — refilled
        // per dispatch, the HZB pattern.
        Ref<UniformBuffer> m_CullParamsUBO;

        std::vector<ReflectionProbeRenderData> m_Submitted;
        u32 m_UploadedCount = 0;
        bool m_GridValid = false;
        bool m_Initialized = false;
        bool m_WarnedPrefilterMismatch = false;
    };
} // namespace OloEngine
