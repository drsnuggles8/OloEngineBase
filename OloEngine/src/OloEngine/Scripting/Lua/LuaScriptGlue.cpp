#include "LuaScriptGlue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace OloEngine {

	namespace Scripting {
		extern sol::state* GetState();
	}

	void LuaScriptGlue::RegisterAllTypes()
	{
	}
}
