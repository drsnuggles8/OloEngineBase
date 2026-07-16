#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Texture3D.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief Froxel volumetric fog compute chain (issue #435).
    //
    // Runs two compute dispatches over a camera-frustum-aligned 3D froxel
    // volume (default 160×90×64, exponential depth slices from the camera
    // near plane to clamp(FogSettings::End, 20, 500)):
    //
    //   1. FroxelFogScatter.comp — inject participating-media density (global
    //      height fog × FBM noise + local FogVolume shapes) and in-scattered
    //      radiance (sun × CSM shadow tap = volumetric god rays, ambient fog
    //      color, and every local light from the clustered light grid ×
    //      shadow-atlas tap), with temporal reprojection against the previous
    //      frame's scatter volume.
    //   2. FroxelFogIntegrate.comp — front-to-back Hillaire integration along
    //      each column, producing the integrated volume (rgb = accumulated
    //      in-scatter, a = transmittance per slice).
    //
    // FogRenderPass samples the integrated volume (TEX_FROXEL_FOG) with one
    // trilinear tap per pixel — this replaces the old per-pixel screen-space
    // raymarch when FogSettings::EnableVolumetric is on.
    //
    // The pass must execute AFTER ScenePass (the clustered light lists are
    // dispatched inline there) and after ShadowPass; it re-binds the cluster
    // SSBOs itself since ScenePass unbinds them after its color pass.
    // The froxel mapping of the LAST frame the compute chain ran — the CPU
    // mirror of the FroxelFogData UBO both shaders read (issue #607). Exposed
    // so a diagnostic (olo_froxel_fog_probe) can reproduce the shader's
    // world <-> froxel transform EXACTLY instead of re-deriving it from the
    // camera and silently drifting: the froxel z distribution is exponential,
    // and a probe that got that wrong would confidently report a neighbouring
    // cell's scattering as the sampled point's.
    struct FroxelVolumeState
    {
        bool Valid = false; // the chain has run at least once; the fields below are that frame's
        u32 DimX = 0;
        u32 DimY = 0;
        u32 DimZ = 0;
        f32 Near = 0.0f;           // fog volume near plane (view depth, metres)
        f32 Far = 0.0f;            // fog volume far plane
        f32 LogFarOverNear = 0.0f; // log2(Far / Near) — the exponential slice exponent
        glm::mat4 View{ 1.0f };    // render-RELATIVE world -> view
        glm::mat4 InverseView{ 1.0f };
        glm::mat4 Projection{ 1.0f };
        glm::mat4 InverseProjection{ 1.0f };
        glm::vec3 RenderOrigin{ 0.0f };
        u32 ScatterTextureID = 0;    // the volume the last scatter dispatch WROTE (rgb = in-scatter, a = extinction)
        u32 IntegratedTextureID = 0; // FroxelFogIntegrate's output (rgb = accumulated in-scatter, a = transmittance)
    };

    class VolumetricFogPass : public RenderGraphNode
    {
      public:
        // Fog froxel grid dimensions. Coarser in XY than the screen and finer
        // in Z than the light-cluster grid — media vary smoothly, lighting
        // varies with depth.
        static constexpr u32 kVolumeWidth = 160;
        static constexpr u32 kVolumeHeight = 90;
        static constexpr u32 kVolumeDepth = 64;

        VolumetricFogPass();
        ~VolumetricFogPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_ScatterShader && m_ScatterShader->IsValid() &&
                   m_IntegrateShader && m_IntegrateShader->IsValid() &&
                   m_FroxelUBO != nullptr;
        }

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }

        // GL id of the integrated fog volume (0 until the pass has run).
        // FogRenderPass binds this at TEX_FROXEL_FOG for the composite.
        [[nodiscard]] u32 GetIntegratedVolumeID() const
        {
            return m_IntegratedVolume ? m_IntegratedVolume->GetRendererID() : 0;
        }

        // True when the compute chain ran this frame (Execute reached the
        // dispatches). FogRenderPass gates the shader's froxel path on this
        // and re-uploads a disabled froxel UBO when false, so a stale
        // "enabled" flag can never outlive a toggle.
        [[nodiscard]] bool RanThisFrame() const
        {
            return m_RanThisFrame;
        }

        // Upload the froxel UBO with enabled = 0 (called by FogRenderPass when
        // the froxel chain did not run this frame).
        void UploadDisabledUBO();

        // The froxel mapping + volume ids of the last frame the chain ran.
        // Valid == false until then (and it deliberately KEEPS the last valid
        // mapping after a disabling toggle, so a probe can still say "this is
        // the volume that produced the frame you are looking at" rather than
        // silently answering from an identity transform).
        [[nodiscard]] const FroxelVolumeState& GetFroxelVolumeState() const noexcept
        {
            return m_VolumeState;
        }

      private:
        bool m_Enabled = false;
        bool m_RanThisFrame = false;
        bool m_HistoryValid = false;
        u32 m_FrameIndex = 0;

        Ref<ComputeShader> m_ScatterShader;
        Ref<ComputeShader> m_IntegrateShader;

        // Scatter ping-pong (temporal history) + integrated result
        Ref<Texture3D> m_ScatterVolume[2];
        Ref<Texture3D> m_IntegratedVolume;
        u32 m_CurrentScatter = 0;

        Ref<UniformBuffer> m_FroxelUBO;

        // Previous frame's ABSOLUTE-world view-projection for 3D reprojection
        glm::mat4 m_PrevViewProjection = glm::mat4(1.0f);
        bool m_PrevViewProjectionValid = false;

        // Last frame's froxel mapping, published for olo_froxel_fog_probe.
        FroxelVolumeState m_VolumeState;
    };
} // namespace OloEngine
