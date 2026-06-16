#pragma once

// Pure, engine-light reasoning for the olo_physics_* "explain" tools (issue
// #306 item A, physics half). The MCP tool handlers in McpTools.cpp gather raw
// facts off the live Jolt simulation on the main thread, then hand them here to
// be turned into a human/machine answer. Keeping the reasoning in a free
// function with NO Jolt / EnTT / editor dependencies means it can be unit-tested
// directly (the test binary compiles McpServer.cpp but deliberately NOT
// McpTools.cpp), and the headline olo_physics_why_no_collision tool's logic is
// covered without needing a live editor or GPU.
//
// Only OloEngine/Core/Base.h is pulled in (for the u32 typedef); everything else
// is the standard library.

#include "OloEngine/Core/Base.h"

#include <string>
#include <vector>

namespace OloEngine::MCP::PhysicsExplain
{
    // Mirror of the engine's EBodyType, kept separate so this header stays free
    // of the physics module. The handler maps EBodyType -> this before calling in.
    enum class BodyType
    {
        Static,
        Dynamic,
        Kinematic
    };

    [[nodiscard]] inline const char* BodyTypeName(BodyType type)
    {
        switch (type)
        {
            case BodyType::Static:
                return "Static";
            case BodyType::Dynamic:
                return "Dynamic";
            case BodyType::Kinematic:
                return "Kinematic";
        }
        return "Unknown";
    }

    // The collision-relevant facts about one entity, gathered from the live sim.
    struct EntityPhysicsFacts
    {
        bool EntityExists = false; // the UUID resolves to an entity in the scene
        bool HasRigidbody = false; // carries a Rigidbody3DComponent (→ a body is authored)
        bool HasCollider = false;  // carries some *Collider3DComponent (→ a shape)
        bool HasBody = false;      // a live Jolt body exists in the running sim
        BodyType Type = BodyType::Static;
        bool IsTrigger = false; // body is a sensor/trigger (overlap events, no solid response)
        u32 LayerId = 0;
        std::string LayerName; // human-readable object-layer name (e.g. "MOVING")
    };

    // Everything ExplainWhyNoCollision needs. The handler fills these from the
    // live Jolt scene; this function never touches the engine.
    struct WhyNoCollisionInput
    {
        bool SameEntity = false;     // the two requested UUIDs are identical
        bool PhysicsRunning = false; // a live, initialized 3D physics scene exists
        EntityPhysicsFacts A;
        EntityPhysicsFacts B;
        // Whether the object-layer pair filter permits A's and B's layers to
        // collide. Only meaningful once both have live bodies.
        bool LayersCollide = false;
        // Whether the two bodies' world-space bounding volumes currently overlap.
        // A coarse proximity signal (broadphase-level), not exact contact.
        bool BoundsOverlap = false;
    };

    struct WhyNoCollisionVerdict
    {
        // Machine-readable primary reason. One of:
        //   same_entity, physics_not_running, entity_a_missing, entity_b_missing,
        //   entity_a_no_rigidbody, entity_b_no_rigidbody, entity_a_no_collider,
        //   entity_b_no_collider, entity_a_no_body, entity_b_no_body, both_static,
        //   layers_dont_collide, trigger_no_solid_response, not_overlapping,
        //   would_collide.
        std::string ReasonCode;
        // One-line human explanation of the primary reason.
        std::string Summary;
        // Ordered trace of every check performed, each prefixed "[ok]"/"[fail]"/
        // "[warn]", so the agent can see exactly how far the chain got.
        std::vector<std::string> Checks;
        // True only when nothing blocks a solid collision response between the
        // pair. (not_overlapping / would_collide are both "can collide".)
        bool CanCollide = false;
    };

    namespace Detail
    {
        // Per-side body-presence cascade: rigidbody → collider → live body. Returns
        // a non-empty {code, summary} when this side blocks collision, plus appends
        // the trace lines it evaluated. `side` is "A" or "B"; `code` uses lowercase.
        struct SideBlock
        {
            bool Blocked = false;
            std::string Code;
            std::string Summary;
        };

        [[nodiscard]] inline SideBlock CheckSideBody(const EntityPhysicsFacts& facts,
                                                     const char* side,
                                                     const char* sideLower,
                                                     std::vector<std::string>& checks)
        {
            SideBlock result;
            if (!facts.HasRigidbody)
            {
                checks.push_back(std::string("[fail] ") + side + " has no Rigidbody3DComponent");
                result.Blocked = true;
                result.Code = std::string("entity_") + sideLower + "_no_rigidbody";
                result.Summary = std::string("Entity ") + side +
                                 " has no Rigidbody3DComponent, so the physics engine never creates a body for it. "
                                 "Add a Rigidbody3DComponent (Static/Dynamic/Kinematic) to give it presence in the simulation.";
                return result;
            }
            checks.push_back(std::string("[ok] ") + side + " has a Rigidbody3DComponent");

            if (!facts.HasCollider)
            {
                checks.push_back(std::string("[fail] ") + side + " has no collider component");
                result.Blocked = true;
                result.Code = std::string("entity_") + sideLower + "_no_collider";
                result.Summary = std::string("Entity ") + side +
                                 " has a rigidbody but no collider component (Box/Sphere/Capsule/Mesh), so it has no "
                                 "collision shape and nothing for the other body to hit. Add a collider component.";
                return result;
            }
            checks.push_back(std::string("[ok] ") + side + " has a collider component");

            if (!facts.HasBody)
            {
                checks.push_back(std::string("[fail] ") + side + " has no live body in the running simulation");
                result.Blocked = true;
                result.Code = std::string("entity_") + sideLower + "_no_body";
                result.Summary = std::string("Entity ") + side +
                                 " is authored with a rigidbody and collider but has no live body in the running "
                                 "simulation — body creation failed, or the component was added after physics "
                                 "started. Check the engine log (tag Physics) for body-creation errors.";
                return result;
            }
            checks.push_back(std::string("[ok] ") + side + " has a live physics body");
            return result;
        }
    } // namespace Detail

    // Map the gathered facts to a verdict. A deterministic cascade from the most
    // fundamental precondition (same entity / physics running / entities exist /
    // bodies exist) down to the collision-specific filters (both-static, layer
    // matrix, trigger, bounds overlap). Stops and reports the first blocker, so
    // ReasonCode is always the *root* cause rather than a downstream symptom.
    [[nodiscard]] inline WhyNoCollisionVerdict ExplainWhyNoCollision(const WhyNoCollisionInput& in)
    {
        WhyNoCollisionVerdict verdict;
        auto& checks = verdict.Checks;

        if (in.SameEntity)
        {
            checks.push_back("[fail] A and B are the same entity");
            verdict.ReasonCode = "same_entity";
            verdict.Summary = "A and B are the same entity — an entity cannot collide with itself.";
            return verdict;
        }
        checks.push_back("[ok] A and B are distinct entities");

        if (!in.PhysicsRunning)
        {
            checks.push_back("[fail] the 3D physics simulation is not running");
            verdict.ReasonCode = "physics_not_running";
            verdict.Summary = "The 3D physics simulation is not running (enter Play mode). Collision can only be "
                              "evaluated against a live, initialized physics scene.";
            return verdict;
        }
        checks.push_back("[ok] the 3D physics simulation is running");

        if (!in.A.EntityExists)
        {
            checks.push_back("[fail] entity A does not exist in the active scene");
            verdict.ReasonCode = "entity_a_missing";
            verdict.Summary = "Entity A does not exist in the active scene — check the UUID with olo_scene_list_entities.";
            return verdict;
        }
        if (!in.B.EntityExists)
        {
            checks.push_back("[fail] entity B does not exist in the active scene");
            verdict.ReasonCode = "entity_b_missing";
            verdict.Summary = "Entity B does not exist in the active scene — check the UUID with olo_scene_list_entities.";
            return verdict;
        }
        checks.push_back("[ok] both entities exist in the active scene");

        if (const Detail::SideBlock blockA = Detail::CheckSideBody(in.A, "A", "a", checks); blockA.Blocked)
        {
            verdict.ReasonCode = blockA.Code;
            verdict.Summary = blockA.Summary;
            return verdict;
        }
        if (const Detail::SideBlock blockB = Detail::CheckSideBody(in.B, "B", "b", checks); blockB.Blocked)
        {
            verdict.ReasonCode = blockB.Code;
            verdict.Summary = blockB.Summary;
            return verdict;
        }

        if (in.A.Type == BodyType::Static && in.B.Type == BodyType::Static)
        {
            checks.push_back("[fail] both bodies are Static");
            verdict.ReasonCode = "both_static";
            verdict.Summary = "Both bodies are Static. Two static bodies never collide — at least one must be "
                              "Dynamic (or a Kinematic/Dynamic pair). This is the classic 'mover passes through "
                              "the floor' cause when the moving object was left Static.";
            return verdict;
        }
        checks.push_back(std::string("[ok] at least one body is non-static (A=") + BodyTypeName(in.A.Type) +
                         ", B=" + BodyTypeName(in.B.Type) + ")");

        if (!in.LayersCollide)
        {
            checks.push_back(std::string("[fail] layers '") + in.A.LayerName + "' and '" + in.B.LayerName +
                             "' are filtered apart");
            verdict.ReasonCode = "layers_dont_collide";
            verdict.Summary = std::string("Their collision layers are filtered apart: '") + in.A.LayerName +
                              "' does not collide with '" + in.B.LayerName +
                              "' in the layer matrix, so no contact is ever generated. Inspect/adjust the matrix "
                              "with olo_physics_layer_matrix.";
            return verdict;
        }
        checks.push_back(std::string("[ok] layers '") + in.A.LayerName + "' and '" + in.B.LayerName +
                         "' are allowed to collide");

        if (in.A.IsTrigger || in.B.IsTrigger)
        {
            const char* which = (in.A.IsTrigger && in.B.IsTrigger) ? "Both A and B are"
                                : in.A.IsTrigger                   ? "A is"
                                                                   : "B is";
            checks.push_back(std::string("[warn] ") + which + " a trigger/sensor");
            verdict.ReasonCode = "trigger_no_solid_response";
            verdict.Summary = std::string(which) +
                              " a trigger/sensor. Triggers detect overlap and fire contact/trigger events but produce "
                              "NO solid collision response, so bodies pass straight through. If you expected a solid "
                              "block, clear the trigger flag; if you expected an event, confirm it with olo_physics_contacts.";
            return verdict;
        }
        checks.push_back("[ok] neither body is a trigger/sensor");

        if (!in.BoundsOverlap)
        {
            checks.push_back("[fail] the bodies' bounding volumes do not currently overlap");
            verdict.ReasonCode = "not_overlapping";
            verdict.Summary = "Everything is configured to collide, but the two bodies' bounding volumes are not "
                              "overlapping right now — they are simply not touching at this moment. If a fast-moving "
                              "body tunnels through, switch it to Continuous collision detection.";
            verdict.CanCollide = true;
            return verdict;
        }
        checks.push_back("[ok] the bodies' bounding volumes overlap");

        verdict.ReasonCode = "would_collide";
        verdict.Summary = "These two bodies are fully eligible to collide and their bounding volumes overlap — the "
                          "engine should be generating contacts between them. If you still see no response, check "
                          "masses/forces, or look at olo_physics_contacts to confirm the contact is registered.";
        verdict.CanCollide = true;
        return verdict;
    }
} // namespace OloEngine::MCP::PhysicsExplain
