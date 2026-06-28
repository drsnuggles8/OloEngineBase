#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class SceneRenderPass;

    // @brief Planar reflection pass (mirror / water).
    //
    // Renders a true planar mirror reflection of the opaque scene for a water /
    // mirror surface. Inserted into the scene+lighting stage AFTER ScenePass
    // (so the opaque command bucket is already batched) and BEFORE WaterPass
    // (so the water material can sample the result):
    //   ShadowPass → ScenePass → PlanarReflectionPass → … → WaterPass
    //
    // How it works (forward / forward+ path only — see SetEnabled gating):
    //   1. ScenePass batches the opaque draw bucket and renders the main view.
    //   2. This pass swaps the shared CameraUBO to a MIRRORED camera — the real
    //      camera reflected across the reflection plane, with a Lengyel oblique
    //      near-clip plane coincident with the surface so geometry on the far
    //      side of the plane cannot leak in — flips front-face winding (a
    //      reflection reverses handedness), and RE-EXECUTES ScenePass's already
    //      batched bucket into an owned RGBA16F+depth target. Re-using the bucket
    //      means the reflection is shaded by the exact same PBR / lighting /
    //      shadow / IBL path as the main view, for free.
    //   3. It restores the real camera + winding, publishes the reflection color
    //      texture id (Renderer3D::SetPlanarReflectionTextureID) and uploads the
    //      PlanarReflectionUBO (binding 43: mirror view-projection + enable /
    //      intensity / distortion) that Water.glsl reads to sample the texture
    //      projectively.
    //
    // Single-plane: one global reflection plane per frame (the dominant water
    // surface), supplied each frame by Scene.cpp via Renderer3D. Multiple water
    // surfaces at different heights share that one plane's reflection — a
    // documented first-slice limitation (see docs/agent-rules/glsl-shaders.md).
    //
    // Deferred path: the opaque bucket writes a G-Buffer, not lit colour, so a
    // single-target replay would capture albedo. The pass disables itself when
    // the active path is Deferred; water falls back to its cubemap / SSR
    // reflection. (A deferred planar reflection would need its own mini lighting
    // resolve — a future slice.)
    class PlanarReflectionRenderPass : public RenderGraphNode
    {
      public:
        // std140 mirror of the PlanarReflectionParams UBO in Water.glsl
        // (binding = UBO_PLANAR_REFLECTION). Kept in lockstep with the GLSL
        // block; the size is asserted in ShaderBindingLayoutTest.
        struct UBOData
        {
            glm::mat4 ViewProjection{ 1.0f }; ///< mirrored, oblique-clipped camera VP (world → reflection clip)
            glm::vec4 Params{ 0.0f };         ///< x = enabled (0/1), y = intensity, z = distortion, w = unused

            static constexpr u32 GetSize()
            {
                return sizeof(UBOData);
            }
        };

        PlanarReflectionRenderPass();
        ~PlanarReflectionRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }

        // Per-frame reflection state, pushed from Scene.cpp through Renderer3D.
        // `plane` is the world-space reflection plane vec4(n.xyz, d) with the
        // normal pointing toward the kept (above-water) half-space. `enabled`
        // is the combined gate (a reflective water surface exists AND the path
        // supports it). Intensity / distortion are artist controls forwarded to
        // the water shader through the UBO.
        void SetReflectionState(const glm::vec4& plane, bool enabled, f32 intensity, f32 distortion) noexcept
        {
            m_ReflectionPlane = plane;
            m_Enabled = enabled;
            m_Intensity = intensity;
            m_Distortion = distortion;
        }

        // The ScenePass whose batched opaque bucket is replayed. Wired once at
        // pipeline construction.
        void SetScenePass(SceneRenderPass* scenePass) noexcept
        {
            m_ScenePass = scenePass;
        }

        // UBO the water pass rebinds before its draw (binding 43). Null until Init.
        [[nodiscard]] const Ref<UniformBuffer>& GetReflectionUBO() const noexcept
        {
            return m_ReflectionUBO;
        }

      private:
        void EnsureFramebuffer();

        bool m_Enabled = false;
        glm::vec4 m_ReflectionPlane{ 0.0f, 1.0f, 0.0f, 0.0f };
        f32 m_Intensity = 1.0f;
        f32 m_Distortion = 0.0f;

        // Reflections can render at reduced resolution; the surface perturbation
        // hides the softness. Half-res keeps the second opaque pass affordable.
        static constexpr u32 kResolutionDivisor = 1u;

        u32 m_Width = 0;
        u32 m_Height = 0;

        SceneRenderPass* m_ScenePass = nullptr;
        Ref<Framebuffer> m_ReflectionFB; ///< owned RGBA16F colour + depth, lazily created
        Ref<UniformBuffer> m_ReflectionUBO;
    };
} // namespace OloEngine
