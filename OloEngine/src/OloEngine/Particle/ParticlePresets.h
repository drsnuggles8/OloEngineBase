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
    };
} // namespace OloEngine
