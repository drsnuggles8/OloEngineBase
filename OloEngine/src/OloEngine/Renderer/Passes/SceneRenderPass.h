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
#include "OloEngine/Renderer/ResourceHandle.h"

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
    // G-Buffer instead of the forward scene target. After the G-Buffer
    // color pass, DeferredLightingPass composites lit HDR into the scene
    // framebuffer; ForwardOverlayRenderPass then adds overlay geometry that
    // did not participate in G-Buffer writes (skybox, terrain, foliage…).
    class SceneRenderPass : public CommandBufferRenderPass
    {
      public:
        SceneRenderPass();
        ~SceneRenderPass() override = default;

        // The scene pass's MRT attachment layout, in attachment order. The
        // ONE definition shared by the render-graph setup (which sizes the
        // real scene target from it) and by every pass that REPLAYS
        // scene-bucket shaders into its own target (planar reflections): a
        // replayed pipeline's fragment interface must match its render
        // targets, so a mirror that drifts from this list resurrects the
        // per-draw unused-output validation warnings under Vulkan
        // (#691).
        [[nodiscard]] static FramebufferAttachmentSpecification SceneMRTAttachments()
        {
            return { FramebufferTextureFormat::RGBA16F,     // [0] HDR color output
                     FramebufferTextureFormat::RED_INTEGER, // [1] entity ID
                     FramebufferTextureFormat::RG16F,       // [2] view-space normals (octahedral, SSAO input)
                     // [3] velocity (rg) + coverage (b) + material profile (a).
                     // Widened to RGBA16F by #1256 in lock-step with G-Buffer
                     // RT3 so the forward and deferred paths hand the temporal
                     // resolve the same channels; unused in Deferred, which
                     // reads G-Buffer RT3.
                     FramebufferTextureFormat::RGBA16F,
                     // [4] the DIFFUSION HAND-OFF (issue #1241): the diffuse half
                     // of a skin pixel's lighting in .rgb, the identity of the
                     // profile that should blur it in .a. Zero everywhere else,
                     // and every shader that renders into this framebuffer WRITES
                     // it -- an MRT output left alone is undefined, not zero. See
                     // include/SkinDiffusionCommon.glsl.
                     //
                     // It costs a full-resolution RGBA16F for every frame, skin
                     // or no skin, and that is the honest price of the attachment
                     // being part of the scene framebuffer: the alternative is a
                     // per-draw draw-buffer switch keyed on material kind, which
                     // is a state change per draw against 16 MB at 1080p.
                     FramebufferTextureFormat::RGBA16F,
                     FramebufferTextureFormat::Depth };
        }

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        // Deferred path accessor — valid once the G-Buffer has been created
        // by the first Execute() call in Deferred mode, or null otherwise.
        [[nodiscard]] const Ref<GBuffer>& GetGBuffer() const noexcept
        {
            return m_GBuffer;
        }

        // Ensure the deferred G-buffer exists and matches the current scene
        // framebuffer dimensions before graph blackboard population/import.
        void PrepareDeferredResources(u32 sampleCount);

        // THE FORWARD PREPASS AS ITS OWN NODE (issue #1452). On Forward and
        // Forward+ the frame's clear, the bucket's batching and the depth
        // prepass run in ScenePrepassRenderPass, ahead of the screen-space AO
        // passes; Execute() then runs only the colour half. With screen-space
        // AO live the prepass also writes the view normals of attachment 2
        // (DepthNormalPrepass*.glsl) and is forced on even where the settings
        // leave it off, because the AO passes read both. The deferred path
        // never calls these: its AO reads the finished G-Buffer, so its
        // prepass stays inside Execute().
        void SetupForwardPrepass(RGBuilder& builder, FrameBlackboard& board);
        void ExecuteForwardPrepass(RGCommandContext& context, const Ref<Framebuffer>& sceneTarget);
        //   produced — an AO buffer is built this frame, so the prepass writes
        //              the view normals it needs and ScenePass reads it;
        //   applied  — the forward shaders multiply their ambient term by it
        //              (off while the AO debug view replaces the frame);
        //   strength — the AO slider, the same number DeferredLighting uses.
        void SetForwardScreenSpaceAO(bool produced, bool applied, f32 strength)
        {
            m_ForwardScreenSpaceAOProduced = produced;
            m_ForwardScreenSpaceAOApplied = produced && applied;
            m_ForwardScreenSpaceAOStrength = std::isfinite(strength) ? strength : 1.0f;
        }
        [[nodiscard]] bool IsForwardScreenSpaceAOProduced() const noexcept
        {
            return m_ForwardScreenSpaceAOProduced;
        }

        // Setup() reads the AO buffer when a forward AO buffer is produced — so
        // that is a declaration input.
        void AppendDeclarationInputs(RGDeclarationKey& key) const override
        {
            key.Add(m_ForwardScreenSpaceAOProduced);
        }

      private:
        // The frame's start: resolve the target, clear every attachment, reset
        // the fixed-function state, batch and sort the bucket. Shared by both
        // the forward prepass node and the deferred Execute().
        [[nodiscard]] Ref<Framebuffer> BeginSceneFrame(bool deferredActive);
        // Replays the bucket depth-only (or depth + view normal, on the forward
        // paths with AO live) under the "DepthPrepass" timing bracket.
        void RunDepthPrepass(bool writeViewNormals);
        // Copies the scene target's depth and view normals into the graph's
        // SceneDepth / SceneNormals exports. `exportVelocity` is the colour
        // half's alone: velocity is written by the colour draws, and only
        // ScenePass declares the write to its export.
        void ExportSceneDepthAndNormals(RGCommandContext& context, RGTextureHandle depthExport,
                                        RGTextureHandle normalsExport, bool deferredActive, bool exportVelocity);

        // Lazily create / resize the G-Buffer to match the forward target.
        void EnsureGBuffer(u32 width, u32 height, u32 sampleCount);
        // Blit the requested G-Buffer channel into m_Target color[0] so the
        // editor viewport shows something meaningful before deferred lighting.
        void BlitGBufferDebug(u32 channel);

        // Blit the forward scene FB's velocity attachment (RG16F at slot 3)
        // into colour[0] for visualisation in Forward / Forward+ paths.
        // Called when RendererSettings::DebugVelocityOverlayForward is true
        // and the active path is not Deferred (the Deferred path has its
        // own velocity debug visualisation through BlitGBufferDebug(5)).
        void BlitForwardVelocityDebug();

        u32 m_FrameCounter = 0;
        Ref<GBuffer> m_GBuffer;
        u32 m_GBufferSampleCount = 1;
        // Fullscreen shader that gathers RT0.a (metallic), RT1.z (roughness),
        // RT1.w (AO) into one RGB image for DebugChannel == 3. The other
        // debug channels are cheap single-attachment blits.
        Ref<Shader> m_DebugRMAShader;
        RGTextureHandle m_SelectedSceneDepthExport{};
        RGTextureHandle m_SelectedSceneNormalsExport{};
        RGTextureHandle m_SelectedVelocityExport{};
        // The forward prepass node's own export handles (its versions of
        // SceneDepth / SceneNormals, which the AO nodes read).
        RGTextureHandle m_PrepassSceneDepthExport{};
        RGTextureHandle m_PrepassSceneNormalsExport{};
        RGTextureHandle m_PrepassForwardAODepthExport{};
        // Set by ExecuteForwardPrepass, consumed by the Execute() that follows
        // in the same frame: the frame has begun and the bucket is batched.
        bool m_ForwardPrepassRan = false;
        // Whether that prepass actually drew (so the colour half must re-test
        // at GL_LEQUAL with depth writes off).
        bool m_ForwardPrepassDrew = false;
        bool m_ForwardScreenSpaceAOProduced = false;
        bool m_ForwardScreenSpaceAOApplied = false;
        f32 m_ForwardScreenSpaceAOStrength = 1.0f;
        // What the colour half reads for the forward shaders' ambient AO.
        RGTextureHandle m_ForwardAOBuffer{};
        RGTextureHandle m_ForwardAODepth{};
    };
} // namespace OloEngine
