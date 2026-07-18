#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class ComputeShader;
    struct CloudscapeRenderState;

    /**
     * @brief Top-down cloud shadow map (issue #633).
     *
     * Owns a raw-GL R8 512² texture covering ShadowWorldSize meters of world
     * XZ centered on the (texel-snapped) camera, regenerated per frame by
     * CloudShadow_Generate.comp: each texel marches from its y = 0 ground
     * reference point toward the sun through the SAME density field the
     * cloud raymarch evaluates and stores the Beer-Lambert transmittance
     * (1 = unshadowed, 0 = fully occluded). Height parallax between a
     * surface's actual Y and the y = 0 reference plane is ignored (v1).
     *
     * Consumed by the PBR surface shaders' directional-light branch via
     * include/AtmosphereShading.glsl (sampler TEX_CLOUD_SHADOW = 62, bound
     * during mesh dispatch by CommandDispatch) with the map transform
     * delivered in the AtmosphereShadingUBO (binding 54).
     *
     * IMPORTANT — caller contract: CloudShadow_Generate.comp also reads the
     * CloudscapeData UBO (binding 52) and the cloud noise samplers
     * (u_CloudBaseNoise = 56, u_CloudDetailNoise = 57, u_CloudWeatherMap = 58).
     * The caller (RenderPipeline::UploadExecutionState) MUST upload/bind all
     * four BEFORE calling Update(). Because Update() creates GL resources and
     * dispatches compute work, it must only be called with a live GL context
     * on the render thread — the render pipeline is the intended caller;
     * there is no headless guard (same contract as CloudNoise::EnsureGenerated()).
     */
    class CloudShadowMap
    {
      public:
        static constexpr u32 kShadowResolution = 512;

        // Regenerate the shadow map for this frame. Lazily creates the R8
        // texture + compute shader on the first call; a creation failure is
        // latched (logged once, subsequent calls no-op cheaply) until
        // Shutdown() clears it. The map center is snapped to whole texels so
        // a moving camera doesn't swim the shadow pattern across surfaces.
        // @param state              This frame's cloudscape snapshot.
        // @param cameraPosAbsolute  ABSOLUTE world-space camera position
        //                           (Renderer3DData::ViewPos — not the
        //                           camera-relative UBO copy; the cloud field
        //                           lives in absolute world space, issue #429).
        static void Update(const CloudscapeRenderState& state, const glm::vec3& cameraPosAbsolute);

        /// Release the texture/shader (and clear the failure latch). Safe to call twice.
        static void Shutdown();

        /// @return true after the first successful Update() dispatch.
        [[nodiscard]] static bool IsReady();

        /// @return GL renderer id of the R8 512² shadow map (0 when not ready).
        [[nodiscard]] static u32 GetTextureID();

        /// @return world-XZ center of the current map (texel-snapped).
        [[nodiscard]] static glm::vec2 GetCenter();

        /// @return world meters covered by the full map.
        [[nodiscard]] static f32 GetWorldSize();

      private:
        struct CloudShadowMapData
        {
            Ref<ComputeShader> m_GenerateShader;
            u32 m_TextureID = 0; // raw GL R8 512², owned (RenderCommand::CreateTexture2D / DeleteTexture)
            glm::vec2 m_Center{ 0.0f, 0.0f };
            f32 m_WorldSize = 0.0f;
            bool m_Ready = false;
            bool m_CreationFailed = false;
        };

        static CloudShadowMapData s_Data;
    };
} // namespace OloEngine
