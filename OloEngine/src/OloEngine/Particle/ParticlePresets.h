#pragma once

#include "OloEngine/Particle/ParticleSystem.h"

namespace OloEngine
{
    /**
     * @brief Factory functions for common particle system presets.
     *
     * Each preset configures a ParticleSystem with physically-plausible
     * defaults so that users get a high-quality starting point without
     * having to tune dozens of parameters from scratch.
     */
    class ParticlePresets
    {
      public:
        /**
         * @brief Configure a ParticleSystem as a realistic snowfall emitter.
         *
         * Settings:
         *   - GPU compute path (UseGPU = true)
         *   - 50,000 max particles, high emission rate
         *   - Gentle downward gravity (~0.8 m/s²)
         *   - High wind influence (1.0) for natural drift
         *   - Low drag for floating feel
         *   - Noise turbulence for chaotic fluttering
         *   - Ground collision enabled (Y = 0)
         *   - Soft particles enabled
         *   - White, small, alpha-blended billboards
         *   - Long lifetime (8–15 s)
         *
         * @param[out] sys  ParticleSystem to configure.
         */
        static void ApplySnowfall(ParticleSystem& sys);

        /**
         * @brief Configure a ParticleSystem as a blizzard / heavy snow emitter.
         *
         * Similar to Snowfall but with:
         *   - 100,000 max particles
         *   - More aggressive wind influence
         *   - Higher noise turbulence
         *   - Smaller, faster particles
         *
         * @param[out] sys  ParticleSystem to configure.
         */
        static void ApplyBlizzard(ParticleSystem& sys);

        /**
         * @brief Configure a ParticleSystem as a standard smoke emitter.
         *
         * Settings:
         *   - Cone shape pointing upward
         *   - 75 particles/sec, lifetime 3–5 s
         *   - Slight upward buoyancy, moderate drag (0.7)
         *   - Noise turbulence for organic billowing
         *   - Semi-transparent dark gray fading to transparent
         *   - Size expands 2.5× over lifetime
         *   - Alpha blend, soft particles, billboard render mode
         *
         * @param[out] sys  ParticleSystem to configure.
         */
        static void ApplySmoke(ParticleSystem& sys);

        /**
         * @brief Configure a ParticleSystem as a thick/heavy smoke emitter.
         *
         * Similar to Smoke but with:
         *   - GPU compute path (UseGPU = true)
         *   - 175 particles/sec, higher opacity
         *   - Slower rise, more drag, larger particles
         *   - Stronger noise for chaotic billowing
         *
         * @param[out] sys  ParticleSystem to configure.
         */
        static void ApplyThickSmoke(ParticleSystem& sys);

        /**
         * @brief Configure a ParticleSystem as a light/wispy smoke emitter.
         *
         * Similar to Smoke but with:
         *   - Lower spawn rate (30/sec), longer lifetime (5–8 s)
         *   - Faster rise, less drag, higher wind influence
         *   - Lower opacity, smaller particles
         *   - Additive blend for ethereal/glowing appearance
         *
         * @param[out] sys  ParticleSystem to configure.
         */
        static void ApplyLightSmoke(ParticleSystem& sys);
    };
} // namespace OloEngine
