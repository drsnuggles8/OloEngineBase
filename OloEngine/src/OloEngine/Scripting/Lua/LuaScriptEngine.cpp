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
		sol::state* GetState() { return s_LuaData.LuaState;}
	}

	static jmp_buf s_LuaPanicJump;

	static int Lua_AtPanicHandler(lua_State* lua)
	{
		longjmp(s_LuaPanicJump, 1);
		return 0; // Will never return
	}
	
	static void OnInternalLuaError()
	{
		std::cout << "[LuaScriptEngine] Internal Lua error!\n";
	}
	
	void LuaScriptEngine::Init()
	{
		std::cout << "[LuaScriptEngine] Initializing.\n";

		s_LuaData.LuaState = new sol::state();
		s_LuaData.LuaState->open_libraries(sol::lib::base, sol::lib::math);

		lua_State* L = s_LuaData.LuaState->lua_state();
		L->l_G->panic = [](lua_State* L)
		{
			std::cout << "[ScriptEngine] PANIC!!! We should never reach this line!\n";
			return 0;
		};

		lua_atpanic(L, &Lua_AtPanicHandler);

		LuaScriptGlue::RegisterAllTypes();
	}
	
	void LuaScriptEngine::Shutdown()
	{
		std::cout << "[LuaScriptEngine] Shutting down.\n";

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
		std::cout << "[ScriptEngine] Running " << file << "...\n";

		sol::load_result loadResult = s_LuaData.LuaState->load_file(file);
		if (!loadResult.valid())
		{
			sol::error error = loadResult;
			std::cout << "[ScriptEngine] Lua error!\n";
			std::cout << "  " << error.what() << std::endl;
		}
		else
		{
			sol::protected_function_result functionResult = loadResult();
			if (!functionResult.valid())
			{
				sol::error error = functionResult;
				std::cout << "[ScriptEngine] Lua error!\n";
				std::cout << "  " << error.what() << std::endl;
			}
		}
	}

	void LuaScriptEngine::OnCreate(const Entity* entity)
	{
		auto& lua = *s_LuaData.LuaState;
		//LUA_CALL(entity->GetName(), "OnCreate");
	}

	void LuaScriptEngine::OnDestroyed(const Entity* entity)
	{
		auto& lua = *s_LuaData.LuaState;
		//LUA_CALL(entity->GetName(), "OnDestroy");
	}

	void LuaScriptEngine::OnUpdate(const Entity* entity, float ts)
	{
		auto& lua = *s_LuaData.LuaState;
		//LUA_CALL(entity->GetName(), "OnUpdate", ts);
	}
}
