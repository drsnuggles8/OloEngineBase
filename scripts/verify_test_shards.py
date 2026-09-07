#!/usr/bin/env python3
"""Prove that a sharded ctest run SELECTED every case the unsharded run would have.

Issue #1083 split the two Windows test steps across a matrix of runners. That trades
runner-minutes for wall-clock, and it introduces one failure mode the unsharded step
did not have: a shard whose ``-I`` range matched nothing, or a shard job that never
started, is INVISIBLE. Every remaining shard passes, the matrix goes green, and the
cases in the gap were never executed. This repository's dominant failure archetype is
a green run that tested nothing, so the sharding is not allowed to exist without a
check that says otherwise.

Each shard writes one small file (see ``--dir``) recording the shard index, the shard
count it believed it was part of, and how many ctest entries its own ``-I`` selection
matched. This script reads them all and fails unless:

  * every shard reported in -- 1..N present exactly once, so a job that was skipped,
    cancelled or never scheduled is caught rather than averaged away;
  * every shard agreed on N -- a matrix list edited out of step with the stride passed
    to ``ctest -I`` would otherwise silently double-run some cases and skip others;
  * no shard matched zero cases -- the guard the issue names explicitly;
  * the counts sum to the total the build job measured with a plain ``ctest -N``.

The last one is the partition proof. The first three are there so that when it fails,
the message says which shard is wrong instead of only that the arithmetic is.

WHAT THIS DOES NOT PROVE, stated here because the distinction is the whole point of
the check: each count comes from ``ctest -N`` BEFORE that shard runs anything, so a
shard whose ctest died a third of the way through still uploaded a full count. This
proves the SELECTION was a partition, not that every selected case executed. The
calling job pairs it with a check on the shard matrix's own result, which covers the
other half.
"""

from __future__ import annotations

import argparse
import pathlib
import sys


def parse_shard_file(path: pathlib.Path) -> dict[str, int]:
    """Read a ``key=value`` shard report. Every value here is an integer by
    construction (the shard writes them from ctest's own output), so a non-integer
    is a corrupted or truncated upload and is worth failing on rather than skipping."""
    fields: dict[str, int] = {}
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if not sep:
            raise SystemExit(f"{path}:{lineno}: expected key=value, got {raw!r}")
        try:
            fields[key.strip()] = int(value.strip())
        except ValueError as exc:
            raise SystemExit(f"{path}:{lineno}: {key.strip()} is not an integer: {value.strip()!r}") from exc
    missing = {"shard", "shards", "count"} - fields.keys()
    if missing:
        raise SystemExit(f"{path}: missing {', '.join(sorted(missing))}")
    return fields


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dir",
        required=True,
        type=pathlib.Path,
        help="Directory the shard-count artifacts were downloaded into. Searched recursively, "
        "because actions/download-artifact with a pattern nests each artifact in its own folder.",
    )
    parser.add_argument(
        "--expected-total",
        required=True,
        type=int,
        help="Case count the build job measured with an unsharded `ctest -N`, under the same "
        "--exclude-regex the shards use. This is the number the shards have to add up to.",
    )
    parser.add_argument("--label", default="test shards", help="Name used in the messages.")
    args = parser.parse_args()

    files = sorted(args.dir.rglob("shard-*.txt"))
    if not files:
        print(f"::error::{args.label}: no shard reports found under {args.dir}. Every shard job "
              f"either failed before its guard ran or its upload was lost; either way nothing "
              f"here proves the suite was executed.")
        return 1

    reports = [parse_shard_file(p) for p in files]

    declared = {r["shards"] for r in reports}
    if len(declared) != 1:
        print(f"::error::{args.label}: shards disagree on the shard count: {sorted(declared)}. The "
              f"matrix list and the stride passed to `ctest -I` have drifted apart, so the shards "
              f"do not partition the suite.")
        return 1
    shard_count = declared.pop()

    seen = sorted(r["shard"] for r in reports)
    if seen != list(range(1, shard_count + 1)):
        print(f"::error::{args.label}: expected reports from shards 1..{shard_count}, got {seen}. "
              f"A missing shard means its cases ran nowhere.")
        return 1

    empty = [r["shard"] for r in reports if r["count"] <= 0]
    if empty:
        print(f"::error::{args.label}: shard(s) {empty} matched ZERO ctest entries. A shard that "
              f"selects nothing passes instantly and looks identical to one that passed.")
        return 1

    total = sum(r["count"] for r in reports)
    print(f"{args.label}: {shard_count} shards, "
          + ", ".join(f"#{r['shard']}={r['count']}" for r in sorted(reports, key=lambda r: r["shard"]))
          + f" -> {total} (build job measured {args.expected_total})")

    if total != args.expected_total:
        print(f"::error::{args.label}: shards covered {total} ctest entries but the unsharded run "
              f"has {args.expected_total}. The shards are not a partition of the suite -- "
              f"{abs(args.expected_total - total)} case(s) were "
              f"{'skipped' if total < args.expected_total else 'double-counted'}.")
        return 1

    print(f"{args.label}: the shards' selections partition all {total} ctest entries.")
    # Deliberately NOT "coverage proven". Each count is recorded from "ctest -N"
    # BEFORE that shard runs anything, so this proves the partition and not the
    # execution: a shard whose ctest died a third of the way through still uploaded a
    # full count. The workflow pairs this with a check on the shard matrix's own
    # result, which is what covers the other half.
    return 0


if __name__ == "__main__":
    sys.exit(main())
