#pragma once

#include <filesystem>
#include <string_view>

// -----------------------------------------------------------------------------
// Per-process, per-case scratch paths for the test suite.
//
// WHY THIS EXISTS
//
// `gtest_discover_tests` registers every gtest case as its own ctest entry, so
// each case runs in its OWN `OloEngine-Tests.exe` process, and CI runs them
// concurrently (`ctest --parallel 4` on Windows, `--parallel 2` in the three
// sanitizer jobs). A test that writes to a FIXED path under
// `std::filesystem::temp_directory_path()` is therefore sharing a mutable OS
// resource with every other process that names the same path — its own sibling
// cases, a second concurrent run of the binary (two worktrees on one box, a
// local run alongside CI), or a stale tree left behind by a crashed run.
//
// The failure is silent: one process's `remove_all` / `create_directories`
// lands in the middle of another's write, and the victim sees
// `!ofstream.is_open()` or empty read-back content. Nothing in the output says
// "race". See docs/agent-rules/shared-temp-dir-test-isolation.md.
//
// THE CONTRACT
//
// Tests do not call `std::filesystem::temp_directory_path()` directly. They call
// `TempDir()` / `TempFile()` below, which hand back a path under a root that is
// unique to THIS process and a leaf that is unique to the CURRENT gtest case.
// A pre-commit hook (`test-temp-dir-isolated`) enforces this.
//
// The root is claimed exclusively at first use and removed at process exit, so
// the suite also stops leaking scratch files into the shared temp root.
// -----------------------------------------------------------------------------

namespace OloEngine::Tests
{
    /// The scratch root owned by THIS process:
    /// `<system temp>/OloEngineTests-<pid>_<rand>`.
    ///
    /// Created on first call (exclusively — a losing claim retries with a fresh
    /// suffix, so a recycled PID or a stale directory from a crashed run can
    /// never be adopted) and `remove_all`'d at normal process exit.
    ///
    /// Deliberately a flat prefix rather than a shared `OloEngineTests/` parent:
    /// a fixed-name directory in a world-writable, sticky `/tmp` is un-creatable
    /// for every account except the one that made it, and this repo's CI runs as
    /// its own user alongside others. `OloEngineTests-*` sweeps just as easily.
    ///
    /// Falls back to the current working directory if the system temp directory
    /// is unusable, and reports on stderr if neither is writable, so a test never
    /// silently writes to an unexpected place.
    [[nodiscard]] const std::filesystem::path& TempRoot();

    /// A directory unique to the current gtest case within this process:
    /// `TempRoot()/<Suite>.<Case>[.<label>]`.
    ///
    /// **Emptied on the first call within each test**, then stable for the rest of
    /// that test — so a fixture gets the clean slate a `SetUp` `remove_all` used to
    /// give it, including under `--gtest_repeat`, where the same case runs several
    /// times in one process and would otherwise inherit its own leftovers.
    /// (Requires `RegisterCleanSlateListener()`; without it the directory is merely
    /// created if missing.)
    ///
    /// `label` distinguishes several directories owned by one case; omit it when
    /// the case only needs one.
    [[nodiscard]] std::filesystem::path TempDir(std::string_view label = {});

    /// A file path inside `TempDir()`. The parent directory is created; the file
    /// itself is NOT, so this is also the way to name a path that is guaranteed
    /// not to exist.
    [[nodiscard]] std::filesystem::path TempFile(std::string_view name);

    /// Arm the clean-slate behaviour described on `TempDir()`. Call once from
    /// `main()` before `RUN_ALL_TESTS()`, alongside the other global listeners.
    void RegisterCleanSlateListener();
} // namespace OloEngine::Tests
