#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Water/WaterRainRipples.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // =========================================================================
    // WaterRainRippleSystem — joins the two halves of the rain-ripple state
    // (issue #1034, §7.3).
    //
    // The feature needs facts from two places that never meet:
    //
    //   * the WATER tile says whether ripples are wanted at all and how strong
    //     (Scene::ProcessScene3DSharedLogic, dominant-surface rule);
    //   * the SKY says whether it is actually raining, and how hard
    //     (RenderPipeline, from PrecipitationSystem's SMOOTHED intensity, so a
    //     storm fading in fades the stipple in with it).
    //
    // This class is where they meet, and it is the only place that decides the
    // final `strength`. Putting that decision in CommandDispatch instead would
    // have meant reaching from the water draw into the precipitation system,
    // which is the coupling the rest of the water services exist to avoid.
    //
    // No GPU resources and nothing to shut down: it is plain CPU state, like
    // WaterWakeSystem. It carries the same obligation to Reset() on scene
    // entry — not because the state is world-anchored, but because a scene
    // switch must not leave the previous scene's rain on the new scene's sea
    // for a frame.
    //
    // WHY SNOW IS EXCLUDED: a snowflake landing on water does not make a ring,
    // it melts. Rain, hail and sleet all strike hard enough to. That test lives
    // here rather than in the shader because the shader has no idea what is
    // falling — it only ever sees the combined strength.
    // =========================================================================
    class WaterRainRippleSystem
    {
      public:
        /// Publish the dominant water surface's ripple tunables. Called every
        /// frame, including with the default-constructed (disabled) form when
        /// no tile asked for ripples — publishing only when enabled is what
        /// would let a scene switch inherit the previous scene's rain.
        static void SetSettings(const WaterRain::WaterRainSettings& settings);

        [[nodiscard]] static const WaterRain::WaterRainSettings& GetSettings();

        /// Publish this frame's live precipitation state.
        ///
        /// `intensity` is PrecipitationSystem's SMOOTHED value, not the
        /// authored target: the smoothed one is what the particles themselves
        /// are using, so the stipple and the raindrops fade together instead of
        /// the surface snapping dry the instant the weather is switched off.
        ///
        /// `rippleCapable` is false for snow (see the class comment) and for a
        /// disabled precipitation system.
        static void SetPrecipitation(bool rippleCapable, f32 intensity);

        /// WaterUBO::RainRippleParams — (strength, density, cellSizeMetres,
        /// unused). Returns x == 0 (the disabled state) whenever ripples are
        /// not usable this frame, so a caller that packs this unconditionally
        /// cannot leave a stale stipple on a dry sea.
        [[nodiscard]] static glm::vec4 GetShaderParams();

        /// WaterUBO::RainRippleParams2 — (fadeStartMetres, fadeEndMetres, 0, 0).
        [[nodiscard]] static glm::vec4 GetShaderParams2();

        /// The combined strength the shader will see. Diagnostics and tests;
        /// 0 means the fragment-side early-out fires.
        [[nodiscard]] static f32 GetEffectiveStrength();

        /// Drop the settings and the precipitation state. Called on scene
        /// load / runtime start alongside WaterDisturbanceSystem::Reset.
        static void Reset();

      private:
        struct WaterRainData
        {
            WaterRain::WaterRainSettings m_Settings{};
            bool m_RippleCapable = false;
            f32 m_Intensity = 0.0f;
        };

        static WaterRainData s_Data;
    };
} // namespace OloEngine
