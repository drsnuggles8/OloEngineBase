#pragma once

namespace sol
{
    class state;
}

namespace OloEngine
{
    // Registers every Lua-visible type and global table.
    //
    // The registrations are SPLIT ACROSS LuaScriptGlue_*.cpp (issue #822): as one
    // TU this was a 5,112-line function that cost ~12.4 GB of compiler memory and
    // ~150 s in a plain Debug build, which OOM-killed CI runners. The split
    // divides Sol2's template instantiation across compiler processes and changes
    // nothing about the Lua surface. LuaScriptGlueInternal.h has the full story.
    //
    // The Register* members below are called by RegisterAllTypes IN THE ORDER
    // DECLARED, and that order is load-bearing — see its definition.
    class LuaScriptGlue
    {
      public:
        static void RegisterAllTypes();
        static void RegisterAllTypes(sol::state& lua);

      private:
        static void RegisterCoreTypes(sol::state& lua);
        static void RegisterEnvironmentTypes(sol::state& lua);
        static void RegisterCharacterTypes(sol::state& lua);
        static void RegisterSceneGraphTypes(sol::state& lua);
        static void RegisterEffectsTypes(sol::state& lua);
        static void RegisterPlatformTypes(sol::state& lua);
        static void RegisterWorldTypes(sol::state& lua);
        static void RegisterGameplayTypes(sol::state& lua);
        static void RegisterEngineApiTypes(sol::state& lua);
    };
} // namespace OloEngine
