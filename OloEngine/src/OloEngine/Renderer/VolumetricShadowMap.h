#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <glm/glm.hpp>

#include <array>

namespace OloEngine
{
    class ComputeShader;
    class Texture3D;
    struct CloudscapeRenderState;
    struct FogSettings;
    struct FogVolumesUBOData;

    // =========================================================================
    // The MATH half of the volumetric shadow map (issue #723), deliberately
    // free of GL so a headless test can pin it (VolumetricShadowMapMathTest,
    // L1). Everything here is pure geometry: how a world-space domain becomes
    // a light-space box, and how that box becomes the two transforms the
    // shader uses.
    // =========================================================================
    namespace VolumetricShadowMath
    {
        // Orthonormal light frame. AxisZ is the direction the light TRAVELS
        // (away from the source), so light-space z increases with distance
        // from the light — which is the direction optical depth accumulates.
        struct LightFrame
        {
            glm::vec3 AxisX{ 1.0f, 0.0f, 0.0f };
            glm::vec3 AxisY{ 0.0f, 1.0f, 0.0f };
            glm::vec3 AxisZ{ 0.0f, 0.0f, 1.0f };
        };

        // World-space axis-aligned domain the cascade must cover.
        struct Bounds
        {
            glm::vec3 Min{ 0.0f };
            glm::vec3 Max{ 0.0f };

            [[nodiscard]] bool IsValid() const noexcept
            {
                return Max.x > Min.x && Max.y > Min.y && Max.z > Min.z;
            }
        };

        // A cascade's oriented box: the world position of texture coordinate
        // (0,0,0) plus the three edge directions and their world lengths.
        struct CascadeFit
        {
            glm::vec3 OriginAbs{ 0.0f };
            glm::vec3 AxisX{ 1.0f, 0.0f, 0.0f };
            glm::vec3 AxisY{ 0.0f, 1.0f, 0.0f };
            glm::vec3 AxisZ{ 0.0f, 0.0f, 1.0f };
            f32 SizeX = 0.0f;
            f32 SizeY = 0.0f;
            f32 SizeZ = 0.0f;

            [[nodiscard]] bool IsValid() const noexcept
            {
                return SizeX > 0.0f && SizeY > 0.0f && SizeZ > 0.0f;
            }
        };

        // Build an orthonormal frame whose AxisZ points AWAY from the light.
        // `towardLight` need not be normalized; a degenerate (zero-length)
        // input falls back to a straight-down sun rather than producing NaNs.
        [[nodiscard]] LightFrame BuildLightFrame(const glm::vec3& towardLight);

        // Fit an oriented box around `domain` in `frame`'s basis.
        //
        // This is what makes the map behave at EVERY sun elevation: the extents
        // come from projecting the domain's eight corners onto the three axes,
        // so a low sun produces a long, thin box across the domain instead of
        // the unbounded march a "start at the layer top, walk down" formulation
        // would need (1 / sin(elevation) blows up at the horizon).
        //
        // `texelsXY` and `slicesZ` texel-snap the light-space origin on all
        // three axes, so a translating camera re-samples the same world
        // positions instead of swimming the shadow pattern — the CloudShadowMap
        // discipline, one dimension up. Pass 0 for either to skip that axis.
        //
        // SNAPPING Z MATTERS TOO, which is not obvious: an unsnapped z leaves
        // the footprint stable but slides every sample along the light ray by a
        // fraction of a slice each frame. The map itself has no history so it
        // would not care — but its CONSUMERS do (the cloud temporal resolve and
        // the froxel history), and a per-frame wobble in the sampled optical
        // depth is exactly what they would smear into shimmer.
        [[nodiscard]] CascadeFit FitCascade(const Bounds& domain, const LightFrame& frame, u32 texelsXY,
                                            u32 slicesZ);

        // RENDER-RELATIVE world -> the cascade's own [0,1]^3 (issue #429: every
        // consumer hands us the space its fragments already carry).
        [[nodiscard]] glm::mat4 MakeRelWorldToTex(const CascadeFit& fit, const glm::vec3& renderOrigin);

        // The cascade's [0,1]^3 -> ABSOLUTE world. NOT the inverse of the
        // matrix above (different source space): the generator evaluates cloud
        // and fog density fields, which are defined in absolute world space.
        [[nodiscard]] glm::mat4 MakeTexToAbsWorld(const CascadeFit& fit);

        // World AABB of one GPU fog volume record, derived from its
        // WorldToLocal transform, shape and extents. Returns an invalid Bounds
        // when the record cannot contribute.
        //
        // The record's FalloffDistance is deliberately NOT an input: in
        // FogVolumeCommon.glsl the influence is
        // `1 - smoothstep(-falloff, 0, sdf)`, which is already zero at the
        // shape's own surface — the falloff softens INWARD, so it never widens
        // the box. Expanding by it would just cost resolution.
        [[nodiscard]] Bounds FogVolumeWorldBounds(const glm::mat4& worldToLocal, i32 shape,
                                                  const glm::vec3& extents);

        // Union of two domains; an invalid operand is ignored.
        [[nodiscard]] Bounds UnionBounds(const Bounds& a, const Bounds& b);

        // Clip `bounds` to the axis-aligned box of side 2*halfExtent centred on
        // `center`. Keeps one distant fog volume from stretching the cascade
        // until nothing near the camera has any resolution left.
        [[nodiscard]] Bounds ClampBoundsToWindow(const Bounds& bounds, const glm::vec3& center, f32 halfExtent);
    } // namespace VolumetricShadowMath

    /**
     * @brief Shared volumetric shadow map — self-shadowing for clouds and fog
     *        volumes (issue #723).
     *
     * Owns ONE R32F 3D texture holding, per texel, the OPTICAL DEPTH
     * accumulated from the light through the participating medium — the
     * quantity a scattering march needs, rather than a transmittance, because
     * the cloud consumer still has to run its powder and multi-scatter octaves
     * on it (`cloudBeerPowder`). Transmittance is `exp(-tap)`.
     *
     * The volume is TWO CASCADES stacked along z, both written by one dispatch
     * of compute/VolumetricShadow_Generate.comp:
     *
     *   Cascade::Cloud — the cloud layer slab (CloudscapeCommon's density
     *                    field, cheap taps). Replaces the far half of the
     *                    raymarch's per-sample light cone, which was capped at
     *                    1400 m and therefore could not darken the inside of a
     *                    kilometre-thick deck at all.
     *   Cascade::Fog   — the height-fog slab plus every enabled FogVolume
     *                    shape. This medium had NO self-occlusion before: a
     *                    dense fog volume was lit identically at its lit face
     *                    and 50 m into it.
     *
     * Each cascade is an oriented box fitted to its domain in the light's frame
     * (VolumetricShadowMath::FitCascade), so one thread per (x, y, cascade)
     * marches straight down the light ray writing the running total into every
     * slice as it passes — the accumulation is free, and no slice is ever
     * undersampled the way an independent per-cell march would be.
     *
     * NO TEMPORAL TERM, deliberately. The march is a fixed midpoint quadrature
     * over a fixed slice grid: it is a pure function of the frame's settings,
     * with no jitter and no history. That is what keeps it out of the
     * golden-flicker business (issue #723's third acceptance criterion) —
     * precisely: the map adds no time dependence of its own, it does not remove
     * the medium's. Animated fog noise still moves the fog, and the shadow
     * follows it, which is correct; a golden capturing that has to freeze the
     * clock exactly as it already does for the fog itself. Banding is handled by
     * the volume's trilinear filter and by the fact that a shadow is inherently
     * low-frequency, not by dithering the samples.
     *
     * ---------------------------------------------------------------------
     * CALLER CONTRACT — two phases, in this order, and the split is REAL:
     *
     *   1. PrepareFrame(...) — CPU only. Computes both cascade frames.
     *   2. The caller folds GetCascade() into UBOStructures::AtmosphereShadingUBO
     *      and uploads + binds it at UBO_ATMOSPHERE_SHADING (54).
     *   3. Dispatch() — the compute reads those transforms back out of that
     *      same UBO, so it CANNOT run before step 2.
     *
     * Dispatch() additionally requires the cloud field's UBO + samplers
     * (UBO_CLOUDSCAPE 53, TEX_CLOUD_* 59/60/61) and the fog UBOs (UBO_FOG 17,
     * UBO_FOG_VOLUMES 20) to be uploaded and bound — the same "the render
     * pipeline uploaded these already" contract CloudShadowMap documents.
     * RenderPipeline::UploadExecutionState is the intended caller and binds all
     * of them earlier in the same function.
     *
     * Dispatch() creates GL resources and dispatches compute work, so it must
     * only run with a live GL context on the render thread. There is no
     * headless guard (same contract as CloudNoise::EnsureGenerated()).
     */
    class VolumetricShadowMap
    {
      public:
        // XY texels per cascade. 128 over a 12 km cloud window is ~94 m/texel,
        // which is the scale cloud self-shadowing actually varies at; over the
        // fog window (~300 m by default) it is sub-metre.
        static constexpr u32 kResolution = 128;
        // Depth texels per cascade. 64 to BALANCE THE TWO AXES rather than for
        // its own sake: the worst case is a low sun over the cloud cascade,
        // where the fitted box is ~17 km deep along the light and 17 km wide,
        // so 128 texels give ~132 m laterally. At 32 slices the march step was
        // ~545 m — four times coarser than the axis beside it, and that axis is
        // the one carrying the "dark underneath" gradient the whole feature
        // exists for. 64 brings it to ~272 m for 8 MB of R32F.
        static constexpr u32 kSlicesPerCascade = 64;
        static constexpr u32 kCascadeCount = 2;
        static constexpr u32 kVolumeDepth = kSlicesPerCascade * kCascadeCount;

        enum class Cascade : u32
        {
            Cloud = 0,
            Fog = 1,
        };

        struct CascadeState
        {
            bool Enabled = false;
            glm::mat4 RelWorldToTex{ 1.0f };
            glm::mat4 TexToAbsWorld{ 1.0f };
            f32 StepLength = 0.0f; // world metres per slice along the light ray
            f32 Strength = 0.0f;   // artistic scale on the sampled optical depth
        };

        // Phase 1 (CPU). Recomputes both cascade frames from this frame's
        // media settings. Safe to call headless — touches no GL.
        //
        // @param clouds            This frame's cloudscape snapshot.
        // @param fog               This frame's fog settings.
        // @param fogVolumes        The fog-volume records already staged for
        //                          UBO_FOG_VOLUMES; their world bounds widen
        //                          the fog cascade so a volume placed away from
        //                          the camera still self-shadows.
        // @param cameraPosAbsolute ABSOLUTE world camera position
        //                          (Renderer3DData::ViewPos), not the
        //                          camera-relative UBO copy.
        // @param renderOrigin      Renderer3D::GetRenderOrigin().
        // @param fogTowardLight    Unit vector toward the directional light for
        //                          the FOG cascade. The cloud cascade uses
        //                          `clouds.SunDirection` instead, on purpose:
        //                          each cascade must agree with the light
        //                          direction its own consumer shades with, or
        //                          the shadow and the lighting disagree.
        // @param cloudFieldReady   Whether the cloud noise volumes actually
        //                          generated this frame. A SEPARATE gate from
        //                          `clouds.Enabled`, because the pipeline's
        //                          fail-safe path leaves the state enabled and
        //                          disables the cloud UBO instead — and the
        //                          generator marches the same noise volumes,
        //                          so it must fail safe with it.
        static void PrepareFrame(const CloudscapeRenderState& clouds, const FogSettings& fog,
                                 const FogVolumesUBOData& fogVolumes, const glm::vec3& cameraPosAbsolute,
                                 const glm::vec3& renderOrigin, const glm::vec3& fogTowardLight,
                                 bool cloudFieldReady);

        // Phase 2 (GPU). Lazily creates the volume + compute shader on the
        // first call; a creation failure is latched (logged once, subsequent
        // calls no-op cheaply) until Shutdown() clears it. No-ops when
        // PrepareFrame left every cascade disabled.
        static void Dispatch();

        /// Release the volume/shader (and clear the failure latch). Safe twice.
        static void Shutdown();

        /// @return true after the first successful Dispatch().
        [[nodiscard]] static bool IsReady();

        /// @return true once resource creation has failed and latched. The
        /// failure is sticky, so this is the honest input to "should the
        /// consumers be told a cascade is available?" — unlike IsReady(), which
        /// is false on the first frame purely because Dispatch() has not run
        /// yet (PrepareFrame precedes it by contract).
        [[nodiscard]] static bool HasFailed();

        /// @return Identity of the R32F volume; RHI::NullResource when not ready.
        [[nodiscard]] static RHI::ResourceHandle GetTextureHandle();

        /// @return This frame's frame data for one cascade (from PrepareFrame).
        [[nodiscard]] static const CascadeState& GetCascade(Cascade cascade);

        /// @return true when PrepareFrame enabled at least one cascade.
        [[nodiscard]] static bool AnyCascadeEnabled();

      private:
        struct VolumetricShadowMapData
        {
            Ref<ComputeShader> m_GenerateShader;
            Ref<Texture3D> m_Volume;
            std::array<CascadeState, kCascadeCount> m_Cascades{};
            bool m_Ready = false;
            bool m_CreationFailed = false;
            // The volume holds optical depth from some earlier frame and has
            // not been cleared since. Set whenever a cascade marched, so the
            // frame that turns the last cascade OFF still runs one clearing
            // dispatch — see Dispatch().
            bool m_HoldsStaleData = false;
        };

        static VolumetricShadowMapData s_Data;
    };
} // namespace OloEngine
