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
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    // CSharpProjectsCompileEverySourceFile
    //
    // Catches a `.cs` file that sits in a script project's source tree but is
    // not compiled. The class is then missing from the assembly, and any
    // scene's `ScriptComponent.ClassName = Sandbox.<MissingClass>` silently
    // no-ops. This happened three times while the build used hand-kept CMake
    // SOURCES lists: `SaveLoadTestPlayer.cs` was missing from
    // Sandbox-Scripting.dll, and `Video.cs` and `Rendering/ShaderLibrary.cs`
    // were missing from OloEngine-ScriptCore.dll (#1405).
    //
    // Both assemblies are now SDK-style projects built by `dotnet build`
    // (cmake/CSharpAssembly.cmake). Each compiles exactly one recursive glob
    // over its source tree, the same tree the CMake target globs for its
    // dependencies. This pins that shape, and forbids every MSBuild way of
    // dropping files from it: a `Remove`, `DefaultItemExcludes`, turning the
    // default items back on or off wholesale, and a `Directory.Build.*` file
    // above the project, which MSBuild imports implicitly.
    // -------------------------------------------------------------------------
    TEST(AssetCSharpScriptValidity, CSharpProjectsCompileEverySourceFile)
    {
        const auto readText = [](const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream buf;
            buf << in.rdbuf();
            return buf.str();
        };
        const auto countMatches = [](const std::string& text, const std::regex& pattern)
        {
            return static_cast<sizet>(std::distance(std::sregex_iterator(text.begin(), text.end(), pattern),
                                                    std::sregex_iterator()));
        };

        const fs::path editorRoot{ OLO_TEST_EDITOR_ROOT };
        const fs::path repoRoot = editorRoot.parent_path();
        const fs::path props = repoRoot / "OloEngine-ScriptCore" / "OloEngine.CSharp.props";

        struct ProjectCase
        {
            fs::path Project;
            std::string Glob;    // the one Compile Include the project may have
            fs::path SourceTree; // the directory that glob covers
        };
        const std::array projects{
            ProjectCase{ repoRoot / "OloEngine-ScriptCore" / "OloEngine-ScriptCore.csproj", "src/**/*.cs",
                         repoRoot / "OloEngine-ScriptCore" / "src" },
            ProjectCase{ editorRoot / "SandboxProject" / "Assets" / "Scripts" / "Sandbox-Scripting.csproj", "Source/**/*.cs",
                         editorRoot / "SandboxProject" / "Assets" / "Scripts" / "Source" },
        };

        const std::regex compileElement(R"(<\s*Compile\b)");
        const std::regex removeAttribute(R"(\bRemove\s*=)");
        const std::regex dropsItems(R"(DefaultItemExcludes|EnableDefaultItems\b)");
        const std::regex defaultCompileItemsOff(R"(<\s*EnableDefaultCompileItems\s*>\s*false\s*<)");

        ASSERT_TRUE(fs::exists(props)) << "Missing " << props.string();
        const std::string propsText = readText(props);
        EXPECT_EQ(countMatches(propsText, compileElement), 0u) << "OloEngine.CSharp.props must not add compile items.";
        EXPECT_EQ(countMatches(propsText, removeAttribute), 0u) << "OloEngine.CSharp.props removes items.";
        EXPECT_EQ(countMatches(propsText, dropsItems), 0u) << "OloEngine.CSharp.props changes which items are compiled.";

        for (const ProjectCase& project : projects)
        {
            SCOPED_TRACE(project.Project.filename().string());
            ASSERT_TRUE(fs::exists(project.Project)) << "Missing " << project.Project.string();
            ASSERT_FALSE(EnumerateCSharpFiles(project.SourceTree).empty()) << project.SourceTree.string();
            const std::string text = readText(project.Project);

            EXPECT_EQ(countMatches(text, compileElement), 1u)
                << "The project must compile exactly one item, the glob " << project.Glob << "; a hand-kept list is how Video.cs went missing.";
            const std::regex expectedInclude(R"(<\s*Compile\s+Include\s*=\s*")" + std::regex_replace(project.Glob, std::regex(R"([.*])"), R"(\$&)") +
                                             R"("\s*/>)");
            EXPECT_EQ(countMatches(text, expectedInclude), 1u) << "The project must compile every .cs by <Compile Include=\"" << project.Glob << "\" />.";
            EXPECT_EQ(countMatches(text, defaultCompileItemsOff), 1u)
                << "The SDK's whole-directory default must stay off, or an in-source obj/ from an IDE build is compiled too.";
            EXPECT_EQ(countMatches(text, removeAttribute), 0u) << "The project removes items; their classes would be missing from the assembly.";
            EXPECT_EQ(countMatches(text, dropsItems), 0u) << "The project changes which items are compiled.";

            for (fs::path dir = project.Project.parent_path(); !dir.empty(); dir = dir.parent_path())
            {
                for (const char* implicitImport : { "Directory.Build.props", "Directory.Build.targets" })
                    EXPECT_FALSE(fs::exists(dir / implicitImport))
                        << (dir / implicitImport).string() << " is imported into the project implicitly and can change what it compiles.";
                if (dir == repoRoot || dir == dir.parent_path())
                    break;
            }
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
