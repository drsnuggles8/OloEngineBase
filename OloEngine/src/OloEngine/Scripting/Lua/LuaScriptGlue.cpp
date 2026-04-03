#include "LuaScriptGlue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Animation/MorphTargets/FacialExpressionLibrary.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageCalculation.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageEvent.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Audio/AudioEvents/AudioPlayback.h"
#include "OloEngine/Audio/AudioEvents/CommandID.h"
#include "OloEngine/Scene/Streaming/SceneStreamer.h"

#include "box2d/box2d.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_map>

namespace OloEngine
{
    namespace Scripting
    {
        extern sol::state* GetState();
    }

    // ── Component registry for Lua entity_utils.get_component / has_component ──
    //
    // Each entry maps a component name string to a pair of type-erased lambdas
    // that call Entity::HasComponent<T> and Entity::GetComponent<T>.
    // Adding a new component requires a single REGISTER_COMPONENT line.

    struct ComponentEntry
    {
        using HasFn = bool (*)(Entity&);
        using GetFn = sol::object (*)(Entity&, sol::this_state);

        HasFn Has = nullptr;
        GetFn Get = nullptr;
    };

    // ── Safe proxy that re-resolves the component from EnTT on every access ──
    //
    // Lua never holds a raw T* into EnTT pool storage.  Instead get_component
    // returns a LuaComponentProxy userdata.  __index / __newindex metamethods
    // resolve Entity → Component& on every property read or write, so pool
    // relocations (caused by AddComponent on other entities) cannot cause
    // dangling-pointer UB.

    struct LuaComponentProxy
    {
        u64 EntityID = 0;
        ComponentEntry::GetFn Resolve = nullptr; // resolves Entity& → sol::object wrapping T*
        std::string_view TypeName;               // component type name for Box2D sync hooks

        // Resolve the component to a sol::object wrapping T* (or nil).
        [[nodiscard]] sol::object ResolveComponent(sol::this_state s) const
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return sol::make_object(s, sol::nil);
            auto entityOpt = scene->TryGetEntityWithUUID(UUID(EntityID));
            if (!entityOpt)
                return sol::make_object(s, sol::nil);
            Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
            return Resolve(entity, s);
        }
    };

    template<typename T>
    static constexpr ComponentEntry MakeEntry()
    {
        return {
            [](Entity& e) -> bool
            { return e.HasComponent<T>(); },
            // Returns a raw T* wrapped in sol::object — only used internally by
            // the proxy's __index/__newindex to perform a single property
            // read/write.  Lua scripts never see this pointer directly.
            [](Entity& e, sol::this_state s) -> sol::object
            {
                if (e.HasComponent<T>())
                    return sol::make_object(s, &e.GetComponent<T>());
                return sol::make_object(s, sol::nil);
            }
        };
    }

    // clang-format off
    static const std::unordered_map<std::string_view, ComponentEntry>& GetComponentRegistry()
    {
        static const std::unordered_map<std::string_view, ComponentEntry> s_Registry = {
            #define REGISTER_COMPONENT(T) { #T, MakeEntry<T>() }
            // Core
            REGISTER_COMPONENT(TransformComponent),
            REGISTER_COMPONENT(Rigidbody2DComponent),
            REGISTER_COMPONENT(CameraComponent),
            REGISTER_COMPONENT(SpriteRendererComponent),
            REGISTER_COMPONENT(CircleRendererComponent),
            REGISTER_COMPONENT(TextComponent),
            REGISTER_COMPONENT(MeshComponent),
            REGISTER_COMPONENT(MaterialComponent),
            REGISTER_COMPONENT(BoxCollider2DComponent),
            REGISTER_COMPONENT(CircleCollider2DComponent),
            REGISTER_COMPONENT(AudioSourceComponent),
            REGISTER_COMPONENT(AudioListenerComponent),
            REGISTER_COMPONENT(ParticleSystemComponent),
            REGISTER_COMPONENT(NavAgentComponent),
            REGISTER_COMPONENT(AbilityComponent),
            REGISTER_COMPONENT(DialogueComponent),
            REGISTER_COMPONENT(NetworkIdentityComponent),
            REGISTER_COMPONENT(IKTargetComponent),
            REGISTER_COMPONENT(NameplateComponent),
            REGISTER_COMPONENT(InventoryComponent),
            REGISTER_COMPONENT(ItemPickupComponent),
            REGISTER_COMPONENT(ItemContainerComponent),
            REGISTER_COMPONENT(QuestJournalComponent),
            REGISTER_COMPONENT(QuestGiverComponent),
            // UI
            REGISTER_COMPONENT(UICanvasComponent),
            REGISTER_COMPONENT(UIRectTransformComponent),
            REGISTER_COMPONENT(UIImageComponent),
            REGISTER_COMPONENT(UIPanelComponent),
            REGISTER_COMPONENT(UITextComponent),
            REGISTER_COMPONENT(UIButtonComponent),
            REGISTER_COMPONENT(UISliderComponent),
            REGISTER_COMPONENT(UICheckboxComponent),
            REGISTER_COMPONENT(UIProgressBarComponent),
            REGISTER_COMPONENT(UIInputFieldComponent),
            REGISTER_COMPONENT(UIScrollViewComponent),
            REGISTER_COMPONENT(UIDropdownComponent),
            REGISTER_COMPONENT(UIGridLayoutComponent),
            REGISTER_COMPONENT(UIToggleComponent),
            REGISTER_COMPONENT(UIWorldAnchorComponent),
            // Lighting
            REGISTER_COMPONENT(LightProbeComponent),
            REGISTER_COMPONENT(LightProbeVolumeComponent),
            // Streaming
            REGISTER_COMPONENT(StreamingVolumeComponent),
            // Animation
            REGISTER_COMPONENT(AnimationGraphComponent),
            REGISTER_COMPONENT(MorphTargetComponent),
            // AI / Behavior
            REGISTER_COMPONENT(NavMeshBoundsComponent),
            REGISTER_COMPONENT(BehaviorTreeComponent),
            REGISTER_COMPONENT(StateMachineComponent),
            #undef REGISTER_COMPONENT
        };
        return s_Registry;
    }
    // clang-format on

    void LuaScriptGlue::RegisterAllTypes()
    {
        sol::state& lua = *Scripting::GetState();

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
                lua_State* L = s;

                // 1. Resolve the real component (T*) as a sol::object
                sol::object comp = proxy.ResolveComponent(s);
                if (!comp.valid() || comp.is<sol::nil_t>())
                    return sol::make_object(s, sol::nil);

                // 2. Push the component on the stack and look up the key
                //    through its metatable (which carries the registered
                //    usertype properties / methods).
                comp.push(L);                                // stack: comp_ud
                lua_getmetatable(L, -1);                     // stack: comp_ud, mt
                lua_pushlstring(L, key.c_str(), key.size()); // stack: comp_ud, mt, key
                lua_rawget(L, -2);                           // stack: comp_ud, mt, value

                sol::object value(L, -1);

                // If the result is a property accessor table generated by
                // sol::property, invoke the getter with the component.
                // Otherwise if it's a plain function, wrap it so it re-resolves.
                if (value.get_type() == sol::type::function)
                {
                    // Check if this is a sol::property getter (sol emits a
                    // special function for __index access that expects the
                    // userdata as sole arg).  We call it directly with the
                    // resolved component to obtain the property value.
                    //
                    // However, methods also appear as functions here.  The
                    // heuristic: try calling with the component.  If it's a
                    // property getter, it returns the value.  If it's a method,
                    // we wrap it instead.
                    //
                    // Actually, sol2 stores property getters/setters in a
                    // different manner than methods in the metatable.  Methods
                    // are stored directly, property getters are wrapped.
                    // The safe universal approach: wrap every function in a
                    // closure that re-resolves the component before each call.

                    // Capture the original function + the proxy identity
                    sol::protected_function origFn(L, -1);
                    u64 entityID = proxy.EntityID;
                    auto resolveFn = proxy.Resolve;

                    auto wrapper = [entityID, resolveFn, origFn = std::move(origFn)](sol::variadic_args va) -> sol::protected_function_result
                    {
                        sol::this_state ws = va.lua_state();
                        lua_State* wL = ws;

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

                    lua_pop(L, 3); // pop value, mt, comp_ud
                    return sol::make_object(s, std::move(wrapper));
                }

                // For non-function values, the metatable lookup returned nil or
                // a static value.  Fall back to using sol2's own __index on the
                // resolved component, which handles sol::property getters.
                lua_pop(L, 3); // pop value, mt, comp_ud

                // Use sol2's high-level getter which properly dispatches through
                // property accessors.
                sol::table compAsTable = comp.as<sol::table>();
                return compAsTable[key];
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
        lua.new_usertype<glm::vec2>("vec2",
                                    sol::constructors<glm::vec2(), glm::vec2(float), glm::vec2(float, float)>(),
                                    "x", &glm::vec2::x,
                                    "y", &glm::vec2::y);

        lua.new_usertype<glm::vec3>("vec3",
                                    sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(),
                                    "x", &glm::vec3::x,
                                    "y", &glm::vec3::y,
                                    "z", &glm::vec3::z);

        lua.new_usertype<glm::vec4>("vec4",
                                    sol::constructors<glm::vec4(), glm::vec4(float), glm::vec4(float, float, float, float)>(),
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
        lua.new_usertype<Rigidbody2DComponent>("Rigidbody2DComponent", "type", sol::property([](Rigidbody2DComponent& rb)
                                                                                             { return rb.Type; }, [](Rigidbody2DComponent& rb, Rigidbody2DComponent::BodyType t)
                                                                                             {
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
                    rb.LinearVelocity = v;
                    if (b2Body_IsValid(rb.RuntimeBody))
                        b2Body_SetLinearVelocity(rb.RuntimeBody, { v.x, v.y }); }),
                                               "angularVelocity", sol::property([](Rigidbody2DComponent& rb) -> f32
                                                                                { return rb.AngularVelocity; }, [](Rigidbody2DComponent& rb, f32 v)
                                                                                {
                    rb.AngularVelocity = v;
                    if (b2Body_IsValid(rb.RuntimeBody))
                        b2Body_SetAngularVelocity(rb.RuntimeBody, v); }),
                                               "applyLinearImpulse", [](Rigidbody2DComponent& rb, const glm::vec2& impulse, const glm::vec2& point, sol::optional<bool> wake)
                                               {
                if (b2Body_IsValid(rb.RuntimeBody))
                    b2Body_ApplyLinearImpulse(rb.RuntimeBody, b2Vec2(impulse.x, impulse.y), b2Vec2(point.x, point.y), wake.value_or(true)); }, "applyLinearImpulseToCenter", [](Rigidbody2DComponent& rb, const glm::vec2& impulse, sol::optional<bool> wake)
                                               {
                if (b2Body_IsValid(rb.RuntimeBody))
                    b2Body_ApplyLinearImpulseToCenter(rb.RuntimeBody, b2Vec2(impulse.x, impulse.y), wake.value_or(true)); });

        // --- BoxCollider2DComponent ---
        lua.new_usertype<BoxCollider2DComponent>("BoxCollider2DComponent",
                                                 "offset", &BoxCollider2DComponent::Offset,
                                                 "size", &BoxCollider2DComponent::Size,
                                                 "density", &BoxCollider2DComponent::Density,
                                                 "friction", &BoxCollider2DComponent::Friction,
                                                 "restitution", &BoxCollider2DComponent::Restitution,
                                                 "restitutionThreshold", &BoxCollider2DComponent::RestitutionThreshold);

        // --- CircleCollider2DComponent ---
        lua.new_usertype<CircleCollider2DComponent>("CircleCollider2DComponent",
                                                    "offset", &CircleCollider2DComponent::Offset,
                                                    "radius", &CircleCollider2DComponent::Radius,
                                                    "density", &CircleCollider2DComponent::Density,
                                                    "friction", &CircleCollider2DComponent::Friction,
                                                    "restitution", &CircleCollider2DComponent::Restitution,
                                                    "restitutionThreshold", &CircleCollider2DComponent::RestitutionThreshold);

        // --- SceneCamera (needed by CameraComponent) ---
        lua.new_usertype<SceneCamera>("SceneCamera",
                                      "projectionType", sol::property(&SceneCamera::GetProjectionType, &SceneCamera::SetProjectionType),
                                      "perspectiveFOV", sol::property(&SceneCamera::GetPerspectiveVerticalFOV, &SceneCamera::SetPerspectiveVerticalFOV),
                                      "perspectiveNearClip", sol::property(&SceneCamera::GetPerspectiveNearClip, &SceneCamera::SetPerspectiveNearClip),
                                      "perspectiveFarClip", sol::property(&SceneCamera::GetPerspectiveFarClip, &SceneCamera::SetPerspectiveFarClip),
                                      "orthographicSize", sol::property(&SceneCamera::GetOrthographicSize, &SceneCamera::SetOrthographicSize),
                                      "orthographicNearClip", sol::property(&SceneCamera::GetOrthographicNearClip, &SceneCamera::SetOrthographicNearClip),
                                      "orthographicFarClip", sol::property(&SceneCamera::GetOrthographicFarClip, &SceneCamera::SetOrthographicFarClip));

        // --- CameraComponent ---
        lua.new_usertype<CameraComponent>("CameraComponent",
                                          "camera", &CameraComponent::Camera,
                                          "primary", &CameraComponent::Primary,
                                          "fixedAspectRatio", &CameraComponent::FixedAspectRatio);

        // --- SpriteRendererComponent ---
        lua.new_usertype<SpriteRendererComponent>("SpriteRendererComponent",
                                                  "color", &SpriteRendererComponent::Color,
                                                  "tilingFactor", &SpriteRendererComponent::TilingFactor);

        // --- CircleRendererComponent ---
        lua.new_usertype<CircleRendererComponent>("CircleRendererComponent",
                                                  "color", &CircleRendererComponent::Color,
                                                  "thickness", &CircleRendererComponent::Thickness,
                                                  "fade", &CircleRendererComponent::Fade);

        // --- TextComponent ---
        lua.new_usertype<TextComponent>("TextComponent",
                                        "text", &TextComponent::TextString,
                                        "color", &TextComponent::Color,
                                        "kerning", &TextComponent::Kerning,
                                        "lineSpacing", &TextComponent::LineSpacing,
                                        "maxWidth", &TextComponent::MaxWidth,
                                        "dropShadow", &TextComponent::DropShadow,
                                        "shadowDistance", &TextComponent::ShadowDistance,
                                        "shadowColor", &TextComponent::ShadowColor);

        // --- MeshComponent ---
        lua.new_usertype<MeshComponent>("MeshComponent",
                                        "primitive", &MeshComponent::m_Primitive);

        // --- UICanvasComponent ---
        lua.new_usertype<UICanvasComponent>("UICanvasComponent",
                                            "renderMode", &UICanvasComponent::m_RenderMode,
                                            "scaleMode", &UICanvasComponent::m_ScaleMode,
                                            "sortOrder", &UICanvasComponent::m_SortOrder,
                                            "referenceResolution", &UICanvasComponent::m_ReferenceResolution);

        // --- UIRectTransformComponent ---
        lua.new_usertype<UIRectTransformComponent>("UIRectTransformComponent",
                                                   "anchorMin", &UIRectTransformComponent::m_AnchorMin,
                                                   "anchorMax", &UIRectTransformComponent::m_AnchorMax,
                                                   "anchoredPosition", &UIRectTransformComponent::m_AnchoredPosition,
                                                   "sizeDelta", &UIRectTransformComponent::m_SizeDelta,
                                                   "pivot", &UIRectTransformComponent::m_Pivot,
                                                   "rotation", &UIRectTransformComponent::m_Rotation,
                                                   "scale", &UIRectTransformComponent::m_Scale);

        // --- UIImageComponent ---
        lua.new_usertype<UIImageComponent>("UIImageComponent",
                                           "color", &UIImageComponent::m_Color,
                                           "borderInsets", &UIImageComponent::m_BorderInsets);

        // --- UIPanelComponent ---
        lua.new_usertype<UIPanelComponent>("UIPanelComponent",
                                           "backgroundColor", &UIPanelComponent::m_BackgroundColor);

        // --- UITextComponent ---
        lua.new_usertype<UITextComponent>("UITextComponent",
                                          "text", &UITextComponent::m_Text,
                                          "fontSize", &UITextComponent::m_FontSize,
                                          "color", &UITextComponent::m_Color,
                                          "alignment", &UITextComponent::m_Alignment,
                                          "kerning", &UITextComponent::m_Kerning,
                                          "lineSpacing", &UITextComponent::m_LineSpacing);

        // --- UIButtonComponent ---
        lua.new_usertype<UIButtonComponent>("UIButtonComponent",
                                            "normalColor", &UIButtonComponent::m_NormalColor,
                                            "hoveredColor", &UIButtonComponent::m_HoveredColor,
                                            "pressedColor", &UIButtonComponent::m_PressedColor,
                                            "disabledColor", &UIButtonComponent::m_DisabledColor,
                                            "interactable", &UIButtonComponent::m_Interactable,
                                            "state", sol::readonly(&UIButtonComponent::m_State));

        // --- UISliderComponent ---
        lua.new_usertype<UISliderComponent>("UISliderComponent",
                                            "value", &UISliderComponent::m_Value,
                                            "minValue", &UISliderComponent::m_MinValue,
                                            "maxValue", &UISliderComponent::m_MaxValue,
                                            "direction", &UISliderComponent::m_Direction,
                                            "backgroundColor", &UISliderComponent::m_BackgroundColor,
                                            "fillColor", &UISliderComponent::m_FillColor,
                                            "handleColor", &UISliderComponent::m_HandleColor,
                                            "interactable", &UISliderComponent::m_Interactable);

        // --- UICheckboxComponent ---
        lua.new_usertype<UICheckboxComponent>("UICheckboxComponent",
                                              "isChecked", &UICheckboxComponent::m_IsChecked,
                                              "uncheckedColor", &UICheckboxComponent::m_UncheckedColor,
                                              "checkedColor", &UICheckboxComponent::m_CheckedColor,
                                              "checkmarkColor", &UICheckboxComponent::m_CheckmarkColor,
                                              "interactable", &UICheckboxComponent::m_Interactable);

        // --- UIProgressBarComponent ---
        lua.new_usertype<UIProgressBarComponent>("UIProgressBarComponent",
                                                 "value", &UIProgressBarComponent::m_Value,
                                                 "minValue", &UIProgressBarComponent::m_MinValue,
                                                 "maxValue", &UIProgressBarComponent::m_MaxValue,
                                                 "fillMethod", &UIProgressBarComponent::m_FillMethod,
                                                 "backgroundColor", &UIProgressBarComponent::m_BackgroundColor,
                                                 "fillColor", &UIProgressBarComponent::m_FillColor);

        // --- UIInputFieldComponent ---
        lua.new_usertype<UIInputFieldComponent>("UIInputFieldComponent",
                                                "text", &UIInputFieldComponent::m_Text,
                                                "placeholder", &UIInputFieldComponent::m_Placeholder,
                                                "fontSize", &UIInputFieldComponent::m_FontSize,
                                                "textColor", &UIInputFieldComponent::m_TextColor,
                                                "placeholderColor", &UIInputFieldComponent::m_PlaceholderColor,
                                                "backgroundColor", &UIInputFieldComponent::m_BackgroundColor,
                                                "characterLimit", &UIInputFieldComponent::m_CharacterLimit,
                                                "interactable", &UIInputFieldComponent::m_Interactable);

        // --- UIScrollViewComponent ---
        lua.new_usertype<UIScrollViewComponent>("UIScrollViewComponent",
                                                "scrollPosition", &UIScrollViewComponent::m_ScrollPosition,
                                                "contentSize", &UIScrollViewComponent::m_ContentSize,
                                                "scrollDirection", &UIScrollViewComponent::m_ScrollDirection,
                                                "scrollSpeed", &UIScrollViewComponent::m_ScrollSpeed,
                                                "showHorizontalScrollbar", &UIScrollViewComponent::m_ShowHorizontalScrollbar,
                                                "showVerticalScrollbar", &UIScrollViewComponent::m_ShowVerticalScrollbar,
                                                "scrollbarColor", &UIScrollViewComponent::m_ScrollbarColor,
                                                "scrollbarTrackColor", &UIScrollViewComponent::m_ScrollbarTrackColor);

        // --- UIDropdownComponent ---
        lua.new_usertype<UIDropdownComponent>("UIDropdownComponent",
                                              "selectedIndex", &UIDropdownComponent::m_SelectedIndex,
                                              "backgroundColor", &UIDropdownComponent::m_BackgroundColor,
                                              "highlightColor", &UIDropdownComponent::m_HighlightColor,
                                              "textColor", &UIDropdownComponent::m_TextColor,
                                              "fontSize", &UIDropdownComponent::m_FontSize,
                                              "itemHeight", &UIDropdownComponent::m_ItemHeight,
                                              "interactable", &UIDropdownComponent::m_Interactable);

        // --- UIGridLayoutComponent ---
        lua.new_usertype<UIGridLayoutComponent>("UIGridLayoutComponent",
                                                "cellSize", &UIGridLayoutComponent::m_CellSize,
                                                "spacing", &UIGridLayoutComponent::m_Spacing,
                                                "padding", &UIGridLayoutComponent::m_Padding,
                                                "startCorner", &UIGridLayoutComponent::m_StartCorner,
                                                "startAxis", &UIGridLayoutComponent::m_StartAxis,
                                                "constraintCount", &UIGridLayoutComponent::m_ConstraintCount);

        // --- UIToggleComponent ---
        lua.new_usertype<UIToggleComponent>("UIToggleComponent",
                                            "isOn", &UIToggleComponent::m_IsOn,
                                            "offColor", &UIToggleComponent::m_OffColor,
                                            "onColor", &UIToggleComponent::m_OnColor,
                                            "knobColor", &UIToggleComponent::m_KnobColor,
                                            "interactable", &UIToggleComponent::m_Interactable);

        // --- ParticleSystemComponent ---
        lua.new_usertype<ParticleSystem>("ParticleSystem",
                                         "playing", &ParticleSystem::Playing,
                                         "looping", &ParticleSystem::Looping,
                                         "duration", &ParticleSystem::Duration,
                                         "playbackSpeed", &ParticleSystem::PlaybackSpeed,
                                         "windInfluence", &ParticleSystem::WindInfluence,
                                         "getAliveCount", &ParticleSystem::GetAliveCount,
                                         "reset", &ParticleSystem::Reset);

        lua.new_usertype<ParticleEmitter>("ParticleEmitter",
                                          "rateOverTime", &ParticleEmitter::RateOverTime,
                                          "initialSpeed", &ParticleEmitter::InitialSpeed,
                                          "speedVariance", &ParticleEmitter::SpeedVariance,
                                          "lifetimeMin", &ParticleEmitter::LifetimeMin,
                                          "lifetimeMax", &ParticleEmitter::LifetimeMax,
                                          "initialSize", &ParticleEmitter::InitialSize,
                                          "sizeVariance", &ParticleEmitter::SizeVariance,
                                          "initialColor", &ParticleEmitter::InitialColor);

        lua.new_usertype<ParticleSystemComponent>("ParticleSystemComponent",
                                                  "system", &ParticleSystemComponent::System);

        // --- LightProbeComponent ---
        lua.new_usertype<LightProbeComponent>("LightProbeComponent",
                                              "influenceRadius", &LightProbeComponent::m_InfluenceRadius,
                                              "intensity", &LightProbeComponent::m_Intensity,
                                              "active", &LightProbeComponent::m_Active);

        // --- LightProbeVolumeComponent ---
        lua.new_usertype<LightProbeVolumeComponent>("LightProbeVolumeComponent",
                                                    "boundsMin", &LightProbeVolumeComponent::m_BoundsMin,
                                                    "boundsMax", &LightProbeVolumeComponent::m_BoundsMax,
                                                    "spacing", &LightProbeVolumeComponent::m_Spacing,
                                                    "intensity", &LightProbeVolumeComponent::m_Intensity,
                                                    "active", &LightProbeVolumeComponent::m_Active,
                                                    "dirty", &LightProbeVolumeComponent::m_Dirty,
                                                    "getTotalProbeCount", &LightProbeVolumeComponent::GetTotalProbeCount);

        // --- UIWorldAnchorComponent ---
        lua.new_usertype<UIWorldAnchorComponent>("UIWorldAnchorComponent",
                                                 "targetEntity", sol::property([](const UIWorldAnchorComponent& c)
                                                                               { return static_cast<u64>(c.m_TargetEntity); }, [](UIWorldAnchorComponent& c, u64 id)
                                                                               { c.m_TargetEntity = UUID(id); }),
                                                 "worldOffset", &UIWorldAnchorComponent::m_WorldOffset);

        // --- NameplateComponent ---
        lua.new_usertype<NameplateComponent>("NameplateComponent",
                                             "enabled", &NameplateComponent::m_Enabled,
                                             "showHealthBar", &NameplateComponent::m_ShowHealthBar,
                                             "showManaBar", &NameplateComponent::m_ShowManaBar,
                                             "worldOffset", &NameplateComponent::m_WorldOffset,
                                             "barSize", &NameplateComponent::m_BarSize,
                                             "healthBarColor", &NameplateComponent::m_HealthBarColor,
                                             "manaBarColor", &NameplateComponent::m_ManaBarColor,
                                             "barBackgroundColor", &NameplateComponent::m_BarBackgroundColor,
                                             "manaBarGap", &NameplateComponent::m_ManaBarGap);

        // --- IKTargetComponent ---
        lua.new_usertype<IKTargetComponent>("IKTargetComponent",
                                            "aimIKEnabled", &IKTargetComponent::AimIKEnabled,
                                            "aimBoneIndex", &IKTargetComponent::AimBoneIndex,
                                            "aimTarget", &IKTargetComponent::AimTarget,
                                            "aimAxis", &IKTargetComponent::AimAxis,
                                            "aimOffset", &IKTargetComponent::AimOffset,
                                            "aimPoleVector", &IKTargetComponent::AimPoleVector,
                                            "aimChainLength", &IKTargetComponent::AimChainLength,
                                            "aimChainFactor", &IKTargetComponent::AimChainFactor,
                                            "aimWeight", &IKTargetComponent::AimWeight,
                                            "aimTargetEntity", sol::property([](const IKTargetComponent& c)
                                                                             { return static_cast<u64>(c.AimTargetEntity); }, [](IKTargetComponent& c, u64 id)
                                                                             { c.AimTargetEntity = UUID(id); }),
                                            "limbIKEnabled", &IKTargetComponent::LimbIKEnabled,
                                            "limbBoneIndex", &IKTargetComponent::LimbBoneIndex,
                                            "limbTarget", &IKTargetComponent::LimbTarget,
                                            "limbChainLength", &IKTargetComponent::LimbChainLength,
                                            "limbWeight", &IKTargetComponent::LimbWeight,
                                            "limbTargetEntity", sol::property([](const IKTargetComponent& c)
                                                                              { return static_cast<u64>(c.LimbTargetEntity); }, [](IKTargetComponent& c, u64 id)
                                                                              { c.LimbTargetEntity = UUID(id); }));

        // --- WindSettings (scene-level) ---
        lua.new_usertype<WindSettings>("WindSettings",
                                       "enabled", &WindSettings::Enabled,
                                       "direction", &WindSettings::Direction,
                                       "speed", &WindSettings::Speed,
                                       "gustStrength", &WindSettings::GustStrength,
                                       "gustFrequency", &WindSettings::GustFrequency,
                                       "turbulenceIntensity", &WindSettings::TurbulenceIntensity,
                                       "turbulenceScale", &WindSettings::TurbulenceScale,
                                       "gridWorldSize", &WindSettings::GridWorldSize,
                                       "gridResolution", &WindSettings::GridResolution);

        // --- StreamingVolumeComponent ---
        lua.new_usertype<StreamingVolumeComponent>("StreamingVolumeComponent",
                                                   "loadRadius", &StreamingVolumeComponent::LoadRadius,
                                                   "unloadRadius", &StreamingVolumeComponent::UnloadRadius,
                                                   "isLoaded", sol::readonly(&StreamingVolumeComponent::IsLoaded));

        // --- StreamingSettings (scene-level) ---
        lua.new_usertype<StreamingSettings>("StreamingSettings",
                                            "enabled", &StreamingSettings::Enabled,
                                            "defaultLoadRadius", &StreamingSettings::DefaultLoadRadius,
                                            "defaultUnloadRadius", &StreamingSettings::DefaultUnloadRadius,
                                            "maxLoadedRegions", &StreamingSettings::MaxLoadedRegions,
                                            "regionDirectory", &StreamingSettings::RegionDirectory);

        // --- NetworkIdentityComponent ---
        lua.new_usertype<NetworkIdentityComponent>("NetworkIdentityComponent",
                                                   "ownerClientID", &NetworkIdentityComponent::OwnerClientID,
                                                   "authority", &NetworkIdentityComponent::Authority,
                                                   "isReplicated", &NetworkIdentityComponent::IsReplicated);

        // --- AudioSourceComponent ---
        lua.new_usertype<AudioSourceComponent>("AudioSourceComponent", "volume", sol::property([](const AudioSourceComponent& c)
                                                                                               { return c.Config.VolumeMultiplier; }, [](AudioSourceComponent& c, f32 v)
                                                                                               {
                    if (!std::isfinite(v)) v = 1.0f;
                    v = std::clamp(v, 0.0f, 2.0f);
                    c.Config.VolumeMultiplier = v;
                    if (c.Source) { c.Source->SetVolume(v); } }),
                                               "pitch", sol::property([](const AudioSourceComponent& c)
                                                                      { return c.Config.PitchMultiplier; }, [](AudioSourceComponent& c, f32 v)
                                                                      {
                    if (!std::isfinite(v)) v = 1.0f;
                    v = std::clamp(v, 0.1f, 3.0f);
                    c.Config.PitchMultiplier = v;
                    if (c.Source) { c.Source->SetPitch(v); } }),
                                               "playOnAwake", sol::property([](const AudioSourceComponent& c)
                                                                            { return c.Config.PlayOnAwake; }, [](AudioSourceComponent& c, bool v)
                                                                            { c.Config.PlayOnAwake = v; }),
                                               "looping", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.Config.Looping; }, [](AudioSourceComponent& c, bool v)
                                                                        {
                    c.Config.Looping = v;
                    if (c.Source) { c.Source->SetLooping(v); } }),
                                               "spatialization", sol::property([](const AudioSourceComponent& c)
                                                                               { return c.Config.Spatialization; }, [](AudioSourceComponent& c, bool v)
                                                                               {
                    c.Config.Spatialization = v;
                    if (c.Source) { c.Source->SetSpatialization(v); } }),
                                               "useEventSystem", &AudioSourceComponent::UseEventSystem, "startEvent", sol::property([](const AudioSourceComponent& c)
                                                                                                                                    { return c.StartEvent; }, [](AudioSourceComponent& c, const std::string& v)
                                                                                                                                    {
                    c.StartEvent = v;
                    c.StartCommandID = Audio::CommandID::FromString(v); }),
                                               "isPlaying", [](const AudioSourceComponent& c) -> bool
                                               {
                if (c.UseEventSystem && c.ActiveEventID != 0)
                {
                    return Audio::AudioPlayback::IsEventActive(c.ActiveEventID);
                }
                return c.Source && c.Source->IsPlaying(); }, "Play", [](AudioSourceComponent& c, sol::optional<u64> ownerUUID)
                                               {
                if (c.UseEventSystem && c.StartCommandID.IsValid())
                {
                    if (c.ActiveEventID != 0)
                    {
                        Audio::AudioPlayback::StopEvent(c.ActiveEventID);
                    }
                    u64 objectID = ownerUUID.value_or(0);
                    c.ActiveEventID = Audio::AudioPlayback::PostTrigger(c.StartCommandID, objectID);
                    return;
                }
                if (c.Source) { c.Source->Play(); } }, "Stop", [](AudioSourceComponent& c)
                                               {
                if (c.UseEventSystem && c.ActiveEventID != 0)
                {
                    Audio::AudioPlayback::StopEvent(c.ActiveEventID);
                    c.ActiveEventID = 0;
                    return;
                }
                if (c.Source) { c.Source->Stop(); } }, "Pause", [](AudioSourceComponent& c)
                                               {
                if (c.UseEventSystem && c.ActiveEventID != 0)
                {
                    Audio::AudioPlayback::PauseEvent(c.ActiveEventID);
                    return;
                }
                if (c.Source) { c.Source->Pause(); } }, "UnPause", [](AudioSourceComponent& c)
                                               {
                if (c.UseEventSystem && c.ActiveEventID != 0)
                {
                    Audio::AudioPlayback::ResumeEvent(c.ActiveEventID);
                    return;
                }
                if (c.Source) { c.Source->UnPause(); } },
                                               // --- Spatial audio properties ---
                                               "attenuationModel", sol::property([](const AudioSourceComponent& c)
                                                                                 { return static_cast<int>(c.Config.AttenuationModel); }, [](AudioSourceComponent& c, int v)
                                                                                 {
                    if (v < 0 || v > static_cast<int>(AttenuationModelType::Exponential)) { v = 0; }
                    c.Config.AttenuationModel = static_cast<AttenuationModelType>(v);
                    if (c.Source) { c.Source->SetAttenuationModel(c.Config.AttenuationModel); } }),
                                               "rollOff", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.Config.RollOff; }, [](AudioSourceComponent& c, f32 v)
                                                                        {
                    if (!std::isfinite(v)) v = 1.0f;
                    c.Config.RollOff = v;
                    if (c.Source) { c.Source->SetRollOff(v); } }),
                                               "minGain", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.Config.MinGain; }, [](AudioSourceComponent& c, f32 v)
                                                                        {
                    if (!std::isfinite(v)) v = 0.0f;
                    c.Config.MinGain = v;
                    if (c.Source) { c.Source->SetMinGain(v); } }),
                                               "maxGain", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.Config.MaxGain; }, [](AudioSourceComponent& c, f32 v)
                                                                        {
                    if (!std::isfinite(v)) v = 1.0f;
                    c.Config.MaxGain = v;
                    if (c.Source) { c.Source->SetMaxGain(v); } }),
                                               "minDistance", sol::property([](const AudioSourceComponent& c)
                                                                            { return c.Config.MinDistance; }, [](AudioSourceComponent& c, f32 v)
                                                                            {
                    if (!std::isfinite(v)) v = 0.3f;
                    c.Config.MinDistance = v;
                    if (c.Source) { c.Source->SetMinDistance(v); } }),
                                               "maxDistance", sol::property([](const AudioSourceComponent& c)
                                                                            { return c.Config.MaxDistance; }, [](AudioSourceComponent& c, f32 v)
                                                                            {
                    if (!std::isfinite(v)) v = 1000.0f;
                    c.Config.MaxDistance = v;
                    if (c.Source) { c.Source->SetMaxDistance(v); } }),
                                               "coneInnerAngle", sol::property([](const AudioSourceComponent& c)
                                                                               { return c.Config.ConeInnerAngle; }, [](AudioSourceComponent& c, f32 v)
                                                                               {
                    if (!std::isfinite(v)) v = glm::radians(360.0f);
                    c.Config.ConeInnerAngle = v;
                    if (c.Source) { c.Source->SetCone(c.Config.ConeInnerAngle, c.Config.ConeOuterAngle, c.Config.ConeOuterGain); } }),
                                               "coneOuterAngle", sol::property([](const AudioSourceComponent& c)
                                                                               { return c.Config.ConeOuterAngle; }, [](AudioSourceComponent& c, f32 v)
                                                                               {
                    if (!std::isfinite(v)) v = glm::radians(360.0f);
                    c.Config.ConeOuterAngle = v;
                    if (c.Source) { c.Source->SetCone(c.Config.ConeInnerAngle, c.Config.ConeOuterAngle, c.Config.ConeOuterGain); } }),
                                               "coneOuterGain", sol::property([](const AudioSourceComponent& c)
                                                                              { return c.Config.ConeOuterGain; }, [](AudioSourceComponent& c, f32 v)
                                                                              {
                    if (!std::isfinite(v)) v = 0.0f;
                    c.Config.ConeOuterGain = v;
                    if (c.Source) { c.Source->SetCone(c.Config.ConeInnerAngle, c.Config.ConeOuterAngle, c.Config.ConeOuterGain); } }),
                                               "SetCone", [](AudioSourceComponent& c, f32 innerAngle, f32 outerAngle, f32 outerGain)
                                               {
                    if (!std::isfinite(innerAngle)) { innerAngle = glm::radians(360.0f); }
                    if (!std::isfinite(outerAngle)) { outerAngle = glm::radians(360.0f); }
                    if (!std::isfinite(outerGain)) { outerGain = 0.0f; }
                    c.Config.ConeInnerAngle = innerAngle;
                    c.Config.ConeOuterAngle = outerAngle;
                    c.Config.ConeOuterGain = outerGain;
                    if (c.Source) { c.Source->SetCone(innerAngle, outerAngle, outerGain); } }, "dopplerFactor", sol::property([](const AudioSourceComponent& c)
                                                                                   { return c.Config.DopplerFactor; }, [](AudioSourceComponent& c, f32 v)
                                                                                   {
                    if (!std::isfinite(v)) v = 1.0f;
                    c.Config.DopplerFactor = v;
                    if (c.Source) { c.Source->SetDopplerFactor(v); } }));

        // --- AudioListenerComponent ---
        lua.new_usertype<AudioListenerComponent>("AudioListenerComponent",
                                                 "active", &AudioListenerComponent::Active);

        // --- AudioEvents (global table) ---
        auto audioEventsTable = lua.create_named_table("AudioEvents");
        audioEventsTable["PostTrigger"] = [](const std::string& eventName, u64 objectID) -> u64
        {
            return Audio::AudioPlayback::PostTriggerByName(eventName, objectID);
        };
        audioEventsTable["StopEvent"] = [](u64 eventID)
        {
            Audio::AudioPlayback::StopEvent(eventID);
        };
        audioEventsTable["PauseEvent"] = [](u64 eventID)
        {
            Audio::AudioPlayback::PauseEvent(eventID);
        };
        audioEventsTable["ResumeEvent"] = [](u64 eventID)
        {
            Audio::AudioPlayback::ResumeEvent(eventID);
        };
        audioEventsTable["StopAll"] = []()
        {
            Audio::AudioPlayback::StopAll();
        };
        audioEventsTable["IsEventActive"] = [](u64 eventID) -> bool
        {
            return Audio::AudioPlayback::IsEventActive(eventID);
        };

        // --- NetworkManager (static functions as table) ---
        auto networkTable = lua.create_named_table("Network");
        networkTable.set_function("isServer", &NetworkManager::IsServer);
        networkTable.set_function("isClient", &NetworkManager::IsClient);
        networkTable.set_function("isConnected", &NetworkManager::IsConnected);
        networkTable.set_function("connect", &NetworkManager::Connect);
        networkTable.set_function("disconnect", &NetworkManager::Disconnect);
        networkTable.set_function("startServer", &NetworkManager::StartServer);
        networkTable.set_function("stopServer", &NetworkManager::StopServer);

        // --- Input (raw + action mapping) ---
        auto inputTable = lua.create_named_table("Input");
        inputTable["IsKeyDown"] = [](u16 keycode)
        {
            return Input::IsKeyPressed(keycode);
        };
        inputTable["IsKeyJustPressed"] = [](u16 keycode)
        {
            return Input::IsKeyJustPressed(static_cast<KeyCode>(keycode));
        };
        inputTable["IsKeyJustReleased"] = [](u16 keycode)
        {
            return Input::IsKeyJustReleased(static_cast<KeyCode>(keycode));
        };
        inputTable["IsMouseButtonDown"] = [](u16 button)
        {
            return Input::IsMouseButtonPressed(button);
        };
        inputTable["IsActionPressed"] = [](const std::string& name)
        {
            return InputActionManager::IsActionPressed(name);
        };
        inputTable["IsActionJustPressed"] = [](const std::string& name)
        {
            return InputActionManager::IsActionJustPressed(name);
        };
        inputTable["IsActionJustReleased"] = [](const std::string& name)
        {
            return InputActionManager::IsActionJustReleased(name);
        };
        inputTable["GetActionAxisValue"] = [](const std::string& name)
        {
            return InputActionManager::GetActionAxisValue(name);
        };
        inputTable["GetMousePosition"] = []() -> std::tuple<f32, f32>
        {
            auto pos = Input::GetMousePosition();
            if (Scene* scene = ScriptEngine::GetSceneContext(); scene)
                pos -= scene->GetViewportOffset();
            return { pos.x, pos.y };
        };
        inputTable["GetWindowSize"] = []() -> std::tuple<f32, f32>
        {
            if (Scene* scene = ScriptEngine::GetSceneContext(); scene && scene->GetViewportWidth() > 0)
                return { static_cast<f32>(scene->GetViewportWidth()), static_cast<f32>(scene->GetViewportHeight()) };
            auto& window = Application::Get().GetWindow();
            return { static_cast<f32>(window.GetWidth()), static_cast<f32>(window.GetHeight()) };
        };

        // --- Gamepad functions (raw access) ---
        {
            OLO_PROFILE_SCOPE("LuaScriptGlue::RegisterGamepad");
            auto gamepadTable = lua.create_named_table("Gamepad");
            gamepadTable["IsButtonPressed"] = [](u8 button, i32 index) -> bool
            {
                if (button >= Gamepad::ButtonCount)
                {
                    return false;
                }
                auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsButtonPressed(static_cast<GamepadButton>(button));
            };
            gamepadTable["IsButtonJustPressed"] = [](u8 button, i32 index) -> bool
            {
                if (button >= Gamepad::ButtonCount)
                {
                    return false;
                }
                auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsButtonJustPressed(static_cast<GamepadButton>(button));
            };
            gamepadTable["IsButtonJustReleased"] = [](u8 button, i32 index) -> bool
            {
                if (button >= Gamepad::ButtonCount)
                {
                    return false;
                }
                auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsButtonJustReleased(static_cast<GamepadButton>(button));
            };
            gamepadTable["GetAxis"] = [](u8 axis, i32 index) -> f32
            {
                if (axis >= Gamepad::AxisCount)
                {
                    return 0.0f;
                }
                auto* gp = GamepadManager::GetGamepad(index);
                return gp ? gp->GetAxis(static_cast<GamepadAxis>(axis)) : 0.0f;
            };
            gamepadTable["GetLeftStick"] = [](i32 index) -> glm::vec2
            {
                auto* gp = GamepadManager::GetGamepad(index);
                return gp ? gp->GetLeftStickDeadzone() : glm::vec2(0.0f);
            };
            gamepadTable["GetRightStick"] = [](i32 index) -> glm::vec2
            {
                auto* gp = GamepadManager::GetGamepad(index);
                return gp ? gp->GetRightStickDeadzone() : glm::vec2(0.0f);
            };
            gamepadTable["IsConnected"] = [](i32 index) -> bool
            {
                auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsConnected();
            };
            gamepadTable["GetConnectedCount"] = []() -> i32
            {
                return GamepadManager::GetConnectedCount();
            };

            // --- Gamepad button/axis enum constants ---
            auto gpButtonTable = lua.create_named_table("GamepadButton");
            gpButtonTable["South"] = static_cast<u8>(GamepadButton::South);
            gpButtonTable["East"] = static_cast<u8>(GamepadButton::East);
            gpButtonTable["West"] = static_cast<u8>(GamepadButton::West);
            gpButtonTable["North"] = static_cast<u8>(GamepadButton::North);
            gpButtonTable["LeftBumper"] = static_cast<u8>(GamepadButton::LeftBumper);
            gpButtonTable["RightBumper"] = static_cast<u8>(GamepadButton::RightBumper);
            gpButtonTable["Back"] = static_cast<u8>(GamepadButton::Back);
            gpButtonTable["Start"] = static_cast<u8>(GamepadButton::Start);
            gpButtonTable["Guide"] = static_cast<u8>(GamepadButton::Guide);
            gpButtonTable["LeftThumb"] = static_cast<u8>(GamepadButton::LeftThumb);
            gpButtonTable["RightThumb"] = static_cast<u8>(GamepadButton::RightThumb);
            gpButtonTable["DPadUp"] = static_cast<u8>(GamepadButton::DPadUp);
            gpButtonTable["DPadRight"] = static_cast<u8>(GamepadButton::DPadRight);
            gpButtonTable["DPadDown"] = static_cast<u8>(GamepadButton::DPadDown);
            gpButtonTable["DPadLeft"] = static_cast<u8>(GamepadButton::DPadLeft);

            auto gpAxisTable = lua.create_named_table("GamepadAxis");
            gpAxisTable["LeftX"] = static_cast<u8>(GamepadAxis::LeftX);
            gpAxisTable["LeftY"] = static_cast<u8>(GamepadAxis::LeftY);
            gpAxisTable["RightX"] = static_cast<u8>(GamepadAxis::RightX);
            gpAxisTable["RightY"] = static_cast<u8>(GamepadAxis::RightY);
            gpAxisTable["LeftTrigger"] = static_cast<u8>(GamepadAxis::LeftTrigger);
            gpAxisTable["RightTrigger"] = static_cast<u8>(GamepadAxis::RightTrigger);
        }

        // --- KeyCode constants (mirrors Key:: enum from KeyCodes.h) ---
        {
            auto keyTable = lua.create_named_table("KeyCode");
            keyTable["Space"] = Key::Space;
            keyTable["Apostrophe"] = Key::Apostrophe;
            keyTable["Comma"] = Key::Comma;
            keyTable["Minus"] = Key::Minus;
            keyTable["Period"] = Key::Period;
            keyTable["Slash"] = Key::Slash;
            keyTable["D0"] = Key::D0;
            keyTable["D1"] = Key::D1;
            keyTable["D2"] = Key::D2;
            keyTable["D3"] = Key::D3;
            keyTable["D4"] = Key::D4;
            keyTable["D5"] = Key::D5;
            keyTable["D6"] = Key::D6;
            keyTable["D7"] = Key::D7;
            keyTable["D8"] = Key::D8;
            keyTable["D9"] = Key::D9;
            keyTable["Semicolon"] = Key::Semicolon;
            keyTable["Equal"] = Key::Equal;
            keyTable["A"] = Key::A;
            keyTable["B"] = Key::B;
            keyTable["C"] = Key::C;
            keyTable["D"] = Key::D;
            keyTable["E"] = Key::E;
            keyTable["F"] = Key::F;
            keyTable["G"] = Key::G;
            keyTable["H"] = Key::H;
            keyTable["I"] = Key::I;
            keyTable["J"] = Key::J;
            keyTable["K"] = Key::K;
            keyTable["L"] = Key::L;
            keyTable["M"] = Key::M;
            keyTable["N"] = Key::N;
            keyTable["O"] = Key::O;
            keyTable["P"] = Key::P;
            keyTable["Q"] = Key::Q;
            keyTable["R"] = Key::R;
            keyTable["S"] = Key::S;
            keyTable["T"] = Key::T;
            keyTable["U"] = Key::U;
            keyTable["V"] = Key::V;
            keyTable["W"] = Key::W;
            keyTable["X"] = Key::X;
            keyTable["Y"] = Key::Y;
            keyTable["Z"] = Key::Z;
            keyTable["LeftBracket"] = Key::LeftBracket;
            keyTable["Backslash"] = Key::Backslash;
            keyTable["RightBracket"] = Key::RightBracket;
            keyTable["GraveAccent"] = Key::GraveAccent;
            keyTable["World1"] = Key::World1;
            keyTable["World2"] = Key::World2;
            keyTable["Escape"] = Key::Escape;
            keyTable["Enter"] = Key::Enter;
            keyTable["Tab"] = Key::Tab;
            keyTable["Backspace"] = Key::Backspace;
            keyTable["Insert"] = Key::Insert;
            keyTable["Delete"] = Key::Delete;
            keyTable["Right"] = Key::Right;
            keyTable["Left"] = Key::Left;
            keyTable["Down"] = Key::Down;
            keyTable["Up"] = Key::Up;
            keyTable["PageUp"] = Key::PageUp;
            keyTable["PageDown"] = Key::PageDown;
            keyTable["Home"] = Key::Home;
            keyTable["End"] = Key::End;
            keyTable["CapsLock"] = Key::CapsLock;
            keyTable["ScrollLock"] = Key::ScrollLock;
            keyTable["NumLock"] = Key::NumLock;
            keyTable["PrintScreen"] = Key::PrintScreen;
            keyTable["Pause"] = Key::Pause;
            keyTable["F1"] = Key::F1;
            keyTable["F2"] = Key::F2;
            keyTable["F3"] = Key::F3;
            keyTable["F4"] = Key::F4;
            keyTable["F5"] = Key::F5;
            keyTable["F6"] = Key::F6;
            keyTable["F7"] = Key::F7;
            keyTable["F8"] = Key::F8;
            keyTable["F9"] = Key::F9;
            keyTable["F10"] = Key::F10;
            keyTable["F11"] = Key::F11;
            keyTable["F12"] = Key::F12;
            keyTable["F13"] = Key::F13;
            keyTable["F14"] = Key::F14;
            keyTable["F15"] = Key::F15;
            keyTable["F16"] = Key::F16;
            keyTable["F17"] = Key::F17;
            keyTable["F18"] = Key::F18;
            keyTable["F19"] = Key::F19;
            keyTable["F20"] = Key::F20;
            keyTable["F21"] = Key::F21;
            keyTable["F22"] = Key::F22;
            keyTable["F23"] = Key::F23;
            keyTable["F24"] = Key::F24;
            keyTable["F25"] = Key::F25;
            keyTable["KP0"] = Key::KP0;
            keyTable["KP1"] = Key::KP1;
            keyTable["KP2"] = Key::KP2;
            keyTable["KP3"] = Key::KP3;
            keyTable["KP4"] = Key::KP4;
            keyTable["KP5"] = Key::KP5;
            keyTable["KP6"] = Key::KP6;
            keyTable["KP7"] = Key::KP7;
            keyTable["KP8"] = Key::KP8;
            keyTable["KP9"] = Key::KP9;
            keyTable["KPDecimal"] = Key::KPDecimal;
            keyTable["KPDivide"] = Key::KPDivide;
            keyTable["KPMultiply"] = Key::KPMultiply;
            keyTable["KPSubtract"] = Key::KPSubtract;
            keyTable["KPAdd"] = Key::KPAdd;
            keyTable["KPEnter"] = Key::KPEnter;
            keyTable["KPEqual"] = Key::KPEqual;
            keyTable["LeftShift"] = Key::LeftShift;
            keyTable["LeftControl"] = Key::LeftControl;
            keyTable["LeftAlt"] = Key::LeftAlt;
            keyTable["LeftSuper"] = Key::LeftSuper;
            keyTable["RightShift"] = Key::RightShift;
            keyTable["RightControl"] = Key::RightControl;
            keyTable["RightAlt"] = Key::RightAlt;
            keyTable["RightSuper"] = Key::RightSuper;
            keyTable["Menu"] = Key::Menu;
        }

        // --- MouseButton constants ---
        {
            auto mouseTable = lua.create_named_table("MouseButton");
            mouseTable["Left"] = static_cast<u16>(0);
            mouseTable["Right"] = static_cast<u16>(1);
            mouseTable["Middle"] = static_cast<u16>(2);
            mouseTable["Button4"] = static_cast<u16>(3);
            mouseTable["Button5"] = static_cast<u16>(4);
        }

        // --- DialogueComponent ---
        lua.new_usertype<DialogueComponent>("DialogueComponent",
                                            "dialogueTree", &DialogueComponent::m_DialogueTree,
                                            "autoTrigger", &DialogueComponent::m_AutoTrigger,
                                            "triggerRadius", &DialogueComponent::m_TriggerRadius,
                                            "hasTriggered", &DialogueComponent::m_HasTriggered,
                                            "triggerOnce", &DialogueComponent::m_TriggerOnce);

        // --- AnimationGraphComponent ---
        lua.new_usertype<AnimationGraphComponent>("AnimationGraphComponent", "SetFloat", [](AnimationGraphComponent& comp, const std::string& name, f32 value)
                                                  { comp.Parameters.SetFloat(name, value); }, "SetBool", [](AnimationGraphComponent& comp, const std::string& name, bool value)
                                                  { comp.Parameters.SetBool(name, value); }, "SetInt", [](AnimationGraphComponent& comp, const std::string& name, i32 value)
                                                  { comp.Parameters.SetInt(name, value); }, "SetTrigger", [](AnimationGraphComponent& comp, const std::string& name)
                                                  { comp.Parameters.SetTrigger(name); }, "GetFloat", [](const AnimationGraphComponent& comp, const std::string& name) -> f32
                                                  { return comp.Parameters.GetFloat(name); }, "GetBool", [](const AnimationGraphComponent& comp, const std::string& name) -> bool
                                                  { return comp.Parameters.GetBool(name); }, "GetInt", [](const AnimationGraphComponent& comp, const std::string& name) -> i32
                                                  { return comp.Parameters.GetInt(name); }, "GetCurrentState", [](const AnimationGraphComponent& comp, sol::optional<i32> layerIndex) -> std::string
                                                  {
                                                       if (!comp.RuntimeGraph)
                                                           return "";
                                                       return std::string(comp.RuntimeGraph->GetCurrentStateName(layerIndex.value_or(0))); });

        // --- MorphTargetComponent ---
        lua.new_usertype<MorphTargetComponent>("MorphTargetComponent", "SetWeight", [](MorphTargetComponent& comp, const std::string& name, f32 weight)
                                               { comp.SetWeight(name, weight); }, "GetWeight", [](const MorphTargetComponent& comp, const std::string& name) -> f32
                                               { return comp.GetWeight(name); }, "ResetAll", &MorphTargetComponent::ResetAllWeights, "HasActiveWeights", &MorphTargetComponent::HasActiveWeights, "GetTargetCount", [](const MorphTargetComponent& comp) -> u32
                                               { return comp.MorphTargets ? comp.MorphTargets->GetTargetCount() : 0; }, "ApplyExpression", [](MorphTargetComponent& comp, const std::string& name, sol::optional<f32> blend)
                                               { FacialExpressionLibrary::ApplyExpression(comp, name, blend.value_or(1.0f)); });

        // --- MaterialComponent ---
        lua.new_usertype<MaterialComponent>("MaterialComponent",
                                            "shaderGraphHandle",
                                            sol::property(
                                                [](const MaterialComponent& mc) -> u64
                                                { return static_cast<u64>(mc.m_ShaderGraphHandle); },
                                                [](MaterialComponent& mc, u64 handle)
                                                {
                                                    if (handle != 0)
                                                    {
                                                        if (auto graphAsset = AssetManager::GetAsset<ShaderGraphAsset>(handle))
                                                        {
                                                            if (auto shader = graphAsset->CompileToShader("ShaderGraph_" + std::to_string(handle)))
                                                            {
                                                                mc.m_ShaderGraphHandle = handle;
                                                                mc.m_Material.SetShader(shader);
                                                                return;
                                                            }
                                                        }
                                                        OLO_CORE_WARN("[Lua] Failed to compile ShaderGraph handle {}", handle);
                                                    }
                                                    else
                                                    {
                                                        mc.m_ShaderGraphHandle = 0;
                                                        mc.m_Material.SetShader(nullptr);
                                                    }
                                                }),
                                            "albedoColor",
                                            sol::property(
                                                [](const MaterialComponent& mc) -> glm::vec4
                                                { return mc.m_Material.GetBaseColorFactor(); },
                                                [](MaterialComponent& mc, const glm::vec4& color)
                                                { mc.m_Material.SetBaseColorFactor(color); }));

        // --- NavAgentComponent ---
        lua.new_usertype<NavAgentComponent>("NavAgentComponent",
                                            "radius", &NavAgentComponent::m_Radius,
                                            "height", &NavAgentComponent::m_Height,
                                            "maxSpeed", &NavAgentComponent::m_MaxSpeed,
                                            "acceleration", &NavAgentComponent::m_Acceleration,
                                            "stoppingDistance", &NavAgentComponent::m_StoppingDistance,
                                            "avoidancePriority", &NavAgentComponent::m_AvoidancePriority,
                                            "targetPosition", sol::property([](const NavAgentComponent& a)
                                                                            { return a.m_TargetPosition; }, [](NavAgentComponent& a, const glm::vec3& pos)
                                                                            {
                                                    a.m_TargetPosition = pos;
                                                    a.m_HasTarget = true;
                                                    a.m_HasPath = false;
                                                    a.m_PathCorners.clear();
                                                    a.m_CurrentCornerIndex = 0; }),
                                            "hasTarget", sol::readonly(&NavAgentComponent::m_HasTarget),
                                            "hasPath", sol::readonly(&NavAgentComponent::m_HasPath),
                                            "lockYAxis", &NavAgentComponent::m_LockYAxis,
                                            "clearTarget", [](NavAgentComponent& agent)
                                            {
                                                agent.m_HasTarget = false;
                                                agent.m_HasPath = false;
                                                agent.m_PathCorners.clear();
                                                agent.m_CurrentCornerIndex = 0; });

        // --- NavMeshBoundsComponent ---
        lua.new_usertype<NavMeshBoundsComponent>("NavMeshBoundsComponent",
                                                 "min", &NavMeshBoundsComponent::m_Min,
                                                 "max", &NavMeshBoundsComponent::m_Max);

        // --- Dialogue system functions ---
        auto dialogueTable = lua.create_named_table("dialogue");
        dialogueTable["start"] = [](Entity* entity)
        {
            if (!entity)
                return;
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->StartDialogue(*entity);
        };
        dialogueTable["advance"] = [](Entity* entity)
        {
            if (!entity)
                return;
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->AdvanceDialogue(*entity);
        };
        dialogueTable["select_choice"] = [](Entity* entity, i32 index)
        {
            if (!entity)
                return;
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->SelectChoice(*entity, index);
        };
        dialogueTable["is_active"] = [](Entity* entity) -> bool
        {
            if (!entity)
                return false;
            return entity->HasComponent<DialogueStateComponent>();
        };
        dialogueTable["end_dialogue"] = [](Entity* entity)
        {
            if (!entity)
                return;
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->EndDialogue(*entity);
        };

        // --- Dialogue variables ---
        auto varsTable = lua.create_named_table("dialogue_vars");
        varsTable["get_bool"] = [](const std::string& key) -> bool
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return false;
            return scene->GetDialogueVariables().GetBool(key);
        };
        varsTable["set_bool"] = [](const std::string& key, bool val)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().SetBool(key, val);
        };
        varsTable["get_int"] = [](const std::string& key) -> i32
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return 0;
            return scene->GetDialogueVariables().GetInt(key);
        };
        varsTable["set_int"] = [](const std::string& key, i32 val)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().SetInt(key, val);
        };
        varsTable["get_float"] = [](const std::string& key) -> f32
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return 0.0f;
            return scene->GetDialogueVariables().GetFloat(key);
        };
        varsTable["set_float"] = [](const std::string& key, f32 val)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().SetFloat(key, val);
        };
        varsTable["get_string"] = [](const std::string& key) -> std::string
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return "";
            return scene->GetDialogueVariables().GetString(key);
        };
        varsTable["set_string"] = [](const std::string& key, const std::string& val)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().SetString(key, val);
        };
        varsTable["has"] = [](const std::string& key) -> bool
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return false;
            return scene->GetDialogueVariables().Has(key);
        };
        varsTable["clear"] = []()
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().Clear();
        };

        // --- SaveGame ---
        auto saveGameTable = lua.create_named_table("SaveGame");
        saveGameTable["Save"] = [](const std::string& slotName, const std::string& displayName) -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::Save");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(SaveLoadResult::NoActiveScene);
            return static_cast<i32>(SaveGameManager::Save(*scene, slotName, displayName));
        };
        saveGameTable["Load"] = [](const std::string& slotName) -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::Load");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(SaveLoadResult::NoActiveScene);
            return static_cast<i32>(SaveGameManager::Load(*scene, slotName));
        };
        saveGameTable["QuickSave"] = []() -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::QuickSave");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(SaveLoadResult::NoActiveScene);
            return static_cast<i32>(SaveGameManager::QuickSave(*scene));
        };
        saveGameTable["QuickLoad"] = []() -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::QuickLoad");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(SaveLoadResult::NoActiveScene);
            return static_cast<i32>(SaveGameManager::QuickLoad(*scene));
        };
        saveGameTable["EnumerateSaves"] = []() -> sol::table
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::EnumerateSaves");
            auto& luaState = *Scripting::GetState();
            auto saves = SaveGameManager::EnumerateSaves();
            sol::table result = luaState.create_table(static_cast<int>(saves.size()), 0);
            int index = 1;
            for (const auto& info : saves)
            {
                sol::table entry = luaState.create_table(0, 3);
                entry["SlotName"] = info.FilePath.stem().string();
                entry["DisplayName"] = info.Metadata.DisplayName;
                entry["TimestampUTC"] = info.Metadata.TimestampUTC;
                result[index++] = entry;
            }
            return result;
        };
        saveGameTable["DeleteSave"] = [](const std::string& slotName)
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::DeleteSave");
            return SaveGameManager::DeleteSave(slotName);
        };
        saveGameTable["ValidateSave"] = [](const std::string& slotName)
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::ValidateSave");
            return SaveGameManager::ValidateSave(slotName);
        };
        saveGameTable["GetAutoSaveInterval"] = []()
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::GetAutoSaveInterval");
            return SaveGameManager::GetAutoSaveInterval();
        };
        saveGameTable["SetAutoSaveInterval"] = [](f32 interval)
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::SetAutoSaveInterval");
            SaveGameManager::SetAutoSaveInterval(interval);
        };

        // --- BehaviorTreeComponent ---
        OLO_PROFILE_SCOPE("Lua::RegisterAITypes");
        lua.new_usertype<BehaviorTreeComponent>("BehaviorTreeComponent", "SetBlackboardBool", [](BehaviorTreeComponent& comp, const std::string& key, bool value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardBool", [](const BehaviorTreeComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Get<bool>(key); }, "SetBlackboardInt", [](BehaviorTreeComponent& comp, const std::string& key, i32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardInt", [](const BehaviorTreeComponent& comp, const std::string& key) -> i32
                                                { return comp.Blackboard.Get<i32>(key); }, "SetBlackboardFloat", [](BehaviorTreeComponent& comp, const std::string& key, f32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardFloat", [](const BehaviorTreeComponent& comp, const std::string& key) -> f32
                                                { return comp.Blackboard.Get<f32>(key); }, "SetBlackboardString", [](BehaviorTreeComponent& comp, const std::string& key, const std::string& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardString", [](const BehaviorTreeComponent& comp, const std::string& key) -> std::string
                                                { return comp.Blackboard.Get<std::string>(key); }, "SetBlackboardVec3", [](BehaviorTreeComponent& comp, const std::string& key, const glm::vec3& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardVec3", [](const BehaviorTreeComponent& comp, const std::string& key) -> glm::vec3
                                                { return comp.Blackboard.Get<glm::vec3>(key); }, "SetBlackboardUUID", [](BehaviorTreeComponent& comp, const std::string& key, u64 value)
                                                { comp.Blackboard.Set(key, UUID(value)); }, "GetBlackboardUUID", [](const BehaviorTreeComponent& comp, const std::string& key) -> u64
                                                { return static_cast<u64>(comp.Blackboard.Get<UUID>(key)); }, "RemoveBlackboardKey", [](BehaviorTreeComponent& comp, const std::string& key)
                                                { comp.Blackboard.Remove(key); }, "HasBlackboardKey", [](const BehaviorTreeComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Has(key); }, "IsRunning", sol::readonly(&BehaviorTreeComponent::IsRunning));

        // --- StateMachineComponent ---
        lua.new_usertype<StateMachineComponent>("StateMachineComponent", "SetBlackboardBool", [](StateMachineComponent& comp, const std::string& key, bool value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardBool", [](const StateMachineComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Get<bool>(key); }, "SetBlackboardInt", [](StateMachineComponent& comp, const std::string& key, i32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardInt", [](const StateMachineComponent& comp, const std::string& key) -> i32
                                                { return comp.Blackboard.Get<i32>(key); }, "SetBlackboardFloat", [](StateMachineComponent& comp, const std::string& key, f32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardFloat", [](const StateMachineComponent& comp, const std::string& key) -> f32
                                                { return comp.Blackboard.Get<f32>(key); }, "SetBlackboardString", [](StateMachineComponent& comp, const std::string& key, const std::string& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardString", [](const StateMachineComponent& comp, const std::string& key) -> std::string
                                                { return comp.Blackboard.Get<std::string>(key); }, "SetBlackboardVec3", [](StateMachineComponent& comp, const std::string& key, const glm::vec3& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardVec3", [](const StateMachineComponent& comp, const std::string& key) -> glm::vec3
                                                { return comp.Blackboard.Get<glm::vec3>(key); }, "SetBlackboardUUID", [](StateMachineComponent& comp, const std::string& key, u64 value)
                                                { comp.Blackboard.Set(key, UUID(value)); }, "GetBlackboardUUID", [](const StateMachineComponent& comp, const std::string& key) -> u64
                                                { return static_cast<u64>(comp.Blackboard.Get<UUID>(key)); }, "RemoveBlackboardKey", [](StateMachineComponent& comp, const std::string& key)
                                                { comp.Blackboard.Remove(key); }, "HasBlackboardKey", [](const StateMachineComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Has(key); }, "GetCurrentState", [](const StateMachineComponent& comp) -> std::string
                                                {
                if (comp.RuntimeFSM && comp.RuntimeFSM->IsStarted())
                    return comp.RuntimeFSM->GetCurrentStateID();
                return ""; }, "ForceTransition", [](StateMachineComponent& comp, Entity entity, const std::string& stateId)
                                                {
                if (comp.RuntimeFSM)
                    comp.RuntimeFSM->ForceTransition(stateId, entity, comp.Blackboard); });

        // --- InventoryComponent ---
        lua.new_usertype<InventoryComponent>("InventoryComponent", "currency", &InventoryComponent::Currency, "AddItem", [](InventoryComponent& comp, const std::string& itemId, sol::optional<i32> count)
                                             {
                                                 i32 total = count.value_or(1);
                                                 if (total <= 0)
                                                     return false;
                                                 const auto* def = ItemDatabase::Get(itemId);
                                                 if (!def)
                                                     return false;
                                                 i32 maxStack = std::max(def->MaxStackSize, 1);
                                                 i32 remaining = total;
                                                 while (remaining > 0)
                                                 {
                                                     ItemInstance instance;
                                                     instance.InstanceID = UUID();
                                                     instance.ItemDefinitionID = itemId;
                                                     instance.StackCount = std::min(remaining, maxStack);
                                                     if (!comp.PlayerInventory.AddItem(instance))
                                                         return false;
                                                     remaining -= instance.StackCount;
                                                 }
                                                 return true; }, "RemoveItem", [](InventoryComponent& comp, const std::string& itemId, sol::optional<i32> count)
                                             { return comp.PlayerInventory.RemoveItemByDefinition(itemId, count.value_or(1)); }, "HasItem", [](const InventoryComponent& comp, const std::string& itemId, sol::optional<i32> count) -> bool
                                             { return comp.PlayerInventory.HasItem(itemId, count.value_or(1)); }, "CountItem", [](const InventoryComponent& comp, const std::string& itemId) -> i32
                                             { return comp.PlayerInventory.CountItem(itemId); }, "GetUsedSlots", [](const InventoryComponent& comp) -> i32
                                             { return comp.PlayerInventory.GetUsedSlots(); }, "GetCapacity", [](const InventoryComponent& comp) -> i32
                                             { return comp.PlayerInventory.GetCapacity(); }, "GetTotalWeight", [](const InventoryComponent& comp) -> f32
                                             { return comp.PlayerInventory.GetTotalWeight(); });

        // --- ItemPickupComponent ---
        lua.new_usertype<ItemPickupComponent>("ItemPickupComponent",
                                              "pickupRadius", &ItemPickupComponent::PickupRadius,
                                              "autoPickup", &ItemPickupComponent::AutoPickup,
                                              "despawnTimer", &ItemPickupComponent::DespawnTimer);

        // --- ItemContainerComponent ---
        lua.new_usertype<ItemContainerComponent>("ItemContainerComponent",
                                                 "isShop", &ItemContainerComponent::IsShop,
                                                 "lootTableID", &ItemContainerComponent::LootTableID,
                                                 "hasBeenLooted", &ItemContainerComponent::HasBeenLooted);

        // --- QuestJournalComponent ---
        lua.new_usertype<QuestJournalComponent>("QuestJournalComponent", "AcceptQuest", [](QuestJournalComponent& comp, const std::string& questId) -> bool
                                                {
                const auto* def = QuestDatabase::Get(questId);
                if (!def)
                    return false;
                return comp.Journal.AcceptQuest(questId, *def); }, "AbandonQuest", [](QuestJournalComponent& comp, const std::string& questId) -> bool
                                                { return comp.Journal.AbandonQuest(questId); }, "CompleteQuest", [](QuestJournalComponent& comp, const std::string& questId, sol::optional<std::string> branch) -> bool
                                                { return comp.Journal.CompleteQuest(questId, branch.value_or("")).has_value(); }, "IsQuestActive", [](const QuestJournalComponent& comp, const std::string& questId) -> bool
                                                { return comp.Journal.IsQuestActive(questId); }, "HasCompletedQuest", [](const QuestJournalComponent& comp, const std::string& questId) -> bool
                                                { return comp.Journal.HasCompletedQuest(questId); }, "IncrementObjective", [](QuestJournalComponent& comp, const std::string& questId, const std::string& objId, sol::optional<i32> amount)
                                                { i32 amt = amount.value_or(1); if (amt <= 0) return; comp.Journal.IncrementObjective(questId, objId, amt); }, "NotifyKill", [](QuestJournalComponent& comp, const std::string& targetTag)
                                                { comp.Journal.NotifyKill(targetTag); }, "NotifyCollect", [](QuestJournalComponent& comp, const std::string& itemId, sol::optional<i32> count)
                                                { i32 cnt = count.value_or(1); if (cnt <= 0) return; comp.Journal.NotifyCollect(itemId, cnt); }, "NotifyInteract", [](QuestJournalComponent& comp, const std::string& id)
                                                { comp.Journal.NotifyInteract(id); }, "NotifyReachLocation", [](QuestJournalComponent& comp, const std::string& locId)
                                                { comp.Journal.NotifyReachLocation(locId); }, "HasTag", [](const QuestJournalComponent& comp, const std::string& tag) -> bool
                                                { return comp.Journal.HasTag(tag); }, "AddTag", [](QuestJournalComponent& comp, const std::string& tag)
                                                { comp.Journal.AddTag(tag); }, "SetPlayerLevel", [](QuestJournalComponent& comp, i32 level)
                                                { if (level < 0) return; comp.Journal.SetPlayerLevel(level); }, "GetPlayerLevel", [](const QuestJournalComponent& comp) -> i32
                                                { return comp.Journal.GetPlayerLevel(); }, "SetReputation", [](QuestJournalComponent& comp, const std::string& factionId, i32 value)
                                                { comp.Journal.SetReputation(factionId, value); }, "GetReputation", [](const QuestJournalComponent& comp, const std::string& factionId) -> i32
                                                { return comp.Journal.GetReputation(factionId); }, "SetItemCount", [](QuestJournalComponent& comp, const std::string& itemId, i32 count)
                                                { if (count < 0) return; comp.Journal.SetItemCount(itemId, count); }, "GetItemCount", [](const QuestJournalComponent& comp, const std::string& itemId) -> i32
                                                { return comp.Journal.GetItemCount(itemId); }, "SetStat", [](QuestJournalComponent& comp, const std::string& statName, i32 value)
                                                { comp.Journal.SetStat(statName, value); }, "GetStat", [](const QuestJournalComponent& comp, const std::string& statName) -> i32
                                                { return comp.Journal.GetStat(statName); }, "SetPlayerClass", [](QuestJournalComponent& comp, const std::string& className)
                                                { comp.Journal.SetPlayerClass(className); }, "GetPlayerClass", [](const QuestJournalComponent& comp) -> std::string
                                                { return comp.Journal.GetPlayerClass(); }, "SetPlayerFaction", [](QuestJournalComponent& comp, const std::string& factionName)
                                                { comp.Journal.SetPlayerFaction(factionName); }, "GetPlayerFaction", [](const QuestJournalComponent& comp) -> std::string
                                                { return comp.Journal.GetPlayerFaction(); });

        // --- QuestGiverComponent ---
        lua.new_usertype<QuestGiverComponent>("QuestGiverComponent",
                                              "questMarkerIcon", &QuestGiverComponent::QuestMarkerIcon,
                                              "offeredQuestIDs", &QuestGiverComponent::OfferedQuestIDs,
                                              "turnInQuestIDs", &QuestGiverComponent::TurnInQuestIDs);

        // --- Log (global table) ---
        auto logTable = lua.create_named_table("Log");
        logTable["Trace"] = [](const std::string& msg)
        { OLO_TRACE("{}", msg); };
        logTable["Info"] = [](const std::string& msg)
        { OLO_INFO("{}", msg); };
        logTable["Warn"] = [](const std::string& msg)
        { OLO_WARN("{}", msg); };
        logTable["Error"] = [](const std::string& msg)
        { OLO_ERROR("{}", msg); };

        // --- Entity utilities ---
        auto entityUtilsTable = lua.create_named_table("entity_utils");
        entityUtilsTable["is_valid"] = [](Entity* entity) -> bool
        {
            if (!entity)
                return false;
            Scene* scene = ScriptEngine::GetSceneContext();
            return scene && scene->TryGetEntityWithUUID(entity->GetUUID()).has_value();
        };

        // --- Physics (raycast) ---
        auto physicsTable = lua.create_named_table("Physics");
        physicsTable["Raycast"] = [](const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance, sol::this_state s) -> sol::object
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return sol::make_object(s, sol::nil);

            JoltScene* joltScene = scene->GetPhysicsScene();
            if (!joltScene)
                return sol::make_object(s, sol::nil);

            RayCastInfo rayInfo(origin, direction, maxDistance);
            SceneQueryHit hit;
            if (!joltScene->CastRay(rayInfo, hit))
                return sol::make_object(s, sol::nil);

            sol::state_view lua(s);
            sol::table result = lua.create_table(0, 4);
            result["position"] = hit.m_Position;
            result["normal"] = hit.m_Normal;
            result["distance"] = hit.m_Distance;
            result["entityID"] = static_cast<u64>(hit.m_HitEntity);
            return result;
        };

        // --- Camera (screen-to-world) ---
        auto cameraTable = lua.create_named_table("Camera");
        cameraTable["ScreenToWorldRay"] = [](u64 cameraEntityID, const glm::vec2& screenPos, sol::this_state s) -> sol::object
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return sol::make_object(s, sol::nil);

            auto cameraOpt = scene->TryGetEntityWithUUID(UUID(cameraEntityID));
            if (!cameraOpt)
                return sol::make_object(s, sol::nil);
            Entity cameraEntity{ static_cast<entt::entity>(*cameraOpt), scene };
            if (!cameraEntity.HasComponent<CameraComponent>() || !cameraEntity.HasComponent<TransformComponent>())
                return sol::make_object(s, sol::nil);

            auto const& cameraComp = cameraEntity.GetComponent<CameraComponent>();
            auto const& transform = cameraEntity.GetComponent<TransformComponent>();

            glm::mat4 viewMatrix = glm::inverse(transform.GetTransform());
            glm::mat4 projMatrix = cameraComp.Camera.GetProjection();
            glm::mat4 invVP = glm::inverse(projMatrix * viewMatrix);

            // screenPos is in pixels (from Input.GetMousePosition); normalise to [0,1]
            auto& window = Application::Get().GetWindow();
            const f32 winW = static_cast<f32>(window.GetWidth());
            const f32 winH = static_cast<f32>(window.GetHeight());
            if (winW <= 0.0f || winH <= 0.0f)
                return sol::make_object(s, sol::nil);

            const f32 normX = screenPos.x / winW;
            const f32 normY = screenPos.y / winH;

            f32 ndcX = normX * 2.0f - 1.0f;
            f32 ndcY = 1.0f - normY * 2.0f; // Flip Y: screen top-left origin → NDC bottom-left origin

            glm::vec4 nearPoint = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
            glm::vec4 farPoint = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
            nearPoint /= nearPoint.w;
            farPoint /= farPoint.w;

            sol::state_view lua(s);
            sol::table result = lua.create_table(0, 2);
            result["origin"] = glm::vec3(nearPoint);
            result["direction"] = glm::normalize(glm::vec3(farPoint - nearPoint));
            return result;
        };

        // --- AbilityComponent ---
        lua.new_usertype<AbilityComponent>("AbilityComponent", "GetAttribute", [](const AbilityComponent& comp, const std::string& name) -> f32
                                           { return comp.Attributes.GetBaseValue(name); }, "SetAttribute", [](AbilityComponent& comp, const std::string& name, f32 value)
                                           { comp.Attributes.SetBaseValue(name, value); }, "GetCurrentAttribute", [](const AbilityComponent& comp, const std::string& name) -> f32
                                           { return comp.Attributes.GetCurrentValue(name); }, "HasTag", [](const AbilityComponent& comp, const std::string& tag) -> bool
                                           { return comp.OwnedTags.HasTagExact(GameplayTag(tag)); }, "AddTag", [](AbilityComponent& comp, const std::string& tag)
                                           { comp.OwnedTags.AddTag(GameplayTag(tag)); }, "RemoveTag", [](AbilityComponent& comp, const std::string& tag)
                                           { comp.OwnedTags.RemoveTag(GameplayTag(tag)); }, "DefineAttribute", [](AbilityComponent& comp, const std::string& name, f32 baseValue)
                                           { comp.Attributes.DefineAttribute(name, baseValue); }, "InitDefaultRPG", [](AbilityComponent& comp, f32 maxHP, f32 maxMana, f32 atk, f32 def)
                                           { comp.InitializeDefaultRPGAttributes(maxHP, maxMana, atk, def); });

        // --- Damage routing (cross-entity, uses scene context) ---
        auto damageTable = lua.create_named_table("Damage");
        damageTable["ApplyToTarget"] = [](u64 sourceID, u64 targetID, f32 rawDamage, sol::optional<std::string> damageType, sol::optional<bool> isCritical) -> f32
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return 0.0f;

            auto sourceOpt = scene->TryGetEntityWithUUID(UUID(sourceID));
            auto targetOpt = scene->TryGetEntityWithUUID(UUID(targetID));
            if (!sourceOpt || !targetOpt)
                return 0.0f;
            Entity source{ static_cast<entt::entity>(*sourceOpt), scene };
            Entity target{ static_cast<entt::entity>(*targetOpt), scene };

            if (!source.HasComponent<AbilityComponent>() || !target.HasComponent<AbilityComponent>())
                return 0.0f;

            auto const& sourceAC = source.GetComponent<AbilityComponent>();
            auto& targetAC = target.GetComponent<AbilityComponent>();

            DamageEvent event;
            event.Source = source;
            event.Target = target;
            event.RawDamage = rawDamage;
            event.IsCritical = isCritical.value_or(false);
            event.CritMultiplier = sourceAC.Attributes.GetCurrentValue("CritMultiplier");
            const std::string dt = damageType.value_or("Physical");
            event.DamageType = GameplayTag(dt);

            f32 finalDamage = DamageCalculation::Calculate(event, sourceAC.Attributes, targetAC.Attributes);

            f32 currentHealth = targetAC.Attributes.GetCurrentValue("Health");
            targetAC.Attributes.SetBaseValue("Health", std::max(currentHealth - finalDamage, 0.0f));

            return finalDamage;
        };

        damageTable["TryActivateAbility"] = [](u64 casterID, const std::string& abilityTag) -> bool
        {
            if (abilityTag.empty())
                return false;

            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return false;

            auto casterOpt = scene->TryGetEntityWithUUID(UUID(casterID));
            if (!casterOpt)
                return false;
            Entity caster{ static_cast<entt::entity>(*casterOpt), scene };

            if (!caster.HasComponent<AbilityComponent>())
                return false;

            return GameplayAbilitySystem::TryActivateAbility(scene, caster, GameplayTag(abilityTag));
        };

        damageTable["TryActivateAbilityOnTarget"] = [](u64 casterID, const std::string& abilityTag, u64 targetID) -> bool
        {
            if (abilityTag.empty())
                return false;

            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return false;

            auto casterOpt = scene->TryGetEntityWithUUID(UUID(casterID));
            auto targetOpt = scene->TryGetEntityWithUUID(UUID(targetID));
            if (!casterOpt || !targetOpt)
                return false;
            Entity caster{ static_cast<entt::entity>(*casterOpt), scene };
            Entity target{ static_cast<entt::entity>(*targetOpt), scene };

            if (!caster.HasComponent<AbilityComponent>() || !target.HasComponent<AbilityComponent>())
                return false;

            GameplayTag tag(abilityTag);
            // Activate on the caster (checks cooldowns, costs, tags).
            // TryActivateAbility also applies ActivationEffects to the caster;
            // for targeted abilities we additionally redirect effects to the
            // target below (intentional self+target duplication — see C# mirror).
            if (!GameplayAbilitySystem::TryActivateAbility(scene, caster, tag))
                return false;

            auto& casterAC = caster.GetComponent<AbilityComponent>();
            for (auto& ability : casterAC.Abilities)
            {
                if (ability.Definition.AbilityTag == tag)
                {
                    if (!ability.Definition.TargetActivationEffects.empty())
                    {
                        auto& targetAC = target.GetComponent<AbilityComponent>();
                        for (auto const& effect : ability.Definition.TargetActivationEffects)
                        {
                            targetAC.ActiveEffects.ApplyEffect(effect, targetAC.OwnedTags, tag);
                        }
                    }
                    break;
                }
            }

            return true;
        };

        // --- ShaderLibrary3D (global table) ---
        auto shaderLib3D = lua.create_named_table("ShaderLibrary3D");

        shaderLib3D["Load"] = [](const std::string& filepath) -> bool
        {
            auto shader = Renderer3D::GetShaderLibrary().Load(filepath);
            return shader != nullptr;
        };
        shaderLib3D["Exists"] = [](const std::string& name) -> bool
        {
            return Renderer3D::GetShaderLibrary().Exists(name);
        };
        shaderLib3D["ReloadAll"] = []()
        {
            Renderer3D::GetShaderLibrary().ReloadShaders();
        };
        shaderLib3D["Reload"] = [](const std::string& name)
        {
            auto& library = Renderer3D::GetShaderLibrary();
            if (library.Exists(name))
            {
                library.Get(name)->Reload();
            }
        };
        shaderLib3D["GetShaderCount"] = []() -> u32
        {
            return Renderer3D::GetShaderLibrary().GetTotalCount();
        };
        shaderLib3D["GetAllNames"] = []() -> sol::table
        {
            auto& luaState = *Scripting::GetState();
            auto names = Renderer3D::GetShaderLibrary().GetAllShaderNames();
            sol::table result = luaState.create_table(static_cast<int>(names.size()), 0);
            for (size_t i = 0; i < names.size(); ++i)
            {
                result[static_cast<int>(i) + 1] = names[i];
            }
            return result;
        };

        // Backward-compatible alias: ShaderLibrary = ShaderLibrary3D
        lua["ShaderLibrary"] = shaderLib3D;

        // --- ShaderLibrary2D (global table) ---
        auto shaderLib2D = lua.create_named_table("ShaderLibrary2D");
        shaderLib2D["Load"] = [](const std::string& filepath) -> bool
        {
            auto shader = Renderer2D::GetShaderLibrary().Load(filepath);
            return shader != nullptr;
        };
        shaderLib2D["Exists"] = [](const std::string& name) -> bool
        {
            return Renderer2D::GetShaderLibrary().Exists(name);
        };
        shaderLib2D["ReloadAll"] = []()
        {
            Renderer2D::GetShaderLibrary().ReloadShaders();
        };
        shaderLib2D["Reload"] = [](const std::string& name)
        {
            auto& library = Renderer2D::GetShaderLibrary();
            if (library.Exists(name))
            {
                library.Get(name)->Reload();
            }
        };
        shaderLib2D["GetShaderCount"] = []() -> u32
        {
            return Renderer2D::GetShaderLibrary().GetTotalCount();
        };
        shaderLib2D["GetAllNames"] = []() -> sol::table
        {
            auto& luaState = *Scripting::GetState();
            auto names = Renderer2D::GetShaderLibrary().GetAllShaderNames();
            sol::table result = luaState.create_table(static_cast<int>(names.size()), 0);
            for (size_t i = 0; i < names.size(); ++i)
            {
                result[static_cast<int>(i) + 1] = names[i];
            }
            return result;
        };

        // --- Application (global table) ---
        auto appTable = lua.create_named_table("Application");
        appTable["GetTimeScale"] = []() -> f32
        {
            return Application::Get().GetTimeScale();
        };
        appTable["SetTimeScale"] = [](f32 scale)
        {
            if (!std::isfinite(scale))
            {
                scale = 1.0f;
            }
            scale = std::clamp(scale, 0.0f, 100.0f);
            Application::Get().SetTimeScale(scale);
        };
        appTable["QuitGame"] = []()
        {
            Application::Get().Close();
        };

        // --- Entity lookup utilities ---
        entityUtilsTable["find_by_name"] = [](const std::string& name) -> sol::object
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return sol::nil;
            Entity entity = scene->FindEntityByName(name);
            if (!entity)
                return sol::nil;
            return sol::make_object(*Scripting::GetState(), static_cast<u64>(entity.GetUUID()));
        };

        // --- Entity convenience helpers (mirror C# Entity base-class properties) ---
        entityUtilsTable["get_translation"] = [](u64 entityID) -> glm::vec3
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return glm::vec3(0.0f);
            if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID)))
            {
                Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                if (entity.HasComponent<TransformComponent>())
                    return entity.GetComponent<TransformComponent>().Translation;
            }
            return glm::vec3(0.0f);
        };

        entityUtilsTable["set_translation"] = [](u64 entityID, const glm::vec3& translation)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return;
            if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID)))
            {
                Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                if (entity.HasComponent<TransformComponent>())
                {
                    entity.GetComponent<TransformComponent>().Translation = translation;
                    // Sync to Box2D runtime body if present
                    if (entity.HasComponent<Rigidbody2DComponent>())
                    {
                        auto const& rb = entity.GetComponent<Rigidbody2DComponent>();
                        if (b2Body_IsValid(rb.RuntimeBody))
                        {
                            auto const& tc = entity.GetComponent<TransformComponent>();
                            b2Body_SetTransform(rb.RuntimeBody, { tc.Translation.x, tc.Translation.y }, b2MakeRot(tc.GetRotationEuler().z));
                        }
                    }
                }
            }
        };

        entityUtilsTable["get_rotation"] = [](u64 entityID) -> glm::vec3
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return glm::vec3(0.0f);
            if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID)))
            {
                Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                if (entity.HasComponent<TransformComponent>())
                    return entity.GetComponent<TransformComponent>().GetRotationEuler();
            }
            return glm::vec3(0.0f);
        };

        entityUtilsTable["set_rotation"] = [](u64 entityID, const glm::vec3& rotation)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return;
            if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID)))
            {
                Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                if (entity.HasComponent<TransformComponent>())
                {
                    entity.GetComponent<TransformComponent>().SetRotationEuler(rotation);
                    // Sync to Box2D runtime body if present
                    if (entity.HasComponent<Rigidbody2DComponent>())
                    {
                        auto const& rb = entity.GetComponent<Rigidbody2DComponent>();
                        if (b2Body_IsValid(rb.RuntimeBody))
                        {
                            auto const& tc = entity.GetComponent<TransformComponent>();
                            b2Body_SetTransform(rb.RuntimeBody, { tc.Translation.x, tc.Translation.y }, b2MakeRot(tc.GetRotationEuler().z));
                        }
                    }
                }
            }
        };

        entityUtilsTable["get_scale"] = [](u64 entityID) -> glm::vec3
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return glm::vec3(1.0f);
            if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID)))
            {
                Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                if (entity.HasComponent<TransformComponent>())
                    return entity.GetComponent<TransformComponent>().Scale;
            }
            return glm::vec3(1.0f);
        };

        entityUtilsTable["set_scale"] = [](u64 entityID, const glm::vec3& scale)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return;
            if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID)))
            {
                Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                if (entity.HasComponent<TransformComponent>())
                    entity.GetComponent<TransformComponent>().Scale = scale;
            }
        };

        entityUtilsTable["get_name"] = [](u64 entityID, sol::this_state s) -> sol::object
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return sol::make_object(s, sol::nil);
            if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID)))
            {
                Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                if (entity.HasComponent<TagComponent>())
                    return sol::make_object(s, entity.GetComponent<TagComponent>().Tag);
            }
            return sol::make_object(s, sol::nil);
        };

        // --- Entity component access (by UUID + component name string) ---
        // Returns a LuaComponentProxy that re-resolves the component from EnTT
        // on every property access, preventing dangling-pointer UB.
        entityUtilsTable["get_component"] = [](u64 entityID, const std::string& compName, sol::this_state s) -> sol::object
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return sol::make_object(s, sol::nil);

            auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID));
            if (!entityOpt)
                return sol::make_object(s, sol::nil);
            Entity entity{ static_cast<entt::entity>(*entityOpt), scene };

            auto const& registry = GetComponentRegistry();
            if (auto const it = registry.find(compName); it != registry.end())
            {
                // Verify the entity actually has this component before returning a proxy
                if (!it->second.Has(entity))
                    return sol::make_object(s, sol::nil);

                LuaComponentProxy proxy;
                proxy.EntityID = entityID;
                proxy.Resolve = it->second.Get;
                proxy.TypeName = it->first;
                return sol::make_object(s, std::move(proxy));
            }

            OLO_CORE_WARN("[Lua] get_component: unknown or missing component '{}' on entity {}", compName, entityID);
            return sol::make_object(s, sol::nil);
        };

        entityUtilsTable["has_component"] = [](u64 entityID, const std::string& compName) -> bool
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return false;

            auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID));
            if (!entityOpt)
                return false;
            Entity entity{ static_cast<entt::entity>(*entityOpt), scene };

            auto const& registry = GetComponentRegistry();
            if (auto const it = registry.find(compName); it != registry.end())
                return it->second.Has(entity);
            return false;
        };

        // --- Scene streaming (global table) ---
        auto sceneTable = lua.create_named_table("Scene");
        sceneTable["LoadRegion"] = [](u64 regionId)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
            {
                if (auto* streamer = scene->GetSceneStreamer())
                    streamer->LoadRegion(UUID(regionId));
            }
        };
        sceneTable["UnloadRegion"] = [](u64 regionId)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
            {
                if (auto* streamer = scene->GetSceneStreamer())
                    streamer->UnloadRegion(UUID(regionId));
            }
        };

        // --- Scene wind access (mirrors C# Scene_GetWind*/SetWind*) ---
        sceneTable["GetWindEnabled"] = []() -> bool
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            return scene && scene->GetWindSettings().Enabled;
        };
        sceneTable["SetWindEnabled"] = [](bool v)
        {
            if (Scene* scene = ScriptEngine::GetSceneContext())
                scene->GetWindSettings().Enabled = v;
        };
        sceneTable["GetWindDirection"] = []() -> glm::vec3
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            return scene ? scene->GetWindSettings().Direction : glm::vec3(1.0f, 0.0f, 0.0f);
        };
        sceneTable["SetWindDirection"] = [](glm::vec3 v)
        {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            {
                v = glm::vec3(1.0f, 0.0f, 0.0f);
            }
            if (auto const len = glm::length(v); len > 1e-6f)
                v /= len;
            else
                v = glm::vec3(1.0f, 0.0f, 0.0f);
            if (Scene* scene = ScriptEngine::GetSceneContext())
                scene->GetWindSettings().Direction = v;
        };
        sceneTable["GetWindSpeed"] = []() -> f32
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            return scene ? scene->GetWindSettings().Speed : 0.0f;
        };
        sceneTable["SetWindSpeed"] = [](f32 v)
        {
            if (!std::isfinite(v))
            {
                v = 0.0f;
            }
            v = std::max(v, 0.0f);
            if (Scene* scene = ScriptEngine::GetSceneContext())
                scene->GetWindSettings().Speed = v;
        };
        sceneTable["GetWindGustStrength"] = []() -> f32
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            return scene ? scene->GetWindSettings().GustStrength : 0.0f;
        };
        sceneTable["SetWindGustStrength"] = [](f32 v)
        {
            if (!std::isfinite(v))
            {
                v = 0.0f;
            }
            v = std::max(v, 0.0f);
            if (Scene* scene = ScriptEngine::GetSceneContext())
                scene->GetWindSettings().GustStrength = v;
        };
        sceneTable["GetWindTurbulenceIntensity"] = []() -> f32
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            return scene ? scene->GetWindSettings().TurbulenceIntensity : 0.0f;
        };
        sceneTable["SetWindTurbulenceIntensity"] = [](f32 v)
        {
            if (!std::isfinite(v))
            {
                v = 0.0f;
            }
            v = std::max(v, 0.0f);
            if (Scene* scene = ScriptEngine::GetSceneContext())
                scene->GetWindSettings().TurbulenceIntensity = v;
        };

        // --- Scene streaming toggle ---
        sceneTable["GetStreamingEnabled"] = []() -> bool
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            return scene && scene->GetStreamingSettings().Enabled;
        };
        sceneTable["SetStreamingEnabled"] = [](bool v)
        {
            if (Scene* scene = ScriptEngine::GetSceneContext())
                scene->GetStreamingSettings().Enabled = v;
        };

        // --- Scene reload ---
        sceneTable["ReloadCurrentScene"] = []()
        {
            if (Scene* scene = ScriptEngine::GetSceneContext())
                scene->SetPendingReload(true);
        };
    }
} // namespace OloEngine
