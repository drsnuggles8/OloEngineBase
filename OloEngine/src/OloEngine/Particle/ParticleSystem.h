#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Particle/ParticlePool.h"
#include "OloEngine/Particle/ParticleEmitter.h"
#include "OloEngine/Particle/ParticleModules.h"
#include "OloEngine/Particle/ParticleCollision.h"
#include "OloEngine/Particle/ParticleTrail.h"
#include "OloEngine/Particle/SubEmitter.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    enum class ParticleSpace : u8
    {
        Local = 0,
        World = 1
    };

    class ParticleSystem
    {
    public:
        explicit ParticleSystem(u32 maxParticles = 1000);

        void Update(f32 dt, const glm::vec3& emitterPosition);
        void Reset();

        [[nodiscard]] u32 GetAliveCount() const { return m_Pool.GetAliveCount(); }
        [[nodiscard]] u32 GetMaxParticles() const { return m_Pool.GetMaxParticles(); }
        void SetMaxParticles(u32 maxParticles);

        [[nodiscard]] const ParticlePool& GetPool() const { return m_Pool; }
        [[nodiscard]] ParticlePool& GetPool() { return m_Pool; }

        [[nodiscard]] const ParticleTrailData& GetTrailData() const { return m_TrailData; }

        // Collect sub-emitter triggers that fired this frame
        [[nodiscard]] const std::vector<SubEmitterTriggerInfo>& GetPendingTriggers() const { return m_PendingTriggers; }
        void ClearPendingTriggers() { m_PendingTriggers.clear(); }

        // Set Jolt scene for raycast collision (optional, set by Scene during runtime)
        void SetJoltScene(JoltScene* scene) { m_JoltScene = scene; }

        // Public settings
        bool Playing = true;
        bool Looping = true;
        f32 Duration = 5.0f;
        f32 PlaybackSpeed = 1.0f;
        ParticleSpace SimulationSpace = ParticleSpace::World;

        // LOD settings
        f32 LODDistance1 = 50.0f;  // Distance at which spawn rate drops to 50%
        f32 LODDistance2 = 100.0f; // Distance at which spawn rate drops to 25%
        f32 LODMaxDistance = 200.0f; // Distance beyond which particles stop spawning

        // Sub-systems (Phase 1)
        ParticleEmitter Emitter;
        ModuleColorOverLifetime ColorModule;
        ModuleSizeOverLifetime SizeModule;
        ModuleVelocityOverLifetime VelocityModule;
        ModuleRotationOverLifetime RotationModule;
        ModuleGravity GravityModule;
        ModuleDrag DragModule;
        ModuleNoise NoiseModule;

        // Sub-systems (Phase 2)
        ModuleCollision CollisionModule;
        ModuleForceField ForceFieldModule;
        ModuleTrail TrailModule;
        ModuleSubEmitter SubEmitterModule;

    private:
        ParticlePool m_Pool;
        ParticleTrailData m_TrailData;
        std::vector<SubEmitterTriggerInfo> m_PendingTriggers;
        JoltScene* m_JoltScene = nullptr;
        f32 m_Time = 0.0f;
        f32 m_LODSpawnRateMultiplier = 1.0f;
    };
}
