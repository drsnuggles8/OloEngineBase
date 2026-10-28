#!/usr/bin/env python3
"""Fail when a golden changes and its vendor twin does not (issue #1516).

A golden under OloEditor/assets/tests/{visual,golden}/ can have a vendor twin at
<root>/<vendor>/<same name>, for example visual/amd/FoliageWind_GL_Forward_MSAA1_Ground.png.
The PR CI compares against the vendor-neutral image on NVIDIA; only the AMD nightly
(gpu-conformance-amd.yml, --olo-golden-vendor=amd) reads the twin. So a change that
rebakes the neutral golden and leaves the twin alone is green on every PR check and
turns the nightly red after the merge. #1533 and #1667 both shipped that way.

The rule: when a diff modifies a neutral golden that has a tracked vendor twin, the
same diff must also change the twin, or carry a waiver. A waiver is a line in
scripts/vendor-golden-twin-waivers.txt:

    <neutral golden path>  <git blob id of its new content>  <reason>

It is pinned to the blob id, so it covers exactly that version of the golden and
lapses the moment the golden changes again. A waiver whose blob no longer matches is
reported as stale.

The diff is <base> against HEAD's working tree, or against --head when given. The base
is --base, else $OLO_GOLDEN_TWIN_BASE (set by the pre-commit CI job to the PR's base
commit), else the merge-base of HEAD and origin/master. When no base can be found
(a shallow clone on a push to master) the diff part is skipped and says so.

Tests: scripts/test_vendor_golden_twins.py.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

GOLDEN_ROOTS = ("OloEditor/assets/tests/visual/", "OloEditor/assets/tests/golden/")
WAIVERS = "scripts/vendor-golden-twin-waivers.txt"


def split_golden(path: str) -> tuple[str, str, str] | None:
    """Return (root, vendor, name) for a golden PNG; vendor is '' for a neutral one."""
    if not path.endswith(".png"):
        return None
    for root in GOLDEN_ROOTS:
        if path.startswith(root):
            parts = path[len(root) :].split("/")
            if len(parts) == 1:
                return root, "", parts[0]
            if len(parts) == 2:
                return root, parts[0], parts[1]
    return None


def parse_waivers(text: str) -> tuple[dict[str, tuple[str, int]], list[str]]:
    waivers: dict[str, tuple[str, int]] = {}
    errors = []
    for number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split(None, 2)
        if len(fields) < 3:
            errors.append(f"{WAIVERS}:{number}: needs '<golden path> <blob id> <reason>'")
            continue
        waivers[fields[0]] = (fields[1], number)
    return waivers, errors


def find_problems(
    changed: dict[str, str],
    tracked: set[str],
    blob_of: Callable[[str], str | None],
    waivers_text: str,
) -> list[str]:
    """`changed` maps a path to its git status letter (M, A, D) between base and head."""
    waivers, errors = parse_waivers(waivers_text)

    twins_by_neutral: dict[str, list[str]] = {}
    for path in tracked:
        parts = split_golden(path)
        if parts and parts[1]:
            twins_by_neutral.setdefault(parts[0] + parts[2], []).append(path)

    for path, status in sorted(changed.items()):
        parts = split_golden(path)
        if status != "M" or not parts or parts[1]:
            continue
        stale = sorted(t for t in twins_by_neutral.get(path, []) if t not in changed)
        if not stale:
            continue
        waiver = waivers.get(path)
        if waiver and waiver[0] == blob_of(path):
            continue
        for twin in stale:
            errors.append(f"{path}: changed, but its vendor twin {twin} did not")

    for path, (blob, number) in sorted(waivers.items()):
        current = blob_of(path)
        if current is None:
            errors.append(f"{WAIVERS}:{number}: {path} is not a tracked file; delete the entry")
        elif current != blob:
            errors.append(f"{WAIVERS}:{number}: {path} has changed since it was waived; delete the entry")
    return errors


def git(root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args], cwd=root, capture_output=True, text=True, encoding="utf-8", check=check
    )


def resolve_base(root: Path, explicit: str | None) -> str | None:
    if explicit:
        return explicit
    from_env = os.environ.get("OLO_GOLDEN_TWIN_BASE", "").strip()
    if from_env:
        return from_env
    merge_base = git(root, "merge-base", "HEAD", "origin/master", check=False)
    return merge_base.stdout.strip() if merge_base.returncode == 0 else None


def collect(root: Path, base: str | None, head: str | None):
    """Return (changed, tracked, blob_of) for the diff base..head (head None = working tree)."""
    changed: dict[str, str] = {}
    if base:
        diff = ["diff", "--name-status", "--no-renames", "-z", base] + ([head] if head else [])
        fields = git(root, *diff, "--", *GOLDEN_ROOTS).stdout.split("\0")
        for status, path in zip(fields[0::2], fields[1::2]):
            changed[path] = status[0]

    if head:
        listing = git(root, "ls-tree", "-r", "-z", head, "--", *GOLDEN_ROOTS).stdout.split("\0")
        blobs = {}
        for entry in filter(None, listing):
            meta, path = entry.split("\t", 1)
            blobs[path] = meta.split()[2]
        return changed, set(blobs), blobs.get

    tracked = set(filter(None, git(root, "ls-files", "-z", "--", *GOLDEN_ROOTS).stdout.split("\0")))
    tracked = {p for p in tracked if (root / p).is_file()}

    def blob_of(path: str) -> str | None:
        if not (root / path).is_file():
            return None
        return git(root, "hash-object", "--", path).stdout.strip()

    return changed, tracked, blob_of


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", help="commit to diff from (default: $OLO_GOLDEN_TWIN_BASE, else merge-base with origin/master)")
    parser.add_argument("--head", help="commit to diff to (default: the working tree)")
    args = parser.parse_args(argv)

    root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel").stdout.strip())
    base = resolve_base(root, args.base)
    if base is None:
        print("check_vendor_golden_twins: no base commit (no origin/master here); checking waivers only")
    changed, tracked, blob_of = collect(root, base, args.head)

    waivers_path = root / WAIVERS
    waivers_text = waivers_path.read_text(encoding="utf-8") if waivers_path.is_file() else ""
    errors = find_problems(changed, tracked, blob_of, waivers_text)
    for line in errors:
        print(line)
    if errors:
        print(
            f"\n{len(errors)} vendor-golden twin error(s). The AMD nightly compares against the twin, so it "
            "goes red after the merge. Rebake the twins from this branch:\n"
            "    gh workflow run gpu-conformance-amd.yml --ref <branch> -f golden_rebase=true\n"
            "and commit the changed PNGs from its gpu_amd_rebaked_baselines artifact. If the change cannot "
            f"reach that vendor, add '<path> <blob id> <reason>' to {WAIVERS} "
            "(blob id: git hash-object <path>). See docs/ops/self-hosted-gpu-runner.md."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
