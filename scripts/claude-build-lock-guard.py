#!/usr/bin/env python3
"""PreToolUse guard: every build goes through the cross-worktree build mutex.

Wired up in ``.claude/settings.json`` as a ``PreToolUse`` hook on ``Bash`` and
``PowerShell``. Reads the tool call as JSON on stdin and denies it when it would
start a build without ``.claude/skills/run-oloengine/build-lock.ps1``.

Why a hook and not a permission rule: a ``deny`` permission rule degrades to a
prompt, and an unattended session in auto/accept mode sails straight through it.
Hooks run regardless of permission mode, so this is the only place the rule can
actually be enforced rather than merely suggested. One clean Debug build peaks at
~47 GiB of the 64 on this host (issue #759), so a second concurrent build is not
a slowdown, it is an OOM.

The check is biased towards blocking -- a false negative costs a wedged machine,
a false positive costs one re-issued tool call -- but not blindly: a build tool
counts only in statement position, a command whose first token never builds
(``rg``, ``git``, ``Get-Process``, ...) is not inspected at all, and the literal
marker ``OLO_BUILD_LOCK_BYPASS`` always allows, so an override is explicit and
greppable rather than silent.

Exit code is always 0; the verdict travels in the JSON on stdout. A crash here
must not block the session, so anything unexpected falls through to "allow".
"""

import json
import os
import re
import sys

LOCK_SCRIPT = ".claude/skills/run-oloengine/build-lock.ps1"
BYPASS_MARKER = "OLO_BUILD_LOCK_BYPASS"

# A build tool only counts when it is in STATEMENT POSITION: at the start, or
# right after a separator, a call operator or an opening quote. A bare word match
# is not good enough -- `Get-Process -Name cl,link,MSBuild` is a diagnostic, and
# blocking it (which the first version of this did, within minutes) trains people
# to reach for the bypass marker, which is worse than the check not existing.
_STATEMENT = r"(?:^|[;&|(){}\n\"'])\s*"
_PATH = r"(?:[A-Za-z]:)?(?:[\w.\-]*[\\/])*"

# `cmake` alone is not enough -- `cmake --preset msvc` only configures, and
# configuring is cheap.
BUILD_PATTERNS = (
    (
        "cmake --build",
        re.compile(
            _STATEMENT + _PATH + r"cmake(?:\.exe)?(?![\w.\-])[^\r\n]*?--build(?![\w.\-])",
            re.IGNORECASE,
        ),
    ),
    ("ninja", re.compile(_STATEMENT + _PATH + r"ninja(?:\.exe)?(?![\w.\-])", re.IGNORECASE)),
    ("msbuild", re.compile(_STATEMENT + _PATH + r"msbuild(?:\.exe)?(?![\w.\-])", re.IGNORECASE)),
)

# Tools that never start a build, but whose arguments routinely name one
# (`rg "cmake --build" docs/`). Matched on the FIRST token only, so
# `cd x && rg ...` is still inspected -- and so is `rg foo; cmake --build ...`.
NON_BUILDING_TOOLS = frozenset(
    {
        "rg",
        "grep",
        "egrep",
        "fgrep",
        "findstr",
        "select-string",
        "sls",
        "cat",
        "get-content",
        "gc",
        "head",
        "tail",
        "less",
        "more",
        "sed",
        "awk",
        "echo",
        "write-host",
        "write-output",
        "git",
        "gh",
        "get-process",
        "stop-process",
        "wait-process",
        "tasklist",
        "taskkill",
    }
)


def first_token(command):
    """Lowercased basename of the command's first word, minus quotes and .exe."""
    stripped = command.lstrip().lstrip("&").lstrip()
    word = stripped.split(None, 1)[0] if stripped.split(None, 1) else ""
    word = word.strip("\"'")
    word = re.split(r"[/\\]", word)[-1]
    if word.lower().endswith(".exe"):
        word = word[: -len(".exe")]
    return word.lower()


def matched_build_tool(command):
    for label, pattern in BUILD_PATTERNS:
        if pattern.search(command):
            return label
    return None


def verdict(command):
    """The build tool this command would start unlocked, or None to allow it."""
    if not isinstance(command, str) or not command.strip():
        return None
    if LOCK_SCRIPT in command.replace("\\", "/"):
        return None
    if BYPASS_MARKER in command:
        return None
    if first_token(command) in NON_BUILDING_TOOLS:
        return None
    return matched_build_tool(command)


def deny(reason):
    json.dump(
        {
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "deny",
                "permissionDecisionReason": reason,
            }
        },
        sys.stdout,
    )


# Run with --selftest. Every entry here is a shape that has actually turned up in
# a session; the "allow" half is the important half, because a guard that blocks
# ordinary diagnostics gets bypassed rather than fixed.
SELF_TEST_CASES = (
    ("cmake --build build --target OloEngine-Tests --config Debug --parallel 6", True),
    ("cd d:/repos/x && cmake --build build --config Debug", True),
    ('pwsh -NoProfile -Command "cmake --build build --parallel 6"', True),
    ("ninja -j6", True),
    ("./ninja -j6", True),
    ("cd build-clang; ninja -j6", True),
    ("msbuild OloEngine.sln /p:Configuration=Debug", True),
    ("& 'D:\\tools\\ninja.exe' -j6", True),
    ("CMAKE --BUILD build", True),
    ("pwsh -NoProfile -File " + LOCK_SCRIPT + " -Command 'cmake --build build'", False),
    ("pwsh -NoProfile -File " + LOCK_SCRIPT.replace("/", "\\") + " -Command 'ninja -j6'", False),
    ('rg -n "cmake --build" docs/', False),
    ("Get-Process -Name cl,link,MSBuild -ErrorAction SilentlyContinue", False),
    ("$log = 'x'; @(Get-Process -Name cl,link,MSBuild).Count", False),
    ("Get-Content build/build.ninja", False),
    ("Test-Path build/build.ninja", False),
    ("Get-Item build/build.ninja | Select-Object Length", False),
    ("tasklist | findstr MSBuild", False),
    ("cmake --preset msvc", False),
    ("cmake --version", False),
    ("git log --oneline -5", False),
    ("build/OloEngine/tests/Debug/OloEngine-Tests.exe --gtest_filter=VisualScript*", False),
    ("cmake --build build --parallel 6  # " + BYPASS_MARKER, False),
    ("", False),
)


def self_test():
    failures = 0
    for command, should_block in SELF_TEST_CASES:
        blocked = verdict(command) is not None
        if blocked != should_block:
            failures += 1
            print("FAIL want={0:5} got={1:5} | {2}".format(
                "deny" if should_block else "allow",
                "deny" if blocked else "allow",
                command or "<empty>",
            ))
    print("{0} case(s), {1} failure(s)".format(len(SELF_TEST_CASES), failures))
    return 1 if failures else 0


def main():
    if "--selftest" in sys.argv[1:]:
        return self_test()

    try:
        payload = json.load(sys.stdin)
    except (ValueError, OSError):
        return 0

    if not isinstance(payload, dict):
        return 0
    if os.environ.get(BYPASS_MARKER):
        return 0

    tool_input = payload.get("tool_input")
    command = tool_input.get("command", "") if isinstance(tool_input, dict) else ""

    tool = verdict(command)
    if tool is None:
        return 0

    quoted = command.replace("'", "''")
    deny(
        "Blocked: this starts a build ({tool}) without the cross-worktree build mutex.\n"
        "One Debug build peaks at ~47 GiB of the 64 on this host and other worktrees build "
        "concurrently, so builds must serialise through {lock}.\n\n"
        "Re-run it as:\n"
        "  pwsh -NoProfile -File {lock} -Command '{quoted}'\n\n"
        "If this command does not actually build (it only mentions the word), use the Grep "
        "tool instead, or add the literal marker {marker} to the command line.".format(
            tool=tool, lock=LOCK_SCRIPT, quoted=quoted, marker=BYPASS_MARKER
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
