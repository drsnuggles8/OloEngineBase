// OLO_TEST_LAYER: plumbing
// =============================================================================
// TestXmlOutputPathTest.cpp — issue #1372, the rule that stops two test
// processes writing one report file.
//
// The bug this guards against produced no test failure and no error: the shards
// stayed green while suites' results silently vanished, and the report step went
// red only when enough files had been spliced for the parser to choke. So the
// assertions below are about the PROPERTY that prevents it — two processes that
// could run at the same time never get the same name — rather than about the
// exact spelling of any one filename.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "TestXmlOutputPath.h"

#include <set>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr int kPid = 4242;
        // What the sanitizer shards actually set.
        constexpr const char* kDirSpec = "xml:D:/a/OloEngineBase/OloEngineBase/test_results/";
    } // namespace

    // -----------------------------------------------------------------------
    // The case that matters: concurrent entries must not collide.
    // -----------------------------------------------------------------------
    TEST(TestXmlOutputPath, ConcurrentCtestEntriesNeverShareAFilename)
    {
        // A representative slice of one real ctest run: plain suites, two chunks
        // of one large suite (same filter, different shard), and the per-case
        // entries gtest_discover_tests registers for the heavy sets.
        struct Entry
        {
            const char* Filter;
            const char* Shard;
        };
        const Entry entries[] = {
            { "SkinOcularSurfaceTest.*-Heavy.*:Mesh.*", "" },
            { "CommandBucketBatchTest.*-Heavy.*:Mesh.*", "" },
            { "GPUScene.*-Heavy.*:Mesh.*", "0" },
            { "GPUScene.*-Heavy.*:Mesh.*", "1" },
            { "GPUScene.*-Heavy.*:Mesh.*", "2" },
            { "AllVariants/DeccerCubesLoaderFixture.Loads/3", "" },
            { "AllVariants/DeccerCubesLoaderFixture.Loads/4", "" },
            { "MeshCacheTest.WarmCacheHits", "" },
        };

        std::set<std::string> seen;
        for (const Entry& e : entries)
        {
            const std::string spec = UniqueGTestOutputSpec(kDirSpec, e.Filter, e.Shard, kPid);
            ASSERT_FALSE(spec.empty()) << "a directory spec must be rewritten: " << e.Filter;
            EXPECT_TRUE(seen.insert(spec).second)
                << "two concurrent ctest entries produced the same report path: " << spec
                << " — this is exactly the #1372 collision, and the shards would stay green while one "
                   "suite's results were overwritten";
        }
        EXPECT_EQ(seen.size(), std::size(entries));

        // The chunks of one suite differ ONLY by shard index, which is why the
        // shard has to be in the name at all. Stated on its own so a refactor
        // that drops it fails here rather than in CI six months later.
        const std::string chunk0 = UniqueGTestOutputSpec(kDirSpec, "GPUScene.*-X", "0", kPid);
        const std::string chunk1 = UniqueGTestOutputSpec(kDirSpec, "GPUScene.*-X", "1", kPid);
        EXPECT_NE(chunk0, chunk1);
    }

    // -----------------------------------------------------------------------
    // The rewrite lands in the directory CI asked for, and is still greppable.
    // -----------------------------------------------------------------------
    TEST(TestXmlOutputPath, TheRewriteKeepsTheRequestedDirectoryAndNamesTheSuite)
    {
        const std::string spec = UniqueGTestOutputSpec(kDirSpec, "SkinOcularSurfaceTest.*-Heavy.*", "", kPid);
        EXPECT_EQ(spec, "xml:D:/a/OloEngineBase/OloEngineBase/test_results/"
                        "OloEngine-Tests_SkinOcularSurfaceTest_4242.xml");

        // The negative half of the filter is identical on every suite entry, so
        // it must NOT reach the name — otherwise every file is the same past the
        // truncation limit, which is the collision again wearing a hat.
        EXPECT_EQ(spec.find("Heavy"), std::string::npos);

        // json is written the same way, extension included.
        const std::string json = UniqueGTestOutputSpec("json:out/", "Foo.*", "", 7);
        EXPECT_EQ(json, "json:out/OloEngine-Tests_Foo_7.json");

        // A Windows separator is a directory too.
        EXPECT_FALSE(UniqueGTestOutputSpec("xml:C:\\results\\", "Foo.*", "", 7).empty());
    }

    // -----------------------------------------------------------------------
    // Everything that must be left alone. A fix that quietly started writing
    // files where none appeared before, or that moved a CI job's explicit
    // output, would be its own incident.
    // -----------------------------------------------------------------------
    TEST(TestXmlOutputPath, NonDirectorySpecsAreLeftExactlyAsGiven)
    {
        // Unset GTEST_OUTPUT — the local default. Must stay unset.
        EXPECT_TRUE(UniqueGTestOutputSpec("", "Foo.*", "", kPid).empty());

        // An explicit FILE, which is what Windows.yml, cross-vendor.yml,
        // vulkan-software.yml and gpu-conformance-amd.yml pass. Already unique.
        EXPECT_TRUE(UniqueGTestOutputSpec("xml:../test_results/vulkan_software.xml", "Vulkan*", "", kPid).empty());

        // No colon: gtest's own default filename, not a directory.
        EXPECT_TRUE(UniqueGTestOutputSpec("xml", "Foo.*", "", kPid).empty());

        // A format gtest does not write. Rewriting it would swallow the warning
        // it is supposed to print.
        EXPECT_TRUE(UniqueGTestOutputSpec("yaml:out/", "Foo.*", "", kPid).empty());
    }

    // -----------------------------------------------------------------------
    // Sanitisation, including the inputs that would produce an unusable path.
    // -----------------------------------------------------------------------
    TEST(TestXmlOutputPath, SanitizationProducesAUsableFilenameFromAnyFilter)
    {
        EXPECT_EQ(SanitizeForFilename("AllVariants/DeccerCubesLoaderFixture.Loads/3"),
                  "AllVariants_DeccerCubesLoaderFixture_Loads_3");
        // A run of unsafe characters collapses rather than stacking underscores.
        EXPECT_EQ(SanitizeForFilename("A.*:B"), "A_B");
        // Leading and trailing separators are dropped, not turned into '_'.
        EXPECT_EQ(SanitizeForFilename("*Foo*"), "Foo");
        EXPECT_EQ(SanitizeForFilename(""), "");
        // '-' is SAFE and survives: it is a filename character, and the prefix
        // these names carry is itself `OloEngine-Tests`. Only characters that
        // cannot go in a path are replaced.
        EXPECT_EQ(SanitizeForFilename("A-B"), "A-B");
        // A filter of only UNSAFE characters collapses to nothing.
        EXPECT_EQ(SanitizeForFilename("*"), "");
        EXPECT_EQ(SanitizeForFilename(".:/"), "");

        // Truncation must not leave a trailing '_'.
        const std::string truncated = SanitizeForFilename("ABCDEFGHIJ.KLMNOP", 10);
        EXPECT_EQ(truncated, "ABCDEFGHIJ");
        EXPECT_EQ(SanitizeForFilename("ABCDEFGHI.KLMNOP", 10), "ABCDEFGHI");

        // A filter of only unsafe characters still yields a writable path —
        // an empty token would produce `OloEngine-Tests__4242.xml`, which is
        // legal but says nothing, so it is named instead.
        const std::string spec = UniqueGTestOutputSpec("xml:out/", "*", "", kPid);
        EXPECT_EQ(spec, "xml:out/OloEngine-Tests_all_4242.xml");
    }

    TEST(TestXmlOutputPath, ThePositiveHalfOfAFilterIsEverythingBeforeTheFirstDash)
    {
        EXPECT_EQ(PositiveFilter("Suite.*-Excluded.*:Other.*"), "Suite.*");
        EXPECT_EQ(PositiveFilter("Suite.*"), "Suite.*");
        EXPECT_EQ(PositiveFilter("-OnlyNegative.*"), "");
        EXPECT_EQ(PositiveFilter(""), "");
    }
} // namespace OloEngine::Tests
