#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Destruction/DestructibleSystem.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h" // MeshComponent, MeshPrimitive
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Gameplay/Abilities/Damage/CombatEvents.h" // EntityKilledEvent
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/Physics3DTypes.h" // EForceMode
#include "OloEngine/Physics3D/PhysicsEvents.h"  // JointBrokeEvent
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Deterministic scatter direction for debris piece `index` of the break on
        // entity `seed`. splitmix64 over (uuid, index) → azimuth + up-bias, giving a
        // reproducible outward/upward "pop" (rollback / replay safe — no global RNG).
        glm::vec3 ScatterDir(UUID seed, u32 index)
        {
            u64 x = static_cast<u64>(seed) ^ (0x9E3779B97F4A7C15ull * (static_cast<u64>(index) + 1ull));
            x ^= x >> 30;
            x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 27;
            x *= 0x94D049BB133111EBull;
            x ^= x >> 31;

            const f32 azimuth = static_cast<f32>(x & 0xFFFFu) / 65535.0f * glm::two_pi<f32>();
            const f32 upBias = static_cast<f32>((x >> 16) & 0xFFFFu) / 65535.0f; // 0..1
            const f32 horiz = 0.35f + 0.65f * (1.0f - upBias);                   // flatter when less "up"
            glm::vec3 dir{ std::cos(azimuth) * horiz, 0.4f + 0.8f * upBias, std::sin(azimuth) * horiz };
            const f32 len = glm::length(dir);
            return len > 1.0e-4f ? dir / len : glm::vec3(0.0f, 1.0f, 0.0f);
        }

        // Snapshot of a break, captured while iterating the DestructibleComponent
        // view so the actual spawn/destroy happens AFTER the walk (no structural
        // registry change mid-iteration).
        struct BreakRequest
        {
            UUID SourceID{ 0 };
            glm::vec3 Position{ 0.0f };
            glm::vec3 Scale{ 1.0f };
            Ref<MeshSource> SourceMesh; // reused when no authored chunk mesh
            AssetHandle ChunkMesh{ 0 };
            u32 ChunkCount = 0;
            f32 ChunkScale = 0.25f;
            f32 ChunkMass = 1.0f;
            f32 Impulse = 0.0f;
            f32 Lifetime = DestructibleSystem::kDefaultDebrisLifetime;
        };

        void SpawnDebris(Scene* scene, const BreakRequest& req, u32 count)
        {
            if (count == 0)
                return;

            // The per-chunk entity create + component adds + Jolt body build is the
            // burst-cost hot path; scope it so a large ChunkCount is visible in the
            // profiler without changing spawn behavior.
            OLO_PROFILE_SCOPE("DestructibleSystem::SpawnDebris");

            JoltScene* physics = scene->GetPhysicsScene();

            // Resolve the debris look: authored chunk mesh → the object's own mesh →
            // a cube primitive (ADR 0013). Physics never depends on this.
            Ref<MeshSource> chunkMesh;
            if (req.ChunkMesh)
                chunkMesh = AssetManager::GetAsset<MeshSource>(req.ChunkMesh);
            if (!chunkMesh)
                chunkMesh = req.SourceMesh;

            const glm::vec3 pieceScale = req.Scale * req.ChunkScale;
            // Small outward spawn offset so the pieces don't start exactly
            // coincident (coincident dynamic bodies explode apart in Jolt).
            const f32 spawnRadius = req.ChunkScale * 0.5f + 0.05f;

            for (u32 i = 0; i < count; ++i)
            {
                const glm::vec3 dir = ScatterDir(req.SourceID, i);

                Entity d = scene->CreateEntity("Debris");
                auto& tc = d.GetComponent<TransformComponent>();
                tc.Translation = req.Position + dir * spawnRadius;
                tc.Scale = pieceScale;

                // Visual (skipped by headless renders; still valid data).
                auto& mc = d.AddComponent<MeshComponent>();
                if (chunkMesh)
                    mc.m_MeshSource = chunkMesh;
                else
                    mc.m_Primitive = MeshPrimitive::Cube;

                // Physics: collider FIRST, then the fully-populated rigidbody —
                // OnComponentAdded<Rigidbody3DComponent> reads its fields and builds
                // the Jolt body on construction (runtime-body ordering contract). The
                // unit box is scaled to pieceScale by the transform (JoltShapes
                // applies TransformComponent.Scale).
                BoxCollider3DComponent box;
                box.m_HalfExtents = glm::vec3(0.5f);
                d.AddComponent<BoxCollider3DComponent>(box);

                Rigidbody3DComponent rb;
                rb.m_Type = BodyType3D::Dynamic;
                rb.m_Mass = req.ChunkMass;
                d.AddComponent<Rigidbody3DComponent>(rb);

                if (physics)
                {
                    if (auto body = physics->GetBody(d))
                    {
                        // DEBRIS object layer: collides with static + moving geometry
                        // but the character controller ignores it, so chunks never
                        // shove the player.
                        body->SetToDebrisLayer();
                        if (req.Impulse > 0.0f)
                            body->AddForce(dir * req.Impulse, EForceMode::Impulse);
                    }
                }

                DebrisComponent deb;
                deb.m_RemainingLifetime = req.Lifetime;
                deb.m_TotalLifetime = req.Lifetime;
                d.AddComponent<DebrisComponent>(deb);
            }
        }

        // Flag a destructible for shattering. Called from bus handlers, so it does a
        // field write only (no structural change) — safe mid-iteration.
        void MarkForBreak(Scene* scene, UUID id, bool viaJoint)
        {
            if (!id)
                return;
            auto opt = scene->TryGetEntityWithUUID(id);
            if (!opt)
                return;
            Entity ent = *opt;
            if (!ent.HasComponent<DestructibleComponent>())
                return;
            auto& dc = ent.GetComponent<DestructibleComponent>();
            if (dc.m_Broken)
                return;
            if (viaJoint && !dc.m_BreakOnJointBreak)
                return;
            dc.m_Health = 0.0f;
            dc.m_PendingBreak = true;
        }
    } // namespace

    void DestructibleSystem::WireEvents(Scene* scene)
    {
        if (!scene)
            return;

        GameplayEventBus& bus = scene->GetGameplayEvents();

        // A combat kill shatters a destructible victim.
        bus.Subscribe<EntityKilledEvent>([scene](const EntityKilledEvent& e)
                                         { MarkForBreak(scene, e.VictimID, /*viaJoint=*/false); });

        // A breakable joint giving way shatters the entity it was on (issue #459's
        // pre-built trigger seam — PhysicsEvents.h fires this for exactly this).
        bus.Subscribe<JointBrokeEvent>([scene](const JointBrokeEvent& e)
                                       { MarkForBreak(scene, e.EntityID, /*viaJoint=*/true); });
    }

    bool DestructibleSystem::ApplyDamage(Scene* scene, Entity entity, f32 amount)
    {
        if (!scene || !entity || !entity.HasComponent<DestructibleComponent>())
            return false;
        if (!std::isfinite(amount) || amount <= 0.0f)
            return false;

        auto& dc = entity.GetComponent<DestructibleComponent>();
        if (dc.m_Broken)
            return false;
        if (amount < dc.m_DamageThreshold)
            return false; // chip resistance — sub-threshold hits don't register

        dc.m_Health -= amount;
        if (dc.m_Health <= 0.0f)
        {
            dc.m_Health = 0.0f;
            dc.m_PendingBreak = true;
        }
        return true;
    }

    void DestructibleSystem::OnUpdate(Scene* scene, f32 dtSeconds)
    {
        if (!scene)
            return;

        OLO_PROFILE_FUNCTION();

        // ── Phase 1: collect breaks (field writes only — no structural change) ──
        std::vector<BreakRequest> breaks;
        std::vector<entt::entity> sourcesToDestroy;
        for (auto view = scene->GetAllEntitiesWith<DestructibleComponent, TransformComponent>(); auto e : view)
        {
            Entity ent{ e, scene };
            auto& dc = ent.GetComponent<DestructibleComponent>();
            if (dc.m_Broken)
                continue;
            if (!dc.m_PendingBreak && dc.m_Health > 0.0f)
                continue;

            // Fires exactly once: mark broken now, before any spawn.
            dc.m_Broken = true;
            dc.m_PendingBreak = false;
            dc.m_Health = 0.0f;

            // Sanitize authored physics inputs to finite values before they reach
            // Jolt. The OLO_SERIALIZE(Clamp) annotations only guard the deserialize
            // path, so a runtime write (a script/MCP bug, a direct AddComponent
            // with a hand-set field) could still hand SpawnDebris a NaN that would
            // poison the debris body's scale/mass/impulse. Matches ApplyDamage's
            // std::isfinite gate on the damage amount.
            const auto& tc = ent.GetComponent<TransformComponent>();
            const glm::vec3 sourcePos = tc.Translation;
            const bool posFinite = std::isfinite(sourcePos.x) && std::isfinite(sourcePos.y) && std::isfinite(sourcePos.z);
            const glm::vec3 sourceScale = tc.Scale;
            const bool scaleFinite = std::isfinite(sourceScale.x) && std::isfinite(sourceScale.y) && std::isfinite(sourceScale.z);
            BreakRequest req;
            req.SourceID = ent.GetUUID();
            // A non-finite source position would spawn every chunk at NaN and
            // poison Jolt; fall back to the world origin rather than propagate it.
            req.Position = posFinite ? sourcePos : glm::vec3(0.0f);
            req.Scale = scaleFinite ? sourceScale : glm::vec3(1.0f);
            req.SourceMesh = ent.HasComponent<MeshComponent>() ? ent.GetComponent<MeshComponent>().m_MeshSource : nullptr;
            req.ChunkMesh = dc.m_ChunkMesh;
            req.ChunkCount = dc.m_ChunkCount;
            req.ChunkScale = (std::isfinite(dc.m_ChunkScale) && dc.m_ChunkScale > 0.0f) ? dc.m_ChunkScale : 0.25f;
            req.ChunkMass = (std::isfinite(dc.m_ChunkMass) && dc.m_ChunkMass > 0.0f) ? dc.m_ChunkMass : 1.0f;
            req.Impulse = (std::isfinite(dc.m_ExplosionImpulse) && dc.m_ExplosionImpulse >= 0.0f) ? dc.m_ExplosionImpulse : 0.0f;
            // finite AND positive: a +inf lifetime passes `> 0` but would make the
            // debris immortal (never cleaned up), so fall back to the default.
            req.Lifetime = (std::isfinite(dc.m_DebrisLifetime) && dc.m_DebrisLifetime > 0.0f) ? dc.m_DebrisLifetime : kDefaultDebrisLifetime;
            breaks.push_back(std::move(req));

            if (dc.m_DestroyOnBreak)
                sourcesToDestroy.push_back(e);
        }

        // ── Phase 2: age debris; collect expired + surviving (age, entity) ──
        std::vector<entt::entity> debrisToDestroy;
        std::vector<std::pair<f32, entt::entity>> survivors; // (age, entity)
        for (auto view = scene->GetAllEntitiesWith<DebrisComponent>(); auto e : view)
        {
            Entity ent{ e, scene };
            auto& deb = ent.GetComponent<DebrisComponent>();
            deb.m_Age += dtSeconds;
            deb.m_RemainingLifetime -= dtSeconds;
            if (deb.m_RemainingLifetime <= 0.0f)
                debrisToDestroy.push_back(e);
            else
                survivors.emplace_back(deb.m_Age, e);
        }

        // Nothing structural to do — survivors simply aged in place.
        if (breaks.empty() && debrisToDestroy.empty())
            return;

        // ── Phase 3: destroy expired debris + broken sources (structural) ──
        for (auto e : debrisToDestroy)
        {
            Entity ent{ e, scene };
            if (ent)
                scene->DestroyEntity(ent);
        }
        for (auto e : sourcesToDestroy)
        {
            Entity ent{ e, scene };
            if (ent)
                scene->DestroyEntityAndChildren(ent);
        }

        // ── Phase 4: spawn debris per break under the global live-debris budget ──
        u32 liveCount = static_cast<u32>(survivors.size());
        bool survivorsSorted = false;
        sizet evictCursor = 0;
        for (auto& req : breaks)
        {
            if (req.ChunkCount == 0)
                continue;
            u32 want = std::min(req.ChunkCount, kMaxLiveDebris);

            if (liveCount + want > kMaxLiveDebris)
            {
                // Evict the oldest survivors to make room for fresh debris.
                if (!survivorsSorted)
                {
                    std::sort(survivors.begin(), survivors.end(),
                              [](const auto& a, const auto& b)
                              { return a.first > b.first; }); // oldest first
                    survivorsSorted = true;
                }
                u32 needEvict = (liveCount + want) - kMaxLiveDebris;
                while (needEvict > 0 && evictCursor < survivors.size())
                {
                    Entity old{ survivors[evictCursor].second, scene };
                    ++evictCursor;
                    if (old)
                    {
                        scene->DestroyEntity(old);
                        if (liveCount > 0)
                            --liveCount;
                        --needEvict;
                    }
                }
                // If eviction couldn't free enough, clamp the spawn so the live
                // count never exceeds the budget (hard invariant). Guard the
                // subtraction against unsigned underflow: if liveCount is already
                // at/over the cap, spawn nothing rather than wrapping to a huge want.
                if (liveCount + want > kMaxLiveDebris)
                    want = (liveCount >= kMaxLiveDebris) ? 0u : (kMaxLiveDebris - liveCount);
            }

            if (want == 0)
                continue;
            SpawnDebris(scene, req, want);
            liveCount += want;
        }
    }
} // namespace OloEngine
