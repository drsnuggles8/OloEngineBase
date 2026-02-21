#include "OloEnginePCH.h"
#include "ParticleSystem.h"
#include "OloEngine/Particle/GPUParticleData.h"
#include "OloEngine/Task/Task.h"

#include <algorithm>
#include <numeric>

namespace OloEngine
{
    ParticleSystem::ParticleSystem(u32 maxParticles)
        : m_Pool(maxParticles)
    {
        m_TrailData.Resize(maxParticles, TrailModule.MaxTrailPoints);

        // Wire up swap callback so trail data stays in sync when particles die
        m_Pool.m_OnSwapCallback = [this](u32 a, u32 b)
        { m_TrailData.SwapParticles(a, b); };
    }

    ParticleSystem::ParticleSystem(const ParticleSystem& other)
        : m_Pool(other.m_Pool), m_TrailData(other.m_TrailData), m_PendingTriggers(other.m_PendingTriggers), m_SortedIndices(other.m_SortedIndices), m_SortDistances(other.m_SortDistances), m_JoltScene(other.m_JoltScene), m_EmitterPosition(other.m_EmitterPosition), m_ParentVelocity(other.m_ParentVelocity), m_Time(other.m_Time), m_LODSpawnRateMultiplier(other.m_LODSpawnRateMultiplier), m_HasWarmedUp(other.m_HasWarmedUp), Playing(other.Playing), Looping(other.Looping), Duration(other.Duration), PlaybackSpeed(other.PlaybackSpeed), WarmUpTime(other.WarmUpTime), SimulationSpace(other.SimulationSpace), BlendMode(other.BlendMode), RenderMode(other.RenderMode), DepthSortEnabled(other.DepthSortEnabled), UseGPU(other.UseGPU), SoftParticlesEnabled(other.SoftParticlesEnabled), SoftParticleDistance(other.SoftParticleDistance), VelocityInheritance(other.VelocityInheritance), LODDistance1(other.LODDistance1), LODMaxDistance(other.LODMaxDistance), Emitter(other.Emitter), ColorModule(other.ColorModule), SizeModule(other.SizeModule), VelocityModule(other.VelocityModule), RotationModule(other.RotationModule), GravityModule(other.GravityModule), DragModule(other.DragModule), NoiseModule(other.NoiseModule), CollisionModule(other.CollisionModule), ForceFields(other.ForceFields), TrailModule(other.TrailModule), SubEmitterModule(other.SubEmitterModule), TextureSheetModule(other.TextureSheetModule)
    {
        // Rewire callback to THIS instance's trail data
        m_Pool.m_OnSwapCallback = [this](u32 a, u32 b)
        { m_TrailData.SwapParticles(a, b); };
    }

    ParticleSystem& ParticleSystem::operator=(const ParticleSystem& other)
    {
        if (this == &other)
        {
            return *this;
        }

        // Copy all public settings
        Playing = other.Playing;
        Looping = other.Looping;
        Duration = other.Duration;
        PlaybackSpeed = other.PlaybackSpeed;
        WarmUpTime = other.WarmUpTime;
        SimulationSpace = other.SimulationSpace;
        BlendMode = other.BlendMode;
        RenderMode = other.RenderMode;
        DepthSortEnabled = other.DepthSortEnabled;
        UseGPU = other.UseGPU;
        SoftParticlesEnabled = other.SoftParticlesEnabled;
        SoftParticleDistance = other.SoftParticleDistance;
        VelocityInheritance = other.VelocityInheritance;
        LODDistance1 = other.LODDistance1;
        LODMaxDistance = other.LODMaxDistance;
        Emitter = other.Emitter;
        ColorModule = other.ColorModule;
        SizeModule = other.SizeModule;
        VelocityModule = other.VelocityModule;
        RotationModule = other.RotationModule;
        GravityModule = other.GravityModule;
        DragModule = other.DragModule;
        NoiseModule = other.NoiseModule;
        CollisionModule = other.CollisionModule;
        ForceFields = other.ForceFields;
        TrailModule = other.TrailModule;
        SubEmitterModule = other.SubEmitterModule;
        TextureSheetModule = other.TextureSheetModule;

        // Copy private state
        m_Pool = other.m_Pool;
        m_TrailData = other.m_TrailData;
        m_PendingTriggers = other.m_PendingTriggers;
        m_SortedIndices = other.m_SortedIndices;
        m_SortDistances = other.m_SortDistances;
        m_JoltScene = other.m_JoltScene;
        m_EmitterPosition = other.m_EmitterPosition;
        m_ParentVelocity = other.m_ParentVelocity;
        m_Time = other.m_Time;
        m_LODSpawnRateMultiplier = other.m_LODSpawnRateMultiplier;
        m_HasWarmedUp = other.m_HasWarmedUp;

        // GPU system is not copied — it will be lazily initialized on next Update
        m_GPUSystem.reset();

        // Rewire callback to THIS instance
        m_Pool.m_OnSwapCallback = [this](u32 a, u32 b)
        { m_TrailData.SwapParticles(a, b); };

        return *this;
    }

    ParticleSystem::ParticleSystem(ParticleSystem&& other) noexcept
        : m_Pool(std::move(other.m_Pool)), m_TrailData(std::move(other.m_TrailData)), m_GPUSystem(std::move(other.m_GPUSystem)), m_PendingTriggers(std::move(other.m_PendingTriggers)), m_SortedIndices(std::move(other.m_SortedIndices)), m_SortDistances(std::move(other.m_SortDistances)), m_JoltScene(other.m_JoltScene), m_EmitterPosition(other.m_EmitterPosition), m_ParentVelocity(other.m_ParentVelocity), m_Time(other.m_Time), m_LODSpawnRateMultiplier(other.m_LODSpawnRateMultiplier), m_HasWarmedUp(other.m_HasWarmedUp), Playing(other.Playing), Looping(other.Looping), Duration(other.Duration), PlaybackSpeed(other.PlaybackSpeed), WarmUpTime(other.WarmUpTime), SimulationSpace(other.SimulationSpace), BlendMode(other.BlendMode), RenderMode(other.RenderMode), DepthSortEnabled(other.DepthSortEnabled), UseGPU(other.UseGPU), SoftParticlesEnabled(other.SoftParticlesEnabled), SoftParticleDistance(other.SoftParticleDistance), VelocityInheritance(other.VelocityInheritance), LODDistance1(other.LODDistance1), LODMaxDistance(other.LODMaxDistance), Emitter(std::move(other.Emitter)), ColorModule(other.ColorModule), SizeModule(other.SizeModule), VelocityModule(other.VelocityModule), RotationModule(other.RotationModule), GravityModule(other.GravityModule), DragModule(other.DragModule), NoiseModule(other.NoiseModule), CollisionModule(other.CollisionModule), ForceFields(std::move(other.ForceFields)), TrailModule(other.TrailModule), SubEmitterModule(std::move(other.SubEmitterModule)), TextureSheetModule(other.TextureSheetModule)
    {
        // Rewire callback to THIS instance
        m_Pool.m_OnSwapCallback = [this](u32 a, u32 b)
        { m_TrailData.SwapParticles(a, b); };
    }

    ParticleSystem& ParticleSystem::operator=(ParticleSystem&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        Playing = other.Playing;
        Looping = other.Looping;
        Duration = other.Duration;
        PlaybackSpeed = other.PlaybackSpeed;
        WarmUpTime = other.WarmUpTime;
        SimulationSpace = other.SimulationSpace;
        BlendMode = other.BlendMode;
        RenderMode = other.RenderMode;
        DepthSortEnabled = other.DepthSortEnabled;
        UseGPU = other.UseGPU;
        SoftParticlesEnabled = other.SoftParticlesEnabled;
        SoftParticleDistance = other.SoftParticleDistance;
        VelocityInheritance = other.VelocityInheritance;
        LODDistance1 = other.LODDistance1;
        LODMaxDistance = other.LODMaxDistance;
        Emitter = std::move(other.Emitter);
        ColorModule = other.ColorModule;
        SizeModule = other.SizeModule;
        VelocityModule = other.VelocityModule;
        RotationModule = other.RotationModule;
        GravityModule = other.GravityModule;
        DragModule = other.DragModule;
        NoiseModule = other.NoiseModule;
        CollisionModule = other.CollisionModule;
        ForceFields = std::move(other.ForceFields);
        TrailModule = other.TrailModule;
        SubEmitterModule = std::move(other.SubEmitterModule);
        TextureSheetModule = other.TextureSheetModule;

        m_Pool = std::move(other.m_Pool);
        m_TrailData = std::move(other.m_TrailData);
        m_GPUSystem = std::move(other.m_GPUSystem);
        m_PendingTriggers = std::move(other.m_PendingTriggers);
        m_SortedIndices = std::move(other.m_SortedIndices);
        m_SortDistances = std::move(other.m_SortDistances);
        m_JoltScene = other.m_JoltScene;
        m_EmitterPosition = other.m_EmitterPosition;
        m_ParentVelocity = other.m_ParentVelocity;
        m_Time = other.m_Time;
        m_LODSpawnRateMultiplier = other.m_LODSpawnRateMultiplier;
        m_HasWarmedUp = other.m_HasWarmedUp;

        // Rewire callback to THIS instance
        m_Pool.m_OnSwapCallback = [this](u32 a, u32 b)
        { m_TrailData.SwapParticles(a, b); };

        return *this;
    }

    void ParticleSystem::SetMaxParticles(u32 maxParticles)
    {
        OLO_PROFILE_FUNCTION();

        m_Pool.Resize(maxParticles);
        m_TrailData.Resize(maxParticles, TrailModule.MaxTrailPoints);
    }

    void ParticleSystem::UpdateLOD(const glm::vec3& cameraPosition, const glm::vec3& emitterPosition)
    {
        OLO_PROFILE_FUNCTION();

        f32 dist = glm::length(cameraPosition - emitterPosition);
        if (dist >= LODMaxDistance)
        {
            m_LODSpawnRateMultiplier = 0.0f;
        }
        else if (dist <= LODDistance1 || LODMaxDistance <= LODDistance1)
        {
            m_LODSpawnRateMultiplier = 1.0f;
        }
        else
        {
            // Smooth linear falloff between LODDistance1 (full rate) and LODMaxDistance (zero)
            m_LODSpawnRateMultiplier = (LODMaxDistance - dist) / (LODMaxDistance - LODDistance1);
        }
    }

    void ParticleSystem::Update(f32 dt, const glm::vec3& emitterPosition, const glm::vec3& parentVelocity, const glm::quat& emitterRotation)
    {
        OLO_PROFILE_FUNCTION();

        if (!Playing)
        {
            return;
        }

        // Warm-up: pre-simulate iteratively to avoid stack overflow risk
        if (!m_HasWarmedUp && WarmUpTime > 0.0f)
        {
            m_HasWarmedUp = true;
            constexpr f32 warmUpStep = 1.0f / 60.0f;
            f32 remaining = WarmUpTime;
            while (remaining > 0.0f)
            {
                f32 step = std::min(remaining, warmUpStep);
                UpdateInternal(step, emitterPosition, parentVelocity, emitterRotation);
                remaining -= step;
            }
            return;
        }
        m_HasWarmedUp = true;

        UpdateInternal(dt, emitterPosition, parentVelocity, emitterRotation);
    }

    void ParticleSystem::UpdateInternal(f32 dt, const glm::vec3& emitterPosition, const glm::vec3& parentVelocity, const glm::quat& emitterRotation)
    {
        OLO_PROFILE_FUNCTION();

        f32 scaledDt = dt * PlaybackSpeed;
        m_Time += scaledDt;
        m_EmitterPosition = emitterPosition;
        m_ParentVelocity = parentVelocity;

        // Check duration
        if (!Looping && m_Time >= Duration)
        {
            Playing = false;
            return;
        }

        if (Looping && m_Time >= Duration)
        {
            m_Time -= Duration;
            Emitter.Reset();
        }

        // GPU path: emit on CPU, simulate on GPU
        if (UseGPU)
        {
            UpdateGPU(scaledDt, emitterPosition, emitterRotation);
            m_PendingTriggers.clear();
            return;
        }

        // CPU path — continued below
        // Determine emission position based on simulation space
        glm::vec3 emitPos = (SimulationSpace == ParticleSpace::Local) ? glm::vec3(0.0f) : emitterPosition;

        // Clear pending sub-emitter triggers from previous frame
        m_PendingTriggers.clear();

        // 1. Emit new particles (with LOD rate multiplier passed as parameter)
        u32 prevAlive = m_Pool.GetAliveCount();
        Emitter.Update(scaledDt, m_Pool, emitPos, m_LODSpawnRateMultiplier, emitterRotation);
        u32 newAlive = m_Pool.GetAliveCount();

        // Apply velocity inheritance: add parent entity velocity to newly spawned particles
        if (VelocityInheritance != 0.0f && newAlive > prevAlive)
        {
            glm::vec3 inherited = m_ParentVelocity * VelocityInheritance;
            for (u32 i = prevAlive; i < newAlive; ++i)
            {
                m_Pool.m_Velocities[i] += inherited;
                m_Pool.m_InitialVelocities[i] += inherited;
            }
        }

        // Fire OnBirth sub-emitter triggers for newly spawned particles
        if (SubEmitterModule.Enabled && newAlive > prevAlive)
        {
            for (const auto& entry : SubEmitterModule.Entries)
            {
                if (entry.Trigger == SubEmitterEvent::OnBirth)
                {
                    for (u32 i = prevAlive; i < newAlive; ++i)
                    {
                        SubEmitterTriggerInfo trigger;
                        trigger.Position = m_Pool.m_Positions[i];
                        trigger.Velocity = entry.InheritVelocity ? m_Pool.m_Velocities[i] * entry.InheritVelocityScale : glm::vec3(0.0f);
                        trigger.Event = SubEmitterEvent::OnBirth;
                        trigger.ChildSystemIndex = entry.ChildSystemIndex;
                        trigger.EmitCount = entry.EmitCount;
                        m_PendingTriggers.push_back(trigger);
                    }
                }
            }
        }

        // Initialize trails for newly spawned particles
        if (TrailModule.Enabled)
        {
            for (u32 i = prevAlive; i < newAlive; ++i)
            {
                m_TrailData.ClearTrail(i);
            }
        }

        // 2. Apply modules — independent modules run concurrently with the velocity chain
        // Color, Size, and Rotation only write m_Colors/m_Sizes/m_Rotations respectively,
        // so they are safe to run in parallel with the velocity chain (Gravity→Drag→Noise→Velocity).
        bool useParallelModules = m_Pool.GetAliveCount() >= 256 && (ColorModule.Enabled || SizeModule.Enabled || RotationModule.Enabled);

        Tasks::TTask<void> colorTask;
        Tasks::TTask<void> sizeTask;
        Tasks::TTask<void> rotationTask;

        if (useParallelModules)
        {
            if (ColorModule.Enabled)
                colorTask = Tasks::Launch("ColorModule", [this]()
                                          { ColorModule.Apply(m_Pool); });
            if (SizeModule.Enabled)
                sizeTask = Tasks::Launch("SizeModule", [this]()
                                         { SizeModule.Apply(m_Pool); });
            if (RotationModule.Enabled)
                rotationTask = Tasks::Launch("RotModule", [this, scaledDt]()
                                             { RotationModule.Apply(scaledDt, m_Pool); });
        }

        // Velocity chain (must be sequential — all write m_Velocities)
        GravityModule.Apply(scaledDt, m_Pool);
        DragModule.Apply(scaledDt, m_Pool);
        NoiseModule.Apply(scaledDt, m_Time, m_Pool);
        VelocityModule.Apply(scaledDt, m_Pool);

        if (useParallelModules)
        {
            // Wait for independent modules to finish before proceeding
            if (colorTask.IsValid())
                colorTask.Wait();
            if (sizeTask.IsValid())
                sizeTask.Wait();
            if (rotationTask.IsValid())
                rotationTask.Wait();
        }
        else
        {
            // Single-threaded fallback for small particle counts
            RotationModule.Apply(scaledDt, m_Pool);
            ColorModule.Apply(m_Pool);
            SizeModule.Apply(m_Pool);
        }

        // 3. Apply Phase 2 modules
        for (auto& forceField : ForceFields)
        {
            forceField.Apply(scaledDt, m_Pool);
        }

        // Collision: use raycasts if Jolt scene available and mode is SceneRaycast
        m_CollisionEvents.clear();
        if (CollisionModule.Enabled)
        {
            auto* eventsPtr = SubEmitterModule.Enabled ? &m_CollisionEvents : nullptr;
            if (CollisionModule.Mode == CollisionMode::SceneRaycast && m_JoltScene)
            {
                CollisionModule.ApplyWithRaycasts(scaledDt, m_Pool, m_JoltScene, eventsPtr);
            }
            else
            {
                CollisionModule.Apply(scaledDt, m_Pool, eventsPtr);
            }

            // Fire OnCollision sub-emitter triggers
            if (SubEmitterModule.Enabled && !m_CollisionEvents.empty())
            {
                for (const auto& entry : SubEmitterModule.Entries)
                {
                    if (entry.Trigger == SubEmitterEvent::OnCollision)
                    {
                        for (const auto& event : m_CollisionEvents)
                        {
                            SubEmitterTriggerInfo trigger;
                            trigger.Position = event.Position;
                            trigger.Velocity = entry.InheritVelocity ? event.Velocity * entry.InheritVelocityScale : glm::vec3(0.0f);
                            trigger.Event = SubEmitterEvent::OnCollision;
                            trigger.ChildSystemIndex = entry.ChildSystemIndex;
                            trigger.EmitCount = entry.EmitCount;
                            m_PendingTriggers.push_back(trigger);
                        }
                    }
                }
            }
        }

        // 4. Integrate positions
        u32 count = m_Pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            m_Pool.m_Positions[i] += m_Pool.m_Velocities[i] * scaledDt;
        }

        // 5. Record trail points after position integration
        if (TrailModule.Enabled)
        {
            count = m_Pool.GetAliveCount();
            for (u32 i = 0; i < count; ++i)
            {
                m_TrailData.RecordPoint(i, m_Pool.m_Positions[i], m_Pool.m_Sizes[i], m_Pool.m_Colors[i], TrailModule.MinVertexDistance);
            }
            m_TrailData.AgePoints(scaledDt, TrailModule.TrailLifetime);
        }

        // 6. Collect death triggers before killing expired particles
        if (SubEmitterModule.Enabled)
        {
            count = m_Pool.GetAliveCount();
            for (u32 i = 0; i < count; ++i)
            {
                if (m_Pool.m_Lifetimes[i] - scaledDt <= 0.0f)
                {
                    for (const auto& entry : SubEmitterModule.Entries)
                    {
                        if (entry.Trigger == SubEmitterEvent::OnDeath)
                        {
                            SubEmitterTriggerInfo trigger;
                            trigger.Position = m_Pool.m_Positions[i];
                            trigger.Velocity = entry.InheritVelocity ? m_Pool.m_Velocities[i] * entry.InheritVelocityScale : glm::vec3(0.0f);
                            trigger.Event = SubEmitterEvent::OnDeath;
                            trigger.ChildSystemIndex = entry.ChildSystemIndex;
                            trigger.EmitCount = entry.EmitCount;
                            m_PendingTriggers.push_back(trigger);
                        }
                    }
                }
            }
        }

        // Kill expired particles (m_OnSwapCallback keeps trail data in sync)
        m_Pool.UpdateLifetimes(scaledDt);

        // 7. Spawn particles from sub-emitter triggers
        ProcessSubEmitterTriggers();
    }

    void ParticleSystem::ProcessSubEmitterTriggers()
    {
        OLO_PROFILE_FUNCTION();

        if (!SubEmitterModule.Enabled || m_PendingTriggers.empty())
        {
            return;
        }

        // Triggers with ChildSystemIndex >= 0 are handled by Scene (emitted into child systems).
        // Only triggers with ChildSystemIndex == -1 fall back to the legacy parent-pool behavior.
        auto& rng = RandomUtils::GetGlobalRandom();

        for (const auto& trigger : m_PendingTriggers)
        {
            // Skip triggers destined for child systems — Scene will process them
            if (trigger.ChildSystemIndex >= 0)
            {
                continue;
            }

            u32 firstSlot = m_Pool.GetAliveCount();
            u32 emitted = m_Pool.Emit(trigger.EmitCount);

            for (u32 i = 0; i < emitted; ++i)
            {
                u32 idx = firstSlot + i;
                m_Pool.m_Positions[idx] = trigger.Position;

                // Random direction + inherited velocity
                glm::vec3 randomVec(
                    rng.GetFloat32InRange(-1.0f, 1.0f),
                    rng.GetFloat32InRange(-1.0f, 1.0f),
                    rng.GetFloat32InRange(-1.0f, 1.0f));
                f32 randomLen = glm::length(randomVec);
                glm::vec3 dir = (randomLen > 0.0001f) ? randomVec / randomLen : glm::vec3(0.0f, 1.0f, 0.0f);
                f32 speed = Emitter.InitialSpeed + rng.GetFloat32InRange(-Emitter.SpeedVariance, Emitter.SpeedVariance);
                glm::vec3 velocity = dir * std::max(speed, 0.0f) + trigger.Velocity;
                m_Pool.m_Velocities[idx] = velocity;
                m_Pool.m_InitialVelocities[idx] = velocity;

                m_Pool.m_Colors[idx] = Emitter.InitialColor;
                m_Pool.m_InitialColors[idx] = Emitter.InitialColor;

                f32 size = Emitter.InitialSize + rng.GetFloat32InRange(-Emitter.SizeVariance, Emitter.SizeVariance);
                m_Pool.m_Sizes[idx] = size;
                m_Pool.m_InitialSizes[idx] = size;
                m_Pool.m_Rotations[idx] = Emitter.InitialRotation + rng.GetFloat32InRange(-Emitter.RotationVariance, Emitter.RotationVariance);

                f32 lifetime = rng.GetFloat32InRange(Emitter.LifetimeMin, Emitter.LifetimeMax);
                m_Pool.m_Lifetimes[idx] = lifetime;
                m_Pool.m_MaxLifetimes[idx] = lifetime;

                if (TrailModule.Enabled)
                {
                    m_TrailData.ClearTrail(idx);
                }
            }
        }
    }

    void ParticleSystem::SortByDepth(const glm::vec3& cameraPosition)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = m_Pool.GetAliveCount();
        if (m_SortedIndices.size() != count)
        {
            m_SortedIndices.resize(count);
            std::iota(m_SortedIndices.begin(), m_SortedIndices.end(), 0u);
        }

        // Precompute squared distances to avoid recomputing in inner loop
        m_SortDistances.resize(count);
        for (u32 i = 0; i < count; ++i)
        {
            glm::vec3 diff = m_Pool.m_Positions[i] - cameraPosition;
            m_SortDistances[i] = glm::dot(diff, diff);
        }

        // Insertion sort: O(n) for nearly-sorted data (particles move little between frames)
        for (u32 i = 1; i < count; ++i)
        {
            u32 key = m_SortedIndices[i];
            f32 keyDist = m_SortDistances[key];
            u32 j = i;
            while (j > 0 && m_SortDistances[m_SortedIndices[j - 1]] < keyDist) // Back-to-front
            {
                m_SortedIndices[j] = m_SortedIndices[j - 1];
                --j;
            }
            m_SortedIndices[j] = key;
        }
    }

    void ParticleSystem::UpdateGPU(f32 scaledDt, const glm::vec3& emitterPosition, const glm::quat& emitterRotation)
    {
        OLO_PROFILE_FUNCTION();

        // Lazy-initialize GPU system
        if (!m_GPUSystem)
        {
            m_GPUSystem = CreateScope<GPUParticleSystem>(m_Pool.GetMaxParticles());
        }

        // Use a temporary CPU pool to emit particles through the existing emitter
        // We reuse the main pool but snapshot the alive count to detect new particles
        glm::vec3 emitPos = (SimulationSpace == ParticleSpace::Local) ? glm::vec3(0.0f) : emitterPosition;
        u32 prevAlive = m_Pool.GetAliveCount();
        Emitter.Update(scaledDt, m_Pool, emitPos, m_LODSpawnRateMultiplier, emitterRotation);
        u32 newAlive = m_Pool.GetAliveCount();
        u32 newCount = newAlive - prevAlive;

        // Convert newly emitted CPU particles to GPU format and upload
        if (newCount > 0)
        {
            std::vector<GPUParticle> gpuParticles(newCount);
            for (u32 i = 0; i < newCount; ++i)
            {
                u32 idx = prevAlive + i;
                auto& gp = gpuParticles[i];
                gp.PositionLifetime = glm::vec4(m_Pool.m_Positions[idx], m_Pool.m_Lifetimes[idx]);
                gp.VelocityMaxLifetime = glm::vec4(m_Pool.m_Velocities[idx], m_Pool.m_MaxLifetimes[idx]);
                gp.Color = m_Pool.m_Colors[idx];
                gp.InitialColor = m_Pool.m_InitialColors[idx];
                gp.InitialVelocitySize = glm::vec4(m_Pool.m_InitialVelocities[idx], m_Pool.m_Sizes[idx]);
                gp.Misc = glm::vec4(m_Pool.m_InitialSizes[idx], m_Pool.m_Rotations[idx], 1.0f, 0.0f);
            }
            m_GPUSystem->EmitParticles(gpuParticles);

            // Clear CPU pool — GPU owns all alive particles; the CPU pool is only
            // used as a staging area for the emitter each frame.
            m_Pool.Resize(m_Pool.GetMaxParticles());
        }

        // Fill simulation params from current module settings
        GPUSimParams params;
        params.DeltaTime = scaledDt;
        params.Gravity = GravityModule.Gravity;
        params.DragCoefficient = DragModule.DragCoefficient;
        params.MaxParticles = m_GPUSystem->GetMaxParticles();
        params.EnableGravity = GravityModule.Enabled ? 1 : 0;
        params.EnableDrag = DragModule.Enabled ? 1 : 0;

        // Dispatch GPU pipeline: simulate → compact → build indirect draw
        m_GPUSystem->Simulate(params);
        m_GPUSystem->Compact();
        m_GPUSystem->PrepareIndirectDraw();
    }

    void ParticleSystem::Reset()
    {
        OLO_PROFILE_FUNCTION();

        m_Time = 0.0f;
        m_HasWarmedUp = false;
        m_Pool.Resize(m_Pool.GetMaxParticles());
        m_TrailData.Resize(m_Pool.GetMaxParticles(), TrailModule.MaxTrailPoints);
        m_PendingTriggers.clear();
        Emitter.Reset();
        Playing = true;

        // Re-initialize GPU system if in GPU mode
        if (m_GPUSystem)
        {
            m_GPUSystem->Init(m_Pool.GetMaxParticles());
        }
    }
} // namespace OloEngine
