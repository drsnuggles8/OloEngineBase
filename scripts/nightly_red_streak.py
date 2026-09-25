#!/usr/bin/env python3
"""Open, update or close one GitHub issue that tracks a scheduled workflow's red streak.

Issue #1473: the AMD GPU nightly failed every scheduled run for 20 nights and nobody
noticed, because a nightly that is always red stops being read. A failed scheduled
run notifies nobody who is not already watching the Actions tab. An open issue shows
up in the tracker, in `/start-work`'s ranking (it carries an olo-score block) and in
the owner's notifications.

Run by the `red-streak` job of .github/workflows/gpu-conformance-amd.yml, after the
hardware job, on a hosted runner: the self-hosted job keeps its read-only token.

    python3 scripts/nightly_red_streak.py --workflow gpu-conformance-amd.yml \
        --result <needs.X.result> --run-id <this run> [--threshold 2] [--dry-run]

What it does, with the streak counted over SCHEDULED runs of the default branch,
newest first, this run included:

  * streak >= threshold -> open the issue, or rename and comment on the open one;
  * this run succeeded  -> comment "green again" on the open issue and close it;
  * anything else       -> nothing.

A cancelled scheduled run neither breaks nor extends a streak: the workflow's
concurrency group cancels a run the next night's supersedes, which says nothing about
the build. `--dry-run` prints the decision and touches nothing; a workflow_dispatch
runs it that way, so the mechanism can be exercised without writing to the tracker.

Needs `gh` on PATH, authenticated through GH_TOKEN (actions: read, issues: write).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

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


def failed_steps(repo: str, run_id: int) -> str:
    try:
        jobs = gh_json("api", f"repos/{repo}/actions/runs/{run_id}/jobs")
    except RuntimeError:
        return "?"
    names = [step["name"] for job in jobs.get("jobs", []) for step in job.get("steps", []) if step.get("conclusion") == "failure"]
    return ", ".join(names) or "no failed step recorded (runner lost, or cancelled mid-job)"


def streak(runs: list[dict], this_run_id: int, this_result: str) -> tuple[list[dict], dict | None]:
    """The unbroken run of failures ending now, and the last success before it."""
    red: list[dict] = []
    if this_result == "failure":
        red.append({"id": this_run_id, "created_at": "tonight", "html_url": None})
    elif this_result == "success":
        return [], None
    for run in runs:
        if run["id"] == this_run_id or run.get("status") != "completed":
            continue
        conclusion = run.get("conclusion")
        if conclusion == "failure":
            red.append(run)
        elif conclusion == "success":
            return red, run
        # cancelled / skipped / timed_out-less states: neither breaks nor extends.
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
    parser.add_argument("--result", required=True, help="this run's job result (needs.<job>.result)")
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--threshold", type=int, default=2)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not args.repo:
        parser.error("--repo or GITHUB_REPOSITORY is required")

    if args.result not in ("success", "failure"):
        # Cancelled (superseded by the next night) or skipped: says nothing about the build.
        print(f"this run's result is {args.result!r}: neither red nor green, nothing to do")
        return 0

    red, last_green = streak(scheduled_runs(args.repo, args.workflow), args.run_id, args.result)
    since = last_green["created_at"] if last_green else "before the oldest run listed (60 nights)"
    if args.result == "failure":
        print(f"{args.workflow}: this run failed; {len(red)} consecutive red scheduled night(s); "
              f"last green scheduled run: {since}")

    issue = open_issue(args.repo, args.workflow)
    prefix = "[dry run] would" if args.dry_run else "will"

    if args.result == "success":
        if issue is None:
            print("green, no open red-streak issue: nothing to do")
            return 0
        print(f"{prefix} comment 'green again' on #{issue['number']} and close it")
        if not args.dry_run:
            gh("issue", "comment", str(issue["number"]), "--repo", args.repo,
               "--body", f"Green again: {run_url(args.repo, args.run_id)}. Closing; the next red streak reopens a new issue.")
            gh("issue", "close", str(issue["number"]), "--repo", args.repo)
        return 0

    if len(red) < args.threshold:
        print(f"streak {len(red)} < threshold {args.threshold}: nothing to do")
        return 0

    title = f"CI: {args.workflow} scheduled run has been red {len(red)} nights in a row"
    tonight = f"{run_url(args.repo, args.run_id)} ({args.result}): {failed_steps(args.repo, args.run_id)}"
    if issue is not None:
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
        f"The scheduled run of `{args.workflow}` has failed {len(red)} nights in a row "
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
