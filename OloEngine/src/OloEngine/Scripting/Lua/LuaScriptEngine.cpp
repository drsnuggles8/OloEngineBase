#include "OloEnginePCH.h"
#include "LuaScriptEngine.h"
#include "LuaScriptGlue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace OloEngine {

	struct LuaScriptEngineData
	{
		sol::state* LuaState = nullptr;
	};
	
	static LuaScriptEngineData s_LuaData;
	
	void LuaScriptEngine::Init()
	{
		std::cout << "[LuaScriptEngine] Initializing.\n";

		s_LuaData.LuaState = new sol::state();
		s_LuaData.LuaState->open_libraries(sol::lib::base, sol::lib::math);
	}
	void LuaScriptEngine::Shutdown()
	{
		std::cout << "[LuaScriptEngine] Shutting down.\n";

		delete s_LuaData.LuaState;
		s_LuaData.LuaState = nullptr;
	}
}
