#pragma once

#include "NativeScript.h"
#include <utility>

namespace OloEngine {

	struct NativeScriptComponent
	{
		std::function<Scope<NativeScript>(Entity entity)> InstantiateScript;
		Scope<NativeScript> Instance;

		template<typename T, typename... Args>
		void Bind(Args... args)
		{
			InstantiateScript = [args...](Entity entity)->Scope<NativeScript> { return CreateScope<T>(entity, args...); };
		}
	};

}
