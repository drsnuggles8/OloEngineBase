#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue_EngineApi.cpp — the engine-level global tables: shader libraries, Application, entity lookup, the Scene API and localization.
//
// One of the LuaScriptGlue_*.cpp parts. See LuaScriptGlueInternal.h for why the
// glue is split and why the call order in RegisterAllTypes is load-bearing.
// =============================================================================

namespace OloEngine
{
    void LuaScriptGlue::RegisterEngineApiTypes(sol::state& lua)
    {
        // Created by RegisterGameplayTypes, which runs first. Re-fetched from
        // the global table rather than passed along: the parts share Lua state,
        // not C++ locals, and this keeps their signatures uniform.
        sol::table entityUtilsTable = lua["entity_utils"];

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
        shaderLib3D["GetAllNames"] = [](sol::this_state s) -> sol::table
        {
            sol::state_view luaState(s);
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
        shaderLib2D["GetAllNames"] = [](sol::this_state s) -> sol::table
        {
            sol::state_view luaState(s);
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
        entityUtilsTable["find_by_name"] = [](const std::string& name, sol::this_state s) -> sol::object
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return sol::make_object(s, sol::nil);
            Entity entity = scene->FindEntityByName(name);
            if (!entity)
                return sol::make_object(s, sol::nil);
            return sol::make_object(s, static_cast<u64>(entity.GetUUID()));
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
            if (!IsFiniteVec3(translation))
                return;
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
            if (!IsFiniteVec3(rotation))
                return;
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
            if (!IsFiniteVec3(scale))
                return;
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return;
            if (auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityID)))
            {
                Entity entity{ static_cast<entt::entity>(*entityOpt), scene };
                // Box2D does not support runtime scale changes
                if (entity.HasComponent<Rigidbody2DComponent>())
                {
                    auto const& rb = entity.GetComponent<Rigidbody2DComponent>();
                    if (b2Body_IsValid(rb.RuntimeBody))
                    {
                        OLO_CORE_WARN("[Lua] Cannot change scale while physics body is active (entity {})", entityID);
                        return;
                    }
                }
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
            const Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
            {
                if (auto* streamer = scene->GetSceneStreamer())
                    streamer->LoadRegion(UUID(regionId));
            }
        };
        sceneTable["UnloadRegion"] = [](u64 regionId)
        {
            const Scene* scene = ScriptEngine::GetSceneContext();
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

        // --- Scene reload / switch ---
        sceneTable["ReloadCurrentScene"] = []()
        {
            if (Scene* scene = ScriptEngine::GetSceneContext())
                scene->SetPendingReload(true);
        };

        // Switch to another scene (issue #642) — the main-menu -> level ->
        // next-level primitive, at parity with C#'s SceneManager.LoadScene.
        //
        // DEFERRED, exactly like ReloadCurrentScene: the request is recorded
        // and serviced by the host after this tick returns, because the scene
        // torn down by the swap is the one this script is running in. The rest
        // of your OnUpdate still runs on the old scene, and the last request
        // made during a tick is the one that happens. Hard cut — no fade.
        //
        // The path may be a bare name ("Level2"), a file name ("Level2.olo")
        // or a path relative to the game's scene directory.
        sceneTable["LoadScene"] = [](const std::string& path)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
            {
                OLO_CORE_WARN("[Lua] Scene.LoadScene called with no active scene context.");
                return;
            }
            if (path.empty())
            {
                OLO_CORE_WARN("[Lua] Scene.LoadScene called with an empty path — ignoring.");
                return;
            }
            scene->SetPendingSceneLoad(path);
        };
        sceneTable["LoadSceneFromSave"] = [](const std::string& path, const std::string& saveSlot)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
            {
                OLO_CORE_WARN("[Lua] Scene.LoadSceneFromSave called with no active scene context.");
                return;
            }
            if (path.empty() || saveSlot.empty())
            {
                OLO_CORE_WARN("[Lua] Scene.LoadSceneFromSave needs a scene path and save slot — ignoring.");
                return;
            }
            scene->SetPendingSceneLoadFromSave(path, saveSlot);
        };

        // --- Runtime spawning (issue #643; mirrors the C# Scene.* surface) ---
        //
        // All three are DEFERRED. Each returns immediately with the new
        // entity's final UUID, but the entity itself materialises when the
        // engine drains its command queue — after every script's OnUpdate has
        // returned this tick. The engine is iterating the script component
        // pools while your OnUpdate runs; creating or destroying an entity in
        // the middle of that walk would invalidate the iteration, which is why
        // the spawn transform is an argument rather than something you assign
        // afterwards (there is no entity to assign to until the drain).
        //
        // By the time physics, transform propagation and rendering run later in
        // the same tick, the spawn is fully live — so a spawn shows up in the
        // same frame, it just isn't readable from the OnUpdate that asked for it.
        sceneTable["CreateEntity"] = [](const std::string& name, sol::optional<glm::vec3> position) -> u64
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
            {
                OLO_CORE_WARN("[Lua] Scene.CreateEntity called with no active scene context.");
                return 0;
            }
            return static_cast<u64>(scene->ScriptCreateEntity(name, position.value_or(glm::vec3(0.0f))));
        };

        sceneTable["Instantiate"] = [](u64 prefabHandle, sol::optional<glm::vec3> position,
                                       sol::optional<glm::vec3> rotationEuler, sol::optional<glm::vec3> scale) -> u64
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
            {
                OLO_CORE_WARN("[Lua] Scene.Instantiate called with no active scene context.");
                return 0;
            }
            return static_cast<u64>(scene->ScriptInstantiatePrefab(AssetHandle(prefabHandle),
                                                                   position.value_or(glm::vec3(0.0f)),
                                                                   rotationEuler.value_or(glm::vec3(0.0f)),
                                                                   scale.value_or(glm::vec3(1.0f))));
        };

        // Project-relative path → prefab AssetHandle, e.g.
        // "Prefabs/Projectile.oprefab". EDITOR-ONLY: a packed runtime serves
        // assets by handle with no path index, so it returns 0 there. Scripts
        // that must ship should carry the handle instead (a component field, or
        // PrefabComponent.prefabID off an authored template entity).
        sceneTable["FindPrefabByPath"] = [](const std::string& path) -> u64
        {
            // Warn once per distinct path: the natural way to write this
            // mistake is a per-tick poll, which would otherwise repeat the same
            // warning every frame and bury the log.
            static std::mutex s_WarnedMutex;
            static std::unordered_set<std::string> s_WarnedPaths;
            auto warnOnce = [&path](const char* reason)
            {
                {
                    std::scoped_lock lock(s_WarnedMutex);
                    if (!s_WarnedPaths.insert(path).second)
                        return;
                }
                OLO_CORE_WARN("[Lua] Scene.FindPrefabByPath('{}') {} — returning 0. "
                              "(Further misses on this path are suppressed.)",
                              path, reason);
            };

            // HasAssetManager first — GetAssetManager asserts when unset.
            Ref<EditorAssetManager> editorManager =
                Project::HasAssetManager() ? Project::GetAssetManager().As<EditorAssetManager>() : nullptr;
            if (!editorManager)
            {
                // Also de-duplicated, and for a stronger reason than a miss: in
                // a packed runtime EVERY call lands here, so a per-tick poll
                // would warn every frame for the life of the process.
                warnOnce("requires the editor asset manager — a packed runtime has no path index, so pass "
                         "the AssetHandle instead");
                return 0;
            }

            AssetHandle handle = editorManager->GetAssetHandleFromFilePath(path);
            // A path resolving to some OTHER asset type must not come back as a
            // prefab handle: Scene.Instantiate would take it and fail a tick
            // later inside the drain, where the offending path is long gone.
            if (handle && editorManager->GetAssetType(handle) != AssetType::Prefab)
            {
                warnOnce("resolves to a non-Prefab asset");
                return 0;
            }
            if (!handle)
                warnOnce("found no asset at that path");
            return static_cast<u64>(handle);
        };

        sceneTable["DestroyEntity"] = [](u64 entityID)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
            {
                OLO_CORE_WARN("[Lua] Scene.DestroyEntity called with no active scene context.");
                return;
            }
            scene->ScriptDestroyEntity(UUID(entityID));
        };

        // --- Localization ---
        // Mirrors LocalizationManager's static API. Keys are arbitrary strings;
        // `Localization.Format` takes an optional Lua table whose entries are
        // forwarded as the formatter's named-parameter map.
        auto localizationTable = lua.create_named_table("Localization");
        localizationTable["Get"] = [](const std::string& key) -> std::string
        {
            OLO_PROFILE_SCOPE("Lua::Localization::Get");
            return LocalizationManager::Get(key);
        };
        localizationTable["Format"] = [](const std::string& key, sol::optional<sol::table> params) -> std::string
        {
            OLO_PROFILE_SCOPE("Lua::Localization::Format");
            TextFormatter::ParamMap pm;
            if (params)
            {
                for (const auto& kv : *params)
                {
                    // Skip keys/values that aren't strings — Lua callers
                    // sometimes pass numbers; we coerce those via tostring.
                    std::string keyStr;
                    if (kv.first.is<std::string>())
                        keyStr = kv.first.as<std::string>();
                    else
                        continue;

                    std::string valStr;
                    if (kv.second.is<std::string>())
                        valStr = kv.second.as<std::string>();
                    else if (kv.second.is<i32>())
                        valStr = std::to_string(kv.second.as<i32>());
                    else if (kv.second.is<f64>())
                        valStr = std::to_string(kv.second.as<f64>());
                    else if (kv.second.is<bool>())
                        valStr = kv.second.as<bool>() ? "true" : "false";
                    else
                        continue;

                    pm.emplace(std::move(keyStr), std::move(valStr));
                }
            }
            return LocalizationManager::Format(key, pm);
        };
        localizationTable["FormatPlural"] = [](const std::string& key, const std::string& countParam, i32 count, sol::optional<sol::table> params) -> std::string
        {
            OLO_PROFILE_SCOPE("Lua::Localization::FormatPlural");
            TextFormatter::ParamMap pm;
            if (params)
            {
                for (const auto& kv : *params)
                {
                    if (!kv.first.is<std::string>())
                        continue;
                    std::string keyStr = kv.first.as<std::string>();
                    std::string valStr;
                    if (kv.second.is<std::string>())
                        valStr = kv.second.as<std::string>();
                    else if (kv.second.is<i32>())
                        valStr = std::to_string(kv.second.as<i32>());
                    else if (kv.second.is<f64>())
                        valStr = std::to_string(kv.second.as<f64>());
                    else if (kv.second.is<bool>())
                        valStr = kv.second.as<bool>() ? "true" : "false";
                    else
                        continue;
                    pm.emplace(std::move(keyStr), std::move(valStr));
                }
            }
            return LocalizationManager::FormatPlural(key, countParam, count, std::move(pm));
        };
        localizationTable["SetLocale"] = [](const std::string& localeCode) -> bool
        {
            OLO_PROFILE_SCOPE("Lua::Localization::SetLocale");
            return LocalizationManager::SetCurrentLocale(localeCode);
        };
        localizationTable["GetCurrentLocale"] = []() -> std::string
        {
            return LocalizationManager::GetCurrentLocale();
        };
        localizationTable["HasKey"] = [](const std::string& key) -> bool
        {
            return LocalizationManager::HasKey(key);
        };
        // ResolveLocalizedText: pass any string; if it starts with the "@key:"
        // prefix the rest is treated as a localization key. Use this when
        // displaying quest titles, item names, etc. — anywhere a single
        // string field might host either a literal or a translation key.
        localizationTable["ResolveLocalizedText"] = [](const std::string& value) -> std::string
        {
            return LocalizationManager::ResolveLocalizedText(value);
        };
        localizationTable["FormatNumber"] = [](sol::object value, sol::optional<i32> decimals, sol::optional<std::string> localeCode) -> std::string
        {
            // Two overloads via Lua's dynamic dispatch: integer-shaped values
            // route through the i64 overload (no decimal section), anything
            // else stringifies through the f64 overload with caller-supplied
            // precision (default 2).
            //
            // Read integer values via their native sol type — going through
            // f64 first would silently truncate the >2^53 range that an i64
            // is allowed to hold (rare in practice but plenty of game
            // scoring/economy code uses 64-bit counters).
            const std::string loc = localeCode.value_or(std::string{});
            if (value.is<i64>())
                return LocalizationManager::FormatNumber(value.as<i64>(), loc);
            if (value.is<i32>())
                return LocalizationManager::FormatNumber(static_cast<i64>(value.as<i32>()), loc);
            return LocalizationManager::FormatNumber(value.as<f64>(), decimals.value_or(2), loc);
        };
        localizationTable["GetMissingKeys"] = [](sol::this_state s) -> sol::table
        {
            sol::state_view luaState(s);
            const auto missing = LocalizationManager::GetMissingKeysSnapshot();
            sol::table out = luaState.create_table(static_cast<int>(missing.size()), 0);
            int idx = 1;
            for (const auto& k : missing)
                out[idx++] = k;
            return out;
        };
        localizationTable["ClearMissingKeys"] = []()
        {
            LocalizationManager::ClearMissingKeys();
        };
        localizationTable["GeneratePseudoLocale"] = [](sol::optional<std::string> source, sol::optional<std::string> pseudoCode) -> bool
        {
            return LocalizationManager::GeneratePseudoLocale(source.value_or("en"), pseudoCode.value_or("pseudo"));
        };
        localizationTable["FormatCurrency"] = [](f64 amount, sol::optional<std::string> localeCode, sol::optional<std::string> symbolOverride) -> std::string
        {
            return LocalizationManager::FormatCurrency(amount, localeCode.value_or(std::string{}), symbolOverride.value_or(std::string{}));
        };
        localizationTable["FormatList"] = [](sol::table items, sol::optional<std::string> localeCode) -> std::string
        {
            // Lua array tables iterate via ipairs (1-based). We collect into
            // a vector<string> rather than trying to share storage so each
            // entry is cleanly owned by the C++ side regardless of Lua GC.
            std::vector<std::string> v;
            v.reserve(items.size());
            for (const auto& kv : items)
            {
                if (kv.second.is<std::string>())
                    v.push_back(kv.second.as<std::string>());
            }
            return LocalizationManager::FormatList(v, localeCode.value_or(std::string{}));
        };
        // FormatDate / FormatTime / FormatRelativeTime take a Unix epoch
        // seconds value from Lua (Lua doesn't have a portable time_point
        // type). 0 means "use now()". Style is an integer matching the
        // DateStyle / TimeStyle enum order.
        localizationTable["FormatDate"] = [](i64 epochSeconds, sol::optional<i32> style, sol::optional<std::string> localeCode) -> std::string
        {
            const auto tp = (epochSeconds == 0)
                                ? std::chrono::system_clock::now()
                                : std::chrono::system_clock::from_time_t(static_cast<std::time_t>(epochSeconds));
            return LocalizationManager::FormatDate(tp,
                                                   static_cast<LocalizationManager::DateStyle>(style.value_or(static_cast<i32>(std::to_underlying(LocalizationManager::DateStyle::Medium)))),
                                                   localeCode.value_or(std::string{}));
        };
        localizationTable["FormatTime"] = [](i64 epochSeconds, sol::optional<i32> style, sol::optional<std::string> localeCode) -> std::string
        {
            const auto tp = (epochSeconds == 0)
                                ? std::chrono::system_clock::now()
                                : std::chrono::system_clock::from_time_t(static_cast<std::time_t>(epochSeconds));
            return LocalizationManager::FormatTime(tp,
                                                   static_cast<LocalizationManager::TimeStyle>(style.value_or(static_cast<i32>(std::to_underlying(LocalizationManager::TimeStyle::Short)))),
                                                   localeCode.value_or(std::string{}));
        };
        localizationTable["FormatRelativeTime"] = [](i64 epochSeconds, sol::optional<std::string> localeCode) -> std::string
        {
            const auto tp = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(epochSeconds));
            return LocalizationManager::FormatRelativeTime(tp, localeCode.value_or(std::string{}));
        };
        localizationTable["GetAvailableLocales"] = [](sol::this_state s) -> sol::table
        {
            sol::state_view luaState(s);
            const auto locales = LocalizationManager::GetAvailableLocales();
            sol::table out = luaState.create_table(static_cast<int>(locales.size()), 0);
            int idx = 1;
            for (const auto& loc : locales)
            {
                sol::table entry = luaState.create_table(0, 2);
                entry["code"] = loc.Code;
                entry["name"] = loc.Name;
                out[idx++] = entry;
            }
            return out;
        };
    }
} // namespace OloEngine
