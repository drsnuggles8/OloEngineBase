// OLO_TEST_LAYER: unit
//
// Guards Core/BuildInfo.{h,cpp} — the version/git-hash/build-id surface
// stamped in at configure time (issue #894). The interesting property isn't
// "the getter returns a string" (that's a compile-time constant, not worth a
// test); it's that GetBuildId() actually composes GetEngineVersion() and
// GetGitHash() the way callers (the game manifest, Application::Run's startup
// log) depend on, and that the "unknown" hash fallback degrades to just the
// version instead of emitting a dangling "+unknown" suffix.

#include "OloEngine/Core/BuildInfo.h"

#include <gtest/gtest.h>

#include <string>

namespace OloEngine::Tests
{
    TEST(BuildInfoTest, EngineVersionIsNonEmpty)
    {
        const std::string version = BuildInfo::GetEngineVersion();
        EXPECT_FALSE(version.empty());
    }

    TEST(BuildInfoTest, GitHashAndDescribeAreNonEmpty)
    {
        // Never empty even outside a git checkout — the CMake side always
        // sets at least "unknown" rather than leaving the macro undefined.
        EXPECT_FALSE(std::string(BuildInfo::GetGitHash()).empty());
        EXPECT_FALSE(std::string(BuildInfo::GetGitDescribe()).empty());
    }

    TEST(BuildInfoTest, BuildTimestampIsNonEmpty)
    {
        EXPECT_FALSE(std::string(BuildInfo::GetBuildTimestamp()).empty());
    }

    TEST(BuildInfoTest, BuildIdEmbedsTheEngineVersion)
    {
        // Whatever the git hash resolves to on this machine, the build id must
        // always carry the engine version as a prefix — that's the part a bug
        // report can compare against a changelog even without a git checkout
        // to look the hash up in.
        const std::string buildId = BuildInfo::GetBuildId();
        const std::string version = BuildInfo::GetEngineVersion();

        EXPECT_FALSE(buildId.empty());
        EXPECT_EQ(buildId.rfind(version, 0), 0u)
            << "GetBuildId() = '" << buildId << "' should start with the engine version '" << version << "'";
    }

    TEST(BuildInfoTest, BuildIdNeverEndsWithAnUnknownSuffix)
    {
        // A checkout that resolves a git hash must not silently mask it; a
        // checkout that CANNOT (a source tarball, a stripped archive) must not
        // grow a "+unknown" suffix that reads as a real, if odd, hash.
        const std::string buildId = BuildInfo::GetBuildId();
        const std::string hash = BuildInfo::GetGitHash();

        if (hash == "unknown")
        {
            EXPECT_EQ(buildId, BuildInfo::GetEngineVersion());
        }
        else
        {
            EXPECT_NE(buildId.find(hash), std::string::npos)
                << "GetBuildId() = '" << buildId << "' should embed the resolved git hash '" << hash << "'";
        }
    }

    TEST(BuildInfoTest, BuildIdDirtySuffixMatchesIsWorkingTreeDirty)
    {
        // GetBuildId()'s "-dirty" suffix must come from IsWorkingTreeDirty()
        // (a real git status check), never from parsing GetGitDescribe()'s
        // text — a permitted tag literally named e.g. "release-dirty" would
        // otherwise make a CLEAN checkout look dirty. This can't fabricate a
        // dirty tree to prove the reverse (there's no seam to fake
        // IsWorkingTreeDirty() from a unit test), but it does pin the
        // consistency contract between the two: whatever this checkout's
        // real dirty state is, GetBuildId() must agree with it exactly.
        const std::string buildId = BuildInfo::GetBuildId();
        const bool endsWithDirty = buildId.size() >= 6 && buildId.compare(buildId.size() - 6, 6, "-dirty") == 0;

        EXPECT_EQ(endsWithDirty, BuildInfo::IsWorkingTreeDirty())
            << "GetBuildId() = '" << buildId << "', IsWorkingTreeDirty() = " << BuildInfo::IsWorkingTreeDirty();
    }
} // namespace OloEngine::Tests
