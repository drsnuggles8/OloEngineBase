#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Discovery/DiscoverySystem.h"

#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <limits>
#include <string>

namespace OloEngine
{
    namespace
    {
        bool Contains(const std::vector<UUID>& set, UUID id)
        {
            // UUID's implicit operator u64() makes `a == b` ambiguous on
            // MSVC (C2666) — same reason RelationshipComponent compares by
            // explicit u64 cast instead of relying on a UUID operator==.
            return std::find_if(set.begin(), set.end(),
                                [id](const UUID& d)
                                { return static_cast<u64>(d) == static_cast<u64>(id); }) != set.end();
        }

        // A DiscoverableComponent entity only counts toward "N of M" — and can
        // only BE registered by TryRegisterLanding below — once it also carries
        // its own trigger volume. Both call sites share this check so a
        // landmark authored without a trigger (a scene-authoring mistake) is
        // excluded from the total rather than making the objective
        // permanently uncompletable.
        bool IsLandingTrigger(Entity landmark)
        {
            return landmark.HasComponent<Rigidbody3DComponent>() && landmark.GetComponent<Rigidbody3DComponent>().m_IsTrigger;
        }

        // If `discovererID` carries DiscoveredSetComponent and `landmarkID`
        // is a DiscoverableComponent entity whose OWN Rigidbody3DComponent is
        // a trigger, record the visit. Returns true on a NEW discovery (false
        // for an already-discovered landmark, or if either side doesn't
        // qualify) — idempotent, so a boat resting inside the trigger volume
        // across many ticks never double-counts.
        bool TryRegisterLanding(Scene& scene, UUID discovererID, UUID landmarkID)
        {
            auto discovererOpt = scene.TryGetEntityWithUUID(discovererID);
            if (!discovererOpt || !discovererOpt->HasComponent<DiscoveredSetComponent>())
                return false;

            const auto landmarkOpt = scene.TryGetEntityWithUUID(landmarkID);
            if (!landmarkOpt || !landmarkOpt->HasComponent<DiscoverableComponent>() || !IsLandingTrigger(*landmarkOpt))
                return false;

            auto& discovered = discovererOpt->GetComponent<DiscoveredSetComponent>().m_Discovered;
            if (Contains(discovered, landmarkID))
                return false;

            discovered.push_back(landmarkID);
            return true;
        }

        void UpdateObjectiveUI(Scene& scene)
        {
            // Reference discoverer: the first entity tracking a discovered
            // set. Drift has exactly one (the boat); a future multi-actor
            // scene would need a per-actor UI wiring this single-readout
            // design doesn't attempt.
            Entity discoverer;
            for (const auto e : scene.GetAllEntitiesWith<DiscoveredSetComponent>())
            {
                discoverer = Entity{ e, &scene };
                break;
            }
            if (!discoverer)
                return;

            const auto& discovered = discoverer.GetComponent<DiscoveredSetComponent>().m_Discovered;
            const glm::vec3 discovererPos = discoverer.HasComponent<TransformComponent>() ? discoverer.GetComponent<TransformComponent>().Translation : glm::vec3{ 0.0f };

            u32 total = 0;
            UUID nearestUndiscovered{ 0 };
            bool foundUndiscovered = false;
            f32 nearestDistSq = std::numeric_limits<f32>::max();

            for (const auto e : scene.GetAllEntitiesWith<DiscoverableComponent, TransformComponent>())
            {
                const Entity landmark{ e, &scene };
                if (!IsLandingTrigger(landmark))
                    continue; // not actually reachable — don't inflate M or target it

                ++total;
                const UUID id = landmark.GetUUID();
                if (Contains(discovered, id))
                    continue;

                const glm::vec3 delta = landmark.GetComponent<TransformComponent>().Translation - discovererPos;
                const f32 distSq = glm::dot(delta, delta);
                if (distSq < nearestDistSq)
                {
                    nearestDistSq = distSq;
                    nearestUndiscovered = id;
                    foundUndiscovered = true;
                }
            }

            // Marker: track the nearest undiscovered landmark. With nothing
            // left to find, target UUID(0) — UILayoutSystem already hides a
            // world anchor whose target entity doesn't resolve.
            for (auto&& [e, marker, anchor] : scene.GetAllEntitiesWith<DiscoveryObjectiveMarkerComponent, UIWorldAnchorComponent>().each())
            {
                if (!marker.m_Enabled)
                    continue;
                anchor.m_TargetEntity = foundUndiscovered ? nearestUndiscovered : UUID{ 0 };
            }

            // Readout: "Discovered N of M".
            const std::string readout = "Discovered " + std::to_string(discovered.size()) + " of " + std::to_string(total);
            for (auto&& [e, tag, text] : scene.GetAllEntitiesWith<DiscoveryReadoutComponent, UITextComponent>().each())
            {
                if (!tag.m_Enabled)
                    continue;
                text.m_Text = readout;
            }
        }
    } // namespace

    void DiscoverySystem::OnUpdate(Scene* scene, f32 dt)
    {
        (void)dt;
        if (scene == nullptr)
            return;

        JoltScene* physics = scene->GetPhysicsScene();
        if (physics != nullptr)
        {
            // Contact pairs are polled rather than evented: the idempotent
            // set-insert above makes seeing the same resting pair every tick
            // harmless, and it avoids threading a discovery-specific queue
            // through the physics layer for a single-feature consumer.
            for (const auto& [a, b] : physics->GetActiveContactPairs())
            {
                TryRegisterLanding(*scene, a, b);
                TryRegisterLanding(*scene, b, a);
            }
        }

        UpdateObjectiveUI(*scene);
    }

} // namespace OloEngine
