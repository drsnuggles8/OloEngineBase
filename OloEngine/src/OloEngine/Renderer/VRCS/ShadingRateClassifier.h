#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Variable Rate Compute Shading — the classification half (issue #683).
    //
    // Rates every 8x8 screen tile into a shading FOOTPRINT (1x1 / 2x2 / 4x4)
    // from depth PLANARITY (the residual of a least-squares plane fit over
    // inverse depth — not the depth range, see the shader's header for why that
    // distinction is the difference between a useful feature and one that only
    // ever coarsens the sky), G-Buffer normal agreement and previous-frame
    // luminance, and publishes the result as an R8UI image with one texel per
    // tile. Consuming compute passes read that image through
    // OloEditor/assets/shaders/include/VRCS.glsl and decide, per invocation,
    // whether they are the leader of their footprint.
    //
    // WHY A HELPER CLASS AND NOT A RENDER-GRAPH NODE. The same shape as
    // HZBGenerator, deliberately: a self-contained GPU utility that owns its
    // shader and its target, dispatched from inside whichever pass needs it
    // first. That keeps one classification usable by several consumers without
    // making every consumer depend on a new graph node's ordering, and it is
    // how the HZB is already shared between GTAO and SSR. The cost is that the
    // rate image is NOT a graph-pooled transient, so it must be bound
    // Persistent — which is correct here, since this object owns it outright.
    //
    // ONE CLASSIFICATION PER FRAME. Classify() is idempotent within a frame:
    // hand it the renderer's frame index and a second consumer in the same
    // frame gets the first consumer's result rather than a redundant dispatch.
    // Pass a strictly increasing index; passing the same index forever pins the
    // rates at whatever the first frame produced.
    class ShadingRateClassifier
    {
      public:
        // Classification tile edge, in pixels. GLSL twin: OLO_VRCS_TILE_SIZE in
        // include/VRCS.glsl, and the shader's local_size_x/y — one workgroup
        // per tile, one invocation per pixel. VRCSClassifierTest pins the three
        // together; changing this alone silently mis-sizes the dispatch.
        static constexpr u32 kTileSize = 8u;

        // Footprint encoding stored in the rate image. Mirrors
        // OLO_VRCS_RATE_* in include/VRCS.glsl.
        static constexpr u8 kRate1x1 = 1u;
        static constexpr u8 kRate2x2 = 2u;
        static constexpr u8 kRate4x4 = 4u;

        // Everything the classification dispatch reads. Handles that are not
        // valid degrade the classification rather than failing it: no previous
        // colour simply drops the luminance term (and makes the result MORE
        // conservative), while a missing depth or normals texture makes
        // Classify() a no-op and leaves consumers at full rate.
        struct Inputs
        {
            RHI::ResourceHandle SceneDepth{};    // device Z, viewport-sized
            RHI::ResourceHandle ViewNormals{};   // octahedral normals (RG16F)
            RHI::ResourceHandle PreviousColor{}; // previous frame's resolved colour; optional
            RHI::HeapSlotLifetime SceneDepthLifetime = RHI::HeapSlotLifetime::FrameTransient;
            RHI::HeapSlotLifetime ViewNormalsLifetime = RHI::HeapSlotLifetime::FrameTransient;
            RHI::HeapSlotLifetime PreviousColorLifetime = RHI::HeapSlotLifetime::FrameTransient;

            // proj[2][2] / proj[3][2] — the same two coefficients GTAO uploads,
            // used to turn device Z into a positive view-space distance so the
            // depth threshold can be relative to the tile's nearest surface.
            f32 DepthLinearizeA = 0.0f;
            f32 DepthLinearizeB = 0.0f;
        };

        // Classification thresholds. Field-for-field the VRCS* members of
        // PostProcessSettings; kept as their own struct so a second consumer
        // does not have to hand the classifier a whole post-process settings
        // block. See PostProcessSettings for what each bound means.
        struct Thresholds
        {
            f32 Depth = 0.01f;
            f32 Normal = 0.02f;
            f32 Luma = 0.25f;
            f32 Coarse4x4Scale = 0.25f;
            bool Allow4x4 = false;
        };

        ShadingRateClassifier() = default;
        ~ShadingRateClassifier() = default;

        void Initialize();
        void Shutdown();
        void Reload();

        // Size the rate image for a viewport. Cheap and idempotent when the
        // tile grid does not change.
        void Resize(u32 viewportWidth, u32 viewportHeight);

        // Dispatch classification, unless this frame index was already
        // classified. Returns true when a rate image is available to consume
        // afterwards — including when the dispatch was skipped because this
        // frame was already done.
        bool Classify(u64 frameIndex, const Inputs& inputs, const Thresholds& thresholds);

        // Drop the memoised frame stamp so the next Classify() re-dispatches.
        // Call on any discontinuity that invalidates the rates (resize, a
        // settings change, leaving and re-entering the feature).
        void InvalidateFrame() noexcept
        {
            m_ClassifiedFrame = kNoFrame;
            m_HasClassified = false;
        }

        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_Shader && m_Shader->IsValid() && m_RateTexture && m_TilesX > 0u && m_TilesY > 0u;
        }

        // True once a dispatch has actually written rates into the image. Until
        // then the texture holds its cleared contents, which decode to full
        // rate — safe, but a consumer that wants to skip the extra sample can
        // ask.
        [[nodiscard]] bool HasClassifiedRates() const noexcept
        {
            return m_HasClassified;
        }

        [[nodiscard]] RHI::ResourceHandle GetRateTexture() const;

        // The rate image as an engine texture object. Consumers should bind
        // through GetRateTexture()'s RHI handle; this exists for the GPU test,
        // which has to read the rates back and needs a Texture2D to do it.
        [[nodiscard]] const Ref<Texture2D>& GetRateTextureResource() const noexcept
        {
            return m_RateTexture;
        }

        // The rate image is this object's own texture, never a graph-pooled
        // one, so its heap views are memoisable. Named as a method rather than
        // left to each consumer to assume, for the reason HZBGenerator's
        // GetHZBLifetime() documents: the lifetime follows the RESOURCE.
        [[nodiscard]] static constexpr RHI::HeapSlotLifetime GetRateLifetime() noexcept
        {
            return RHI::HeapSlotLifetime::Persistent;
        }

        [[nodiscard]] u32 GetTilesX() const noexcept
        {
            return m_TilesX;
        }
        [[nodiscard]] u32 GetTilesY() const noexcept
        {
            return m_TilesY;
        }

        // Tile count for a viewport edge. Free function shape so tests and
        // callers can size expectations without owning a classifier.
        [[nodiscard]] static constexpr u32 TileCountFor(u32 pixels) noexcept
        {
            return (pixels + kTileSize - 1u) / kTileSize;
        }

      private:
        static constexpr u64 kNoFrame = ~0ull;

        Ref<ComputeShader> m_Shader;
        Ref<Texture2D> m_RateTexture;
        Ref<UniformBuffer> m_ParamsUBO;

        u32 m_ViewportWidth = 0;
        u32 m_ViewportHeight = 0;
        u32 m_TilesX = 0;
        u32 m_TilesY = 0;

        u64 m_ClassifiedFrame = kNoFrame;
        bool m_HasClassified = false;
    };
} // namespace OloEngine
