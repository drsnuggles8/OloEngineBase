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
        // Vertical lift (metres) above a landmark's trigger centre for the
        // objective marker — keeps it clear of the terrain silhouette without
        // depending on any one island's height.
        constexpr f32 kMarkerLiftAboveTriggerCentre = 30.0f;

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

        // The landmark's TRIGGER centre in world space, not its entity origin —
        // Drift's islands are authored with the entity Translation at a tile
        // CORNER (see Drift.olo's per-island comment), so the trigger volume
        // is offset from it via BoxCollider3DComponent::m_Offset. Using the raw
        // Translation here would measure "nearest" from the corner, silently
        // biasing the objective marker toward whichever island's corner (not
        // landmass) happens to be closest.
        //
        // Transforms the LOCAL offset through the entity's full WORLD matrix
        // (GetWorldTransform(), not the local TransformComponent) in one step,
        // so rotation, scale AND parenting all compose correctly — the offset
        // is local to the collider/body exactly like Jolt's own shape-transform
        // pipeline treats it (JoltShapes.cpp pairs the offset with the shape
        // before the body's full world transform applies). Every Drift island
        // is unrotated and unparented today, which is exactly the kind of case
        // that would hide a plain Translation-only add as "working".
        glm::vec3 LandmarkWorldCentre(Entity landmark)
        {
            const glm::vec3 localOffset = landmark.HasComponent<BoxCollider3DComponent>()
                                              ? landmark.GetComponent<BoxCollider3DComponent>().m_Offset
                                              : glm::vec3{ 0.0f };
            return glm::vec3(landmark.GetWorldTransform() * glm::vec4(localOffset, 1.0f));
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
            const auto discovererView = scene.GetAllEntitiesWith<DiscoveredSetComponent>();
            const auto discovererIt = discovererView.begin();
            if (discovererIt == discovererView.end())
                return;
            Entity discoverer{ *discovererIt, &scene };

            const auto& discovered = discoverer.GetComponent<DiscoveredSetComponent>().m_Discovered;
            // World-space, not local Translation — a discoverer parented under
            // a moving platform/vehicle-carrier entity (not the case in Drift
            // today, but nothing here should assume otherwise) would otherwise
            // measure distance from the wrong origin.
            const glm::vec3 discovererPos = glm::vec3(discoverer.GetWorldTransform()[3]);

            u32 total = 0;
            u32 validDiscoveredCount = 0;
            Entity nearestUndiscoveredLandmark;
            f32 nearestDistSq = std::numeric_limits<f32>::max();

            for (const auto e : scene.GetAllEntitiesWith<DiscoverableComponent, TransformComponent>())
            {
                const Entity landmark{ e, &scene };
                if (!IsLandingTrigger(landmark))
                    continue; // not actually reachable — don't inflate M or target it

                ++total;
                if (Contains(discovered, landmark.GetUUID()))
                {
                    // Counted against M above (the total scan), and separately
                    // here against N: m_Discovered keeps a UUID forever once
                    // landed (that's the save-game history), but a landmark
                    // could in principle be destroyed or lose its trigger later
                    // — recomputing N from currently-valid landmarks each tick
                    // keeps the readout from ever showing N > M.
                    ++validDiscoveredCount;
                    continue;
                }

                const glm::vec3 delta = LandmarkWorldCentre(landmark) - discovererPos;
                const f32 distSq = glm::dot(delta, delta);
                if (distSq < nearestDistSq)
                {
                    nearestDistSq = distSq;
                    nearestUndiscoveredLandmark = landmark;
                }
            }

            // Marker: track the nearest undiscovered landmark's TRIGGER CENTRE
            // (the m_WorldOffset UILayoutSystem adds is relative to the target
            // entity's raw Translation too — same corner-vs-centre pitfall as
            // LandmarkWorldCentre above, so it's set here from the landmark's
            // own collider offset rather than a fixed authored constant). With
            // nothing left to find, target UUID(0) — UILayoutSystem already
            // hides a world anchor whose target entity doesn't resolve.
            for (auto&& [e, marker, anchor] : scene.GetAllEntitiesWith<DiscoveryObjectiveMarkerComponent, UIWorldAnchorComponent>().each())
            {
                if (!marker.m_Enabled)
                    continue;
                if (nearestUndiscoveredLandmark)
                {
                    anchor.m_TargetEntity = nearestUndiscoveredLandmark.GetUUID();
                    // UILayoutSystem adds m_WorldOffset to the target's raw
                    // Translation (no rotation applied there either), so derive
                    // it from the same rotation-aware centre LandmarkWorldCentre
                    // computes rather than re-deriving the offset by hand — the
                    // lift stays pure world-space "up", not rotated with the
                    // landmark, so the marker doesn't tilt with a sloped island.
                    const glm::vec3 landmarkPos = nearestUndiscoveredLandmark.GetComponent<TransformComponent>().Translation;
                    anchor.m_WorldOffset = (LandmarkWorldCentre(nearestUndiscoveredLandmark) - landmarkPos) + glm::vec3{ 0.0f, kMarkerLiftAboveTriggerCentre, 0.0f };
                }
                else
                {
                    anchor.m_TargetEntity = UUID{ 0 };
                }
            }

            // Readout: "Discovered N of M" — N is validDiscoveredCount, not
            // discovered.size() (see the comment above).
            const std::string readout = "Discovered " + std::to_string(validDiscoveredCount) + " of " + std::to_string(total);
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
