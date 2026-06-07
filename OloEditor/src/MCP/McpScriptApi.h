#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace OloEngine::MCP
{
    // Build a digest of the scripting API surface a game developer can call.
    //
    //   language  : "csharp" (reads the OloHeaderTool-generated + hand-written C#
    //               bindings under OloEngine-ScriptCore/src/OloEngine) or "lua"
    //               (reads the Sol2 usertype registrations in LuaScriptGlue.cpp).
    //   typeFilter : case-insensitive substring. Empty → return the type index
    //               (names only, cheap). Non-empty → matching types with members.
    //
    // Pure file I/O over the source tree (this is a dev-time editor), so it is
    // lock-safe and runs on the handler thread. Returns an object carrying an
    // "error" key if the sources are not present (e.g. a shipped editor).
    nlohmann::json BuildScriptApiDigest(const std::string& language, const std::string& typeFilter);
} // namespace OloEngine::MCP
