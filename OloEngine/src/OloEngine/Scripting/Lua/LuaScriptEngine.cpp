#include "OloEnginePCH.h"
#include "LuaScriptEngine.h"
#include "LuaScriptGlue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#include "lstate.h"
#include "setjmp.h"

namespace OloEngine {

	struct LuaScriptEngineData
	{
		sol::state* LuaState = nullptr;
	};

	static LuaScriptEngineData s_LuaData;

	namespace Scripting {
		[[nodiscard("This returns s_LuaData.LuaState, you probably wanted another function!")]] sol::state* GetState() { return s_LuaData.LuaState;}
	}

	static jmp_buf s_LuaPanicJump;

	static int Lua_AtPanicHandler(lua_State*)
	{
		::longjmp(s_LuaPanicJump, 1);
	}

	static void OnInternalLuaError()
	{
		OLO_CORE_TRACE("[LuaScriptEngine] Internal Lua error!");
	}

	void LuaScriptEngine::Init()
	{
		OLO_CORE_TRACE("[LuaScriptEngine] Initializing.");

		s_LuaData.LuaState = new sol::state();
		s_LuaData.LuaState->open_libraries(sol::lib::base, sol::lib::math);

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

		delete s_LuaData.LuaState;
		s_LuaData.LuaState = nullptr;
	}

#define LUA_CALL(Namespace, Function, ...) \
	if (setjmp(s_LuaPanicJump) == 0)\
	{\
		lua[Namespace][Function](__VA_ARGS__);\
	}\
	else\
	{\
		OnInternalLuaError();\
	}

	void LuaScriptEngine::LoadEntityScript(const std::string& file)
	{
		OLO_CORE_TRACE("[LuaScriptEngine] Running file {0}", file);

		sol::load_result loadResult = s_LuaData.LuaState->load_file(file);
		if (!loadResult.valid())
		{
			sol::error error = loadResult;
			OLO_CORE_ERROR("[LuaScriptEngine] Lua error!{0}", error.what());
		}
		else
		{
			sol::protected_function_result functionResult = loadResult();
			if (!functionResult.valid())
			{
				sol::error error = functionResult;
				OLO_CORE_ERROR("[LuaScriptEngine] Lua error!{0}", error.what());
			}
		}
	}

	void LuaScriptEngine::OnCreate(const Entity*)
	{
		//auto const& lua = *s_LuaData.LuaState;
		//LUA_CALL(entity->GetName(), "OnCreate");
	}

	void LuaScriptEngine::OnDestroyed(const Entity*)
	{
		//auto const& lua = *s_LuaData.LuaState;
		//LUA_CALL(entity->GetName(), "OnDestroy");
	}

	void LuaScriptEngine::OnUpdate(const Entity*, float)
	{
		//auto const& lua = *s_LuaData.LuaState;
		//LUA_CALL(entity->GetName(), "OnUpdate", ts);
	}
}
