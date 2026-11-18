#!/usr/bin/env python3
"""Open, update or close one GitHub issue that tracks a scheduled workflow's red streak.

Issue #1473: the AMD GPU nightly failed every scheduled run for 20 nights and nobody
noticed, because a nightly that is always red stops being read. A failed scheduled
run notifies nobody who is not already watching the Actions tab. An open issue shows
up in the tracker, in `/start-work`'s ranking (it carries an olo-score block) and in
the owner's notifications.

Run by the `red-streak` job of .github/workflows/gpu-conformance-amd.yml, after the
hardware job, on a hosted runner: the self-hosted job keeps its read-only token.
The `red-streak` job of .github/workflows/video-ffmpeg.yml runs it too (#1711), judging
a weekly run by its two matrix legs.

    python3 scripts/nightly_red_streak.py --workflow gpu-conformance-amd.yml \\
        --job "<hardware job name>" [--job "<another>"] --timeout-minutes <its timeout> \\
        --result <needs.X.result> --run-id <this run> [--threshold 2] [--dry-run]

A night is judged by the named jobs' conclusions, never the run's: the run's includes
this alert job, so a night where only the alert failed would otherwise count as red.
`success` is green, `skipped` is neither, and `cancelled` is red only when the job ran
to its `timeout-minutes` (see verdict()). Everything else is red. With several jobs a
night is red when any of them is red and green only when all of them are green (see
combine()). A neutral night neither breaks nor extends a streak: the streak counts
consecutive OBSERVED nights, and the usual neutral night is a run the concurrency
group cancelled because a dispatch arrived, which says nothing about whether the
nightly still fails.

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

Opened by scripts/nightly_red_streak.py (issue #1473). Stability 8: the workflow's coverage is off
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
    """Newest first. One page of 60 is two months of a nightly or over a year of a weekly,
    far past any threshold."""
    data = gh_json("api", f"repos/{repo}/actions/workflows/{workflow}/runs?event=schedule&per_page=60")
    return data.get("workflow_runs", [])


def run_jobs(repo: str, run_id: int, job_names: list[str]) -> dict[str, dict | None]:
    """Each named job's record in one run, None for a name the run has no job for. One API
    read per run, however many names. An API error raises: guessing from the run's own
    conclusion would let this alert job's result decide the night."""
    jobs = gh_json("api", f"repos/{repo}/actions/runs/{run_id}/jobs").get("jobs", [])
    return {name: next((job for job in jobs if job.get("name") == name), None) for name in job_names}


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


def failed_jobs_summary(jobs: dict[str, dict]) -> str:
    """One job: its failed steps, as before. Several: each job that did not succeed, by name."""
    if len(jobs) == 1:
        return failed_steps(next(iter(jobs.values())))
    parts = [f"{name}: {failed_steps(job)}" for name, job in jobs.items() if job.get("conclusion") != "success"]
    return "; ".join(parts) or "every job succeeded"


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


def combine(verdicts: list[str]) -> str:
    """One night judged by several jobs: red if any is red, green only if all are green.

    A neutral job beside a green one leaves the night neutral: a leg the concurrency
    group cancelled has not shown that the night is green.
    """
    if "red" in verdicts:
        return "red"
    if verdicts and all(v == "green" for v in verdicts):
        return "green"
    return "neutral"


def job_verdict(run: dict, job: dict | None, timeout_minutes: float) -> str:
    """One job of a past run. With no such job in the run (a startup failure, or a renamed
    job) a failed run is still red, but a successful one proves nothing about the job (it
    may hold only this alert job), so it is never read as green."""
    if job is not None:
        return verdict(job.get("conclusion"), job, timeout_minutes)
    night = verdict(run.get("conclusion"), None, timeout_minutes)
    return "neutral" if night == "green" else night


def streak(repo: str, runs: list[dict], this_run_id: int, job_names: list[str],
           timeout_minutes: float) -> tuple[list[dict], dict | None]:
    """The unbroken run of red nights ending with this (red) run, and the last green one."""
    red: list[dict] = [{"id": this_run_id, "created_at": "this run", "html_url": None}]
    for run in runs:
        if run["id"] == this_run_id or run.get("status") != "completed":
            continue
        night = combine([job_verdict(run, job, timeout_minutes)
                         for job in run_jobs(repo, run["id"], job_names).values()])
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
    parser.add_argument("--job", required=True, action="append",
                        help="display name of a job a night is judged by; repeat it for several")
    parser.add_argument("--result", required=True,
                        help="this run's needs.<job>.result, quoted in the issue; the verdict reads each job's record")
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--timeout-minutes", type=float, required=True,
                        help="that job's timeout-minutes: a cancelled job that ran this long timed out")
    parser.add_argument("--threshold", type=int, default=2)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not args.repo:
        parser.error("--repo or GITHUB_REPOSITORY is required")

    # The jobs have finished by the time this runs (`needs:`), so their records are final.
    # Each is judged by its own record: needs.<job>.result folds a matrix's legs into one
    # word and cannot say which leg ran into its timeout.
    tonight_jobs = run_jobs(args.repo, args.run_id, args.job)
    for name, job in tonight_jobs.items():
        if job is None:
            # This run certainly has the jobs the workflow names, so a miss means --job no
            # longer matches a display name: every night would be misjudged. Say so.
            print(f"::error::no job named {name!r} in run {args.run_id} -- update --job in the workflow")
            return 1
    tonight_verdict = combine([verdict(job.get("conclusion"), job, args.timeout_minutes)
                               for job in tonight_jobs.values()])
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
    since = last_green["created_at"] if last_green else "before the oldest of the 60 scheduled runs listed"
    print(f"{args.workflow}: this run {args.result}; {len(red)} red scheduled run(s) in a row; "
          f"last green scheduled run: {since}")

    if issue is None and len(red) < args.threshold:
        print(f"streak {len(red)} < threshold {args.threshold} and no open issue: nothing to do")
        return 0

    title = f"CI: {args.workflow} has been red on {len(red)} scheduled runs in a row"
    tonight = f"{run_url(args.repo, args.run_id)} ({args.result}): {failed_jobs_summary(tonight_jobs)}"
    if issue is not None:
        # Below the threshold too: an issue left open by a failed close must not go stale.
        print(f"{prefix} retitle #{issue['number']} to '{title}' and comment tonight's run")
        if not args.dry_run:
            gh("issue", "edit", str(issue["number"]), "--repo", args.repo, "--title", title)
            gh("issue", "comment", str(issue["number"]), "--repo", args.repo,
               "--body", f"Still red, scheduled run {len(red)} in a row: {tonight}")
        return 0

    rows = "\n".join(
        f"- {run['created_at']}: {run['html_url'] or run_url(args.repo, args.run_id)}"
        for run in red[:30]
    )
    body = (
        f"`{args.workflow}` has been red on {len(red)} scheduled runs in a row "
        f"(last green scheduled run: {since}).\n\n"
        f"This run: {tonight}\n\nThe streak, newest first:\n{rows}\n\n"
        "This issue is maintained by `scripts/nightly_red_streak.py` (#1473): it is retitled and "
        "commented on each red scheduled run, and closed by the first green one.\n\n"
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
