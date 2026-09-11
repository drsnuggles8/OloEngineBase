// OLO_TEST_LAYER: unit
// =============================================================================
// McpAutomationBuildTest — structured build invocation (issue #1163, Epic H
// slice 3).
//
// Two things are pinned here, and the second is the reason the slice was
// deferred rather than built with the rest of #1130:
//
//   1. THE PURE CORE. Automation/AutomationBuildInvocation.h holds the target /
//      configuration / build-directory tables, the compiler-diagnostic parser,
//      the build-lock transcript reader, and the freshness verdict. Nothing here
//      launches a process, takes the lock or touches a build tree.
//
//   2. THE CONCURRENCY CONTRACT, as far as it is expressible without a machine.
//      In particular the verdict: `Verdict()` must never call a zero exit code
//      evidence of a build. Three real mechanisms in this repo report a build
//      that did not happen as exit 0 — build-lock.ps1's stand-down, an
//      incremental build with nothing to do, and a build that never started at
//      all — and each is a distinct outcome here, never merged into "success".
//
// The refusal path (the lock held by another worktree) and the live-editor path
// are verified by hand against the real lock; they cannot be unit tested,
// because a test that took the lock would be the very second unaudited build
// this slice exists to prevent.
//
// Classification: unit (no GL, no editor, no live server, no child process).
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationBuildInvocation.h"

#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace Build = OloEngine::Automation::BuildInvocation;
    using Build::Diagnostic;
    using Build::LockOutcome;
    using Build::Severity;
    using Build::TargetOutcome;

    // ---- the three fixed vocabularies (also the injection boundary) --------

    TEST(AutomationBuildTablesTest, TheEditorsOwnTargetsAreRefusedWithAMechanism)
    {
        // The footgun the issue asks about by name. Both are REFUSED, and each
        // refusal names the mechanism rather than saying "not allowed" — a
        // reader who does not know why cannot judge whether to work around it.
        for (const char* name : { "OloEditor", "OloEngine-ScriptCore" })
        {
            const Build::TargetSpec* spec = Build::FindTarget(name);
            ASSERT_NE(spec, nullptr) << name << " must be in the table so it can be refused BY NAME; a target "
                                                "missing from the table is refused as 'unknown', which reads as a "
                                                "typo rather than as a deliberate no.";
            EXPECT_FALSE(spec->RefusedBecause.empty()) << name << " must be refused from inside a running editor.";
        }

        EXPECT_NE(std::string(Build::FindTarget("OloEditor")->RefusedBecause).find("LNK1168"), std::string::npos)
            << "The OloEditor refusal should name the linker error, because that is the observable a caller who "
               "tries it from a shell will hit.";
    }

    TEST(AutomationBuildTablesTest, TheBuildableSetIsExactlyWhatTheEditorDoesNotHoldOpen)
    {
        // A ratchet in BOTH directions. Widening it is a decision about what the
        // editor holds open while it runs, and it should fail here rather than
        // at a link step forty minutes into a build.
        const std::vector<std::string> expected{ "OloEngine", "OloEngine-Tests", "OloRuntime",
                                                 "OloServer", "oloctl", "OloEngine-LuaScriptCore" };
        std::vector<std::string> actual;
        for (const Build::TargetSpec& spec : Build::kTargets)
        {
            if (spec.RefusedBecause.empty())
                actual.emplace_back(spec.Name);
        }
        EXPECT_EQ(actual, expected);

        // Every buildable target must also carry an artefact path, or its
        // freshness verdict could never be anything but "missing-artifact".
        for (const Build::TargetSpec& spec : Build::kTargets)
            EXPECT_FALSE(spec.Artifact.empty()) << spec.Name << " has no artefact path.";
    }

    TEST(AutomationBuildTablesTest, UnknownTargetsConfigsAndTreesAreNotResolvable)
    {
        // These three are interpolated into the PowerShell command build-lock.ps1
        // executes. There is no escaping scheme that makes freehand text safe
        // there, so the tables ARE the safety property — and the shapes below are
        // exactly what an injection attempt looks like.
        EXPECT_EQ(Build::FindTarget("all"), nullptr);
        EXPECT_EQ(Build::FindTarget("ALL_BUILD"), nullptr);
        EXPECT_EQ(Build::FindTarget("install"), nullptr);
        EXPECT_EQ(Build::FindTarget("OloEngine-Tests\"; rm -rf /"), nullptr);
        EXPECT_EQ(Build::FindTarget(""), nullptr);

        EXPECT_TRUE(Build::IsKnownConfig("Debug"));
        EXPECT_TRUE(Build::IsKnownConfig("Release"));
        EXPECT_TRUE(Build::IsKnownConfig("Dist"));
        EXPECT_FALSE(Build::IsKnownConfig("debug")) << "Configuration names are case-sensitive to CMake.";
        EXPECT_FALSE(Build::IsKnownConfig("Debug; whoami"));

        EXPECT_TRUE(Build::IsKnownBuildDir("build-cached"));
        EXPECT_TRUE(Build::IsKnownBuildDir("build"));
        EXPECT_TRUE(Build::IsKnownBuildDir("build-clang"));
        EXPECT_FALSE(Build::IsKnownBuildDir("../../elsewhere"));
        EXPECT_FALSE(Build::IsKnownBuildDir("build-cached extra"));
    }

    TEST(AutomationBuildTablesTest, ArtifactPathsExpandToTheRepoLayout)
    {
        // The layout is not folklore: olo_configure_app -> olo_set_output_directories
        // (cmake/CommonProperties.cmake) puts every app under <repo>/bin/<Config>/<target>/,
        // and OloEngine-Tests calls neither, so it keeps the Ninja-multi-config
        // default under the BUILD directory. A wrong entry here makes a
        // successful build report "missing-artifact", which is the safe
        // direction but still a bug.
        const Build::TargetSpec* tests = Build::FindTarget("OloEngine-Tests");
        ASSERT_NE(tests, nullptr);
        EXPECT_TRUE(tests->ArtifactUnderBuildDir);
        EXPECT_EQ(Build::ExpandArtifact(tests->Artifact, "Debug", /*windows*/ true),
                  "OloEngine/tests/Debug/OloEngine-Tests.exe");
        EXPECT_EQ(Build::ExpandArtifact(tests->Artifact, "Release", /*windows*/ false),
                  "OloEngine/tests/Release/OloEngine-Tests");

        const Build::TargetSpec* runtime = Build::FindTarget("OloRuntime");
        ASSERT_NE(runtime, nullptr);
        EXPECT_FALSE(runtime->ArtifactUnderBuildDir)
            << "bin/ is under the SOURCE tree, so every build tree writes the same file — the artefact path alone "
               "cannot say which tree produced it, and reporting it as build-relative would claim it can.";
        EXPECT_EQ(Build::ExpandArtifact(runtime->Artifact, "Debug", /*windows*/ true),
                  "bin/Debug/OloRuntime/OloRuntime.exe");

        const Build::TargetSpec* engine = Build::FindTarget("OloEngine");
        ASSERT_NE(engine, nullptr);
        EXPECT_EQ(Build::ExpandArtifact(engine->Artifact, "Debug", /*windows*/ true), "bin/Debug/OloEngine/OloEngine.lib");
        EXPECT_EQ(Build::ExpandArtifact(engine->Artifact, "Debug", /*windows*/ false), "bin/Debug/OloEngine/libOloEngine.a");
    }

    TEST(AutomationBuildTablesTest, TheBuildCommandIsAlwaysParallelCapped)
    {
        const std::string command = Build::BuildCommand("build-cached", "OloEngine-Tests", "Debug");
        EXPECT_EQ(command, "cmake --build build-cached --target OloEngine-Tests --config Debug --parallel 6");
        // The lock rewrites the number from measured free memory, but only when
        // it is asked to. An uncapped build has OOM'd this box before, so the
        // flag is present even though it is usually overwritten.
        EXPECT_NE(command.find("--parallel"), std::string::npos);
        // One target per invocation: `--target A --target B` builds both under
        // one cmake, and there is then no honest way to attribute wall time or a
        // failure to one of them.
        EXPECT_EQ(command.find("--target", command.find("--target") + 1), std::string::npos);
    }

    // ---- compiler diagnostics ---------------------------------------------

    TEST(AutomationBuildDiagnosticsTest, ParsesMsvcClangClGccAndLinkerShapes)
    {
        // MSVC cl: (line) only — cl emits no column at all.
        const auto msvc = Build::ParseDiagnosticLine(
            R"(C:\repos\Olo\OloEngine\src\Foo.cpp(42): error C2065: 'x': undeclared identifier)");
        ASSERT_TRUE(msvc.has_value());
        EXPECT_EQ(msvc->Level, Severity::Error);
        EXPECT_EQ(msvc->File, R"(C:\repos\Olo\OloEngine\src\Foo.cpp)");
        EXPECT_EQ(msvc->Line, 42);
        EXPECT_EQ(msvc->Column, 0);
        EXPECT_EQ(msvc->Code, "C2065");
        EXPECT_EQ(msvc->Message, "'x': undeclared identifier");

        // clang-cl: (line,col).
        const auto clangCl =
            Build::ParseDiagnosticLine(R"(C:\repos\Olo\Bar.cpp(7,13): warning: unused variable 'y' [-Wunused-variable])");
        ASSERT_TRUE(clangCl.has_value());
        EXPECT_EQ(clangCl->Level, Severity::Warning);
        EXPECT_EQ(clangCl->Line, 7);
        EXPECT_EQ(clangCl->Column, 13);
        EXPECT_TRUE(clangCl->Code.empty()) << "clang emits no MSVC-style diagnostic id.";
        EXPECT_EQ(clangCl->Message, "unused variable 'y' [-Wunused-variable]");

        // gcc / clang on Linux: file:line:col.
        const auto gcc = Build::ParseDiagnosticLine("/home/r/OloEngine/src/Baz.cpp:99:5: error: expected ';'");
        ASSERT_TRUE(gcc.has_value());
        EXPECT_EQ(gcc->File, "/home/r/OloEngine/src/Baz.cpp");
        EXPECT_EQ(gcc->Line, 99);
        EXPECT_EQ(gcc->Column, 5);
        EXPECT_EQ(gcc->Message, "expected ';'");

        // The linker knows an object, not a source location.
        const auto link = Build::ParseDiagnosticLine("Foo.obj : error LNK2019: unresolved external symbol \"void f(void)\"");
        ASSERT_TRUE(link.has_value());
        EXPECT_EQ(link->Level, Severity::Error);
        EXPECT_EQ(link->File, "Foo.obj");
        EXPECT_EQ(link->Line, 0);
        EXPECT_EQ(link->Code, "LNK2019");

        // ...and sometimes not even that.
        const auto fatal = Build::ParseDiagnosticLine("LINK : fatal error LNK1104: cannot open file 'OloEngine.lib'");
        ASSERT_TRUE(fatal.has_value());
        EXPECT_EQ(fatal->Level, Severity::Error) << "A 'fatal error' is an error, not a fourth severity.";
        EXPECT_TRUE(fatal->File.empty()) << "'LINK' is the linker naming itself, not a file.";
        EXPECT_EQ(fatal->Code, "LNK1104");
    }

    TEST(AutomationBuildDiagnosticsTest, LNK1168IsTheShapeBuildingTheEditorWouldProduce)
    {
        // Kept as a test rather than a comment: this is the exact failure the
        // OloEditor refusal exists to pre-empt, and if it ever reaches the
        // parser it must come back as an error record rather than as noise.
        const auto locked = Build::ParseDiagnosticLine(
            R"(LINK : fatal error LNK1168: cannot open bin\Debug\OloEditor\OloEditor.exe for writing)");
        ASSERT_TRUE(locked.has_value());
        EXPECT_EQ(locked->Code, "LNK1168");
        EXPECT_EQ(locked->Level, Severity::Error);
    }

    TEST(AutomationBuildDiagnosticsTest, ParenthesesInThePathAndInTheMessageDoNotDefeatTheSplit)
    {
        // Both of these are real lines this box can produce, and both were lost
        // by anchoring on the FIRST ')'.
        //
        // A path with parentheses: the first ')' closes "(x86)", "x86" is not a
        // line number, and giving up there dropped the diagnostic entirely.
        const auto programFiles = Build::ParseDiagnosticLine(
            R"(C:\Program Files (x86)\Windows Kits\10\include\um\winnt.h(9): warning C4005: 'X': macro redefinition)");
        ASSERT_TRUE(programFiles.has_value());
        EXPECT_EQ(programFiles->File, R"(C:\Program Files (x86)\Windows Kits\10\include\um\winnt.h)");
        EXPECT_EQ(programFiles->Line, 9);
        EXPECT_EQ(programFiles->Code, "C4005");
        EXPECT_EQ(programFiles->Level, Severity::Warning);

        // A gcc/clang message containing a call: "foo(3)" parses as a line
        // number, so the line was MIS-split and then discarded before the
        // gcc branch ever saw it. Falling through is what recovers it.
        const auto call = Build::ParseDiagnosticLine(
            "/h/f.cpp:99:5: error: no matching function for call to 'foo(3)'");
        ASSERT_TRUE(call.has_value());
        EXPECT_EQ(call->File, "/h/f.cpp");
        EXPECT_EQ(call->Line, 99);
        EXPECT_EQ(call->Column, 5);
        EXPECT_EQ(call->Message, "no matching function for call to 'foo(3)'");

        // Both at once, MSVC style: parens in the path AND in the message.
        const auto both = Build::ParseDiagnosticLine(
            R"(C:\Program Files (x86)\a\b.cpp(42,7): error C2660: 'f(int)': function does not take 2 arguments)");
        ASSERT_TRUE(both.has_value());
        EXPECT_EQ(both->Line, 42);
        EXPECT_EQ(both->Column, 7);
        EXPECT_EQ(both->Message, "'f(int)': function does not take 2 arguments");
    }

    TEST(AutomationBuildDiagnosticsTest, StripsTheMsbuildProjectSuffix)
    {
        // MSBuild appends the owning project to every line. Left in, two
        // otherwise identical diagnostics from two projects would fail to
        // deduplicate, and the message would carry a path nobody asked for.
        const auto withProject = Build::ParseDiagnosticLine(
            R"(  C:\r\Foo.cpp(3): warning C4996: 'strcpy': deprecated [C:\r\build\OloEngine.vcxproj])");
        ASSERT_TRUE(withProject.has_value());
        EXPECT_EQ(withProject->Message, "'strcpy': deprecated");
        EXPECT_EQ(withProject->Code, "C4996");

        // A message that merely ENDS in a bracket is not a project suffix.
        const auto notProject = Build::ParseDiagnosticLine(R"(C:\r\Foo.cpp(3): warning: unused 'y' [-Wunused])");
        ASSERT_TRUE(notProject.has_value());
        EXPECT_EQ(notProject->Message, "unused 'y' [-Wunused]");
    }

    TEST(AutomationBuildDiagnosticsTest, NonDiagnosticLinesAreNotDiagnostics)
    {
        for (const char* line : { "",
                                  "[42/1337] Building CXX object OloEngine/CMakeFiles/OloEngine.dir/src/Foo.cpp.obj",
                                  "ninja: build stopped: subcommand failed.",
                                  "[build-lock] acquired (pid=1234) -> cmake --build build-cached",
                                  "Foo.cpp",
                                  "  1 error generated." })
        {
            EXPECT_FALSE(Build::ParseDiagnosticLine(line).has_value()) << "misparsed: " << line;
        }
    }

    TEST(AutomationBuildDiagnosticsTest, DeduplicatesAndCountsUniqueRecords)
    {
        // MSVC emits a header warning once per translation unit that includes it.
        // Forty copies is one defect; reporting it forty times pushes the real
        // error past whatever cap the caller set.
        const std::string log =
            R"(C:\r\Shared.h(9): warning C4996: 'x': deprecated [C:\r\A.vcxproj])"
            "\n"
            R"(C:\r\Shared.h(9): warning C4996: 'x': deprecated [C:\r\B.vcxproj])"
            "\n"
            R"(C:\r\Shared.h(9): warning C4996: 'x': deprecated [C:\r\C.vcxproj])"
            "\n"
            R"(C:\r\Only.cpp(2): error C2065: 'z': undeclared identifier)"
            "\n";

        const std::vector<Diagnostic> parsed = Build::ParseDiagnostics(log, "C:/r");
        ASSERT_EQ(parsed.size(), 2u);
        EXPECT_EQ(parsed[0].Occurrences, 3u);
        EXPECT_EQ(parsed[1].Occurrences, 1u);
        // Order is first-occurrence order: the warning was seen first. What
        // matters is that nothing is re-sorted, so the FIRST error keeps its
        // position rather than being buried.
        EXPECT_EQ(parsed[0].Code, "C4996");
        EXPECT_EQ(parsed[1].Code, "C2065");

        const Build::DiagnosticCounts counts = Build::CountDiagnostics(parsed);
        EXPECT_EQ(counts.Errors, 1u);
        EXPECT_EQ(counts.Warnings, 1u) << "Counts are of UNIQUE records: '1 warning' means one thing to fix.";
    }

    TEST(AutomationBuildDiagnosticsTest, DeduplicationIsIndexedNotScanned)
    {
        // Order must still be first-occurrence order, and counts must still be
        // right, now that the duplicate lookup is a hash probe rather than a
        // linear scan. 5000 unique records would be 12.5M four-string compares
        // under the scan; this is here so the indexing cannot regress silently.
        std::vector<Diagnostic> many;
        constexpr sizet kUnique = 5000;
        many.reserve(kUnique * 2);
        for (sizet i = 0; i < kUnique; ++i)
            many.push_back(Diagnostic{ Severity::Warning, "f" + std::to_string(i) + ".cpp", 1, 0, "C4996", "w", 1 });
        for (sizet i = 0; i < kUnique; ++i) // every one again, in the same order
            many.push_back(Diagnostic{ Severity::Warning, "f" + std::to_string(i) + ".cpp", 1, 0, "C4996", "w", 1 });

        const std::vector<Diagnostic> unique = Build::Deduplicate(many);
        ASSERT_EQ(unique.size(), kUnique);
        EXPECT_EQ(unique.front().File, "f0.cpp") << "first-occurrence order must survive the indexing";
        EXPECT_EQ(unique.back().File, "f" + std::to_string(kUnique - 1) + ".cpp");
        for (const Diagnostic& diagnostic : unique)
            EXPECT_EQ(diagnostic.Occurrences, 2u);
        EXPECT_EQ(Build::CountDiagnostics(unique).Warnings, kUnique);

        // Records differing in ONE equality field only must not collapse — the
        // key is a concatenation, so a separator bug would merge them.
        const std::vector<Diagnostic> neighbours = Build::Deduplicate(
            { Diagnostic{ Severity::Warning, "a.cpp", 1, 0, "C1", "m", 1 },
              Diagnostic{ Severity::Warning, "a.cpp", 1, 0, "C1", "m2", 1 },    // message
              Diagnostic{ Severity::Warning, "a.cpp", 1, 0, "C11", "m", 1 },    // code
              Diagnostic{ Severity::Warning, "a.cpp", 11, 0, "C1", "m", 1 },    // line
              Diagnostic{ Severity::Warning, "a.cpp", 1, 10, "C1", "m", 1 },    // column
              Diagnostic{ Severity::Error, "a.cpp", 1, 0, "C1", "m", 1 },       // severity
              Diagnostic{ Severity::Warning, "a.cpp2", 1, 0, "C1", "m", 1 } }); // file
        EXPECT_EQ(neighbours.size(), 7u);
    }

    TEST(AutomationBuildDiagnosticsTest, RelativizesPathsUnderTheRepoRootOnly)
    {
        // MSVC and ninja disagree about the case of a Windows drive letter
        // within one build, so the comparison is case-insensitive.
        EXPECT_EQ(Build::RelativizePath(R"(C:\repos\Olo\OloEngine\src\Foo.cpp)", "c:/repos/Olo"),
                  "OloEngine/src/Foo.cpp");
        EXPECT_EQ(Build::RelativizePath("C:/repos/Olo/A.cpp", "C:/repos/Olo/"), "A.cpp");
        // A vcpkg header outside the tree keeps its absolute path: that is the
        // truthful answer, and blanking it would lose the only clue to where the
        // diagnostic came from.
        EXPECT_EQ(Build::RelativizePath("C:/vcpkg/installed/x64/include/foo.h", "C:/repos/Olo"),
                  "C:/vcpkg/installed/x64/include/foo.h");
        // A sibling directory that merely shares a prefix is NOT under the root.
        EXPECT_EQ(Build::RelativizePath("C:/repos/OloOther/A.cpp", "C:/repos/Olo"), "C:/repos/OloOther/A.cpp");
    }

    TEST(AutomationBuildDiagnosticsTest, ResolvesTheBuildDirRelativePathsNinjaActuallyEmits)
    {
        // ninja runs the compiler with the BUILD directory as its working
        // directory, so `__FILE__` comes back relative to that, not to the repo
        // root. These three shapes are verbatim from a live OloServer build on
        // 2026-09-10 — relativizing against the repo root alone left them
        // exactly as they arrived, which is not a path anything can open.
        EXPECT_EQ(Build::ResolveDiagnosticFile("../OloEngine/src/OloEngine/Core/UUID.h", "C:/repos/Olo",
                                               "build-cached"),
                  "OloEngine/src/OloEngine/Core/UUID.h");
        EXPECT_EQ(Build::ResolveDiagnosticFile("vcpkg_installed/x64-windows-static-md/include/entt/entity/storage.hpp",
                                               "C:/repos/Olo", "build-cached"),
                  "build-cached/vcpkg_installed/x64-windows-static-md/include/entt/entity/storage.hpp")
            << "a vcpkg header really is inside the build tree; rebasing it to the repo root would invent a path";
        // An absolute path outside the tree stays absolute: that is the truthful
        // answer, and blanking it would lose the only clue to where it came from.
        EXPECT_EQ(Build::ResolveDiagnosticFile(
                      "C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/14.51.36231/include/xmemory",
                      "C:/repos/Olo", "build-cached"),
                  "C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/14.51.36231/include/xmemory");
        // An absolute path INSIDE the tree still relativizes, as MSVC's cl emits.
        EXPECT_EQ(Build::ResolveDiagnosticFile(R"(C:\repos\Olo\OloEngine\src\Foo.cpp)", "c:/repos/Olo", "build"),
                  "OloEngine/src/Foo.cpp");

        // The collapse is lexical, so it never touches the filesystem — and a
        // '..' it cannot pop is KEPT, so an escape is visible rather than
        // silently rebased onto the repo root.
        EXPECT_EQ(Build::CollapseLexicalPath("a/./b/../c"), "a/c");
        EXPECT_EQ(Build::CollapseLexicalPath("../../x"), "../../x");
        EXPECT_EQ(Build::ResolveDiagnosticFile("../../outside/x.h", "C:/repos/Olo", "build-cached"), "../outside/x.h");
    }

    TEST(AutomationBuildDiagnosticsTest, ErrorsAreEmittedBeforeWarningsUnderACap)
    {
        // A cap that dropped the one error in favour of two warnings would hide
        // the reason the build failed.
        Build::TargetResult result;
        result.Target = "OloEngine-Tests";
        result.Outcome = TargetOutcome::Failed;
        result.Diagnostics = { Diagnostic{ Severity::Warning, "a.cpp", 1, 0, "C4996", "w1", 1 },
                               Diagnostic{ Severity::Warning, "b.cpp", 2, 0, "C4996", "w2", 1 },
                               Diagnostic{ Severity::Error, "c.cpp", 3, 0, "C2065", "boom", 1 } };

        const auto json = Build::TargetResultJson(result, /*maxDiagnostics*/ 1);
        ASSERT_EQ(json.at("diagnostics").size(), 1u);
        EXPECT_EQ(json.at("diagnostics")[0].at("severity"), "error");
        EXPECT_EQ(json.at("diagnostics")[0].at("message"), "boom");
        EXPECT_EQ(json.at("diagnosticsOmitted"), 2u) << "Truncation is counted, never silent.";
        EXPECT_EQ(json.at("errorCount"), 1u);
        EXPECT_EQ(json.at("warningCount"), 2u);
    }

    // ---- what build-lock.ps1 said ------------------------------------------

    TEST(AutomationBuildLockTest, ReadsTheAcquireTranscript)
    {
        const std::string log =
            "[build-lock] waiting - 1 ahead of us; held by pid=4242 in C:/repos/OloEngine-other\n"
            "[build-lock] parallelism: -j8 (free 31 GB); rewrote the caller's flag\n"
            "[build-lock] acquired (pid=99) slot=0 of 2, 1 building -> cmake --build build-cached\n"
            "[1/3] Building CXX object a.obj\n"
            "[build-lock] released (pid=99)\n";

        const Build::LockTranscript lock = Build::ReadLockTranscript(log);
        EXPECT_EQ(lock.Outcome, LockOutcome::Acquired);
        EXPECT_TRUE(lock.Waited);
        EXPECT_EQ(lock.HeldByPid, 4242);
        EXPECT_EQ(lock.HeldByWorktree, "C:/repos/OloEngine-other");
        EXPECT_EQ(lock.Jobs, 8) << "The lock decides parallelism at acquire time; the caller's -j6 was a hint.";
        EXPECT_EQ(lock.Lines.size(), 4u) << "Every [build-lock] line is kept verbatim, and only those.";
    }

    TEST(AutomationBuildLockTest, RecognisesTheStandDownThatExitsZeroWithoutBuilding)
    {
        // The case that makes an exit code worthless as evidence, and the one a
        // reader of build-lock.ps1 is most likely to miss: Test-Superseded exits
        // 0 having built nothing.
        const std::string log =
            "[build-lock] waiting - next in line; held by pid=7 in C:/repos/x\n"
            "[build-lock] superseded - a newer build was queued for C:/repos/x; standing down so the slot is not "
            "spent on a stale tree\n";

        const Build::LockTranscript lock = Build::ReadLockTranscript(log);
        EXPECT_EQ(lock.Outcome, LockOutcome::Superseded);
        EXPECT_EQ(Build::Verdict(lock.Outcome, /*exitCode*/ 0, Build::ArtifactState{ "p", true, 10, "t", true, -5.0 }),
                  TargetOutcome::NotBuilt)
            << "Exit 0 with a present artefact is EXACTLY what a stand-down looks like from the outside. Reading "
               "it as success is the failure this whole result shape exists to prevent.";
    }

    TEST(AutomationBuildLockTest, TheUnsafeReadingNeverOverridesTheSafeOne)
    {
        // A stand-down and an acquire cannot both happen in one run — the
        // stand-down path exits before ever acquiring. But if two runs' output
        // ever landed in one log, "nothing was built" must win, because the
        // consequence of guessing wrong the other way is a caller trusting an
        // artefact this call did not produce.
        const Build::LockTranscript afterSuperseded = Build::ReadLockTranscript(
            "[build-lock] superseded - a newer build was queued for C:/repos/x; standing down\n"
            "[build-lock] acquired (pid=9) -> cmake --build build-cached\n");
        EXPECT_EQ(afterSuperseded.Outcome, LockOutcome::Superseded);

        const Build::LockTranscript afterOrphan = Build::ReadLockTranscript(
            "[build-lock] the process that launched this build (pid=5) is gone - killing the orphaned build tree\n"
            "[build-lock] acquired (pid=9) -> cmake --build build-cached\n");
        EXPECT_EQ(afterOrphan.Outcome, LockOutcome::Orphaned);

        // The ordinary case is unaffected.
        EXPECT_EQ(Build::ReadLockTranscript("[build-lock] acquired (pid=9) -> cmake --build build-cached\n").Outcome,
                  LockOutcome::Acquired);
    }

    TEST(AutomationBuildLockTest, RecognisesTheOrphanKillAndTheConcurrencyRefusal)
    {
        const Build::LockTranscript orphan = Build::ReadLockTranscript(
            "[build-lock] acquired (pid=5) -> cmake --build build-cached\n"
            "[build-lock] the process that launched this build (pid=5) is gone - killing the orphaned build tree "
            "and releasing the lock\n");
        EXPECT_EQ(orphan.Outcome, LockOutcome::Orphaned);
        EXPECT_EQ(Build::Verdict(orphan.Outcome, /*exitCode*/ 0, Build::ArtifactState{ "p", true, 10, "t", true, 5.0 }),
                  TargetOutcome::Failed)
            << "An orphaned build never succeeded, whatever the kill happened to report.";

        // BOTH dash encodings, and this is not hypothetical. build-lock.ps1's
        // source writes an EM dash; a redirected log on this box contains a plain
        // ASCII '-', because the PowerShell host transcodes its output to the
        // console code page on the way out. Observed 2026-09-10, verbatim:
        //   [build-lock] not building alongside the current build - only 22.6 GB free (need 24)
        // A parser that matched the literal em dash would have returned the whole
        // line as the "reason" on every real machine and passed every test that
        // used the source spelling.
        for (const char* line : {
                 "[build-lock] not building alongside the current build - only 22.6 GB free (need 24)\n",
                 "[build-lock] not building alongside the current build \xe2\x80\x94 only 22.6 GB free (need 24)\n" })
        {
            const Build::LockTranscript refused = Build::ReadLockTranscript(line);
            EXPECT_EQ(refused.Outcome, LockOutcome::NeverStarted);
            EXPECT_EQ(refused.ConcurrencyRefusal, "only 22.6 GB free (need 24)") << line;
        }
    }

    TEST(AutomationBuildLockTest, ALogWithNoAcquireLineIsNeverStarted)
    {
        // The refusal path: the wait budget expired while another worktree held
        // the lock. Nothing was built, and the ABSENCE of the acquire line is
        // the evidence — the script's timeout is a PowerShell `throw`, so there
        // is no positive marker to look for.
        const Build::LockTranscript lock = Build::ReadLockTranscript(
            "[build-lock] waiting - 2 ahead of us; held by pid=11 in C:/repos/y\n"
            "Exception: [build-lock] timed out after 5m waiting for pid=11 (C:/repos/y).\n");
        EXPECT_EQ(lock.Outcome, LockOutcome::NeverStarted);
        EXPECT_TRUE(lock.Waited);
        EXPECT_EQ(lock.HeldByPid, 11);
        // The decorated line must still be captured: the host prefixes a
        // terminating error ("Exception: "), so a reader anchored at column 0
        // would drop exactly the line that names the holder.
        ASSERT_EQ(lock.Lines.size(), 2u);
        EXPECT_TRUE(lock.Lines[1].starts_with("[build-lock] timed out"))
            << "captured: " << lock.Lines[1];
        EXPECT_EQ(Build::Verdict(lock.Outcome, /*exitCode*/ 0, Build::ArtifactState{}), TargetOutcome::NotBuilt);
        // ...and an exit code of 1 from the throw does not turn it into a plain
        // failure: "nothing was built" is the more actionable fact, and it is
        // what the caller must not confuse with "the tree does not compile".
        EXPECT_EQ(Build::Verdict(lock.Outcome, /*exitCode*/ 1, Build::ArtifactState{}), TargetOutcome::NotBuilt);
    }

    TEST(AutomationBuildLockTest, TheRealRefusalTranscriptFromThisBox)
    {
        // Captured VERBATIM on 2026-09-10 by driving the live editor with
        // lockWaitSeconds:0 while three other worktrees were queued. It is here
        // rather than as a hand-written approximation because the hand-written
        // one was wrong twice: the em dash is ASCII by the time it lands in a
        // log, and a terminating error makes PowerShell echo the offending
        // SOURCE line — a truncated look-alike carrying an uninterpolated
        // ${TimeoutMinutes} — immediately above the real message.
        const std::string log =
            "[build-lock] waiting - 3 ahead of us; held by pid=36932 in C:\\repos\\OloEngine-parallel-recording-phase2-1013\r\n"
            "Exception: C:\\repos\\OloEngine-x\\.claude\\skills\\run-oloengine\\build-lock.ps1:679\r\n"
            "Line |\r\n"
            " 679 |              throw \"[build-lock] timed out after ${TimeoutMinutes}m wa .\r\n"
            "     |              ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\r\n"
            "     | [build-lock] timed out after 0m waiting for pid=36932 (C:\\repos\\OloEngine-parallel-recording-phase2-1013). A\r\n"
            "     | build that has held the lock this long is wedged - investigate it rather than overriding.\r\n";

        const Build::LockTranscript lock = Build::ReadLockTranscript(log);
        EXPECT_EQ(lock.Outcome, LockOutcome::NeverStarted);
        EXPECT_TRUE(lock.Waited);
        EXPECT_EQ(lock.HeldByPid, 36932);
        EXPECT_EQ(lock.HeldByWorktree, "C:\\repos\\OloEngine-parallel-recording-phase2-1013");

        // Exactly two real lines: the wait and the timeout. The echoed source
        // line is dropped — it is the host quoting the script, not the script
        // speaking, and it would show a caller a `${TimeoutMinutes}` that never
        // appears in real output.
        ASSERT_EQ(lock.Lines.size(), 2u) << "captured: " << (lock.Lines.empty() ? std::string{} : lock.Lines[0]);
        EXPECT_TRUE(lock.Lines[0].starts_with("[build-lock] waiting"));
        EXPECT_TRUE(lock.Lines[1].starts_with("[build-lock] timed out after 0m"));
        for (const std::string& line : lock.Lines)
            EXPECT_EQ(line.find("${"), std::string::npos) << line;

        // And the whole point: a stale artefact sitting on disk next to this
        // transcript must not read as a build.
        Build::ArtifactState stale;
        stale.Exists = true;
        stale.MtimeKnown = true;
        stale.AgeRelativeToStartSeconds = -1562.0;
        EXPECT_EQ(Build::Verdict(lock.Outcome, /*exitCode*/ 1, stale), TargetOutcome::NotBuilt);
    }

    TEST(AutomationBuildLockTest, NinjaProgressComesFromTheLastEdgeCounter)
    {
        const Build::BuildProgress progress = Build::ReadLastNinjaProgress(
            "[1/120] Building CXX object a.obj\n"
            "[2/120] Building CXX object b.obj\n"
            "[117/120] Linking CXX static library OloEngine.lib\n");
        EXPECT_TRUE(progress.Known());
        EXPECT_EQ(progress.Done, 117u);
        EXPECT_EQ(progress.Total, 120u);

        // MSBuild prints no edge counter at all, so a `build/` tree reports no
        // fraction rather than a made-up one.
        EXPECT_FALSE(Build::ReadLastNinjaProgress("Build succeeded.\n    0 Warning(s)\n").Known());
        EXPECT_FALSE(Build::ReadLastNinjaProgress("[build-lock] acquired (pid=1)\n").Known());
    }

    // ---- the freshness verdict ---------------------------------------------

    TEST(AutomationBuildVerdictTest, AZeroExitWithNoArtifactIsNeverASuccess)
    {
        Build::ArtifactState absent;
        absent.Path = "build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe";
        absent.Exists = false;
        EXPECT_EQ(Build::Verdict(LockOutcome::Acquired, /*exitCode*/ 0, absent), TargetOutcome::MissingArtifact);
        EXPECT_FALSE(Build::IsSuccess(TargetOutcome::MissingArtifact));
    }

    TEST(AutomationBuildVerdictTest, AnUnreadableTimestampIsNotAPositiveVerdict)
    {
        // `StatArtifact` returns early when `fs::last_write_time` fails, leaving
        // the age at its default 0.0 — which is indistinguishable from "written
        // exactly when the build started". Read as a number that would be
        // `Rebuilt`: a positive verdict out of a measurement that never happened,
        // and the precise conflation this whole file exists to prevent. So the
        // flag is separate from the value.
        Build::ArtifactState unreadable;
        unreadable.Path = "bin/Debug/OloServer/OloServer.exe";
        unreadable.Exists = true;
        unreadable.MtimeKnown = false;
        unreadable.AgeRelativeToStartSeconds = 0.0;
        EXPECT_EQ(Build::Verdict(LockOutcome::Acquired, /*exitCode*/ 0, unreadable),
                  TargetOutcome::ArtifactUnverifiable);
        EXPECT_FALSE(Build::IsSuccess(TargetOutcome::ArtifactUnverifiable));

        // Flipping only the flag, with the same 0.0, flips the verdict — which is
        // the point: the value was never the evidence.
        unreadable.MtimeKnown = true;
        EXPECT_EQ(Build::Verdict(LockOutcome::Acquired, 0, unreadable), TargetOutcome::Rebuilt);

        // A failed build still reports as failed: the exit code is decided first.
        unreadable.MtimeKnown = false;
        EXPECT_EQ(Build::Verdict(LockOutcome::Acquired, 1, unreadable), TargetOutcome::Failed);
    }

    TEST(AutomationBuildVerdictTest, RebuiltAndUpToDateAreDistinguishedByTheArtifactsAge)
    {
        // Both are successes and both exit 0, but they are NOT the same answer:
        // "up to date" means the artefact predates this call, which is what a
        // stale binary looks like from the outside. Merging them is how an exit
        // code comes to be read as proof of a fresh build.
        Build::ArtifactState fresh;
        fresh.Exists = true;
        fresh.MtimeKnown = true;
        fresh.AgeRelativeToStartSeconds = 12.0; // written after the build began
        EXPECT_EQ(Build::Verdict(LockOutcome::Acquired, 0, fresh), TargetOutcome::Rebuilt);

        Build::ArtifactState stale;
        stale.Exists = true;
        stale.MtimeKnown = true;
        stale.AgeRelativeToStartSeconds = -3600.0; // an hour older than this call
        EXPECT_EQ(Build::Verdict(LockOutcome::Acquired, 0, stale), TargetOutcome::UpToDate);

        EXPECT_TRUE(Build::IsSuccess(TargetOutcome::Rebuilt));
        EXPECT_TRUE(Build::IsSuccess(TargetOutcome::UpToDate));
        EXPECT_NE(std::string(Build::TargetOutcomeName(TargetOutcome::Rebuilt)),
                  std::string(Build::TargetOutcomeName(TargetOutcome::UpToDate)));
    }

    TEST(AutomationBuildVerdictTest, TheLockOutcomeOutranksTheExitCode)
    {
        // Ordering matters: a stand-down and an orphan kill both carry exit
        // codes that would otherwise be believed.
        Build::ArtifactState present;
        present.Exists = true;
        present.MtimeKnown = true;
        present.AgeRelativeToStartSeconds = 5.0;

        EXPECT_EQ(Build::Verdict(LockOutcome::Superseded, 0, present), TargetOutcome::NotBuilt);
        EXPECT_EQ(Build::Verdict(LockOutcome::NeverStarted, 0, present), TargetOutcome::NotBuilt);
        EXPECT_EQ(Build::Verdict(LockOutcome::Orphaned, 0, present), TargetOutcome::Failed);
        // And a non-zero exit is a failure even with a fresh artefact: a build
        // can produce one target's output and then fail on the next edge.
        EXPECT_EQ(Build::Verdict(LockOutcome::Acquired, 1, present), TargetOutcome::Failed);

        EXPECT_FALSE(Build::IsSuccess(TargetOutcome::NotBuilt));
        EXPECT_FALSE(Build::IsSuccess(TargetOutcome::Failed));
        EXPECT_FALSE(Build::IsSuccess(TargetOutcome::Skipped));
    }

    TEST(AutomationBuildVerdictTest, TheResultReportsTheArtifactBeforeAndAfterWhenItMoved)
    {
        Build::TargetResult result;
        result.Target = "OloEngine-Tests";
        result.Outcome = TargetOutcome::Rebuilt;
        result.ExitCode = 0;
        result.WallSeconds = 42.5;
        result.Before = Build::ArtifactState{ "p", true, 100, "2026-09-01T00:00:00Z", true, -100.0 };
        result.After = Build::ArtifactState{ "p", true, 120, "2026-09-10T00:00:00Z", true, 3.0 };
        result.Lock.Outcome = LockOutcome::Acquired;

        const auto json = Build::TargetResultJson(result, 100);
        EXPECT_EQ(json.at("outcome"), "rebuilt");
        EXPECT_TRUE(json.at("success").get<bool>());
        EXPECT_EQ(json.at("artifact").at("modifiedUtc"), "2026-09-10T00:00:00Z");
        ASSERT_TRUE(json.contains("artifactBefore"))
            << "'the artefact is 9 days old and this build did not touch it' is exactly the question an exit code "
               "cannot answer.";
        EXPECT_EQ(json.at("artifactBefore").at("modifiedUtc"), "2026-09-01T00:00:00Z");
        EXPECT_EQ(json.at("lock").at("outcome"), "acquired");

        // Unchanged artefact => no before block, so the field's presence itself
        // carries the signal.
        result.Before = result.After;
        EXPECT_FALSE(Build::TargetResultJson(result, 100).contains("artifactBefore"));
    }

    TEST(AutomationBuildVerdictTest, WallTimeIsSplitIntoQueueAndBuild)
    {
        // Measured live on 2026-09-10: building a 15 KB DLL through the command
        // reported 1474 wall seconds, of which ~1451 were spent queued behind
        // three other worktrees. The total alone reads as "this build is
        // pathologically slow"; the split reads as "the machine was busy" — and
        // those two conclusions lead to opposite actions.
        Build::TargetResult result;
        result.Target = "OloEngine-LuaScriptCore";
        result.Outcome = TargetOutcome::Rebuilt;
        result.WallSeconds = 1474.1;
        result.QueuedSeconds = 1451.4;
        result.BuildSeconds = 22.7;
        result.After = Build::ArtifactState{ "p", true, 15872, "2026-09-10T18:13:59Z", true, 20.0 };

        const auto json = Build::TargetResultJson(result, 100);
        EXPECT_DOUBLE_EQ(json.at("queuedSeconds").get<double>(), 1451.4);
        EXPECT_DOUBLE_EQ(json.at("buildSeconds").get<double>(), 22.7);

        // Unobserved => OMITTED, not zero. A zero would say "no queue wait",
        // which is a claim, and the fields are only ever set from a poll that
        // actually saw the acquire happen.
        result.QueuedSeconds = -1.0;
        result.BuildSeconds = -1.0;
        const auto unknown = Build::TargetResultJson(result, 100);
        EXPECT_FALSE(unknown.contains("queuedSeconds"));
        EXPECT_FALSE(unknown.contains("buildSeconds"));
        EXPECT_TRUE(unknown.contains("wallSeconds")) << "the total is always known";
    }
} // namespace OloEngine::Tests
