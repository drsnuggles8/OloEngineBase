#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    class ComputeShader;
    class Texture2D;
    class Texture3D;

    /**
     * @brief Procedurally generated cloud-noise resources.
     *
     * Owns the tiling noise textures the volumetric-cloud raymarch samples:
     *   - base noise   128³ RGBA8 (repeat): R = Perlin-Worley (freq 4),
     *                  G/B/A = Worley FBM at freq 8/16/32 (erosion octaves)
     *   - detail noise 32³ RGBA8 (repeat):  R/G/B = Worley FBM at freq 8/16/32
     *   - weather map  512² RGBA8:          R = coverage, G = cloud type,
     *                  B = wetness (0), A = 255 — a deterministic CPU
     *                  value-noise default for scenes without an authored map
     *
     * The 3D volumes are baked once on the GPU (CloudNoise_Generate.comp) on
     * the first EnsureGenerated() call and cached until Shutdown(). Because
     * EnsureGenerated() creates GL resources and dispatches compute work, it
     * MUST only be called with a live GL context on the render thread — the
     * render pipeline is the intended caller; there is no headless guard
     * (same contract as WindSystem::Init()).
     *
     * Usage:
     *   if (CloudNoise::EnsureGenerated())
     *   {
     *       // bind GetBaseNoiseTextureID() / GetDetailNoiseTextureID() /
     *       // GetDefaultWeatherMapTextureID() for the cloud raymarch
     *   }
     *   ...
     *   CloudNoise::Shutdown();   // safe to call twice
     */
    class CloudNoise
    {
      public:
        // Lazily creates + generates on first call (needs a live GL context;
        // caller is the render pipeline). Returns false when resources failed.
        // A failure is latched (logged once, subsequent calls return false
        // cheaply) until Shutdown() clears it.
        static bool EnsureGenerated();

        /// Release textures/shader. Safe to call twice.
        static void Shutdown();

        /// @return true after EnsureGenerated() succeeded.
        [[nodiscard]] static bool IsReady();

        /// @return GL renderer id of the 128³ RGBA8 repeat base-noise volume (0 when not ready).
        [[nodiscard]] static u32 GetBaseNoiseTextureID();

        /// @return GL renderer id of the 32³ RGBA8 repeat detail-noise volume (0 when not ready).
        [[nodiscard]] static u32 GetDetailNoiseTextureID();

        /// @return GL renderer id of the 512² RGBA8 procedural default weather map (0 when not ready).
        [[nodiscard]] static u32 GetDefaultWeatherMapTextureID();

      private:
        struct CloudNoiseData
        {
            Ref<ComputeShader> m_GenerateShader;
            Ref<Texture3D> m_BaseNoise;   // 128³ RGBA8, repeat
            Ref<Texture3D> m_DetailNoise; // 32³ RGBA8, repeat
            Ref<Texture2D> m_WeatherMap;  // 512² RGBA8 procedural default

            bool m_Generated = false;
            bool m_GenerationFailed = false;
        };

        static CloudNoiseData s_Data;
    };
} // namespace OloEngine
