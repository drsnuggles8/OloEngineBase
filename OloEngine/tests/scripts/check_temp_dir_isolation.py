#!/usr/bin/env python3
# =============================================================================
# check_temp_dir_isolation.py
#
# Fails if a file under OloEngine/tests calls std::filesystem::temp_directory_path()
# directly instead of going through the shared helper in OloEngine/tests/TestTempDir.h
# (OloEngine::Tests::TempRoot / TempDir / TempFile).
#
# WHY
#
# gtest_discover_tests registers every gtest case as its own ctest entry, so each
# case runs in its own OloEngine-Tests.exe process, and CI runs them concurrently
# (ctest --parallel 4 on Windows, --parallel 2 in the sanitizer jobs). A fixed path
# under the system temp directory is therefore shared with every other process that
# names it: sibling cases, a second concurrent run of the binary (two worktrees on
# one box, a local run alongside CI), or a stale tree from a crashed run. One
# process's remove_all / create_directories lands in the middle of another's write
# and the victim sees !ofstream.is_open() or empty content — a false CI red that
# says nothing about a race.
#
# See docs/agent-rules/shared-temp-dir-test-isolation.md and issue #789.
#
# ESCAPE HATCH
#
# A site that genuinely must name the system temp root itself can opt out with
#
#     // OLO_TEMP_DIR_OK: <reason>
#
# on the same line or the line immediately above. Say WHY in the reason — the
# point of the marker is that the next reader can tell a considered exception
# from an oversight.
#
# Usage:  python OloEngine/tests/scripts/check_temp_dir_isolation.py
# =============================================================================

import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
TEST_ROOT = REPO_ROOT / "OloEngine" / "tests"

NEEDLE = "temp_directory_path"
OPT_OUT = "OLO_TEMP_DIR_OK"

# The helper itself is the one place that is allowed to call it.
ALLOWED = {
    pathlib.PurePosixPath("TestTempDir.cpp"),
    pathlib.PurePosixPath("TestTempDir.h"),
    pathlib.PurePosixPath("scripts/check_temp_dir_isolation.py"),
}


def main() -> int:
    offenders = []

    for path in sorted(TEST_ROOT.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".inl"):
            continue
        rel = pathlib.PurePosixPath(path.relative_to(TEST_ROOT).as_posix())
        if rel in ALLOWED:
            continue

        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        for i, line in enumerate(lines):
            if NEEDLE not in line:
                continue
            # Diagnostic strings that merely mention the name are fine; only a call
            # is a problem.
            if f"{NEEDLE}(" not in line:
                continue
            # ...and so is a comment explaining the rule (this file's own docs, the
            # helper header's rationale). A comment cannot construct a path.
            if line.lstrip().startswith(("//", "*", "#")):
                continue
            prev = lines[i - 1] if i > 0 else ""
            if OPT_OUT in line or OPT_OUT in prev:
                continue
            offenders.append((rel, i + 1, line.strip()))

    if not offenders:
        return 0

    print("Direct std::filesystem::temp_directory_path() calls in the test tree:", file=sys.stderr)
    print(file=sys.stderr)
    for rel, lineno, text in offenders:
        print(f"  OloEngine/tests/{rel}:{lineno}", file=sys.stderr)
        print(f"      {text}", file=sys.stderr)
    print(file=sys.stderr)
    print(
        "Every gtest case runs in its own process and CI runs them in parallel, so a\n"
        "fixed temp path is shared mutable state across processes. Use the helper:\n"
        "\n"
        '    #include "TestTempDir.h"\n'
        "\n"
        "    OloEngine::Tests::TempDir()            // per-process, per-case directory\n"
        '    OloEngine::Tests::TempDir("label")     // ...a second one for the same case\n'
        '    OloEngine::Tests::TempFile("x.yaml")   // a file inside it\n'
        "\n"
        "If a site genuinely must name the system temp root, annotate it with\n"
        "    // OLO_TEMP_DIR_OK: <reason>\n"
        "See docs/agent-rules/shared-temp-dir-test-isolation.md.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
