// =============================================================================
// AssetLuaScriptValidityTest.cpp
//
// Catches the OloEditor breakage class where a Lua script under
// `SandboxProject/Assets/Scripts/` has been edited carelessly and no
// longer parses. The editor surfaces this as a silent failure (the
// entity's `OnUpdate` simply never fires, the demo scene runs without
// its scripted behaviour) or, depending on the Lua engine's robustness,
// as a generic "Lua error" with no actionable info.
//
// What this test does
// -------------------
//   For every `.lua` file under `OloEditor/SandboxProject/Assets/Scripts/`:
//     1. Create a fresh `sol::state` (no engine bindings — pure-Lua parse).
//     2. Call `lua.load_file(path)`. This returns a load_result that
//        signals syntax errors via `.valid() == false`.
//     3. Aggregate failures so one broken script doesn't mask others.
//
// What this test does NOT do
// --------------------------
//   * Execute the scripts. `load_file` only parses; full execution
//     would need every engine binding the script references (Entity,
//     Transform, etc.) and is the job of the per-script Functional
//     tests under `OloEngine/tests/Functional/Scripting/`.
//   * Validate that the script registers the expected hooks
//     (`OnUpdate`, `OnCreate`, etc.). That's runtime behaviour.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <sol/sol.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        std::vector<fs::path> EnumerateLuaScripts(const fs::path& dir)
        {
            std::vector<fs::path> out;
            std::error_code ec;
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
                return out;
            for (auto& entry : fs::recursive_directory_iterator(dir, ec))
            {
                if (ec)
                    break;
                if (!entry.is_regular_file())
                    continue;
                if (entry.path().extension() == ".lua")
                    out.push_back(entry.path());
            }
            std::ranges::sort(out);
            return out;
        }

        struct Failure
        {
            std::string Path;
            std::string Reason;
        };
    } // namespace

    TEST(AssetLuaScriptValidity, AllSandboxLuaScriptsParseCleanly)
    {
        const fs::path scriptsDir = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                    "SandboxProject" / "Assets" / "Scripts";
        const auto scripts = EnumerateLuaScripts(scriptsDir);
        ASSERT_FALSE(scripts.empty())
            << "No .lua files found under " << scriptsDir.string()
            << " — test root broken?";

        std::vector<Failure> failures;

        for (const auto& path : scripts)
        {
            // Fresh sol state per script: a parse error in one script
            // can't pollute another, and we don't accumulate global
            // bindings between iterations.
            sol::state lua;

            // We don't open any libraries: pure-syntax parse needs none,
            // and skipping `open_libraries()` avoids loading io / os /
            // package that we don't want exercised here.

            sol::load_result loaded = lua.load_file(path.generic_string());
            if (!loaded.valid())
            {
                sol::error err = loaded;
                failures.push_back({ path.generic_string(),
                                     std::string("Lua parse error: ") + err.what() });
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " Lua script(s) failed to parse:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }

        EXPECT_GE(scripts.size(), 1u);
    }

    // -------------------------------------------------------------------------
    // EveryLuaScriptDeclaresAHook
    //
    // A Lua script bound to an entity via LuaScriptComponent has to
    // declare at least one engine-recognised hook function (`OnCreate`,
    // `OnUpdate`, or `OnDestroy`) — otherwise the script loads and parses
    // but the engine's per-tick dispatch can't find anything to call, and
    // the entity's "scripted behaviour" is silently a no-op. The bug
    // surfaces in OloEditor as "I attached this script but nothing
    // happens", with no actionable error message.
    //
    // Detection: substring scan of the script source for each hook name.
    // The engine dispatches by looking up the function by name in the
    // module table, so any source mention is good enough — false
    // positives would require the developer to deliberately type a
    // hook name inside a string literal or comment and never define
    // the actual function, which doesn't happen in practice.
    // -------------------------------------------------------------------------
    TEST(AssetLuaScriptValidity, EveryLuaScriptDeclaresAtLeastOneEngineHook)
    {
        const fs::path scriptsDir = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                    "SandboxProject" / "Assets" / "Scripts";
        const auto scripts = EnumerateLuaScripts(scriptsDir);
        ASSERT_FALSE(scripts.empty());

        std::vector<Failure> failures;

        for (const auto& path : scripts)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            const std::string source = buf.str();

            const bool hasOnCreate = source.find("OnCreate") != std::string::npos;
            const bool hasOnUpdate = source.find("OnUpdate") != std::string::npos;
            const bool hasOnDestroy = source.find("OnDestroy") != std::string::npos;

            if (!hasOnCreate && !hasOnUpdate && !hasOnDestroy)
            {
                failures.push_back({ path.generic_string(),
                                     "declares none of OnCreate / OnUpdate / OnDestroy — "
                                     "engine has no hook to call on this script's entity." });
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " Lua script(s) declare no engine hooks:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // EveryLuaScriptIsReferencedByAtLeastOneScene
    //
    // Catches the orphan-script case: a `.lua` file sits under
    // `Scripts/LuaScripts/` but no scene's `LuaScriptComponent.ScriptFile`
    // points at it anymore. Usually this means a refactor renamed the
    // script binding in scenes but forgot to delete the old .lua, or
    // someone copied a script template and never wired it up. The orphan
    // doesn't cause runtime failures, but it's code rot — and a developer
    // editing the dead script will spend time wondering why their changes
    // aren't visible in the editor.
    //
    // The forward direction (every scene-bound script exists on disk)
    // is covered by AssetContentValidity.SandboxSceneReferencedScripts­ExistOnDisk.
    // -------------------------------------------------------------------------
    TEST(AssetLuaScriptValidity, EveryLuaScriptIsReferencedByAtLeastOneScene)
    {
        namespace fs = std::filesystem;

        const fs::path assetsRoot = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                    "SandboxProject" / "Assets";
        const auto scripts = EnumerateLuaScripts(assetsRoot / "Scripts");
        ASSERT_FALSE(scripts.empty());

        // Collect every `LuaScriptComponent.ScriptFile` reference across
        // all scenes. Stored as forward-slash project-relative paths to
        // match the YAML format directly.
        std::set<std::string> referencedScripts;
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(assetsRoot / "Scenes", ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() != ".olo")
                continue;

            YAML::Node sceneNode;
            try
            {
                sceneNode = YAML::LoadFile(entry.path().generic_string());
            }
            catch (...)
            {
                continue;
            }

            const YAML::Node entities = sceneNode["Entities"];
            if (!entities || !entities.IsSequence())
                continue;
            for (sizet i = 0; i < entities.size(); ++i)
            {
                const YAML::Node ent = entities[i];
                if (!ent.IsMap())
                    continue;
                const YAML::Node lsc = ent["LuaScriptComponent"];
                if (!lsc || !lsc.IsMap())
                    continue;
                const YAML::Node sf = lsc["ScriptFile"];
                if (!sf || !sf.IsScalar())
                    continue;
                std::string scriptFile = sf.as<std::string>();
                if (scriptFile.empty())
                    continue;
                std::replace(scriptFile.begin(), scriptFile.end(), '\\', '/');
                referencedScripts.insert(std::move(scriptFile));
            }
        }

        std::vector<Failure> orphans;
        for (const auto& scriptPath : scripts)
        {
            // Build the "Assets/"-relative POSIX path the way scenes
            // store it (the format we just normalised the referenced
            // set to).
            const std::string relative =
                fs::relative(scriptPath, assetsRoot).generic_string();
            if (!referencedScripts.contains(relative))
            {
                orphans.push_back({ scriptPath.generic_string(),
                                    "no scene's LuaScriptComponent.ScriptFile references this file" });
            }
        }

        if (!orphans.empty())
        {
            std::ostringstream oss;
            oss << orphans.size() << " orphan Lua script(s) with no scene reference:\n";
            for (const auto& f : orphans)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // LuaScriptHookSignaturesMatchEngineConvention
    //
    // The existing `EveryLuaScriptDeclaresAtLeastOneEngineHook` test only
    // checks that the hook name appears anywhere in the source. This
    // tighter test checks that any declared hook ALSO uses the engine's
    // expected signature:
    //
    //     function <Module>.OnCreate(id)        — single entity-id arg
    //     function <Module>.OnUpdate(id, dt)    — entity-id + delta time
    //     function <Module>.OnDestroy(id)       — single entity-id arg
    //
    // A script that declares `function Foo.OnUpdate(dt)` (forgot the
    // `id` arg) or `function Foo.OnUpdate()` (forgot both) will load
    // fine but the engine's dispatch will pass arguments the script
    // never consumes — and the script's own attempt to look up the
    // entity via the missing arg will yield nil, surfacing as the
    // entire scripted behaviour silently breaking.
    // -------------------------------------------------------------------------
    TEST(AssetLuaScriptValidity, AllHookDeclarationsMatchEngineSignature)
    {
        namespace fs = std::filesystem;

        const fs::path scriptsDir = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                    "SandboxProject" / "Assets" / "Scripts";
        const auto scripts = EnumerateLuaScripts(scriptsDir);
        ASSERT_FALSE(scripts.empty());

        struct HookSpec
        {
            const char* Name;
            const char* ExpectedArgs;
        };
        // For each hook, the regex MUST match what's after the hook
        // name's opening paren. `id` for OnCreate/OnDestroy, `id` plus
        // any comma-separated second arg for OnUpdate.
        const std::array<HookSpec, 3> hooks = { {
            { "OnCreate", R"(\(\s*[a-zA-Z_]\w*\s*\))" },                    // (id)
            { "OnUpdate", R"(\(\s*[a-zA-Z_]\w*\s*,\s*[a-zA-Z_]\w*\s*\))" }, // (id, dt)
            { "OnDestroy", R"(\(\s*[a-zA-Z_]\w*\s*\))" },                   // (id)
        } };

        std::vector<Failure> wrongSig;

        for (const auto& path : scripts)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            const std::string src = buf.str();

            for (const auto& hook : hooks)
            {
                // Find every `function <Mod>.<HookName>(...)` declaration.
                const std::regex declarationPat{
                    std::string{ R"(function\s+\w+\.)" } + hook.Name + R"(\s*\([^)]*\))",
                };
                // Match the *correct* signature anywhere — module name
                // can be any identifier, so we anchor on the hook name
                // and the args list.
                const std::regex okPat{
                    std::string{ R"(function\s+\w+\.)" } + hook.Name + R"(\s*)" + hook.ExpectedArgs,
                };

                auto begin = std::sregex_iterator(src.begin(), src.end(), declarationPat);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it)
                {
                    const std::string match = it->str();
                    if (!std::regex_match(match, okPat))
                    {
                        wrongSig.push_back({
                            path.generic_string(),
                            std::string{ hook.Name } + " declared as `" + match +
                                "` — expected `function <Mod>." + hook.Name +
                                "(id" + (std::string{ hook.Name } == "OnUpdate" ? ", dt" : "") +
                                ")`. Engine dispatch passes those args; missing them silently breaks the script.",
                        });
                    }
                }
            }
        }

        if (!wrongSig.empty())
        {
            std::ostringstream oss;
            oss << wrongSig.size() << " Lua hook declaration(s) with wrong signature:\n";
            for (const auto& f : wrongSig)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
