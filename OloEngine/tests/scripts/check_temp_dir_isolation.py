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
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
TEST_ROOT = REPO_ROOT / "OloEngine" / "tests"

NEEDLE = "temp_directory_path"

# A call, not a mention. The `(` may be separated from the name by whitespace, a
# newline, or a comment — `temp_directory_path /* why */ ()` and a call split
# across two lines are still calls, so the scan runs over the whole file rather
# than line by line.
CALL = re.compile(
    NEEDLE + r"""\s*(?:/\*.*?\*/\s*|//[^\n]*\n\s*)*\(""",
    re.DOTALL,
)

# Only a real comment marker with a non-empty reason opts out. Matching the bare
# word anywhere would let a string literal — or the word in running prose — turn
# the guard off.
OPT_OUT_LINE = re.compile(r"(?://|\*|#)\s*OLO_TEMP_DIR_OK:\s*\S")

# A string literal, or a `//` or `/* */` comment: none of these can construct a
# path, and all three legitimately name the function while explaining the rule.
#
# The string alternative MUST come first. A `//` inside a string literal
# (`"http://x"`) would otherwise start a fake comment and mask the rest of the
# line, hiding a real call that follows it on that line.
COMMENT_OR_STRING = re.compile(r"\"(?:[^\"\\\n]|\\.)*\"|//[^\n]*|/\*.*?\*/", re.DOTALL)

# The helper itself is the one place that is allowed to call it.
ALLOWED = {
    pathlib.PurePosixPath("TestTempDir.cpp"),
    pathlib.PurePosixPath("TestTempDir.h"),
    pathlib.PurePosixPath("scripts/check_temp_dir_isolation.py"),
}


def scan(text, rel):
    """Every un-excused temp_directory_path() call in `text`, as (rel, line, source)."""
    # Blank out comments and string literals first, keeping the text's length and
    # newlines so offsets still map to the right line. This is what makes a
    # mention immune and a call visible, without a C++ parser.
    masked = COMMENT_OR_STRING.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)

    lines = text.splitlines()
    found = []
    for match in CALL.finditer(masked):
        lineno = masked.count("\n", 0, match.start()) + 1
        source = lines[lineno - 1] if lineno <= len(lines) else ""
        # The marker must be a comment carrying a reason, on the call line or the
        # one immediately above it.
        prev = lines[lineno - 2] if lineno >= 2 else ""
        if OPT_OUT_LINE.search(source) or OPT_OUT_LINE.search(prev):
            continue
        found.append((rel, lineno, source.strip()))
    return found


SELF_TEST_CASES = [
    # (source, expected offender count, description)
    ('auto p = std::filesystem::temp_directory_path() / "x";\n', 1, "plain call"),
    ('auto p = fs::temp_directory_path(ec);\n', 1, "call with an argument"),
    # Split across lines / interrupted by a comment — a line-by-line substring
    # check misses all three of these, and they are all still calls.
    ('auto p = fs::temp_directory_path\n    ();\n', 1, "call split across lines"),
    ('auto p = fs::temp_directory_path /* why */ ();\n', 1, "comment before the paren"),
    ('auto p = fs::temp_directory_path //\n();\n', 1, "line comment before the paren"),
    # Mentions, not calls.
    ('// see std::filesystem::temp_directory_path() for why\n', 0, "comment mention"),
    ('error = "temp_directory_path() failed: " + ec.message();\n', 0, "string mention"),
    ('/* temp_directory_path() is banned here */\n', 0, "block-comment mention"),
    # A `//` inside a string must not start a comment and mask the call after it.
    ('log("see http://x"); auto p = fs::temp_directory_path();\n',
     1, "call after a string containing //"),
    # Opt-out forms.
    ('// OLO_TEMP_DIR_OK: libFuzzer target.\nauto p = fs::temp_directory_path();\n',
     0, "marker with a reason on the preceding line"),
    ('auto p = fs::temp_directory_path(); // OLO_TEMP_DIR_OK: libFuzzer target.\n',
     0, "marker with a reason trailing the call"),
    # ...and the forms that must NOT excuse a call.
    ('// OLO_TEMP_DIR_OK:\nauto p = fs::temp_directory_path();\n',
     1, "marker with no reason"),
    ('const char* s = "OLO_TEMP_DIR_OK: nope";\nauto p = fs::temp_directory_path();\n',
     1, "marker inside a string literal"),
    ('int OLO_TEMP_DIR_OK = 0;\nauto p = fs::temp_directory_path();\n',
     1, "marker as an identifier, not a comment"),
    ('// OLO_TEMP_DIR_OK: reason\n\nauto p = fs::temp_directory_path();\n',
     1, "marker two lines above the call"),
]


def self_test() -> int:
    """Guard the guard. Runs on every invocation — it costs microseconds, and a
    silently-broken checker is worse than no checker."""
    failures = 0
    for source, expected, description in SELF_TEST_CASES:
        got = len(scan(source, pathlib.PurePosixPath("<self-test>")))
        if got != expected:
            failures += 1
            print(
                f"SELF-TEST FAILED ({description}): expected {expected} offender(s), got {got}\n"
                f"  source: {source!r}",
                file=sys.stderr,
            )
    return failures


def main() -> int:
    if self_test():
        print(
            "\ncheck_temp_dir_isolation.py is not working correctly; fix it before\n"
            "trusting its verdict on the test tree.",
            file=sys.stderr,
        )
        return 2

    offenders = []

    for path in sorted(TEST_ROOT.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".inl"):
            continue
        rel = pathlib.PurePosixPath(path.relative_to(TEST_ROOT).as_posix())
        if rel in ALLOWED:
            continue

        offenders.extend(scan(path.read_text(encoding="utf-8", errors="replace"), rel))

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
