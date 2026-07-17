#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Timestep.h"

namespace OloEngine
{
    class Scene;
    struct WeatherPreset;
    struct WeatherStateComponent;
    enum class WeatherStateId : i32;

    // Weather director (issue #633, Pillar A). Owns the cross-blend between
    // the named weather states authored on the scene's WeatherStateComponent
    // and applies the blended result coherently to every coupled subsystem:
    // the scene-level Wind/Fog/Precipitation/SnowAccumulation settings (read
    // by gameplay systems like ClothWindSystem), their Renderer3D global
    // twins (read by the render pipeline's per-frame system updates), and the
    // derived wetness / cloud-coverage / sun-dimming signals the other two
    // pillars consume.
    //
    // Registered in Scene::GetGameplayScheduler() as "Weather"
    // (Before("PhysicsKick") — cloth reads wind settings inside the kick).
    // In edit mode the scheduler doesn't run; Scene::OnUpdateEditor calls
    // ApplyImmediate every editor tick so edits preview live without Play.
    class WeatherSystem
    {
      public:
        // Scheduler tick: advance the transition + wetness dynamics on the
        // first enabled WeatherStateComponent, then apply the blend.
        static void Tick(Scene& scene, Timestep ts);

        // Apply the component's current state without advancing time
        // (editor preview after an inspector edit, play-start coherence).
        static void ApplyImmediate(Scene& scene);

        // ── Pure helpers (unit-tested by WeatherBlendTest.cpp) ──

        // Smoothstep ease used for every transition parameter.
        [[nodiscard]] static f32 EaseSmoothstep(f32 t);

        // Blend two authored presets at eased factor t in [0,1].
        // Continuous fields lerp; a disabled endpoint contributes zero
        // fog density / precipitation intensity / snow rate so toggles ramp
        // instead of popping; the precipitation kind switches at t >= 0.5;
        // fog colour uses the enabled endpoint's colour when only one side
        // has fog.
        [[nodiscard]] static WeatherPreset BlendPresets(const WeatherPreset& from,
                                                        const WeatherPreset& to, f32 t);

        // Wetness integrator: rises toward `target` at riseRate while
        // precipitating (targetActive), decays toward target (usually lower)
        // at dryRate otherwise. Returns the new wetness in [0,1].
        [[nodiscard]] static f32 AdvanceWetness(f32 current, f32 target, bool precipitating,
                                                f32 riseRate, f32 dryRate, f32 dt);

        // The authored preset for a named state on this component.
        [[nodiscard]] static const WeatherPreset& PresetFor(const WeatherStateComponent& component,
                                                            WeatherStateId id);

      private:
        // Transition bookkeeping: detect target edits, snapshot the blend
        // origin, advance progress, recompute m_Blended. dt = 0 recomputes
        // without advancing (editor preview).
        static void UpdateTransition(WeatherStateComponent& component, f32 dt);

        // Write the component's m_Blended into the scene settings + the
        // Renderer3D global settings twins.
        static void ApplyBlended(Scene& scene, WeatherStateComponent& component);
    };
} // namespace OloEngine
