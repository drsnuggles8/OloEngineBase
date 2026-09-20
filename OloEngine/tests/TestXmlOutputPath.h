#pragma once

// Issue #1372 — give every test process its own gtest report filename.
//
// THE RACE. When `GTEST_OUTPUT` (or `--gtest_output`) names a DIRECTORY rather
// than a file, googletest picks the filename itself, in
// FilePath::GenerateUniqueFileName():
//
//     int number = 0;
//     do { full_pathname.Set(MakeFileName(directory, base_name, number++, ext)); }
//     while (full_pathname.FileOrDirectoryExists());
//
// That is check-then-create with no lock and no O_EXCL, and the sanitizer shards
// run `ctest --parallel 4`. Two processes starting together both see
// `OloEngine-Tests_134.xml` as the highest that exists, both choose `_135`, and
// both write it. The survivors are spliced mid-tag —
//
//     <testcase name="..." classname="SkinOcularSurfaceTest" />
//     <testct="completed" ... classname="CommandBucketBatchTest" />
//
// — or a whole second `<?xml?>` document appended to a complete first one. The
// report step then fails to parse them, and one suite's results are simply gone.
// It had been happening on GREEN runs too: the #1083 shard-coverage guard counts
// ctest entries, not parseable XML, so nobody was told.
//
// THE FIX, AND WHY IT LIVES IN THE TEST BINARY. googletest resolves the filename
// inside InitGoogleTest (PostFlagParsingInit -> ConfigureXmlOutput ->
// GetAbsolutePathToOutputFile), so the flag has to be corrected BEFORE that call;
// nothing downstream can undo it. Doing it here rather than in CMake also covers
// the per-case `gtest_discover_tests` entries (OLO_HEAVY_TESTS,
// OLO_MESH_CACHE_TESTS), which have no per-test environment CMake could set
// without knowing each case's name at configure time. One place, every entry.
//
// WHAT IT DOES NOT TOUCH. An output spec naming an actual FILE is already unique
// by construction and is left exactly as given — that is what Windows.yml,
// cross-vendor.yml, vulkan-software.yml and gpu-conformance-amd.yml pass, and
// they are single-process runs. An unset `GTEST_OUTPUT` stays unset, so a local
// run is unchanged and no file appears where none did before.
//
// THE CORRECTION GOES BACK TO WHICHEVER SOURCE GTEST WILL READ — the `output`
// flag when the directory came from the environment, the argv slot when it came
// from `--gtest_output=`. Setting the flag alone is not enough: InitGoogleTest
// parses argv afterwards and the command line wins, so a directory passed there
// would silently reopen the race. Nothing in this repository passes one today.

#include <string>
#include <string_view>

namespace OloEngine::Tests
{
    // Filename-safe rendering of a gtest filter: every character outside
    // [A-Za-z0-9_-] becomes '_', runs collapse, and leading/trailing '_' are
    // dropped. `AllVariants/DeccerCubesLoaderFixture.Loads/3` becomes
    // `AllVariants_DeccerCubesLoaderFixture_Loads_3`, which is still the thing a
    // human greps for when a report goes missing.
    [[nodiscard]] std::string SanitizeForFilename(std::string_view text, std::string::size_type maxLength = 100);

    // The positive half of a gtest filter — everything before the first '-'.
    // Every suite entry carries the same long negative half (the per-case
    // exclusions), so including it would make every filename identical past the
    // truncation limit and defeat the point.
    [[nodiscard]] std::string_view PositiveFilter(std::string_view filter);

    // The value gtest's `output` flag should be REPLACED with, or an empty string
    // to leave it alone.
    //
    // Returns empty — deliberately, not as a failure — when the flag is empty,
    // has no `format:path` colon, already names a file, or names a format gtest
    // does not write. Only the directory case is rewritten, because only the
    // directory case races.
    //
    // `shardIndex` is GTEST_SHARD_INDEX, which is what distinguishes the chunks of
    // one large suite: they share a filter and differ only by that. `pid` is
    // belt-and-braces — (filter, shard) is already unique across the entries of a
    // single ctest run, since ctest runs each entry once.
    [[nodiscard]] std::string UniqueGTestOutputSpec(std::string_view outputFlag, std::string_view filter,
                                                    std::string_view shardIndex, int pid);
} // namespace OloEngine::Tests
