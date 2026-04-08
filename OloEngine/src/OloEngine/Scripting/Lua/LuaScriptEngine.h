#pragma once

#include "OloEngine/Scene/Entity.h"

#include <string>

namespace OloEngine
{
    class LuaScriptEngine
    {
      public:
        static void Init();
        static void Shutdown();

        static void ExecuteScript(const std::string& file);
        static void LoadScript(const std::string& file);
        static void LoadEntityScript(const std::string& file);

        // Per-entity lifecycle
        static void OnCreateEntity(Entity entity, const std::string& scriptFile);
        static void OnUpdateEntity(Entity entity, f32 ts);
        static void OnDestroyEntity(Entity entity);

        // Scene lifecycle
        static void OnRuntimeStart(Scene* scene);
        static void OnRuntimeStop();
    };
} // namespace OloEngine
