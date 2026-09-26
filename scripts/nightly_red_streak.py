#!/usr/bin/env python3
"""Open, update or close one GitHub issue that tracks a scheduled workflow's red streak.

Issue #1473: the AMD GPU nightly failed every scheduled run for 20 nights and nobody
noticed, because a nightly that is always red stops being read. A failed scheduled
run notifies nobody who is not already watching the Actions tab. An open issue shows
up in the tracker, in `/start-work`'s ranking (it carries an olo-score block) and in
the owner's notifications.

Run by the `red-streak` job of .github/workflows/gpu-conformance-amd.yml, after the
hardware job, on a hosted runner: the self-hosted job keeps its read-only token.

    python3 scripts/nightly_red_streak.py --workflow gpu-conformance-amd.yml \\
        --job "<hardware job name>" --timeout-minutes <its timeout> \\
        --result <needs.X.result> --run-id <this run> [--threshold 2] [--dry-run]

A night is judged by the HARDWARE job's conclusion, never the run's: the run's includes
this alert job, so a night where only the alert failed would otherwise count as red.
`success` is green, `skipped` is neither, and `cancelled` is red only when the job ran
to its `timeout-minutes` (see verdict()). Everything else is red. A neutral night
neither breaks nor extends a streak: the streak counts consecutive OBSERVED nights,
and the usual neutral night is a run the concurrency group cancelled because a
dispatch arrived, which says nothing about whether the nightly still fails.

What it does, with the streak counted over SCHEDULED runs, newest first, this run
included:

  * this run green   -> comment "green again" on the open issue and close it;
  * this run red     -> open the issue once the streak reaches the threshold, and
                        retitle and comment on an open one on every red night;
  * otherwise        -> nothing.

`--dry-run` prints the decision and touches nothing; a workflow_dispatch runs it that
way, so the mechanism can be exercised without writing to the tracker.

Needs `gh` on PATH, authenticated through GH_TOKEN (actions: read, issues: write).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime

LABEL = "ci-nightly-red"
SCORE_BLOCK = """<!-- olo-score:begin -->
## Score
```olo-score
capability: 3
craft: 2
stability: 8
decay: 5
effort: 2
confidence: 0.5
learning: 2
fun: 5
kano: table-stakes
blocked_by: []
blocks: []
```

Opened by scripts/nightly_red_streak.py (issue #1473). Stability 8: the nightly's coverage is off
while it is red. Confidence 0.5: the cause is unknown until someone reads the failing step.
<!-- olo-score:end -->"""


def gh(*args: str, stdin: str | None = None) -> str:
    completed = subprocess.run(["gh", *args], input=stdin, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"gh {' '.join(args)} failed: {completed.stderr.strip()}")
    return completed.stdout


def gh_json(*args: str):
    return json.loads(gh(*args) or "null")


def scheduled_runs(repo: str, workflow: str) -> list[dict]:
    """Newest first. One page of 60 is two months of nightlies, far past any threshold."""
    data = gh_json("api", f"repos/{repo}/actions/workflows/{workflow}/runs?event=schedule&per_page=60")
    return data.get("workflow_runs", [])


def hardware_job(repo: str, run_id: int, job_name: str) -> dict | None:
    """None only when the run has no such job. An API error raises: guessing from the run's
    own conclusion would let this alert job's result decide the night."""
    jobs = gh_json("api", f"repos/{repo}/actions/runs/{run_id}/jobs")
    return next((job for job in jobs.get("jobs", []) if job.get("name") == job_name), None)


def minutes_ran(job: dict) -> float:
    started, completed = job.get("started_at"), job.get("completed_at")
    if not started or not completed:
        return 0.0
    begin = datetime.fromisoformat(started.replace("Z", "+00:00"))
    end = datetime.fromisoformat(completed.replace("Z", "+00:00"))
    return (end - begin).total_seconds() / 60.0


def failed_steps(job: dict | None) -> str:
    if job is None:
        return "hardware job not found in the run"
    names = [step["name"] for step in job.get("steps", []) if step.get("conclusion") == "failure"]
    return ", ".join(names) or "no failed step recorded (runner lost, timed out or cancelled mid-job)"


def verdict(conclusion: str | None, job: dict | None, timeout_minutes: float) -> str:
    """green / red / neutral for one night.

    `cancelled` is the ambiguous one. The workflow's concurrency group cancels a run that a
    newer one supersedes (a dispatch during the nightly, or a nightly still queued when the
    next fires), which says nothing about the build: neutral. But a job that reaches its
    `timeout-minutes` is ALSO reported as cancelled, and a nightly that times out every
    night is a red streak. So a cancelled job that ran to within five minutes of its
    timeout is red.
    """
    if conclusion == "success":
        return "green"
    if conclusion in (None, "skipped", "neutral"):
        return "neutral"
    if conclusion == "cancelled":
        return "red" if job is not None and minutes_ran(job) >= timeout_minutes - 5 else "neutral"
    return "red"


def streak(repo: str, runs: list[dict], this_run_id: int, job_name: str, timeout_minutes: float) -> tuple[list[dict], dict | None]:
    """The unbroken run of red nights ending with this (red) run, and the last green one."""
    red: list[dict] = [{"id": this_run_id, "created_at": "tonight", "html_url": None}]
    for run in runs:
        if run["id"] == this_run_id or run.get("status") != "completed":
            continue
        job = hardware_job(repo, run["id"], job_name)
        if job is not None:
            night = verdict(job.get("conclusion"), job, timeout_minutes)
        else:
            # No hardware job in the run: a startup failure, or a renamed job. A failed
            # run is still red, but a successful one proves nothing about the hardware
            # (it may hold only this alert job), so it is never read as green.
            night = verdict(run.get("conclusion"), None, timeout_minutes)
            if night == "green":
                night = "neutral"
        if night == "red":
            red.append(run)
        elif night == "green":
            return red, run
    return red, None


def open_issue(repo: str, workflow: str) -> dict | None:
    issues = gh_json("api", f"repos/{repo}/issues?labels={LABEL}&state=open&per_page=100")
    for issue in issues or []:
        if workflow in (issue.get("body") or ""):
            return issue
    return None


def run_url(repo: str, run_id: int) -> str:
    return f"https://github.com/{repo}/actions/runs/{run_id}"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--workflow", required=True, help="workflow file name, e.g. gpu-conformance-amd.yml")
    parser.add_argument("--job", required=True, help="display name of the job a night is judged by")
    parser.add_argument("--result", required=True, help="this run's result for that job (needs.<job>.result)")
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--timeout-minutes", type=float, required=True,
                        help="that job's timeout-minutes: a cancelled job that ran this long timed out")
    parser.add_argument("--threshold", type=int, default=2)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not args.repo:
        parser.error("--repo or GITHUB_REPOSITORY is required")

    # The job has finished by the time this runs (`needs:`), so its record is final.
    tonight_job = hardware_job(args.repo, args.run_id, args.job)
    if tonight_job is None:
        # This run certainly has the job the workflow names, so a miss means --job no
        # longer matches its display name: every night would be misjudged. Say so.
        print(f"::error::no job named {args.job!r} in run {args.run_id} -- update --job in the workflow")
        return 1
    tonight_verdict = verdict(args.result, tonight_job, args.timeout_minutes)
    if tonight_verdict == "neutral":
        print(f"this run's result is {args.result!r}: neither red nor green, nothing to do")
        return 0

    issue = open_issue(args.repo, args.workflow)
    prefix = "[dry run] would" if args.dry_run else "will"

    if tonight_verdict == "green":
        if issue is None:
            print("green, no open red-streak issue: nothing to do")
            return 0
        print(f"{prefix} comment 'green again' on #{issue['number']} and close it")
        if not args.dry_run:
            gh("issue", "comment", str(issue["number"]), "--repo", args.repo,
               "--body", f"Green again: {run_url(args.repo, args.run_id)}. Closing; the next red streak opens a new issue.")
            gh("issue", "close", str(issue["number"]), "--repo", args.repo)
        return 0

    red, last_green = streak(args.repo, scheduled_runs(args.repo, args.workflow), args.run_id, args.job,
                             args.timeout_minutes)
    since = last_green["created_at"] if last_green else "before the oldest run listed (60 nights)"
    print(f"{args.workflow}: this run {args.result}; {len(red)} consecutive red scheduled night(s); "
          f"last green scheduled run: {since}")

    if issue is None and len(red) < args.threshold:
        print(f"streak {len(red)} < threshold {args.threshold} and no open issue: nothing to do")
        return 0

    title = f"CI: {args.workflow} scheduled run has been red {len(red)} nights in a row"
    tonight = f"{run_url(args.repo, args.run_id)} ({args.result}): {failed_steps(tonight_job)}"
    if issue is not None:
        # Below the threshold too: an issue left open by a failed close must not go stale.
        print(f"{prefix} retitle #{issue['number']} to '{title}' and comment tonight's run")
        if not args.dry_run:
            gh("issue", "edit", str(issue["number"]), "--repo", args.repo, "--title", title)
            gh("issue", "comment", str(issue["number"]), "--repo", args.repo,
               "--body", f"Still red, night {len(red)}: {tonight}")
        return 0

    rows = "\n".join(
        f"- {run['created_at']}: {run['html_url'] or run_url(args.repo, args.run_id)}"
        for run in red[:30]
    )
    body = (
        f"The scheduled run of `{args.workflow}` has been red {len(red)} nights in a row "
        f"(last green scheduled run: {since}).\n\n"
        f"Tonight: {tonight}\n\nThe streak, newest first:\n{rows}\n\n"
        "This issue is maintained by `scripts/nightly_red_streak.py` (#1473): it is retitled and "
        "commented each red night, and closed by the first green scheduled run.\n\n"
        f"{SCORE_BLOCK}\n"
    )
    print(f"{prefix} open an issue: '{title}'")
    if args.dry_run:
        print(body)
        return 0
    gh("label", "create", LABEL, "--repo", args.repo, "--color", "B60205",
       "--description", "A scheduled CI workflow is on a red streak", "--force")
    gh("issue", "create", "--repo", args.repo, "--title", title, "--label", LABEL, "--body-file", "-", stdin=body)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
