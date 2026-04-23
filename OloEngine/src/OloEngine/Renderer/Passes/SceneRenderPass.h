#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/GBuffer.h"

namespace OloEngine
{
    // @brief Render pass for the main 3D scene.
    //
    // This pass handles the rendering of 3D scene objects to an offscreen framebuffer
    // using the command bucket system for efficient batching and sorting.
    //
    // All standard meshes, terrain, voxels, and skybox go through the CommandBucket
    // for DrawKey-based sorting and dispatch (Molecular Matters style).
    //
    // Foliage and decals are handled by their own dedicated render passes
    // (FoliageRenderPass, DecalRenderPass) that execute after this pass in the
    // render graph.
    //
    // Deferred path: when RenderingPath::Deferred is active, Execute() binds a
    // 4-RT G-Buffer instead of the forward scene target. After the G-Buffer
    // color pass, DeferredLightingPass composites lit HDR into the scene
    // framebuffer; ForwardOverlayRenderPass then adds overlay geometry that
    // did not participate in G-Buffer writes (skybox, terrain, foliage…).
    class SceneRenderPass : public CommandBufferRenderPass
    {
      public:
        SceneRenderPass();
        ~SceneRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        // Deferred path accessor — valid once the G-Buffer has been created
        // by the first Execute() call in Deferred mode, or null otherwise.
        [[nodiscard]] const Ref<GBuffer>& GetGBuffer() const noexcept
        {
            return m_GBuffer;
        }

      private:
        // Lazily create / resize the G-Buffer to match the forward target.
        void EnsureGBuffer(u32 width, u32 height, u32 sampleCount);
        // Blit the requested G-Buffer channel into m_Target color[0] so the
        // editor viewport shows something meaningful before Phase 3 lighting.
        void BlitGBufferDebug(u32 channel);

        u32 m_FrameCounter = 0;
        Ref<GBuffer> m_GBuffer;
        u32 m_GBufferSampleCount = 1;
        // Fullscreen shader that gathers RT0.a (metallic), RT1.z (roughness),
        // RT1.w (AO) into one RGB image for DebugChannel == 3. The other
        // debug channels are cheap single-attachment blits.
        Ref<Shader> m_DebugRMAShader;
    };
} // namespace OloEngine
