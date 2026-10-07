#pragma once

#include "MCP/McpSchemaBuilder.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include <box2d/box2d.h>
#include <box2d/math_functions.h>
#include <algorithm>

namespace OloEngine::MCP::Physics2D
{
    using Json = nlohmann::json;

    inline Json Vec(b2Vec2 v)
    {
        return Json::array({ v.x, v.y });
    }
    inline const char* BodyType(int type)
    {
        return type == 1 ? "Dynamic" : type == 2 ? "Kinematic"
                                                 : "Static";
    }
    inline const char* LiveBodyType(b2BodyType type)
    {
        return type == b2_dynamicBody ? "Dynamic" : type == b2_kinematicBody ? "Kinematic"
                                                                             : "Static";
    }
    inline Json ListSchema()
    {
        return Schema::Object().Prop("page", Schema::Int().Min(0).Max(1000000)).Prop("pageSize", Schema::Int().Min(1).Max(2000)).NoAdditional();
    }
    inline Json RaySchema()
    {
        return Schema::Object()
            .Prop("origin", Schema::Array(Schema::Number()).MinItems(2).MaxItems(2))
            .Prop("translation", Schema::Array(Schema::Number()).MinItems(2).MaxItems(2))
            .Required({ "origin", "translation" });
    }
    inline bool HasAuthored(const Scene& scene)
    {
        return !scene.GetAllEntitiesWith<Rigidbody2DComponent>().empty() ||
               !scene.GetAllEntitiesWith<BoxCollider2DComponent>().empty() ||
               !scene.GetAllEntitiesWith<CircleCollider2DComponent>().empty() ||
               !scene.GetAllEntitiesWith<TilemapComponent>().empty();
    }
    inline void DescribeJoltScope(Json& result, const Scene& scene)
    {
        result["backend"] = "Jolt";
        result["dimension"] = 3;
        result["hasAuthored2D"] = HasAuthored(scene);
        if (HasAuthored(scene))
            result["scopeNote"] = "This tool inspects Jolt 3D only. Box2D bodies/shapes are not included; use olo_physics2d_list_bodies, olo_physics2d_list_colliders or olo_physics2d_raycast.";
    }
    inline Json Shape(b2ShapeId id)
    {
        Json j{ { "density", b2Shape_GetDensity(id) }, { "friction", b2Shape_GetFriction(id) }, { "restitution", b2Shape_GetRestitution(id) }, { "sensor", b2Shape_IsSensor(id) } };
        const auto filter = b2Shape_GetFilter(id);
        j["categoryBits"] = std::to_string(filter.categoryBits);
        j["maskBits"] = std::to_string(filter.maskBits);
        j["groupIndex"] = filter.groupIndex;
        const auto bounds = b2Shape_GetAABB(id);
        j["aabb"] = Json{ { "min", Vec(bounds.lowerBound) }, { "max", Vec(bounds.upperBound) } };
        switch (b2Shape_GetType(id))
        {
            case b2_circleShape:
            {
                const auto c = b2Shape_GetCircle(id);
                j["type"] = "Circle";
                j["center"] = Vec(c.center);
                j["radius"] = c.radius;
                break;
            }
            case b2_polygonShape:
            {
                const auto p = b2Shape_GetPolygon(id);
                j["type"] = "Polygon";
                j["radius"] = p.radius;
                j["vertices"] = Json::array();
                for (int i = 0; i < p.count; ++i)
                    j["vertices"].push_back(Vec(p.vertices[i]));
                break;
            }
            case b2_capsuleShape:
            {
                const auto c = b2Shape_GetCapsule(id);
                j["type"] = "Capsule";
                j["center1"] = Vec(c.center1);
                j["center2"] = Vec(c.center2);
                j["radius"] = c.radius;
                break;
            }
            case b2_segmentShape:
            case b2_chainSegmentShape:
            {
                const bool chain = b2Shape_GetType(id) == b2_chainSegmentShape;
                const auto segment = chain ? b2Shape_GetChainSegment(id).segment : b2Shape_GetSegment(id);
                j["type"] = chain ? "ChainSegment" : "Segment";
                j["point1"] = Vec(segment.point1);
                j["point2"] = Vec(segment.point2);
                break;
            }
            default:
                j["type"] = "Unknown";
                break;
        }
        return j;
    }
    inline Json AuthoredShapes(Entity e)
    {
        Json arr = Json::array();
        if (e.HasComponent<BoxCollider2DComponent>())
        {
            const auto& c = e.GetComponent<BoxCollider2DComponent>();
            arr.push_back(Json{ { "type", "Box" }, { "halfExtents", Json::array({ c.Size.x, c.Size.y }) }, { "offset", Json::array({ c.Offset.x, c.Offset.y }) }, { "density", c.Density }, { "friction", c.Friction }, { "restitution", c.Restitution }, { "restitutionThreshold", c.RestitutionThreshold } });
        }
        if (e.HasComponent<CircleCollider2DComponent>())
        {
            const auto& c = e.GetComponent<CircleCollider2DComponent>();
            arr.push_back(Json{ { "type", "Circle" }, { "radius", c.Radius }, { "offset", Json::array({ c.Offset.x, c.Offset.y }) }, { "density", c.Density }, { "friction", c.Friction }, { "restitution", c.Restitution }, { "restitutionThreshold", c.RestitutionThreshold } });
        }
        return arr;
    }
    // Entity-backed rigidbodies and collider-only authoring. Tilemap bodies are
    // counted separately by the world; a ray hit on one has entity:null.
    inline Json List(Scene& scene, long long page, int pageSize, bool collidersOnly)
    {
        TArray<Entity> entities;
        for (auto id : scene.GetAllEntitiesWith<IDComponent>())
        {
            Entity e{ id, &scene };
            const bool collider = e.HasComponent<BoxCollider2DComponent>() || e.HasComponent<CircleCollider2DComponent>();
            if (collidersOnly ? collider : e.HasComponent<Rigidbody2DComponent>())
                entities.Add(e);
        }
        std::sort(entities.begin(), entities.end(), [](Entity a, Entity b)
                  { return static_cast<u64>(a.GetUUID()) < static_cast<u64>(b.GetUUID()); });
        const auto total = static_cast<long long>(entities.Num());
        const auto start = page > total / pageSize ? total : page * pageSize;
        const auto world = scene.GetPhysicsWorld2D();
        const bool running = b2World_IsValid(world);
        Json j{ { "backend", "Box2D" }, { "dimension", 2 }, { "physicsRunning", running }, { "source", running ? "live" : "authored" }, { "total", total }, { "page", page }, { "pageSize", pageSize } };
        j["note"] = "Lists entity-backed Rigidbody2D or Box/CircleCollider2D components. Tilemap-generated bodies are included in worldCounts and raycasts, not these entity lists. Authored restitutionThreshold is not applied per shape by Box2D.";
        if (running)
        {
            const auto counters = b2World_GetCounters(world);
            j["worldCounts"] = Json{ { "bodies", counters.bodyCount }, { "shapes", counters.shapeCount } };
        }
        Json rows = Json::array();
        for (long long i = start; i < total && i - start < pageSize; ++i)
        {
            Entity e = entities[static_cast<sizet>(i)];
            const auto& t = e.GetComponent<TransformComponent>();
            Json row{ { "id", std::to_string(static_cast<u64>(e.GetUUID())) }, { "name", e.GetName() }, { "position", Json::array({ t.Translation.x, t.Translation.y }) }, { "rotation", t.GetRotationEuler().z }, { "scale", Json::array({ t.Scale.x, t.Scale.y }) }, { "colliders", AuthoredShapes(e) }, { "live", nullptr } };
            row["hasRigidbody"] = e.HasComponent<Rigidbody2DComponent>();
            if (e.HasComponent<Rigidbody2DComponent>())
            {
                const auto& rb = e.GetComponent<Rigidbody2DComponent>();
                row["bodyType"] = BodyType(static_cast<int>(rb.Type));
                row["fixedRotation"] = rb.FixedRotation;
                row["linearVelocity"] = Json::array({ rb.LinearVelocity.x, rb.LinearVelocity.y });
                row["angularVelocity"] = rb.AngularVelocity;
                if (running && b2Body_IsValid(rb.RuntimeBody))
                {
                    const auto body = rb.RuntimeBody;
                    Json live{ { "position", Vec(b2Body_GetPosition(body)) }, { "rotation", b2Rot_GetAngle(b2Body_GetRotation(body)) }, { "linearVelocity", Vec(b2Body_GetLinearVelocity(body)) }, { "angularVelocity", b2Body_GetAngularVelocity(body) }, { "bodyType", LiveBodyType(b2Body_GetType(body)) }, { "awake", b2Body_IsAwake(body) }, { "enabled", b2Body_IsEnabled(body) } };
                    TArray<b2ShapeId> shapes;
                    shapes.SetNum(b2Body_GetShapeCount(body));
                    const int count = b2Body_GetShapes(body, shapes.GetData(), static_cast<int>(shapes.Num()));
                    live["colliders"] = Json::array();
                    for (int s = 0; s < count; ++s)
                        live["colliders"].push_back(Shape(shapes[s]));
                    row["live"] = std::move(live);
                }
            }
            rows.push_back(std::move(row));
        }
        j["returned"] = rows.size();
        if (start + pageSize < total)
            j["nextPage"] = page + 1;
        j[collidersOnly ? "colliders" : "bodies"] = std::move(rows);
        return j;
    }
    inline Json Raycast(Scene& scene, b2Vec2 origin, b2Vec2 translation)
    {
        const auto world = scene.GetPhysicsWorld2D();
        if (!b2World_IsValid(world))
            return Json{ { "error", "Box2D is not running; enter Play or Simulate mode to raycast the live world." } };
        const auto hit = b2World_CastRayClosest(world, origin, translation, b2DefaultQueryFilter());
        Json j{ { "backend", "Box2D" }, { "dimension", 2 }, { "hit", hit.hit } };
        if (hit.hit)
        {
            j["point"] = Vec(hit.point);
            j["normal"] = Vec(hit.normal);
            j["fraction"] = hit.fraction;
            j["shape"] = Shape(hit.shapeId);
            j["entity"] = nullptr;
            const auto body = b2Shape_GetBody(hit.shapeId);
            for (auto id : scene.GetAllEntitiesWith<Rigidbody2DComponent>())
            {
                Entity e{ id, &scene };
                if (B2_ID_EQUALS(e.GetComponent<Rigidbody2DComponent>().RuntimeBody, body))
                {
                    j["entity"] = std::to_string(static_cast<u64>(e.GetUUID()));
                    break;
                }
            }
        }
        return j;
    }
} // namespace OloEngine::MCP::Physics2D
