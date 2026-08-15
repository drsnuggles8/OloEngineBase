#include "OloEnginePCH.h"
#include "NodeBuilders.h"

#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <cmath>
#include <optional>

namespace OloEngine::VisualScript
{
    using namespace Builders;

    namespace
    {
        // Every ECS node funnels through this. A graph running with no Scene (a
        // bare VM unit test) or naming a dead entity reports an error on the
        // instance instead of dereferencing — the alternative is a crash inside a
        // node body, with no indication which node.
        std::optional<Entity> ResolveEntity(const NodeContext& ctx, UUID id, const char* what)
        {
            Scene* scene = ctx.GetScene();
            if (scene == nullptr)
            {
                ctx.Error(std::string(what) + " needs a Scene; none is attached");
                return std::nullopt;
            }
            std::optional<Entity> entity = scene->TryGetEntityWithUUID(id);
            if (!entity.has_value())
            {
                ctx.Error(std::string(what) + " targets entity " + std::to_string(static_cast<u64>(id)) + ", which does not exist");
                return std::nullopt;
            }
            return entity;
        }

        // Add/Remove Component works over a curated table rather than every
        // component in the generated AllComponents tuple: an EnTT type is not
        // reachable from a runtime string without a name->type dispatch, and the
        // engine has none (the generated MCP field registry is editor-side).
        // Adding a row here is the whole cost of exposing another component; a
        // name outside the table is reported, never silently ignored.
        struct ComponentOp
        {
            const char* m_Name;
            bool (*m_Has)(Entity&);
            void (*m_Add)(Entity&);
            void (*m_Remove)(Entity&);
        };

        template<typename T>
        ComponentOp MakeComponentOp(const char* name)
        {
            return ComponentOp{
                name,
                [](Entity& e)
                { return e.HasComponent<T>(); },
                [](Entity& e)
                {
                    if (!e.HasComponent<T>())
                    {
                        e.AddComponent<T>();
                    }
                },
                [](Entity& e)
                {
                    if (e.HasComponent<T>())
                    {
                        e.RemoveComponent<T>();
                    }
                }
            };
        }

        const std::vector<ComponentOp>& ComponentOps()
        {
            static const std::vector<ComponentOp> s_Ops = {
                MakeComponentOp<TransformComponent>("Transform"),
                MakeComponentOp<TagComponent>("Tag"),
                MakeComponentOp<SpriteRendererComponent>("SpriteRenderer"),
                MakeComponentOp<MeshComponent>("Mesh"),
                MakeComponentOp<CameraComponent>("Camera"),
                MakeComponentOp<AudioSourceComponent>("AudioSource"),
                MakeComponentOp<Rigidbody3DComponent>("Rigidbody3D"),
                MakeComponentOp<BoxCollider3DComponent>("BoxCollider3D"),
                MakeComponentOp<SphereCollider3DComponent>("SphereCollider3D"),
                MakeComponentOp<NavAgentComponent>("NavAgent"),
                MakeComponentOp<BoidComponent>("Boid"),
                MakeComponentOp<DialogueComponent>("Dialogue"),
                MakeComponentOp<InventoryComponent>("Inventory"),
                MakeComponentOp<QuestJournalComponent>("QuestJournal"),
                MakeComponentOp<ProgressionComponent>("Progression"),
                MakeComponentOp<LuaScriptComponent>("LuaScript"),
            };
            return s_Ops;
        }

        const ComponentOp* FindComponentOp(const std::string& name)
        {
            for (const ComponentOp& op : ComponentOps())
            {
                if (name == op.m_Name)
                {
                    return &op;
                }
            }
            return nullptr;
        }
    } // namespace

    void RegisterEntityNodes(NodeRegistry& registry)
    {
        //-- Identity -------------------------------------------------------------
        RegisterPureNode(registry, "Entity.GetSelf", "Get Self", "Entity", "The entity this graph runs on.",
                         { Out("Self", PinType::Entity) },
                         [](NodeContext& ctx)
                         { ctx.SetOutput(0, PinValue::MakeEntity(ctx.GetEntityID())); });

        RegisterPureNode(registry, "Entity.GetTag", "Get Tag", "Entity", "The target entity's tag (name).",
                         { In("Target", PinType::Entity), Out("Tag", PinType::String) },
                         [](NodeContext& ctx)
                         {
                             std::optional<Entity> entity = ResolveEntity(ctx, ctx.GetInputEntity(0), "Get Tag");
                             if (!entity.has_value())
                             {
                                 return;
                             }
                             ctx.SetOutput(1, PinValue::MakeString(entity->HasComponent<TagComponent>() ? entity->GetComponent<TagComponent>().Tag : std::string{}));
                         });

        RegisterPureNode(registry, "Entity.FindByTag", "Find Entity By Tag", "Entity",
                         "The first entity whose tag matches, if any.",
                         { In("Tag", PinValue::MakeString({})), Out("Entity", PinType::Entity), Out("Found", PinType::Bool) },
                         [](NodeContext& ctx)
                         {
                             Scene* scene = ctx.GetScene();
                             if (scene == nullptr)
                             {
                                 ctx.Error("Find Entity By Tag needs a Scene; none is attached");
                                 return;
                             }
                             Entity found = scene->FindEntityByName(ctx.GetInputString(0));
                             ctx.SetOutput(1, PinValue::MakeEntity(found ? found.GetUUID() : UUID(0)));
                             ctx.SetOutput(2, PinValue::MakeBool(static_cast<bool>(found)));
                         });

        //-- Transform reads ------------------------------------------------------
        const auto registerTransformGetter = [&registry](std::string typeName, std::string displayName, std::string tooltip,
                                                         glm::vec3 (*read)(const TransformComponent&))
        {
            RegisterPureNode(registry, std::move(typeName), std::move(displayName), "Entity", std::move(tooltip),
                             { In("Target", PinType::Entity), Out("Value", PinType::Vec3) },
                             [read](NodeContext& ctx)
                             {
                                 std::optional<Entity> entity = ResolveEntity(ctx, ctx.GetInputEntity(0), "Transform getter");
                                 if (!entity.has_value() || !entity->HasComponent<TransformComponent>())
                                 {
                                     return;
                                 }
                                 ctx.SetOutput(1, PinValue::MakeVec3(read(entity->GetComponent<TransformComponent>())));
                             });
        };

        registerTransformGetter("Entity.GetTranslation", "Get Translation", "The target's local translation.",
                                [](const TransformComponent& t)
                                { return t.Translation; });
        registerTransformGetter("Entity.GetRotation", "Get Rotation", "The target's local rotation, in radians.",
                                [](const TransformComponent& t)
                                { return t.GetRotationEuler(); });
        registerTransformGetter("Entity.GetScale", "Get Scale", "The target's local scale.",
                                [](const TransformComponent& t)
                                { return t.Scale; });

        //-- Transform writes -----------------------------------------------------
        const auto registerTransformSetter = [&registry](std::string typeName, std::string displayName, std::string tooltip,
                                                         PinValue defaultValue, void (*write)(TransformComponent&, const glm::vec3&))
        {
            RegisterExecNode(registry, std::move(typeName), std::move(displayName), "Entity", std::move(tooltip),
                             { ExecIn(), In("Target", PinType::Entity), In("Value", std::move(defaultValue)), ExecOut() },
                             [write](NodeContext& ctx)
                             {
                                 std::optional<Entity> entity = ResolveEntity(ctx, ctx.GetInputEntity(1), "Transform setter");
                                 if (entity.has_value() && entity->HasComponent<TransformComponent>())
                                 {
                                     // GetInputVec3 already went through
                                     // PinValue's non-finite sanitisation, so a
                                     // NaN authored upstream cannot reach the
                                     // transform and poison the render matrix.
                                     write(entity->GetComponent<TransformComponent>(), ctx.GetInputVec3(2));
                                 }
                                 ctx.Trigger(3);
                             });
        };

        registerTransformSetter("Entity.SetTranslation", "Set Translation", "Sets the target's local translation.",
                                PinValue::MakeVec3(glm::vec3(0.0f)),
                                [](TransformComponent& t, const glm::vec3& v)
                                { t.Translation = v; });
        registerTransformSetter("Entity.SetRotation", "Set Rotation", "Sets the target's local rotation, in radians.",
                                PinValue::MakeVec3(glm::vec3(0.0f)),
                                [](TransformComponent& t, const glm::vec3& v)
                                { t.SetRotationEuler(v); });
        registerTransformSetter("Entity.SetScale", "Set Scale", "Sets the target's local scale.",
                                PinValue::MakeVec3(glm::vec3(1.0f)),
                                [](TransformComponent& t, const glm::vec3& v)
                                { t.Scale = v; });
        registerTransformSetter("Entity.Translate", "Translate", "Adds a delta to the target's local translation.",
                                PinValue::MakeVec3(glm::vec3(0.0f)),
                                [](TransformComponent& t, const glm::vec3& v)
                                { t.Translation += v; });

        //-- Components -----------------------------------------------------------
        RegisterPureNode(registry, "Entity.HasComponent", "Has Component", "Entity",
                         "Whether the target carries the named component (see the curated list in EntityNodes.cpp).",
                         { In("Target", PinType::Entity), In("Component", PinValue::MakeString({})), Out("Has", PinType::Bool) },
                         [](NodeContext& ctx)
                         {
                             const std::string name = ctx.GetInputString(1);
                             const ComponentOp* op = FindComponentOp(name);
                             if (op == nullptr)
                             {
                                 ctx.Error("Unknown component name '" + name + "'");
                                 ctx.SetOutput(2, PinValue::MakeBool(false));
                                 return;
                             }
                             std::optional<Entity> entity = ResolveEntity(ctx, ctx.GetInputEntity(0), "Has Component");
                             ctx.SetOutput(2, PinValue::MakeBool(entity.has_value() && op->m_Has(*entity)));
                         });

        const auto registerComponentMutator = [&registry](std::string typeName, std::string displayName, std::string tooltip, bool add)
        {
            // Structural, but NOT routed through the deferred entity-command
            // queue: that queue only carries entity create/destroy. This is safe
            // because VisualScriptSystem iterates a SNAPSHOT of entity UUIDs
            // rather than a live EnTT view, so mutating another component's pool
            // mid-run cannot invalidate the iteration. If that system is ever
            // changed to iterate a live view, these two nodes must move to the
            // queue first — see docs/agent-rules/script-structural-command-safe-point.md.
            RegisterExecNode(registry, std::move(typeName), std::move(displayName), "Entity", std::move(tooltip), { ExecIn(), In("Target", PinType::Entity), In("Component", PinValue::MakeString({})), ExecOut() }, [add](NodeContext& ctx)
                             {
                                 const std::string name = ctx.GetInputString(2);
                                 if (const ComponentOp* op = FindComponentOp(name); op == nullptr)
                                 {
                                     ctx.Error("Unknown component name '" + name + "'");
                                 }
                                 else if (!add && (name == "Transform" || name == "Tag"))
                                 {
                                     // Every entity is assumed to carry Transform and
                                     // Tag: transform propagation, rendering, the
                                     // spatial index, serialization and Entity::GetName
                                     // all read them unconditionally. Letting a graph
                                     // remove either turns a designer's typo into an
                                     // engine-wide crash far from the cause.
                                     ctx.Error("Refusing to remove '" + name + "' — every entity must keep it");
                                 }
                                 else if (std::optional<Entity> entity = ResolveEntity(ctx, ctx.GetInputEntity(1), "Component mutator"); entity.has_value())
                                 {
                                     if (add)
                                     {
                                         op->m_Add(*entity);
                                     }
                                     else
                                     {
                                         op->m_Remove(*entity);
                                     }
                                 }
                                 ctx.Trigger(3); }, NodeFlags::Structural);
        };

        registerComponentMutator("Entity.AddComponent", "Add Component", "Adds the named component to the target if absent.", true);
        registerComponentMutator("Entity.RemoveComponent", "Remove Component", "Removes the named component from the target if present.", false);

        //-- Spawn / destroy (deferred) -------------------------------------------
        RegisterExecNode(registry, "Entity.SpawnPrefab", "Spawn Prefab", "Entity", "Queues a prefab instantiation. The entity materialises at this tick's command drain; its "
                                                                                   "components are readable from the next tick.",
                         { ExecIn(), In("Prefab", PinType::Asset), In("Location", PinValue::MakeVec3(glm::vec3(0.0f))), In("Rotation", PinValue::MakeVec3(glm::vec3(0.0f))), In("Scale", PinValue::MakeVec3(glm::vec3(1.0f))), ExecOut(), Out("Spawned", PinType::Entity) }, [](NodeContext& ctx)
                         {
                             Scene* scene = ctx.GetScene();
                             const UUID prefab = ctx.GetInput(1).AsAsset();
                             if (scene == nullptr || static_cast<u64>(prefab) == 0)
                             {
                                 ctx.Error("Spawn Prefab needs a Scene and a non-null prefab handle");
                                 ctx.SetOutput(6, PinValue::MakeEntity(UUID(0)));
                                 ctx.Trigger(5);
                                 return;
                             }
                             const UUID spawned = scene->ScriptInstantiatePrefab(prefab, ctx.GetInputVec3(2), ctx.GetInputVec3(3), ctx.GetInputVec3(4));
                             ctx.SetOutput(6, PinValue::MakeEntity(spawned));
                             ctx.Trigger(5); }, NodeFlags::Structural);

        RegisterExecNode(registry, "Entity.Destroy", "Destroy Entity", "Entity", "Queues destruction of the target and its children. Applied at this tick's command drain.", { ExecIn(), In("Target", PinType::Entity), ExecOut() }, [](NodeContext& ctx)
                         {
                             if (Scene* scene = ctx.GetScene(); scene != nullptr)
                             {
                                 scene->ScriptDestroyEntity(ctx.GetInputEntity(1));
                             }
                             else
                             {
                                 ctx.Error("Destroy Entity needs a Scene; none is attached");
                             }
                             ctx.Trigger(2); }, NodeFlags::Structural);

        //-- Queries --------------------------------------------------------------
        RegisterExecNode(registry, "Entity.Raycast", "Raycast", "Queries",
                         "Casts a ray through the 3D physics world and branches on whether it hit.",
                         { ExecIn(), In("Origin", PinValue::MakeVec3(glm::vec3(0.0f))),
                           In("Direction", PinValue::MakeVec3(glm::vec3(0.0f, 0.0f, -1.0f))),
                           In("Max Distance", PinValue::MakeFloat(100.0f)),
                           ExecOut("Hit"), ExecOut("Miss"),
                           Out("Hit Entity", PinType::Entity), Out("Hit Point", PinType::Vec3), Out("Hit Distance", PinType::Float) },
                         [](NodeContext& ctx)
                         {
                             Scene* scene = ctx.GetScene();
                             JoltScene* physics = scene != nullptr ? scene->GetPhysicsScene() : nullptr;
                             if (physics == nullptr)
                             {
                                 // No physics world is a legitimate state (a 2D
                                 // scene, a headless harness) — take Miss rather
                                 // than logging an error every tick.
                                 ctx.Trigger(5);
                                 return;
                             }

                             // Same squared-length epsilon Vector.Normalize uses: a
                             // near-zero direction makes glm::normalize produce NaNs,
                             // and a NaN ray is a hard-to-trace physics query failure
                             // rather than an obvious authoring mistake. length() on a
                             // non-finite vector is itself non-finite, so comparing it
                             // against 0 would let that case straight through.
                             const glm::vec3 direction = ctx.GetInputVec3(2);
                             const f32 lengthSquared = glm::dot(direction, direction);
                             if (!std::isfinite(lengthSquared) || lengthSquared < 1e-9f)
                             {
                                 ctx.Error("Raycast direction must be a finite, non-zero vector");
                                 ctx.Trigger(5);
                                 return;
                             }

                             RayCastInfo info(ctx.GetInputVec3(1), glm::normalize(direction), ctx.GetInputFloat(3));
                             SceneQueryHit hit;
                             if (!physics->CastRay(info, hit) || !hit.HasHit())
                             {
                                 ctx.Trigger(5);
                                 return;
                             }
                             ctx.SetOutput(6, PinValue::MakeEntity(hit.m_HitEntity));
                             ctx.SetOutput(7, PinValue::MakeVec3(hit.m_Position));
                             ctx.SetOutput(8, PinValue::MakeFloat(hit.m_Distance));
                             ctx.Trigger(4);
                         });
    }

} // namespace OloEngine::VisualScript
