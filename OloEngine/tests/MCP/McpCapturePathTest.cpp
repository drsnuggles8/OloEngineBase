#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpCapturePathTest — unit test (headless, no GL, no live editor).
//
// Pins the sandbox behind `olo_screenshot`'s optional `path` (issue #607).
//
// THE REJECTIONS ARE THE FEATURE. The argument exists so a session driving the
// diagnostics server over raw HTTP can open a file instead of base64-decoding
// every capture. What makes that safe to ship on an unauthenticated localhost
// server is that the argument is a NAME under one fixed directory, not a path:
// an absolute path or a '..' component is refused outright.
//
// So the assertions below are mostly negative, and each one is a way the guard
// has been got wrong elsewhere rather than a hypothetical:
//
//   * "C:/x.png" and "/etc/x.png" — the obvious escape, and the one a caller
//     reaches for first because every other file argument in this repo takes a
//     project-relative path that MAY be absolute (olo_scene_open does).
//   * "../../x.png" — refused on the path AS WRITTEN. A guard that prepends the
//     root first and checks the normalised result afterwards passes or fails
//     depending on how DEEP the root happens to be, which is a guard that works
//     until someone shortens the root.
//   * "a/../../x.png" — the same escape hidden behind a component that cancels,
//     which lexically_normal() would collapse before a post-hoc check saw it.
//   * "a..b.png" — NOT a traversal. A substring test instead of a component
//     test rejects this, and a caller with a date-stamped name hits it.
//
// The positive half pins the two things a caller depends on: a bare name lands
// under the root, and the extension is forced to .png so the file never lies
// about its format.
//
// Resolve()'s symlink-containment half needs a real tree and is covered by the
// live verification; ValidateRelative is pure and is what this file pins.
// =============================================================================

#include "MCP/McpCapturePath.h"

#include <filesystem>
#include <string>
#include <string_view>

// OLO_TEST_LAYER: unit

namespace
{
    namespace CapturePath = OloEngine::MCP::CapturePath;
    namespace fs = std::filesystem;

    // The resolved path as forward-slash text, so an assertion reads the same
    // on both platforms.
    [[nodiscard]] std::string Accept(std::string_view input)
    {
        fs::path resolved;
        const auto error = CapturePath::ValidateRelative(input, resolved);
        EXPECT_FALSE(error.has_value()) << "expected '" << input << "' to be accepted, got: " << error.value_or("");
        return resolved.generic_string();
    }

    [[nodiscard]] std::string Reject(std::string_view input)
    {
        fs::path resolved;
        const auto error = CapturePath::ValidateRelative(input, resolved);
        EXPECT_TRUE(error.has_value()) << "expected '" << input << "' to be REJECTED, but it resolved to "
                                       << resolved.generic_string();
        return error.value_or("");
    }

    TEST(McpCapturePath, BareNameLandsUnderTheCapturesRoot)
    {
        EXPECT_EQ(Accept("shot.png"), "assets/mcp-captures/shot.png");
    }

    TEST(McpCapturePath, SubdirectoriesAreAllowedUnderTheRoot)
    {
        EXPECT_EQ(Accept("ssr/before.png"), "assets/mcp-captures/ssr/before.png");
        EXPECT_EQ(Accept("a/b/c/deep.png"), "assets/mcp-captures/a/b/c/deep.png");
    }

    TEST(McpCapturePath, APathAlreadyRootedAtTheCapturesDirIsNotDoubled)
    {
        // The tool reports the resolved path back; feeding that path in again
        // must name the same file rather than assets/mcp-captures/assets/...
        EXPECT_EQ(Accept("assets/mcp-captures/shot.png"), "assets/mcp-captures/shot.png");
    }

    TEST(McpCapturePath, TheExtensionIsForcedToPngBecauseTheBytesAlwaysArePng)
    {
        EXPECT_EQ(Accept("shot"), "assets/mcp-captures/shot.png");
        EXPECT_EQ(Accept("shot.jpg"), "assets/mcp-captures/shot.jpg.png");
        // An existing .png is not doubled, in either case.
        EXPECT_EQ(Accept("shot.png"), "assets/mcp-captures/shot.png");
        EXPECT_EQ(Accept("shot.PNG"), "assets/mcp-captures/shot.PNG");
    }

    TEST(McpCapturePath, BackslashSeparatorsResolveLikeForwardSlashes)
    {
        // The caller is often a Windows session pasting a Windows-shaped name.
        EXPECT_EQ(Accept("ssr\\before.png"), "assets/mcp-captures/ssr/before.png");
    }

    TEST(McpCapturePath, AnAbsolutePathIsRejectedNeverRewritten)
    {
        EXPECT_NE(Reject("C:/Windows/System32/evil.png").find("relative"), std::string::npos);
        EXPECT_FALSE(Reject("/etc/passwd.png").empty());
        EXPECT_FALSE(Reject("\\\\server\\share\\evil.png").empty());
        // A drive-relative path ("C:evil.png") has a root NAME but no root
        // directory — rejected by the same predicate, and worth pinning because
        // is_absolute() alone returns false for it on Windows.
        EXPECT_FALSE(Reject("C:evil.png").empty());
    }

    TEST(McpCapturePath, ParentDirectoryTraversalIsRejected)
    {
        EXPECT_NE(Reject("../escape.png").find(".."), std::string::npos);
        EXPECT_FALSE(Reject("../../../../escape.png").empty());
        EXPECT_FALSE(Reject("ssr/../../escape.png").empty());
        // The cancelling-component form: normalisation collapses "a/.." and a
        // post-hoc check would then see a path that looks clean.
        EXPECT_FALSE(Reject("a/../../escape.png").empty());
        EXPECT_FALSE(Reject("a\\..\\..\\escape.png").empty());
        // A trailing traversal cannot name a file, but must still be refused by
        // the traversal rule rather than by accident.
        EXPECT_FALSE(Reject("shots/..").empty());
    }

    TEST(McpCapturePath, ADotDotSubstringInAFilenameIsNotATraversal)
    {
        // A component test, not a substring test. "2026-09-20..final.png" is a
        // perfectly ordinary name and a substring guard rejects it.
        EXPECT_EQ(Accept("a..b.png"), "assets/mcp-captures/a..b.png");
        EXPECT_EQ(Accept("2026-09-20..final.png"), "assets/mcp-captures/2026-09-20..final.png");
    }

    TEST(McpCapturePath, NamingTheRootItselfDoesNotEscapeToItsSIBLING)
    {
        // The hole this pins, found in review: "assets/mcp-captures" takes the
        // already-under-root branch (it IS the root), the forced extension is
        // then appended, and the result is "assets/mcp-captures.png" — a file
        // OUTSIDE the directory, reached with no '..' and no absolute path.
        // Every input-shaped check passes it; only a containment check on the
        // FINAL path catches it.
        for (const char* bad : { "assets/mcp-captures", "assets/mcp-captures/", "assets/mcp-captures/." })
        {
            fs::path resolved;
            const auto error = CapturePath::ValidateRelative(bad, resolved);
            EXPECT_TRUE(error.has_value()) << "'" << bad << "' resolved to " << resolved.generic_string();
            EXPECT_NE(resolved.generic_string(), "assets/mcp-captures.png")
                << "'" << bad << "' escaped to the sibling file";
        }
    }

    TEST(McpCapturePath, EveryAcceptedPathIsStrictlyInsideTheRoot)
    {
        // The invariant stated directly, over everything the positive tests
        // accept: the result always has the root as a proper prefix and always
        // names something beneath it.
        for (const char* good : { "shot.png", "shot", "ssr/before.png", "a/b/c/deep.png",
                                  "assets/mcp-captures/shot.png", "a..b.png", "ssr\\before.png" })
        {
            fs::path resolved;
            ASSERT_FALSE(CapturePath::ValidateRelative(good, resolved).has_value()) << good;
            const std::string text = resolved.generic_string();
            EXPECT_TRUE(text.rfind("assets/mcp-captures/", 0) == 0)
                << "'" << good << "' resolved outside the root: " << text;
            EXPECT_GT(text.size(), std::string("assets/mcp-captures/").size()) << text;
        }
    }

    TEST(McpCapturePath, EmptyAndDirectoryOnlyPathsAreRejected)
    {
        EXPECT_FALSE(Reject("").empty());
        EXPECT_FALSE(Reject(".").empty());
        // Ends in a separator: names a directory, not a file.
        EXPECT_FALSE(Reject("ssr/").empty());
    }

    TEST(McpCapturePath, EveryRejectionNamesTheArgumentAndTheRoot)
    {
        // The error text is the only thing a raw-HTTP caller sees, and it has
        // to say which argument and where captures go — otherwise the caller's
        // next move is to guess another path.
        for (const char* bad : { "", "/abs.png", "../x.png" })
        {
            const std::string error = Reject(bad);
            EXPECT_NE(error.find("'path'"), std::string::npos) << "for input '" << bad << "': " << error;
        }
        EXPECT_NE(Reject("").find(CapturePath::RootDisplay()), std::string::npos);
        EXPECT_NE(Reject("/abs.png").find(CapturePath::RootDisplay()), std::string::npos);
    }

    TEST(McpCapturePath, TheRootIsTheDocumentedOne)
    {
        // The schema description, the guide and the .gitignore entry all name
        // this directory. Pinning it here means a rename breaks a test rather
        // than silently stranding the three of them.
        EXPECT_EQ(CapturePath::Root().generic_string(), "assets/mcp-captures");
        EXPECT_EQ(CapturePath::RootDisplay(), "assets/mcp-captures/");
    }
} // namespace
