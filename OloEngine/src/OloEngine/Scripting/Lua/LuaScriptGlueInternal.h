#pragma once

// =============================================================================
// LuaScriptGlueInternal.h — the shared preamble of the Lua binding glue.
//
// PRIVATE to OloEngine/Scripting/Lua/. Not an engine API: it exists only so the
// LuaScriptGlue_*.cpp parts can share one spelling of the helpers and the
// component-proxy types that LuaScriptGlue.cpp used to keep to itself.
//
// WHY THE GLUE IS SPLIT AT ALL (issue #822). It used to be one 5,746-line file
// whose RegisterAllTypes was a single 5,112-line function registering 151 Sol2
// usertypes. That one TU cost ~12.4 GB of compiler memory and ~150 s wall clock
// in a plain Debug build with NO sanitizer -- more than 3x the next-heaviest TU
// in the tree, and enough to OOM-kill the CI runners when two Linux sanitizer
// jobs built concurrently on a 31 GiB box. Sol2 instantiates a deep template
// tree per registered member and all of it landed in one function in one
// process; the existing OLO_HEAVY_COMPILE_JOBS pool and the whole-program
// optimization exclusion were mitigations for that, not fixes.
//
// The registrations now live in LuaScriptGlue_*.cpp, so the instantiation work
// divides across compiler processes and each one's peak stays bounded. Nothing
// about the Lua-visible surface changes: RegisterAllTypes calls the parts in the
// SAME ORDER the single function used, which is load-bearing -- the GLM vector
// types must be registered before the components that expose them, and
// SceneCamera before CameraComponent.
//
// ADDING A BINDING: put it in the part that owns that subsystem and keep the
// call order. A whole new part means a new .cpp, a declaration in
// LuaScriptGlue.h, a call in RegisterAllTypes, and an entry in the three source
// lists in OloEngine/src/CMakeLists.txt.
// =============================================================================

#include "LuaScriptGlue.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptSystem.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Animation/MorphTargets/FacialExpressionLibrary.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Prediction/NetworkMovementInput.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Localization/TextFormatter.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Video/VideoSystem.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "Platform/Steam/SteamManager.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Combat/CombatComponents.h"
#include "OloEngine/Gameplay/Inventory/InventorySystem.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageCalculation.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageEvent.h"
#include "OloEngine/Gameplay/Progression/ExperienceCurve.h"
#include "OloEngine/Gameplay/Progression/ProgressionComponents.h"
#include "OloEngine/Gameplay/Progression/ProgressionSystem.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Audio/AudioEvents/AudioPlayback.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSound.h"
#include "OloEngine/Audio/AudioEvents/CommandID.h"
#include "OloEngine/Scene/Streaming/SceneStreamer.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OloEngine
{
    [[nodiscard]] inline bool IsFiniteVec2(const glm::vec2& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y);
    }

    [[nodiscard]] inline bool IsFiniteVec3(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    [[nodiscard]] inline bool IsFiniteVec4(const glm::vec4& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
    }

    // ── RPC marshalling helpers ──
    //
    // The wire types are deliberately few (see ERpcArgType); these map the Lua
    // value model onto them. Anything else is refused at the call site rather than
    // coerced, because a silently-coerced argument would mean two different things
    // on the two ends of the connection.
    [[nodiscard]] inline ERpcTarget ParseRpcTarget(std::string_view name)
    {
        if (name == "client")
        {
            return ERpcTarget::Client;
        }
        if (name == "multicast")
        {
            return ERpcTarget::Multicast;
        }
        if (name != "server")
        {
            OLO_CORE_WARN_TAG("Networking", "Unknown RPC target '{}'; defaulting to 'server'", name);
        }
        return ERpcTarget::Server;
    }

    [[nodiscard]] inline ERpcReliability ParseRpcReliability(std::string_view name)
    {
        if (name == "unreliable")
        {
            return ERpcReliability::Unreliable;
        }
        if (name != "reliable")
        {
            OLO_CORE_WARN_TAG("Networking", "Unknown RPC reliability '{}'; defaulting to 'reliable'", name);
        }
        return ERpcReliability::Reliable;
    }

    [[nodiscard]] inline std::optional<RpcArg> LuaToRpcArg(const sol::object& value)
    {
        switch (value.get_type())
        {
            case sol::type::boolean:
                return RpcArg::MakeBool(value.as<bool>());
            case sol::type::string:
                return RpcArg::MakeString(value.as<std::string>());
            case sol::type::number:
            {
                // Lua 5.4 keeps integers and floats as distinct number subtypes, and
                // under SOL_ALL_SAFETIES_ON sol2's numeric check honours that — so an
                // integral Lua value round-trips as an integer instead of silently
                // becoming a double on the far end.
                //
                // The value check is deliberately NOT left to that macro alone: it is
                // set per-TU here, and a build that lost the define would let a
                // fractional number pass is<i64>() and then truncate in as<i64>().
                // Confirm the value really is integral before taking the integer path.
                const f64 asDouble = value.as<f64>();
                const bool integral = std::isfinite(asDouble) && std::trunc(asDouble) >= static_cast<f64>(INT64_MIN) &&
                                      std::trunc(asDouble) <= static_cast<f64>(INT64_MAX) &&
                                      Math::BitwiseEqual(std::trunc(asDouble), asDouble);
                if (value.is<i64>() && integral)
                {
                    return RpcArg::MakeInt(value.as<i64>());
                }
                // Sanitize exactly as RpcDispatcher::ReadArg does for wire data. A
                // locally-executed RPC (the server running its own Multicast) never
                // crosses the wire, so without this the same call would deliver NaN
                // locally and 0 remotely.
                if (!std::isfinite(asDouble))
                {
                    OLO_CORE_WARN_TAG("Networking", "RPC number argument is not finite; sending 0");
                    return RpcArg::MakeFloat(0.0);
                }
                return RpcArg::MakeFloat(asDouble);
            }
            case sol::type::userdata:
                if (value.is<glm::vec3>())
                {
                    const glm::vec3 v = value.as<glm::vec3>();
                    if (!IsFiniteVec3(v))
                    {
                        OLO_CORE_WARN_TAG("Networking", "RPC vec3 argument is not finite; sending (0,0,0)");
                        return RpcArg::MakeVec3(glm::vec3(0.0f));
                    }
                    return RpcArg::MakeVec3(v);
                }
                return std::nullopt;
            default:
                return std::nullopt;
        }
    }

    [[nodiscard]] inline sol::object RpcArgToLua(sol::state_view& view, const RpcArg& arg)
    {
        switch (arg.Type)
        {
            case ERpcArgType::Bool:
                return sol::make_object(view, arg.AsBool);
            case ERpcArgType::Int:
                return sol::make_object(view, arg.AsInt);
            case ERpcArgType::Float:
                return sol::make_object(view, arg.AsFloat);
            case ERpcArgType::String:
                return sol::make_object(view, arg.AsString);
            case ERpcArgType::Vec3:
                return sol::make_object(view, arg.AsVec3);
            case ERpcArgType::Entity:
                return sol::make_object(view, arg.AsEntity);
        }
        return sol::lua_nil;
    }

    // ── WeatherStateId <-> name mapping for the Lua "targetState" /
    // "currentState" string properties (case-sensitive; the names mirror the
    // WeatherStateId enumerators exactly) ──
    [[nodiscard]] inline std::string_view WeatherStateIdToName(WeatherStateId state)
    {
        switch (state)
        {
            case WeatherStateId::Clear:
                return "Clear";
            case WeatherStateId::Overcast:
                return "Overcast";
            case WeatherStateId::Rain:
                return "Rain";
            case WeatherStateId::Storm:
                return "Storm";
            case WeatherStateId::Snow:
                return "Snow";
            case WeatherStateId::FogBank:
                return "FogBank";
            default:
                return "Clear";
        }
    }

    [[nodiscard]] inline std::optional<WeatherStateId> WeatherStateIdFromName(std::string_view name)
    {
        if (name == "Clear")
            return WeatherStateId::Clear;
        if (name == "Overcast")
            return WeatherStateId::Overcast;
        if (name == "Rain")
            return WeatherStateId::Rain;
        if (name == "Storm")
            return WeatherStateId::Storm;
        if (name == "Snow")
            return WeatherStateId::Snow;
        if (name == "FogBank")
            return WeatherStateId::FogBank;
        return std::nullopt;
    }

    // Recover (active scene, owning entity) for a script-bound gameplay
    // component so quest/inventory mutations driven from Lua publish
    // entity-stamped gameplay events. Returns a null Entity when there's no
    // active scene context; callers then fall back to the raw, event-less
    // data-structure path so behaviour is unchanged outside of runtime.
    template<typename T>
    [[nodiscard]] inline std::pair<Scene*, Entity> LuaOwnerContext(T& component)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        Entity entity = scene ? scene->GetEntityForComponent(component) : Entity{};
        return { scene, entity };
    }

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
        std::string_view TypeName = {};          // component type name for Box2D sync hooks

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

    // Defined in LuaScriptGlue.cpp. Declared rather than defined here on
    // purpose: its body instantiates MakeEntry<T> for every registered component
    // type, and putting that in a header would make every part TU pay the
    // instantiation this split exists to divide up.
    [[nodiscard]] const std::unordered_map<std::string_view, ComponentEntry>& GetComponentRegistry();
} // namespace OloEngine
