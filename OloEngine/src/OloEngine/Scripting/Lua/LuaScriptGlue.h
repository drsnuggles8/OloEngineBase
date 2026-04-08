#pragma once

namespace sol
{
    class state;
}

namespace OloEngine
{
    class LuaScriptGlue
    {
      public:
        static void RegisterAllTypes();
        static void RegisterAllTypes(sol::state& lua);
    };
} // namespace OloEngine
