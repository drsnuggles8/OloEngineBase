#include "OloEnginePCH.h"
#include "OloEngine/Fluid/FluidSystem.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Fluid/CPUFluidSolver.h"
#include "OloEngine/Fluid/FluidSettings.h"
#include "OloEngine/Fluid/FluidWorld.h"
#include "OloEngine/Fluid/GPUFluidSolver.h"
#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/Physics3DSystem.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <vector>

namespace OloEngine
{
    namespace
    {
        /// Base seed for per-fluid emission jitter streams. Constant (not the
        /// run seed) so emission is reproducible run-to-run; each fluid entity
        /// gets its own sub-stream via DeriveSeed(seed, entityUUID).
        constexpr u64 kFluidEmitterSeed = 0x464C5549445F5042ULL; // "FLUID_PB"

        /// Coupling impulses are clamped so one step can change a body's
        /// velocity by at most this much — keeps a mis-tuned fluid from
        /// launching bodies into orbit.
        constexpr f32 kMaxCouplingDeltaV = 10.0f; // m/s
        /// Angular clamp: characteristic lever arm (m) times the linear clamp.
        constexpr f32 kMaxCouplingLeverArm = 0.5f;

        [[nodiscard]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        [[nodiscard]] bool InsideAabb(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi)
        {
            return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
        }

        struct EmitterSnapshot
        {
            u64 Id = 0;
            glm::vec3 Position{ 0.0f };
            glm::vec3 Direction{ 0.0f, -1.0f, 0.0f };
            f32 Rate = 0.0f;
            f32 Speed = 0.0f;
            f32 Spread = 0.0f;
        };

        [[nodiscard]] std::vector<GPUFluidEmitEntry> BuildPrefillLattice(const FluidSolverParams& params,
                                                                         f32 fraction, u32 maxParticles)
        {
            std::vector<GPUFluidEmitEntry> entries;
            const f32 d = params.Spacing();
            if (d <= 0.0f)
            {
                return entries;
            }

            const glm::vec3 lo = params.BoundsMin + glm::vec3(params.ParticleRadius);
            glm::vec3 hi = params.BoundsMax - glm::vec3(params.ParticleRadius);
            hi.y = lo.y + (hi.y - lo.y) * std::clamp(fraction, 0.0f, 1.0f);

            const glm::vec3 extent = hi - lo;
            if (extent.x <= 0.0f || extent.y <= 0.0f || extent.z <= 0.0f)
            {
                return entries;
            }

            const glm::uvec3 counts(
                std::max(1u, static_cast<u32>(extent.x / d)),
                std::max(1u, static_cast<u32>(extent.y / d)),
                std::max(1u, static_cast<u32>(extent.z / d)));
            // Centre the lattice inside the fill region.
            const glm::vec3 origin = lo + (extent - glm::vec3(counts) * d) * 0.5f + glm::vec3(d * 0.5f);

            entries.reserve(std::min<sizet>(static_cast<sizet>(counts.x) * counts.y * counts.z, maxParticles));
            for (u32 y = 0; y < counts.y; ++y)
            {
                for (u32 z = 0; z < counts.z; ++z)
                {
                    for (u32 x = 0; x < counts.x; ++x)
                    {
                        if (entries.size() >= maxParticles)
                        {
                            return entries;
                        }
                        GPUFluidEmitEntry entry{};
                        entry.Position = glm::vec4(origin + glm::vec3(x, y, z) * d, 0.0f);
                        entry.Velocity = glm::vec4(0.0f);
                        entries.push_back(entry);
                    }
                }
            }
            return entries;
        }

        /// Build a collision proxy from a Jolt body. Sphere/box/capsule shapes
        /// map exactly; anything else (mesh, convex, compound, scaled) falls
        /// back to its local-bounds box — coarse but stable.
        [[nodiscard]] FluidBodyProxy BuildProxy(const Ref<JoltBody>& body)
        {
            FluidBodyProxy proxy{};
            const glm::vec3 com = body->GetPosition(); // centre of mass
            const glm::quat rot = body->GetRotation();
            // Only dynamic bodies may be asked for mass/velocities: Jolt's
            // Body::GetMotionProperties() asserts !IsStatic() in Debug (and the
            // assert traps SILENTLY wherever Physics3DSystem::Init never wired
            // JPH::AssertFailed — the editor play-mode crash this guards).
            const bool isDynamic = body->IsDynamic();
            const f32 mass = isDynamic ? body->GetMass() : 0.0f;

            FluidBodyProxyShape shapeType = FluidBodyProxyShape::Box;
            glm::vec3 halfExtents(0.5f);

            if (JPH::RefConst<JPH::Shape> shape = body->GetShape())
            {
                switch (shape->GetSubType())
                {
                    case JPH::EShapeSubType::Sphere:
                    {
                        shapeType = FluidBodyProxyShape::Sphere;
                        halfExtents = glm::vec3(static_cast<const JPH::SphereShape*>(shape.GetPtr())->GetRadius(), 0.0f, 0.0f);
                        break;
                    }
                    case JPH::EShapeSubType::Capsule:
                    {
                        const auto* capsule = static_cast<const JPH::CapsuleShape*>(shape.GetPtr());
                        shapeType = FluidBodyProxyShape::Capsule;
                        halfExtents = glm::vec3(capsule->GetRadius(), capsule->GetHalfHeightOfCylinder(), 0.0f);
                        break;
                    }
                    case JPH::EShapeSubType::Box:
                    {
                        const JPH::Vec3 he = static_cast<const JPH::BoxShape*>(shape.GetPtr())->GetHalfExtent();
                        halfExtents = glm::vec3(he.GetX(), he.GetY(), he.GetZ());
                        break;
                    }
                    default:
                    {
                        const JPH::AABox bounds = shape->GetLocalBounds();
                        const JPH::Vec3 he = bounds.GetExtent(); // half extent
                        halfExtents = glm::vec3(he.GetX(), he.GetY(), he.GetZ());
                        break;
                    }
                }
            }

            proxy.Position = glm::vec4(com, static_cast<f32>(shapeType));
            proxy.Rotation = glm::vec4(rot.x, rot.y, rot.z, rot.w);
            proxy.HalfExtents = glm::vec4(halfExtents, 0.0f);
            proxy.LinearVelocity = glm::vec4(isDynamic ? body->GetLinearVelocity() : glm::vec3(0.0f),
                                             mass > 0.0f ? 1.0f / mass : 0.0f);
            proxy.AngularVelocity = glm::vec4(isDynamic ? body->GetAngularVelocity() : glm::vec3(0.0f), 0.0f);
            return proxy;
        }

        void ExtractBodyProxies(JoltScene* jolt, const FluidSolverParams& params,
                                std::vector<FluidBodyProxy>& outProxies,
                                std::vector<UUID>& outEntities)
        {
            OLO_PROFILE_SCOPE("FluidSystem::ExtractBodyProxies");

            const glm::vec3 center = (params.BoundsMin + params.BoundsMax) * 0.5f;
            const glm::vec3 half = (params.BoundsMax - params.BoundsMin) * 0.5f;

            std::vector<SceneQueryHit> hits(static_cast<sizet>(kFluidMaxBodyProxies) * 2);
            const BoxOverlapInfo overlap(center, half);
            const i32 hitCount = jolt->OverlapBox(overlap, hits.data(), static_cast<i32>(hits.size()));

            struct Candidate
            {
                Ref<JoltBody> Body;
                UUID Id = 0;
                bool Dynamic = false;
                f32 Dist2 = 0.0f;
            };
            std::vector<Candidate> candidates;
            candidates.reserve(static_cast<sizet>(hitCount));

            for (i32 i = 0; i < hitCount; ++i)
            {
                const SceneQueryHit& hit = hits[static_cast<sizet>(i)];
                if (!hit.m_HitBody || hit.m_HitEntity == 0)
                {
                    continue;
                }
                // A compound body can report several sub-shape hits — dedupe.
                bool seen = false;
                for (const Candidate& c : candidates)
                {
                    if (c.Id == hit.m_HitEntity)
                    {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                {
                    continue;
                }
                Candidate c;
                c.Body = hit.m_HitBody;
                c.Id = hit.m_HitEntity;
                c.Dynamic = hit.m_HitBody->IsDynamic();
                const glm::vec3 d = hit.m_HitBody->GetPosition() - center;
                c.Dist2 = glm::dot(d, d);
                candidates.push_back(c);
            }

            // Dynamic bodies first (they need the coupling), then nearest-first;
            // UUID as the deterministic tie-break.
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
                      {
                if (a.Dynamic != b.Dynamic)
                {
                    return a.Dynamic;
                }
                // Branchless strict-weak ordering on Dist2 without a float != /
                // == comparison: only the two < probes decide, and equal
                // distances fall through to the deterministic Id tie-break.
                if (a.Dist2 < b.Dist2)
                {
                    return true;
                }
                if (b.Dist2 < a.Dist2)
                {
                    return false;
                }
                return static_cast<u64>(a.Id) < static_cast<u64>(b.Id); });

            const sizet count = std::min<sizet>(candidates.size(), kFluidMaxBodyProxies);
            outProxies.reserve(count);
            outEntities.reserve(count);
            for (sizet i = 0; i < count; ++i)
            {
                outProxies.push_back(BuildProxy(candidates[i].Body));
                outEntities.push_back(candidates[i].Id);
            }
        }

        void ApplyFeedback(JoltScene* jolt, std::span<const UUID> entities,
                           std::span<const FluidBodyFeedback> feedback, f32 dt)
        {
            if (!jolt)
            {
                return;
            }
            for (sizet i = 0; i < entities.size() && i < feedback.size(); ++i)
            {
                const FluidBodyFeedback& fb = feedback[i];
                if (!IsFinite(fb.Impulse) || !IsFinite(fb.AngularImpulse))
                {
                    continue;
                }
                if (glm::dot(fb.Impulse, fb.Impulse) <= 0.0f && glm::dot(fb.AngularImpulse, fb.AngularImpulse) <= 0.0f)
                {
                    continue;
                }
                Ref<JoltBody> body = jolt->GetBodyByEntityID(entities[i]);
                if (!body || !body->IsDynamic())
                {
                    continue;
                }
                const f32 mass = body->GetMass();
                if (mass <= 0.0f)
                {
                    continue;
                }

                glm::vec3 impulse = fb.Impulse;
                const f32 maxImpulse = mass * kMaxCouplingDeltaV;
                const f32 impulseLen = glm::length(impulse);
                if (impulseLen > maxImpulse)
                {
                    impulse *= maxImpulse / impulseLen;
                }
                body->AddForce(impulse, EForceMode::Impulse);

                glm::vec3 angular = fb.AngularImpulse;
                const f32 maxAngular = maxImpulse * kMaxCouplingLeverArm;
                const f32 angularLen = glm::length(angular);
                if (angularLen > maxAngular)
                {
                    angular *= maxAngular / angularLen;
                }
                if (angularLen > 0.0f && dt > 0.0f)
                {
                    // Apply the reaction as an angular impulse directly, so the
                    // rotational response is independent of the frame timestep
                    // (AddTorque would integrate torque * dt instead).
                    body->AddAngularImpulse(angular);
                }
            }
        }

        void StageEmissions(FluidInstance& instance, const FluidSolverParams& params,
                            std::span<const EmitterSnapshot> emitters, f32 dt,
                            std::vector<GPUFluidEmitEntry>& outStaged)
        {
            for (const EmitterSnapshot& emitter : emitters)
            {
                if (!InsideAabb(emitter.Position, params.BoundsMin, params.BoundsMax))
                {
                    continue;
                }

                f32& carry = instance.EmitCarry[emitter.Id];
                const f32 want = emitter.Rate * dt + carry;
                u32 n = static_cast<u32>(want);
                carry = want - static_cast<f32>(n);

                n = std::min(n, kFluidMaxEmitPerStep - static_cast<u32>(outStaged.size()));
                if (n == 0)
                {
                    continue;
                }

                // Orthonormal basis around the emit direction for disc sampling.
                const glm::vec3 dir = emitter.Direction;
                const glm::vec3 tangent = std::abs(dir.y) < 0.99f
                                              ? glm::normalize(glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)))
                                              : glm::vec3(1.0f, 0.0f, 0.0f);
                const glm::vec3 bitangent = glm::cross(dir, tangent);

                for (u32 k = 0; k < n; ++k)
                {
                    const f32 angle = instance.EmitRng.GetFloat32() * 2.0f * std::numbers::pi_v<f32>;
                    const f32 radius = std::sqrt(instance.EmitRng.GetFloat32()) * emitter.Spread;
                    GPUFluidEmitEntry entry{};
                    entry.Position = glm::vec4(
                        emitter.Position + (std::cos(angle) * tangent + std::sin(angle) * bitangent) * radius, 0.0f);
                    entry.Velocity = glm::vec4(dir * emitter.Speed, 0.0f);
                    outStaged.push_back(entry);
                }
            }
        }
    } // namespace

    bool FluidSystem::IsSequentialForced()
    {
        static const bool s_Forced = []
        {
            const char* env = std::getenv("OLO_FLUID_SEQUENTIAL");
            return env && env[0] == '1';
        }();
        return s_Forced;
    }

    void FluidSystem::OnUpdate(Scene* scene, f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene || !(deltaTime > 0.0f) || !std::isfinite(deltaTime))
        {
            return;
        }

        auto fluidView = scene->GetAllEntitiesWith<TransformComponent, FluidComponent, IDComponent>();
        if (fluidView.begin() == fluidView.end())
        {
            // No fluid entities left: sweep any stale instances (component or
            // entity was removed) so GPU buffers release on the game thread.
            if (FluidWorld* world = scene->TryGetFluidWorld(); world && world->GetInstanceCount() > 0)
            {
                world->Sweep(world->AdvanceTick());
            }
            return;
        }

        FluidWorld& world = scene->GetFluidWorld();
        const u64 tick = world.AdvanceTick();

        JoltScene* jolt = scene->GetPhysicsScene();
        const glm::vec3 gravity = jolt ? jolt->GetGravity() : Physics3DSystem::GetSettings().m_Gravity;

        // ---- Snapshot kill volumes and emitters (shared across domains) -----
        std::vector<FluidKillBox> killBoxes;
        for (auto e : scene->GetAllEntitiesWith<TransformComponent, FluidKillVolumeComponent>())
        {
            Entity entity{ e, scene };
            const auto& kv = entity.GetComponent<FluidKillVolumeComponent>();
            if (!kv.m_Enabled)
            {
                continue;
            }
            const glm::vec3 center(scene->GetWorldTransform(e)[3]);
            if (!IsFinite(center))
            {
                continue;
            }
            killBoxes.push_back({ center - kv.m_HalfExtents, center + kv.m_HalfExtents });
        }

        std::vector<EmitterSnapshot> emitters;
        for (auto e : scene->GetAllEntitiesWith<TransformComponent, FluidEmitterComponent, IDComponent>())
        {
            Entity entity{ e, scene };
            const auto& ec = entity.GetComponent<FluidEmitterComponent>();
            if (!ec.m_Enabled || ec.m_Rate <= 0.0f)
            {
                continue;
            }
            const glm::mat4 worldTm = scene->GetWorldTransform(e);
            EmitterSnapshot snapshot;
            snapshot.Id = static_cast<u64>(entity.GetComponent<IDComponent>().ID);
            snapshot.Position = glm::vec3(worldTm[3]);
            const glm::vec3 forward = glm::mat3(worldTm) * glm::vec3(0.0f, 0.0f, -1.0f);
            const f32 forwardLen = glm::length(forward);
            snapshot.Direction = forwardLen > 1.0e-5f ? forward / forwardLen : glm::vec3(0.0f, -1.0f, 0.0f);
            snapshot.Rate = ec.m_Rate;
            snapshot.Speed = ec.m_Speed;
            snapshot.Spread = ec.m_SpreadRadius;
            if (!IsFinite(snapshot.Position) || !IsFinite(snapshot.Direction))
            {
                continue;
            }
            emitters.push_back(snapshot);
        }

        // ---- Per-domain update ----------------------------------------------
        for (auto e : fluidView)
        {
            Entity entity{ e, scene };
            const auto& fluid = entity.GetComponent<FluidComponent>();
            if (!fluid.m_Enabled)
            {
                continue; // untouched instance sweeps away below
            }

            const glm::vec3 center(scene->GetWorldTransform(e)[3]);
            if (!IsFinite(center))
            {
                continue;
            }
            const u64 id = static_cast<u64>(entity.GetComponent<IDComponent>().ID);

            // Resolve settings (0 / unresolvable handle => engine defaults).
            Ref<FluidSettings> settingsAsset;
            if (fluid.m_Settings != 0 && Project::GetActive() &&
                AssetManager::IsAssetHandleValid(fluid.m_Settings))
            {
                settingsAsset = AssetManager::GetAsset<FluidSettings>(fluid.m_Settings);
            }
            static const FluidSettings s_Defaults;
            const FluidSettings& settings = settingsAsset ? *settingsAsset : s_Defaults;

            const FluidSolverParams params =
                settings.ToSolverParams(center, fluid.m_DomainHalfExtents, gravity);

            const bool wantGpu = !IsSequentialForced() &&
                                 fluid.m_SolverMode != FluidSolverMode::CPU &&
                                 (fluid.m_SolverMode == FluidSolverMode::GPU || Renderer3D::IsInitialized());

            FluidInstance& instance = world.GetOrCreate(id);
            instance.LastTouchedTick = tick;

            const u32 maxParticles = std::clamp(fluid.m_MaxParticles, 64u, 1000000u);
            const bool missingBackend = !instance.Cpu && !instance.Gpu;
            if (missingBackend || instance.UsingGpu != wantGpu || instance.ConfiguredMaxParticles != maxParticles)
            {
                instance.Cpu.reset();
                instance.Gpu.reset();
                // UsingGpu records the *requested* backend so a failed GPU init
                // (fallback to CPU below) doesn't recreate the instance every tick.
                instance.UsingGpu = wantGpu;
                instance.ConfiguredMaxParticles = maxParticles;
                instance.Prefilled = false;
                instance.PrevProxies.clear();
                instance.PrevProxyEntities.clear();
                instance.EmitCarry.clear();
                instance.EmitRng.SetSeed(ParticleSystem::DeriveSeed(kFluidEmitterSeed, id));

                if (wantGpu)
                {
                    instance.Gpu = CreateScope<GPUFluidSolver>(maxParticles);
                    if (!instance.Gpu->IsValid())
                    {
                        OLO_CORE_WARN("FluidSystem: GPU solver init failed for fluid entity {} — "
                                      "falling back to the CPU reference solver",
                                      id);
                        instance.Gpu.reset();
                        instance.Cpu = CreateScope<CPUFluidSolver>(maxParticles);
                    }
                }
                else
                {
                    instance.Cpu = CreateScope<CPUFluidSolver>(maxParticles);
                }
            }

            if (!instance.Prefilled)
            {
                instance.Prefilled = true;
                if (fluid.m_PrefillFraction > 0.0f)
                {
                    const std::vector<GPUFluidEmitEntry> seed =
                        BuildPrefillLattice(params, fluid.m_PrefillFraction, maxParticles);
                    if (!seed.empty())
                    {
                        if (instance.Gpu)
                        {
                            instance.Gpu->SeedParticles(seed);
                        }
                        else
                        {
                            instance.Cpu->Emit(seed);
                        }
                    }
                }
            }

            std::vector<GPUFluidEmitEntry> staged;
            StageEmissions(instance, params, emitters, deltaTime, staged);
            if (!staged.empty())
            {
                if (instance.Gpu)
                {
                    instance.Gpu->Emit(staged);
                }
                else
                {
                    instance.Cpu->Emit(staged);
                }
            }

            std::vector<FluidBodyProxy> proxies;
            std::vector<UUID> proxyEntities;
            if (jolt)
            {
                ExtractBodyProxies(jolt, params, proxies, proxyEntities);
            }

            if (instance.Gpu)
            {
                // GPU feedback has one step of latency: harvest the impulses the
                // PREVIOUS step accumulated (for its proxy set) before launching
                // this step, and apply them to those bodies.
                if (!instance.PrevProxies.empty())
                {
                    std::vector<FluidBodyFeedback> feedback(instance.PrevProxies.size());
                    instance.Gpu->HarvestFeedback(feedback);
                    ApplyFeedback(jolt, instance.PrevProxyEntities, feedback, deltaTime);
                }
                instance.Gpu->Step(params, deltaTime, proxies, killBoxes);
                instance.PrevProxies = std::move(proxies);
                instance.PrevProxyEntities = std::move(proxyEntities);
            }
            else
            {
                std::vector<FluidBodyFeedback> feedback(proxies.size());
                instance.Cpu->Step(params, deltaTime, proxies, feedback, killBoxes);
                ApplyFeedback(jolt, proxyEntities, feedback, deltaTime);
            }
        }

        world.Sweep(tick);
    }
} // namespace OloEngine
