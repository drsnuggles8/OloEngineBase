#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpPhysicsExplain.h"
#include "MCP/McpSetCollisionLayer.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltLayerInterface.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Physics3D/PhysicsLayer.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Physics MCP tools: the read-only Jolt introspection family (layer matrix,
// colliders, contacts, raycast, overlap, why-no-collision) and the consented
// olo_set_collision_layer write. Split out of the McpTools.cpp monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
        // ======================================================================
        // olo_physics_* — physics introspection + "explain" tools (#306 item A).
        //
        // The layer matrix is static / mutex-guarded registry data and reads
        // lock-safe from the handler thread. Everything else touches the live
        // Jolt scene + EnTT registry, so it runs inside MarshalRead on the
        // editor's main thread, exactly like the olo_scene_* tools. All are
        // strictly read-only — no body, layer, or component is ever mutated.
        // ======================================================================

        // ObjectLayers built-in count + names; user-defined layers map to object
        // layers ObjectLayers::NUM_LAYERS + layerId (see JoltLayerInterface).
        std::string ObjectLayerName(JPH::ObjectLayer layer)
        {
            switch (layer)
            {
                case ObjectLayers::NON_MOVING:
                    return "NON_MOVING";
                case ObjectLayers::MOVING:
                    return "MOVING";
                case ObjectLayers::TRIGGER:
                    return "TRIGGER";
                case ObjectLayers::CHARACTER:
                    return "CHARACTER";
                case ObjectLayers::DEBRIS:
                    return "DEBRIS";
                default:
                    break;
            }
            // layer >= NUM_LAYERS here, so the subtraction never underflows.
            const u32 userId = static_cast<u32>(layer) - ObjectLayers::NUM_LAYERS;
            if (const PhysicsLayer pl = PhysicsLayerManager::GetLayer(userId); pl.IsValid())
                return pl.m_Name;
            return "Layer#" + std::to_string(static_cast<u32>(layer));
        }

        PhysicsExplain::BodyType MapBodyType(EBodyType type)
        {
            switch (type)
            {
                case EBodyType::Dynamic:
                    return PhysicsExplain::BodyType::Dynamic;
                case EBodyType::Kinematic:
                    return PhysicsExplain::BodyType::Kinematic;
                case EBodyType::Static:
                default:
                    return PhysicsExplain::BodyType::Static;
            }
        }

        PhysicsExplain::BodyType MapBodyType(BodyType3D type)
        {
            switch (type)
            {
                case BodyType3D::Dynamic:
                    return PhysicsExplain::BodyType::Dynamic;
                case BodyType3D::Kinematic:
                    return PhysicsExplain::BodyType::Kinematic;
                case BodyType3D::Static:
                default:
                    return PhysicsExplain::BodyType::Static;
            }
        }

        EBodyType ToEBodyType(BodyType3D type)
        {
            switch (type)
            {
                case BodyType3D::Dynamic:
                    return EBodyType::Dynamic;
                case BodyType3D::Kinematic:
                    return EBodyType::Kinematic;
                case BodyType3D::Static:
                default:
                    return EBodyType::Static;
            }
        }

        bool HasAnyCollider3D(Entity entity)
        {
            return entity.HasComponent<BoxCollider3DComponent>() ||
                   entity.HasComponent<SphereCollider3DComponent>() ||
                   entity.HasComponent<CapsuleCollider3DComponent>() ||
                   entity.HasComponent<MeshCollider3DComponent>() ||
                   entity.HasComponent<ConvexMeshCollider3DComponent>() ||
                   entity.HasComponent<TriangleMeshCollider3DComponent>();
        }

        // Describe an entity's authored collision shape(s) from its collider
        // components. Most entities have exactly one; an array keeps compound
        // setups honest. Main-thread only (reads the registry).
        Json DescribeColliders(Entity entity)
        {
            const auto vec3 = [](const glm::vec3& v)
            { return Json::array({ v.x, v.y, v.z }); };

            Json arr = Json::array();
            if (entity.HasComponent<BoxCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<BoxCollider3DComponent>();
                arr.push_back(Json{ { "type", "Box" }, { "halfExtents", vec3(c.m_HalfExtents) }, { "offset", vec3(c.m_Offset) } });
            }
            if (entity.HasComponent<SphereCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<SphereCollider3DComponent>();
                arr.push_back(Json{ { "type", "Sphere" }, { "radius", c.m_Radius }, { "offset", vec3(c.m_Offset) } });
            }
            if (entity.HasComponent<CapsuleCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<CapsuleCollider3DComponent>();
                arr.push_back(Json{ { "type", "Capsule" }, { "radius", c.m_Radius }, { "halfHeight", c.m_HalfHeight }, { "offset", vec3(c.m_Offset) } });
            }
            if (entity.HasComponent<MeshCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<MeshCollider3DComponent>();
                arr.push_back(Json{ { "type", "Mesh" }, { "colliderAsset", std::to_string(static_cast<u64>(c.m_ColliderAsset)) } });
            }
            if (entity.HasComponent<ConvexMeshCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<ConvexMeshCollider3DComponent>();
                arr.push_back(Json{ { "type", "ConvexMesh" }, { "colliderAsset", std::to_string(static_cast<u64>(c.m_ColliderAsset)) } });
            }
            if (entity.HasComponent<TriangleMeshCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<TriangleMeshCollider3DComponent>();
                arr.push_back(Json{ { "type", "TriangleMesh" }, { "colliderAsset", std::to_string(static_cast<u64>(c.m_ColliderAsset)) } });
            }
            return arr;
        }

        std::string EntityName(Entity entity)
        {
            return entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : std::string{};
        }

        // A live, initialized 3D physics scene, or nullptr. Call on the main thread.
        JoltScene* GetRunningPhysics(const Ref<Scene>& scene)
        {
            if (!scene)
                return nullptr;
            JoltScene* physics = scene->GetPhysicsScene();
            return (physics && physics->IsInitialized()) ? physics : nullptr;
        }

        // ---- olo_physics_layer_matrix (lock-safe) ------------------------------
        // Dumps the object-layer collision matrix the simulation actually uses:
        // the five built-in Jolt object layers plus every user-defined
        // PhysicsLayerManager layer, with pairwise collide/no-collide from the
        // real ObjectLayerPairFilter. Works in Edit mode too (static registry).
        ToolResult Handle_PhysicsLayerMatrix(McpServer& /*server*/, const Json& /*args*/)
        {
            // Collect the object layers in play: built-ins 0..NUM_LAYERS-1, then
            // each valid user layer at NUM_LAYERS + layerId.
            struct LayerEntry
            {
                JPH::ObjectLayer ObjectLayer;
                std::string Name;
                bool IsUser;
                u32 UserLayerId;
            };
            std::vector<LayerEntry> layers;
            for (JPH::ObjectLayer i = 0; i < ObjectLayers::NUM_LAYERS; ++i)
                layers.push_back({ i, ObjectLayerName(i), false, 0 });

            const std::vector<PhysicsLayer> userLayers = PhysicsLayerManager::GetLayers();
            for (const PhysicsLayer& pl : userLayers)
            {
                if (!pl.IsValid())
                    continue;
                layers.push_back({ static_cast<JPH::ObjectLayer>(ObjectLayers::NUM_LAYERS + pl.m_LayerID), pl.m_Name, true, pl.m_LayerID });
            }

            const ObjectLayerPairFilter& filter = JoltLayerInterface::GetObjectLayerPairFilter();

            Json objectLayers = Json::array();
            for (const LayerEntry& e : layers)
            {
                Json le{ { "objectLayer", static_cast<u32>(e.ObjectLayer) }, { "name", e.Name }, { "kind", e.IsUser ? "user" : "builtin" } };
                if (e.IsUser)
                    le["userLayerId"] = e.UserLayerId;
                objectLayers.push_back(std::move(le));
            }

            // Upper triangle (incl. self-pairs) — the matrix is symmetric.
            Json matrix = Json::array();
            for (sizet a = 0; a < layers.size(); ++a)
            {
                for (sizet b = a; b < layers.size(); ++b)
                {
                    const bool collides = filter.ShouldCollide(layers[a].ObjectLayer, layers[b].ObjectLayer);
                    matrix.push_back(Json{ { "a", layers[a].Name }, { "b", layers[b].Name }, { "collides", collides } });
                }
            }

            Json userDefined = Json::array();
            for (const PhysicsLayer& pl : userLayers)
            {
                if (!pl.IsValid())
                    continue;
                Json collidesWith = Json::array();
                for (const PhysicsLayer& other : userLayers)
                {
                    if (!other.IsValid())
                        continue;
                    if (PhysicsLayerManager::ShouldCollide(pl.m_LayerID, other.m_LayerID))
                        collidesWith.push_back(other.m_Name);
                }
                userDefined.push_back(Json{ { "id", pl.m_LayerID },
                                            { "name", pl.m_Name },
                                            { "bitValue", pl.m_BitValue },
                                            { "collidesWithSelf", pl.m_CollidesWithSelf },
                                            { "collidesWith", std::move(collidesWith) } });
            }

            Json j{ { "objectLayers", std::move(objectLayers) },
                    { "collisionMatrix", std::move(matrix) },
                    { "userDefinedLayers", std::move(userDefined) },
                    { "note",
                      "Built-in object layers and their collide rules are fixed (NON_MOVING vs NON_MOVING never "
                      "collide; MOVING collides with all; TRIGGER/CHARACTER/DEBRIS have specific rules). "
                      "User-defined layers are authored in the editor's Physics settings." } };
            return ToolResult::Text(j.dump(2));
        }

        // ---- olo_physics_list_colliders (main-marshaled) -----------------------
        // Every entity with a Rigidbody3DComponent: authored body type / layer /
        // trigger / collider shapes, plus live body state (object layer, position,
        // awake/asleep) when physics is running. Paginated like list_entities.
        ToolResult Handle_PhysicsListColliders(McpServer& server, const Json& args)
        {
            // Keep page/pageSize in a wide signed type end-to-end: a huge 'page'
            // narrowed to int wraps negative (well-defined but wrong in C++20), and
            // a negative page * pageSize yields a negative start index that, cast to
            // sizet for bodies[i], reads far out of bounds. Clamp low here and bound
            // the start against the collection below.
            long long page = 0;
            long long pageSize = 50;
            if (args.contains("page") && args["page"].is_number_integer())
                page = std::max<long long>(0, args["page"].get<long long>());
            if (args.contains("pageSize") && args["pageSize"].is_number_integer())
                pageSize = std::clamp<long long>(args["pageSize"].get<long long>(), 1, 200);

            Json result = server.MarshalRead([&server, page, pageSize]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                {
                    j["error"] = "No active scene";
                    return j;
                }
                JoltScene* physics = GetRunningPhysics(scene);
                j["physicsRunning"] = physics != nullptr;

                std::vector<Entity> bodies;
                for (const auto handle : scene->GetAllEntitiesWith<Rigidbody3DComponent>())
                    bodies.push_back(Entity{ handle, scene.get() });

                const auto total = static_cast<long long>(bodies.size());
                // Guard the page*pageSize multiply against overflow: any page beyond
                // the data is an empty page (start == total), never a negative index.
                const long long start = (page > total / pageSize) ? total : page * pageSize;
                Json colliders = Json::array();
                for (long long i = start; i < total && i < start + pageSize; ++i)
                {
                    Entity entity = bodies[static_cast<sizet>(i)];
                    const auto& rb = entity.GetComponent<Rigidbody3DComponent>();

                    Json e;
                    e["id"] = UuidToString(entity.GetUUID());
                    e["name"] = EntityName(entity);
                    e["bodyType"] = PhysicsExplain::BodyTypeName(MapBodyType(rb.m_Type));
                    e["layerId"] = rb.m_LayerID;
                    e["isTrigger"] = rb.m_IsTrigger;
                    e["disableGravity"] = rb.m_DisableGravity;
                    e["colliders"] = DescribeColliders(entity);
                    e["hasCollider"] = HasAnyCollider3D(entity);

                    if (physics)
                    {
                        if (Ref<JoltBody> body = physics->GetBody(entity); body && body->IsValid())
                        {
                            const JPH::ObjectLayer objLayer = physics->GetBodyInterface().GetObjectLayer(body->GetBodyID());
                            Json live;
                            live["bodyType"] = PhysicsExplain::BodyTypeName(MapBodyType(body->GetBodyType()));
                            live["objectLayer"] = static_cast<u32>(objLayer);
                            live["objectLayerName"] = ObjectLayerName(objLayer);
                            live["isTrigger"] = body->IsTrigger();
                            const glm::vec3 p = body->GetPosition();
                            live["position"] = Json::array({ p.x, p.y, p.z });
                            live["active"] = body->IsActive();
                            live["sleeping"] = body->IsSleeping();
                            e["live"] = std::move(live);
                        }
                        else
                        {
                            e["live"] = nullptr; // authored but no live body (creation failed / added at runtime)
                        }
                    }
                    colliders.push_back(std::move(e));
                }

                j["total"] = total;
                j["page"] = page;
                j["pageSize"] = pageSize;
                j["returned"] = static_cast<int>(colliders.size());
                if (start + pageSize < total)
                    j["nextPage"] = page + 1;
                j["colliders"] = std::move(colliders);
                return j; });

            if (result.contains("error"))
                return ToolResult::Error(result["error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_physics_contacts (main-marshaled) -----------------------------
        // The entity pairs whose bodies are touching right now, from the contact
        // listener's active-contact set (deduplicated per entity pair).
        ToolResult Handle_PhysicsContacts(McpServer& server, const Json& args)
        {
            int maxResults = 200;
            if (args.contains("maxResults") && args["maxResults"].is_number_integer())
                maxResults = static_cast<int>(std::clamp<long long>(args["maxResults"].get<long long>(), 1, 2000));

            Json result = server.MarshalRead([&server, maxResults]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                {
                    j["error"] = "No active scene";
                    return j;
                }
                JoltScene* physics = GetRunningPhysics(scene);
                j["physicsRunning"] = physics != nullptr;
                if (!physics)
                {
                    j["activeContactCount"] = 0;
                    j["contacts"] = Json::array();
                    j["note"] = "Physics is not running — enter Play mode to observe live contacts.";
                    return j;
                }

                const std::vector<std::pair<UUID, UUID>> pairs = physics->GetActiveContactPairs();
                j["activeContactCount"] = static_cast<std::uint64_t>(pairs.size());

                Json contacts = Json::array();
                const auto describe = [&scene](UUID id) -> Json
                {
                    Json e{ { "id", UuidToString(id) } };
                    if (const auto entityOpt = scene->TryGetEntityWithUUID(id))
                        e["name"] = EntityName(*entityOpt);
                    return e;
                };
                int emitted = 0;
                for (const auto& [a, b] : pairs)
                {
                    if (emitted++ >= maxResults)
                        break;
                    contacts.push_back(Json{ { "a", describe(a) }, { "b", describe(b) } });
                }
                if (static_cast<int>(pairs.size()) > maxResults)
                    j["truncated"] = true;
                j["returned"] = static_cast<int>(contacts.size());
                j["contacts"] = std::move(contacts);
                return j; });

            if (result.contains("error"))
                return ToolResult::Error(result["error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_physics_raycast (main-marshaled) ------------------------------
        // Cast a ray through the live physics world. Origin + (direction | to).
        // Returns the closest hit by default, or up to maxHits ordered hits.
        ToolResult Handle_PhysicsRaycast(McpServer& server, const Json& args)
        {
            glm::vec3 origin{ 0.0f };
            if (!args.contains("origin") || !ParseVec3(args["origin"], origin))
                return ToolResult::Error("Missing or invalid 'origin': expected [x, y, z] finite numbers.");

            glm::vec3 direction{ 0.0f, 0.0f, 1.0f };
            glm::vec3 toPoint{ 0.0f };
            const bool hasDirKey = args.contains("direction");
            const bool hasToKey = args.contains("to");
            // Require exactly one of 'direction' / 'to', and reject a present-but-
            // malformed field rather than silently falling back — keep the query
            // intent unambiguous and surface bad input instead of ignoring it.
            if (hasDirKey && hasToKey)
                return ToolResult::Error("Provide either 'direction' or 'to', not both.");
            if (!hasDirKey && !hasToKey)
                return ToolResult::Error("Provide either 'direction' [x,y,z] or 'to' [x,y,z].");
            if (hasDirKey && !ParseVec3(args["direction"], direction))
                return ToolResult::Error("Invalid 'direction': expected [x, y, z] finite numbers.");
            if (hasToKey && !ParseVec3(args["to"], toPoint))
                return ToolResult::Error("Invalid 'to': expected [x, y, z] finite numbers.");
            const bool hasTo = hasToKey;
            if (hasDirKey && glm::length(direction) <= 0.0f)
                return ToolResult::Error("'direction' must be non-zero.");

            f32 maxDistance = 500.0f;
            if (args.contains("maxDistance") && args["maxDistance"].is_number())
            {
                const f32 d = args["maxDistance"].get<f32>();
                if (std::isfinite(d) && d > 0.0f)
                    maxDistance = d;
            }

            int maxHits = 1;
            if (args.contains("maxHits") && args["maxHits"].is_number_integer())
                maxHits = static_cast<int>(std::clamp<long long>(args["maxHits"].get<long long>(), 1, 64));

            Json result = server.MarshalRead([&server, origin, direction, toPoint, hasTo, maxDistance, maxHits]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                JoltScene* physics = GetRunningPhysics(scene);
                if (!physics)
                    return Json{ { "__error", "Physics is not running — enter Play mode to run physics queries." } };

                RayCastInfo info = hasTo ? SceneQueryUtils::CreateRayInfo(origin, toPoint)
                                         : RayCastInfo(origin, glm::normalize(direction), maxDistance);

                const auto describeHit = [&scene](const SceneQueryHit& hit) -> Json
                {
                    Json h;
                    h["entity"] = Json{ { "id", UuidToString(hit.m_HitEntity) } };
                    if (const auto entityOpt = scene->TryGetEntityWithUUID(hit.m_HitEntity))
                        h["entity"]["name"] = EntityName(*entityOpt);
                    h["position"] = Json::array({ hit.m_Position.x, hit.m_Position.y, hit.m_Position.z });
                    h["normal"] = Json::array({ hit.m_Normal.x, hit.m_Normal.y, hit.m_Normal.z });
                    h["distance"] = hit.m_Distance;
                    return h;
                };

                Json hits = Json::array();
                if (maxHits <= 1)
                {
                    SceneQueryHit hit;
                    if (physics->CastRay(info, hit))
                        hits.push_back(describeHit(hit));
                }
                else
                {
                    std::vector<SceneQueryHit> buffer(static_cast<sizet>(maxHits));
                    const i32 count = physics->CastRayMultiple(info, buffer.data(), maxHits);
                    for (i32 k = 0; k < count; ++k)
                        hits.push_back(describeHit(buffer[static_cast<sizet>(k)]));
                }

                j["origin"] = Json::array({ info.m_Origin.x, info.m_Origin.y, info.m_Origin.z });
                j["direction"] = Json::array({ info.m_Direction.x, info.m_Direction.y, info.m_Direction.z });
                j["maxDistance"] = info.m_MaxDistance;
                j["hitCount"] = static_cast<int>(hits.size());
                j["hits"] = std::move(hits);
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_physics_overlap (main-marshaled) ------------------------------
        // Find bodies overlapping a sphere (default) or box at a world point.
        ToolResult Handle_PhysicsOverlap(McpServer& server, const Json& args)
        {
            glm::vec3 origin{ 0.0f };
            if (!args.contains("origin") || !ParseVec3(args["origin"], origin))
                return ToolResult::Error("Missing or invalid 'origin': expected [x, y, z] finite numbers.");

            // Box if halfExtents given, else sphere (radius, default 0.5). A present-
            // but-malformed halfExtents is an error, not a silent downgrade to sphere.
            glm::vec3 halfExtents{ 0.5f };
            const bool isBox = args.contains("halfExtents");
            if (isBox && !ParseVec3(args["halfExtents"], halfExtents))
                return ToolResult::Error("Invalid 'halfExtents': expected [x, y, z] finite numbers.");
            if (isBox && (halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f))
                return ToolResult::Error("Invalid 'halfExtents': all components must be positive (a box needs non-zero size).");
            f32 radius = 0.5f;
            if (args.contains("radius") && args["radius"].is_number())
            {
                const f32 r = args["radius"].get<f32>();
                if (std::isfinite(r) && r > 0.0f)
                    radius = r;
            }

            int maxHits = 32;
            if (args.contains("maxHits") && args["maxHits"].is_number_integer())
                maxHits = static_cast<int>(std::clamp<long long>(args["maxHits"].get<long long>(), 1, 256));

            Json result = server.MarshalRead([&server, origin, halfExtents, radius, isBox, maxHits]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                JoltScene* physics = GetRunningPhysics(scene);
                if (!physics)
                    return Json{ { "__error", "Physics is not running — enter Play mode to run physics queries." } };

                std::vector<SceneQueryHit> buffer(static_cast<sizet>(maxHits));
                i32 count = 0;
                if (isBox)
                {
                    BoxOverlapInfo info(origin, halfExtents);
                    count = physics->OverlapBox(info, buffer.data(), maxHits);
                }
                else
                {
                    SphereOverlapInfo info(origin, radius);
                    count = physics->OverlapSphere(info, buffer.data(), maxHits);
                }

                Json overlaps = Json::array();
                for (i32 k = 0; k < count; ++k)
                {
                    const SceneQueryHit& hit = buffer[static_cast<sizet>(k)];
                    Json e{ { "id", UuidToString(hit.m_HitEntity) } };
                    if (const auto entityOpt = scene->TryGetEntityWithUUID(hit.m_HitEntity))
                        e["name"] = EntityName(*entityOpt);
                    e["position"] = Json::array({ hit.m_Position.x, hit.m_Position.y, hit.m_Position.z });
                    overlaps.push_back(std::move(e));
                }

                j["shape"] = isBox ? "box" : "sphere";
                j["origin"] = Json::array({ origin.x, origin.y, origin.z });
                if (isBox)
                    j["halfExtents"] = Json::array({ halfExtents.x, halfExtents.y, halfExtents.z });
                else
                    j["radius"] = radius;
                j["overlapCount"] = static_cast<int>(overlaps.size());
                if (count >= maxHits)
                    j["truncated"] = true;
                j["overlaps"] = std::move(overlaps);
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_physics_why_no_collision (main-marshaled) ---------------------
        // The headline tool: explain why two entities are NOT colliding (the
        // "player falls through the floor" case). Gathers the collision-relevant
        // facts off the live sim, then runs the pure ExplainWhyNoCollision cascade.
        ToolResult Handle_PhysicsWhyNoCollision(McpServer& server, const Json& args)
        {
            if (!args.contains("a") || !args.contains("b"))
                return ToolResult::Error("Missing required arguments 'a' and 'b' (entity UUIDs).");
            u64 idA = 0;
            u64 idB = 0;
            if (!ParseUuid(args["a"], idA))
                return ToolResult::Error("Invalid 'a': expected a UUID as a string or number.");
            if (!ParseUuid(args["b"], idB))
                return ToolResult::Error("Invalid 'b': expected a UUID as a string or number.");

            Json result = server.MarshalRead([&server, idA, idB]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                JoltScene* physics = GetRunningPhysics(scene);

                PhysicsExplain::WhyNoCollisionInput in;
                in.SameEntity = (idA == idB);
                in.PhysicsRunning = physics != nullptr;

                // Gather one side's facts. Returns the live body (or null) so the
                // caller can compute the cross-body layer/bounds checks.
                Ref<JoltBody> bodyA;
                Ref<JoltBody> bodyB;
                const auto gather = [&](u64 id, PhysicsExplain::EntityPhysicsFacts& facts, Ref<JoltBody>& outBody)
                {
                    const auto entityOpt = scene->TryGetEntityWithUUID(UUID(id));
                    facts.EntityExists = entityOpt.has_value();
                    if (!facts.EntityExists)
                        return;
                    Entity entity = *entityOpt;
                    facts.HasRigidbody = entity.HasComponent<Rigidbody3DComponent>();
                    facts.HasCollider = HasAnyCollider3D(entity);

                    if (facts.HasRigidbody)
                    {
                        const auto& rb = entity.GetComponent<Rigidbody3DComponent>();
                        facts.Type = MapBodyType(rb.m_Type);
                        facts.IsTrigger = rb.m_IsTrigger;
                        facts.LayerId = rb.m_LayerID;
                        // Authored object-layer name as a fallback when no live body.
                        facts.LayerName = ObjectLayerName(JoltLayerInterface::GetObjectLayerForCollider(rb.m_LayerID, ToEBodyType(rb.m_Type), rb.m_IsTrigger));
                    }

                    if (physics)
                    {
                        if (Ref<JoltBody> body = physics->GetBody(entity); body && body->IsValid())
                        {
                            outBody = body;
                            facts.HasBody = true;
                            facts.Type = MapBodyType(body->GetBodyType());
                            facts.IsTrigger = body->IsTrigger();
                            const JPH::ObjectLayer objLayer = physics->GetBodyInterface().GetObjectLayer(body->GetBodyID());
                            facts.LayerName = ObjectLayerName(objLayer);
                        }
                    }
                };

                gather(idA, in.A, bodyA);
                gather(idB, in.B, bodyB);

                // Cross-body checks only make sense once both have live bodies.
                if (physics && bodyA && bodyB && bodyA->IsValid() && bodyB->IsValid())
                {
                    const JPH::BodyInterface& bi = physics->GetBodyInterface();
                    const JPH::ObjectLayer layerA = bi.GetObjectLayer(bodyA->GetBodyID());
                    const JPH::ObjectLayer layerB = bi.GetObjectLayer(bodyB->GetBodyID());
                    in.LayersCollide = JoltLayerInterface::GetObjectLayerPairFilter().ShouldCollide(layerA, layerB);

                    const JPH::BodyLockInterface& lockInterface = physics->GetBodyLockInterface();
                    JPH::AABox boundsA;
                    JPH::AABox boundsB;
                    bool haveA = false;
                    bool haveB = false;
                    {
                        JPH::BodyLockRead lockA(lockInterface, bodyA->GetBodyID());
                        if (lockA.Succeeded())
                        {
                            boundsA = lockA.GetBody().GetWorldSpaceBounds();
                            haveA = true;
                        }
                    }
                    {
                        JPH::BodyLockRead lockB(lockInterface, bodyB->GetBodyID());
                        if (lockB.Succeeded())
                        {
                            boundsB = lockB.GetBody().GetWorldSpaceBounds();
                            haveB = true;
                        }
                    }
                    in.BoundsOverlap = haveA && haveB && boundsA.Overlaps(boundsB);
                }

                const PhysicsExplain::WhyNoCollisionVerdict verdict = PhysicsExplain::ExplainWhyNoCollision(in);

                const auto factsJson = [](const PhysicsExplain::EntityPhysicsFacts& f) -> Json
                {
                    return Json{ { "entityExists", f.EntityExists },
                                 { "hasRigidbody", f.HasRigidbody },
                                 { "hasCollider", f.HasCollider },
                                 { "hasLiveBody", f.HasBody },
                                 { "bodyType", PhysicsExplain::BodyTypeName(f.Type) },
                                 { "isTrigger", f.IsTrigger },
                                 { "layerId", f.LayerId },
                                 { "layerName", f.LayerName } };
                };

                j["a"] = UuidToString(UUID(idA));
                j["b"] = UuidToString(UUID(idB));
                j["reasonCode"] = verdict.ReasonCode;
                j["summary"] = verdict.Summary;
                j["canCollide"] = verdict.CanCollide;
                j["checks"] = verdict.Checks;
                j["facts"] = Json{ { "a", factsJson(in.A) }, { "b", factsJson(in.B) }, { "layersCollide", in.LayersCollide }, { "boundsOverlap", in.BoundsOverlap } };
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // Keep the schema's layer cap aligned with the engine's object-layer budget.
        // A user layer id maps to the Jolt object layer ObjectLayers::NUM_LAYERS + id,
        // and the whole budget is JoltUtils::kMaxJoltLayers; the largest authored id
        // that still fits a valid slot is kMaxJoltLayers - NUM_LAYERS - 1. The shared
        // header keeps kMaxLayerId Jolt-free (it compiles into the test binary), so pin
        // it here — where the engine constants are in scope — and fail the build if the
        // budget ever changes rather than silently letting an out-of-budget layer through.
        static_assert(SetCollisionLayer::kMaxLayerId == JoltUtils::kMaxJoltLayers - ObjectLayers::NUM_LAYERS - 1,
                      "olo_set_collision_layer layer cap is out of sync with the Jolt object-layer budget");

        // ---- olo_set_collision_layer (main-marshaled; PROJECT WRITE) -----------
        // The first consented, undoable write tool (#306 item C, first slice): set an
        // entity's physics-body collision layer through the editor's undo stack, so an
        // agent can try a fix the user can Ctrl-Z. The mutation is gated at dispatch by
        // the "Allow writes" session toggle (ToolDef::ProjectWrite); the shared apply
        // logic lives in McpSetCollisionLayer.h so it is unit-tested at the dispatch
        // seam without this TU. The command is built + executed inside the MarshalRead
        // job, i.e. on the main thread, since it touches the EnTT registry and the
        // editor command stack.
        ToolResult Handle_SetCollisionLayer(McpServer& server, const Json& args)
        {
            if (!server.Context().GetActiveScene || !server.Context().GetCommandHistory)
                return ToolResult::Error("Project writes are not available in this editor build.");

            u64 entityUuid = 0;
            u32 layer = 0;
            if (const auto error = SetCollisionLayer::ParseArgs(args, entityUuid, layer))
                return ToolResult::Error(*error);

            const Json result = server.MarshalRead([&server, entityUuid, layer]() -> Json
                                                   {
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                CommandHistory* history = server.Context().GetCommandHistory
                                              ? server.Context().GetCommandHistory()
                                              : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                if (!history)
                    return Json{ { "__error", "No editor command history available." } };

                const SetCollisionLayer::ApplyResult applied = SetCollisionLayer::Apply(scene, *history, entityUuid, layer);
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                return applied.Data; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

    } // namespace

    void RegisterPhysicsTools(McpServer& server)
    {
        {
            ToolDef tool;
            tool.Name = "olo_physics_layer_matrix";
            tool.Toolset = "physics";
            tool.Title = "Physics layer matrix";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Dump the physics collision-layer matrix the simulation actually uses: the five built-in "
                "Jolt object layers (NON_MOVING, MOVING, TRIGGER, CHARACTER, DEBRIS) plus every user-defined "
                "physics layer, with the pairwise collide/no-collide result from the real layer filter. Use "
                "this to confirm whether two layers are even allowed to collide. Works in Edit mode too.";
            tool.InputSchema = Schema::EmptyObject();
            tool.MainMarshaled = false;
            tool.Handler = Handle_PhysicsLayerMatrix;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_list_colliders";
            tool.Toolset = "physics";
            tool.Title = "List physics colliders";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List every entity with a Rigidbody3DComponent (paginated): authored body type "
                "(Static/Dynamic/Kinematic), collision layer id, trigger flag, and collider shape(s). When "
                "physics is running, also reports the live body's object layer, world position, and "
                "awake/asleep state. Pair with olo_physics_why_no_collision to debug missing collisions.";
            tool.InputSchema = Schema::Object()
                                   .Pagination("Entities per page (default 50, max 200).")
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsListColliders;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_contacts";
            tool.Toolset = "physics";
            tool.Title = "List physics contacts";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List the entity pairs whose physics bodies are touching right now (live active-contact set, "
                "deduplicated per pair). Requires Play mode. Use this to confirm a collision/trigger is "
                "actually being detected by the engine.";
            tool.InputSchema = Schema::Object()
                                   .Prop("maxResults", Schema::Int().Min(1).Max(2000).Desc("Max contact pairs to return (default 200)."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsContacts;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_raycast";
            tool.Toolset = "physics";
            tool.Title = "Physics raycast";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Cast a ray through the live physics world and return what it hits. Specify 'origin' plus "
                "either 'direction' (a vector) or 'to' (an end point). Returns the closest hit by default, or "
                "up to 'maxHits' ordered hits, each with the hit entity, world position, surface normal, and "
                "distance. Requires Play mode.";
            tool.InputSchema = Schema::Object()
                                   .Prop("origin", Schema::Array().Desc("Ray start [x, y, z]."))
                                   .Prop("direction", Schema::Array().Desc("Ray direction [x, y, z] (need not be normalised). Provide this or 'to'."))
                                   .Prop("to", Schema::Array().Desc("Ray end point [x, y, z]; sets direction and distance. Provide this or 'direction'."))
                                   .Prop("maxDistance", Schema::Number().Desc("Max ray length when using 'direction' (default 500)."))
                                   .Prop("maxHits", Schema::Int().Min(1).Max(64).Desc("Return up to N ordered hits (default 1 = closest only)."))
                                   .Required({ "origin" })
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("origin", Schema::Array(Schema::Number()).Desc("Resolved ray origin [x, y, z]."))
                                    .Prop("direction", Schema::Array(Schema::Number()).Desc("Resolved normalised ray direction [x, y, z]."))
                                    .Prop("maxDistance", Schema::Number().Desc("Resolved ray length."))
                                    .Prop("hitCount", Schema::Int().Min(0).Desc("Number of hits returned."))
                                    .Prop("hits", Schema::Array(Schema::Object()
                                                                    .Prop("entity", Schema::Object()
                                                                                        .Prop("id", Schema::String())
                                                                                        .Prop("name", Schema::String()))
                                                                    .Prop("position", Schema::Array(Schema::Number()))
                                                                    .Prop("normal", Schema::Array(Schema::Number()))
                                                                    .Prop("distance", Schema::Number()))
                                                      .Desc("Hits ordered nearest-first."))
                                    .Required({ "origin", "direction", "maxDistance", "hitCount", "hits" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsRaycast;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_overlap";
            tool.Toolset = "physics";
            tool.Title = "Physics overlap query";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Find the physics bodies overlapping a shape at a world point. Pass 'origin' plus 'radius' "
                "for a sphere (the default), or 'halfExtents' [x,y,z] for a box. Returns the overlapping "
                "entities and their positions. Requires Play mode.";
            tool.InputSchema = Schema::Object()
                                   .Prop("origin", Schema::Array().Desc("Query centre [x, y, z]."))
                                   .Prop("radius", Schema::Number().Desc("Sphere radius (default 0.5; ignored if 'halfExtents' is given)."))
                                   .Prop("halfExtents", Schema::Array().Desc("Box half-extents [x, y, z]; selects a box query instead of a sphere."))
                                   .Prop("maxHits", Schema::Int().Min(1).Max(256).Desc("Max overlapping bodies to return (default 32)."))
                                   .Required({ "origin" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsOverlap;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_why_no_collision";
            tool.Toolset = "physics";
            tool.Title = "Explain missing collision";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Explain why two entities are NOT colliding — the 'player falls through the floor' debugger. "
                "Given two entity UUIDs ('a' and 'b'), it checks, in order: physics running, both entities "
                "exist, both have a rigidbody + collider + live body, not both Static, their collision layers "
                "are allowed to collide, neither is a trigger, and their bounds overlap. Returns the root-cause "
                "reasonCode, a human summary, the ordered checks performed, and the raw facts for each entity.";
            tool.InputSchema = Schema::Object()
                                   .Prop("a", Schema::String().Desc("First entity UUID (string; also accepts a number)."))
                                   .Prop("b", Schema::String().Desc("Second entity UUID (string; also accepts a number)."))
                                   .Required({ "a", "b" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsWhyNoCollision;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_set_collision_layer";
            tool.Toolset = "physics";
            tool.Title = "Set collision layer (undoable)";
            // The first project-WRITE tool: gated behind the session "Allow writes"
            // toggle and routed through the editor undo stack. readOnlyHint:false (not
            // idempotent — each call snapshots the prior layer into a distinct undo
            // command; not destructive — fully reversible via Ctrl-Z / undo).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Set the collision layer of an entity's physics body — the undoable fix half of the "
                "olo_physics_* debugging story (e.g. after olo_physics_why_no_collision blames the layer "
                "filter). Targets the entity's Rigidbody3DComponent (or CharacterController3DComponent) "
                "'m_LayerID'. The change is applied through the editor's undo stack, so it is a single "
                "Ctrl-Z. This is a WRITE tool: it is refused unless 'Allow writes' is enabled in the "
                "editor's MCP Server panel (off by default). Discover valid layer ids with "
                "olo_physics_layer_matrix.";
            tool.InputSchema = SetCollisionLayer::InputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SetCollisionLayer;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
