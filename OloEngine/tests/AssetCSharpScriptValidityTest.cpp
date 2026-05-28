// =============================================================================
// AssetCSharpScriptValidityTest.cpp
//
// Catches the OloEditor breakage class where a `.cs` file under
// `SandboxProject/Assets/Scripts/Source/` has been renamed (or moved,
// or split into a different class) without updating its `class` line.
// Scenes reference scripts by `ScriptComponent.ClassName` (e.g.
// `Sandbox.Player` → `Source/Player.cs`); if the .cs file's class name
// no longer matches the file name, the editor compiles
// Sandbox-Scripting.dll fine but every scene binding to that script
// silently no-ops — no error, just dropped behaviour.
//
// What this test does
// -------------------
//   For every `.cs` file under `Assets/Scripts/Source/`:
//     1. Read the source as text.
//     2. Search for a `class <FileName>` declaration (with `<FileName>`
//        being the file stem — e.g. `Player.cs` must contain `class Player`).
//   Pure text scan — no Roslyn / no compile step. Robust enough for the
//   engine's script style; false positives would require deliberately
//   typing the file name in a comment without ever declaring the actual
//   class, which doesn't happen in practice.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
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

        std::vector<fs::path> EnumerateCSharpFiles(const fs::path& dir)
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
                if (entry.path().extension() == ".cs")
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

    TEST(AssetCSharpScriptValidity, EveryCSharpFileDeclaresClassMatchingFilename)
    {
        const fs::path sourceDir = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                   "SandboxProject" / "Assets" / "Scripts" / "Source";
        const auto files = EnumerateCSharpFiles(sourceDir);
        ASSERT_FALSE(files.empty())
            << "No .cs files found under " << sourceDir.string();

        std::vector<Failure> failures;

        for (const auto& path : files)
        {
            const std::string expectedClass = path.stem().generic_string();

            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            const std::string source = buf.str();

            // Match `class <ExpectedName>` where the token boundary
            // ensures we don't pick up `class FooBar` when expecting
            // `Foo`. Anchored to whitespace before so `myclass Foo`
            // doesn't false-match.
            const std::regex pattern{
                R"((^|\W)class\s+)" + expectedClass + R"((\W|$))",
            };
            if (!std::regex_search(source, pattern))
            {
                failures.push_back({ path.generic_string(),
                                     "no `class " + expectedClass + "` declaration found — "
                                                                    "file name does not match the contained class. "
                                                                    "Scenes binding via ScriptComponent.ClassName will silently no-op." });
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " C# file(s) declare no class matching their filename:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // EveryEntityDerivedCSharpScriptIsReferencedByAtLeastOneScene
    //
    // Mirror of `AssetLuaScriptValidity.EveryLuaScriptIsReferencedBy­
    // AtLeastOneScene`. An Entity-derived C# class compiled into
    // Sandbox-Scripting.dll only does anything if some scene's
    // `ScriptComponent.ClassName` resolves to it; orphan Entity-derived
    // classes are dead code. Non-Entity helper classes (e.g. static
    // utilities, plain structs) are deliberately excluded — they're
    // legitimately consumed by other C# code, not by the engine's
    // script-binding system.
    // -------------------------------------------------------------------------
    TEST(AssetCSharpScriptValidity, EveryEntityDerivedScriptIsReferencedByAtLeastOneScene)
    {
        const fs::path assetsRoot = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                    "SandboxProject" / "Assets";
        const fs::path sourceDir = assetsRoot / "Scripts" / "Source";
        const auto files = EnumerateCSharpFiles(sourceDir);
        ASSERT_FALSE(files.empty());

        // Detect Entity-derived classes (those bindable via
        // ScriptComponent). A non-Entity helper is excluded from the
        // orphan check.
        auto isEntityDerived = [](const std::string& source) -> bool
        {
            // Match `class <Anything> : Entity` (with optional access
            // modifier, possibly across `: Entity, ISomething`).
            static const std::regex pat{
                R"(class\s+\w+\s*:\s*Entity\b)",
            };
            return std::regex_search(source, pat);
        };

        // Collect every scene-referenced class name (unqualified — we
        // strip the namespace prefix the same way the production
        // ScriptComponent.ClassName format declares it).
        std::set<std::string> referencedClassNames;
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
                const YAML::Node sc = ent["ScriptComponent"];
                if (!sc || !sc.IsMap())
                    continue;
                const YAML::Node cn = sc["ClassName"];
                if (!cn || !cn.IsScalar())
                    continue;
                std::string fullName = cn.as<std::string>();
                if (fullName.empty())
                    continue;
                const auto dot = fullName.rfind('.');
                std::string className = (dot == std::string::npos)
                                            ? fullName
                                            : fullName.substr(dot + 1);
                referencedClassNames.insert(std::move(className));
            }
        }

        std::vector<Failure> orphans;
        for (const auto& path : files)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            if (const std::string source = buf.str(); !isEntityDerived(source))
                continue; // non-Entity helpers are not subject to the orphan check

            const std::string className = path.stem().generic_string();
            if (!referencedClassNames.contains(className))
            {
                orphans.push_back({
                    path.generic_string(),
                    "Entity-derived class '" + className +
                        "' is not referenced by any scene's ScriptComponent.ClassName — "
                        "the compiled class is dead code.",
                });
            }
        }

        if (!orphans.empty())
        {
            std::ostringstream oss;
            oss << orphans.size() << " orphan Entity-derived C# script(s):\n";
            for (const auto& f : orphans)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // EveryCSharpFileIsListedInBuild
    //
    // Catches the case where a `.cs` file sits in `Source/` but the
    // CMakeLists.txt `SOURCES` list never references it. Result: the
    // class isn't compiled into Sandbox-Scripting.dll, and any scene's
    // `ScriptComponent.ClassName = Sandbox.<MissingClass>` silently
    // no-ops — same silent-failure class as the scene-reference check,
    // but caused by an oversight in CMakeLists.txt rather than the .cs
    // file itself going missing.
    //
    // Concretely: this would have caught `SaveLoadTestPlayer.cs` missing
    // from the build before the discovery via the orphan check —
    // SaveLoadTest.olo binds to it but the DLL didn't contain the class.
    // -------------------------------------------------------------------------
    TEST(AssetCSharpScriptValidity, EveryCSharpFileIsListedInCMakeSources)
    {
        const fs::path sourceDir = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                   "SandboxProject" / "Assets" / "Scripts" / "Source";
        const fs::path cmakeFile = sourceDir / "CMakeLists.txt";
        ASSERT_TRUE(fs::exists(cmakeFile))
            << "Missing CMakeLists.txt at " << cmakeFile.string();

        std::ifstream cmakeIn(cmakeFile, std::ios::binary);
        std::ostringstream cmakeBuf;
        cmakeBuf << cmakeIn.rdbuf();
        const std::string cmakeContent = cmakeBuf.str();

        const auto files = EnumerateCSharpFiles(sourceDir);
        ASSERT_FALSE(files.empty());

        std::vector<Failure> missing;
        for (const auto& path : files)
        {
            // The SOURCES list quotes each entry as `"<FileName>.cs"`.
            // A simple substring match for the quoted filename is robust
            // here — CMakeLists.txt edits don't reuse this token outside
            // the SOURCES set.
            const std::string token = '"' + path.filename().generic_string() + '"';
            if (cmakeContent.find(token) == std::string::npos)
            {
                missing.push_back({ path.generic_string(),
                                    "no `\"" + path.filename().generic_string() +
                                        "\"` entry found in CMakeLists.txt — Sandbox-Scripting.dll "
                                        "will not contain the class declared in this file." });
            }
        }

        if (!missing.empty())
        {
            std::ostringstream oss;
            oss << missing.size() << " C# file(s) missing from CMakeLists.txt SOURCES:\n";
            for (const auto& f : missing)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // EveryNonEntityHelperClassIsReferenced
    //
    // Companion to `EveryEntityDerivedScriptIsReferencedByAtLeastOneScene`:
    // for *non*-Entity classes (utility helpers like `DamageFlashHelper`),
    // the orphan-detection target is OTHER .cs files. An unused helper
    // is dead code that bloats the Sandbox-Scripting.dll and confuses
    // developers reading `Source/` looking for live code.
    //
    // We check by class-name substring search across every other .cs
    // file. Production code references helpers either via `new
    // HelperName(...)` instantiation or `HelperName.StaticMethod()`
    // call — both contain the class-name token.
    // -------------------------------------------------------------------------
    TEST(AssetCSharpScriptValidity, EveryNonEntityHelperClassIsReferencedByAnotherSource)
    {
        const fs::path sourceDir = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                   "SandboxProject" / "Assets" / "Scripts" / "Source";
        const auto files = EnumerateCSharpFiles(sourceDir);
        ASSERT_FALSE(files.empty());

        // Read every .cs file once.
        std::vector<std::pair<fs::path, std::string>> sources;
        sources.reserve(files.size());
        for (const auto& path : files)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            sources.emplace_back(path, buf.str());
        }

        const std::regex entityClassPat{ R"(class\s+\w+\s*:\s*Entity\b)" };

        std::vector<Failure> orphans;
        for (const auto& [path, source] : sources)
        {
            if (std::regex_search(source, entityClassPat))
                continue; // Entity-derived: covered by the scene-reference orphan check.

            const std::string className = path.stem().generic_string();
            const std::regex usagePat{ R"(\b)" + className + R"(\b)" };

            bool referenced = false;
            for (const auto& [otherPath, otherSource] : sources)
            {
                if (otherPath == path)
                    continue;
                if (std::regex_search(otherSource, usagePat))
                {
                    referenced = true;
                    break;
                }
            }
            if (!referenced)
            {
                orphans.push_back({
                    path.generic_string(),
                    "non-Entity helper class '" + className +
                        "' is not referenced by any other .cs file — dead code.",
                });
            }
        }

        if (!orphans.empty())
        {
            std::ostringstream oss;
            oss << orphans.size() << " orphan helper class(es) with no .cs callers:\n";
            for (const auto& f : orphans)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
