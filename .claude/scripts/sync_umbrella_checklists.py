#!/usr/bin/env python3
"""Tick every open issue's checklist item whose referenced issue or PR is done.

Umbrella and programme issues track their children as task-list lines:

    - [ ] #1355 — Hybrid reflections: canonical textured hit shading

A PR's `Closes #1355` shuts the child, but nothing ticks the umbrella's box, so umbrellas
drift: #1359 had five closed children still unticked when this script was written.
/cleanup-worktree and /start-work run it after they heal the registry.

An item is ticked only when the `#N` is the first thing after the box (the line IS that
issue, not a mention of it) and N is
  - an issue closed as COMPLETED (an issue closed before GitHub recorded a reason counts), or
  - a MERGED pull request.
An issue closed as NOT_PLANNED or DUPLICATE, and a PR closed without merging, is listed
under "left unticked" for a person to decide: a tick would claim work that never happened.

The only edit is `[ ]` -> `[x]` on those lines; every other byte of the body, including
line endings and the olo-score block, is kept, and the script asserts it. Checklist-looking
lines inside fenced code blocks are examples, not items, and are never ticked. Each body is
re-read right before it is written, so an edit made since the scan survives; one made in the
second between that re-read and the write is overwritten (see write_body).

Usage:
  python .claude/scripts/sync_umbrella_checklists.py              # dry run: print what would change
  python .claude/scripts/sync_umbrella_checklists.py --apply      # write the ticks
  python .claude/scripts/sync_umbrella_checklists.py --issue 1359 # only that open issue
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    _stream.reconfigure(encoding="utf-8", errors="replace")

# A task-list item whose first token is an issue reference. Group 1 is everything before the
# box, group 2 the referenced number. `[^\S\r\n]` is horizontal whitespace, so the match never
# crosses or consumes a line ending.
UNTICKED_ITEM = re.compile(r"^([^\S\r\n]*[-*+][^\S\r\n]+)\[ \][^\S\r\n]+#(\d+)\b", re.MULTILINE)

LIST_LIMIT = 1000
GRAPHQL_BATCH = 50


@dataclass(frozen=True)
class RefState:
    kind: str  # "issue" or "pr"
    state: str  # OPEN / CLOSED / MERGED
    reason: str | None  # issue stateReason: COMPLETED / NOT_PLANNED / DUPLICATE / REOPENED / None

    @property
    def done(self) -> bool:
        if self.kind == "pr":
            return self.state == "MERGED"
        return self.state == "CLOSED" and self.reason in (None, "COMPLETED")

    @property
    def closed_not_done(self) -> bool:
        return self.state in ("CLOSED", "MERGED") and not self.done

    def describe(self) -> str:
        if self.kind == "pr":
            return f"PR {self.state.lower()}"
        return f"issue closed as {(self.reason or 'unknown').lower()}"


# A fenced code block's opening or closing line: up to three spaces, then three or more
# backticks or tildes (CommonMark). Group 1 is the fence run.
FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})")


def fenced_spans(body: str) -> list[tuple[int, int]]:
    """Character ranges of fenced code blocks, so an example checklist inside one is not an
    item. An unclosed fence runs to the end of the body, as CommonMark renders it."""
    spans: list[tuple[int, int]] = []
    fence: str | None = None
    start = pos = 0
    for line in body.splitlines(keepends=True):
        m = FENCE.match(line)
        if fence is None and m:
            fence, start = m.group(1), pos
        elif fence is not None and m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence) \
                and not line[m.end():].strip():
            spans.append((start, pos + len(line)))
            fence = None
        pos += len(line)
    if fence is not None:
        spans.append((start, len(body)))
    return spans


def active_items(body: str) -> list[re.Match[str]]:
    """UNTICKED_ITEM matches outside fenced code blocks."""
    spans = fenced_spans(body)
    return [m for m in UNTICKED_ITEM.finditer(body)
            if not any(lo <= m.start() < hi for lo, hi in spans)]


def unticked_refs(body: str) -> list[int]:
    return [int(m.group(2)) for m in active_items(body)]


def tick(body: str, done: set[int]) -> tuple[str, list[int]]:
    """Return the body with every unticked item for a number in `done` ticked, and the numbers
    ticked, in body order. Nothing but the box character changes."""
    ticked: list[int] = []
    chars = list(body)
    for m in active_items(body):
        if int(m.group(2)) in done:
            ticked.append(int(m.group(2)))
            chars[m.end(1) + 1] = "x"  # the space inside "[ ]"
    new_body = "".join(chars)
    assert_only_boxes_changed(body, new_body)
    return new_body, ticked


def assert_only_boxes_changed(old: str, new: str) -> None:
    if len(old) != len(new):
        raise AssertionError("tick changed the body length")
    for i, (a, b) in enumerate(zip(old, new)):
        if a != b and not (a == " " and b == "x" and old[i - 1] == "[" and old[i + 1] == "]"):
            raise AssertionError(f"tick changed byte {i}: {a!r} -> {b!r}")


def run(args: list[str], check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and result.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed ({result.returncode}): {result.stderr.strip()}")
    return result


def owner_repo() -> tuple[str, str]:
    url = run(["git", "remote", "get-url", "origin"]).stdout.strip()
    match = re.search(r"github\.com[:/]([^/]+)/([^/]+?)(?:\.git)?$", url)
    if not match:
        sys.exit(f"origin is not a GitHub remote: {url}")
    return match.group(1), match.group(2)


def open_issues(repo: str, only: int | None) -> list[dict]:
    if only is not None:
        issue = json.loads(run(["gh", "issue", "view", str(only), "--repo", repo, "--json", "number,state,body"]).stdout)
        if issue["state"] != "OPEN":
            sys.exit(f"#{only} is {issue['state']}; only open issues are synced")
        return [issue]
    rows = json.loads(run(["gh", "issue", "list", "--repo", repo, "--state", "open",
                           "--limit", str(LIST_LIMIT), "--json", "number,body"]).stdout)
    if len(rows) >= LIST_LIMIT:
        sys.exit(f"gh issue list returned {len(rows)} rows, its full limit; the scan would be incomplete")
    return rows


def ref_states(owner: str, name: str, numbers: set[int]) -> dict[int, RefState]:
    states: dict[int, RefState] = {}
    ordered = sorted(numbers)
    for start in range(0, len(ordered), GRAPHQL_BATCH):
        chunk = ordered[start:start + GRAPHQL_BATCH]
        fields = " ".join(
            f"n{n}: issueOrPullRequest(number: {n}) {{ __typename "
            f"... on Issue {{ state stateReason }} ... on PullRequest {{ state }} }}"
            for n in chunk)
        query = f'query {{ repository(owner: "{owner}", name: "{name}") {{ {fields} }} }}'
        # A number that is no issue or PR here comes back null with a NOT_FOUND error and a
        # non-zero exit, beside the other answers; any other error means the answers are wrong.
        result = run(["gh", "api", "graphql", "-f", f"query={query}"], check=False)
        try:
            data = json.loads(result.stdout)
        except json.JSONDecodeError:
            sys.exit(f"gh api graphql failed ({result.returncode}): {result.stderr.strip()}")
        fatal = [e for e in data.get("errors") or [] if e.get("type") != "NOT_FOUND"]
        repo_data = (data.get("data") or {}).get("repository")
        if fatal or repo_data is None:
            sys.exit(f"gh api graphql failed: {fatal or result.stderr.strip()}")
        for n in chunk:
            node = repo_data.get(f"n{n}")
            if node is None:
                continue  # a number that is not an issue or PR here (a typo, another repo's ref)
            kind = "pr" if node["__typename"] == "PullRequest" else "issue"
            states[n] = RefState(kind, node["state"], node.get("stateReason"))
    return states


def write_body(repo: str, number: int, body: str) -> None:
    # Replaces the whole body. `gh issue edit --body-file` has no revision precondition, so an
    # edit made remotely between main()'s re-read and this write is overwritten. The re-read
    # only narrows that window to about a second; it does not close it.
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8", newline="") as f:
        f.write(body)
        path = f.name
    try:
        run(["gh", "issue", "edit", str(number), "--repo", repo, "--body-file", path])
    finally:
        Path(path).unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--apply", action="store_true", help="write the ticks (default: dry run)")
    parser.add_argument("--issue", type=int, help="sync only this open issue")
    args = parser.parse_args()

    owner, name = owner_repo()
    repo = f"{owner}/{name}"
    issues = [i for i in open_issues(repo, args.issue) if unticked_refs(i["body"] or "")]
    refs = {n for i in issues for n in unticked_refs(i["body"])}
    states = ref_states(owner, name, refs)
    done = {n for n, s in states.items() if s.done}

    changed = 0
    for issue in issues:
        number, body = issue["number"], issue["body"]
        _, ticked = tick(body, done)
        skipped = [n for n in unticked_refs(body) if n in states and states[n].closed_not_done]
        if ticked:
            if args.apply:
                fresh = json.loads(run(["gh", "issue", "view", str(number), "--repo", repo,
                                        "--json", "body"]).stdout)["body"]
                new_body, ticked = tick(fresh, done)
                if ticked:
                    write_body(repo, number, new_body)
            if ticked:
                changed += 1
                verb = "ticked" if args.apply else "would tick"
                print(f"#{number}: {verb} " + ", ".join(f"#{n}" for n in ticked))
        for n in skipped:
            print(f"#{number}: left unticked #{n} ({states[n].describe()}): decide by hand")

    if not changed:
        print("checklists in sync: no open issue has an unticked item for a done issue or merged PR")
    elif not args.apply:
        print("dry run: re-run with --apply to write")
    return 0


if __name__ == "__main__":
    sys.exit(main())
