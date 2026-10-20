#!/usr/bin/env python3
"""Classify every non-base worktree of this repository: is it finished, or still work?

One gate, shared by /cleanup-worktree (removes the finished ones) and /resume-worktrees
(re-opens the unfinished ones), so a worktree one of them refuses is always one the other
handles. Read-only: it runs git and gh queries and changes nothing.

A worktree is COMPLETED only when all of these hold:

  landed   its HEAD is an ancestor of origin/master, or a MERGED PR's head commit equals
           its HEAD (the squash-merge form, which defeats the ancestor test);
  no PR    no OPEN pull request has the branch as its head;
  clean    `git status --porcelain` lists nothing but disposable output (see DISPOSABLE).

Anything else is INCOMPLETE, and the status says why. Kinds:

  feature  a feature/* branch made by /start-work;
  other    any other branch (a bot branch such as renovate/*, a hand-made branch);
           classified the same way, but /cleanup-worktree never deletes the branch itself;
  detached a detached HEAD, typically a before-baseline checkout. It holds no task, so
           /resume-worktrees never re-opens it; it is completed when clean and on master.

The script fetches nothing: run `git fetch --prune origin` first, or every answer is
against a stale origin/master.

Usage:
  python .claude/scripts/worktree_status.py            # table
  python .claude/scripts/worktree_status.py --json     # machine-readable
  python .claude/scripts/worktree_status.py --slug foo # only feature/foo
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

# Paths a finished task-loop worktree is normally dirty with. They are test or handover
# output, so they do not make a worktree "work in progress". Anything else does.
DISPOSABLE = (
    re.compile(r"^HANDOVER\.md$"),
    re.compile(r"^OloEditor/assets/tests/visual/"),
    re.compile(r"^OloEditor/SupportedTargetTestGame/"),
)


@dataclass
class Worktree:
    path: str
    head: str
    branch: str | None
    kind: str = "feature"
    status: str = ""
    landed: str = ""  # "ancestor", "merged-pr #N", or ""
    open_pr: int | None = None
    wip_files: list[str] = field(default_factory=list)
    disposable_files: int = 0
    reasons: list[str] = field(default_factory=list)


def run(args: list[str], cwd: str | None = None, check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and result.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed ({result.returncode}): {result.stderr.strip()}")
    return result


def base_repo() -> str:
    common = run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"]).stdout.strip()
    return str(Path(common).parent)


def owner_repo(base: str) -> str:
    url = run(["git", "-C", base, "remote", "get-url", "origin"]).stdout.strip()
    match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$", url)
    if not match:
        raise RuntimeError(f"cannot parse owner/repo from origin URL {url!r}")
    return match.group(1)


def list_worktrees(base: str) -> list[Worktree]:
    out = run(["git", "-C", base, "worktree", "list", "--porcelain"]).stdout
    trees: list[Worktree] = []
    for block in out.strip().split("\n\n"):
        fields = dict(line.split(" ", 1) if " " in line else (line, "") for line in block.splitlines())
        path = fields.get("worktree", "")
        if Path(path).resolve() == Path(base).resolve():
            continue
        branch = fields.get("branch", "").removeprefix("refs/heads/") or None
        trees.append(Worktree(path=path, head=fields.get("HEAD", ""), branch=branch))
    return trees


def gh_prs(repo: str, branch: str, state: str) -> list[dict]:
    result = run(
        ["gh", "pr", "list", "--repo", repo, "--state", state, "--head", branch,
         "--json", "number,headRefOid,mergedAt", "--limit", "20"],
        check=False,
    )
    if result.returncode != 0:
        # An unanswered PR query must not read as "no PR": that would mark live work completed.
        raise RuntimeError(f"gh pr list --head {branch} --state {state} failed: {result.stderr.strip()}")
    return json.loads(result.stdout or "[]")


def classify(tree: Worktree, base: str, repo: str) -> None:
    if tree.branch is None:
        tree.kind = "detached"
    elif not tree.branch.startswith("feature/"):
        tree.kind = "other"

    if not Path(tree.path).exists():
        tree.status = "missing"
        tree.reasons.append("registered but the directory is gone: `git worktree prune` clears it")
        return

    porcelain = run(["git", "-C", tree.path, "status", "--porcelain", "--untracked-files=all"]).stdout
    for line in porcelain.splitlines():
        name = line[3:].strip().strip('"')
        if " -> " in name:
            name = name.split(" -> ", 1)[1]
        if any(p.search(name) for p in DISPOSABLE):
            tree.disposable_files += 1
        else:
            tree.wip_files.append(line)

    ancestor = run(["git", "-C", base, "merge-base", "--is-ancestor", tree.head, "origin/master"], check=False)
    if ancestor.returncode == 0:
        tree.landed = "ancestor"

    if tree.branch is not None:
        open_prs = gh_prs(repo, tree.branch, "open")
        if open_prs:
            tree.open_pr = open_prs[0]["number"]
        if not tree.landed:
            for pr in gh_prs(repo, tree.branch, "merged"):
                if pr.get("headRefOid") == tree.head:
                    tree.landed = f"merged-pr #{pr['number']}"
                    break

    if tree.wip_files:
        tree.reasons.append(f"{len(tree.wip_files)} changed or untracked non-disposable file(s)")
    if tree.open_pr is not None:
        tree.reasons.append(f"open PR #{tree.open_pr}")
    if not tree.landed:
        tree.reasons.append("HEAD is not on origin/master and no merged PR has this head")

    if not tree.reasons:
        tree.status = "completed"
    elif tree.wip_files:
        tree.status = "incomplete-dirty"
    elif tree.open_pr is not None:
        tree.status = "incomplete-open-pr"
    else:
        tree.status = "incomplete-unmerged"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", action="store_true", help="print JSON instead of a table")
    parser.add_argument("--slug", help="only the worktree on feature/<slug>")
    args = parser.parse_args()

    base = base_repo()
    repo = owner_repo(base)
    trees = list_worktrees(base)
    if args.slug:
        trees = [t for t in trees if t.branch == f"feature/{args.slug}"]
        if not trees:
            print(f"no worktree on feature/{args.slug}", file=sys.stderr)
            return 1

    for tree in trees:
        classify(tree, base, repo)

    if args.json:
        print(json.dumps({"base": base, "repo": repo, "worktrees": [asdict(t) for t in trees]}, indent=2))
        return 0

    print(f"base: {base}   repo: {repo}")
    for t in trees:
        label = t.branch or f"(detached {t.head[:9]})"
        why = "; ".join(t.reasons) if t.reasons else f"landed ({t.landed})"
        print(f"{t.status:20} {t.kind:8} {t.path}  [{label}]  {why}")
        for line in t.wip_files[:10]:
            print(f"{'':30}{line}")
        if len(t.wip_files) > 10:
            print(f"{'':30}... {len(t.wip_files) - 10} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
