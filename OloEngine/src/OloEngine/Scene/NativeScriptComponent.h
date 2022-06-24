#pragma once

#include "NativeScript.h"
#include <utility>

namespace OloEngine {

	struct NativeScriptComponent
	{
		NativeScript* Instance = nullptr;

		NativeScript* (*InstantiateScript)();
		void (*DestroyScript)(NativeScriptComponent*);

		template<typename T>
		void Bind()
		{
			InstantiateScript = []() { return static_cast<NativeScript*>(new T()); };
			DestroyScript = [](NativeScriptComponent* nsc) { delete nsc->Instance; nsc->Instance = nullptr; };
		}
	};

}
