#!/usr/bin/env python3
"""Fail when a golden changes and its vendor twin does not (issue #1516).

A golden under OloEditor/assets/tests/{visual,golden}/ can have a vendor twin at
<root>/<vendor>/<same name>, for example visual/amd/FoliageWind_GL_Forward_MSAA1_Ground.png.
The PR CI compares against the vendor-neutral image on NVIDIA; only the AMD nightly
(gpu-conformance-amd.yml, --olo-golden-vendor=amd) reads the twin. So a change that
rebakes the neutral golden and leaves the twin alone is green on every PR check and
turns the nightly red after the merge. #1533 and #1667 both shipped that way.

The rules, for a diff from a base to a head:

- a MODIFIED neutral golden with a tracked vendor twin needs the twin modified too;
- an ADDED neutral golden whose family (same root, same name up to the first '_') has
  twins in a vendor directory needs its own twin added there, because a missing vendor
  baseline is a hard failure;
- a DELETED twin whose neutral golden is still tracked is reported.

A waiver is a line in scripts/vendor-golden-twin-waivers.txt:

    <neutral golden path>  <git blob id of its new content>  <reason>

It is pinned to the blob id, so it covers exactly that version of the golden and
lapses the moment the golden changes again. A waiver whose blob no longer matches is
reported as stale.

The head is the working tree, or --head. The base is --base, else $OLO_GOLDEN_TWIN_BASE
(the pre-commit CI job sets it to the first parent of the PR's merge commit), else the
merge-base of HEAD and origin/master. With no base (a shallow clone on a push to
master) only the waivers are checked.

--hook is how pre-commit runs it. Without $OLO_GOLDEN_TWIN_BASE (a local run, the Stop
hook, the push guard) it prints its findings and exits 0: a branch whose AMD rebake is
still running must be pushable, since the rebake is dispatched from the pushed branch.
CI sets the base, and there the findings fail the job.

Tests: scripts/test_vendor_golden_twins.py.
"""

from __future__ import annotations

import argparse
import functools
import os
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

GOLDEN_ROOTS = ("OloEditor/assets/tests/visual/", "OloEditor/assets/tests/golden/")
WAIVERS = "scripts/vendor-golden-twin-waivers.txt"
BASE_ENV = "OLO_GOLDEN_TWIN_BASE"


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


def family(name: str) -> str:
    return name.split("_", 1)[0]


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
        if fields[0] in waivers:
            errors.append(f"{WAIVERS}:{number}: {fields[0]} is already waived on line {waivers[fields[0]][1]}")
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
    vendors_by_family: dict[tuple[str, str], set[str]] = {}
    for path in tracked:
        parts = split_golden(path)
        if parts and parts[1]:
            root, vendor, name = parts
            twins_by_neutral.setdefault(root + name, []).append(path)
            vendors_by_family.setdefault((root, family(name)), set()).add(vendor)

    def waived(path: str) -> bool:
        waiver = waivers.get(path)
        return waiver is not None and waiver[0] == blob_of(path)

    for path, status in sorted(changed.items()):
        parts = split_golden(path)
        if not parts:
            continue
        root, vendor, name = parts
        if vendor:
            neutral = root + name
            if status == "D" and neutral in tracked and not waived(neutral):
                errors.append(f"{path}: deleted, but its neutral golden {neutral} is still tracked")
            continue
        if status == "M":
            missing = sorted(t for t in twins_by_neutral.get(path, []) if changed.get(t) not in ("M", "A"))
            verb = "changed"
        elif status == "A":
            vendors = vendors_by_family.get((root, family(name)), set())
            missing = sorted(f"{root}{v}/{name}" for v in vendors if changed.get(f"{root}{v}/{name}") != "A")
            verb = "added"
        else:
            continue
        if missing and not waived(path):
            for twin in missing:
                errors.append(f"{path}: {verb}, but its vendor twin {twin} was not")

    for path, (blob, number) in sorted(waivers.items()):
        current = blob_of(path)
        if current is None:
            errors.append(f"{WAIVERS}:{number}: {path} is not a tracked file; delete the entry")
        elif current != blob:
            errors.append(f"{WAIVERS}:{number}: {path} has changed since it was waived; delete the entry")
    return errors


class GitError(Exception):
    pass


def git(root: Path, *args: str) -> str:
    result = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        raise GitError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def resolve_base(root: Path, explicit: str | None) -> str | None:
    if explicit:
        return explicit
    from_env = os.environ.get(BASE_ENV, "").strip()
    if from_env:
        return from_env
    try:
        return git(root, "merge-base", "HEAD", "origin/master").strip()
    except GitError:
        return None


def collect(root: Path, base: str | None, head: str | None):
    """Return (changed, tracked, blob_of, waivers_text) for base..head (head None = working tree)."""
    changed: dict[str, str] = {}
    if base:
        diff = ["diff", "--name-status", "--no-renames", "-z", base] + ([head] if head else [])
        fields = git(root, *diff, "--", *GOLDEN_ROOTS).split("\0")
        for status, path in zip(fields[0::2], fields[1::2]):
            changed[path] = status[0]

    if head:
        blobs = {}
        for entry in filter(None, git(root, "ls-tree", "-r", "-z", head, "--", *GOLDEN_ROOTS).split("\0")):
            meta, path = entry.split("\t", 1)
            blobs[path] = meta.split()[2]
        try:
            waivers_text = git(root, "show", f"{head}:{WAIVERS}")
        except GitError:
            waivers_text = ""
        return changed, set(blobs), blobs.get, waivers_text

    tracked = {p for p in filter(None, git(root, "ls-files", "-z", "--", *GOLDEN_ROOTS).split("\0")) if (root / p).is_file()}

    @functools.cache
    def blob_of(path: str) -> str | None:
        if not (root / path).is_file():
            return None
        return git(root, "hash-object", "--", path).strip()

    waivers_path = root / WAIVERS
    waivers_text = waivers_path.read_text(encoding="utf-8") if waivers_path.is_file() else ""
    return changed, tracked, blob_of, waivers_text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", help=f"commit to diff from (default: ${BASE_ENV}, else merge-base with origin/master)")
    parser.add_argument("--head", help="commit to diff to (default: the working tree)")
    parser.add_argument("--hook", action="store_true", help=f"report without failing unless ${BASE_ENV} is set (CI)")
    args = parser.parse_args(argv)
    enforce = not args.hook or bool(os.environ.get(BASE_ENV, "").strip())

    try:
        root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel").strip())
        base = resolve_base(root, args.base)
        if base is None:
            print("check_vendor_golden_twins: no base commit (no origin/master here); checking waivers only")
        changed, tracked, blob_of, waivers_text = collect(root, base, args.head)
    except GitError as error:
        print(f"check_vendor_golden_twins: {error}\nIs the base commit fetched? (--base / ${BASE_ENV})")
        return 2 if enforce else 0

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
        if not enforce:
            print(f"(Reported, not failed: the PR's pre-commit job enforces this with ${BASE_ENV}.)")
            return 0
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
