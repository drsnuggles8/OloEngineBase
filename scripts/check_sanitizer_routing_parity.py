#!/usr/bin/env python3
"""Prove the Linux sanitizer routing predicates have not drifted apart (#1219).

WHY THIS EXISTS. `asan.yml` decides where each Linux sanitizer arm runs in TWO
places that must agree:

  * the arm's own `runs-on` expression, which picks the runner, and
  * the matching `<ARM>_SELF_HOSTED` env var on `detect-changes.linuxplan`, which
    sizes that arm's ctest shard matrix and decides whether the build job stages
    an artifact at all.

`runs-on` cannot read a `needs` output from the job that produces it, so the
predicate is written twice on purpose. When the two disagree the run does not
fail loudly: a job routed to the box while the plan says "hosted, 4 shards"
stages a tree for shards it will not get, and one routed hosted while the plan
says "self-hosted, 1 shard" runs the whole suite in a job whose siblings are
already running it. Both look green for a while.

It has drifted once already and got away with it: `github.event_name != 'push'`
was added to the three jobs in #1084 and not to the plan (4c2ddafb1,
2026-09-11). That copy was only ACCIDENTALLY harmless -- on a `push` the jobs
are skipped at their own `if`, so nothing read the wrong plan.

#1219 made the three arms route DIFFERENTLY -- UBSan and TSan hosted, ASan +
LSan on the box -- so there are three pairs to keep honest now instead of one,
and "they are all the same string" is no longer a check a reader can do by eye.
"""

from __future__ import annotations

import io
import os
import re
import sys

import yaml

# arm key -> (job id, `linuxplan` env var name)
ARMS = {
    "asan-lsan": ("asan-lsan-linux", "ASAN_LSAN_SELF_HOSTED"),
    "ubsan": ("ubsan-linux", "UBSAN_SELF_HOSTED"),
    "tsan": ("tsan-linux", "TSAN_SELF_HOSTED"),
}

# The tail `runs-on` carries and the plan does not: the predicate's TRUE branch
# (the self-hosted label set) and its FALSE branch (the hosted image). Everything
# before it is the predicate itself, and that is what has to match.
RUNS_ON_TAIL = (
    "&& fromJSON('[\"self-hosted\",\"linux\",\"x64\",\"olo-ci\"]') "
    "|| 'ubuntu-24.04'"
)

WORKFLOW = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    ".github", "workflows", "asan.yml",
)


def unwrap(expr: str, where: str) -> str:
    """Strip the `${{ ... }}` wrapper and collapse the line folding to one space."""
    text = " ".join(expr.split())
    if not (text.startswith("${{") and text.endswith("}}")):
        sys.exit("%s: expected a `${{ ... }}` expression, got: %s" % (where, text))
    return text[3:-2].strip()


def main() -> int:
    with io.open(WORKFLOW, "r", encoding="utf-8") as handle:
        doc = yaml.safe_load(handle)

    jobs = doc["jobs"]
    detect = jobs["detect-changes"]

    plan = None
    for step in detect["steps"]:
        if step.get("id") == "linuxplan":
            plan = step
            break
    if plan is None:
        sys.exit("detect-changes has no step with `id: linuxplan` — routing plan is gone.")

    plan_env = plan.get("env") or {}
    problems = []

    for arm, (job_id, env_name) in sorted(ARMS.items()):
        if job_id not in jobs:
            problems.append("job `%s` (arm %s) is missing from the workflow." % (job_id, arm))
            continue
        if env_name not in plan_env:
            problems.append(
                "`linuxplan` has no `%s` env var, so arm %s's shard plan is "
                "computed from nothing." % (env_name, arm)
            )
            continue

        runs_on = unwrap(str(jobs[job_id]["runs-on"]), "%s.runs-on" % job_id)
        if not runs_on.endswith(RUNS_ON_TAIL):
            problems.append(
                "%s.runs-on no longer ends in the `olo-ci` / `ubuntu-24.04` "
                "branch this check knows how to strip.\n    got tail: ...%s"
                % (job_id, runs_on[-90:])
            )
            continue
        predicate = runs_on[: -len(RUNS_ON_TAIL)].strip()
        planned = unwrap(str(plan_env[env_name]), "linuxplan.env.%s" % env_name)

        if predicate != planned:
            problems.append(
                "arm %s: `%s.runs-on` and `linuxplan.env.%s` disagree.\n"
                "    runs-on : %s\n"
                "    plan    : %s" % (arm, job_id, env_name, predicate, planned)
            )

        # The plan reads three outputs per arm; a renamed arm silently gives every
        # consumer an empty string, which compares unequal to '1' and shards a
        # self-hosted job onto a matrix the box cannot absorb.
        for suffix in ("self-hosted", "shards", "shard-matrix"):
            key = "%s-%s" % (arm, suffix)
            if key not in (detect.get("outputs") or {}):
                problems.append("detect-changes does not output `%s`." % key)

    # Nothing may still read the pre-#1219 single-plan outputs.
    with io.open(WORKFLOW, "r", encoding="utf-8") as handle:
        raw = handle.read()
    for stale in re.findall(r"outputs\.linux-(?:shards|shard-matrix|self-hosted)", raw):
        problems.append(
            "`%s` is the pre-#1219 whole-workflow plan and no longer exists; "
            "read the per-arm output instead." % stale
        )

    if problems:
        sys.stderr.write("Linux sanitizer routing has drifted:\n\n")
        for problem in problems:
            sys.stderr.write("  * %s\n" % problem)
        sys.stderr.write(
            "\nThe predicate is written twice per arm on purpose (see this "
            "script's docstring). Both copies move together or neither does.\n"
        )
        return 1

    print("Linux sanitizer routing parity OK for %d arms:" % len(ARMS))
    for arm, (job_id, env_name) in sorted(ARMS.items()):
        runs_on = unwrap(str(jobs[job_id]["runs-on"]), job_id)
        where = "self-hosted when the vars allow" if "olo-ci" in runs_on else "?"
        first = runs_on.split("&&")[0].strip()
        print("  %-10s %-14s gate: %s (%s)" % (arm, job_id, first, where))
    return 0


if __name__ == "__main__":
    sys.exit(main())
