#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Particle/EmissionShape.h"
#include "OloEngine/Particle/ParticlePool.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace OloEngine
{
    struct BurstEntry
    {
        f32 Time = 0.0f; // Time offset within loop
        u32 Count = 10;
        f32 Probability = 1.0f; // 0..1
    };

    class ParticleEmitter
    {
      public:
        // Emission settings
        f32 RateOverTime = 10.0f; // Particles per second
        f32 InitialSpeed = { 5.0f };
        f32 SpeedVariance = 0.0f;
        f32 LifetimeMin = 1.0f;
        f32 LifetimeMax = 2.0f;
        f32 InitialSize = 1.0f;
        f32 SizeVariance = 0.0f;
        f32 InitialRotation = 0.0f;
        f32 RotationVariance = 0.0f;
        glm::vec4 InitialColor{ 1.0f, 1.0f, 1.0f, 1.0f };

        EmissionShape Shape;

        std::vector<BurstEntry> Bursts;

        // Emit particles for this frame, returns number emitted
        u32 Update(f32 dt, ParticlePool& pool, const glm::vec3& emitterPosition, f32 rateMultiplier = 1.0f, const glm::quat& emitterRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

        void Reset();

      private:
        void InitializeParticle(u32 index, ParticlePool& pool, const glm::vec3& emitterPosition, const glm::quat& emitterRotation);

        f32 m_EmitAccumulator = 0.0f;
        f32 m_LoopTime = 0.0f;
        u32 m_NextBurstIndex = 0;
    };
} // namespace OloEngine
