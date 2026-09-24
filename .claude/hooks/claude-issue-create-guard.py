#!/usr/bin/env python3
"""PreToolUse guard: a task worktree files an issue only with a reason it did not fix the bug.

Wired up in ``.claude/settings.json`` as a ``PreToolUse`` hook on ``Bash`` and
``PowerShell``. Reads the tool call as JSON on stdin and denies ``gh issue create``
from a task worktree when the issue body lacks either of:

- the line ``Not fixed in <#PR or branch> because: <reason> — <fact>``, where
  <reason> is one of the four in docs/process/task-loop.md Phase 1a
  (needs a decision, needs access, too big, owned elsewhere);
- an ``olo-score`` block (docs/process/issue-scoring.md §5). ``gh issue create
  --body`` bypasses the issue templates, and an unscored issue is invisible to
  ``/start-work``.

WHY THIS EXISTS. Worker sessions filed bugs they had just proven instead of fixing
them (#1421, #1422, #1431, #1439-#1441 in two days), because the old rule let
"widens the diff" justify a follow-up and filing looks rigorous. The task loop now
says fix-by-default; this makes the exceptions state their reason where it counts.

SCOPE. Only a task worktree is checked: the project dir has a ``HANDOVER.md`` and
the branch is ``feature/*``. Filing from the base repo or an ad-hoc session is not
this guard's business.

WHAT IT READS. The command text itself (covers ``--body "..."`` and a heredoc fed
to ``--body-file -``) plus the file named by ``--body-file`` / ``-F`` when it can be
read. The marker ``OLO_ISSUE_NOT_A_FINDING`` allows an issue that is not a bug found
on the way (a feature idea the user asked for, a tracker).

Exit code is always 0; the verdict travels in the JSON on stdout. Anything
unexpected falls through to "allow": a crash here must not wedge the session.
"""

import json
import os
import re
import shlex
import subprocess
import sys

OVERRIDE_MARKER = "OLO_ISSUE_NOT_A_FINDING"

CREATE_RE = re.compile(r"(?:^|[;&|(]|\b(?:and|then)\b)\s*gh\b[^;&|]*\bissue\s+create\b", re.IGNORECASE)
REASON_RE = re.compile(
    r"Not fixed in\b[^\n]{0,120}?\bbecause:\s*(needs a decision|needs access|too big|owned elsewhere)\b",
    re.IGNORECASE,
)
SCORE_RE = re.compile(r"```olo-score\b")
BODY_FILE_RE = re.compile(r"(?:--body-file|(?<![\w-])-F)(?:=|\s+)(\"[^\"]+\"|'[^']+'|\S+)")


def emit(decision, reason=None):
    out = {"hookSpecificOutput": {"hookEventName": "PreToolUse"}}
    if decision == "deny":
        out["hookSpecificOutput"]["permissionDecision"] = "deny"
        out["hookSpecificOutput"]["permissionDecisionReason"] = reason
    sys.stdout.write(json.dumps(out))
    return 0


def in_task_worktree(project):
    if not os.path.isfile(os.path.join(project, "HANDOVER.md")):
        return False
    try:
        branch = subprocess.run(
            ["git", "-C", project, "symbolic-ref", "--short", "HEAD"],
            capture_output=True, text=True, timeout=10,
        ).stdout.strip()
    except Exception:
        return False
    return branch.startswith("feature/")


def body_file_text(command, cwd):
    texts = []
    for match in BODY_FILE_RE.finditer(command):
        raw = match.group(1)
        try:
            path = shlex.split(raw)[0]
        except ValueError:
            path = raw.strip("\"'")
        if path == "-":
            continue
        if not os.path.isabs(path):
            path = os.path.join(cwd, path)
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                texts.append(handle.read())
        except OSError:
            continue
    return "\n".join(texts)


def main():
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return emit("allow")

    if (payload.get("tool_name") or "") not in ("Bash", "PowerShell"):
        return emit("allow")
    command = (payload.get("tool_input") or {}).get("command") or ""
    if not command or not CREATE_RE.search(command) or OVERRIDE_MARKER in command:
        return emit("allow")

    project = os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or "."
    if not in_task_worktree(project):
        return emit("allow")

    text = command + "\n" + body_file_text(command, payload.get("cwd") or project)
    missing = []
    if not REASON_RE.search(text):
        missing.append(
            "the line `Not fixed in #<PR or branch> because: <needs a decision | needs access | "
            "too big | owned elsewhere> — <the fact>`"
        )
    if not SCORE_RE.search(text):
        missing.append("an ```olo-score block (docs/process/issue-scoring.md §5, scored against the §4 anchors)")
    if not missing:
        return emit("allow")

    return emit(
        "deny",
        "Blocked: this task worktree is filing an issue, and the body is missing "
        + " and ".join(missing) + ".\n\n"
        "docs/process/task-loop.md Phase 1a: a bug you find on the way is FIXED here, in its own "
        "commit, and listed under 'Found and fixed' in the PR body. File it only when the fix needs a "
        "decision from the user, needs access this session lacks, is bigger than the task itself, or "
        "touches files a sibling worktree owns. 'It widens the diff' and 'it is out of scope' are not "
        "reasons.\n\n"
        "If one of the four reasons holds, add the line and the score block and file again. If this "
        "issue is not a bug found on the way (e.g. the user asked for a tracker), add the literal "
        "marker " + OVERRIDE_MARKER + " to the command.",
    )


if __name__ == "__main__":
    sys.exit(main())
