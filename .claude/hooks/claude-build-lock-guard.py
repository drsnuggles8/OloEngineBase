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
(``rg``, ``git``, ``Get-Process``, ...) is not inspected at all, and two literal
markers allow: ``OLO_NOT_A_BUILD`` for a command that merely mentions a build tool,
and ``OLO_BUILD_LOCK_OVERRIDE`` for a deliberate unlocked build. The second is
AUDITED to olo-build-metrics.jsonl and, by policy, needs the user's explicit
permission for that build. Keeping them apart matters: while they were one string,
the "merely mentions" wording and the "always allows" wording disagreed, and the
looser reading was taken by accident.

Exit code is always 0; the verdict travels in the JSON on stdout. A crash here
must not block the session, so anything unexpected falls through to "allow".
"""

import json
import os
import re
import sys
import time

LOCK_SCRIPT = ".claude/skills/run-oloengine/build-lock.ps1"

# THREE markers, because one string used to mean two very different things and the
# looser reading won by accident (2026-08-19: an agent ran a real unlocked build by
# putting OLO_BUILD_LOCK_BYPASS in a COMMENT — the check is a plain substring test,
# so a comment satisfies it).
#
#   OLO_NOT_A_BUILD          - "this only MENTIONS a build tool". Silent allow.
#   OLO_BUILD_LOCK_OVERRIDE  - "this really is a build and I am running it unlocked".
#                              Allowed, but AUDITED. Policy: requires the user's
#                              explicit permission for THAT build; a past grant does
#                              not carry forward.
#   OLO_BUILD_LOCK_BYPASS    - legacy, still honoured so no running session breaks.
#                              Audited when it wraps an actual build tool.
#
# None of this can *prevent* an override — the marker is text and the agent writes
# the command. What it buys is that the two cases stop being confusable, and that a
# genuine unlocked build becomes countable instead of invisible (it takes no lock, so
# it otherwise never reaches olo-build-metrics.jsonl and silently biases the very
# build-time data we collect to size admission control).
NOT_A_BUILD_MARKER = "OLO_NOT_A_BUILD"
OVERRIDE_MARKER = "OLO_BUILD_LOCK_OVERRIDE"
BYPASS_MARKER = "OLO_BUILD_LOCK_BYPASS"
METRICS_FILE = "olo-build-metrics.jsonl"

# A build tool only counts when it is in STATEMENT POSITION: at the start, or
# right after a separator, a call operator or an opening quote. A bare word match
# is not good enough -- `Get-Process -Name cl,link,MSBuild` is a diagnostic, and
# blocking it (which the first version of this did, within minutes) trains people
# to reach for the bypass marker, which is worse than the check not existing.
# A quote does NOT open a statement on its own: a line whose text merely BEGINS
# with a build word (`"msbuild alive: " + ...` in a diagnostic script) is not an
# invocation, and treating it as one is the second false positive this check has
# produced. A quote only counts where a command genuinely follows one -- after the
# call operator `&`, or after -Command / -c.
_STATEMENT = r"(?:^|[;|(){}\n]\s*|&+\s*[\"']?\s*|(?:-Command|-c)\s+[\"']?\s*)"
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

# Splits a command into shell statements. Used so the exemption below is decided
# per statement: `rg foo; cmake --build .` must NOT inherit rg's exemption.
STATEMENT_SPLIT = re.compile(r"\|\||&&|[;&|\n]")

# A heredoc body is DATA, not shell. Commit messages and docs routinely quote a
# build command (`git commit -F - <<'MSG' ... cmake --build ... MSG`), and reading
# that as an invocation blocks the commit describing the build system.
HEREDOC_START = re.compile(r"<<-?\s*(['\"]?)(\w+)\1")


def strip_heredocs(command):
    """Removes `<<WORD ... WORD` bodies so only actual shell text is classified."""
    while True:
        start = HEREDOC_START.search(command)
        if start is None:
            return command
        word = start.group(2)
        rest = command[start.end():]
        terminator = re.search(r"^[ \t]*" + re.escape(word) + r"[ \t]*$", rest, re.MULTILINE)
        consumed = terminator.end() if terminator else len(rest)
        command = command[: start.start()] + " " + command[start.end() + consumed :]

# Tools that never start a build, but whose arguments routinely name one
# (`rg "cmake --build" docs/`). The exemption applies only when EVERY statement
# in the command leads with one of these, so `cd x && rg ...` is still
# inspected, and so is `rg foo; cmake --build ...`.
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


def leads_with_non_building_tool(command):
    """True only when EVERY statement in the command leads with a non-builder."""
    statements = [s for s in STATEMENT_SPLIT.split(command) if s.strip()]
    if not statements:
        return False
    return all(first_token(s) in NON_BUILDING_TOOLS for s in statements)


def classify(command):
    """(decision, tool) where decision is 'allow', 'override', 'legacy' or 'deny'.

    Order matters: the build tool is detected FIRST, so an override marker around a
    command that does not actually build stays a silent allow rather than a spurious
    audit entry.
    """
    if not isinstance(command, str) or not command.strip():
        return ("allow", None)
    if LOCK_SCRIPT in command.replace("\\", "/"):
        return ("allow", None)          # properly wrapped — the normal path
    if NOT_A_BUILD_MARKER in command:
        return ("allow", None)

    shell = strip_heredocs(command)
    if leads_with_non_building_tool(shell):
        return ("allow", None)
    tool = matched_build_tool(shell)

    if tool is None:
        return ("allow", None)          # nothing build-like; markers are irrelevant
    if OVERRIDE_MARKER in command:
        return ("override", tool)
    if BYPASS_MARKER in command:
        return ("legacy", tool)         # honoured for compatibility, but recorded
    return ("deny", tool)


def verdict(command):
    """The build tool this command would start unlocked, or None to allow it."""
    decision, tool = classify(command)
    return tool if decision == "deny" else None


def audit(decision, tool, command):
    """Record an unlocked build next to the lock's own metrics. Never raises.

    Best-effort by design: an audit failure must not block a tool call, and git is
    only invoked on this rare path so the common case adds no latency.
    """
    try:
        import subprocess

        common = subprocess.run(
            ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
            capture_output=True, text=True, timeout=10,
        )
        if common.returncode != 0 or not common.stdout.strip():
            return
        path = os.path.join(common.stdout.strip(), METRICS_FILE)
        record = {
            "event": "unlocked_build",
            "decision": decision,
            "tool": tool,
            "cwd": os.getcwd(),
            "command": command[:500],
            "ts": __import__("datetime").datetime.now().astimezone().isoformat(),
        }
        line = json.dumps(record) + "\n"
        # Retry, matching Write-BuildMetric's 5 x 50 ms shape in build-lock.ps1. Both
        # writers append to the SAME file, and the PowerShell side holds it with
        # FileShare.Read -- which excludes other WRITERS -- so a concurrent build-lock
        # append makes this open fail outright. Without the retry the audit record is
        # silently dropped, and a dropped record defeats the point of auditing at all.
        # Still gives up quietly after all attempts: an audit must never fail a tool call.
        for _ in range(5):
            try:
                with open(path, "a", encoding="utf-8") as handle:
                    handle.write(line)
                return
            except OSError:
                time.sleep(0.05)
    except Exception:
        return


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
    # A non-building leader must not exempt the statements after it.
    ('rg -n "cmake --build" docs/; cmake --build build --parallel 6', True),
    ("git log --oneline -5 && ninja -j6", True),
    ("pwsh -NoProfile -File " + LOCK_SCRIPT + " -Command 'cmake --build build'", False),
    ("pwsh -NoProfile -File " + LOCK_SCRIPT.replace("/", "\\") + " -Command 'ninja -j6'", False),
    ('rg -n "cmake --build" docs/', False),
    ("Get-Process -Name cl,link,MSBuild -ErrorAction SilentlyContinue", False),
    ("$log = 'x'; @(Get-Process -Name cl,link,MSBuild).Count", False),
    ("Get-Content build/build.ninja", False),
    ("Test-Path build/build.ninja", False),
    ("Get-Item build/build.ninja | Select-Object Length", False),
    ("tasklist | findstr MSBuild", False),
    # Diagnostics whose OUTPUT text starts with a build word. Both of these were
    # blocked by earlier versions of this check.
    ('"msbuild alive: " + @(Get-Process -Name MSBuild).Count', False),
    ('$n = 1\n"ninja processes: " + $n', False),
    ('Write-Output "cmake --build is the wrapped form"', False),
    # A heredoc body is data. A commit message describing the build system must
    # not read as running one.
    ("git commit -F - <<'MSG'\nfix: rg foo; cmake --build . now blocked\nMSG", False),
    # ...but a heredoc must not become a hiding place either: shell AFTER the
    # terminator is still shell.
    ("git commit -F - <<'MSG'\nmessage\nMSG\ncmake --build build --parallel 6", True),
    ("cmake --preset msvc", False),
    ("cmake --version", False),
    ("git log --oneline -5", False),
    ("build/OloEngine/tests/Debug/OloEngine-Tests.exe --gtest_filter=VisualScript*", False),
    ("cmake --build build --parallel 6  # " + BYPASS_MARKER, False),
    ("cmake --build build --parallel 6  # " + NOT_A_BUILD_MARKER, False),
    ("cmake --build build --parallel 6  # " + OVERRIDE_MARKER, False),
    ("", False),
)

# The markers do not just allow/deny — they pick WHICH allow, and only two of the
# four decisions are audited. A marker on a command that does not build must stay a
# plain allow, or the audit log fills with noise and stops being read.
DECISION_CASES = (
    ("cmake --build build --parallel 6", "deny"),
    ("cmake --build build --parallel 6  # " + OVERRIDE_MARKER, "override"),
    ("cmake --build build --parallel 6  # " + BYPASS_MARKER, "legacy"),
    ("cmake --build build --parallel 6  # " + NOT_A_BUILD_MARKER, "allow"),
    ("pwsh -NoProfile -File " + LOCK_SCRIPT + " -Command 'cmake --build build'", "allow"),
    # Markers on a non-build must NOT be audited.
    ('rg -n "cmake --build" docs/  # ' + OVERRIDE_MARKER, "allow"),
    ('rg -n "cmake --build" docs/  # ' + BYPASS_MARKER, "allow"),
    ("Get-Process -Name cl,link,MSBuild", "allow"),
    ("", "allow"),
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
    for command, want in DECISION_CASES:
        got = classify(command)[0]
        if got != want:
            failures += 1
            print("FAIL decision want={0:9} got={1:9} | {2}".format(want, got, command or "<empty>"))
    total = len(SELF_TEST_CASES) + len(DECISION_CASES)
    print("{0} case(s), {1} failure(s)".format(total, failures))
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

    decision, tool = classify(command)
    if decision == "allow":
        return 0
    if decision in ("override", "legacy"):
        audit(decision, tool, command)
        return 0

    quoted = command.replace("'", "''")
    deny(
        "Blocked: this starts a build ({tool}) without the cross-worktree build mutex.\n"
        "One Debug build peaks at ~47 GiB of the 64 on this host and other worktrees build "
        "concurrently, so builds must serialise through {lock}.\n\n"
        "Re-run it as:\n"
        "  pwsh -NoProfile -File {lock} -Command '{quoted}'\n\n"
        "If this command does NOT actually build (it only mentions the word), add the literal "
        "marker {notabuild}, or use the Grep tool instead.\n\n"
        "If you deliberately want to run this build UNLOCKED, that needs the user's explicit "
        "permission for THIS build (a past grant does not carry forward) — then add the literal "
        "marker {override}, which allows it and records it to {metrics}.".format(
            tool=tool, lock=LOCK_SCRIPT, quoted=quoted,
            notabuild=NOT_A_BUILD_MARKER, override=OVERRIDE_MARKER, metrics=METRICS_FILE,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
