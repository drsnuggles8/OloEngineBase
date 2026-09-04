#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceField.h"
#include "OloEngine/Renderer/Water/WaterFoam.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class ComputeShader;
    class Texture2D;
    class UniformBuffer;

    /// One submitted water disturbance (issue #967).
    ///
    /// A CAPSULE from `m_From` to `m_To`, so one record covers a whole frame's
    /// hull sweep; a point disturbance (a propeller burst, an impact splash) is
    /// simply `m_From == m_To`. See WaterDisturbance::SplatWeight for why the
    /// swept form rather than a per-frame disc.
    ///
    /// Note what is NOT here: nothing about boats. Submitting a splat requires
    /// no BoatComponent, no Rigidbody, and no physics scene — that independence
    /// is an acceptance criterion of #967, and it is what lets propellers,
    /// impacts, scripted effects and future actors be clients on equal terms
    /// with the hull wake.
    struct WaterDisturbanceSplat
    {
        glm::vec2 m_From{ 0.0f }; ///< world XZ (ABSOLUTE, not camera-relative)
        glm::vec2 m_To{ 0.0f };   ///< world XZ at the far end of the sweep
        f32 m_Radius = 1.5f;      ///< metres
        f32 m_Strength = 1.0f;    ///< peak intensity in [0, 1]
        f32 m_Softness = 1.5f;    ///< radial falloff exponent (higher = tighter core)
        f32 m_TimeSeconds = 0.0f; ///< submission timestamp; diagnostic only, the
                                  ///< field's own decay is what ages a splat
    };

    /// Scene-level appearance/lifetime controls, published each frame by
    /// Scene::ProcessScene3DSharedLogic from the dominant WaterComponent and
    /// consumed by RenderPipeline. Mirrors the CloudscapeRenderState handoff.
    struct WaterDisturbanceSettings
    {
        bool m_Enabled = false;
        /// Shader multiplier on the sampled field. 0 disables the whole feature
        /// at the sampling end too (WaterUBO::WakeFieldParams.w).
        f32 m_Intensity = 1.0f;
        /// Seconds for a disturbance to halve. Drives the exponential decay, so
        /// it is frame-rate independent by construction.
        f32 m_HalfLifeSeconds = 6.0f;
        /// Camera distances over which the wake fades out. Deliberately far
        /// longer than the crest-foam fade — see WaterUBO::WakeFieldParams2.
        f32 m_FadeStartMetres = 60.0f;
        f32 m_FadeEndMetres = 220.0f;
    };

    // =========================================================================
    // WaterDisturbanceSystem — the renderer-owned water-field service
    // (issues #967 and #1034).
    //
    // Maintains a world-anchored, toroidally stored RGBA16F field, updated once
    // per frame by ONE compute dispatch. Two independent signals share the
    // lattice because they want exactly the same window, and the engine has one
    // free UBO binding and no free sampler slot to spend on a second one:
    //
    //   .r  = "how churned is the water here" — the wake / actor disturbance
    //         (issue #967). Decays in place, stamped by submitted splats.
    //         Water.glsl samples it ON TOP of the existing procedural /
    //         shoreline / Jacobian foam.
    //   .g  = advected open-ocean foam (issue #1034 §2.2). Decays AND MOVES with
    //         the surface, deposited where the FFT folds. Water.glsl samples it
    //         INSTEAD OF the instantaneous Jacobian term, which is the term that
    //         pulses in place.
    //   .ba = the previous frame's FFT horizontal displacement, which is the
    //         state the foam's advecting velocity is differenced from.
    //
    // The pass is PING-PONGED — two textures, swapped each frame — and only the
    // foam needs that: the wake reads its own texel, but a semi-Lagrangian
    // advection reads a neighbour, and reading neighbours while writing in
    // place races between work groups. See WaterFoam.h §2.
    //
    // The field's precision, format and addressing are WaterDisturbanceField.h;
    // the advection recurrence is WaterFoam.h.
    //
    // Deliberately a static singleton dispatched from RenderPipeline rather
    // than a render-graph pass, mirroring SnowAccumulationSystem — which is the
    // same shape (a persistent, camera-followed, compute-updated world field
    // sampled by surface shading). Two reasons, and the second is the load-
    // bearing one:
    //
    //   * a persistent history texture is exactly the resource the render
    //     graph's transient aliasing is wrong for — `WriteNewVersion` renames a
    //     physical resource, and the stale-pool-read archetype in
    //     docs/agent-rules/render-graph-transient-aliasing.md is precisely what
    //     a cross-frame field invites;
    //   * it keeps this change out of the render graph's internals entirely,
    //     which #807 is concurrently reworking.
    //
    // The whole first slice is VISUAL. Nothing here feeds buoyancy or water
    // height; that is #967's stated non-goal and the reason no part of this
    // service is readable by physics.
    // =========================================================================
    class WaterDisturbanceSystem
    {
      public:
        /// Create the field texture, compute shader and params UBO.
        static void Init();

        /// Release GPU resources.
        static void Shutdown();

        /// @return true once Init() has succeeded.
        [[nodiscard]] static bool IsInitialized();

        /// Queue one disturbance for this frame's dispatch.
        ///
        /// Safe to call with no GL context and before Init(): the queue is plain
        /// CPU state, so a headless scene tick can record wake without a
        /// renderer. Non-finite fields are REJECTED here (returning false) —
        /// this is the single validation boundary, which is what lets
        /// WaterDisturbance::SplatWeight stay a literal mirror of its GLSL twin
        /// instead of sanitising differently on each side.
        ///
        /// Bounded at WaterDisturbance::kMaxSplatsPerFrame. When the queue is
        /// full the splat is DROPPED and counted (GetDroppedSplatCount), not
        /// substituted for an existing one: first-come is deterministic given
        /// the gameplay scheduler's deterministic system order, whereas
        /// evicting by strength or age would make the visible wake depend on
        /// submission interleaving. The cap is generous enough (96) that a
        /// realistic scene never reaches it; a scene that does gets a throttled
        /// warning rather than silence.
        ///
        /// @return true if the splat was queued.
        static bool SubmitSplat(const WaterDisturbanceSplat& splat);

        /// Run this frame's field update: recentre the window on `followXZ`,
        /// decay, and stamp the queued splats. Clears the queue.
        ///
        /// `followXZ` is the ABSOLUTE world XZ the field window centres on —
        /// the camera, in practice. It is snapped to the texel lattice inside,
        /// so passing a continuously moving position is correct and expected.
        /// The dispatch runs when EITHER feature is enabled, and the two gate
        /// independently: open-ocean whitecaps advect in a scene with no boat
        /// in it, and a boat leaves a wake on a Gerstner sea that has no fold
        /// signal to deposit foam from. With both off the field is marked for a
        /// clear and nothing is dispatched at all.
        static void Update(const WaterDisturbanceSettings& settings,
                           const WaterFoam::WaterFoamSettings& foam, glm::vec2 followXZ,
                           Timestep dt);

        /// Publish + bind the field at TEX_WATER_DISTURBANCE for water shading.
        static void BindFieldTexture();

        [[nodiscard]] static RHI::ResourceHandle GetFieldTextureHandle();

        /// WaterUBO::WakeFieldParams for the current window. Returns w == 0
        /// (the disabled state) whenever the field is not usable this frame, so
        /// a caller that packs this unconditionally cannot show a stale field.
        [[nodiscard]] static glm::vec4 GetShaderParams();

        /// WaterUBO::WakeFieldParams2 for the current settings.
        [[nodiscard]] static glm::vec4 GetShaderParams2();

        /// WaterUBO::FoamFieldParams for the current window (issue #1034).
        /// Returns w == 0 (the disabled state) for every reason the advected
        /// foam could be unusable — not initialized, advection off, or the
        /// field never yet written — so a caller that packs this
        /// unconditionally cannot put a stale foam field on screen. It is a
        /// SEPARATE accessor from GetShaderParams() rather than a channel of
        /// it because the two features gate independently.
        [[nodiscard]] static glm::vec4 GetFoamShaderParams();

        /// Drop the whole field on the next Update.
        ///
        /// Must be called on scene load / topology reset: the field is
        /// world-anchored and persists across frames, so without this a new
        /// scene inherits the previous scene's wake at the same world
        /// coordinates. That is the cross-frame-history defect
        /// docs/agent-rules/runtime-scene-switching.md describes, and it is
        /// invisible in any test that only ever loads one scene.
        static void Reset();

        /// Splats queued for the next dispatch. Diagnostics and tests.
        [[nodiscard]] static u32 GetQueuedSplatCount();

        /// Splats rejected since the last Reset because the queue was full.
        /// Diagnostics and tests; a non-zero value means the visible wake is
        /// incomplete.
        [[nodiscard]] static u32 GetDroppedSplatCount();

      private:
        static void UploadComputeParams();

        struct WaterDisturbanceData
        {
            Ref<ComputeShader> m_UpdateShader;
            /// The ping-pong pair. RGBA16F, kResolution^2 each.
            /// m_FieldTextures[m_WriteIndex] is what the last dispatch wrote and
            /// what BindFieldTexture publishes; the other holds the frame before.
            Ref<Texture2D> m_FieldTextures[2];
            u32 m_WriteIndex = 0;
            Ref<UniformBuffer> m_ParamsUBO; // UBO_WATER_DISTURBANCE

            // The frame's queue. A plain array rather than a vector: the bound
            // is structural (it is the UBO array's size), and a growable
            // container here would let the CPU side silently accept splats the
            // GPU side cannot possibly consume.
            WaterDisturbanceSplat m_Splats[WaterDisturbance::kMaxSplatsPerFrame]{};
            u32 m_SplatCount = 0;
            u32 m_DroppedSplats = 0;

            glm::ivec2 m_LatticeMin{ 0 };
            glm::ivec2 m_PrevLatticeMin{ 0 };
            /// Set until the first Update runs, and again after Reset(). While
            /// set, the dispatch clears the field instead of decaying it — and
            /// GetShaderParams() reports disabled, so nothing samples a field
            /// that has never been written.
            bool m_NeedsClear = true;
            bool m_HasValidWindow = false;
            bool m_Initialized = false;

            WaterDisturbanceSettings m_Settings{};
            WaterFoam::WaterFoamSettings m_FoamSettings{};
            f32 m_DecayFactor = 1.0f;
            f32 m_FoamDecayFactor = 1.0f;
            f32 m_DeltaSeconds = 0.0f;
        };

        static WaterDisturbanceData s_Data;
    };
} // namespace OloEngine
