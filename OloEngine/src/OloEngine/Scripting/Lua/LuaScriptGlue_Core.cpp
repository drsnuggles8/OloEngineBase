#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue_Core.cpp — the component proxy, the GLM vector types, transforms, and the 2D/3D rigid-body and collider family.
//
// One of the LuaScriptGlue_*.cpp parts. See LuaScriptGlueInternal.h for why the
// glue is split and why the call order in RegisterAllTypes is load-bearing.
// =============================================================================

namespace OloEngine
{
    void LuaScriptGlue::RegisterCoreTypes(sol::state& lua)
    {

        // ── LuaComponentProxy — safe proxy that prevents dangling pointers ──
        //
        // __index :  resolve entity → T&, then forward property read / method
        //            lookup to the real component's usertype.  For methods, wrap
        //            the result in a closure that re-resolves the component on
        //            each call so the method never captures a stale T*.
        // __newindex: resolve entity → T&, then forward property write.
        lua.new_usertype<LuaComponentProxy>(
            "LuaComponentProxy", sol::no_constructor,
            sol::meta_function::index,
            [](sol::this_state s, LuaComponentProxy& proxy, const std::string& key) -> sol::object
            {
                // 1. Resolve the real component (T*) as a sol::object
                sol::object comp = proxy.ResolveComponent(s);
                if (!comp.valid() || comp.is<sol::nil_t>())
                    return sol::make_object(s, sol::nil);

                // 2. Use sol2's high-level accessor to retrieve the value.
                //    This automatically invokes property getters and member-
                //    pointer accessors, returning the resolved value.
                //    Methods are returned as sol::function objects.
                sol::table compAsTable = comp.as<sol::table>();
                sol::object result = compAsTable[key];

                // 3. If the result is a method (function), wrap it in a closure
                //    that re-resolves the component on each call so the method
                //    never captures a stale T*.
                if (result.get_type() == sol::type::function)
                {
                    sol::protected_function origFn = result.as<sol::protected_function>();
                    u64 entityID = proxy.EntityID;
                    auto resolveFn = proxy.Resolve;

                    auto wrapper = [entityID, resolveFn, origFn = std::move(origFn)](sol::variadic_args va) -> sol::protected_function_result
                    {
                        sol::this_state ws = va.lua_state();

                        // Re-resolve component fresh
                        Scene* scene = ScriptEngine::GetSceneContext();
                        if (!scene)
                            return origFn(sol::nil);
                        auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID));
                        if (!entityOpt)
                            return origFn(sol::nil);
                        Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                        sol::object freshComp = resolveFn(entity, ws);

                        // Strip the proxy "self" (first arg from colon-call syntax)
                        // and forward only the real arguments after it.
                        std::vector<sol::object> args;
                        args.reserve(va.size());
                        bool skippedSelf = false;
                        for (auto const& arg : va)
                        {
                            if (!skippedSelf && arg.is<LuaComponentProxy*>())
                            {
                                skippedSelf = true;
                                continue;
                            }
                            args.emplace_back(arg.get<sol::object>());
                        }

                        // Call original with freshly-resolved component + remaining args
                        switch (args.size())
                        {
                            case 0:
                                return origFn(freshComp);
                            case 1:
                                return origFn(freshComp, args[0]);
                            case 2:
                                return origFn(freshComp, args[0], args[1]);
                            case 3:
                                return origFn(freshComp, args[0], args[1], args[2]);
                            case 4:
                                return origFn(freshComp, args[0], args[1], args[2], args[3]);
                            default:
                                // Fallback for 5+ args — build a Lua call via stack
                                return origFn(freshComp, sol::as_args(args));
                        }
                    };

                    return sol::make_object(s, std::move(wrapper));
                }

                // 4. Deep-copy aggregate types to prevent bypass of __newindex.
                //    sol2 returns references for member-pointer bindings (e.g.
                //    &TransformComponent::Translation).  If we hand that reference
                //    to Lua, mutations like `proxy.translation.x = 5` go straight
                //    through to the component, skipping validation / Box2D sync.
                //    Returning a copy forces writers to go through set_translation
                //    or proxy.__newindex = vec3(...).
                if (result.is<glm::vec4>())
                    return sol::make_object(s, result.as<glm::vec4>());
                if (result.is<glm::vec3>())
                    return sol::make_object(s, result.as<glm::vec3>());
                if (result.is<glm::vec2>())
                    return sol::make_object(s, result.as<glm::vec2>());

                // Return nested usertypes by reference so mutations
                // (e.g. proxy.camera.perspectiveFOV = 1.2) modify the real
                // component.  These types have their own validated setters.
                if (result.is<SceneCamera>() || result.is<ColliderMaterial>() || result.is<ParticleSystem>())
                    return result;

                return result;
            },
            sol::meta_function::new_index,
            [](sol::this_state s, LuaComponentProxy& proxy, const std::string& key, sol::object value)
            {
                // Resolve the real component fresh
                sol::object comp = proxy.ResolveComponent(s);
                if (!comp.valid() || comp.is<sol::nil_t>())
                {
                    OLO_CORE_WARN("[Lua] ComponentProxy: cannot set '{}' — component no longer exists on entity {}", key, proxy.EntityID);
                    return;
                }

                // ── Box2D safety: reject collider writes while body is live ──
                // Shape IDs are not stored, so we cannot rebuild fixtures at
                // runtime.  Warn and silently discard the write.
                if (proxy.TypeName == "BoxCollider2DComponent" || proxy.TypeName == "CircleCollider2DComponent")
                {
                    Scene* scene = ScriptEngine::GetSceneContext();
                    if (scene)
                    {
                        if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(proxy.EntityID)))
                        {
                            Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                            if (entity.HasComponent<Rigidbody2DComponent>())
                            {
                                auto const& rb = entity.GetComponent<Rigidbody2DComponent>();
                                if (b2Body_IsValid(rb.RuntimeBody))
                                {
                                    OLO_CORE_WARN("[Lua] Cannot mutate {} property '{}' while physics body is active (entity {})", proxy.TypeName, key, proxy.EntityID);
                                    return;
                                }
                            }
                        }
                    }
                }

                // ── Finite-value validation for TransformComponent writes ──
                if (proxy.TypeName == "TransformComponent")
                {
                    if ((key == "translation" || key == "scale" || key == "rotation") && value.is<glm::vec3>())
                    {
                        if (!IsFiniteVec3(value.as<glm::vec3>()))
                        {
                            OLO_CORE_WARN("[Lua] Rejected non-finite vec3 for TransformComponent.{} on entity {}", key, proxy.EntityID);
                            return;
                        }
                    }

                    // Box2D does not support runtime scale changes — reject while body is live
                    if (key == "scale")
                    {
                        Scene* scene = ScriptEngine::GetSceneContext();
                        if (scene)
                        {
                            if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(proxy.EntityID)))
                            {
                                Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                                if (entity.HasComponent<Rigidbody2DComponent>())
                                {
                                    auto const& rb = entity.GetComponent<Rigidbody2DComponent>();
                                    if (b2Body_IsValid(rb.RuntimeBody))
                                    {
                                        OLO_CORE_WARN("[Lua] Cannot change TransformComponent.scale while physics body is active (entity {})", proxy.EntityID);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }

                // Delegate the write through sol2's property dispatch
                sol::table compAsTable = comp.as<sol::table>();
                compAsTable[key] = value;

                // ── Box2D sync: push TransformComponent changes to body ──
                if (proxy.TypeName == "TransformComponent" && (key == "translation" || key == "rotation"))
                {
                    Scene* scene = ScriptEngine::GetSceneContext();
                    if (scene)
                    {
                        if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(proxy.EntityID)))
                        {
                            Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                            if (entity.HasComponent<Rigidbody2DComponent>())
                            {
                                auto const& rb = entity.GetComponent<Rigidbody2DComponent>();
                                if (b2Body_IsValid(rb.RuntimeBody))
                                {
                                    auto const& tc = entity.GetComponent<TransformComponent>();
                                    b2Body_SetTransform(rb.RuntimeBody,
                                                        { tc.Translation.x, tc.Translation.y },
                                                        b2MakeRot(tc.GetRotationEuler().z));
                                }
                            }
                        }
                    }
                }
            });

        // --- GLM vector types (needed before components that use them) ---
        // sol::constructors alone binds only `vec3.new(x, y, z)`; the bare
        // `vec3(x, y, z)` every Lua author reaches for first raised "attempt to
        // call a table value". sol::call_constructor installs __call on the
        // type table so both spellings work.
        using Vec2Ctors = sol::constructors<glm::vec2(), glm::vec2(float), glm::vec2(float, float)>;
        lua.new_usertype<glm::vec2>("vec2",
                                    Vec2Ctors(),
                                    sol::call_constructor, Vec2Ctors(),
                                    "x", &glm::vec2::x,
                                    "y", &glm::vec2::y);

        using Vec3Ctors = sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>;
        lua.new_usertype<glm::vec3>("vec3",
                                    Vec3Ctors(),
                                    sol::call_constructor, Vec3Ctors(),
                                    "x", &glm::vec3::x,
                                    "y", &glm::vec3::y,
                                    "z", &glm::vec3::z);

        using Vec4Ctors = sol::constructors<glm::vec4(), glm::vec4(float), glm::vec4(float, float, float, float)>;
        lua.new_usertype<glm::vec4>("vec4",
                                    Vec4Ctors(),
                                    sol::call_constructor, Vec4Ctors(),
                                    "x", &glm::vec4::x,
                                    "y", &glm::vec4::y,
                                    "z", &glm::vec4::z,
                                    "w", &glm::vec4::w);

        // --- TransformComponent ---
        lua.new_usertype<TransformComponent>("TransformComponent",
                                             "translation", &TransformComponent::Translation,
                                             "scale", &TransformComponent::Scale,
                                             "rotation", sol::property(&TransformComponent::GetRotationEuler, &TransformComponent::SetRotationEuler));

        // --- Rigidbody2DComponent ---
        // All properties use sol::property so setters sync through Box2D when a runtime body exists.
        lua.new_usertype<Rigidbody2DComponent>("Rigidbody2DComponent", "type", sol::property([](Rigidbody2DComponent& rb) -> int
                                                                                             { return std::to_underlying(rb.Type); }, [](Rigidbody2DComponent& rb, int v)
                                                                                             {
                    if (v < 0 || v > std::to_underlying(Rigidbody2DComponent::BodyType::Kinematic)) { v = 0; }
                    auto const t = static_cast<Rigidbody2DComponent::BodyType>(v);
                    rb.Type = t;
                    if (b2Body_IsValid(rb.RuntimeBody))
                    {
                        b2BodyType b2t = b2_staticBody;
                        switch (t)
                        {
                            using enum Rigidbody2DComponent::BodyType;
                            case Dynamic:   b2t = b2_dynamicBody;   break;
                            case Kinematic: b2t = b2_kinematicBody; break;
                            default: break;
                        }
                        b2Body_SetType(rb.RuntimeBody, b2t);
                    } }),
                                               "fixedRotation", sol::property([](Rigidbody2DComponent& rb)
                                                                              { return rb.FixedRotation; }, [](Rigidbody2DComponent& rb, bool v)
                                                                              {
                    rb.FixedRotation = v;
                    if (b2Body_IsValid(rb.RuntimeBody))
                        b2Body_SetFixedRotation(rb.RuntimeBody, v); }),
                                               "linearVelocity", sol::property([](Rigidbody2DComponent& rb) -> glm::vec2
                                                                               { return rb.LinearVelocity; }, [](Rigidbody2DComponent& rb, const glm::vec2& v)
                                                                               {
                    if (!IsFiniteVec2(v)) return;
                    rb.LinearVelocity = v;
                    if (b2Body_IsValid(rb.RuntimeBody))
                        b2Body_SetLinearVelocity(rb.RuntimeBody, { v.x, v.y }); }),
                                               "angularVelocity", sol::property([](Rigidbody2DComponent& rb) -> f32
                                                                                { return rb.AngularVelocity; }, [](Rigidbody2DComponent& rb, f32 v)
                                                                                {
                    if (!std::isfinite(v)) return;
                    rb.AngularVelocity = v;
                    if (b2Body_IsValid(rb.RuntimeBody))
                        b2Body_SetAngularVelocity(rb.RuntimeBody, v); }),
                                               "applyLinearImpulse", [](Rigidbody2DComponent& rb, const glm::vec2& impulse, const glm::vec2& point, sol::optional<bool> wake)
                                               {
                if (!IsFiniteVec2(impulse) || !IsFiniteVec2(point)) return;
                if (b2Body_IsValid(rb.RuntimeBody))
                    b2Body_ApplyLinearImpulse(rb.RuntimeBody, b2Vec2(impulse.x, impulse.y), b2Vec2(point.x, point.y), wake.value_or(true)); }, "applyLinearImpulseToCenter", [](Rigidbody2DComponent& rb, const glm::vec2& impulse, sol::optional<bool> wake)
                                               {
                if (!IsFiniteVec2(impulse)) return;
                if (b2Body_IsValid(rb.RuntimeBody))
                    b2Body_ApplyLinearImpulseToCenter(rb.RuntimeBody, b2Vec2(impulse.x, impulse.y), wake.value_or(true)); });

        // --- BoxCollider2DComponent ---
        lua.new_usertype<BoxCollider2DComponent>("BoxCollider2DComponent",
                                                 "offset", sol::property([](const BoxCollider2DComponent& c)
                                                                         { return c.Offset; }, [](BoxCollider2DComponent& c, const glm::vec2& v)
                                                                         { if (IsFiniteVec2(v)) c.Offset = v; }),
                                                 "size", sol::property([](const BoxCollider2DComponent& c)
                                                                       { return c.Size; }, [](BoxCollider2DComponent& c, const glm::vec2& v)
                                                                       { if (IsFiniteVec2(v) && v.x > 0.0f && v.y > 0.0f) c.Size = v; }),
                                                 "density", sol::property([](const BoxCollider2DComponent& c)
                                                                          { return c.Density; }, [](BoxCollider2DComponent& c, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f) c.Density = v; }),
                                                 "friction", sol::property([](const BoxCollider2DComponent& c)
                                                                           { return c.Friction; }, [](BoxCollider2DComponent& c, f32 v)
                                                                           { if (std::isfinite(v)) c.Friction = std::clamp(v, 0.0f, 1.0f); }),
                                                 "restitution", sol::property([](const BoxCollider2DComponent& c)
                                                                              { return c.Restitution; }, [](BoxCollider2DComponent& c, f32 v)
                                                                              { if (std::isfinite(v)) c.Restitution = std::clamp(v, 0.0f, 1.0f); }),
                                                 "restitutionThreshold", sol::property([](const BoxCollider2DComponent& c)
                                                                                       { return c.RestitutionThreshold; }, [](BoxCollider2DComponent& c, f32 v)
                                                                                       { if (std::isfinite(v) && v >= 0.0f) c.RestitutionThreshold = v; }));

        // --- CircleCollider2DComponent ---
        lua.new_usertype<CircleCollider2DComponent>("CircleCollider2DComponent",
                                                    "offset", sol::property([](const CircleCollider2DComponent& c)
                                                                            { return c.Offset; }, [](CircleCollider2DComponent& c, const glm::vec2& v)
                                                                            { if (IsFiniteVec2(v)) c.Offset = v; }),
                                                    "radius", sol::property([](const CircleCollider2DComponent& c)
                                                                            { return c.Radius; }, [](CircleCollider2DComponent& c, f32 v)
                                                                            { if (std::isfinite(v) && v > 0.0f) c.Radius = v; }),
                                                    "density", sol::property([](const CircleCollider2DComponent& c)
                                                                             { return c.Density; }, [](CircleCollider2DComponent& c, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f) c.Density = v; }),
                                                    "friction", sol::property([](const CircleCollider2DComponent& c)
                                                                              { return c.Friction; }, [](CircleCollider2DComponent& c, f32 v)
                                                                              { if (std::isfinite(v)) c.Friction = std::clamp(v, 0.0f, 1.0f); }),
                                                    "restitution", sol::property([](const CircleCollider2DComponent& c)
                                                                                 { return c.Restitution; }, [](CircleCollider2DComponent& c, f32 v)
                                                                                 { if (std::isfinite(v)) c.Restitution = std::clamp(v, 0.0f, 1.0f); }),
                                                    "restitutionThreshold", sol::property([](const CircleCollider2DComponent& c)
                                                                                          { return c.RestitutionThreshold; }, [](CircleCollider2DComponent& c, f32 v)
                                                                                          { if (std::isfinite(v) && v >= 0.0f) c.RestitutionThreshold = v; }));

        // --- ColliderMaterial (shared by all 3D collider components) ---
        lua.new_usertype<ColliderMaterial>("ColliderMaterial", sol::no_constructor,
                                           "staticFriction", sol::property(&ColliderMaterial::GetStaticFriction, &ColliderMaterial::SetStaticFriction),
                                           "dynamicFriction", sol::property(&ColliderMaterial::GetDynamicFriction, &ColliderMaterial::SetDynamicFriction),
                                           "restitution", sol::property(&ColliderMaterial::GetRestitution, &ColliderMaterial::SetRestitution),
                                           "density", sol::property(&ColliderMaterial::GetDensity, &ColliderMaterial::SetDensity));

        // --- Rigidbody3DComponent ---
        lua.new_usertype<Rigidbody3DComponent>("Rigidbody3DComponent",
                                               "type", sol::property([](const Rigidbody3DComponent& rb) -> int
                                                                     { return std::to_underlying(rb.m_Type); }, [](Rigidbody3DComponent& rb, int v)
                                                                     { if (v >= 0 && v <= 2) rb.m_Type = static_cast<BodyType3D>(v); }),
                                               "layerID", sol::property([](const Rigidbody3DComponent& rb)
                                                                        { return rb.m_LayerID; }, [](Rigidbody3DComponent& rb, int v)
                                                                        { if (v >= 0) rb.m_LayerID = static_cast<u32>(v); }),
                                               "mass", sol::property([](const Rigidbody3DComponent& rb)
                                                                     { return rb.m_Mass; }, [](Rigidbody3DComponent& rb, f32 v)
                                                                     { if (std::isfinite(v) && v >= 0.0f) rb.m_Mass = v; }),
                                               "linearDrag", sol::property([](const Rigidbody3DComponent& rb)
                                                                           { return rb.m_LinearDrag; }, [](Rigidbody3DComponent& rb, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) rb.m_LinearDrag = v; }),
                                               "angularDrag", sol::property([](const Rigidbody3DComponent& rb)
                                                                            { return rb.m_AngularDrag; }, [](Rigidbody3DComponent& rb, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f) rb.m_AngularDrag = v; }),
                                               "disableGravity", &Rigidbody3DComponent::m_DisableGravity,
                                               "isTrigger", &Rigidbody3DComponent::m_IsTrigger,
                                               "lockedAxes", sol::property([](const Rigidbody3DComponent& rb) -> int
                                                                           { return static_cast<int>(std::to_underlying(rb.m_LockedAxes)); }, [](Rigidbody3DComponent& rb, int v)
                                                                           { if (v >= 0 && (static_cast<u32>(v) & ~AxisMask) == 0) rb.m_LockedAxes = static_cast<EActorAxis>(v); }),
                                               "initialLinearVelocity", sol::property([](const Rigidbody3DComponent& rb)
                                                                                      { return rb.m_InitialLinearVelocity; }, [](Rigidbody3DComponent& rb, const glm::vec3& v)
                                                                                      { if (IsFiniteVec3(v)) rb.m_InitialLinearVelocity = v; }),
                                               "initialAngularVelocity", sol::property([](const Rigidbody3DComponent& rb)
                                                                                       { return rb.m_InitialAngularVelocity; }, [](Rigidbody3DComponent& rb, const glm::vec3& v)
                                                                                       { if (IsFiniteVec3(v)) rb.m_InitialAngularVelocity = v; }),
                                               "maxLinearVelocity", sol::property([](const Rigidbody3DComponent& rb)
                                                                                  { return rb.m_MaxLinearVelocity; }, [](Rigidbody3DComponent& rb, f32 v)
                                                                                  { if (std::isfinite(v) && v >= 0.0f) rb.m_MaxLinearVelocity = v; }),
                                               "maxAngularVelocity", sol::property([](const Rigidbody3DComponent& rb)
                                                                                   { return rb.m_MaxAngularVelocity; }, [](Rigidbody3DComponent& rb, f32 v)
                                                                                   { if (std::isfinite(v) && v >= 0.0f) rb.m_MaxAngularVelocity = v; }));

        // --- DestructibleComponent (issue #459) ---
        // A script deals structural damage by depleting `health`; the
        // DestructibleSystem shatters the object on the next tick when it hits 0.
        // `broken` is read-only (the system owns it). Setters validate/clamp to
        // match the component's serializer ranges.
        lua.new_usertype<DestructibleComponent>("DestructibleComponent",
                                                "health", sol::property([](const DestructibleComponent& c)
                                                                        { return c.m_Health; }, [](DestructibleComponent& c, f32 v)
                                                                        { if (std::isfinite(v)) c.m_Health = v < 0.0f ? 0.0f : v; }),
                                                "maxHealth", sol::property([](const DestructibleComponent& c)
                                                                           { return c.m_MaxHealth; }, [](DestructibleComponent& c, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) c.m_MaxHealth = v; }),
                                                "damageThreshold", sol::property([](const DestructibleComponent& c)
                                                                                 { return c.m_DamageThreshold; }, [](DestructibleComponent& c, f32 v)
                                                                                 { if (std::isfinite(v) && v >= 0.0f) c.m_DamageThreshold = v; }),
                                                "chunkCount", sol::property([](const DestructibleComponent& c)
                                                                            { return c.m_ChunkCount; }, [](DestructibleComponent& c, int v)
                                                                            { if (v >= 0) c.m_ChunkCount = static_cast<u32>(v > 64 ? 64 : v); }),
                                                "chunkScale", sol::property([](const DestructibleComponent& c)
                                                                            { return c.m_ChunkScale; }, [](DestructibleComponent& c, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.01f && v <= 10.0f) c.m_ChunkScale = v; }),
                                                "explosionImpulse", sol::property([](const DestructibleComponent& c)
                                                                                  { return c.m_ExplosionImpulse; }, [](DestructibleComponent& c, f32 v)
                                                                                  { if (std::isfinite(v) && v >= 0.0f) c.m_ExplosionImpulse = v; }),
                                                "debrisLifetime", sol::property([](const DestructibleComponent& c)
                                                                                { return c.m_DebrisLifetime; }, [](DestructibleComponent& c, f32 v)
                                                                                { if (std::isfinite(v) && v >= 0.0f) c.m_DebrisLifetime = v; }),
                                                "breakOnJointBreak", &DestructibleComponent::m_BreakOnJointBreak,
                                                "destroyOnBreak", &DestructibleComponent::m_DestroyOnBreak,
                                                "broken", sol::readonly(&DestructibleComponent::m_Broken));

        // --- BoxCollider3DComponent ---
        lua.new_usertype<BoxCollider3DComponent>("BoxCollider3DComponent",
                                                 "halfExtents", sol::property([](const BoxCollider3DComponent& c)
                                                                              { return c.m_HalfExtents; }, [](BoxCollider3DComponent& c, const glm::vec3& v)
                                                                              { if (IsFiniteVec3(v) && v.x >= 0.0f && v.y >= 0.0f && v.z >= 0.0f) c.m_HalfExtents = v; }),
                                                 "offset", sol::property([](const BoxCollider3DComponent& c)
                                                                         { return c.m_Offset; }, [](BoxCollider3DComponent& c, const glm::vec3& v)
                                                                         { if (IsFiniteVec3(v)) c.m_Offset = v; }),
                                                 "material", &BoxCollider3DComponent::m_Material);

        // --- SphereCollider3DComponent ---
        lua.new_usertype<SphereCollider3DComponent>("SphereCollider3DComponent",
                                                    "radius", sol::property([](const SphereCollider3DComponent& c)
                                                                            { return c.m_Radius; }, [](SphereCollider3DComponent& c, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f) c.m_Radius = v; }),
                                                    "offset", sol::property([](const SphereCollider3DComponent& c)
                                                                            { return c.m_Offset; }, [](SphereCollider3DComponent& c, const glm::vec3& v)
                                                                            { if (IsFiniteVec3(v)) c.m_Offset = v; }),
                                                    "material", &SphereCollider3DComponent::m_Material);

        // --- CapsuleCollider3DComponent ---
        lua.new_usertype<CapsuleCollider3DComponent>("CapsuleCollider3DComponent",
                                                     "radius", sol::property([](const CapsuleCollider3DComponent& c)
                                                                             { return c.m_Radius; }, [](CapsuleCollider3DComponent& c, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f) c.m_Radius = v; }),
                                                     "halfHeight", sol::property([](const CapsuleCollider3DComponent& c)
                                                                                 { return c.m_HalfHeight; }, [](CapsuleCollider3DComponent& c, f32 v)
                                                                                 { if (std::isfinite(v) && v >= 0.0f) c.m_HalfHeight = v; }),
                                                     "offset", sol::property([](const CapsuleCollider3DComponent& c)
                                                                             { return c.m_Offset; }, [](CapsuleCollider3DComponent& c, const glm::vec3& v)
                                                                             { if (IsFiniteVec3(v)) c.m_Offset = v; }),
                                                     "material", &CapsuleCollider3DComponent::m_Material);

        // --- MeshCollider3DComponent ---
        lua.new_usertype<MeshCollider3DComponent>("MeshCollider3DComponent",
                                                  "colliderAsset", &MeshCollider3DComponent::m_ColliderAsset,
                                                  "offset", sol::property([](const MeshCollider3DComponent& c)
                                                                          { return c.m_Offset; }, [](MeshCollider3DComponent& c, const glm::vec3& v)
                                                                          { if (IsFiniteVec3(v)) c.m_Offset = v; }),
                                                  "scale", sol::property([](const MeshCollider3DComponent& c)
                                                                         { return c.m_Scale; }, [](MeshCollider3DComponent& c, const glm::vec3& v)
                                                                         { if (IsFiniteVec3(v) && v.x > 0.0f && v.y > 0.0f && v.z > 0.0f) c.m_Scale = v; }),
                                                  "material", &MeshCollider3DComponent::m_Material,
                                                  "useComplexAsSimple", &MeshCollider3DComponent::m_UseComplexAsSimple);

        // --- ConvexMeshCollider3DComponent ---
        lua.new_usertype<ConvexMeshCollider3DComponent>("ConvexMeshCollider3DComponent",
                                                        "colliderAsset", &ConvexMeshCollider3DComponent::m_ColliderAsset,
                                                        "offset", sol::property([](const ConvexMeshCollider3DComponent& c)
                                                                                { return c.m_Offset; }, [](ConvexMeshCollider3DComponent& c, const glm::vec3& v)
                                                                                { if (IsFiniteVec3(v)) c.m_Offset = v; }),
                                                        "scale", sol::property([](const ConvexMeshCollider3DComponent& c)
                                                                               { return c.m_Scale; }, [](ConvexMeshCollider3DComponent& c, const glm::vec3& v)
                                                                               { if (IsFiniteVec3(v) && v.x > 0.0f && v.y > 0.0f && v.z > 0.0f) c.m_Scale = v; }),
                                                        "material", &ConvexMeshCollider3DComponent::m_Material,
                                                        "convexRadius", sol::property([](const ConvexMeshCollider3DComponent& c)
                                                                                      { return c.m_ConvexRadius; }, [](ConvexMeshCollider3DComponent& c, f32 v)
                                                                                      { if (std::isfinite(v) && v >= 0.0f) c.m_ConvexRadius = v; }),
                                                        "maxVertices", &ConvexMeshCollider3DComponent::m_MaxVertices);

        // --- TriangleMeshCollider3DComponent ---
        lua.new_usertype<TriangleMeshCollider3DComponent>("TriangleMeshCollider3DComponent",
                                                          "colliderAsset", &TriangleMeshCollider3DComponent::m_ColliderAsset,
                                                          "offset", sol::property([](const TriangleMeshCollider3DComponent& c)
                                                                                  { return c.m_Offset; }, [](TriangleMeshCollider3DComponent& c, const glm::vec3& v)
                                                                                  { if (IsFiniteVec3(v)) c.m_Offset = v; }),
                                                          "scale", sol::property([](const TriangleMeshCollider3DComponent& c)
                                                                                 { return c.m_Scale; }, [](TriangleMeshCollider3DComponent& c, const glm::vec3& v)
                                                                                 { if (IsFiniteVec3(v) && v.x > 0.0f && v.y > 0.0f && v.z > 0.0f) c.m_Scale = v; }),
                                                          "material", &TriangleMeshCollider3DComponent::m_Material);

    }
} // namespace OloEngine
