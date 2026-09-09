#!/usr/bin/env python3
"""PreToolUse guard: a push must not race the formatter that runs after the turn.

Wired up in ``.claude/settings.json`` as a ``PreToolUse`` hook on ``Bash`` and
``PowerShell``. Reads the tool call as JSON on stdin and denies a ``git push``
while ``pre-commit`` would still reformat something in the working tree.

WHY THIS EXISTS. The formatter runs from the ``Stop`` hook, i.e. at the END of a
turn. A session that edits, commits and pushes inside ONE turn therefore pushes
the unformatted text and only reformats afterwards, leaving the change sitting
in the working tree while CI fails the ``pre-commit`` job on the commit that was
already sent. That is not a hypothetical: it happened on PR #1160, where adding
one longer enumerator re-aligned a whole trailing-comment column.

The Stop hook cannot fix this — by construction it cannot run before the push
inside its own turn — so the check has to live on the push itself.

WHAT IT CHECKS. ``pre-commit run --all-files``, exactly what CI runs, and it is
the tree state that decides: if the run modifies files or fails, the push is
denied and the (now formatted) files are left staged-ready for the caller to
commit. A clean run allows the push. When ``pre-commit`` is not installed the
push is ALLOWED with no output — this guard exists to catch a race, not to add a
new hard dependency to every clone.

The marker ``OLO_PUSH_UNFORMATTED`` allows a push regardless. It is for the case
where the formatter and CI genuinely disagree; it is not a way to skip the check
because it is inconvenient, and it never bypasses ``--no-verify`` policy.

Exit code is always 0; the verdict travels in the JSON on stdout. A crash here
must not block the session, so anything unexpected falls through to "allow".
"""

import json
import os
import re
import shutil
import subprocess
import sys

OVERRIDE_MARKER = "OLO_PUSH_UNFORMATTED"

# `git push` in statement position: start of the command, or after a separator.
# `git --no-pager push`, `git -C <dir> push` and friends all still match.
PUSH_RE = re.compile(r"(?:^|[;&|]|\b(?:and|then)\b)\s*git\b[^;&|]*\bpush\b", re.IGNORECASE)


def emit(decision, reason=None):
    out = {"hookSpecificOutput": {"hookEventName": "PreToolUse"}}
    if decision == "deny":
        out["hookSpecificOutput"]["permissionDecision"] = "deny"
        out["hookSpecificOutput"]["permissionDecisionReason"] = reason
    sys.stdout.write(json.dumps(out))


def allow():
    emit("allow")
    return 0


def deny(reason):
    emit("deny", reason)
    return 0


def main():
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return allow()

    tool = payload.get("tool_name") or ""
    if tool not in ("Bash", "PowerShell"):
        return allow()

    command = (payload.get("tool_input") or {}).get("command") or ""
    if not command or not PUSH_RE.search(command):
        return allow()
    if OVERRIDE_MARKER in command:
        return allow()

    project = os.environ.get("CLAUDE_PROJECT_DIR") or "."
    if shutil.which("pre-commit") is None:
        return allow()

    try:
        result = subprocess.run(
            ["pre-commit", "run", "--all-files"],
            cwd=project, capture_output=True, text=True, timeout=600,
        )
    except Exception:
        # A guard that cannot run must not wedge the session.
        return allow()

    if result.returncode == 0:
        return allow()

    tail = "\n".join((result.stdout or "").strip().splitlines()[-25:])
    return deny(
        "Blocked: pre-commit is not clean, so this push would send text the formatter "
        "is about to change.\n\n"
        "The formatter runs from the Stop hook, i.e. AFTER this turn — so a push made "
        "inside the same turn as the edits races it, CI fails the pre-commit job on the "
        "commit you already sent, and the reformat is left sitting in your working tree.\n\n"
        "pre-commit has now been run for you and any fixes are in the working tree. "
        "Re-stage them, amend or add a commit, then push again:\n"
        "  git add -u && git commit --amend --no-edit   (if the commit is NOT yet pushed)\n\n"
        "pre-commit output:\n" + tail + "\n\n"
        "If the formatter and CI genuinely disagree, add the literal marker "
        + OVERRIDE_MARKER + " to the command."
    )


if __name__ == "__main__":
    sys.exit(main())
