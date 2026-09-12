#pragma once

// Per-process resident-set ceiling for OloEngine-Tests.
//
// ctest runs every gtest case as its own process (gtest_discover_tests), so the
// memory a single case takes IS the memory of a process. On the self-hosted
// Linux runners that process shares a 14 GiB cgroup with a sibling (ctest
// --parallel 2) and with the runner itself; a case that grows past that is
// killed by the kernel's OOM killer with no test name, no message and, when
// systemd-oomd takes the whole user slice with it, no runner either. That is
// how six BC6HGpuEncoder cases -- whose compute shader took Mesa's AMD
// compiler 14 GB to build -- read as "Subprocess killed" for a week.
//
// The watchdog turns that into a test failure with a name attached: a thread
// polls the process's resident set and, past the ceiling, prints which test
// was running and exits with kMemoryCeilingExitCode. The default ceiling
// (TestOptions::RssCeilingMb, --olo-rss-ceiling-mb) is set from that CI
// arithmetic: two processes under 14 GiB with room left for the runner.
//
// Resident set, not virtual size: a sanitizer build reserves terabytes of
// shadow address space it never touches, so RLIMIT_AS / RLIMIT_DATA cannot
// express this bound, and Linux does not enforce RLIMIT_RSS at all.

#include "OloEngine/Core/Base.h"

namespace OloEngine::Tests
{
    // Exit code of a process the watchdog stopped. Distinct from gtest's 1 and
    // from the sanitizers' exit codes, so a ctest log or a grep can tell "went
    // over the ceiling" from "failed" and from "sanitizer report".
    inline constexpr int kMemoryCeilingExitCode = 77;

    // The process's current resident set, in bytes (0 when the platform cannot say).
    [[nodiscard]] u64 CurrentResidentBytes();

    // Starts the polling thread. A ceiling of 0 starts nothing. Call once, from main.
    void StartMemoryCeilingWatchdog(u64 ceilingMb);

    // Records the running test's name so the watchdog's message can carry it.
    // Appends a gtest listener; call after InitGoogleTest and before RUN_ALL_TESTS.
    void RegisterMemoryCeilingListener();
} // namespace OloEngine::Tests
