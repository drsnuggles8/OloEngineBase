#include "OloEnginePCH.h"
#include "LuaScriptEngine.h"
#include "LuaScriptGlue.h"

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#include "lstate.h"
#include "setjmp.h"

#include <unordered_map>

namespace OloEngine
{
    struct LuaScriptEngineData
    {
        sol::state* LuaState = nullptr;
        Scene* SceneContext = nullptr;

        // Per-entity script instance: UUID → table returned by dofile
        std::unordered_map<u64, sol::table> EntityScriptInstances;
    };

    static LuaScriptEngineData s_LuaData;

    namespace Scripting
    {
        [[nodiscard("Store this!")]] sol::state* GetState()
        {
            return s_LuaData.LuaState;
        }
    } // namespace Scripting

    static jmp_buf s_LuaPanicJump;

    static int Lua_AtPanicHandler(lua_State*)
    {
        ::longjmp(s_LuaPanicJump, 1);
    }

    void LuaScriptEngine::Init()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_TRACE("[LuaScriptEngine] Initializing.");

        s_LuaData.LuaState = new sol::state();
        s_LuaData.LuaState->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);

        lua_State* L = s_LuaData.LuaState->lua_state();
        L->l_G->panic = [](lua_State*)
        {
            OLO_CORE_CRITICAL("[ScriptEngine] PANIC!!! We should never reach this line!");
            return 0;
        };

        ::lua_atpanic(L, &Lua_AtPanicHandler);

        LuaScriptGlue::RegisterAllTypes();
    }

    void LuaScriptEngine::Shutdown()
    {
        OLO_CORE_TRACE("[LuaScriptEngine] Shutting down.");

        s_LuaData.EntityScriptInstances.clear();
        s_LuaData.SceneContext = nullptr;

        delete s_LuaData.LuaState;
        s_LuaData.LuaState = nullptr;
    }

    void LuaScriptEngine::LoadEntityScript(const std::string& file)
    {
        OLO_CORE_TRACE("[LuaScriptEngine] Running file {0}", file);

        sol::load_result loadResult = s_LuaData.LuaState->load_file(file);
        if (!loadResult.valid())
        {
            sol::error error = loadResult;
            OLO_CORE_ERROR("[LuaScriptEngine] Lua error: {0}", error.what());
        }
        else
        {
            sol::protected_function_result functionResult = loadResult();
            if (!functionResult.valid())
            {
                sol::error error = functionResult;
                OLO_CORE_ERROR("[LuaScriptEngine] Lua error: {0}", error.what());
            }
        }
    }

    void LuaScriptEngine::ExecuteScript(const std::string& file)
    {
        LoadEntityScript(file);
    }

    void LuaScriptEngine::LoadScript(const std::string& file)
    {
        LoadEntityScript(file);
    }

    void LuaScriptEngine::OnRuntimeStart(Scene* scene)
    {
        s_LuaData.SceneContext = scene;
        s_LuaData.EntityScriptInstances.clear();

        // Expose the scene context to the Lua state for Scene table APIs
        auto& lua = *s_LuaData.LuaState;
        lua["__scene_context"] = scene;
    }

    void LuaScriptEngine::OnRuntimeStop()
    {
        s_LuaData.EntityScriptInstances.clear();
        s_LuaData.SceneContext = nullptr;

        auto& lua = *s_LuaData.LuaState;
        lua["__scene_context"] = sol::nil;
    }

    void LuaScriptEngine::OnCreateEntity(Entity entity, const std::string& scriptFile)
    {
        if (!s_LuaData.LuaState || scriptFile.empty())
            return;

        auto& lua = *s_LuaData.LuaState;
        auto entityID = static_cast<u64>(entity.GetUUID());

        // Load the script file — it should return a table
        sol::load_result loadResult = lua.load_file(scriptFile);
        if (!loadResult.valid())
        {
            sol::error error = loadResult;
            OLO_CORE_ERROR("[LuaScriptEngine] Failed to load '{}': {}", scriptFile, error.what());
            return;
        }

        sol::protected_function_result result = loadResult();
        if (!result.valid())
        {
            sol::error error = result;
            OLO_CORE_ERROR("[LuaScriptEngine] Error executing '{}': {}", scriptFile, error.what());
            return;
        }

        // Expect the script to return a table
        if (result.get_type() != sol::type::table)
        {
            OLO_CORE_WARN("[LuaScriptEngine] Script '{}' did not return a table — lifecycle callbacks will not work", scriptFile);
            return;
        }

        sol::table scriptTable = result;
        s_LuaData.EntityScriptInstances[entityID] = scriptTable;

        // Call OnCreate if it exists
        sol::optional<sol::protected_function> onCreate = scriptTable["OnCreate"];
        if (onCreate)
        {
            sol::protected_function_result createResult = onCreate.value()(entityID);
            if (!createResult.valid())
            {
                sol::error error = createResult;
                OLO_CORE_ERROR("[LuaScriptEngine] OnCreate error for entity {}: {}", entityID, error.what());
            }
        }
    }

    void LuaScriptEngine::OnUpdateEntity(Entity entity, f32 ts)
    {
        OLO_PROFILE_FUNCTION();
        auto entityID = static_cast<u64>(entity.GetUUID());
        if (auto const it = s_LuaData.EntityScriptInstances.find(entityID); it != s_LuaData.EntityScriptInstances.end())
        {
            sol::optional<sol::protected_function> onUpdate = it->second["OnUpdate"];
            if (onUpdate)
            {
                sol::protected_function_result result = onUpdate.value()(entityID, ts);
                if (!result.valid())
                {
                    sol::error error = result;
                    OLO_CORE_ERROR("[LuaScriptEngine] OnUpdate error for entity {}: {}", entityID, error.what());
                }
            }
        }
    }

    void LuaScriptEngine::OnDestroyEntity(Entity entity)
    {
        auto entityID = static_cast<u64>(entity.GetUUID());
        if (auto const it = s_LuaData.EntityScriptInstances.find(entityID); it != s_LuaData.EntityScriptInstances.end())
        {
            sol::optional<sol::protected_function> onDestroy = it->second["OnDestroy"];
            if (onDestroy)
            {
                sol::protected_function_result result = onDestroy.value()(entityID);
                if (!result.valid())
                {
                    sol::error error = result;
                    OLO_CORE_ERROR("[LuaScriptEngine] OnDestroy error for entity {}: {}", entityID, error.what());
                }
            }
            s_LuaData.EntityScriptInstances.erase(it);
        }
    }
} // namespace OloEngine
