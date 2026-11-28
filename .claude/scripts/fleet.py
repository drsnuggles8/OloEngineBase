#!/usr/bin/env python3
"""Fleet state for the /orchestrate loop: every task worktree, its session and its PR, and
what changed since the last look. The orchestrator model reads the output; the script does
the gathering so a quiet tick costs one command, not a dozen queries or peer messages.

Subcommands:
  status [--json] [--no-save]        snapshot now, print the EVENTS since the last snapshot
                                     and the STANDING items that still need someone
  launch <slug> [--dry-run]          start a background worker session in a standby worktree
         [--ignore-hold]
  hold <slug> --after-pr N [N ...]   record a start condition: launch only once these PRs merged
       [--note TEXT]                 (--note alone records a hold only a person can clear)
  release <slug>                     drop a hold
  goal                               print the kickoff /goal (single source: start-work SKILL.md)

State lives in <git-common-dir>/olo-fleet/, beside the build-lock files: never committed,
shared by every worktree of the repository.
  ledger.json    holds and launches (written by hold / release / launch)
  snapshot.json  the previous status, so the next one reports only what changed

Read-only except for those two files, `launch` (starts a session, and may write the
worktree's git-ignored .claude/settings.local.json, see ensure_mcp_approval) and nothing
else: it never merges, closes, pushes or deletes. Run `git fetch --prune origin` first, as
for worktree_status.py.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import worktree_status as ws  # noqa: E402  (the shared gate; same directory)

START_WORK_SKILL = Path(".claude/skills/start-work/SKILL.md")
LAUNCH_SETTLE_SECONDS = 60
QUIET_TICKS = 3  # idle with an unchanged worktree for this many ticks in a row = "quiet" (stuck?)
SUCCESS_CONCLUSIONS = {"SUCCESS"}
PENDING_STATES = {"QUEUED", "IN_PROGRESS", "PENDING", "EXPECTED", "WAITING", "REQUESTED"}
FAILED_CONCLUSIONS = {"FAILURE", "TIMED_OUT", "CANCELLED", "ERROR", "STARTUP_FAILURE", "ACTION_REQUIRED"}


# --- pure helpers (unit-tested in test_fleet.py) ------------------------------------------

def now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def slug_of(tree: dict) -> str:
    branch = tree.get("branch") or ""
    if branch.startswith("feature/"):
        return branch.removeprefix("feature/")
    return Path(tree["path"]).name


def parse_model_effort(handover: str) -> tuple[str, str] | None:
    """`**Opus 5.5, effort high**` under *Recommended model + effort* -> (claude-opus-5-5, high)."""
    match = re.search(
        r"\*\*\s*(Opus|Sonnet|Haiku|Fable)\s+(\d+(?:\.\d+)?)\s*,\s*effort\s+(low|medium|high|xhigh|max)\s*\*\*",
        handover,
        re.IGNORECASE,
    )
    if not match:
        return None
    family, version, effort = match.groups()
    return f"claude-{family.lower()}-{version.replace('.', '-')}", effort.lower()


def extract_goal(skill_text: str) -> str | None:
    """The one `/goal ...` line of start-work §5c: the kickoff has a single source."""
    lines = [line.strip() for line in skill_text.splitlines() if line.strip().startswith("/goal ")]
    return lines[0] if len(lines) == 1 else None


def summarize_checks(rollup: list[dict]) -> dict[str, list[str]]:
    """statusCheckRollup -> {success, pending, failed, other}: name lists."""
    out: dict[str, list[str]] = {"success": [], "pending": [], "failed": [], "other": []}
    for item in rollup or []:
        name = item.get("name") or item.get("context") or "?"
        status = (item.get("status") or "").upper()
        conclusion = (item.get("conclusion") or item.get("state") or "").upper()
        if status in PENDING_STATES or (item.get("__typename") == "CheckRun" and status != "COMPLETED"):
            out["pending"].append(name)
        elif conclusion in PENDING_STATES:
            out["pending"].append(name)
        elif conclusion in SUCCESS_CONCLUSIONS:
            out["success"].append(name)
        elif conclusion in FAILED_CONCLUSIONS:
            out["failed"].append(name)
        else:
            out["other"].append(f"{name}={conclusion or status or '?'}")  # NEUTRAL, SKIPPED, STALE
    for names in out.values():
        names.sort()
    return out


def gate_status(pr: dict) -> tuple[bool, list[str]]:
    """The machine-checkable half of the Phase 6 gate. The PR-body half (matrix, Found and
    fixed, Filed not fixed) is the worker's to show; this never claims it."""
    missing = []
    if pr.get("mergeable") != "MERGEABLE":
        missing.append(f"mergeable={pr.get('mergeable')}")
    checks = pr.get("checks", {})
    if checks.get("pending"):
        missing.append(f"{len(checks['pending'])} pending")
    if checks.get("failed"):
        missing.append("failed: " + ", ".join(checks["failed"]))
    if checks.get("other"):
        missing.append("not SUCCESS: " + ", ".join(checks["other"]))
    if pr.get("threads") is None:
        missing.append("thread count unknown")
    elif pr["threads"]:
        missing.append(f"{pr['threads']} unresolved thread(s)")
    return not missing, missing


def next_quiet_ticks(prev_record: dict | None, record: dict) -> int:
    """Count consecutive ticks a session sat idle while its worktree did not change at all.
    Idle alone is normal (a worker waits on a background build or CI); idle AND untouched for
    QUIET_TICKS ticks is what shwarm calls a quiet branch: ask the worker, don't act for it."""
    session = record.get("session") or {}
    if session.get("status") != "idle":
        return 0
    if not prev_record or prev_record.get("activity") != record.get("activity"):
        return 0
    return int(prev_record.get("quiet_ticks", 0)) + 1


def hold_cleared(hold: dict, merged: set[int]) -> bool:
    after = hold.get("after_prs") or []
    return bool(after) and all(n in merged for n in after)


def diff_events(prev: dict | None, cur: dict) -> list[str]:
    """Changes between two snapshots, one line each, for the orchestrator to act on."""
    if not prev:
        return ["baseline: first snapshot, nothing to compare (every STANDING item is current)"]
    events: list[str] = []
    pw, cw = prev.get("worktrees", {}), cur.get("worktrees", {})
    for slug in sorted(cw.keys() - pw.keys()):
        events.append(f"worktree-added {slug}: {cw[slug]['status']}")
    for slug in sorted(pw.keys() - cw.keys()):
        events.append(f"worktree-removed {slug}")
    for slug in sorted(cw.keys() & pw.keys()):
        a, b = pw[slug], cw[slug]
        if a["status"] != b["status"]:
            events.append(f"status {slug}: {a['status']} -> {b['status']}")
        sa, sb = (a.get("session") or {}).get("status"), (b.get("session") or {}).get("status")
        if sa != sb:
            events.append(f"session {slug}: {sa or 'none'} -> {sb or 'none'}")
    pp, cp = prev.get("prs", {}), cur.get("prs", {})
    for num in sorted(pp.keys() - cp.keys(), key=int):
        events.append(f"pr-left-open #{num} ({pp[num].get('branch')}): now {cur.get('closed_prs', {}).get(num, 'not open')}")
    for num in sorted(cp.keys() - pp.keys(), key=int):
        events.append(f"pr-opened #{num} ({cp[num].get('branch')})")
    for num in sorted(cp.keys() & pp.keys(), key=int):
        a, b = pp[num], cp[num]
        if a.get("head") != b.get("head"):
            events.append(f"pr-push #{num}: head {str(a.get('head'))[:9]} -> {str(b.get('head'))[:9]} (gate re-runs)")
        new_failed = sorted(set(b["checks"]["failed"]) - set(a["checks"]["failed"]))
        if new_failed:
            events.append(f"pr-check-failed #{num}: {', '.join(new_failed)}")
        if a.get("mergeable") != b.get("mergeable"):
            events.append(f"pr-mergeable #{num}: {a.get('mergeable')} -> {b.get('mergeable')}")
        if a.get("threads") != b.get("threads"):
            events.append(f"pr-threads #{num}: {a.get('threads')} -> {b.get('threads')}")
        if a.get("gate") != b.get("gate"):
            events.append(f"pr-gate #{num}: {'MET' if b.get('gate') else 'lost'}")
    for slug in sorted(set(cur.get("ready_to_launch", [])) - set(prev.get("ready_to_launch", []))):
        events.append(f"ready-to-launch {slug}: its hold cleared")
    if prev.get("builds_running") != cur.get("builds_running"):
        events.append(f"builds-running: {prev.get('builds_running')} -> {cur.get('builds_running')}")
    for issue in cur.get("new_issues", []):
        events.append(f"new-issue #{issue['number']}: {issue['title']}")
    return events


def standing_items(cur: dict, ledger: dict) -> list[str]:
    """What still needs someone right now, whether or not it changed."""
    items: list[str] = []
    for slug, tree in sorted(cur["worktrees"].items()):
        session = tree.get("session")
        if tree["status"] == "completed" and tree["kind"] != "detached":
            items.append(f"cleanup {slug}: completed ({tree.get('landed')}); /cleanup-worktree {slug}")
        if session and session.get("status") == "blocked":
            items.append(f"blocked {slug}: session '{session['name']}' waits on a prompt; `claude attach {session.get('id') or session['name']}`")
        if tree["status"] == "incomplete-standby" and not session:
            hold = ledger.get("holds", {}).get(slug)
            if slug in cur.get("ready_to_launch", []):
                items.append(f"launch {slug}: hold cleared; fleet.py launch {slug}")
            elif hold:
                why = ", ".join(f"#{n}" for n in hold.get("after_prs", [])) or hold.get("note", "held")
                items.append(f"held {slug}: waits for {why}")
            else:
                items.append(f"launch {slug}: standby, no hold; fleet.py launch {slug}")
        pr_num = tree.get("open_pr")
        pr = cur["prs"].get(str(pr_num)) if pr_num else None
        if pr and pr.get("gate"):
            items.append(f"gate {slug}: PR #{pr_num} machine gate met; the user merges (body half: ask the worker)")
        elif session and session.get("status") == "idle" and tree["status"] != "completed":
            quiet = tree.get("quiet_ticks", 0) >= QUIET_TICKS
            if pr and pr.get("mergeable") == "CONFLICTING":
                items.append(f"conflict {slug}: PR #{pr_num} conflicts with master and the session is idle")
            elif pr and not pr["checks"]["pending"] and quiet:
                items.append(f"quiet {slug}: idle {tree['quiet_ticks']} ticks, PR #{pr_num} not at the gate ({'; '.join(pr['gate_missing'])})")
            elif not pr and quiet:
                items.append(f"quiet {slug}: idle {tree['quiet_ticks']} ticks with an untouched worktree, no PR yet")
    for num, pr in sorted(cur["prs"].items(), key=lambda kv: int(kv[0])):
        if not pr.get("worktree"):
            items.append(f"pr-without-worktree #{num} ({pr['branch']})")
    return items


# --- gathering ------------------------------------------------------------------------------

def run_json(args: list[str], cwd: str | None = None) -> object:
    result = ws.run(args, cwd=cwd)
    return json.loads(result.stdout or "null")


def norm(path: str) -> str:
    return os.path.normcase(os.path.normpath(path))


def activity_marker(tree: ws.Worktree) -> str:
    """Changes whenever the worker commits or touches a changed file."""
    newest = 0
    for line in tree.wip_files:
        name = line[3:].strip().strip('"').split(" -> ")[-1]
        try:
            newest = max(newest, int((Path(tree.path) / name).stat().st_mtime))
        except OSError:
            pass
    return f"{tree.head[:12]}:{len(tree.wip_files)}:{newest}"


def common_dir(base: str) -> Path:
    out = ws.run(["git", "-C", base, "rev-parse", "--path-format=absolute", "--git-common-dir"]).stdout.strip()
    return Path(out)


def fleet_dir(base: str) -> Path:
    path = common_dir(base) / "olo-fleet"
    path.mkdir(exist_ok=True)
    return path


def load(path: Path, default: dict) -> dict:
    if not path.exists():
        return default
    return json.loads(path.read_text(encoding="utf-8"))


def save(path: Path, data: dict) -> None:
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    os.replace(tmp, path)


def claude_exe() -> str:
    exe = shutil.which("claude")
    if not exe:
        raise RuntimeError("`claude` is not on PATH: cannot list or launch sessions")
    return exe


def sessions() -> list[dict]:
    """Every live session on this machine, interactive and background (`claude agents --json`)."""
    data = run_json([claude_exe(), "agents", "--json"])
    return data if isinstance(data, list) else []


def builds_running(cdir: Path) -> int:
    """Build-lock slots held right now. A held slot is an exclusive handle, so opening it fails.
    Builds only: a GPU test sweep holds no slot, so 0 is necessary for a quiet box, not sufficient."""
    held = 0
    for path in sorted(cdir.glob("olo-build*.lock")):
        if path.name == "olo-build.admission.lock":
            continue
        try:
            with open(path, "a", encoding="utf-8"):
                pass
        except PermissionError:
            held += 1
    return held


PR_FIELDS = "number,headRefName,headRefOid,mergeable,statusCheckRollup"
THREADS_QUERY = (
    "query($o:String!,$r:String!,$n:Int!,$endCursor:String){repository(owner:$o,name:$r){pullRequest(number:$n)"
    "{reviewThreads(first:100,after:$endCursor){pageInfo{hasNextPage endCursor} nodes{isResolved}}}}}"
)


def unresolved_threads(repo: str, number: int) -> int | None:
    owner, name = repo.split("/", 1)
    result = ws.run(
        ["gh", "api", "graphql", "--paginate", "-f", f"query={THREADS_QUERY}", "-F", f"o={owner}", "-F",
         f"r={name}", "-F", f"n={number}", "--jq",
         "[.data.repository.pullRequest.reviewThreads.nodes[]|select(.isResolved==false)]|length"],
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        return None  # reported as "thread count unknown", never as 0
    return sum(int(x) for x in result.stdout.split())


def pr_state(repo: str, number: int) -> str:
    data = run_json(["gh", "pr", "view", str(number), "--repo", repo, "--json", "state"])
    return data.get("state", "?") if isinstance(data, dict) else "?"


def gather(base: str, repo: str, ledger: dict, prev: dict | None) -> dict:
    trees = ws.list_worktrees(base)
    for tree in trees:
        ws.classify(tree, base, repo)
    live = sessions()
    by_cwd = {norm(s.get("cwd", "")): s for s in live}

    snap: dict = {"taken_at": now_iso(), "worktrees": {}, "prs": {}, "closed_prs": {}}
    branch_to_slug = {}
    for tree in trees:
        record = {k: getattr(tree, k) for k in ("path", "branch", "kind", "status", "landed", "open_pr", "has_handover")}
        slug = slug_of(record)
        session = by_cwd.get(norm(tree.path))
        record["session"] = (
            {"name": session.get("name"), "id": session.get("id") or session.get("sessionId", "")[:8],
             "kind": session.get("kind"), "status": session.get("status") or session.get("state")}
            if session else None
        )
        record["activity"] = activity_marker(tree)
        record["quiet_ticks"] = next_quiet_ticks((prev or {}).get("worktrees", {}).get(slug), record)
        snap["worktrees"][slug] = record
        if tree.branch:
            branch_to_slug[tree.branch] = slug

    for pr in run_json(["gh", "pr", "list", "--repo", repo, "--state", "open", "--limit", "50", "--json", PR_FIELDS]) or []:
        entry = {
            "branch": pr["headRefName"],
            "head": pr["headRefOid"],
            "mergeable": pr.get("mergeable"),
            "checks": summarize_checks(pr.get("statusCheckRollup") or []),
            "threads": unresolved_threads(repo, pr["number"]),
            "worktree": branch_to_slug.get(pr["headRefName"]),
        }
        entry["gate"], entry["gate_missing"] = gate_status(entry)
        snap["prs"][str(pr["number"])] = entry

    if prev:
        for num in prev.get("prs", {}).keys() - snap["prs"].keys():
            snap["closed_prs"][num] = pr_state(repo, int(num))

    merged: set[int] = set()
    for slug, hold in ledger.get("holds", {}).items():
        for n in hold.get("after_prs", []):
            if n not in merged and pr_state(repo, n) == "MERGED":
                merged.add(n)
    snap["ready_to_launch"] = sorted(
        slug for slug, hold in ledger.get("holds", {}).items()
        if hold_cleared(hold, merged) and snap["worktrees"].get(slug, {}).get("status") == "incomplete-standby"
    )

    snap["builds_running"] = builds_running(common_dir(base))

    since = (prev or {}).get("taken_at")
    snap["new_issues"] = []
    if since:
        issues = run_json(["gh", "issue", "list", "--repo", repo, "--state", "open", "--limit", "50",
                           "--search", f"created:>={since[:10]}", "--json", "number,title,createdAt"]) or []
        snap["new_issues"] = sorted(
            ({"number": i["number"], "title": i["title"]} for i in issues if i["createdAt"] > since),
            key=lambda i: i["number"],
        )
    snap["live_sessions_elsewhere"] = sorted(
        s.get("name", "?") for s in live if norm(s.get("cwd", "")) not in {norm(t.path) for t in trees}
    )
    return snap


# --- subcommands ----------------------------------------------------------------------------

def cmd_status(args, base: str, repo: str) -> int:
    fdir = fleet_dir(base)
    ledger = load(fdir / "ledger.json", {"holds": {}, "launches": {}})
    prev = load(fdir / "snapshot.json", {}) or None
    cur = gather(base, repo, ledger, prev)
    events = diff_events(prev, cur)
    standing = standing_items(cur, ledger)
    if not args.no_save:
        save(fdir / "snapshot.json", cur)
    if args.json:
        print(json.dumps({"events": events, "standing": standing, "snapshot": cur}, indent=2))
        return 0
    print(f"fleet @ {cur['taken_at']}  (previous: {(prev or {}).get('taken_at', 'none')})  builds running: {cur['builds_running']}")
    print("EVENTS:" if events else "EVENTS: none (NO CHANGE)")
    for line in events:
        print(f"  {line}")
    print("STANDING:" if standing else "STANDING: none")
    for line in standing:
        print(f"  {line}")
    return 0


def ensure_mcp_approval(base: str, worktree: Path) -> str:
    """A background session cannot answer the "New MCP server found in this project" dialog, so a
    fresh worktree blocks at startup. Mirror the approval the user already gave in the base repo:
    copy ONLY its enabledMcpjsonServers / disabledMcpjsonServers keys into the worktree's
    git-ignored settings.local.json, and only when that file does not exist yet."""
    target = worktree / ".claude" / "settings.local.json"
    if target.exists():
        return f"kept existing {target}"
    source = Path(base) / ".claude" / "settings.local.json"
    if not source.exists():
        return "base repo has no settings.local.json: the MCP dialog may block the session (check `claude agents`)"
    base_settings = json.loads(source.read_text(encoding="utf-8"))
    keys = {k: base_settings[k] for k in ("enabledMcpjsonServers", "disabledMcpjsonServers") if k in base_settings}
    if not keys:
        return "base settings.local.json approves no MCP servers: the MCP dialog may block the session"
    target.parent.mkdir(exist_ok=True)
    target.write_text(json.dumps(keys, indent=2) + "\n", encoding="utf-8")
    return f"wrote {target} with {sorted(keys)} from the base repo"


def parse_launch_id(output: str) -> str | None:
    # The separator is "·", but a piped `claude` may not write it as UTF-8: match any one character.
    match = re.search(r"backgrounded\s*\S\s*([0-9a-f]{6,})\s*\S", output)
    return match.group(1) if match else None


def cmd_launch(args, base: str, repo: str) -> int:
    slug = args.slug
    trees = [t for t in ws.list_worktrees(base) if t.branch == f"feature/{slug}"]
    if not trees:
        print(f"refused: no worktree on feature/{slug}", file=sys.stderr)
        return 1
    tree = trees[0]
    ws.classify(tree, base, repo)
    if tree.status != "incomplete-standby":
        print(f"refused: {slug} is {tree.status}, not incomplete-standby ({'; '.join(tree.reasons)}). "
              "A started task is resumed, not relaunched: `claude --bg --resume <session-id>` from its worktree.",
              file=sys.stderr)
        return 1
    live = [s for s in sessions() if norm(s.get("cwd", "")) == norm(tree.path)]
    if live:
        print(f"refused: a session already runs there: {[s.get('name') for s in live]}", file=sys.stderr)
        return 1

    fdir = fleet_dir(base)
    ledger = load(fdir / "ledger.json", {"holds": {}, "launches": {}})
    hold = ledger.get("holds", {}).get(slug)
    if hold and not args.ignore_hold:
        merged = {n for n in hold.get("after_prs", []) if pr_state(repo, n) == "MERGED"}
        if not hold_cleared(hold, merged):
            print(f"refused: {slug} is held ({hold}); merged so far: {sorted(merged)}. "
                  "Pass --ignore-hold only on the user's word.", file=sys.stderr)
            return 1

    handover_path = Path(tree.path) / "HANDOVER.md"
    if not handover_path.is_file():
        print(f"refused: {handover_path} is missing: a worker needs its handover", file=sys.stderr)
        return 1
    model_effort = parse_model_effort(handover_path.read_text(encoding="utf-8"))
    if not model_effort:
        print("refused: no `**<Model> <ver>, effort <level>**` line in HANDOVER.md's "
              "*Recommended model + effort*; add it rather than guessing", file=sys.stderr)
        return 1
    model, effort = model_effort
    goal = extract_goal((Path(base) / START_WORK_SKILL).read_text(encoding="utf-8"))
    if not goal:
        print(f"refused: could not find exactly one `/goal` line in {START_WORK_SKILL}", file=sys.stderr)
        return 1

    cmd = [claude_exe(), "--bg", "-n", slug, "--model", model, "--effort", effort, "--permission-mode", "auto", goal]
    print(f"launch {slug}: model {model}, effort {effort}, cwd {tree.path}")
    if args.dry_run:
        print("dry run: would also ensure MCP approval and run:", " ".join(cmd[1:-1]), "'<goal>'")
        return 0
    print(ensure_mcp_approval(base, Path(tree.path)))

    result = subprocess.run(cmd, cwd=tree.path, capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(result.stdout.strip())
    session_id = parse_launch_id(result.stdout)
    if result.returncode != 0 or not session_id:
        print(f"launch failed (exit {result.returncode}): {result.stderr.strip()}", file=sys.stderr)
        return 1

    state = "?"
    deadline = time.monotonic() + LAUNCH_SETTLE_SECONDS
    while time.monotonic() < deadline:
        time.sleep(5)
        mine = [s for s in sessions() if (s.get("id") or "").startswith(session_id) or s.get("name") == slug]
        state = (mine[0].get("state") or mine[0].get("status")) if mine else "not listed"
        if state in ("blocked", "busy"):
            break
    ledger.setdefault("launches", {})[slug] = {
        "session_id": session_id, "launched_at": now_iso(), "model": model, "effort": effort,
    }
    ledger.get("holds", {}).pop(slug, None)
    save(fdir / "ledger.json", ledger)
    print(f"session {session_id} state after launch: {state}")
    if state == "blocked":
        print(f"BLOCKED at startup: a dialog waits for a person. `claude attach {session_id}` to answer it.")
        return 2
    return 0


def cmd_hold(args, base: str, repo: str) -> int:
    if not args.after_pr and not args.note:
        print("refused: a hold needs --after-pr and/or --note", file=sys.stderr)
        return 1
    fdir = fleet_dir(base)
    ledger = load(fdir / "ledger.json", {"holds": {}, "launches": {}})
    ledger.setdefault("holds", {})[args.slug] = {
        "after_prs": sorted(set(args.after_pr or [])), "note": args.note or "", "since": now_iso(),
    }
    save(fdir / "ledger.json", ledger)
    print(f"held {args.slug}: {ledger['holds'][args.slug]}")
    return 0


def cmd_release(args, base: str, repo: str) -> int:
    fdir = fleet_dir(base)
    ledger = load(fdir / "ledger.json", {"holds": {}, "launches": {}})
    dropped = ledger.get("holds", {}).pop(args.slug, None)
    save(fdir / "ledger.json", ledger)
    print(f"released {args.slug}: {dropped}" if dropped else f"{args.slug} had no hold")
    return 0


def cmd_goal(args, base: str, repo: str) -> int:
    goal = extract_goal((Path(base) / START_WORK_SKILL).read_text(encoding="utf-8"))
    if not goal:
        print(f"no single /goal line in {START_WORK_SKILL}", file=sys.stderr)
        return 1
    print(goal)
    return 0


def main() -> int:
    # Claude's own output carries "·" and "❯"; a cp1252 Windows console would raise on print.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("status")
    p.add_argument("--json", action="store_true")
    p.add_argument("--no-save", action="store_true", help="do not replace the stored snapshot")
    p = sub.add_parser("launch")
    p.add_argument("slug")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--ignore-hold", action="store_true")
    p = sub.add_parser("hold")
    p.add_argument("slug")
    p.add_argument("--after-pr", type=int, nargs="+")
    p.add_argument("--note")
    p = sub.add_parser("release")
    p.add_argument("slug")
    sub.add_parser("goal")
    args = parser.parse_args()

    base = ws.base_repo()
    repo = ws.owner_repo(base)
    handler = {"status": cmd_status, "launch": cmd_launch, "hold": cmd_hold, "release": cmd_release, "goal": cmd_goal}
    return handler[args.cmd](args, base, repo)


if __name__ == "__main__":
    sys.exit(main())
