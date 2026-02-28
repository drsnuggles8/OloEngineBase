#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Particle/ParticlePool.h"
#include "OloEngine/Particle/ParticleEmitter.h"
#include "OloEngine/Particle/ParticleModules.h"
#include "OloEngine/Particle/ParticleCollision.h"
#include "OloEngine/Particle/ParticleTrail.h"
#include "OloEngine/Particle/SubEmitter.h"
#include "OloEngine/Particle/GPUParticleSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    enum class ParticleSpace : u8
    {
        Local = 0,
        World = 1
    };

    enum class ParticleBlendMode : u8
    {
        Alpha = 0, // Standard alpha blending (requires depth sorting)
        Additive,  // Additive blending — fire, sparks, glows (no sorting needed)
        PremultipliedAlpha
    };

    enum class ParticleRenderMode : u8
    {
        Billboard = 0,      // Camera-facing quads
        StretchedBillboard, // Stretched along velocity
        Mesh                // User-specified mesh per particle
    };

    class ParticleSystem
    {
      public:
        explicit ParticleSystem(u32 maxParticles = 1000);

        // Copy constructor: deep-copies all state but rewires OnSwapCallback to this instance
        ParticleSystem(const ParticleSystem& other);
        ParticleSystem& operator=(const ParticleSystem& other);
        ParticleSystem(ParticleSystem&&) noexcept;
        ParticleSystem& operator=(ParticleSystem&&) noexcept;

        void Update(f32 dt, const glm::vec3& emitterPosition, const glm::vec3& parentVelocity = glm::vec3(0.0f), const glm::quat& emitterRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        void Reset();

        // Compute LOD spawn rate multiplier based on camera distance to emitter
        void UpdateLOD(const glm::vec3& cameraPosition, const glm::vec3& emitterPosition);

        // Directly set the LOD spawn rate multiplier (0 = no emission, 1 = full rate)
        void SetLODSpawnRateMultiplier(f32 multiplier)
        {
            m_LODSpawnRateMultiplier = multiplier;
        }

        // Sort alive particles back-to-front relative to camera for correct alpha blending.
        // Populates an internal index array; retrieve with GetSortedIndices().
        void SortByDepth(const glm::vec3& cameraPosition);

        // Get depth-sorted index array (valid after SortByDepth(); size == GetAliveCount()).
        [[nodiscard]] const std::vector<u32>& GetSortedIndices() const
        {
            return m_SortedIndices;
        }

        // Get the emitter world position (used for Local space rendering transform)
        [[nodiscard]] const glm::vec3& GetEmitterPosition() const
        {
            return m_EmitterPosition;
        }

        [[nodiscard]] u32 GetAliveCount() const
        {
            return m_Pool.GetAliveCount();
        }
        [[nodiscard]] u32 GetMaxParticles() const
        {
            return m_Pool.GetMaxParticles();
        }
        void SetMaxParticles(u32 maxParticles);

        [[nodiscard]] const ParticlePool& GetPool() const
        {
            return m_Pool;
        }
        [[nodiscard]] ParticlePool& GetPool()
        {
            return m_Pool;
        }

        [[nodiscard]] const ParticleTrailData& GetTrailData() const
        {
            return m_TrailData;
        }

        // GPU particle system accessor (valid when UseGPU is true and system has been updated)
        [[nodiscard]] GPUParticleSystem* GetGPUSystem() const
        {
            return m_GPUSystem.get();
        }

        // Collect sub-emitter triggers that fired this frame
        [[nodiscard]] const std::vector<SubEmitterTriggerInfo>& GetPendingTriggers() const
        {
            return m_PendingTriggers;
        }
        void ClearPendingTriggers()
        {
            m_PendingTriggers.clear();
        }

        // Set Jolt scene for raycast collision (optional, set by Scene during runtime)
        void SetJoltScene(JoltScene* scene)
        {
            m_JoltScene = scene;
        }

        // Public settings
        bool Playing = true;
        bool Looping = true;
        f32 Duration = 5.0f;
        f32 PlaybackSpeed = 1.0f;
        f32 WarmUpTime = 0.0f; // Pre-simulate this many seconds on first play
        ParticleSpace SimulationSpace = ParticleSpace::World;

        // Rendering settings
        ParticleBlendMode BlendMode = ParticleBlendMode::Alpha;
        ParticleRenderMode RenderMode = ParticleRenderMode::Billboard;
        bool DepthSortEnabled = true; // Sort particles back-to-front (not needed for Additive)

        // GPU compute simulation (requires Billboard render mode)
        bool UseGPU = false;

        // GPU wind & turbulence settings
        f32 WindInfluence = 1.0f;     // 0–1 multiplier on wind field velocity
        f32 GPUNoiseStrength = 0.0f;  // Per-particle noise turbulence amplitude
        f32 GPUNoiseFrequency = 1.0f; // Spatial frequency of noise turbulence

        // GPU ground collision
        bool GPUGroundCollision = false;
        f32 GPUGroundY = 0.0f;
        f32 GPUCollisionBounce = 0.3f;
        f32 GPUCollisionFriction = 0.8f;

        // Soft particles — alpha-fade near opaque surfaces using scene depth buffer
        bool SoftParticlesEnabled = false;
        f32 SoftParticleDistance = 1.0f; // Distance over which particles fade (world units)

        // Velocity inheritance — adds parent entity velocity to spawned particles
        f32 VelocityInheritance = 0.0f; // 0 = none, 1 = full parent velocity

        // LOD settings
        f32 LODDistance1 = 50.0f;    // Distance at which spawn rate starts to drop
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
        std::vector<ModuleForceField> ForceFields;
        ModuleTrail TrailModule;
        ModuleSubEmitter SubEmitterModule;

        // Phase 3 modules
        ModuleTextureSheetAnimation TextureSheetModule;

      private:
        ParticlePool m_Pool;
        ParticleTrailData m_TrailData;
        Scope<GPUParticleSystem> m_GPUSystem;
        std::vector<SubEmitterTriggerInfo> m_PendingTriggers;
        std::vector<CollisionEvent> m_CollisionEvents;
        std::vector<u32> m_SortedIndices;
        std::vector<f32> m_SortDistances;
        JoltScene* m_JoltScene = nullptr;
        glm::vec3 m_EmitterPosition{ 0.0f };
        glm::vec3 m_ParentVelocity{ 0.0f };
        f32 m_Time = 0.0f;
        f32 m_LODSpawnRateMultiplier = 1.0f;
        bool m_HasWarmedUp = false;

        void ProcessSubEmitterTriggers();
        void UpdateInternal(f32 dt, const glm::vec3& emitterPosition, const glm::vec3& parentVelocity, const glm::quat& emitterRotation);
        void UpdateGPU(f32 dt, const glm::vec3& emitterPosition, const glm::quat& emitterRotation);
    };
} // namespace OloEngine
